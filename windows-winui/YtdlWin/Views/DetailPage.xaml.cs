/* One video's page.
 *
 * Everything shown here was already on disk and unread: the description and
 * metadata from info.json, the comment tree yt-dlp captured, the subtitle track
 * as prose, the file inventory, and what is actually inside the media
 * container. The Library tells you what you have; this tells you what it is.
 *
 * THE PLAYER PLAYS THE ORIGINAL FILE, or says why it could not. The webview
 * build remuxed .mkv to WebM into a cache directory because a browser engine
 * cannot play Matroska. Nothing here writes anything, anywhere -- and in
 * particular it does not rebuild that remux apparatus, which is exactly what
 * this project deleted.
 */

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using YtdlWin.Core;

namespace YtdlWin.Views;

/// Everything the worker read, handed to the page in one piece.
public sealed class LoadedDetail
{
    public MediaProbe Probe { get; set; } = new();
    public List<Comment> Comments { get; set; } = new();
    public List<Cue> Cues { get; set; } = new();
    public string TranscriptText { get; set; } = "";
    public string Description { get; set; } = "";
    public long ViewCount { get; set; } = -1;
    public long LikeCount { get; set; } = -1;
    public string Tags { get; set; } = "";
    public string? SubtitlePath { get; set; }
    public string SubtitleName { get; set; } = "";
    public bool SubtitleIsAuto { get; set; }
}

public sealed partial class DetailPage : Page
{
    /* Comment sections run to thousands of entries. Capped, with the total said
     * out loud -- a page that quietly showed 200 of 4,000 would be worse than
     * one that takes a moment. */
    private const int MaxCommentsShown = 200;

    private string _key = "";
    private LoadedDetail? _loaded;
    private bool _loading = true;
    private string _tab = "details";

    /* The WinUI spelling of the GTK app's generation counter. A load that
     * finishes after the user has navigated on belongs to a page they have
     * already left, and rendering it would replace what they are looking at
     * now. */
    private CancellationTokenSource? _loadCancel;

    private AppModel Model => AppModel.Current;
    private ArchiveEntry? Entry => Model.Entry(_key);

    public DetailPage()
    {
        InitializeComponent();
        Tabs.SelectedItem = Tabs.MenuItems[0];
    }

    protected override async void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        _key = e.Parameter as string ?? "";
        await LoadAsync();
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        base.OnNavigatedFrom(e);
        _loadCancel?.Cancel();

        /* LEAVING THE PAGE STOPS PLAYBACK, and this is hung on the navigation
         * event rather than on a back button because there are several ways out
         * of a page and only one of them is the button -- Alt+Left, the mouse's
         * back button, a sidebar click, and a rescan that navigates away all
         * reach here and none of them touch the button.
         *
         * The MediaPlayer must be DISPOSED, not merely paused. MediaPlayerElement
         * does not own the player's lifetime: an undisposed MediaPlayer keeps a
         * decoder and its file handle alive, which means audio continuing after
         * the page is gone AND a locked file the pipeline cannot overwrite.
         * SetMediaPlayer(null) first, so the element is not left holding a
         * disposed object. */
        var player = Player.MediaPlayer;
        Player.SetMediaPlayer(null);
        if (player is not null)
        {
            try
            {
                player.Pause();
                player.Source = null;
                player.MediaFailed -= OnMediaFailed;
                player.Dispose();
            }
            catch (Exception) { /* already torn down */ }
        }
    }

    // MARK: - Loading

    private async Task LoadAsync()
    {
        var entry = Entry;
        if (entry is null) return;

        _loading = true;
        _loaded = null;
        VerifyText.Text = "";
        LoadRing.IsActive = true;
        LoadRing.Visibility = Visibility.Visible;

        TitleText.Text = entry.Title;
        var date = Format.UploadDate(entry.UploadDate);
        SubtitleText.Text = date.Length == 0 ? entry.Uploader : $"{entry.Uploader} · {date}";
        SummaryText.Text = "Reading…";

        HeaderBadges.Children.Clear();
        if (entry.LayoutTooNew)
            HeaderBadges.Children.Add(Controls.Pill("written with a newer archive layout", PillVariant.Warn));
        if (entry.MediaIndex < 0)
            HeaderBadges.Children.Add(Controls.Pill(entry.DownloadMode ?? "no media file"));

        SetUpPlayer(entry);

        var player = ExternalOpen.FindPlayer();
        OpenInPlayerButton.Content = player is null ? "Open externally" : $"Open in {player.Value.Display}";
        OpenInPlayerButton.IsEnabled = entry.MediaPath is not null;

        _loadCancel?.Cancel();
        var cts = new CancellationTokenSource();
        _loadCancel = cts;

        var dir = entry.Dir;
        var files = entry.Files;
        var mediaPath = entry.MediaPath;

        /* Off the UI thread: an info.json with a large comment tree takes long
         * enough to stall a click, and ffprobe is a subprocess. */
        var result = await Task.Run(() =>
        {
            var d = new LoadedDetail { Probe = Media.Probe(mediaPath) };

            /* Chosen HERE and not before the hop: picking a subtitle track means
             * reading the head of every subtitle file in the folder to tell an
             * auto-generated track from a written one, and that is file I/O on
             * the UI thread if it happens in the caller. */
            var subtitle = PickSubtitle(dir, files);

            if (VideoInfo.Load(dir) is { } info)
            {
                d.Description = info.String("description") ?? "";
                d.ViewCount = info.Int("view_count");
                d.LikeCount = info.Int("like_count");
                d.Comments = info.Comments;
                d.Tags = info.Tags;
            }

            if (subtitle is not null)
            {
                d.SubtitlePath = subtitle.Value.Path;
                d.SubtitleName = subtitle.Value.Name;
                d.SubtitleIsAuto = subtitle.Value.IsAuto;
                d.Cues = Transcript.Cues(subtitle.Value.Path);
                d.TranscriptText = string.Join("\n",
                    d.Cues.Select(c => $"[{Format.Timecode(c.Start)}]  {c.Text}"));
            }
            return d;
        }, cts.Token);

        if (cts.IsCancellationRequested) return;

        _loaded = result;
        _loading = false;
        LoadRing.IsActive = false;
        LoadRing.Visibility = Visibility.Collapsed;
        SummaryText.Text = Media.Summary(result.Probe);
        RenderTab();
    }

    private void SetUpPlayer(ArchiveEntry entry)
    {
        PlaybackNote.Visibility = Visibility.Collapsed;
        Player.Visibility = Visibility.Collapsed;

        var media = entry.MediaPath;
        if (media is null) return;

        try
        {
            var player = new Windows.Media.Playback.MediaPlayer
            {
                AutoPlay = false,
                Source = Windows.Media.Core.MediaSource.CreateFromUri(new Uri(media)),
            };
            player.MediaFailed += OnMediaFailed;
            Player.SetMediaPlayer(player);
            Player.Visibility = Visibility.Visible;
        }
        catch (Exception)
        {
            ShowPlaybackNote(null);
        }
    }

    /* MediaFailed arrives on a background thread, so everything it touches has
     * to be marshalled. This is also where the honest note comes from rather
     * than from a guess: by the time the media stack has refused the file,
     * ffprobe has usually already told us which codec is in it. */
    private void OnMediaFailed(Windows.Media.Playback.MediaPlayer sender,
                               Windows.Media.Playback.MediaPlayerFailedEventArgs args)
    {
        var message = args.ErrorMessage;
        DispatcherQueue.TryEnqueue(() => ShowPlaybackNote(message));
    }

    private void ShowPlaybackNote(string? platformMessage)
    {
        Player.Visibility = Visibility.Collapsed;
        PlaybackNote.Visibility = Visibility.Visible;
        PlaybackNoteText.Text = Media.PlaybackFailedNote(
            _loaded?.Probe ?? new MediaProbe(), Entry?.MediaPath, platformMessage);
    }

    /* A human-written track wins over an auto-generated one, since the whole
     * reason the distinction is read from file CONTENTS is that it matters which
     * you are reading. */
    private static (string Path, string Name, bool IsAuto)? PickSubtitle(
        string dir, List<ArchiveFile> files)
    {
        /* A stand-in entry with just the two fields PathForIndex reads. The path
         * is still resolved and containment-checked by the index's own code from
         * the index's own directory -- this is not a caller supplying a path. */
        var entry = new ArchiveEntry { Key = "", Dir = dir, Rel = "", Channel = "", Files = files };

        (string Path, string Name, bool IsAuto)? best = null;
        for (var i = 0; i < files.Count; i++)
        {
            if (!MediaExtensions.IsSubtitle(files[i].Ext)) continue;
            var path = entry.PathForIndex(i);
            if (path is null) continue;

            var isAuto = Transcript.IsAutoGenerated(path);
            if (best is null || (best.Value.IsAuto && !isAuto))
                best = (path, files[i].Rel, isAuto);
        }
        return best;
    }

    // MARK: - Actions

    private void OnOpenInPlayer(object sender, RoutedEventArgs e)
    {
        if (Entry?.MediaPath is { } path) ExternalOpen.InPlayer(path);
    }

    private void OnReveal(object sender, RoutedEventArgs e)
    {
        if (Entry is { } entry) ExternalOpen.RevealInExplorer(entry.MediaPath ?? entry.Dir);
    }

    private async void OnVerify(object sender, RoutedEventArgs e)
    {
        var dir = Entry?.Dir;
        if (dir is null) return;

        VerifyText.Text = "Verifying…";
        VerifyText.Foreground = Controls.Resource<Brush>("TextFillColorSecondaryBrush");

        /* Hashing every file in a folder is seconds of work on a large video, so
         * it goes off the UI thread like everything else here that touches the
         * disk in bulk. */
        var result = await Task.Run(() => Health.VerifyChecksums(dir));

        if (!result.Present)
        {
            VerifyText.Text = "No checksums.sha256 in this folder.";
            VerifyText.Foreground = Controls.Resource<Brush>("TextFillColorSecondaryBrush");
        }
        else if (result.Failed.Count == 0 && result.Missing.Count == 0)
        {
            VerifyText.Text = $"All {result.Checked} files verify.";
            VerifyText.Foreground = Controls.Resource<Brush>("SystemFillColorSuccessBrush");
        }
        else
        {
            /* video_postprocessing.log is EXCLUDED from checksums.sha256 by
             * postprocess.ps1 because it is still being appended to when the
             * hashes are computed -- so it is never one of these, and anything
             * listed here is a real mismatch. */
            VerifyText.Text = $"{result.Ok} of {result.Checked} verify · " +
                              $"{result.Failed.Count} failed · {result.Missing.Count} missing";
            VerifyText.Foreground = Controls.Resource<Brush>("SystemFillColorCriticalBrush");
        }
    }

    // MARK: - Tabs

    private void OnTabChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is NavigationViewItem item && item.Tag is string tag)
        {
            _tab = tag;
            RenderTab();
        }
    }

    private void RenderTab()
    {
        TabContent.Children.Clear();
        var entry = Entry;
        if (entry is null) return;

        switch (_tab)
        {
            case "media": RenderMedia(); break;
            case "comments": RenderComments(entry); break;
            case "transcript": RenderTranscript(); break;
            case "files": RenderFiles(entry); break;
            default: RenderDetails(entry); break;
        }
    }

    private void Add(UIElement e) => TabContent.Children.Add(e);

    private void AddText(string text, bool secondary = false)
    {
        Add(new TextBlock
        {
            Text = text,
            TextWrapping = TextWrapping.Wrap,
            IsTextSelectionEnabled = true,
            Foreground = secondary ? Controls.Resource<Brush>("TextFillColorSecondaryBrush") : null,
            Margin = new Thickness(0, 0, 0, 6),
        });
    }

    private void RenderDetails(ArchiveEntry entry)
    {
        Add(Controls.KeyValueRow("Channel", entry.Uploader));
        Add(Controls.KeyValueRow("Uploaded", Format.UploadDate(entry.UploadDate)));
        if (_loaded is { ViewCount: > 0 } d1) Add(Controls.KeyValueRow("Views", Format.Count(d1.ViewCount)));
        if (_loaded is { LikeCount: > 0 } d2) Add(Controls.KeyValueRow("Likes", Format.Count(d2.LikeCount)));
        if (entry.OriginalUrl is { } url) Add(Controls.KeyValueRow("Source", url));

        Add(Controls.KeyValueRow("Archive layout",
            entry.LayoutVersion == 0 ? "1 (predates versioning)" : entry.LayoutVersion.ToString()));
        if (entry.DownloadMode is { } mode) Add(Controls.KeyValueRow("Download mode", mode));
        if (_loaded is { } d3 && d3.Tags.Length > 0) Add(Controls.KeyValueRow("Tags", d3.Tags));
        Add(Controls.KeyValueRow("Folder", entry.Dir, monospaced: true));

        if (_loaded is { } d4 && d4.Description.Length > 0)
        {
            Add(new TextBlock
            {
                Text = "Description",
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                Margin = new Thickness(0, 12, 0, 4),
            });
            AddText(d4.Description);
        }
    }

    private void RenderMedia()
    {
        if (_loading) { AddText("Reading the container…", secondary: true); return; }
        var probe = _loaded?.Probe;
        if (probe is null) return;

        if (!probe.Ok)
        {
            AddText(probe.Error ?? "No media file.", secondary: true);
            return;
        }

        Add(Controls.KeyValueRow("Container", probe.ContainerFormat));
        if (probe.BitRate > 0)
        {
            Add(Controls.KeyValueRow("Bitrate", string.Format(
                System.Globalization.CultureInfo.InvariantCulture,
                "{0:0.00} Mb/s overall", probe.BitRate / 1_000_000.0)));
        }
        foreach (var s in probe.Streams)
            Add(Controls.KeyValueRow($"{s.Kind} #{s.Index}", Describe(s)));

        if (probe.Chapters.Count > 0)
        {
            Add(new TextBlock
            {
                Text = $"{probe.Chapters.Count} chapters",
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                Margin = new Thickness(0, 12, 0, 4),
            });
            foreach (var c in probe.Chapters)
                Add(Controls.KeyValueRow(Format.Timecode(c.Start), c.Title));
        }
    }

    private static string Describe(MediaStream s)
    {
        var inv = System.Globalization.CultureInfo.InvariantCulture;
        var head = s.Codec ?? "unknown codec";
        if (s.Profile is { } profile) head += $" ({profile})";

        var parts = new List<string> { head };
        if (s.Width > 0 && s.Height > 0) parts.Add($"{s.Width}×{s.Height}");
        if (s.Fps > 0.01) parts.Add(string.Format(inv, "{0:0.###} fps", s.Fps));
        if (s.Channels > 0) parts.Add($"{s.Channels} ch");
        if (s.SampleRate > 0) parts.Add($"{s.SampleRate} Hz");
        if (s.BitRate > 0) parts.Add($"{s.BitRate / 1000} kb/s");
        if (s.Language is { } lang) parts.Add(lang);
        if (s.Title is { } title) parts.Add($"“{title}”");
        if (s.IsDefault) parts.Add("default");
        /* Called out because this is exactly what made the webview build serve
         * every thumbnail with a video MIME type: ffprobe reports an attached
         * cover as a one-frame video stream. */
        if (s.AttachedPic) parts.Add("attached cover image, not a video track");
        return string.Join(" · ", parts);
    }

    private void RenderComments(ArchiveEntry entry)
    {
        var d = _loaded;
        if (d is null) return;

        if (d.Comments.Count == 0)
        {
            var mode = entry.DownloadMode;
            AddText(mode is "metadata-only" or "subs-only"
                ? "This video was fetched in a mode that does not capture comments."
                : "No comments were captured for this video. That is what --no-comments " +
                  "produces, and also what a video with comments disabled produces.",
                secondary: true);
            return;
        }

        var shown = d.Comments.Take(MaxCommentsShown).ToList();
        var header = $"{Comment.TotalCount(d.Comments)} comments including replies · " +
                     $"{d.Comments.Count} threads";
        if (shown.Count < d.Comments.Count) header += $" (showing the first {MaxCommentsShown})";
        AddText(header, secondary: true);

        foreach (var c in shown)
        {
            Add(CommentRow(c, isReply: false));
            foreach (var r in c.Replies) Add(CommentRow(r, isReply: true));
        }
    }

    private static UIElement CommentRow(Comment c, bool isReply)
    {
        var badges = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        badges.Children.Add(new TextBlock
        {
            Text = c.Author,
            FontSize = 12,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            VerticalAlignment = VerticalAlignment.Center,
        });
        if (c.AuthorIsUploader) badges.Children.Add(Controls.Pill("uploader", PillVariant.Accent));
        if (c.IsPinned) badges.Children.Add(Controls.Pill("pinned", PillVariant.Accent));
        if (c.IsFavorited) badges.Children.Add(Controls.Pill("hearted", PillVariant.Accent));
        if (c.LikeCount > 0) badges.Children.Add(Controls.Pill($"{Format.Count(c.LikeCount)} likes"));
        if (c.TimeText is { } t) badges.Children.Add(Controls.Pill(t));

        var body = new StackPanel { Spacing = 2 };
        body.Children.Add(badges);
        body.Children.Add(new TextBlock
        {
            Text = c.Text,
            TextWrapping = TextWrapping.Wrap,
            IsTextSelectionEnabled = true,
        });

        /* A rule down the left rather than an indent alone, so a long thread
         * still reads as one conversation once the replies wrap. */
        return new Border
        {
            Margin = new Thickness(isReply ? 22 : 0, 0, 0, isReply ? 6 : 12),
            Padding = new Thickness(isReply ? 10 : 0, 0, 0, 0),
            BorderThickness = new Thickness(isReply ? 2 : 0, 0, 0, 0),
            BorderBrush = Controls.Resource<Brush>("DividerStrokeColorDefaultBrush"),
            Child = body,
        };
    }

    private void RenderTranscript()
    {
        var d = _loaded;
        if (d is null) return;

        if (d.SubtitlePath is null)
        {
            AddText("No subtitle track in this folder. --no-subs produces that, and so does a " +
                    "video with no captions available.", secondary: true);
            return;
        }
        if (d.Cues.Count == 0)
        {
            AddText("The subtitle file is present but produced no readable cues.", secondary: true);
            return;
        }

        AddText($"{d.SubtitleName} · {d.Cues.Count} lines" +
                (d.SubtitleIsAuto ? " · auto-generated, rolling duplication collapsed"
                                  : " · uploaded track"),
                secondary: true);
        AddText(d.TranscriptText);
    }

    private void RenderFiles(ArchiveEntry entry)
    {
        foreach (var f in entry.Files)
            Add(Controls.KeyValueRow(f.Rel, Format.Bytes(f.Size)));
    }
}
