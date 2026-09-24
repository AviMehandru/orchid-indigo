/* Sends what NoticeTracker decides to say.
 *
 * The rules are in Core/Notices.swift. This is only the posting, and the two
 * checks that decide whether a notice goes out at all: the setting, and
 * whether the app is active. Both are checked AFTER the tracker has been
 * updated, so its counts stay right while the window has focus and a summary
 * posted later still covers the whole queue.
 *
 * Fed by the Runner's onSettled rather than by the Downloads view, because a
 * queue that finishes while the Library is showing is exactly the case this
 * is for, and the views only exist to be looked at.
 *
 * WHAT IS NOT HERE. No sound, and no priority: macOS's only step up from a
 * plain banner is time-sensitive, which needs an entitlement this ad-hoc
 * signed app cannot carry, so Notice.failure goes unused on this platform.
 */

import AppKit
import UserNotifications

@MainActor
final class Notifier {
    private let tracker: NoticeTracker
    private let settings: Settings

    init(runner: Runner, settings: Settings) {
        tracker = NoticeTracker(history: runner.history)
        self.settings = settings
        runner.onSettled = { [weak self] history, remaining in
            /* The Runner calls this from its drain, which a Timer on the
             * main run loop drives -- so this IS the main thread, and
             * assumeIsolated says so to the compiler. It traps if that ever
             * stops being true, which is the right failure: a notification
             * decided on a background thread would be reading NSApp from one. */
            MainActor.assumeIsolated {
                self?.settled(history: history, remaining: remaining)
            }
        }
    }

    private func settled(history: [RunRecord], remaining: Int) {
        guard let notice = tracker.update(history: history, remaining: remaining) else { return }
        guard settings.notify, !NSApp.isActive else { return }
        post(notice)
    }

    private func post(_ notice: Notice) {
        /* No bundle identifier means no .app bundle -- a bare binary run out
         * of a build directory -- and UNUserNotificationCenter.current() raises
         * an Objective-C exception there rather than returning an error. */
        guard Bundle.main.bundleIdentifier != nil else { return }

        let center = UNUserNotificationCenter.current()
        /* Permission is asked here, the first time there is something to
         * say, rather than at launch: a prompt before the app has done
         * anything is one people decline on reflex. Once answered, this
         * returns the stored answer without asking again.
         *
         * The async forms, in a Task, rather than completion handlers: those
         * are called on a background queue, and a closure written inside this
         * @MainActor class is one the compiler may decide is main-actor
         * isolated. A Task keeps where each line runs explicit. */
        Task {
            let granted = (try? await center.requestAuthorization(options: [.alert])) ?? false
            guard granted else { return }
            let content = UNMutableNotificationContent()
            content.title = notice.title
            content.body = notice.body
            /* One identifier for everything: a request that reuses a
             * delivered notification's identifier REPLACES it, which is what
             * lets the end-of-queue summary stand in for a mid-queue failure
             * notice rather than stack under it. */
            let request = UNNotificationRequest(identifier: NoticeTracker.identifier,
                                                content: content, trigger: nil)
            try? await center.add(request)
        }
    }
}
