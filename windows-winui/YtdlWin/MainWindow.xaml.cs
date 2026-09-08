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

        /* A sensible minimum and no adaptive story below it. The window is
         * still resizable; what this stops is a width at which the Downloads
         * form's labels and controls overlap, which is the state a user reaches
         * by dragging and then reports as a rendering bug. */
        AppWindow.Resize(new Windows.Graphics.SizeInt32(1180, 880));

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
        if (Content is UIElement root) root.KeyboardAccelerators.Add(rescan);
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

        var page = item.Tag as string switch
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

    public void UpdateBackButton()
    {
        Nav.IsBackButtonVisible = ContentFrame.CanGoBack
            ? NavigationViewBackButtonVisible.Visible
            : NavigationViewBackButtonVisible.Collapsed;
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
