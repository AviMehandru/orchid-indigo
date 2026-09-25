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
    /* SponsorBlock is one choice -- off, mark, or cut -- plus one category
     * list, and the two RunOptions fields it becomes are derived in
     * EffectiveOptions(). Kept apart so switching the mode off and on again does
     * not make anybody retype a list. */
    public string SponsorMode { get; set; } = "off";
    public string SponsorCats { get; set; } = "sponsor";
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
        Opts.Fps = p.Fps;
        Opts.SubLangs = p.SubLangs;
        Opts.NoChapters = p.NoChapters;
        /* A profile written before these existed has neither list, which reads
         * as SponsorBlock off -- the behaviour it always had. The category
         * field keeps whatever it held. */
        if (p.SponsorblockRemove.Length > 0) { SponsorMode = "remove"; SponsorCats = p.SponsorblockRemove; }
        else if (p.SponsorblockMark.Length > 0) { SponsorMode = "mark"; SponsorCats = p.SponsorblockMark; }
        else SponsorMode = "off";

        /* The destination is part of the profile, but an empty one must not
         * wipe a destination the user has set for this session. */
        if (p.DataRoot.Length > 0) Opts.DataRoot = p.DataRoot;

        ExtraArgsText = string.Join(Environment.NewLine, p.YtdlpArgs);
        Opts.YtdlpArgs = new List<string>(p.YtdlpArgs);
    }

    /* What the form MEANS, as opposed to what its controls hold: the
     * SponsorBlock choice folded into its two fields, and whatever the form is
     * showing greyed out dropped (RunOptions.DropInapplicable). This is what
     * is queued, previewed and saved as a profile. The connection is not here
     * -- the Runner stamps it on at enqueue. */
    public RunOptions EffectiveOptions()
    {
        var o = Opts.Clone();
        var cats = SponsorCats.Trim();
        /* An empty list with a mode chosen is sent as "sponsor" rather than
         * dropped: the picker says SponsorBlock is on, and a run that quietly
         * did nothing about it would contradict the form. */
        if (cats.Length == 0) cats = "sponsor";
        o.SponsorblockMark = SponsorMode == "mark" ? cats : "";
        o.SponsorblockRemove = SponsorMode == "remove" ? cats : "";
        o.DropInapplicable();
        return o;
    }

    /// Mirrors RunOptions.DropInapplicable rule for rule: the one decides
    /// what is greyed out, the other what is sent.
    public bool MediaOptionsApply =>
        Opts.Mode is not ("metadata-only" or "comments-only" or "subs-only");
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
        /* Before the store is read, because a fresh install has to have its
         * default profile on disk by the time the Profiles menu is built. Does
         * nothing on every launch after the first. */
        ProfileStore.SeedDefaultIfMissing();
        Profiles = ProfileStore.Load();
        Form = new DownloadsFormState(Settings, Profiles);
        /* Before anything can enqueue, so the very first run -- a re-fetch
         * started from a video's page before Downloads is ever opened
         * included -- goes out with the saved cookies and proxy. The Downloads
         * page updates it whenever the Connection settings change. */
        Runner.SetConnection(Settings.Connection());
        ArchiveRoot = ResolveRoot();

        /* The saved ordering, before the first scan, so the first grid ever
         * drawn is already in the order this user chose. The FACETS
         * deliberately do not persist -- see Settings. */
        Filter.Sort = SortKeys.FromId(Settings.LibrarySort);
        Filter.Descending = Settings.LibrarySortDescending;

        /* The watched set, handed over ONCE and live: it is the same HashSet
         * the store mutates, so marking a video watched on its page is visible
         * to the next filter pass without anything re-handing it. Unlike the
         * facets this is not a filter the user set -- it is the store's
         * answer, and the Unwatched facet is wrong without it. */
        Filter.WatchedKeys = UserData.WatchedKeys;
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

    /// <summary>
    /// The Library's sort and facets. The RULES live in
    /// <see cref="LibraryFilter"/> rather than here, so they can be tested
    /// without a window and read against the C and Swift copies.
    /// </summary>
    public LibraryFilter Filter { get; } = new();

    /// <summary>
    /// Verification results, read by the "failed verification" facet and
    /// written by the detail page's Verify button.
    /// </summary>
    public VerifyCache VerifyCache { get; } = new();

    /// <summary>Collection-wide comment and transcript search.</summary>
    /// <remarks>
    /// The index is LOADED at startup and never built there. Reading every
    /// info.json in an archive is the most expensive thing this app can do,
    /// and doing it unasked on every launch would make opening the window cost
    /// what opening every video costs -- the exact rule the archive scan
    /// already follows. The banner offers it when a scope needs it.
    /// </remarks>
    public SearchIndex SearchIndex { get; } = new();

    /// <summary>
    /// Watch state, resume points and playlists: the one store here whose
    /// contents came from the person rather than from the pipeline, which is
    /// why it lives in the state directory rather than in the cache one.
    /// </summary>
    public UserData UserData { get; } = new();

    /// <summary>
    /// The playlist the Library is restricted to, by id, or null for all
    /// videos. Kept beside the filter rather than in it, because the filter
    /// wants a SET of keys and this is the user's choice of which playlist.
    /// </summary>
    public string? PlaylistId
    {
        get => _playlistId;
        set
        {
            _playlistId = value;
            Filter.PlaylistKeys = value is null ? null : UserData.PlaylistKeys(value);
        }
    }

    private string? _playlistId;

    public SearchScope SearchScope { get; set; } = SearchScope.Metadata;

    /// The keys the current collection-wide search admits. null when the scope
    /// needs no index or the box is empty, which is NOT the same as empty:
    /// null means "not narrowing", empty means "narrowing to nothing".
    public HashSet<string>? SearchHits { get; private set; }

    public bool Indexing { get; private set; }
    public string IndexProgress { get; private set; } = "";

    /// <summary>
    /// How many videos a collection-wide search currently cannot see.
    /// </summary>
    /// <remarks>
    /// The banner is the only place the app can be honest about this. A
    /// comment search against an index that covers none of the archive returns
    /// nothing, and "no results" is a lie about the archive rather than a fact
    /// about it.
    /// </remarks>
    public int SearchIndexOutdated => SearchIndex.Outdated(Index.Entries);

    /// <summary>
    /// True when a collection-wide scope is selected and the index cannot
    /// answer for part of the archive.
    /// </summary>
    public bool SearchNeedsIndex =>
        SearchScopes.NeedsIndex(SearchScope) && SearchIndexOutdated > 0;

    /// <summary>
    /// Recompute the collection-wide hit set for the current scope and box.
    /// </summary>
    public void UpdateSearch()
    {
        var text = SearchText.Trim();
        if (SearchScope == SearchScope.Metadata || text.Length == 0)
        {
            SearchHits = null;
            UpdateCounts();
            return;
        }

        var hits = SearchIndex.Query(text, SearchScope);
        if (SearchScope == SearchScope.Everything)
        {
            /* A union cannot be expressed as a needle plus a key set, because
             * those AND -- so the metadata matches are folded into the key set
             * here rather than left to the filter. */
            foreach (var e in Index.Entries)
            {
                if (LibraryFilter.MetadataMatches(e, text)) hits.Add(e.Key);
            }
        }
        SearchHits = hits;
        UpdateCounts();
    }

    public async void BuildSearchIndex()
    {
        if (Indexing || Scanning) return;
        Indexing = true;
        IndexProgress = "Reading comments and captions…";
        Changed?.Invoke();

        var entries = Index.Entries;
        var store = SearchIndex;

        await System.Threading.Tasks.Task.Run(() =>
        {
            store.Build(entries, (done, total) =>
            {
                /* Marshalled back rather than written from the worker: every
                 * property on this class is read by a page on the UI thread. */
                _dispatcher.TryEnqueue(() =>
                {
                    IndexProgress = $"Reading comments and captions… {done} of {total}";
                    Changed?.Invoke();
                });
            });
            store.Save();
        });

        Indexing = false;
        /* Re-run the search rather than just redrawing: the whole point of
         * having built the index is that the query the user already typed can
         * now be answered. */
        UpdateSearch();
        Changed?.Invoke();
    }

    /// <summary>
    /// The search box and the facets are ONE filter, so the needle is pushed
    /// into it here rather than being a second, parallel narrowing that the
    /// count and the empty state would each have to remember to apply.
    /// </summary>
    public List<ArchiveEntry> FilteredEntries()
    {
        var text = SearchText.Trim();

        /* The three shapes are deliberately different:
         *
         *   Metadata    the substring match the Library always had.
         *   Comments
         *   Transcript  the index answers alone. The needle is cleared,
         *               because leaving it set would AND the metadata match on
         *               top and a search for a word SAID in a video would
         *               return only the videos with that word in the TITLE as
         *               well.
         *   Everything  the union of both, folded into the key set by
         *               UpdateSearch -- a union cannot be expressed as a
         *               needle plus a key set, because those AND. */
        if (SearchScope == SearchScope.Metadata || text.Length == 0)
        {
            Filter.Needle = text;
            Filter.KeyAllow = null;
        }
        else
        {
            Filter.Needle = "";
            Filter.KeyAllow = SearchHits ?? new HashSet<string>(StringComparer.Ordinal);
        }

        return Filter.Apply(Index.Entries, VerifyCache.State);
    }

    /// <summary>
    /// Whether anything at all is narrowing the library, INCLUDING the search
    /// box. Drives the empty state, which has to tell "there is no archive
    /// here" apart from "your filters exclude everything" -- they look
    /// identical as an empty grid and only one of them is the user's own doing.
    /// </summary>
    public bool IsNarrowing => SearchText.Trim().Length > 0 || Filter.FacetCount > 0;

    /// <summary>
    /// Clear the facets AND the search box. The needle is part of the filter
    /// as far as the user is concerned, so leaving the search box populated
    /// after "Clear filters" would leave a term visibly applied that is not.
    /// </summary>
    public void ClearFilters()
    {
        Filter.Reset();
        /* Reset() drops the playlist RESTRICTION; this drops the user's
         * selection with it. Leaving the id set would leave the flyout
         * claiming a playlist while the grid showed the whole archive. */
        PlaylistId = null;
        SearchText = "";
        SearchHits = null;
        UpdateCounts();
    }

    // ------------------------------------------------------------------ //
    // Watch state and playlists                                          //
    // ------------------------------------------------------------------ //

    public bool IsWatched(string key) => UserData.IsWatched(key);

    public void SetWatched(string key, bool watched)
    {
        UserData.SetWatched(key, watched);
        UserData.Save();
        UpdateCounts();
    }

    /// <summary>
    /// Record where playback got to. The three rules -- finished clears the
    /// resume point, a glance stores nothing, anything else is kept -- are
    /// UserData's, not this class's.
    /// </summary>
    /// <returns>
    /// True when the WATCHED flag moved, so the caller knows whether anything
    /// on screen needs redrawing. A resume point changes every five seconds
    /// while something is playing, and redrawing the grid that often for a
    /// number nothing is showing would be a waste.
    /// </returns>
    public bool RecordPosition(string key, double seconds, double duration)
    {
        var was = UserData.IsWatched(key);
        UserData.SetPosition(key, seconds, duration);
        UserData.Save();
        if (UserData.IsWatched(key) == was) return false;
        UpdateCounts();
        return true;
    }

    public double ResumePosition(string key) => UserData.Position(key);

    public Playlist? CreatePlaylist(string name, string? adding = null)
    {
        var pl = UserData.CreatePlaylist(name);
        if (pl is null) return null;
        if (adding is not null) UserData.AddToPlaylist(pl.Id, adding);
        UserData.Save();
        RefreshPlaylistFilter();
        return pl;
    }

    public void TogglePlaylistMembership(string id, string key)
    {
        if (UserData.PlaylistContains(id, key)) UserData.RemoveFromPlaylist(id, key);
        else UserData.AddToPlaylist(id, key);
        UserData.Save();
        RefreshPlaylistFilter();
    }

    public void DeletePlaylist(string id)
    {
        UserData.DeletePlaylist(id);
        if (string.Equals(PlaylistId, id, StringComparison.Ordinal)) PlaylistId = null;
        UserData.Save();
        RefreshPlaylistFilter();
    }

    // ------------------------------------------------------------------ //
    // Multi-select and bulk actions                                      //
    // ------------------------------------------------------------------ //

    /* A MODE rather than "ctrl-click always multi-selects": the primary
     * gesture on a card is "open this", and a grid where a stray click adds to
     * a hidden selection does the wrong thing quietly. */
    public bool Selecting
    {
        get => _selecting;
        set
        {
            _selecting = value;
            /* Leaving the mode clears the selection. A selection nothing on
             * screen is showing is not a selection. */
            if (!value) SelectedKeys.Clear();
        }
    }

    private bool _selecting;

    /// The keys the user has ticked. Owned here rather than read off the
    /// GridView, so a bulk action does not depend on a control still existing.
    public HashSet<string> SelectedKeys { get; } = new(StringComparer.Ordinal);

    /// Progress for the one bulk action that takes real time.
    public bool VerifyingBulk { get; private set; }
    public string VerifyProgress { get; private set; } = "";

    /// One save for the whole batch. Saving per video would rewrite the store a
    /// few hundred times for one button press.
    public string BulkSetWatched(bool watched)
    {
        if (SelectedKeys.Count == 0 || UserData.IsReadOnly) return "";
        foreach (var key in SelectedKeys) UserData.SetWatched(key, watched);
        UserData.Save();

        var n = SelectedKeys.Count;
        UpdateCounts();
        return $"Marked {n} video{(n == 1 ? "" : "s")} " +
               (watched ? "watched." : "unwatched.");
    }

    /* ADD-ONLY, and deliberately without a tick state. A mixed selection where
     * some videos are in a playlist and some are not has no honest checkbox
     * state, and a control that flipped each one independently would remove
     * half of them. */
    public string BulkAddToPlaylist(string id)
    {
        if (SelectedKeys.Count == 0 || UserData.IsReadOnly) return "";

        var added = 0;
        foreach (var key in SelectedKeys)
        {
            if (UserData.AddToPlaylist(id, key)) added++;
        }
        UserData.Save();
        RefreshPlaylistFilter();

        var name = UserData.GetPlaylist(id)?.Name ?? "the playlist";
        return $"Added {added} video{(added == 1 ? "" : "s")} to {name}.";
    }

    /// The selected entries, resolved against the CURRENT index.
    public List<ArchiveEntry> SelectedEntries() =>
        FilteredEntries().Where(e => SelectedKeys.Contains(e.Key)).ToList();

    /* ONE RUN PER VIDEO, not one run with many URLs. `ytdl --refresh` refreshes
     * the video it is given; a session with several URLs would be a --sync-like
     * shape the refusal list rejects, and one that failed halfway would leave
     * no way to tell which videos were reached. Separate queue entries also
     * mean a single failure is one red row rather than the whole batch. */
    public string BulkRefetch(string mode)
    {
        var queued = 0;
        foreach (var e in SelectedEntries())
        {
            if (string.IsNullOrEmpty(e.OriginalUrl)) continue;
            Runner.Enqueue(new RunOptions
            {
                Url = e.OriginalUrl!,
                Mode = mode,
                Refresh = true,
            });
            queued++;
        }

        if (queued == 0) return "None of the selected videos recorded a source URL.";
        return $"Queued {queued} {mode} refresh{(queued == 1 ? "" : "es")}.";
    }

    /// Verify every selected folder. Seconds per video, so it runs off the UI
    /// thread with a progress readout.
    public async void BulkVerify(Action<string> done)
    {
        if (VerifyingBulk || SelectedKeys.Count == 0) return;

        /* The keys and directories are copied out HERE, while the index is
         * known to be current. A worker holding entries would be holding them
         * against an index a rescan may have replaced. */
        var targets = SelectedEntries()
            .Select(e => (Key: e.Key, Dir: e.Dir))
            .ToList();
        if (targets.Count == 0) return;

        VerifyingBulk = true;
        VerifyProgress = $"Verifying 0 of {targets.Count}…";
        Changed?.Invoke();

        var results = new List<(string Key, VerifyState State)>();

        await Task.Run(() =>
        {
            for (var i = 0; i < targets.Count; i++)
            {
                var r = Health.VerifyChecksums(targets[i].Dir);

                /* UNKNOWN, not Ok. A folder with no checksums.sha256 has not
                 * passed and has not failed -- the layout contract says to
                 * tolerate one -- and recording it as a pass would put a green
                 * answer in the cache for a folder nothing hashed. */
                VerifyState state;
                if (!r.Present) state = VerifyState.Unknown;
                else if (r.Failed.Count > 0 || r.Missing.Count > 0) state = VerifyState.Failed;
                else state = VerifyState.Ok;

                results.Add((targets[i].Key, state));

                var n = i + 1;
                _dispatcher.TryEnqueue(() =>
                {
                    VerifyProgress = $"Verifying {n} of {targets.Count}…";
                    Changed?.Invoke();
                });
            }
        });

        VerifyingBulk = false;
        VerifyProgress = "";
        done(FinishBulkVerify(results));
    }

    /* Back on the UI thread, because the cache stamps every record with the
     * folder's archive_creation_time and that means a lookup in the index.
     *
     * The results go into the same cache the detail page writes: a bulk verify
     * whose findings the "failed verification" facet could not see would be a
     * summary you read once and then had no way to act on. */
    private string FinishBulkVerify(List<(string Key, VerifyState State)> results)
    {
        var checkedCount = 0;
        var bad = 0;
        var unchecked_ = 0;

        foreach (var (key, state) in results)
        {
            if (state == VerifyState.Unknown) { unchecked_++; continue; }
            checkedCount++;
            if (state == VerifyState.Failed) bad++;

            var e = Index.Entry(key);
            if (e is not null) VerifyCache.Set(e, state);
        }
        VerifyCache.Save();
        UpdateCounts();

        /* The summary separates "passed" from "had nothing to check". A folder
         * with no checksums.sha256 is not a failure, but it is not a pass
         * either, and folding it into the pass count would be this app claiming
         * it checked folders it never opened a single hash in. */
        string note;
        if (checkedCount == 0)
        {
            note = $"Nothing to verify: {unchecked_} folder" +
                   (unchecked_ == 1 ? " has" : "s have") + " no checksums.sha256.";
        }
        else if (bad == 0)
        {
            note = $"All {checkedCount} verified.";
        }
        else
        {
            note = $"{bad} of {checkedCount} failed verification.";
        }

        if (checkedCount > 0 && unchecked_ > 0)
        {
            note += $" {unchecked_} had no checksums.sha256.";
        }
        return note;
    }

    /* The playlist the Library is showing may be the very one that just
     * changed, so its key set is rebuilt rather than assumed still right. The
     * WATCHED set needs no such call: the filter holds the store's live
     * HashSet, so a change is already visible to the next pass. */
    private void RefreshPlaylistFilter()
    {
        if (PlaylistId is not null) Filter.PlaylistKeys = UserData.PlaylistKeys(PlaylistId);
        UpdateCounts();
    }

    /// <summary>
    /// Remember an ordering the user chose. Called from the sort control, not
    /// from the facet controls: a sort is a standing preference for how you
    /// like to read a list, while a facet is a question you asked once, and an
    /// app that reopens showing a fifth of the archive with no visible reason
    /// is an app that looks like it lost your videos.
    /// </summary>
    public void PersistSort()
    {
        Settings.LibrarySort = SortKeys.Id(Filter.Sort);
        Settings.LibrarySortDescending = Filter.Descending;
        Settings.Save();
        UpdateCounts();
    }

    /// <summary>
    /// Record a verification result and redraw anything reading the facet.
    /// </summary>
    public void RecordVerification(ArchiveEntry entry, VerifyState state)
    {
        VerifyCache.Set(entry, state);
        Changed?.Invoke();
    }

    public ArchiveEntry? Entry(string key) => Index.Entry(key);

    /// <summary>
    /// Put a one-line outcome in the status line -- this app's toast, since
    /// WinUI has none. Used by the Subscriptions page, whose commands finish
    /// after the button that started them has been forgotten about.
    /// </summary>
    public void Say(string message)
    {
        Status = message;
        Changed?.Invoke();
    }
}
