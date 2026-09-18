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
    /* A SECOND pill rather than another entry in the priority chain below.
     * Watch state is a different axis from what the folder contains, and
     * letting "watched" displace "no media file" would hide the more
     * important of the two. */
    public required bool Watched { get; init; }
    public required string Directory { get; init; }
    public BitmapImage? Thumbnail { get; init; }

    public Visibility DurationVisibility =>
        Duration.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    public Visibility BadgeVisibility =>
        BadgeText.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    public Visibility WatchedVisibility =>
        Watched ? Visibility.Visible : Visibility.Collapsed;
    public Visibility PlaceholderVisibility =>
        Thumbnail is null ? Visibility.Visible : Visibility.Collapsed;

    /* <paramref name="watched"/> is passed in rather than read from AppModel
     * here, because this type is built on a hot path -- once per card per
     * rebuild -- and the caller already has the store's set in hand. */
    public static VideoCardModel From(ArchiveEntry e, bool watched)
    {
        var date = Format.UploadDate(e.UploadDate);

        /* Not an error. --mode metadata-only, comments-only and subs-only all
         * write a complete folder with no media, and so does an interrupted
         * run. Saying WHICH is the manifest's job, not a guess from here. */
        /* One pill, so this is a priority order rather than a list, and the
         * order is by how much the fact changes what the folder IS. A missing
         * media file first: it is the difference between a video and a
         * metadata stub. Then a layout this reader cannot fully understand.
         * Then a refresh, which is worth saying -- it is the one case where the
         * sidecars are newer than the media, so the comments on this video were
         * fetched after it was archived -- but never at the cost of hiding
         * either of the two above it. */
        var badge = "";
        if (e.MediaIndex < 0) badge = e.DownloadMode ?? "no media file";
        else if (e.LayoutTooNew) badge = "newer archive layout";
        else if (e.RefreshCount == 1) badge = "refreshed";
        else if (e.RefreshCount > 1) badge = $"refreshed ×{e.RefreshCount}";

        return new VideoCardModel
        {
            Key = e.Key,
            Title = e.Title,
            Subtitle = date.Length == 0 ? e.Uploader : $"{e.Uploader} · {date}",
            Duration = Format.Duration(e.Duration),
            BadgeText = badge,
            Watched = watched,
            Directory = e.Dir,
            Thumbnail = ThumbnailCache.Load(e.ThumbnailPath),
        };
    }
}

public sealed partial class LibraryPage : Page
{
    private readonly ObservableCollection<VideoCardModel> _items = new();
    private AppModel Model => AppModel.Current;

    /* Set while the scope box is being populated from code, so its
     * SelectionChanged does not re-run a search against a control that is
     * being rebuilt. */
    private bool _populating;

    public LibraryPage()
    {
        InitializeComponent();
        Grid_.ItemsSource = _items;

        _populating = true;
        foreach (var scope in SearchScopes.All)
        {
            SearchScopeBox.Items.Add(SearchScopes.Label(scope));
        }
        SearchScopeBox.SelectedIndex = Array.IndexOf(SearchScopes.All, Model.SearchScope);
        _populating = false;
    }

    // ---------------------------------------------------------------- //
    // Collection-wide search                                           //
    // ---------------------------------------------------------------- //

    private void OnSearchScopeChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_populating) return;
        var i = SearchScopeBox.SelectedIndex;
        if (i < 0 || i >= SearchScopes.All.Length) return;

        Model.SearchScope = SearchScopes.All[i];
        Model.UpdateSearch();
        RefreshBanner();
        RefreshGrid();
    }

    private void OnBuildIndex(object sender, RoutedEventArgs e)
    {
        Model.BuildSearchIndex();
        RefreshBanner();
    }

    private void RefreshBanner()
    {
        if (Model.Indexing)
        {
            IndexBanner.Title = "";
            IndexBanner.Message = Model.IndexProgress;
            IndexBuildButton.IsEnabled = false;
            IndexBanner.IsOpen = true;
            return;
        }

        IndexBuildButton.IsEnabled = true;

        if (!Model.SearchNeedsIndex)
        {
            IndexBanner.IsOpen = false;
            return;
        }

        var stale = Model.SearchIndexOutdated;
        var plural = stale == 1 ? "" : "s";
        IndexBanner.Message = Model.SearchIndex.Count == 0
            ? $"Searching comments and captions needs an index. {stale} video{plural} to read."
            : $"{stale} video{plural} changed since the index was built.";
        IndexBanner.IsOpen = true;
    }

    // ---------------------------------------------------------------- //
    // Sort and facets                                                  //
    // ---------------------------------------------------------------- //

    /* Built in code rather than declared in XAML, and rebuilt on every scan,
     * because the channel list is the ARCHIVE's: it changes when the archive
     * does. A declared flyout with an ItemsSource binding would need an
     * observable layer over a list that is replaced wholesale a few times a
     * session, to say what one rebuild says directly. */
    private void RebuildFilterFlyout()
    {
        var f = Model.Filter;
        var flyout = new MenuFlyout();

        var sortMenu = new MenuFlyoutSubItem { Text = "Sort by" };
        foreach (var key in SortKeys.All)
        {
            var captured = key;
            var item = new ToggleMenuFlyoutItem
            {
                Text = SortKeys.Label(key),
                IsChecked = f.Sort == key,
            };
            item.Click += (_, _) =>
            {
                f.Sort = captured;
                Model.PersistSort();
                RebuildFilterFlyout();
                RefreshGrid();
            };
            sortMenu.Items.Add(item);
        }
        flyout.Items.Add(sortMenu);

        var order = new ToggleMenuFlyoutItem
        {
            Text = "Newest first",
            IsChecked = f.Descending,
        };
        order.Click += (_, _) =>
        {
            f.Descending = !f.Descending;
            Model.PersistSort();
            RebuildFilterFlyout();
            RefreshGrid();
        };
        flyout.Items.Add(order);
        flyout.Items.Add(new MenuFlyoutSeparator());

        /* One channel is not a choice. Offering a facet whose only effect is
         * to hide everything or nothing is worse than not offering it. */
        if (Model.Index.Channels.Count > 1)
        {
            var channels = new MenuFlyoutSubItem { Text = "Channels" };
            foreach (var channel in Model.Index.Channels)
            {
                var captured = channel;
                var item = new ToggleMenuFlyoutItem
                {
                    Text = channel,
                    IsChecked = f.Channels.Contains(channel),
                };
                item.Click += (_, _) =>
                {
                    f.SetChannel(captured, !f.Channels.Contains(captured));
                    RebuildFilterFlyout();
                    RefreshGrid();
                };
                channels.Items.Add(item);
            }
            flyout.Items.Add(channels);
        }

        /* No playlists is not a choice either, exactly like one channel. The
         * submenu appears the moment there is one to pick. */
        if (Model.UserData.Playlists.Count > 0)
        {
            var lists = new MenuFlyoutSubItem { Text = "Playlist" };

            var all = new ToggleMenuFlyoutItem
            {
                Text = "All videos",
                IsChecked = Model.PlaylistId is null,
            };
            all.Click += (_, _) =>
            {
                Model.PlaylistId = null;
                RebuildFilterFlyout();
                RefreshGrid();
            };
            lists.Items.Add(all);

            foreach (var pl in Model.UserData.Playlists)
            {
                var capturedId = pl.Id;
                var item = new ToggleMenuFlyoutItem
                {
                    Text = pl.Name,
                    IsChecked = string.Equals(Model.PlaylistId, capturedId,
                                              StringComparison.Ordinal),
                };
                item.Click += (_, _) =>
                {
                    /* Set rather than toggled: a playlist restriction is a
                     * choice of ONE, and a second ticked item would be an
                     * intersection of two playlists that the filter cannot
                     * express and nobody asked for. */
                    Model.PlaylistId = capturedId;
                    RebuildFilterFlyout();
                    RefreshGrid();
                };
                lists.Items.Add(item);
            }
            flyout.Items.Add(lists);
        }

        foreach (var flag in Facets.All)
        {
            var captured = flag;
            var item = new ToggleMenuFlyoutItem
            {
                Text = Facets.Label(flag),
                IsChecked = f.Flags.HasFlag(flag),
                /* The verification facet can only see videos somebody has
                 * actually verified. With none verified it would silently
                 * match nothing, which reads as a broken control rather than
                 * as an empty answer -- so it is disabled and the reason is
                 * spelled out in the item below. */
                IsEnabled = flag != FacetFlags.VerifyFailed || Model.VerifyCache.KnownCount > 0,
            };
            item.Click += (_, _) =>
            {
                if (f.Flags.HasFlag(captured)) f.Flags &= ~captured;
                else f.Flags |= captured;
                RebuildFilterFlyout();
                RefreshGrid();
            };
            flyout.Items.Add(item);
        }

        /* The verification facet has to say what it is a subset of. A facet
         * that silently means "of the four I have checked" while looking like
         * it means "of your whole archive" is a facet that will be believed. */
        var known = Model.VerifyCache.KnownCount;
        flyout.Items.Add(new MenuFlyoutItem
        {
            Text = known == 0
                ? "Nothing verified yet — use Verify on a video's page"
                : $"Of the {known} video{(known == 1 ? "" : "s")} verified so far",
            IsEnabled = false,
        });

        /* The mirror image, and honest for the opposite reason: the Unwatched
         * facet DOES see the whole archive -- a video nobody has marked is
         * unwatched, which is the correct answer rather than an unknown one.
         * What it has to say is how much is already marked, because on a fresh
         * install "unwatched" means "all of them" and a facet that appears to
         * do nothing reads as broken. */
        var watched = Model.UserData.WatchedCount;
        flyout.Items.Add(new MenuFlyoutItem
        {
            Text = Model.UserData.IsReadOnly
                ? "userdata.json could not be read — watch state is read-only"
                : watched == 0
                    ? "Nothing is marked watched yet, so this shows everything"
                    : $"{watched} video{(watched == 1 ? " is" : "s are")} marked watched",
            IsEnabled = false,
        });

        flyout.Items.Add(new MenuFlyoutSeparator());

        var clear = new MenuFlyoutItem { Text = "Clear filters", IsEnabled = Model.IsNarrowing };
        clear.Click += OnClearFilters;
        flyout.Items.Add(clear);

        FilterButton.Flyout = flyout;

        var n = f.FacetCount;
        FilterLabel.Text = n > 0 ? $"Filter ({n})" : "Filter";
    }

    private void OnClearFilters(object sender, RoutedEventArgs e)
    {
        Model.ClearFilters();
        /* The search box lives on the window, not on this page, so clearing
         * the model's SearchText has to be reflected there too or the box
         * would keep showing a term that is no longer applied. */
        App.Window?.SyncSearchBox();
        RebuildFilterFlyout();
        RefreshGrid();
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        Model.Changed += OnModelChanged;
        RebuildFilterFlyout();
        RefreshBanner();
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

        /* The banner is refreshed even mid-scan, because the indexing progress
         * it carries is raised through this same event and a scan and an index
         * build are not mutually exclusive states of this page. */
        RefreshBanner();
        if (Model.Scanning) return;

        /* The channel facet is the ARCHIVE's channel list, so the flyout is
         * rebuilt from each new index. A channel that has gone away also goes
         * out of the offered set by construction: the rebuild only ever
         * creates items for channels the index still has. */
        RebuildFilterFlyout();
        RefreshGrid();
    }

    /// Rebuild the grid from the current filter. Called by the window when the
    /// search box changes, and after every scan.
    public void RefreshGrid()
    {
        var entries = Model.FilteredEntries();

        /* The store's live set, read once per rebuild rather than once per
         * card: this is the hot path the whole card model exists to keep
         * cheap. */
        var watched = Model.UserData.WatchedKeys;

        _items.Clear();
        foreach (var e in entries)
        {
            _items.Add(VideoCardModel.From(e, watched.Contains(e.Key)));
        }

        var empty = entries.Count == 0;
        EmptyState.Visibility = empty ? Visibility.Visible : Visibility.Collapsed;
        Grid_.Visibility = empty ? Visibility.Collapsed : Visibility.Visible;

        /* The empty state has to say which of two very different things
         * happened. "There is no archive here" and "your filters exclude
         * everything" look identical as an empty grid, and only one of them is
         * the user's own doing -- showing the wrong message sends someone
         * looking for a lost archive when all they did was tick a facet. So it
         * keys off whether the INDEX is empty, not off whether the search box
         * is. */
        var archiveHasVideos = Model.Index.Entries.Count > 0;
        if (empty && archiveHasVideos)
        {
            EmptyIcon.Glyph = "";
            EmptyTitle.Text = "No video matches";
            EmptyDetail.Text =
                "The archive is not empty — the current search and filters exclude every " +
                "video in it.";
            EmptyClear.Visibility = Visibility.Visible;
        }
        else
        {
            EmptyIcon.Glyph = "";
            EmptyTitle.Text = "Nothing to show";
            EmptyDetail.Text =
                "Point the app at the same path you would pass to `ytdl --path` on the Health " +
                "pane, then press Rescan.";
            EmptyClear.Visibility = Visibility.Collapsed;
        }

        var n = Model.Filter.FacetCount;
        FilterLabel.Text = n > 0 ? $"Filter ({n})" : "Filter";
    }

    private void OnRescan(object sender, RoutedEventArgs e) => Model.StartScan();

    private void OnItemClick(object sender, ItemClickEventArgs e)
    {
        if (e.ClickedItem is not VideoCardModel card) return;
        App.Window?.NavigateToDetail(card.Key);
    }
}
