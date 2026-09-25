/* The Subscriptions pane's reading of `ytdl --subscriptions --json`, and every
 * word it shows -- as fixtures.
 *
 * THE SAME DOCUMENT AND THE SAME STRINGS are asserted by
 * linux-gtk/tests/test_subscriptions.c and
 * windows-winui/YtdlWin.Tests/SubscriptionTests.cs. Three apps describing the
 * same subscription three different ways -- "3 h ago" here, "3 hours ago"
 * there, "2 h ago" in the third because it rounded down -- is the drift this
 * file exists to stop, so the expected strings are spelled out in full.
 *
 * The document is the contract in orchid-ochre's docs/subscriptions.md, with
 * every state a row can be in: never checked, fine, nothing new, found errors,
 * failed, paused. `now` is fixed, so every relative time is exact.
 */

import XCTest
@testable import YtdlMac

final class SubscriptionTests: XCTestCase {
    /// now = 1790000000. Each started/next_due is now plus or minus a round
    /// number of seconds, so the expectations below can be checked by eye.
    static let fixture = """
    {"subscriptions_version":1,"now":1790000000,
     "schedule":{"mechanism":"systemd","supported":true,"installed":true,"active":true,
       "running":false,"next_check":1790001500,"linger":false,
       "detail":"systemd user timer ytdl-subscriptions.timer, hourly"},
     "subscriptions":[
      {"id":"a1b2c3d4","url":"https://www.youtube.com/@Never/videos","name":null,
       "options":["--sync"],"data_root":null,"every_hours":24,"enabled":true,
       "added":1789990000,"updated":1789990000,"last_run":null,"next_due":1790000000,"due":true},
      {"id":"b2c3d4e5","url":"https://www.youtube.com/@Fine/videos","name":"Fine Channel",
       "options":["--sync","--quality","1080","--proxy","socks5://***@127.0.0.1:1080"],
       "data_root":"/mnt/archive","every_hours":6,"enabled":true,"added":1789000000,"updated":1789000000,
       "last_run":{"started":1789989200,"finished":1789989500,"result":"ok","exit_code":0,
         "touched":2,"skipped":1,"errors":0,"warnings":0,"trigger":"schedule","message":null},
       "next_due":1790010800,"due":false},
      {"id":"c3d4e5f6","url":"https://www.youtube.com/@Quiet/videos","name":"Quiet","options":[],
       "data_root":null,"every_hours":168,"enabled":true,"added":1788000000,"updated":1788000000,
       "last_run":{"started":1789913600,"finished":1789913660,"result":"ok","exit_code":0,
         "touched":0,"skipped":1,"errors":0,"warnings":0,"trigger":"schedule","message":null},
       "next_due":1790518400,"due":false},
      {"id":"d4e5f6a7","url":"https://www.youtube.com/@Errs/videos","name":"Some Errors",
       "options":["--sync"],"data_root":null,"every_hours":1,"enabled":true,"added":1788000000,
       "updated":1788000000,
       "last_run":{"started":1789999970,"finished":1789999990,"result":"errors","exit_code":0,
         "touched":3,"skipped":0,"errors":1,"warnings":2,"trigger":"manual","message":null},
       "next_due":1790003570,"due":false},
      {"id":"e5f6a7b8","url":"https://www.youtube.com/@Broken/videos","name":"Broken",
       "options":["--codec","av1"],"data_root":null,"every_hours":12,"enabled":true,
       "added":1788000000,"updated":1788000000,
       "last_run":{"started":1789992800,"finished":1789992802,"result":"failed","exit_code":1,
         "touched":0,"skipped":0,"errors":0,"warnings":0,"trigger":"schedule",
         "message":"Error: --codec must be one of: any, avc1, vp9, av01 (got: 'av1'). Note av01 is spelled with a zero."},
       "next_due":1790036000,"due":false},
      {"id":"f6a7b8c9","url":"https://www.youtube.com/@Paused/videos","name":"Paused One",
       "options":["--sync"],"data_root":null,"every_hours":48,"enabled":false,
       "added":1788000000,"updated":1788000000,"a_future_field":{"x":1},
       "last_run":{"started":1789740800,"finished":1789740900,"result":"ok","exit_code":0,
         "touched":1,"skipped":0,"errors":0,"warnings":0,"trigger":"schedule","message":null},
       "next_due":null,"due":false}
     ]}
    """

    static let expected: [(id: String, title: String, every: String, status: String)] = [
        ("a1b2c3d4", "https://www.youtube.com/@Never/videos", "Every day",
         "Not checked yet · due now"),
        ("b2c3d4e5", "Fine Channel", "Every 6 hours",
         "Checked 3 h ago · 2 new · next in 3 h"),
        ("c3d4e5f6", "Quiet", "Every 7 days",
         "Checked 24 h ago · nothing new · next in 6 d"),
        ("d4e5f6a7", "Some Errors", "Every hour",
         "Checked just now · 3 new, 1 error · next in 60 min"),
        ("e5f6a7b8", "Broken", "Every 12 hours",
         "Check failed 2 h ago · next in 10 h"),
        ("f6a7b8c9", "Paused One", "Every 2 days",
         "Paused · last checked 3 d ago"),
    ]

    func testParseFixture() throws {
        let l = try SubscriptionList.parse(Self.fixture)
        XCTAssertEqual(l.version, 1)
        XCTAssertEqual(l.now, 1_790_000_000)
        XCTAssertEqual(l.subscriptions.count, Self.expected.count)

        XCTAssertEqual(l.schedule.mechanism, "systemd")
        XCTAssertTrue(l.schedule.supported)
        XCTAssertTrue(l.schedule.installed)
        XCTAssertTrue(l.schedule.active)
        XCTAssertFalse(l.schedule.running)
        XCTAssertEqual(l.schedule.nextCheck, 1_790_001_500)
        XCTAssertEqual(l.schedule.linger, false)

        let never = l.subscriptions[0]
        XCTAssertNil(never.name)
        XCTAssertNil(never.lastRun)
        XCTAssertNil(never.dataRoot)
        XCTAssertTrue(never.due)

        let fine = l.subscriptions[1]
        XCTAssertEqual(fine.dataRoot, "/mnt/archive")
        XCTAssertEqual(fine.options.count, 5)
        XCTAssertEqual(fine.options[4], "socks5://***@127.0.0.1:1080")
        XCTAssertEqual(fine.lastRun?.touched, 2)
        XCTAssertEqual(fine.lastRun?.skipped, 1)
        XCTAssertEqual(fine.lastRun?.trigger, "schedule")

        XCTAssertEqual(l.subscriptions[2].options, [])

        let broken = l.subscriptions[4]
        XCTAssertEqual(broken.lastRun?.result, "failed")
        XCTAssertEqual(broken.lastRun?.exitCode, 1)
        XCTAssertTrue(broken.lastRun?.message?.hasPrefix("Error: --codec must be one of") ?? false)

        let paused = l.subscriptions[5]
        XCTAssertFalse(paused.enabled)
        XCTAssertEqual(paused.nextDue, 0)
    }

    func testRowWording() throws {
        let l = try SubscriptionList.parse(Self.fixture)
        for (s, e) in zip(l.subscriptions, Self.expected) {
            XCTAssertEqual(s.id, e.id)
            XCTAssertEqual(s.title, e.title)
            XCTAssertEqual(SubscriptionText.every(s.everyHours), e.every)
            XCTAssertEqual(s.statusLine(now: l.now), e.status)
        }
    }

    func testScheduleWording() throws {
        let l = try SubscriptionList.parse(Self.fixture)
        var s = l.schedule
        XCTAssertEqual(s.statusLine(now: l.now),
                       "On · checks hourly · next check in 25 min · only while you are logged in")
        s.linger = true
        s.running = true
        XCTAssertEqual(s.statusLine(now: l.now),
                       "On · checks hourly · next check in 25 min · checking now")
        s.running = false
        s.nextCheck = 0
        s.linger = nil
        XCTAssertEqual(s.statusLine(now: l.now), "On · checks hourly")
        s.active = false
        XCTAssertEqual(s.statusLine(now: l.now),
                       "Installed but not running · turn it off and on again")
        s.installed = false
        XCTAssertEqual(s.statusLine(now: l.now),
                       "Off · subscriptions are checked only when you press Check now")
        s.supported = false
        XCTAssertEqual(s.statusLine(now: l.now),
                       "systemd user timer ytdl-subscriptions.timer, hourly")
    }

    func testRelativeTimes() {
        let now: Int64 = 1_790_000_000
        let cases: [(Int64, String, String)] = [
            (0, "just now", "due now"),
            (89, "just now", "in 1 min"),
            (90, "2 min ago", "in 2 min"),
            (150, "3 min ago", "in 3 min"),     // 2.5 rounds UP, in all three
            (5399, "90 min ago", "in 90 min"),
            (5400, "2 h ago", "in 2 h"),
            (9000, "3 h ago", "in 3 h"),        // 2.5 h rounds up
            (129_599, "36 h ago", "in 36 h"),
            (129_600, "2 d ago", "in 2 d"),     // 1.5 d rounds up
        ]
        for (delta, ago, inFuture) in cases {
            XCTAssertEqual(SubscriptionText.ago(now - delta, now: now), ago)
            XCTAssertEqual(SubscriptionText.inFuture(now + delta, now: now), inFuture)
        }
        // A clock that went backwards is "just now", not "-3 min ago".
        XCTAssertEqual(SubscriptionText.ago(now + 600, now: now), "just now")
        XCTAssertEqual(SubscriptionText.inFuture(now - 600, now: now), "due now")
    }

    func testEveryLabels() {
        let cases: [(Int, String)] = [
            (1, "Every hour"), (2, "Every 2 hours"), (23, "Every 23 hours"),
            (24, "Every day"), (48, "Every 2 days"), (36, "Every 36 hours"),
            (168, "Every 7 days"), (720, "Every 30 days"),
        ]
        for (h, label) in cases { XCTAssertEqual(SubscriptionText.every(h), label) }
    }

    func testRefusesWhatItCannotRead() {
        XCTAssertThrowsError(try SubscriptionList.parse(
            #"{"subscriptions_version":2,"subscriptions":[]}"#)) { error in
            XCTAssertEqual(error as? SubscriptionList.ParseError, .newerVersion(2))
            XCTAssertTrue(error.localizedDescription.contains("version 2"))
        }
        XCTAssertThrowsError(try SubscriptionList.parse("[]"))
        XCTAssertThrowsError(try SubscriptionList.parse("not json"))

        // An empty list is a list.
        let empty = try? SubscriptionList.parse(
            #"{"subscriptions_version":1,"now":1,"schedule":{"supported":true,"detail":"not installed"},"subscriptions":[]}"#)
        XCTAssertEqual(empty?.subscriptions.count, 0)
        XCTAssertNil(empty?.schedule.linger)
    }

    func testArguments() {
        XCTAssertEqual(SubscriptionArgs.list, ["--subscriptions", "--json"])

        var o = RunOptions()
        o.url = "https://www.youtube.com/@Chan/videos"
        o.sync = true
        o.quality = "1080"
        o.proxy = "socks5://me:pw@127.0.0.1:1080"
        o.refresh = true // never stored: the pipeline refuses it
        XCTAssertEqual(
            SubscriptionArgs.subscribe(o, everyHours: 12, name: "My Channel").joined(separator: " "),
            "https://www.youtube.com/@Chan/videos --sync --quality 1080 "
                + "--proxy socks5://me:pw@127.0.0.1:1080 --subscribe --every 12h --name My Channel")
        XCTAssertTrue(SubscriptionArgs.subscribe(o, everyHours: 48, name: nil)
            .joined(separator: " ").hasSuffix("--subscribe --every 2d"))

        XCTAssertEqual(SubscriptionArgs.edit(id: "b2c3d4e5", pause: true),
                       ["--edit-subscription", "b2c3d4e5", "--pause"])
        XCTAssertEqual(SubscriptionArgs.edit(id: "b2c3d4e5", everyHours: 168, pause: false),
                       ["--edit-subscription", "b2c3d4e5", "--every", "7d", "--resume"])
        XCTAssertEqual(SubscriptionArgs.edit(id: "b2c3d4e5", everyHours: 6),
                       ["--edit-subscription", "b2c3d4e5", "--every", "6h"])
        XCTAssertEqual(SubscriptionArgs.unsubscribe("b2c3d4e5"), ["--unsubscribe", "b2c3d4e5"])
        XCTAssertEqual(SubscriptionArgs.schedule(install: true), ["--schedule", "install"])
        XCTAssertEqual(SubscriptionArgs.schedule(install: false), ["--schedule", "remove"])
    }

    func testCheckNowGoesThroughTheQueue() throws {
        let l = try SubscriptionList.parse(Self.fixture)
        var o = l.subscriptions[1].runOptions()

        /* What the Runner stamps on at enqueue, and what a Run again copies:
         * none of it may reach the command, because the subscription has its
         * own. */
        o.proxy = "http://other:1"
        o.quality = "480"
        XCTAssertEqual(o.toArgs(), ["--run-subscriptions", "b2c3d4e5"])
        XCTAssertEqual(o.commandPreview(), "ytdl --run-subscriptions b2c3d4e5")

        // Survives the queue file.
        let back = RunOptions.fromJSON(o.toJSON())
        XCTAssertEqual(back.subscriptionID, "b2c3d4e5")
        XCTAssertEqual(back.url, "https://www.youtube.com/@Fine/videos")
        XCTAssertEqual(back.dataRoot, "/mnt/archive")

        // A queue file written before subscriptions existed reads as a plain
        // download.
        let old = RunOptions.fromJSON(["url": "https://youtu.be/x", "sync": false])
        XCTAssertEqual(old.subscriptionID, "")
        XCTAssertEqual(old.toArgs().first, "https://youtu.be/x")
    }

    func testFailureMessages() {
        var r = YtdlCommandResult(
            exitCode: 1,
            stderr: "Note: without --sync, every check walks the whole listing.\n"
                + "Error: --codec must be one of: any, avc1, vp9, av01\n")
        XCTAssertEqual(r.message, "Error: --codec must be one of: any, avc1, vp9, av01")

        r = YtdlCommandResult(exitCode: 3, stderr: "[subscriptions] Another subscription check "
            + "is already running; not starting a second one.\n")
        XCTAssertTrue(r.message.hasPrefix("[subscriptions] Another"))
        XCTAssertEqual(YtdlCommandResult(exitCode: 3).message, "ytdl exited with code 3")

        // The two ways an older pipeline answers.
        XCTAssertTrue(YtdlCommandResult(exitCode: 1, stderr: "Warning: '--subscriptions' does not "
            + "look like a YouTube URL.\nUnknown option: --json\n").meansTooOld)
        XCTAssertTrue(YtdlCommandResult(exitCode: 1, stderr: "Error: /x/scripts/subscriptions.ps1 "
            + "is missing -- this install predates subscriptions.\n").meansTooOld)
        XCTAssertFalse(YtdlCommandResult(exitCode: 1, stderr: "Error: no subscription 'x'.\n")
            .meansTooOld)
    }
}
