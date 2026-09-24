/* What the queue tells you when you are not looking at the window.
 *
 * This file decides WHAT to say and WHEN. It does not send anything: the app
 * turns a Notice into a UNNotificationRequest, and only while it is not the
 * active application. Kept apart from the sending so the rules can be tested
 * without a notification center, and read side by side with the C and C#
 * copies -- linux-gtk/src/notify.c and windows-winui/YtdlWin/Core/Notices.cs
 * -- which are asserted against the same fixture values (Tests/NoticeTests).
 *
 * THE RULES. A "queue" is everything the runner does between two moments
 * when it has nothing left to run; thirty re-fetches from one bulk press are
 * one queue, and so is a single Add to queue.
 *
 *  1. When a queue ends, ONE summary, not one notification per run. A bulk
 *     re-fetch would otherwise put thirty on screen, which teaches people to
 *     switch notifications off -- after which the 3am failure this exists for
 *     goes unannounced again.
 *  2. When a run FAILS and more are still to run, say so at once: the end of
 *     the queue may be hours away. A success mid-queue says nothing.
 *  3. Both use ONE notification identifier (NoticeTracker.identifier), so the
 *     summary replaces a mid-queue failure notice instead of stacking under
 *     it. The summary repeats the failure, so nothing is lost.
 *  4. A queue in which every run was cancelled says nothing. Cancelling is
 *     done at the window, by somebody looking at it.
 *  5. Runs restored from the last session's history are never announced.
 *
 * Whether the app is active, and whether the setting is on, are the caller's
 * to check -- deliberately AFTER update(), so the counts stay right while the
 * window has focus and a summary sent later still covers the whole queue.
 */

import Foundation

struct Notice: Equatable {
    var title: String
    var body: String
    /// Something failed. Shown with more weight where the platform has a way
    /// to; it never decides WHETHER to notify.
    var failure: Bool
}

final class NoticeTracker {
    /// The one identifier this app posts under. See rule 3.
    static let identifier = "queue"
    /// Longest failure reason and run target carried into a notice, in
    /// characters. A yt-dlp error line can be several hundred characters of
    /// URL and traceback; a notification shows two or three lines, and a body
    /// the system truncates mid-word reads worse than one cut here.
    static let reasonMax = 160
    static let targetMax = 100

    /// Every run id already accounted for. Rebuilt from the history on each
    /// update, so it never outgrows the capped history.
    private var seen: Set<String>

    // The queue in progress. Reset when it ends.
    private var done = 0
    private var failed = 0
    private var cancelled = 0
    /// Any run reported a session summary.
    private var haveCounts = false
    private var touched: Int64 = 0
    private var skipped: Int64 = 0
    private var errors: Int64 = 0
    private var lastDoneTarget = ""
    private var lastFailureTarget = ""
    private var lastFailureReason = ""

    /// `history` is what the Runner restored at launch. Every id in it counts
    /// as already announced -- rule 5.
    init(history: [RunRecord]) {
        seen = Set(history.map(\.id).filter { !$0.isEmpty })
    }

    /* Whitespace trimmed, then cut at `max` CHARACTERS -- Unicode scalars, to
     * count exactly what the C copy's g_utf8_strlen counts -- with an ellipsis
     * standing in for what was dropped. Line breaks and tabs inside become
     * spaces: the body's own line breaks separate the target from the reason,
     * and a reason that brought its own would scramble that. */
    static func clip(_ text: String, max: Int) -> String {
        let ascii: Set<Unicode.Scalar> = [" ", "\t", "\n", "\r", "\u{0B}", "\u{0C}"]
        var scalars = Array(text.unicodeScalars)
        while let first = scalars.first, ascii.contains(first) { scalars.removeFirst() }
        while let last = scalars.last, ascii.contains(last) { scalars.removeLast() }
        scalars = scalars.map { $0 == "\n" || $0 == "\r" || $0 == "\t" ? " " : $0 }

        func string(_ s: ArraySlice<Unicode.Scalar>) -> String {
            var v = String.UnicodeScalarView()
            v.append(contentsOf: s)
            return String(v)
        }
        if scalars.count <= max { return string(scalars[...]) }

        /* One character goes to the ellipsis, so the result is never longer
         * than asked for; trailing space before it is trimmed so the cut
         * never reads "word …". */
        var head = scalars[0..<(max - 1)]
        while let last = head.last, ascii.contains(last) { head = head.dropLast() }
        return string(head) + "…"
    }

    /// What the run was OF: the URL someone pasted, which is what they would
    /// recognise. The command stands in for a record restored from a store
    /// written before options were saved.
    private static func target(_ r: RunRecord) -> String {
        let t = !r.opts.url.isEmpty ? r.opts.url
            : !r.command.isEmpty ? r.command
            : "a run"
        return clip(t, max: targetMax)
    }

    /// Why it failed: the last line it printed, almost always yt-dlp's ERROR
    /// line or the reason the app could not start it. Only a run that died
    /// silently falls back to its exit code.
    private static func reason(_ r: RunRecord) -> String {
        let clipped = clip(r.lastLine, max: reasonMax)
        if !clipped.isEmpty { return clipped }
        if r.exitCode > 0 { return "ytdl exited with code \(r.exitCode)." }
        return "It printed no error before it stopped."
    }

    private func resetQueue() {
        done = 0; failed = 0; cancelled = 0
        haveCounts = false
        touched = 0; skipped = 0; errors = 0
        lastDoneTarget = ""; lastFailureTarget = ""; lastFailureReason = ""
    }

    /// "3 touched, 12 already archived, 1 error": the pipeline's own session
    /// summary in its own words, summed. nil when no run printed one, because
    /// "0 touched" would read as "it ran and found nothing".
    private var countsLine: String? {
        guard haveCounts else { return nil }
        var s = "\(touched) touched, \(skipped) already archived"
        if errors > 0 { s += ", \(errors) \(errors == 1 ? "error" : "errors")" }
        return s
    }

    private func summary() -> Notice {
        let runs = done + failed + cancelled
        /* One run: say what it was. "Queue finished: 1 done" would be a
         * strange way to put the common case. */
        if runs == 1 {
            if failed == 1 {
                return Notice(title: "Download failed",
                              body: "\(lastFailureTarget)\n\(lastFailureReason)",
                              failure: true)
            }
            let body = countsLine.map { "\(lastDoneTarget)\n\($0)" } ?? lastDoneTarget
            return Notice(title: "Download finished", body: body, failure: false)
        }

        var parts: [String] = []
        if done > 0 { parts.append("\(done) done") }
        if failed > 0 { parts.append("\(failed) failed") }
        if cancelled > 0 { parts.append("\(cancelled) cancelled") }

        var lines: [String] = []
        if let counts = countsLine { lines.append(counts) }
        if failed > 0 {
            lines.append("Last failure: \(lastFailureTarget) — \(lastFailureReason)")
        }
        let body = lines.isEmpty ? "Nothing failed." : lines.joined(separator: "\n")
        return Notice(title: "Queue finished: " + parts.joined(separator: ", "),
                      body: body, failure: failed > 0)
    }

    /* Feed it every state change, with the history (newest first) and how
     * many runs are still to run -- Runner.remaining, read under the same lock
     * as the history. Returns what to announce now, or nil.
     *
     * Coalescing is expected: the Runner drains every 50ms, so one call may
     * carry several newly finished runs, or a run finishing and the queue
     * going idle at once. The second produces only the summary. */
    func update(history: [RunRecord], remaining: Int) -> Notice? {
        var failedNow = false

        /* Newest first, so walked backwards: when two runs finish inside one
         * tick, the later one's failure is the one a notice should name. */
        for r in history.reversed() where !r.id.isEmpty && !seen.contains(r.id) {
            switch r.state {
            case "done":
                done += 1
                lastDoneTarget = NoticeTracker.target(r)
            case "failed":
                failed += 1
                failedNow = true
                lastFailureTarget = NoticeTracker.target(r)
                lastFailureReason = NoticeTracker.reason(r)
            case "cancelled":
                cancelled += 1
            default:
                break
            }
            // -1 means the run never printed a summary; see countsLine.
            if r.videosTouched >= 0 {
                haveCounts = true
                touched += r.videosTouched
                skipped += Swift.max(r.archiveSkipped, 0)
                errors += Swift.max(r.errors, 0)
            }
        }
        seen = Set(history.map(\.id).filter { !$0.isEmpty })

        if remaining == 0 {
            // Rule 4: a queue that only ever got cancelled ends silently.
            let notice = done + failed > 0 ? summary() : nil
            resetQueue()
            return notice
        }

        if failedNow {
            let title = failed == 1 ? "A download failed" : "\(failed) downloads failed so far"
            return Notice(
                title: title,
                body: "\(lastFailureTarget)\n\(lastFailureReason)\n"
                    + "The queue goes on: \(remaining) still to run.",
                failure: true)
        }
        return nil
    }
}
