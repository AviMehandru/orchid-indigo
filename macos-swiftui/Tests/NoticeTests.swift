/* What the queue announces, and when -- Core/Notices.swift's rules as
 * fixtures.
 *
 * THE SAME VALUES are asserted by linux-gtk/tests/test_notify.c and
 * windows-winui/YtdlWin.Tests/NoticeTests.cs. A title or a rule changed here
 * and not there is three apps announcing the same queue three different
 * ways, so the strings are spelled out in full rather than matched loosely.
 *
 * The last test drives the real Runner, to pin the one thing the tracker
 * cannot check for itself: that `remaining` never reads zero while a run is
 * about to start.
 */

import XCTest
@testable import YtdlMac

final class NoticeTests: XCTestCase {
    private func rec(_ id: String, _ state: String, _ lastLine: String = "",
                     touched: Int64 = -1, skipped: Int64 = -1, errors: Int64 = -1) -> RunRecord {
        var r = RunRecord()
        r.id = id
        r.state = state
        r.opts.url = "https://youtu.be/\(id)"
        r.command = "ytdl \(r.opts.url)"
        r.lastLine = lastLine
        r.videosTouched = touched
        r.archiveSkipped = skipped
        r.errors = errors
        r.warnings = 0
        r.exitCode = state == "failed" ? 1 : 0
        return r
    }

    func testSingleDone() {
        let t = NoticeTracker(history: [])
        let n = t.update(history: [rec("aaaaaaaaaaa", "done", touched: 3, skipped: 12, errors: 0)],
                         remaining: 0)
        XCTAssertEqual(n, Notice(title: "Download finished",
                                 body: "https://youtu.be/aaaaaaaaaaa\n3 touched, 12 already archived",
                                 failure: false))
    }

    func testSingleFailed() {
        let t = NoticeTracker(history: [])
        let n = t.update(history: [rec("bbbbbbbbbbb", "failed",
                                       "ERROR: [youtube] bbbbbbbbbbb: Private video")],
                         remaining: 0)
        XCTAssertEqual(n, Notice(title: "Download failed",
                                 body: "https://youtu.be/bbbbbbbbbbb\n"
                                     + "ERROR: [youtube] bbbbbbbbbbb: Private video",
                                 failure: true))
    }

    /// Three runs, one at a time, the middle one failing: silence, the failure
    /// at once, then one summary that repeats it.
    func testQueueOfThree() {
        let t = NoticeTracker(history: [])
        let c1 = rec("c1", "done", touched: 1, skipped: 0, errors: 0)
        let c2 = rec("c2", "failed", "ERROR: HTTP Error 403: Forbidden",
                     touched: 0, skipped: 0, errors: 1)
        let c3 = rec("c3", "done", touched: 2, skipped: 5, errors: 0)

        XCTAssertNil(t.update(history: [c1], remaining: 2))
        XCTAssertEqual(t.update(history: [c2, c1], remaining: 1),
                       Notice(title: "A download failed",
                              body: "https://youtu.be/c2\nERROR: HTTP Error 403: Forbidden\n"
                                  + "The queue goes on: 1 still to run.",
                              failure: true))
        // Nothing new: nothing to say, and the failure is not announced twice.
        XCTAssertNil(t.update(history: [c2, c1], remaining: 1))
        XCTAssertEqual(t.update(history: [c3, c2, c1], remaining: 0),
                       Notice(title: "Queue finished: 2 done, 1 failed",
                              body: "3 touched, 5 already archived, 1 error\n"
                                  + "Last failure: https://youtu.be/c2 — "
                                  + "ERROR: HTTP Error 403: Forbidden",
                              failure: true))
    }

    func testSecondFailureCounts() {
        let t = NoticeTracker(history: [])
        let f1 = rec("f1", "failed", "ERROR: one")
        let f2 = rec("f2", "failed", "ERROR: two")
        XCTAssertEqual(t.update(history: [f1], remaining: 3)?.title, "A download failed")
        XCTAssertEqual(t.update(history: [f2, f1], remaining: 2),
                       Notice(title: "2 downloads failed so far",
                              body: "https://youtu.be/f2\nERROR: two\n"
                                  + "The queue goes on: 2 still to run.",
                              failure: true))
    }

    /// A run finishing and the queue going idle in one drain tick: the summary
    /// only, never a failure notice followed by a summary.
    func testCoalescedTick() {
        let t = NoticeTracker(history: [])
        let n = t.update(history: [rec("d2", "failed", "ERROR: gone"),
                                   rec("d1", "done", touched: 4, skipped: 0, errors: 0)],
                         remaining: 0)
        XCTAssertEqual(n, Notice(title: "Queue finished: 1 done, 1 failed",
                                 body: "4 touched, 0 already archived\n"
                                     + "Last failure: https://youtu.be/d2 — ERROR: gone",
                                 failure: true))
    }

    func testCancelledOnlyIsSilent() {
        let t = NoticeTracker(history: [])
        let e1 = rec("e1", "cancelled")
        XCTAssertNil(t.update(history: [e1], remaining: 0))
        // ...and the queue it ended is closed: the next one does not count it.
        XCTAssertEqual(t.update(history: [rec("e2", "done", touched: 1, skipped: 0, errors: 0), e1],
                                remaining: 0),
                       Notice(title: "Download finished",
                              body: "https://youtu.be/e2\n1 touched, 0 already archived",
                              failure: false))
    }

    func testCancelledIsNamedInAQueue() {
        let t = NoticeTracker(history: [])
        // No run printed a summary, so there are no counts to give.
        XCTAssertEqual(t.update(history: [rec("g2", "cancelled"), rec("g1", "done")],
                                remaining: 0),
                       Notice(title: "Queue finished: 1 done, 1 cancelled",
                              body: "Nothing failed.", failure: false))
    }

    func testRestoredHistoryIsNotAnnounced() {
        let restored = [rec("r1", "failed", "Interrupted")]
        let t = NoticeTracker(history: restored)
        XCTAssertNil(t.update(history: restored, remaining: 0))
    }

    func testFailureReasons() {
        let t = NoticeTracker(history: [])
        var silent = rec("h1", "failed")
        silent.exitCode = 2
        XCTAssertEqual(t.update(history: [silent], remaining: 0)?.body,
                       "https://youtu.be/h1\nytdl exited with code 2.")

        var killed = rec("h2", "failed", "  \n")
        killed.exitCode = -1
        XCTAssertEqual(t.update(history: [killed, silent], remaining: 0)?.body,
                       "https://youtu.be/h2\nIt printed no error before it stopped.")
    }

    /// A record from a store written before options were saved has no URL;
    /// the command stands in for it.
    func testTargetFallsBackToCommand() {
        let t = NoticeTracker(history: [])
        var r = rec("i1", "done")
        r.opts = RunOptions()
        XCTAssertEqual(t.update(history: [r], remaining: 0),
                       Notice(title: "Download finished", body: "ytdl https://youtu.be/i1",
                              failure: false))
    }

    func testClip() {
        let c1 = NoticeTracker.clip(String(repeating: "x", count: 200), max: 160)
        XCTAssertEqual(c1.unicodeScalars.count, 160)
        XCTAssertTrue(c1.hasSuffix("x…"))

        // Characters, not bytes -- and the same count the C copy makes.
        let c2 = NoticeTracker.clip(String(repeating: "é", count: 200), max: 160)
        XCTAssertEqual(c2.unicodeScalars.count, 160)

        XCTAssertEqual(NoticeTracker.clip("  ERROR: a\nb\tc  ", max: 160), "ERROR: a b c")
        XCTAssertEqual(NoticeTracker.clip("abc defgh", max: 5), "abc…")
        XCTAssertEqual(NoticeTracker.clip("short", max: 160), "short")
    }

    // MARK: - The Runner's side of it

    /* Nothing-left-to-run must never be reported while a run is between
     * leaving the queue and being recorded as current. The worker takes the
     * item, drops the lock, and only then does runOne set it current; in that
     * gap the queue is empty and nothing is running.
     *
     * Driven with runs that fail at once -- HOME and the install root point at
     * an empty directory, so each stops at "pwsh not found" or "ytdl.ps1 does
     * not exist" having gone through the same take / finish path a real run
     * does, spawning nothing -- and polled through settled() as fast as this
     * thread will go, like the GTK and C# copies. (An earlier draft watched
     * onSettled instead, and passed 8 runs of 8 with the bug put back: a 50ms
     * drain almost never lands in a gap that wide.) Probabilistic in the
     * direction that matters: it cannot fail on correct code. With
     * `pendingCurrent != nil` counted instead of pendingInFlight it failed 3
     * runs in 6, each time seeing the gap over a thousand times. */
    func testSettledNeverReportsFinishedEarly() throws {
        let dir = try FixtureSupport.makeTempDir(prefix: "ytdl-macos-notice")
        /* Through the override rather than setenv, like every other test
         * here that must not touch ~/Library: the Runner reads and writes its
         * queue under Paths.stateDir(). Cleared only after stop(), because
         * the worker reads it too. */
        Paths.environmentOverride = ["HOME": dir, "YTDLP_INSTALL_ROOT": dir]
        defer {
            Paths.environmentOverride = nil
            try? FileManager.default.removeItem(atPath: dir)
        }

        let runner = Runner()
        XCTAssertEqual(runner.settled().remaining, 0)

        let n = 40
        runner.setPaused(true)
        var o = RunOptions()
        o.url = "https://youtu.be/abcdefghijk"
        for _ in 0..<n { _ = try runner.enqueue(o) }
        // Paused with runs waiting is not finished.
        XCTAssertEqual(runner.settled().remaining, n)

        runner.start()
        runner.setPaused(false)

        var premature = 0
        var finished = false
        let deadline = Date().addingTimeInterval(20)
        while !finished && Date() < deadline {
            let s = runner.settled()
            if s.remaining == 0 {
                if s.history.count < n { premature += 1 } else { finished = true }
            }
        }

        /* The drain publishes the same pair, and hands it to onSettled. */
        var delivered: (Int, Int)?
        runner.onSettled = { history, remaining in delivered = (history.count, remaining) }
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        runner.stop()

        XCTAssertTrue(finished)
        XCTAssertEqual(premature, 0)
        XCTAssertEqual(runner.remaining, 0)
        XCTAssertEqual(runner.history.count, n)
        XCTAssertTrue(runner.history.allSatisfy { $0.state == "failed" })
        XCTAssertEqual(delivered?.0, n)
        XCTAssertEqual(delivered?.1, 0)
    }
}
