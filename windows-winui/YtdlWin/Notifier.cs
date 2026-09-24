/* Sends what NoticeTracker decides to say.
 *
 * The rules are in Core/Notices.cs. This is the posting, the poll that feeds
 * the tracker, and the two checks that decide whether a notice goes out: the
 * setting, and whether the window is in front. Both are checked AFTER the
 * tracker has been updated, so its counts stay right while the window has
 * focus and a summary sent later still covers the whole queue.
 *
 * A POLL, NOT THE DRAIN. The Runner's Drain belongs to the Downloads page and
 * its timer stops whenever another page is showing -- and a queue that ends
 * while the Library is on screen is exactly the case this exists for. So this
 * owns a timer of its own, for the life of the window, and asks
 * Runner.Settled, which does not consume anything Drain needs and answers
 * null (no clone, nothing to do) on every tick where nothing changed.
 *
 * CLICKS, AND WHY THEY MAY NOT WORK YET. A click reaches the app through
 * AppNotificationManager.Register. On Windows App SDK 2.4.0 -- the version
 * this project pins -- Register throws 0x8007007E in a self-contained
 * unpackaged app, which is what this app is: the runtime looks for a resource
 * DLL only the installed runtime ships (microsoft/WindowsAppSDK#6774; a fix
 * was merged but had not reached a release as of 2026-09). Show still works
 * without registration. So the failure is caught and the toast is shown
 * anyway: a notification you cannot click through is still the notification
 * this was for. Clicking it then does nothing rather than bringing the
 * window forward.
 */

using System;
using Microsoft.UI.Dispatching;
using Microsoft.Windows.AppNotifications;
using Microsoft.Windows.AppNotifications.Builder;
using YtdlWin.Core;

namespace YtdlWin;

public sealed class Notifier
{
    /// Four times a second: a finished queue is announced within a quarter of
    /// a second, and a tick with nothing new costs one lock and a comparison.
    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(250);

    private readonly Runner _runner;
    private readonly Settings _settings;
    private readonly NoticeTracker _tracker;
    private readonly DispatcherQueueTimer _timer;
    private long _version;

    private static bool _registered;

    /// Set by the window from its Activated event. Starts true because the
    /// window is created in front.
    public bool WindowActive { get; set; } = true;

    /* Constructed before the Runner's worker starts, so the history it is
     * seeded from is exactly what was restored -- last session's runs are
     * never announced as if they had just finished. */
    public Notifier(Runner runner, Settings settings, DispatcherQueue dispatcher)
    {
        _runner = runner;
        _settings = settings;
        var now = runner.Settled(0)!;
        _tracker = new NoticeTracker(now.History);
        _version = now.Version;

        _timer = dispatcher.CreateTimer();
        _timer.Interval = PollInterval;
        _timer.Tick += (_, _) => Poll();
        _timer.Start();
    }

    public void Stop() => _timer.Stop();

    private void Poll()
    {
        var s = _runner.Settled(_version);
        if (s is null) return;
        _version = s.Version;

        var notice = _tracker.Update(s.History, s.Remaining);
        if (notice is null || !_settings.Notify || WindowActive) return;
        Show(notice);
    }

    private static void Show(Notice notice)
    {
        try
        {
            var toast = new AppNotificationBuilder()
                .AddArgument("action", "show-downloads")
                .AddText(notice.Title)
                .AddText(notice.Body)
                .BuildNotification();
            /* One tag for everything: a toast that reuses a shown toast's tag
             * and group REPLACES it, which is what lets the end-of-queue
             * summary stand in for a mid-queue failure toast rather than stack
             * under it. */
            toast.Tag = NoticeTracker.Tag;
            toast.Group = "ytdl";
            if (notice.Failure) toast.Priority = AppNotificationPriority.High;
            AppNotificationManager.Default.Show(toast);
        }
        catch (Exception)
        {
            /* A toast that cannot be shown -- notifications off system-wide,
             * a platform too old -- is not worth interrupting anything for.
             * The run's outcome is in the history either way. */
        }
    }

    /* Called once at launch, before the window exists, which is the order
     * the App SDK asks for. `onClick` runs on the UI thread. See the header
     * for why this can fail on the pinned SDK and why that is survivable. */
    public static void RegisterForClicks(Action onClick)
    {
        try
        {
            AppNotificationManager.Default.NotificationInvoked += (_, args) =>
            {
                if (args.Arguments.TryGetValue("action", out var action) && action == "show-downloads")
                    App.Window?.DispatcherQueue.TryEnqueue(() => onClick());
            };
            AppNotificationManager.Default.Register();
            _registered = true;
        }
        catch (Exception)
        {
            _registered = false;
        }
    }

    /// Undone on the way out, as the App SDK's own samples do, so the
    /// registration does not outlive the process that made it.
    public static void Unregister()
    {
        if (!_registered) return;
        try { AppNotificationManager.Default.Unregister(); }
        catch (Exception) { /* nothing useful to do at exit */ }
        _registered = false;
    }
}
