/* Everything the window shares: the index, the scan, and the stores the panes
 * write through.
 *
 * ONE INSTANCE, owned by MainWindow and reached as AppModel.Current. A static
 * accessor rather than a dependency passed down: a WinUI Frame constructs its
 * pages itself and gives no route for handing one a constructor argument, and
 * the alternative -- stuffing the model into Frame.Navigate's parameter and
 * unpacking it in every OnNavigatedTo -- is the same global reached through
 * more ceremony.
 *
 * THE DOWNLOADS FORM STATE LIVES HERE, not in DownloadsPage. A NavigationView
 * Frame destroys a page when you navigate away from it, so a half-typed URL and
 * every un-saved option would be gone after a trip to the Library and back,
 * silently. The GTK app keeps one YtdlDownloadsView alive inside its
 * AdwViewStack for exactly this reason, and the SwiftUI app puts the same state
 * on its AppModel; an object owned here is the same guarantee.
 */

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.UI.Dispatching;
using YtdlWin.Core;

namespace YtdlWin;

/// The form state the Downloads pane binds to, kept alive across navigation.
public sealed class DownloadsFormState
{
    public RunOptions Opts { get; set; } = new();
    /// One --ytdlp-arg per line, because a real --match-filter expression
    /// contains commas and spaces and there is no separator that would be safe
    /// to split a single-line field on.
    public string ExtraArgsText { get; set; } = "";
    public string? SelectedProfile { get; set; }
    public string Status { get; set; } = "";
    public bool StatusIsError { get; set; }

    public DownloadsFormState(Settings settings, ProfileStore store)
    {
        Opts.DataRoot = settings.DataRoot;
        Opts.Workers = settings.DefaultWorkers;
        Opts.Mode = "full";
        Opts.Quality = "best";
        Opts.Codec = "any";
        Opts.AudioCodec = "any";
        Opts.Container = "mkv";

        SelectedProfile = store.Active;
        if (store.Active is { } name && store.Get(name) is { } p) Apply(p.Opts);
    }

    /* Applies a profile's options and LEAVES THE URL ALONE. Fields with no
     * control of their own -- --items, --after, --pot-port, --skip-pot-update --
     * are applied too rather than dropped: they are part of the option set
     * somebody saved, the command preview shows every flag that will run, and
     * silently discarding stored settings is the worse of the two failures. */
    public void Apply(RunOptions p)
    {
        Opts.Mode = p.Mode.Length == 0 ? "full" : p.Mode;
        Opts.Quality = p.Quality.Length == 0 ? "best" : p.Quality;
        Opts.Codec = p.Codec.Length == 0 ? "any" : p.Codec;
        Opts.AudioCodec = p.AudioCodec.Length == 0 ? "any" : p.AudioCodec;
        Opts.Container = p.Container.Length == 0 ? "mkv" : p.Container;
        Opts.Workers = p.Workers > 0 ? p.Workers : 1;
        Opts.Sync = p.Sync;
        Opts.Lazy = p.Lazy;
        Opts.NoPot = p.NoPot;
        Opts.SkipPotUpdate = p.SkipPotUpdate;
        Opts.PotPort = p.PotPort;
        Opts.Items = p.Items;
        Opts.After = p.After;
        Opts.NoComments = p.NoComments;
        Opts.NoSubs = p.NoSubs;
        Opts.NoThumbnail = p.NoThumbnail;
        Opts.NoMetadata = p.NoMetadata;

        /* The destination is part of the profile, but an empty one must not
         * wipe a destination the user has set for this session. */
        if (p.DataRoot.Length > 0) Opts.DataRoot = p.DataRoot;

        ExtraArgsText = string.Join(Environment.NewLine, p.YtdlpArgs);
        Opts.YtdlpArgs = new List<string>(p.YtdlpArgs);
    }
}

public sealed class AppModel
{
    public static AppModel Current { get; private set; } = null!;

    public Settings Settings { get; }
    public Runner Runner { get; } = new();
    public ProfileStore Profiles { get; }
    public DownloadsFormState Form { get; }

    public ArchiveIndex Index { get; private set; } = new();
    public bool Scanning { get; private set; }
    public string Status { get; private set; } = "Starting…";
    public string SearchText { get; set; } = "";
    public string? ArchiveRoot { get; private set; }

    /// Raised on the UI thread whenever Index, Scanning or Status changed.
    public event Action? Changed;
    /// Raised when a scan fails outright, which is the one thing that must
    /// interrupt rather than sit in the status line.
    public event Action<string>? Alert;

    private readonly DispatcherQueue _dispatcher;
    private CancellationTokenSource? _scanCancel;

    private AppModel(DispatcherQueue dispatcher)
    {
        _dispatcher = dispatcher;
        Settings = Settings.Load();
        Profiles = ProfileStore.Load();
        Form = new DownloadsFormState(Settings, Profiles);
        ArchiveRoot = ResolveRoot();
    }

    public static AppModel Initialise(DispatcherQueue dispatcher)
    {
        Current = new AppModel(dispatcher);
        return Current;
    }

    private string? ResolveRoot()
    {
        var configured = Settings.ArchiveRoot.Trim();
        if (configured.Length > 0 && Paths.ResolveArchiveRoot(configured) is { } resolved)
            return resolved;
        return Paths.AutodetectArchiveRoot();
    }

    /// Point the app at a different tree, and remember it.
    public void SetArchiveRoot(string path)
    {
        Settings.ArchiveRoot = path;
        Settings.Save();
        ArchiveRoot = ResolveRoot();
        StartScan();
    }

    // MARK: - Scanning

    public void StartScan()
    {
        if (Scanning) return;

        if (ArchiveRoot is null)
        {
            var message =
                "Could not find an archive. Looked for “Youtube Videos\\Complete Archive” under " +
                "C:\\yt-dlp and the usual locations. Choose the folder you would pass to " +
                "`ytdl --path` on the Health pane.";
            Status = message;
            Index = new ArchiveIndex();
            Changed?.Invoke();
            Alert?.Invoke(message);
            return;
        }

        Scanning = true;
        Status = "Scanning…";
        ThumbnailCache.Clear();
        Changed?.Invoke();

        var root = ArchiveRoot;
        _scanCancel?.Cancel();
        var cts = new CancellationTokenSource();
        _scanCancel = cts;

        /* A counter read by a timer rather than a callback that marshals to the
         * UI thread per folder: a scan of a large archive calls the progress
         * closure thousands of times a second, and one dispatcher hop per call
         * is one re-render per call. The same shape as the GTK app's g_idle
         * drain and the SwiftUI app's ticker, because the problem is the same. */
        var done = 0;
        var total = 0;

        var ticker = _dispatcher.CreateTimer();
        ticker.Interval = TimeSpan.FromMilliseconds(100);
        ticker.Tick += (_, _) =>
        {
            if (!Scanning) { ticker.Stop(); return; }
            var d = Volatile.Read(ref done);
            var t = Volatile.Read(ref total);
            Status = t > 0 ? $"Scanning… {d} of {t}" : "Scanning…";
            Changed?.Invoke();
        };
        ticker.Start();

        Task.Run(() =>
        {
            try
            {
                var built = ArchiveIndex.Scan(root, (d, t, _) =>
                {
                    Volatile.Write(ref done, d);
                    Volatile.Write(ref total, t);
                }, cts.Token);
                _dispatcher.TryEnqueue(() => FinishScan(built, null, ticker));
            }
            catch (OperationCanceledException)
            {
                _dispatcher.TryEnqueue(() => { Scanning = false; ticker.Stop(); });
            }
            catch (Exception ex)
            {
                _dispatcher.TryEnqueue(() => FinishScan(null, ex, ticker));
            }
        });
    }

    private void FinishScan(ArchiveIndex? built, Exception? error, DispatcherQueueTimer ticker)
    {
        Scanning = false;
        ticker.Stop();

        if (built is not null)
        {
            Index = built;
            UpdateCounts();
            Changed?.Invoke();
            return;
        }

        /* Named in full rather than reduced to "scan failed". An empty library
         * with no explanation is the exact outcome the layout contract exists
         * to prevent, and the same rule applies to not finding one at all. */
        Index = new ArchiveIndex();
        Status = error?.Message ?? "The scan failed.";
        Changed?.Invoke();
        Alert?.Invoke(Status);
    }

    /// The status line: what is in the archive, and how much of it is showing.
    public void UpdateCounts()
    {
        if (Scanning) return;
        var size = Format.Bytes(Index.TotalBytes);
        var shown = FilteredEntries().Count;
        Status = shown != Index.VideoCount
            ? $"{shown} of {Index.VideoCount} videos · {Index.ChannelCount} channels · {size}"
            : $"{Index.VideoCount} videos · {Index.ChannelCount} channels · {size}";
        Changed?.Invoke();
    }

    // MARK: - Filtering

    /// Case-insensitive substring over title, uploader, video id and channel.
    public List<ArchiveEntry> FilteredEntries()
    {
        var needle = SearchText.Trim();
        if (needle.Length == 0) return Index.Entries;

        return Index.Entries.Where(e =>
            Contains(e.Title, needle) ||
            Contains(e.Uploader, needle) ||
            Contains(e.VideoId ?? "", needle) ||
            Contains(e.Channel, needle)).ToList();
    }

    /* OrdinalIgnoreCase rather than lowercasing both sides and comparing, which
     * is what the other two ports do. Same answer for the Latin text that is
     * most of it, no allocation per field per keystroke, and it does not depend
     * on the machine's locale for the answer -- searching "TITLE" on a Turkish
     * machine should find "title". */
    private static bool Contains(string haystack, string needle)
        => haystack.Contains(needle, StringComparison.OrdinalIgnoreCase);

    public ArchiveEntry? Entry(string key) => Index.Entry(key);
}
