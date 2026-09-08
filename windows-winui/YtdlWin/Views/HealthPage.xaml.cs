/* The Health pane's contents.
 *
 * The cheap half -- installed files, yt-dlp.conf, the archive counters -- is
 * straight file reads and happens inline. The dependency probe is seven process
 * spawns with an eight-second ceiling each and goes on a background thread, for
 * the same reason the archive scan does: the pane must draw before it knows the
 * answers, or it looks broken rather than busy.
 */

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using YtdlWin.Core;

namespace YtdlWin.Views;

public sealed partial class HealthPage : Page
{
    private List<Dependency> _dependencies = new();
    private List<Health.InstalledFile> _files = new();
    private Health.ConfigInfo? _config;
    private Health.ArchiveStats? _stats;
    private CliVersion? _pin;
    private bool _probing;

    private AppModel Model => AppModel.Current;

    public HealthPage() => InitializeComponent();

    protected override async void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        Model.Changed += OnModelChanged;
        await RefreshAsync(force: false);
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        base.OnNavigatedFrom(e);
        Model.Changed -= OnModelChanged;
    }

    /* A finished scan changes what half of this pane reports -- the data root,
     * global_manifest.json, archive.txt and the snapshot count all come from
     * files beside the archive that was just re-read. Without this they stay as
     * they were when the pane was last opened. */
    private bool _wasScanning;
    private async void OnModelChanged()
    {
        var scanning = Model.Scanning;
        var justFinished = _wasScanning && !scanning;
        _wasScanning = scanning;
        if (justFinished) await RefreshAsync(force: false);
    }

    private async Task RefreshAsync(bool force)
    {
        _files = Health.InstalledFiles();
        _config = Health.ConfigDetails();
        _stats = Health.Stats(Model.Settings.ResolvedDataRoot, Model.Index.VideoCount,
                              Model.Index.ChannelCount, Model.Index.TotalBytes);
        _pin ??= CliVersion.Load(Paths.Join(AppContext.BaseDirectory, "CLI_VERSION"));
        ReloadLog();
        Render();

        if (_probing) return;
        _probing = true;
        Render();
        try
        {
            _dependencies = await Task.Run(() => Health.Dependencies(force));
        }
        finally
        {
            _probing = false;
            Render();
        }
    }

    private void Render()
    {
        Sections.Children.Clear();
        Sections.Children.Add(BuildDependencies());
        Sections.Children.Add(BuildInstalledFiles());
        Sections.Children.Add(BuildConfig());
        Sections.Children.Add(BuildArchive());
    }

    // MARK: - Dependencies

    private UIElement BuildDependencies()
    {
        var reprobe = new Button { Content = "Re-probe", IsEnabled = !_probing };
        ToolTipService.SetToolTip(reprobe,
            "Skips the five-minute cache and runs every --version again. What you want right " +
            "after installing something that was missing.");
        reprobe.Click += async (_, _) => await RefreshAsync(force: true);

        var trailing = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        if (_probing)
        {
            trailing.Children.Add(new ProgressRing
            {
                Width = 16, Height = 16, IsActive = true,
                VerticalAlignment = VerticalAlignment.Center,
            });
        }
        trailing.Children.Add(reprobe);

        var rows = _dependencies.Count == 0
            ? new UIElement[] { Secondary("Probing…") }
            : _dependencies.Select(DependencyRow).ToArray();

        return Controls.SectionBox("Dependencies", trailing, rows);
    }

    private static UIElement DependencyRow(Dependency d)
    {
        var variant = d.Found
            ? PillVariant.Ok
            : (d.Importance == "required" ? PillVariant.Error : PillVariant.Warn);

        var grid = new Grid { ColumnSpacing = 8, Margin = new Thickness(0, 3, 0, 3) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var pill = Controls.Pill(d.Found ? "found" : "missing", variant);
        pill.VerticalAlignment = VerticalAlignment.Top;
        pill.Margin = new Thickness(0, 2, 0, 0);
        Grid.SetColumn(pill, 0);
        grid.Children.Add(pill);

        /* The subtitle is where it is, when it is there, and why it matters when
         * it is not. The note only earns its space when something is wrong: on a
         * healthy machine seven paragraphs of explanation is just noise to
         * scroll past. */
        var text = new StackPanel { Spacing = 2 };
        text.Children.Add(new TextBlock { Text = d.Name });
        text.Children.Add(new TextBlock
        {
            Text = d.Found ? (d.Path ?? "") : d.Note,
            FontSize = 12,
            TextWrapping = TextWrapping.Wrap,
            IsTextSelectionEnabled = true,
            Foreground = Controls.Resource<Brush>("TextFillColorSecondaryBrush"),
        });
        Grid.SetColumn(text, 1);
        grid.Children.Add(text);

        /* Found with no version means the probe hit its eight-second timeout,
         * which is a different thing from missing and is worth saying rather
         * than showing a blank. */
        var version = d.Version ?? (d.Found ? "version probe timed out" : null);
        if (version is not null)
        {
            var block = new TextBlock
            {
                Text = version,
                FontSize = 12,
                FontFamily = new FontFamily("Consolas, Cascadia Mono, Courier New"),
                IsTextSelectionEnabled = true,
                VerticalAlignment = VerticalAlignment.Top,
                Foreground = Controls.Resource<Brush>("TextFillColorSecondaryBrush"),
            };
            Grid.SetColumn(block, 2);
            grid.Children.Add(block);
        }

        var importance = Controls.Pill(d.Importance);
        importance.VerticalAlignment = VerticalAlignment.Top;
        importance.Margin = new Thickness(0, 2, 0, 0);
        Grid.SetColumn(importance, 3);
        grid.Children.Add(importance);

        return grid;
    }

    // MARK: - Installed files

    private UIElement BuildInstalledFiles()
    {
        /* What is INSTALLED, never what is in a checkout. Editing a file in a
         * clone has no effect on a live install until it is copied over, which
         * is exactly the confusion this list settles. */
        var rows = _files.Select(f =>
        {
            var grid = new Grid { ColumnSpacing = 8, Margin = new Thickness(0, 3, 0, 3) };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

            var pill = Controls.Pill(f.Present ? "installed" : "missing",
                                     f.Present ? PillVariant.Ok : PillVariant.Error);
            pill.VerticalAlignment = VerticalAlignment.Top;
            pill.Margin = new Thickness(0, 2, 0, 0);
            Grid.SetColumn(pill, 0);
            grid.Children.Add(pill);

            var text = new StackPanel { Spacing = 2 };
            text.Children.Add(new TextBlock { Text = f.Name });
            text.Children.Add(new TextBlock
            {
                Text = f.Present
                    ? $"{Format.Bytes(f.Size)} · {Format.Timestamp(f.Modified)} · {f.Path}"
                    : f.Path,
                FontSize = 12,
                TextWrapping = TextWrapping.Wrap,
                IsTextSelectionEnabled = true,
                Foreground = Controls.Resource<Brush>("TextFillColorSecondaryBrush"),
            });
            Grid.SetColumn(text, 1);
            grid.Children.Add(text);
            return (UIElement)grid;
        }).ToArray();

        return Controls.SectionBox("Installed pipeline files", null, rows);
    }

    // MARK: - Config

    private UIElement BuildConfig()
    {
        var rows = new List<UIElement>
        {
            Controls.KeyValueRow("CONFIG_VERSION", _config?.ConfigVersion ?? "not found", monospaced: true),
            Controls.KeyValueRow("Options set", (_config?.OptionCount ?? 0).ToString()),
            Controls.KeyValueRow("Path", _config?.Path, monospaced: true),
        };

        if (_config is { Present: false })
        {
            rows.Add(Warning(PillVariant.Error,
                "yt-dlp.conf is not installed. Downloads will run with yt-dlp's own defaults " +
                "rather than this pipeline's."));
        }

        return Controls.SectionBox("yt-dlp.conf", null, rows.ToArray());
    }

    // MARK: - Archive

    private UIElement BuildArchive()
    {
        var choose = new Button { Content = "Choose folder…" };
        ToolTipService.SetToolTip(choose,
            "Point the Library at a different archive — a data root, the Youtube Videos " +
            "folder, or Complete Archive itself.");
        choose.Click += async (_, _) =>
        {
            var picked = await DownloadsPage.PickFolderAsync(
                "Choose the archive folder (a data root, or Complete Archive itself)");
            if (picked is null) return;
            Model.SetArchiveRoot(picked);
            await RefreshAsync(force: false);
        };

        var rows = new List<UIElement>
        {
            Controls.KeyValueRow("Indexed",
                $"{Model.Index.VideoCount} videos · {Model.Index.ChannelCount} channels · " +
                Format.Bytes(Model.Index.TotalBytes)),
            Controls.KeyValueRow("Archive root",
                Model.Index.Root.Length == 0 ? "none found" : Model.Index.Root, monospaced: true),
            Controls.KeyValueRow("Data root", _stats?.DataRoot, monospaced: true),
            Controls.KeyValueRow("global_manifest.json",
                (_stats?.GlobalManifestEntries ?? -1) >= 0
                    ? _stats!.GlobalManifestEntries.ToString() : "not readable"),
            Controls.KeyValueRow("archive.txt ids",
                (_stats?.ArchiveTxtIds ?? -1) >= 0
                    ? _stats!.ArchiveTxtIds.ToString() : "not readable"),
            Controls.KeyValueRow("Archive History snapshots",
                (_stats?.HistorySnapshots ?? 0).ToString()),

            /* The pin this app was built against, shown here because the number
             * that decides whether a video renders is otherwise invisible. The
             * conformance suite asserts these two agree; this is where a human
             * can see the same thing. */
            Controls.KeyValueRow("Archive layout read",
                $"{ArchiveLayout.Supported}" +
                (_pin?.RequiresArchiveLayout is { } pinned && pinned != ArchiveLayout.Supported
                    ? $"  (CLI_VERSION pins {pinned} — these disagree)"
                    : "")),
            Controls.KeyValueRow("Pipeline pin",
                _pin is null ? "CLI_VERSION not found beside the binary"
                             : $"{_pin.CliRepo} @ {_pin.CliRef}", monospaced: true),
        };

        /* The index counts what this app walked; global_manifest.json is what
         * the pipeline wrote. Them disagreeing is the single most useful signal
         * on this pane -- it means one of them is looking at a different
         * folder. */
        if (_stats is { GlobalManifestEntries: >= 0 } s && s.GlobalManifestEntries != Model.Index.VideoCount)
        {
            rows.Add(Warning(PillVariant.Warn,
                "The indexed count and global_manifest.json disagree. Usually that means the " +
                "Library is pointed at a different folder from the one downloads are going to."));
        }

        return Controls.SectionBox("Archive", choose, rows.ToArray());
    }

    // MARK: - Logs

    private void OnLogChoiceChanged(object sender, SelectionChangedEventArgs e) => ReloadLog();

    private void ReloadLog()
    {
        var name = (LogChoice.SelectedItem as ComboBoxItem)?.Tag as string ?? "download.log";
        var path = Paths.Join(Paths.Join(Model.Settings.ResolvedDataRoot, @"Archive Logs\Logs"), name);
        var tail = Health.LogTail(path, 300);
        LogText.Text = tail.Length == 0 ? $"Nothing to show. {path} does not exist yet." : tail;
    }

    // MARK: - Small pieces

    private static UIElement Secondary(string text) => new TextBlock
    {
        Text = text,
        TextWrapping = TextWrapping.Wrap,
        Foreground = Controls.Resource<Brush>("TextFillColorSecondaryBrush"),
    };

    private static UIElement Warning(PillVariant variant, string text)
    {
        var panel = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            Margin = new Thickness(0, 6, 0, 0),
        };
        var pill = Controls.Pill(variant == PillVariant.Error ? "problem" : "check", variant);
        pill.VerticalAlignment = VerticalAlignment.Top;
        pill.Margin = new Thickness(0, 2, 0, 0);
        panel.Children.Add(pill);
        panel.Children.Add(new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap });
        return panel;
    }
}
