/* The window: the three pages, the status line, and stopping the download when
 * it closes.
 *
 * Closing the window must take the running download's process tree with it. A
 * run is a TREE -- ytdl.ps1, a child pwsh, yt-dlp, ffmpeg -- and on the other
 * two platforms an app that dies without doing this leaves the download running
 * with nothing reading its output. Here the job object's KILL_ON_JOB_CLOSE
 * makes that impossible even for a hard kill, so Runner.Stop() on Closed is
 * about doing it in the right ORDER -- letting the worker finish its last
 * history write before the process goes -- rather than about doing it at all.
 */

using System;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;
using YtdlWin.Core;
using YtdlWin.Views;

namespace YtdlWin;

public sealed partial class MainWindow : Window
{
    private AppModel _model = null!;
    private bool _alerting;

    public MainWindow()
    {
        InitializeComponent();

        _model = AppModel.Initialise(DispatcherQueue);
        _model.Changed += OnModelChanged;
        _model.Alert += OnAlert;

        /* Clamped to the display's work area, not a fixed 1180x880.
         *
         * A hardcoded height taller than the screen pushes the bottom of the
         * window off the desktop, and the bottom of THIS window is the status
         * line -- so on any display shorter than 880 the status bar simply is
         * not there, with nothing on screen to say why. Laptops and virtual
         * machines are routinely 768 tall, so that is the common case rather
         * than the exotic one. The window is then centred, because a clamped
         * window left at the default position can still hang off an edge. */
        var work = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary).WorkArea;
        var width = Math.Min(1180, Math.Max(640, work.Width - 80));
        var height = Math.Min(880, Math.Max(480, work.Height - 80));
        AppWindow.Resize(new Windows.Graphics.SizeInt32(width, height));
        AppWindow.Move(new Windows.Graphics.PointInt32(
            work.X + (work.Width - width) / 2,
            work.Y + (work.Height - height) / 2));

        Nav.SelectedItem = Nav.MenuItems[0];
        ContentFrame.Navigate(typeof(LibraryPage));

        /* The worker starts only once the window it will publish into exists. A
         * restored queue would otherwise begin producing events with nothing to
         * receive them. */
        _model.Runner.Start();

        /* The scan is queued rather than called here, and that is a real fix
         * rather than tidiness. StartScan raises Alert synchronously when there
         * is no archive to find -- which is the FIRST RUN on a machine where
         * the pipeline is not installed yet, so it is a common path, not an edge
         * one. A ContentDialog shown from inside this constructor has no
         * XamlRoot yet (Content is not connected until the window is activated)
         * and throws, so the one message a new user most needs would be the one
         * message they never see. Enqueueing puts it after activation. */
        DispatcherQueue.TryEnqueue(() => _model.StartScan());

        Closed += OnClosed;

        var rescan = new KeyboardAccelerator { Key = VirtualKey.R, Modifiers = VirtualKeyModifiers.Control };
        rescan.Invoked += (_, e) => { e.Handled = true; _model.StartScan(); };

        /* Alt+Left is wired by hand. A WinUI desktop Frame does not bring the
         * shell's back gestures with it, which the XAML in this file used to
         * claim it did. */
        var back = new KeyboardAccelerator { Key = VirtualKey.Left, Modifiers = VirtualKeyModifiers.Menu };
        back.Invoked += (_, e) =>
        {
            e.Handled = true;
            if (ContentFrame.CanGoBack) ContentFrame.GoBack();
            UpdateBackButton();
        };

        if (Content is UIElement root)
        {
            root.KeyboardAccelerators.Add(rescan);
            root.KeyboardAccelerators.Add(back);
        }
    }

    private void OnClosed(object sender, WindowEventArgs args)
    {
        _model.Changed -= OnModelChanged;
        _model.Alert -= OnAlert;
        _model.Runner.Stop();
    }

    private void OnModelChanged()
    {
        StatusText.Text = _model.Status;
        ScanRing.IsActive = _model.Scanning;
        ScanRing.Visibility = _model.Scanning ? Visibility.Visible : Visibility.Collapsed;
    }

    /* The one thing that interrupts. A ContentDialog cannot be shown while
     * another is up -- it throws rather than queueing -- and a failing scan can
     * fire this more than once, so the flag is not politeness but a real
     * guard. */
    private async void OnAlert(string message)
    {
        if (_alerting) return;
        _alerting = true;
        try
        {
            var dialog = new ContentDialog
            {
                Title = "No archive found",
                Content = new TextBlock { Text = message, TextWrapping = TextWrapping.Wrap },
                CloseButtonText = "OK",
                /* Required in WinUI 3, and the source of the most common
                 * "ContentDialog throws for no reason" bug: without an XamlRoot
                 * the dialog has no window to attach to. */
                XamlRoot = Content.XamlRoot,
            };
            await dialog.ShowAsync();
        }
        catch (Exception) { /* the window went away mid-dialog */ }
        finally { _alerting = false; }
    }

    private void OnSectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is not NavigationViewItem item) return;

        /* The parentheses are load-bearing. Without them this parses as
         * `item.Tag as (string switch { ... })`, because `as` binds looser than
         * a switch expression, which is what CS8848 was warning about. */
        var page = (item.Tag as string) switch
        {
            "downloads" => typeof(DownloadsPage),
            "health" => typeof(HealthPage),
            _ => typeof(LibraryPage),
        };

        // Search only means anything on the Library page.
        Search.Visibility = page == typeof(LibraryPage) ? Visibility.Visible : Visibility.Collapsed;

        if (ContentFrame.CurrentSourcePageType != page)
        {
            ContentFrame.Navigate(page);
            ContentFrame.BackStack.Clear();
            UpdateBackButton();
        }
    }

    private void OnBackRequested(NavigationView sender, NavigationViewBackRequestedEventArgs args)
    {
        if (ContentFrame.CanGoBack) ContentFrame.GoBack();
        UpdateBackButton();
    }

    /* BOTH properties, and that is the whole bug this fixes.
     *
     * NavigationView has IsBackButtonVisible and IsBackEnabled, and
     * IsBackEnabled defaults to FALSE. Setting only the first draws the button
     * and leaves it inert: the detail page opens and there is then no way out
     * of it, which is exactly how it behaved. Nothing warns about this -- a
     * greyed-out button looks like a considered state rather than a property
     * nobody set. */
    public void UpdateBackButton()
    {
        var canGoBack = ContentFrame.CanGoBack;
        Nav.IsBackButtonVisible = canGoBack
            ? NavigationViewBackButtonVisible.Visible
            : NavigationViewBackButtonVisible.Collapsed;
        Nav.IsBackEnabled = canGoBack;
    }

    /// Navigate the shared frame, which is how the Library opens a video.
    public void NavigateToDetail(string key)
    {
        ContentFrame.Navigate(typeof(DetailPage), key);
        UpdateBackButton();
    }

    private void OnSearchChanged(AutoSuggestBox sender, AutoSuggestBoxTextChangedEventArgs args)
    {
        if (args.Reason != AutoSuggestionBoxTextChangeReason.UserInput) return;
        _model.SearchText = sender.Text;
        (ContentFrame.Content as LibraryPage)?.RefreshGrid();
        _model.UpdateCounts();
    }
}
