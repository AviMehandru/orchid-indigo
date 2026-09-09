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
using Microsoft.UI.Xaml.Navigation;
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
        ExtraArgsBox.Text = Form.ExtraArgsText;
        WorkersBox.Value = Math.Max(1, Form.Opts.Workers);

        Select(ModeBox, Form.Opts.Mode);
        Select(QualityBox, Form.Opts.Quality);
        Select(CodecBox, Form.Opts.Codec);
        Select(AudioCodecBox, Form.Opts.AudioCodec);
        Select(ContainerBox, Form.Opts.Container);

        foreach (var row in PassesPanel.Children.OfType<OptionRow>())
        {
            if (row.Content is ToggleSwitch t && t.Tag is string field)
                t.IsOn = ReadPass(field);
        }

        ProfileStatus.Text = Form.Status;
        ProfileStatus.Foreground = Controls.Resource<Brush>(
            Form.StatusIsError ? "SystemFillColorCriticalBrush" : "TextFillColorSecondaryBrush");

        _suppressChanges = false;
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
        UpdatePreview();
    }

    /* With no URL typed the preview would read ytdl "" -- which looks like a bug
     * rather than an empty field, and is what it showed right after a queue add
     * cleared the box. A placeholder keeps the rest of the command visible, so
     * the options you have set are still readable while you paste a URL. */
    private void UpdatePreview()
    {
        var shown = Form.Opts.Clone();
        if (string.IsNullOrWhiteSpace(shown.Url)) shown.Url = "<URL>";
        PreviewText.Text = shown.CommandPreview();
    }

    // MARK: - Actions

    private async void OnAddToQueue(object sender, RoutedEventArgs e)
    {
        try
        {
            Model.Runner.Enqueue(Form.Opts);
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
            Model.Profiles.Save(name, Form.Opts);
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

    private static UIElement BuildHistoryRow(RunRecord record)
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
        return panel;
    }
}
