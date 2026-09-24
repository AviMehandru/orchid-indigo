/* The Downloads pane: build a `ytdl` command line, run it, watch it.
 *
 * The form's state lives on AppModel, not on this page. A NavigationView Frame
 * destroys a page when you navigate away from it, so a half-typed URL and every
 * un-saved option would be gone after a trip to the Library and back, silently.
 * The GTK app keeps one YtdlDownloadsView alive inside its AdwViewStack for
 * exactly this reason; state owned by the model is the same guarantee.
 *
 * The drain timer is the other half of the runner's threading story. The worker
 * thread never touches a control; it buffers lines and state under a lock and
 * this page collects them 20 times a second. yt-dlp redraws its progress line
 * several times a second and a --sync of a large channel emits thousands of
 * lines a minute -- pushing each one into a bound collection as it arrived
 * would put the layout system in a re-render storm for the whole download.
 */

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.Linq;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Navigation;
using System.Threading;
using YtdlWin.Core;

namespace YtdlWin.Views;

public sealed partial class DownloadsPage : Page
{
    /// At 50ms the bar still looks continuous and the UI thread wakes 20 times
    /// a second instead of hundreds.
    private static readonly TimeSpan DrainInterval = TimeSpan.FromMilliseconds(50);
    private const int MaxLogRows = 4000;

    private readonly ObservableCollection<string> _log = new();
    private DispatcherQueueTimer? _drain;
    private bool _suppressChanges;

    /* The URL preview.
     *
     * The form used to know nothing about the URL in it until the run failed:
     * the Quality list was a fixed ladder, asking for 1440p AV1 was a request
     * that silently resolved to something else, and a typo'd URL was
     * discovered by a failed row in the history list. */
    private Probe? _probe;
    private CancellationTokenSource? _probeCts;
    private readonly List<CheckBox> _entryChecks = new();

    /* Set while Items is being rewritten from the tick boxes, so the change
     * handler does not immediately re-derive the boxes from the text it just
     * wrote. Separate from _suppressChanges, which is held across whole-form
     * rebuilds and would swallow the user's own typing. */
    private bool _writingItems;

    /// How many playlist entries get a tick row before the list is summarised
    /// instead. A ListView builds these eagerly, so a 500-entry channel is 500
    /// controls constructed on the UI thread the moment the expander opens --
    /// a visible stall, for a list nobody scrolls to the bottom of. Past this
    /// the range field stays the way to say what you want.
    private const int MaxEntryRows = 200;

    private AppModel Model => AppModel.Current;
    private DownloadsFormState Form => Model.Form;

    private static readonly (string Label, string Value)[] Modes =
    {
        ("Everything", "full"), ("Video only", "video-only"), ("Audio only", "audio-only"),
        ("Metadata only", "metadata-only"), ("Comments only", "comments-only"),
        ("Subtitles only", "subs-only"),
    };
    private static readonly (string Label, string Value)[] Qualities =
    {
        ("Best", "best"), ("2160p", "2160"), ("1440p", "1440"), ("1080p", "1080"),
        ("720p", "720"), ("480p", "480"), ("360p", "360"),
    };
    private static readonly (string Label, string Value)[] Codecs =
    {
        ("Any", "any"), ("AVC1 / H.264", "avc1"), ("VP9", "vp9"), ("AV1", "av01"),
    };
    private static readonly (string Label, string Value)[] AudioCodecs =
    {
        ("Any", "any"), ("Opus", "opus"), ("AAC", "aac"), ("MP3", "mp3"), ("FLAC", "flac"),
    };
    private static readonly (string Label, string Value)[] Containers =
    {
        ("MKV", "mkv"), ("MP4", "mp4"), ("WebM", "webm"),
    };
    private static readonly (string Label, string Value)[] FrameRates =
    {
        ("Any", "0"), ("≤ 60 fps", "60"), ("≤ 30 fps", "30"),
    };
    private static readonly (string Label, string Value)[] SponsorModes =
    {
        ("Off", "off"), ("Mark as chapters", "mark"), ("Cut out of the file", "remove"),
    };
    private static readonly (string Label, string Value)[] CookieSources =
    {
        ("None", "none"), ("From a browser", "browser"), ("From a cookies.txt file", "file"),
    };
    /// yt-dlp's --cookies-from-browser names. No Safari: yt-dlp reads it on
    /// macOS only. Edge first: this is Windows.
    private static readonly (string Label, string Value)[] Browsers =
    {
        ("Edge", "edge"), ("Chrome", "chrome"), ("Firefox", "firefox"), ("Brave", "brave"),
        ("Chromium", "chromium"), ("Opera", "opera"), ("Vivaldi", "vivaldi"), ("Whale", "whale"),
    };
    private static readonly (string Label, string Value)[] Downloaders =
    {
        ("Built in", "native"), ("aria2c", "aria2c"),
    };

    /* Every switch carries its own explanation, the way the GTK rows do. These
     * were tooltips once, and a tooltip nobody hovers over is not documentation
     * -- the reason to turn a pass off is the thing somebody needs to read at
     * the moment they are deciding. */
    private static readonly (string Title, string Note, string Field)[] Passes =
    {
        ("Sync", "Walk the whole channel, stopping at the first video already archived.", "Sync"),
        ("Lazy", "Skip the up-front enumeration.", "Lazy"),
        ("Skip PO token", "Do not run the PO token provider for this run.", "NoPot"),
        ("Skip comments", "Do not run the comments pass.", "NoComments"),
        ("Skip subtitles", "Do not capture subtitles.", "NoSubs"),
        ("Skip thumbnail", "Do not capture the thumbnail.", "NoThumbnail"),
        ("Skip metadata", "Do not run the metadata pass.", "NoMetadata"),
    };

    public DownloadsPage()
    {
        InitializeComponent();
        LogList.ItemsSource = _log;
        BuildCombos();
        BuildPasses();
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        LoadFormIntoControls();
        RefreshProfiles();
        ApplyState(Model.Runner.CurrentState());
        UpdatePreview();

        _drain = DispatcherQueue.CreateTimer();
        _drain.Interval = DrainInterval;
        _drain.Tick += (_, _) => DrainOnce();
        _drain.Start();
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        base.OnNavigatedFrom(e);
        /* Stopped on the way out, and this is not tidiness. A DispatcherQueueTimer
         * keeps a strong reference to its handler, so a running timer holds this
         * page and everything it draws alive for the life of the window -- and
         * every trip back here would start another one, all draining the same
         * runner. */
        _drain?.Stop();
        _drain = null;
    }

    // MARK: - Building the form

    private void BuildCombos()
    {
        _suppressChanges = true;
        Fill(ModeBox, Modes);
        Fill(QualityBox, Qualities);
        Fill(CodecBox, Codecs);
        Fill(AudioCodecBox, AudioCodecs);
        Fill(ContainerBox, Containers);
        Fill(FpsBox, FrameRates);
        Fill(SponsorModeBox, SponsorModes);
        Fill(CookiesSourceBox, CookieSources);
        Fill(CookiesBrowserBox, Browsers);
        Fill(DownloaderBox, Downloaders);
        _suppressChanges = false;
    }

    private static void Fill(ComboBox box, (string Label, string Value)[] items)
    {
        foreach (var (label, value) in items)
            box.Items.Add(new ComboBoxItem { Content = label, Tag = value });
    }

    private static void Select(ComboBox box, string value)
    {
        foreach (var item in box.Items.OfType<ComboBoxItem>())
        {
            if ((string?)item.Tag == value) { box.SelectedItem = item; return; }
        }
        if (box.Items.Count > 0) box.SelectedIndex = 0;
    }

    private static string Selected(ComboBox box)
        => (box.SelectedItem as ComboBoxItem)?.Tag as string ?? "";

    private void BuildPasses()
    {
        foreach (var (title, note, field) in Passes)
        {
            var toggle = new ToggleSwitch { OnContent = "On", OffContent = "Off", Tag = field };
            toggle.Toggled += OnPassToggled;
            PassesPanel.Children.Add(new OptionRow
            {
                Header = title,
                Description = note,
                Content = toggle,
            });
        }
    }

    private void LoadFormIntoControls()
    {
        _suppressChanges = true;

        UrlBox.Text = Form.Opts.Url;
        DataRootBox.Text = Form.Opts.DataRoot;
        /* Applied even when empty: an empty range means "all of them", which
         * is a real setting rather than an unset one, and leaving a previous
         * URL's selection in place would silently narrow the next run. */
        ItemsBox.Text = Form.Opts.Items;
        ExtraArgsBox.Text = Form.ExtraArgsText;
        WorkersBox.Value = Math.Max(1, Form.Opts.Workers);

        Select(ModeBox, Form.Opts.Mode);
        Select(QualityBox, Form.Opts.Quality);
        Select(CodecBox, Form.Opts.Codec);
        Select(AudioCodecBox, Form.Opts.AudioCodec);
        Select(ContainerBox, Form.Opts.Container);
        Select(FpsBox, Form.Opts.Fps.ToString(CultureInfo.InvariantCulture));
        SubLangsBox.Text = Form.Opts.SubLangs;
        ChaptersSwitch.IsOn = !Form.Opts.NoChapters;
        Select(SponsorModeBox, Form.SponsorMode);
        SponsorCatsBox.Text = Form.SponsorCats;

        /* The Connection rows read Settings, not the form: they are settings. */
        var st = Model.Settings;
        Select(CookiesSourceBox, st.CookiesSource);
        Select(CookiesBrowserBox, st.CookiesBrowser);
        CookiesProfileBox.Text = st.CookiesProfile;
        CookiesFileBox.Text = st.CookiesFile;
        ProxyBox.Text = st.Proxy;
        LimitRateBox.Text = st.LimitRate;
        Select(DownloaderBox, st.Downloader);
        UpdateConnectionVisibility();

        foreach (var row in PassesPanel.Children.OfType<OptionRow>())
        {
            if (row.Content is ToggleSwitch t && t.Tag is string field)
                t.IsOn = ReadPass(field);
        }

        ProfileStatus.Text = Form.Status;
        ProfileStatus.Foreground = Controls.Resource<Brush>(
            Form.StatusIsError ? "SystemFillColorCriticalBrush" : "TextFillColorSecondaryBrush");

        _suppressChanges = false;
        UpdateSensitivity();
    }

    /* Grey out what cannot apply, mirroring RunOptions.DropInapplicable rule
     * for rule -- the one decides what the user SEES, the other what is SENT,
     * and the two must never disagree. Disabled rather than hidden, so the
     * value is still visible and comes back when the mode does. */
    private void UpdateSensitivity()
    {
        var media = Form.MediaOptionsApply;
        var mode = Selected(SponsorModeBox);
        FpsBox.IsEnabled = media;
        SponsorModeBox.IsEnabled = media;
        SponsorCatsBox.IsEnabled = media && mode != "off";
        ChaptersSwitch.IsEnabled = media && mode != "mark";
        SubLangsBox.IsEnabled = !Form.Opts.NoSubs;
    }

    private void UpdateConnectionVisibility()
    {
        var source = Selected(CookiesSourceBox);
        CookiesBrowserRow.Visibility = source == "browser" ? Visibility.Visible : Visibility.Collapsed;
        CookiesProfileRow.Visibility = source == "browser" ? Visibility.Visible : Visibility.Collapsed;
        CookiesFileRow.Visibility = source == "file" ? Visibility.Visible : Visibility.Collapsed;
    }

    /* A switch statement rather than reflection over the field name. Reflection
     * would be shorter and would fail at RUN time on a typo, in a build that has
     * never been compiled and cannot be tested -- and .NET trimming, which a
     * self-contained WinUI publish does by default, is entitled to remove a
     * property nothing appears to reference. */
    private bool ReadPass(string field) => field switch
    {
        "Sync" => Form.Opts.Sync,
        "Lazy" => Form.Opts.Lazy,
        "NoPot" => Form.Opts.NoPot,
        "NoComments" => Form.Opts.NoComments,
        "NoSubs" => Form.Opts.NoSubs,
        "NoThumbnail" => Form.Opts.NoThumbnail,
        "NoMetadata" => Form.Opts.NoMetadata,
        _ => false,
    };

    private void WritePass(string field, bool value)
    {
        switch (field)
        {
            case "Sync": Form.Opts.Sync = value; break;
            case "Lazy": Form.Opts.Lazy = value; break;
            case "NoPot": Form.Opts.NoPot = value; break;
            case "NoComments": Form.Opts.NoComments = value; break;
            case "NoSubs": Form.Opts.NoSubs = value; break;
            case "NoThumbnail": Form.Opts.NoThumbnail = value; break;
            case "NoMetadata": Form.Opts.NoMetadata = value; break;
        }
    }

    // MARK: - Form events

    private void OnUrlChanged(object sender, TextChangedEventArgs e)
    {
        if (_suppressChanges) return;
        Form.Opts.Url = UrlBox.Text;
        /* Editing the URL invalidates any preview on screen: a Quality row
         * still listing the previous video's heights is worse than one listing
         * the generic ladder, because it looks like knowledge. */
        if (_probe != null || _probeCts != null) ClearProbe();
        UpdatePreview();
    }

    private void OnDataRootChanged(object sender, TextChangedEventArgs e)
    {
        if (_suppressChanges) return;
        Form.Opts.DataRoot = DataRootBox.Text;
        /* Kept in memory as it is typed and written on commit, not on every
         * keystroke: a destination path is thirty characters and thirty writes
         * of settings.json is thirty writes too many. */
        Model.Settings.DataRoot = DataRootBox.Text;
        UpdatePreview();
    }

    private void OnOptionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_suppressChanges) return;
        Form.Opts.Mode = Selected(ModeBox);
        Form.Opts.Quality = Selected(QualityBox);
        Form.Opts.Codec = Selected(CodecBox);
        Form.Opts.AudioCodec = Selected(AudioCodecBox);
        Form.Opts.Container = Selected(ContainerBox);
        Form.Opts.Fps = int.TryParse(Selected(FpsBox), NumberStyles.Integer,
            CultureInfo.InvariantCulture, out var fps) ? fps : 0;
        UpdateSensitivity();

        /* Changing the codec re-filters the Quality list: 1440p is commonly
         * published only in VP9, so a height that only one codec offers cannot
         * stay selected once another is chosen. Only the codec box triggers
         * this -- rebuilding on every selection would reset Quality whenever
         * the user touched Container. */
        if (_probe != null && ReferenceEquals(sender, CodecBox))
        {
            _suppressChanges = true;
            RebuildCombos();
            _suppressChanges = false;
        }

        UpdatePreview();
    }

    private void OnWorkersChanged(NumberBox sender, NumberBoxValueChangedEventArgs args)
    {
        if (_suppressChanges) return;
        /* NumberBox reports NaN when its text is not a number, which is the
         * state it is in for the moment between clearing the box and typing a
         * digit. (int)double.NaN is 0 with no exception, so without this the
         * workers count silently becomes 0. */
        var value = double.IsNaN(args.NewValue) ? 1 : (int)Math.Clamp(args.NewValue, 1, 16);
        Form.Opts.Workers = value;
        Model.Settings.DefaultWorkers = value;
        Model.Settings.Save();
        UpdatePreview();
    }

    private void OnExtraArgsChanged(object sender, TextChangedEventArgs e)
    {
        if (_suppressChanges) return;
        Form.ExtraArgsText = ExtraArgsBox.Text;
        Form.Opts.YtdlpArgs = ExtraArgsBox.Text
            .Replace("\r\n", "\n").Replace('\r', '\n')
            .Split('\n')
            .Select(l => l.Trim())
            .Where(l => l.Length > 0)
            .ToList();
        UpdatePreview();
    }

    private void OnPassToggled(object sender, RoutedEventArgs e)
    {
        if (_suppressChanges) return;
        if (sender is ToggleSwitch t && t.Tag is string field) WritePass(field, t.IsOn);
        UpdateSensitivity();
        UpdatePreview();
    }

    private void OnExtrasTextChanged(object sender, TextChangedEventArgs e)
    {
        if (_suppressChanges) return;
        Form.Opts.SubLangs = SubLangsBox.Text;
        Form.SponsorCats = SponsorCatsBox.Text;
        UpdatePreview();
    }

    private void OnChaptersToggled(object sender, RoutedEventArgs e)
    {
        if (_suppressChanges) return;
        Form.Opts.NoChapters = !ChaptersSwitch.IsOn;
        UpdatePreview();
    }

    private void OnSponsorModeChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_suppressChanges) return;
        Form.SponsorMode = Selected(SponsorModeBox);
        UpdateSensitivity();
        UpdatePreview();
    }

    /* Every Connection change is saved and handed straight to the Runner.
     * Saved per change rather than on commit -- unlike the destination --
     * because a proxy typed, used for a run, and then gone on the next launch
     * would be a setting that does not behave like one; the file is a few
     * hundred bytes. */
    private void ConnectionChanged()
    {
        if (_suppressChanges) return;
        var st = Model.Settings;
        st.CookiesSource = Selected(CookiesSourceBox);
        st.CookiesBrowser = Selected(CookiesBrowserBox);
        st.CookiesProfile = CookiesProfileBox.Text;
        st.CookiesFile = CookiesFileBox.Text;
        st.Proxy = ProxyBox.Text;
        st.LimitRate = LimitRateBox.Text;
        st.Downloader = Selected(DownloaderBox);
        st.Save();
        Model.Runner.SetConnection(st.Connection());
        UpdateConnectionVisibility();
        UpdatePreview();
    }

    private void OnConnectionSelectionChanged(object sender, SelectionChangedEventArgs e)
        => ConnectionChanged();

    private void OnConnectionTextChanged(object sender, TextChangedEventArgs e)
        => ConnectionChanged();

    /* The same HWND initialisation as the folder picker below, for the same
     * reason: an unpackaged desktop app must tell a WinRT picker its owner. */
    private async void OnChooseCookieFile(object sender, RoutedEventArgs e)
    {
        try
        {
            var picker = new Windows.Storage.Pickers.FileOpenPicker
            {
                SuggestedStartLocation = Windows.Storage.Pickers.PickerLocationId.Downloads,
            };
            picker.FileTypeFilter.Add(".txt");
            picker.FileTypeFilter.Add("*");
            var window = App.Window;
            if (window is null) return;
            WinRT.Interop.InitializeWithWindow.Initialize(
                picker, WinRT.Interop.WindowNative.GetWindowHandle(window));
            var file = await picker.PickSingleFileAsync();
            if (file is null) return;
            CookiesFileBox.Text = file.Path; // TextChanged saves it
        }
        catch (Exception) { /* dismissed or unavailable -- nothing to change */ }
    }

    /* With no URL typed the preview would read ytdl "" -- which looks like a bug
     * rather than an empty field, and is what it showed right after a queue add
     * cleared the box. A placeholder keeps the rest of the command visible, so
     * the options you have set are still readable while you paste a URL. */
    private void UpdatePreview()
    {
        /* What the form means, with the connection stamped on as the Runner
         * will: the preview is the command that runs. CommandPreview masks a
         * proxy password. */
        var shown = Form.EffectiveOptions();
        shown.SetConnection(Model.Settings.Connection());
        if (string.IsNullOrWhiteSpace(shown.Url)) shown.Url = "<URL>";
        PreviewText.Text = shown.CommandPreview();
    }

    // MARK: - The URL preview

    private void OnItemsChanged(object sender, TextChangedEventArgs e)
    {
        if (_suppressChanges) return;
        Form.Opts.Items = ItemsBox.Text;
        /* _writingItems is set while WriteItemsFromChecks is the one editing,
         * which is what keeps ticking a box from immediately re-deriving the
         * boxes from the text it just wrote. */
        if (!_writingItems) SyncChecksFromItems();
        UpdatePreview();
    }

    private async void OnProbe(object sender, RoutedEventArgs e)
    {
        /* A second press while one is running cancels it rather than starting
         * a race between two answers for the same form. */
        if (_probeCts != null)
        {
            _probeCts.Cancel();
            _probeCts = null;
            SetProbeBusy(false);
            SetProbeStatus("", isError: false);
            return;
        }

        if (string.IsNullOrWhiteSpace(UrlBox.Text))
        {
            PreviewSection.Visibility = Visibility.Visible;
            ProbeMetaPanel.Visibility = Visibility.Collapsed;
            SetProbeStatus("Paste a URL first.", isError: true);
            return;
        }

        var cts = new CancellationTokenSource();
        _probeCts = cts;
        PreviewSection.Visibility = Visibility.Visible;
        ProbeMetaPanel.Visibility = Visibility.Collapsed;
        SetProbeBusy(true);
        SetProbeStatus("Reading the URL…", isError: false);

        var request = new ProbeRequest
        {
            Url = UrlBox.Text,
            Items = ItemsBox.Text,
            NoPot = Form.Opts.NoPot,
            PotPort = Form.Opts.PotPort,
            /* The same list the run would send, so a URL that needs
             * --cookies-from-browser is probed with it too -- a preview that
             * fails where the download would succeed is worse than none. */
            ExtraArgs = new List<string>(Form.Opts.YtdlpArgs),
            /* Cookies and proxy only: `ytdl --probe` refuses the speed limit
             * and the downloader, which govern moving media bytes. */
            ConnectionArgs = Model.Settings.Connection().ConnectionArgs(forProbe: true),
        };

        ProbeResult result;
        try
        {
            result = await ProbeRunner.RunAsync(request, cts.Token);
        }
        catch (OperationCanceledException)
        {
            result = new ProbeResult { Cancelled = true };
        }

        /* A probe whose token has been replaced belongs to a URL the user has
         * since changed. Its answer describes a different video, so it is
         * dropped rather than written into the form. */
        if (!ReferenceEquals(_probeCts, cts)) return;
        _probeCts = null;
        SetProbeBusy(false);

        if (result.Cancelled)
        {
            SetProbeStatus("", isError: false);
            return;
        }
        if (!result.Ok)
        {
            /* The message is yt-dlp's or ytdl.ps1's own sentence -- "Video
             * unavailable", "Sign in to confirm your age" -- not one invented
             * here, because theirs says what to do about it. */
            ProbeMetaPanel.Visibility = Visibility.Collapsed;
            SetProbeStatus(result.Error, isError: true);
            return;
        }

        _probe = result.Probe;
        if (_probe!.ProbeVersion > Probe.SupportedVersion)
        {
            /* Said, not guessed around. The contract's rule is that fields are
             * added and never redefined, so a newer document is still readable
             * -- but the one thing this app must not do is present a partial
             * reading of it as a complete one. */
            SetProbeStatus(
                "This pipeline's probe is newer than this app knows about; "
                + "some of what it reported is not shown.", isError: false);
        }
        else
        {
            SetProbeStatus("", isError: false);
        }

        ApplyProbe();
    }

    private void SetProbeBusy(bool busy)
    {
        ProbeSpinner.IsActive = busy;
        ProbeSpinner.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
        ProbeButtonText.Text = busy ? "Stop" : "Preview";
        ProbeIcon.Glyph = busy ? "" : "";
    }

    private void SetProbeStatus(string text, bool isError)
    {
        ProbeStatusText.Text = text;
        ProbeStatusText.Foreground = Controls.Resource<Brush>(
            isError ? "SystemFillColorCriticalBrush" : "TextFillColorSecondaryBrush");
        ProbeStatusPanel.Visibility =
            string.IsNullOrEmpty(text) && !ProbeSpinner.IsActive
                ? Visibility.Collapsed
                : Visibility.Visible;
    }

    /* Everything the preview put on screen goes away, and every combo goes
     * back to its full static list.
     *
     * Called when the URL changes, which is the important case: a Quality row
     * still showing the heights of the PREVIOUS video, against a URL that has
     * been replaced, is worse than one showing the generic ladder -- it looks
     * like knowledge and is not. */
    private void ClearProbe()
    {
        _probeCts?.Cancel();
        _probeCts = null;
        _probe = null;
        _entryChecks.Clear();
        EntriesList.Items.Clear();
        FormatsList.Items.Clear();
        ProbeThumb.Source = null;
        PreviewSection.Visibility = Visibility.Collapsed;
        ProbeMetaPanel.Visibility = Visibility.Collapsed;
        ProbeNote.Visibility = Visibility.Collapsed;
        FormatsExpander.Visibility = Visibility.Collapsed;
        EntriesExpander.Visibility = Visibility.Collapsed;
        SetProbeBusy(false);
        SetProbeStatus("", isError: false);

        _suppressChanges = true;
        RebuildCombos();
        _suppressChanges = false;
    }

    /* The four combos are rebuilt from the probe when there is one and from
     * the static tables when there is not. "Best" and "Any" are pinned first
     * and are NOT probed values: they are pipeline concepts, always available,
     * meaning "no cap" and "no preference". */
    private void RebuildCombos()
    {
        var quality = new List<(string, string)> { ("Best", "best") };
        /* Cross-filtered by the selected codec: 1440p is commonly published
         * only in VP9, so a list built from the union of every height offers a
         * combination this video does not have -- the same silent wrong answer
         * the static ladder gave, with better-looking numbers in it. */
        var heights = _probe?.HeightsForCodec(Form.Opts.Codec);
        if (heights != null)
        {
            foreach (var h in heights)
                quality.Add(($"{h}p", h.ToString(CultureInfo.InvariantCulture)));
        }
        else
        {
            foreach (var q in Qualities)
                if (q.Value != "best") quality.Add((q.Label, q.Value));
        }

        var codecs = new List<(string, string)> { ("Any", "any") };
        foreach (var c in _probe?.VideoCodecs ?? new List<string> { "avc1", "vp9", "av01" })
            codecs.Add((VideoCodecLabel(c), c));

        var audio = new List<(string, string)> { ("Any", "any") };
        foreach (var c in _probe?.AudioCodecs ?? new List<string> { "opus", "aac", "mp3", "flac" })
            audio.Add((AudioCodecLabel(c), c));

        var containers = new List<(string, string)>();
        var containerIds = _probe?.Containers;
        /* Never empty: a ComboBox with no items renders as a blank control
         * that cannot be opened, which reads as a broken widget rather than as
         * "this video has none of these". mkv is always muxable. */
        if (containerIds == null || containerIds.Count == 0)
            containerIds = new List<string> { "mkv", "mp4", "webm" };
        foreach (var c in containerIds) containers.Add((ContainerLabel(c), c));

        Refill(QualityBox, quality, Form.Opts.Quality);
        Refill(CodecBox, codecs, Form.Opts.Codec);
        Refill(AudioCodecBox, audio, Form.Opts.AudioCodec);
        Refill(ContainerBox, containers, Form.Opts.Container);

        /* The form follows the controls, not the other way round: a value that
         * did not survive the rebuild must not stay in Opts, or the command
         * preview would show a flag the picker no longer offers. */
        Form.Opts.Quality = Selected(QualityBox);
        Form.Opts.Codec = Selected(CodecBox);
        Form.Opts.AudioCodec = Selected(AudioCodecBox);
        Form.Opts.Container = Selected(ContainerBox);
    }

    /// Replace a combo's items, keeping <paramref name="keep"/> selected when
    /// the new set still contains it and falling back to the first entry when
    /// it does not.
    private static void Refill(
        ComboBox box, List<(string Label, string Value)> items, string keep)
    {
        box.Items.Clear();
        foreach (var (label, value) in items)
            box.Items.Add(new ComboBoxItem { Content = label, Tag = value });
        Select(box, keep);
    }

    private static string VideoCodecLabel(string id) => id switch
    {
        "avc1" => "AVC1 / H.264",
        "vp9" => "VP9",
        "av01" => "AV1",
        _ => id,
    };

    private static string AudioCodecLabel(string id) => id switch
    {
        "opus" => "Opus",
        "aac" => "AAC",
        "mp3" => "MP3",
        "flac" => "FLAC",
        _ => id,
    };

    private static string ContainerLabel(string id) => id switch
    {
        "mkv" => "MKV",
        "mp4" => "MP4",
        "webm" => "WebM",
        _ => id,
    };

    /// Everything the probe knows, written into the form and onto the screen.
    private void ApplyProbe()
    {
        var p = _probe;
        if (p == null) return;

        var wantedQuality = Form.Opts.Quality;
        var wantedCodec = Form.Opts.Codec;
        var wantedAudio = Form.Opts.AudioCodec;
        var wantedContainer = Form.Opts.Container;

        _suppressChanges = true;
        RebuildCombos();
        _suppressChanges = false;

        var dropped = Form.Opts.Quality != wantedQuality
            || Form.Opts.Codec != wantedCodec
            || Form.Opts.AudioCodec != wantedAudio
            || Form.Opts.Container != wantedContainer;

        // --- The metadata row ---
        ProbeTitle.Text = string.IsNullOrEmpty(p.Title) ? "(no title)" : p.Title;

        var bits = new List<string>();
        if (!string.IsNullOrEmpty(p.Uploader)) bits.Add(p.Uploader);
        if (p.Duration > 0) bits.Add(Format.Duration(p.Duration));
        if (p.ViewCount > 0) bits.Add($"{Format.Count(p.ViewCount)} views");
        if (!string.IsNullOrEmpty(p.UploadDate))
        {
            var when = Format.UploadDate(p.UploadDate);
            if (!string.IsNullOrEmpty(when)) bits.Add(when);
        }
        if (p.IsPlaylist)
            bits.Add($"{p.EntryCount} item{(p.EntryCount == 1 ? "" : "s")}");
        ProbeMeta.Text = string.Join(" · ", bits);

        ProbeThumb.Source = null;
        if (!string.IsNullOrEmpty(p.Thumbnail)
            && Uri.TryCreate(p.Thumbnail, UriKind.Absolute, out var thumbUri))
        {
            /* BitmapImage fetches on its own and raises ImageFailed rather
             * than throwing, so a thumbnail that will not load leaves the rest
             * of the preview exactly as it was. It is the one part of this
             * allowed to be absent. */
            ProbeThumb.Source = new BitmapImage(thumbUri);
        }
        ProbeMetaPanel.Visibility = Visibility.Visible;

        // --- The note: everything the user should not have to infer ---
        var note = new List<string>();
        if (dropped)
        {
            note.Add("Some of what was selected is not offered for this URL, "
                + "so those rows moved to what is.");
        }
        if (p.FromFallback)
        {
            note.Add("Read with yt-dlp directly: the installed pipeline "
                + "predates `ytdl --probe`, so the PO token provider was not "
                + "used and the list may be short.");
        }
        else if (!p.PotHealthy && !string.IsNullOrEmpty(p.PotNote))
        {
            note.Add(p.PotNote);
        }
        if (!string.IsNullOrEmpty(p.FormatsFromId))
        {
            /* Named rather than presented as the playlist's own, because a
             * channel can serve 4K AV1 for a recent upload and 360p AVC for
             * one from 2011. */
            var which = string.IsNullOrEmpty(p.FormatsFromTitle)
                ? p.FormatsFromId
                : p.FormatsFromTitle;
            note.Add($"Formats shown are for \"{which}\".");
        }
        if (p.AgeLimit > 0) note.Add($"Age restricted ({p.AgeLimit}+).");
        if (p.LiveStatus == "is_live") note.Add("This is live right now.");

        ProbeNote.Text = string.Join(" ", note);
        ProbeNote.Visibility = note.Count > 0 ? Visibility.Visible : Visibility.Collapsed;

        FillFormatsList(p);
        FillEntriesList(p);
        UpdatePreview();
    }

    private void FillFormatsList(Probe p)
    {
        FormatsList.Items.Clear();
        foreach (var f in p.Formats)
        {
            var detail = new List<string> { string.IsNullOrEmpty(f.FormatId) ? "?" : f.FormatId };
            if (f.HasVideoStream) detail.Add(f.VCodec);
            if (f.HasAudioStream) detail.Add(f.ACodec);
            if (!string.IsNullOrEmpty(f.Ext)) detail.Add(f.Ext);
            /* An exact size and an estimate are shown differently on purpose:
             * the tilde is the difference between a fact and yt-dlp's
             * tbr*duration guess, and presenting the guess as a fact is how a
             * 4 GB download surprises someone. */
            if (f.Filesize > 0) detail.Add(Format.Bytes(f.Filesize));
            else if (f.FilesizeApprox > 0) detail.Add("~" + Format.Bytes(f.FilesizeApprox));

            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            row.Children.Add(new TextBlock
            {
                Text = f.Height > 0 ? $"{f.Height}p" : "Audio",
                Width = 64,
            });
            row.Children.Add(new TextBlock
            {
                Text = string.Join(" · ", detail),
                Style = Controls.Resource<Style>("CaptionSecondary"),
            });
            FormatsList.Items.Add(row);
        }

        FormatsHeader.Text =
            $"Formats — {p.Formats.Count} rendition{(p.Formats.Count == 1 ? "" : "s")} "
            + "this video actually has";
        FormatsExpander.Visibility =
            p.Formats.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void FillEntriesList(Probe p)
    {
        _entryChecks.Clear();
        EntriesList.Items.Clear();

        if (p.Entries.Count == 0)
        {
            EntriesExpander.Visibility = Visibility.Collapsed;
            return;
        }

        var shown = Math.Min(p.Entries.Count, MaxEntryRows);
        for (var i = 0; i < shown; i++)
        {
            var e = p.Entries[i];
            var detail = new List<string>();
            if (e.Duration > 0) detail.Add(Format.Duration(e.Duration));
            if (!string.IsNullOrEmpty(e.VideoId)) detail.Add(e.VideoId);

            var content = new StackPanel { Spacing = 1 };
            content.Children.Add(new TextBlock
            {
                Text = $"{e.Index}. {(string.IsNullOrEmpty(e.Title) ? "(untitled)" : e.Title)}",
                TextWrapping = TextWrapping.NoWrap,
            });
            content.Children.Add(new TextBlock
            {
                Text = string.Join(" · ", detail),
                Style = Controls.Resource<Style>("CaptionSecondary"),
            });

            var check = new CheckBox
            {
                Content = content,
                IsChecked = true,
                /* The PLAYLIST position, carried on the control rather than
                 * recomputed from the row's place in the list: the list is
                 * truncated, and a position derived from the visible order
                 * queues the wrong videos -- a bug whose first symptom is a
                 * successful download of something nobody asked for. */
                Tag = e.Index,
            };
            check.Checked += OnEntryToggled;
            check.Unchecked += OnEntryToggled;
            EntriesList.Items.Add(check);
            _entryChecks.Add(check);
        }

        var header = $"Playlist items — {p.EntryCount} item{(p.EntryCount == 1 ? "" : "s")}";
        if (p.PlaylistCount > p.EntryCount) header += $" of {p.PlaylistCount}";
        if (shown < p.Entries.Count) header += $" · first {shown} shown";
        if (p.EntriesTruncated)
        {
            /* Said out loud, because a silently short list of a 4,000-upload
             * channel reads as a complete one. */
            header += " · the listing was cut short; use the range field to reach the rest";
        }
        EntriesHeader.Text = header;
        EntriesExpander.Visibility = Visibility.Visible;

        SyncChecksFromItems();
    }

    private void OnEntryToggled(object sender, RoutedEventArgs e)
    {
        if (_suppressChanges) return;
        WriteItemsFromChecks();
    }

    private void OnSelectAllItems(object sender, RoutedEventArgs e) => SetAllItems(true);

    private void OnSelectNoItems(object sender, RoutedEventArgs e) => SetAllItems(false);

    private void SetAllItems(bool on)
    {
        _suppressChanges = true;
        foreach (var c in _entryChecks) c.IsChecked = on;
        _suppressChanges = false;
        WriteItemsFromChecks();
    }

    /* Turn the ticked rows into an --items value.
     *
     * The empty string is a meaningful answer and not a failure to produce
     * one: ytdl with no --items takes the whole listing, which is what
     * "everything is ticked" means. Writing out "1-200" instead would silently
     * CAP a 4,000-video channel at the entries this window happened to
     * enumerate -- the run would succeed and quietly archive a twentieth of
     * what was asked for. So a full selection only collapses to "" when the
     * list is known to be complete. */
    private void WriteItemsFromChecks()
    {
        if (_entryChecks.Count == 0) return;

        var picked = new List<int>();
        var all = true;
        foreach (var c in _entryChecks)
        {
            var index = c.Tag is int i ? i : 0;
            if (c.IsChecked == true) picked.Add(index);
            else all = false;
        }

        var complete = all
            && _probe != null
            && !_probe.EntriesTruncated
            && _entryChecks.Count == _probe.Entries.Count;

        var spec = complete ? "" : ItemsRange.Compact(picked);

        _writingItems = true;
        _suppressChanges = true;
        ItemsBox.Text = spec;
        _suppressChanges = false;
        _writingItems = false;

        Form.Opts.Items = spec;
        UpdatePreview();
    }

    /// The reverse: a range typed, or restored from a profile, re-ticks the
    /// rows, so the two halves of the same statement cannot disagree on screen.
    private void SyncChecksFromItems()
    {
        if (_entryChecks.Count == 0) return;

        var spec = ItemsBox.Text?.Trim() ?? "";
        var empty = spec.Length == 0;
        var want = new HashSet<int>(ItemsRange.Parse(spec));

        _suppressChanges = true;
        foreach (var c in _entryChecks)
        {
            var index = c.Tag is int i ? i : 0;
            c.IsChecked = empty || want.Contains(index);
        }
        _suppressChanges = false;
    }

    // MARK: - Actions

    private async void OnAddToQueue(object sender, RoutedEventArgs e)
    {
        try
        {
            Model.Runner.Enqueue(Form.EffectiveOptions());
            /* The URL is cleared; the options are not. Queueing five videos with
             * the same settings is the common case, and re-picking them each
             * time would be the wrong kind of tidy. */
            Form.Opts.Url = "";
            _suppressChanges = true;
            UrlBox.Text = "";
            _suppressChanges = false;
            UpdatePreview();

            // The destination is committed here rather than on every keystroke.
            Model.Settings.Save();
        }
        catch (Exception ex)
        {
            await ShowError(ex.Message);
        }
    }

    private async void OnCancel(object sender, RoutedEventArgs e)
    {
        if (!Model.Runner.Cancel()) await ShowError("Nothing is running.");
    }

    private void OnTogglePause(object sender, RoutedEventArgs e)
    {
        var state = Model.Runner.CurrentState();
        Model.Runner.SetPaused(!state.Paused);
    }

    private void OnClearHistory(object sender, RoutedEventArgs e) => Model.Runner.ClearHistory();

    private async void OnChooseFolder(object sender, RoutedEventArgs e)
    {
        var picked = await PickFolderAsync();
        if (picked is null) return;

        Form.Opts.DataRoot = picked;
        _suppressChanges = true;
        DataRootBox.Text = picked;
        _suppressChanges = false;

        Model.Settings.DataRoot = picked;
        Model.Settings.Save();
        UpdatePreview();
    }

    /* THE WinUI 3 FOLDER-PICKER TRAP, and it is worth stating once because it
     * costs an afternoon otherwise.
     *
     * FolderPicker is a WinRT type that expects to be told which window owns it.
     * In a packaged UWP app the system knows; in a desktop app it does not, and
     * the picker throws COMException 0x80070005 (or, worse, silently returns
     * null) unless it is initialised with the window's HWND first. There is no
     * compile-time hint that this is required.
     *
     * FileTypeFilter looks pointless on a FOLDER picker and is not optional
     * either -- the picker fails to open with an empty filter list. "*" is the
     * documented incantation. */
    /* No caller-supplied button text. FolderPicker has CommitButtonText and
     * nothing else: unlike NSOpenPanel, which has a `prompt` for the button and
     * a separate `message` for the explanation, there is nowhere here to put a
     * sentence. Passing one in made the confirm button read as a paragraph. The
     * explanation belongs on the control that opens the picker, which already
     * carries it as a tooltip. */
    internal static async System.Threading.Tasks.Task<string?> PickFolderAsync()
    {
        try
        {
            var picker = new Windows.Storage.Pickers.FolderPicker
            {
                SuggestedStartLocation = Windows.Storage.Pickers.PickerLocationId.VideosLibrary,
                CommitButtonText = "Choose",
            };
            picker.FileTypeFilter.Add("*");

            var window = App.Window;
            if (window is null) return null;
            var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(window);
            WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);

            var folder = await picker.PickSingleFolderAsync();
            return folder?.Path;
        }
        catch (Exception) { return null; }
    }

    // MARK: - Profiles

    private void RefreshProfiles()
    {
        _suppressChanges = true;
        ProfileBox.Items.Clear();
        /* "(no profile)" is a real state, not a placeholder: it is what the
         * window is in before anything has been saved. */
        ProfileBox.Items.Add(new ComboBoxItem { Content = "(no profile)", Tag = null });
        foreach (var p in Model.Profiles.Profiles)
            ProfileBox.Items.Add(new ComboBoxItem { Content = p.Name, Tag = p.Name });

        var wanted = Form.SelectedProfile;
        ProfileBox.SelectedIndex = 0;
        if (wanted is not null)
        {
            foreach (var item in ProfileBox.Items.OfType<ComboBoxItem>())
            {
                if ((string?)item.Tag == wanted) { ProfileBox.SelectedItem = item; break; }
            }
        }
        _suppressChanges = false;
    }

    private void OnProfileSelected(object sender, SelectionChangedEventArgs e)
    {
        if (_suppressChanges) return;
        var name = (ProfileBox.SelectedItem as ComboBoxItem)?.Tag as string;
        Form.SelectedProfile = name;

        try
        {
            if (name is not null && Model.Profiles.Get(name) is { } p)
            {
                Form.Apply(p.Opts);
                Model.Profiles.Activate(name);
            }
            else
            {
                /* Clearing the selection does NOT reset the form. The options
                 * stay exactly as they are; you have simply stopped calling them
                 * a profile. */
                Model.Profiles.Activate(null);
            }
            SetProfileStatus("", isError: false);
        }
        catch (Exception ex)
        {
            SetProfileStatus(ex.Message, isError: true);
        }

        LoadFormIntoControls();
        UpdatePreview();
    }

    private async void OnSaveProfile(object sender, RoutedEventArgs e)
    {
        var name = await AskForName("Save these options as a profile",
            "Every option except the URL is saved under this name.",
            Form.SelectedProfile ?? "", "Save");
        if (name is null) return;

        try
        {
            Model.Profiles.Save(name, Form.EffectiveOptions());
            Form.SelectedProfile = Model.Profiles.Active;
            RefreshProfiles();
            SetProfileStatus($"Saved “{name}”.", isError: false);
        }
        catch (Exception ex) { SetProfileStatus(ex.Message, isError: true); }
    }

    private async void OnRenameProfile(object sender, RoutedEventArgs e)
    {
        if (Form.SelectedProfile is not { } from)
        {
            SetProfileStatus("Select a profile to rename.", isError: true);
            return;
        }

        var name = await AskForName("Rename profile",
            "The options stay as they are; only the name changes.", from, "Rename");
        if (name is null) return;

        try
        {
            Model.Profiles.Rename(from, name);
            Form.SelectedProfile = Model.Profiles.Active;
            RefreshProfiles();
            SetProfileStatus($"Renamed to “{name}”.", isError: false);
        }
        catch (Exception ex) { SetProfileStatus(ex.Message, isError: true); }
    }

    private void OnDeleteProfile(object sender, RoutedEventArgs e)
    {
        if (Form.SelectedProfile is not { } name)
        {
            SetProfileStatus("Select a profile to delete.", isError: true);
            return;
        }
        try
        {
            Model.Profiles.Delete(name);
            Form.SelectedProfile = null;
            RefreshProfiles();
            SetProfileStatus($"Deleted “{name}”.", isError: false);
        }
        catch (Exception ex) { SetProfileStatus(ex.Message, isError: true); }
    }

    private void SetProfileStatus(string text, bool isError)
    {
        Form.Status = text;
        Form.StatusIsError = isError;
        ProfileStatus.Text = text;
        ProfileStatus.Foreground = Controls.Resource<Brush>(
            isError ? "SystemFillColorCriticalBrush" : "TextFillColorSecondaryBrush");
    }

    /// A dialog rather than an inline row, because naming a profile is a
    /// question with two answers and Escape has to mean one of them.
    private async System.Threading.Tasks.Task<string?> AskForName(
        string title, string explanation, string initial, string commit)
    {
        var box = new TextBox { Text = initial, PlaceholderText = "Name" };
        var panel = new StackPanel { Spacing = 10 };
        panel.Children.Add(new TextBlock { Text = explanation, TextWrapping = TextWrapping.Wrap });
        panel.Children.Add(box);

        var dialog = new ContentDialog
        {
            Title = title,
            Content = panel,
            PrimaryButtonText = commit,
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = XamlRoot,
        };

        try
        {
            var result = await dialog.ShowAsync();
            if (result != ContentDialogResult.Primary) return null;
            var typed = box.Text.Trim();
            return typed.Length == 0 ? null : typed;
        }
        catch (Exception) { return null; }
    }

    private async System.Threading.Tasks.Task ShowError(string message)
    {
        try
        {
            await new ContentDialog
            {
                Title = "Cannot do that",
                Content = new TextBlock { Text = message, TextWrapping = TextWrapping.Wrap },
                CloseButtonText = "OK",
                XamlRoot = XamlRoot,
            }.ShowAsync();
        }
        catch (Exception) { /* another dialog is already up */ }
    }

    // MARK: - The drain

    private void DrainOnce()
    {
        var snapshot = Model.Runner.Drain();

        foreach (var (text, transient) in snapshot.Lines)
        {
            if (transient)
            {
                // A redraw REPLACES its predecessor rather than adding a row.
                TransientLine.Text = text;
            }
            else
            {
                /* A permanent line appends BELOW the live redraw rather than
                 * swallowing it, so the last progress reading a download
                 * printed stays in the log instead of vanishing the moment the
                 * next stage announces itself. */
                if (TransientLine.Text.Length > 0)
                {
                    _log.Add(TransientLine.Text);
                    TransientLine.Text = "";
                }
                _log.Add(text);
            }
        }

        /* Trimmed in blocks rather than one line at a time, so a busy run is not
         * doing an O(n) RemoveAt(0) per line -- and each removal is a collection
         * change the ListView has to react to. */
        if (_log.Count > MaxLogRows + 200)
        {
            var excess = _log.Count - MaxLogRows;
            for (var i = 0; i < excess; i++) _log.RemoveAt(0);
        }

        if (snapshot.Lines.Count > 0 && _log.Count > 0)
        {
            try { LogList.ScrollIntoView(_log[^1]); }
            catch (Exception) { /* the list is not realised yet */ }
        }

        if (snapshot.State is { } state) ApplyState(state);
    }

    private void ApplyState(RunnerState state)
    {
        var running = state.Current is not null;
        CancelButton.IsEnabled = running;
        PauseButton.Content = state.Paused ? "Resume queue" : "Pause queue";

        StageText.Text = StageLine(state);

        if (state.Progress.Percent >= 0)
        {
            Progress.IsIndeterminate = false;
            Progress.Value = Math.Clamp(state.Progress.Percent, 0, 100);
        }
        else
        {
            /* Indeterminate while something is running but has no percentage --
             * a merge, a comments fetch, a post-process pass. A bar frozen at
             * its last download percentage through a five-minute merge reads as
             * a stalled run, which is the thing clearing the percentage in the
             * parser exists to prevent. */
            Progress.IsIndeterminate = running;
            if (!running) Progress.Value = 0;
        }

        QueueHeader.Text = $"Queue ({state.Queue.Count})";
        /* The queue is sequential by design and that is correct -- but thirty
         * runs from a bulk re-fetch going one at a time look like a stuck app
         * next to a competitor running eight at once, unless something says
         * the wait is deliberate and where the real parallelism lives. Shown
         * only while something is waiting. */
        QueueNote.Text = $"{state.Queue.Count} waiting. Runs go one at a time on purpose: two "
            + "ytdl runs at once would race on the archive's shared manifests. To download "
            + "several videos of one playlist or channel at the same time, raise Workers "
            + "before adding it.";
        QueueNote.Visibility = state.Queue.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        QueueEmpty.Visibility = state.Queue.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        QueueList.Visibility = state.Queue.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        QueueList.ItemsSource = state.Queue.Select(BuildQueueRow).ToList();

        HistoryHeader.Text = $"History ({state.History.Count})";
        HistoryEmpty.Visibility = state.History.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        HistoryList.Visibility = state.History.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        ClearHistoryButton.Visibility = state.History.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        HistoryList.ItemsSource = state.History.Select(BuildHistoryRow).ToList();
    }

    private static string StageLine(RunnerState state)
    {
        if (state.Current is null) return "Idle";
        var p = state.Progress;
        var parts = new List<string> { p.Stage ?? "running" };
        if (p.VideoId is { } id) parts.Add(id);
        if (p.Speed is { } speed) parts.Add(speed);
        if (p.Eta is { } eta) parts.Add($"ETA {eta}");
        if (p.Total is { } total) parts.Add($"of {total}");
        return string.Join(" · ", parts);
    }

    private UIElement BuildQueueRow(RunRecord record)
    {
        var grid = new Grid { ColumnSpacing = 8 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var url = new TextBlock
        {
            Text = record.Opts.Url,
            TextTrimming = TextTrimming.CharacterEllipsis,
            TextWrapping = TextWrapping.NoWrap,
            VerticalAlignment = VerticalAlignment.Center,
            FontSize = 12,
        };
        Grid.SetColumn(url, 0);
        grid.Children.Add(url);

        var remove = new Button
        {
            // U+E738 is Segoe Fluent Icons' "Remove". Written as an escape: a raw
            // private-use character in a source file compiles fine and does not
            // survive an editor with a different font or a diff viewer.
            Content = new FontIcon { Glyph = "\uE738", FontSize = 12 },
            Padding = new Thickness(6, 2, 6, 2),
        };
        ToolTipService.SetToolTip(remove, "Remove from the queue");
        var id = record.Id;
        remove.Click += (_, _) => Model.Runner.RemoveQueued(id);
        Grid.SetColumn(remove, 1);
        grid.Children.Add(remove);

        return grid;
    }

    private UIElement BuildHistoryRow(RunRecord record)
    {
        var variant = record.State switch
        {
            "done" => PillVariant.Ok,
            "failed" => PillVariant.Error,
            _ => PillVariant.Warn,
        };

        var head = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        head.Children.Add(Controls.Pill(record.State, variant));
        head.Children.Add(new TextBlock
        {
            Text = record.Command,
            FontSize = 12,
            TextTrimming = TextTrimming.CharacterEllipsis,
            TextWrapping = TextWrapping.NoWrap,
            VerticalAlignment = VerticalAlignment.Center,
        });

        /* The four counts exist only in the pipeline's own session summary line,
         * which is why they are parsed out of it as the run goes. A run that was
         * cancelled or died early never printed one, and shows nothing rather
         * than four zeroes that would read as "it ran and found nothing". */
        var parts = new List<string>();
        var when = Format.When(record.Started);
        if (when.Length > 0) parts.Add(when);
        if (record.VideosTouched >= 0)
        {
            parts.Add($"{record.VideosTouched} touched · {record.ArchiveSkipped} skipped · " +
                      $"{record.Errors} errors · {record.Warnings} warnings");
        }
        if (record.LastLine.Length > 0) parts.Add(record.LastLine);

        var panel = new StackPanel { Spacing = 2 };
        panel.Children.Add(head);
        panel.Children.Add(new TextBlock
        {
            Text = string.Join(" · ", parts),
            FontSize = 11,
            MaxLines = 2,
            TextWrapping = TextWrapping.Wrap,
            TextTrimming = TextTrimming.CharacterEllipsis,
            Foreground = Controls.Resource<Brush>("TextFillColorSecondaryBrush"),
        });

        /* Re-enqueues the record's OWN options, which RunRecord already carries
         * whole. Reconstructing them by parsing record.Command back into flags
         * would be a second, worse copy of the argument builder -- and the one
         * place it would go wrong is a quoted URL with a space in it, which is
         * exactly the run someone wants to repeat.
         *
         * A run with no URL cannot be repeated: it came from a build before
         * options were recorded, or from a hand-edited file. */
        var again = new Button
        {
            Content = new FontIcon { Glyph = "\uE72C", FontSize = 14 },
            VerticalAlignment = VerticalAlignment.Top,
            IsEnabled = record.Opts.Url.Length > 0,
        };
        ToolTipService.SetToolTip(again, "Queue this run again with the same options");
        again.Click += (_, _) => Model.Runner.Enqueue(record.Opts.Clone());

        var row = new Grid();
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        Grid.SetColumn(panel, 0);
        Grid.SetColumn(again, 1);
        row.Children.Add(panel);
        row.Children.Add(again);
        return row;
    }
}
