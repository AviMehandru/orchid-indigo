/* The library grid.
 *
 * A GridView rather than an eager stack of cards: it realises only the items on
 * screen, so an archive with several thousand videos costs the same to open as
 * one with twenty. That is the shape of archive this tool exists to produce.
 *
 * The grid hands over the opaque KEY, not the entry, so a rescan that finishes
 * between the click and the navigation cannot leave the detail page showing a
 * video that is no longer in the index -- it looks the key up again against
 * whatever index is current.
 */

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Navigation;
using YtdlWin.Core;

namespace YtdlWin.Views;

/* A flattened row for the template to bind to, rather than the ArchiveEntry
 * itself.
 *
 * ArchiveEntry.MediaIndex and ThumbnailIndex walk the whole file list every time
 * they are read, and x:Bind reads each bound property at least once per
 * realisation -- so binding the entry directly would re-walk every file of every
 * card on every scroll. Computing it once here is the difference between a grid
 * that scrolls and one that stutters on exactly the large archives this tool
 * produces. */
public sealed class VideoCardModel
{
    public required string Key { get; init; }
    public required string Title { get; init; }
    public required string Subtitle { get; init; }
    public required string Duration { get; init; }
    public required string BadgeText { get; init; }
    public required string Directory { get; init; }
    public BitmapImage? Thumbnail { get; init; }

    public Visibility DurationVisibility =>
        Duration.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    public Visibility BadgeVisibility =>
        BadgeText.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    public Visibility PlaceholderVisibility =>
        Thumbnail is null ? Visibility.Visible : Visibility.Collapsed;

    public static VideoCardModel From(ArchiveEntry e)
    {
        var date = Format.UploadDate(e.UploadDate);

        /* Not an error. --mode metadata-only, comments-only and subs-only all
         * write a complete folder with no media, and so does an interrupted
         * run. Saying WHICH is the manifest's job, not a guess from here. */
        var badge = "";
        if (e.MediaIndex < 0) badge = e.DownloadMode ?? "no media file";
        else if (e.LayoutTooNew) badge = "newer archive layout";

        return new VideoCardModel
        {
            Key = e.Key,
            Title = e.Title,
            Subtitle = date.Length == 0 ? e.Uploader : $"{e.Uploader} · {date}",
            Duration = Format.Duration(e.Duration),
            BadgeText = badge,
            Directory = e.Dir,
            Thumbnail = ThumbnailCache.Load(e.ThumbnailPath),
        };
    }
}

public sealed partial class LibraryPage : Page
{
    private readonly ObservableCollection<VideoCardModel> _items = new();
    private AppModel Model => AppModel.Current;

    public LibraryPage()
    {
        InitializeComponent();
        Grid_.ItemsSource = _items;
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        Model.Changed += OnModelChanged;
        RefreshGrid();
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        base.OnNavigatedFrom(e);
        /* Unsubscribed on the way out. A page a Frame has navigated away from is
         * not collected while the model still holds a delegate to it, and every
         * trip to Downloads and back would add another live page listening to
         * the same event -- so a rescan would eventually be rebuilding a dozen
         * dead grids. */
        Model.Changed -= OnModelChanged;
    }

    private void OnModelChanged()
    {
        RescanButton.IsEnabled = !Model.Scanning;
        if (!Model.Scanning) RefreshGrid();
    }

    /// Rebuild the grid from the current filter. Called by the window when the
    /// search box changes, and after every scan.
    public void RefreshGrid()
    {
        var entries = Model.FilteredEntries();

        _items.Clear();
        foreach (var e in entries) _items.Add(VideoCardModel.From(e));

        var empty = entries.Count == 0;
        EmptyState.Visibility = empty ? Visibility.Visible : Visibility.Collapsed;
        Grid_.Visibility = empty ? Visibility.Collapsed : Visibility.Visible;

        EmptyDetail.Text = Model.SearchText.Trim().Length == 0
            ? "Point the app at the same path you would pass to `ytdl --path` on the Health " +
              "pane, then press Rescan."
            : $"No video matches “{Model.SearchText}”.";
    }

    private void OnRescan(object sender, RoutedEventArgs e) => Model.StartScan();

    private void OnItemClick(object sender, ItemClickEventArgs e)
    {
        if (e.ClickedItem is not VideoCardModel card) return;
        App.Window?.NavigateToDetail(card.Key);
    }
}
