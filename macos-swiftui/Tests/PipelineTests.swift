/* The download runner's pure parts.
 *
 * Three things are covered here, and they are the three that would break
 * silently rather than loudly:
 *
 *   1. Argument building. A flag emitted when it should not be, or a default
 *      leaking into the command line, changes what ends up in the archive.
 *   2. Progress parsing. yt-dlp's progress line is not an API; a change in it
 *      makes the bar stop moving with no error anywhere.
 *   3. The session summary. Those four counts exist ONLY in that one line of
 *      run_ytdlp.ps1 output. Nothing else in the system reports them.
 *
 * The spawn/pump/cancel path has its own file -- see RunnerProcessTests.swift,
 * which is where the process-group requirement is actually exercised.
 */

import XCTest
@testable import YtdlMac

final class PipelineTests: XCTestCase {
    /// Joined with '|' so a failure prints the whole command line rather than
    /// "expected 4, got 6".
    private func args(_ o: RunOptions) -> String {
        o.toArgs().joined(separator: "|")
    }

    func testNormalizeURL() {
        XCTAssertEqual(
            RunOptions.normalizeURL("dQw4w9WgXcQ"),
            "https://www.youtube.com/watch?v=dQw4w9WgXcQ"
        )

        /* About one YouTube id in thirty starts with "-" or "_", and a bare one
         * would be bound as a parameter before ytdl.ps1 ever saw it.
         * Normalising is what removes that whole class. */
        XCTAssertEqual(
            RunOptions.normalizeURL("-dQw4w9WgXc"),
            "https://www.youtube.com/watch?v=-dQw4w9WgXc"
        )

        XCTAssertEqual(
            RunOptions.normalizeURL("https://www.youtube.com/watch?v=abc"),
            "https://www.youtube.com/watch?v=abc"
        )
        XCTAssertEqual(
            RunOptions.normalizeURL("youtu.be/abcdefghijk"),
            "https://youtu.be/abcdefghijk"
        )

        /* Not a validator. A channel name is passed through untouched, because
         * yt-dlp's extractor is the real authority on what is downloadable. */
        XCTAssertEqual(RunOptions.normalizeURL("  not a url  "), "not a url")
    }

    func testDefaultsEmitNothing() {
        var o = RunOptions()
        o.url = "https://example.com/v"
        // Exactly what the form hands over when nothing has been touched.
        o.mode = "full"
        o.quality = "best"
        o.codec = "any"
        o.audioCodec = "any"
        o.container = "mkv"
        o.workers = 1

        /* A plain download must produce the command line it produced before any
         * of these options existed. --mode full and friends are no-ops the
         * pipeline would accept and ignore, but emitting them would put four
         * extra flags in the preview of every ordinary download. */
        XCTAssertEqual(args(o), "https://example.com/v")
    }

    func testNonDefaultsEmit() {
        var o = RunOptions()
        o.url = "https://example.com/v"
        o.mode = "audio-only"
        o.quality = "1080"
        o.codec = "vp9"
        o.audioCodec = "opus"
        o.container = "webm"
        o.sync = true
        o.lazy = true
        o.noComments = true
        o.workers = 4

        XCTAssertEqual(
            args(o),
            "https://example.com/v|--sync|--lazy|--workers|4|"
            + "--mode|audio-only|--quality|1080|--codec|vp9|"
            + "--audio-codec|opus|--container|webm|--no-comments"
        )
    }

    func testWorkersOnlyAboveOne() {
        var o = RunOptions()
        o.url = "u"

        o.workers = 1
        XCTAssertFalse(args(o).contains("--workers"))

        o.workers = 2
        XCTAssertTrue(args(o).contains("--workers|2"))
    }

    func testPassthroughArgsRepeat() {
        var o = RunOptions()
        o.url = "u"
        o.ytdlpArgs = [
            "--match-filter",
            // Contains a comma AND a space -- which is exactly why these are
            // repeated rather than joined on any separator.
            "duration > 60 & view_count > 10",
            "   ",
        ]

        XCTAssertEqual(
            args(o),
            "u|--ytdlp-arg|--match-filter|--ytdlp-arg|duration > 60 & view_count > 10"
        )
    }

    func testDataRootTildeIsExpanded() {
        var o = RunOptions()
        o.url = "u"
        o.dataRoot = "~/Movies"

        let v = o.toArgs()
        XCTAssertEqual(v[1], "--path")

        /* The bug this pins: run_ytdlp.ps1 resolves -DataRoot with GetFullPath,
         * which has no notion of a home directory, so an unexpanded "~/Movies"
         * reached it as a literal "~" folder under the pipeline's working
         * directory -- while this app's own expansion pointed somewhere else.
         * The library indexed one folder and the downloads went to another,
         * with nothing reporting an error. */
        XCTAssertTrue(v[2].hasPrefix("/"))
        XCTAssertFalse(v[2].contains("~"))
        XCTAssertTrue(v[2].hasSuffix("/Movies"))
    }

    func testCommandPreviewQuotes() {
        var o = RunOptions()
        o.url = "https://example.com/v"
        o.ytdlpArgs = ["a b"]

        /* The URL is always quoted; anything with a space is quoted; nothing
         * else is. What is shown must be pasteable into a terminal as-is. */
        XCTAssertEqual(
            o.commandPreview(),
            "ytdl \"https://example.com/v\" --ytdlp-arg \"a b\""
        )
    }

    func testStripANSI() {
        XCTAssertEqual(
            OutputParser.stripANSI("\u{1b}[0;32m[download]\u{1b}[0m  12.0%"),
            "[download]  12.0%"
        )
        XCTAssertEqual(OutputParser.stripANSI("no escapes here"), "no escapes here")
    }

    func testProgressLine() {
        var p = RunProgress()

        XCTAssertTrue(OutputParser.parseProgressLine(
            "[download]  45.2% of  120.00MiB at   2.00MiB/s ETA 00:30", into: &p
        ))
        XCTAssertEqual(p.percent, 45.2, accuracy: 0.001)
        XCTAssertEqual(p.total, "120.00MiB")
        XCTAssertEqual(p.speed, "2.00MiB/s")
        XCTAssertEqual(p.eta, "00:30")
        XCTAssertEqual(p.stage, "downloading")

        XCTAssertTrue(OutputParser.parseProgressLine(
            "[download] Destination: Final Video.f137.mp4", into: &p
        ))
        XCTAssertEqual(p.destination, "Final Video.f137.mp4")

        /* A stage marker clears the percentage: 45% of the download is not 45%
         * of the merge, and leaving the old number up reads as a stalled bar. */
        XCTAssertTrue(OutputParser.parseProgressLine("[Merger] Merging formats", into: &p))
        XCTAssertEqual(p.stage, "merging")
        XCTAssertLessThan(p.percent, 0)

        XCTAssertTrue(OutputParser.parseProgressLine(
            "[youtube] dQw4w9WgXcQ: Downloading webpage", into: &p
        ))
        XCTAssertEqual(p.videoID, "dQw4w9WgXcQ")

        // Ordinary chatter changes nothing.
        XCTAssertFalse(OutputParser.parseProgressLine("some unrelated output", into: &p))
    }

    func testSessionSummary() {
        let full = OutputParser.parseSessionSummary(
            "-- Session summary: 12 video(s) touched, 3 already archived (skipped), "
            + "1 error(s), 2 warning(s) --"
        )
        XCTAssertEqual(full, OutputParser.SessionSummary(
            videos: 12, skipped: 3, errors: 1, warnings: 2
        ))

        // Zeroes are a real answer and must parse, not be mistaken for absence.
        let zeroes = OutputParser.parseSessionSummary(
            "-- Session summary: 0 video(s) touched, 0 already archived (skipped), "
            + "0 error(s), 0 warning(s) --"
        )
        XCTAssertEqual(zeroes?.videos, 0)

        XCTAssertNil(OutputParser.parseSessionSummary("[download] 100% of 1.00MiB"))
        XCTAssertNil(OutputParser.parseSessionSummary("Session summary: incomplete 1 2"))
    }

    /* A run written by an older build must still load. Every field is optional
     * on read, which is what makes a queue survive an upgrade rather than
     * failing the whole file. */
    func testRunOptionsRoundTripAndTolerateMissingFields() {
        var o = RunOptions()
        o.url = "https://example.com/v"
        o.mode = "audio-only"
        o.workers = 4
        o.sync = true
        o.ytdlpArgs = ["--match-filter", "duration > 60"]

        let restored = RunOptions.fromJSON(o.toJSON())
        XCTAssertEqual(restored, o)

        let sparse = RunOptions.fromJSON(["mode": "video-only"])
        XCTAssertEqual(sparse.mode, "video-only")
        XCTAssertEqual(sparse.workers, 0)
        XCTAssertTrue(sparse.ytdlpArgs.isEmpty)

        // Not an object at all, which is what a truncated write leaves behind.
        XCTAssertEqual(RunOptions.fromJSON(nil), RunOptions())
    }

    // MARK: - The options that used to be reachable only through Advanced

    func testMediaOptionsEmitInOrder() {
        var o = RunOptions()
        o.url = "u"
        o.container = "mkv"
        o.fps = 30
        o.subLangs = "en.*,de"
        o.noChapters = true
        o.sponsorblockRemove = "sponsor,selfpromo"
        XCTAssertEqual(args(o),
                       "u|--fps|30|--sub-langs|en.*,de|--no-chapters|"
                       + "--sponsorblock-remove|sponsor,selfpromo")
    }

    func testNewOptionsDefaultToNothing() {
        var o = RunOptions()
        o.url = "u"
        o.subLangs = "   "
        o.downloader = "native"
        XCTAssertEqual(args(o), "u")
    }

    /* After every content option and before the passthrough, so a
     * --ytdlp-arg --proxy from before these existed still comes last and
     * still wins. */
    func testConnectionOptionsEmitBeforePassthrough() {
        var o = RunOptions()
        o.url = "u"
        o.noSubs = true
        o.cookiesFromBrowser = "firefox:default"
        o.proxy = "socks5://u:pw@h:1080"
        o.limitRate = "2M"
        o.downloader = "aria2c"
        o.ytdlpArgs = ["--proxy"]
        XCTAssertEqual(args(o),
                       "u|--no-subs|--cookies-from-browser|firefox:default|"
                       + "--proxy|socks5://u:pw@h:1080|--limit-rate|2M|"
                       + "--downloader|aria2c|--ytdlp-arg|--proxy")
    }

    func testProbeGetsCookiesAndProxyOnly() {
        var o = RunOptions()
        o.cookiesFile = "/Users/me/cookies.txt"
        o.proxy = "http://p:3128"
        o.limitRate = "2M"
        o.downloader = "aria2c"
        XCTAssertEqual(o.connectionArgs(forProbe: true).joined(separator: "|"),
                       "--cookies|/Users/me/cookies.txt|--proxy|http://p:3128")
        XCTAssertEqual(o.connectionArgs(forProbe: false).joined(separator: "|"),
                       "--cookies|/Users/me/cookies.txt|--proxy|http://p:3128|"
                       + "--limit-rate|2M|--downloader|aria2c")
    }

    /// Shared fixture: the GTK and WinUI suites assert these same six pairs.
    func testRedactProxy() {
        let cases: [(String, String)] = [
            ("socks5://me:hunter2@127.0.0.1:1080", "socks5://***@127.0.0.1:1080"),
            ("http://user@proxy:3128", "http://***@proxy:3128"),
            ("http://proxy:3128", "http://proxy:3128"),
            ("https://a:b@c@host:1/", "https://***@host:1/"),
            ("socks5h://u:p@[::1]:1080", "socks5h://***@[::1]:1080"),
            ("not a url", "not a url"),
        ]
        for (input, expected) in cases {
            XCTAssertEqual(RunOptions.redactProxy(input), expected, input)
        }
    }

    func testPreviewMasksTheProxyPasswordAndArgvDoesNot() {
        var o = RunOptions()
        o.url = "u"
        o.proxy = "socks5://me:hunter2@h:1080"
        let cmd = o.commandPreview()
        XCTAssertFalse(cmd.contains("hunter2"))
        XCTAssertTrue(cmd.contains("--proxy socks5://***@h:1080"))
        XCTAssertTrue(args(o).contains("socks5://me:hunter2@h:1080"),
                      "the real argv must carry it whole")
    }

    /// The rules the form's greyed-out rows follow. Shared fixture with the
    /// GTK and WinUI suites, row for row.
    func testDropInapplicable() {
        var a = RunOptions()
        a.mode = "comments-only"
        a.fps = 30
        a.noChapters = true
        a.sponsorblockMark = "all"
        a.sponsorblockRemove = "sponsor"
        a.subLangs = "de"
        a.proxy = "http://p:1"
        a.dropInapplicable()
        XCTAssertEqual(a.fps, 0)
        XCTAssertFalse(a.noChapters)
        XCTAssertEqual(a.sponsorblockMark, "")
        XCTAssertEqual(a.sponsorblockRemove, "")
        XCTAssertEqual(a.subLangs, "de", "subtitles are written in every mode")
        XCTAssertEqual(a.proxy, "http://p:1", "the connection is not the form's to drop")

        var b = RunOptions()
        b.noSubs = true
        b.subLangs = "de"
        b.dropInapplicable()
        XCTAssertEqual(b.subLangs, "")

        var c = RunOptions()
        c.mode = "full"
        c.noChapters = true
        c.sponsorblockMark = "all"
        c.dropInapplicable()
        XCTAssertFalse(c.noChapters, "marking needs chapters, so it wins")
        XCTAssertEqual(c.sponsorblockMark, "all")

        var d = RunOptions()
        d.mode = "audio-only"
        d.fps = 60
        d.noChapters = true
        d.sponsorblockRemove = "sponsor"
        d.dropInapplicable()
        XCTAssertEqual(d.fps, 60)
        XCTAssertTrue(d.noChapters)
        XCTAssertEqual(d.sponsorblockRemove, "sponsor")
    }

    func testNewFieldsRoundTripJSON() {
        var o = RunOptions()
        o.url = "u"
        o.fps = 60
        o.subLangs = "all,-live_chat"
        o.noChapters = true
        o.sponsorblockMark = "all,-filler"
        o.cookiesFile = "/c.txt"
        o.proxy = "http://p:1"
        o.limitRate = "500K"
        o.downloader = "aria2c"
        XCTAssertEqual(RunOptions.fromJSON(o.toJSON()), o)
    }

    func testLogTail() throws {
        let dir = try FixtureSupport.makeTempDir(prefix: "ytdl-macos-tail")
        defer { try? FileManager.default.removeItem(atPath: dir) }

        let path = Paths.join(dir, "download.log")
        let body = (1...500).map { "line \($0)" }.joined(separator: "\n") + "\n"
        try FixtureSupport.write(body, to: path)

        XCTAssertEqual(
            Health.logTail(path: path, lines: 3),
            "line 498\nline 499\nline 500"
        )

        /* Asking for more lines than exist yields the file, and no leading
         * blank from the trailing newline. */
        let all = Health.logTail(path: path, lines: 10000)
        XCTAssertTrue(all.hasPrefix("line 1\n"))
        XCTAssertTrue(all.hasSuffix("line 500"))

        XCTAssertEqual(Health.logTail(path: Paths.join(dir, "nope.log"), lines: 10), "")
    }
}
