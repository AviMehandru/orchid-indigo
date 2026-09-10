/* Option profiles, and the two small stores beside them.
 *
 * These are the app's only user-authored data. An index rebuilds from one
 * rescan and a queue is retypeable; a set of profiles built up over months is
 * not, which is why the store writes through a temp file and a rename and why
 * the round trip is pinned here.
 *
 * The rules that are easy to get subtly wrong, and are all asserted below:
 * names are case-insensitive but keep their newest capitalisation, deleting the
 * active profile clears the selection rather than leaving it pointing at
 * nothing, and THE URL IS NEVER STORED.
 */

import XCTest
@testable import YtdlMac

final class ProfileTests: XCTestCase {
    private var home = ""

    override func setUpWithError() throws {
        /* The store writes to ~/Library/Application Support/ytdl-macos.
         * Pointing HOME at a temp directory keeps the tests off the real
         * profiles of whoever runs them. */
        home = try FixtureSupport.makeTempDir(prefix: "ytdl-macos-profiles")
        Paths.environmentOverride = ["HOME": home]
    }

    override func tearDownWithError() throws {
        Paths.environmentOverride = nil
        try? FileManager.default.removeItem(atPath: home)
    }

    private func sampleOptions() -> RunOptions {
        var o = RunOptions()
        o.url = "https://example.com/should-not-be-stored"
        o.mode = "audio-only"
        o.quality = "1080"
        o.container = "webm"
        o.workers = 4
        o.sync = true
        o.noComments = true
        o.ytdlpArgs = ["--match-filter", "duration > 60"]
        return o
    }

    func testSaveThenReloadRoundTrips() throws {
        let o = sampleOptions()

        let store = ProfileStore.load()
        XCTAssertTrue(store.profiles.isEmpty)
        try store.save(name: "Archival", opts: o)
        XCTAssertEqual(store.active, "Archival")

        // A fresh load: this is the bit that proves it reached the disk.
        let again = ProfileStore.load()
        XCTAssertEqual(again.profiles.count, 1)
        XCTAssertEqual(again.active, "Archival")

        let p = try XCTUnwrap(again.profile(named: "Archival"))
        XCTAssertEqual(p.opts.mode, "audio-only")
        XCTAssertEqual(p.opts.quality, "1080")
        XCTAssertEqual(p.opts.container, "webm")
        XCTAssertEqual(p.opts.workers, 4)
        XCTAssertTrue(p.opts.sync)
        XCTAssertTrue(p.opts.noComments)
        XCTAssertEqual(p.opts.ytdlpArgs, ["--match-filter", "duration > 60"])
        XCTAssertGreaterThan(p.saved, 0)
    }

    func testURLIsNeverStored() throws {
        let o = sampleOptions()
        let store = ProfileStore.load()
        try store.save(name: "P", opts: o)

        /* A preset that replaced what you were about to download would be the
         * one thing a preset must never do. */
        XCTAssertEqual(store.profile(named: "P")?.opts.url, "")
        XCTAssertEqual(ProfileStore.load().profile(named: "P")?.opts.url, "")

        /* The caller's own options are untouched -- saving a profile must not
         * clear the URL box the user is still working in. */
        XCTAssertEqual(o.url, "https://example.com/should-not-be-stored")
    }

    func testNamesAreCaseInsensitiveButKeepNewCapitalisation() throws {
        let o = sampleOptions()
        let store = ProfileStore.load()

        try store.save(name: "archival", opts: o)
        try store.save(name: "Archival", opts: o)

        // One profile, not two indistinguishable rows in a menu.
        XCTAssertEqual(store.profiles.count, 1)
        // Re-saving fixes the capitalisation rather than ignoring it.
        XCTAssertEqual(store.profiles[0].name, "Archival")
        XCTAssertNotNil(store.profile(named: "ARCHIVAL"))
    }

    func testDeleteClearsActiveWhenItWasActive() throws {
        let o = sampleOptions()
        let store = ProfileStore.load()

        try store.save(name: "A", opts: o)
        try store.save(name: "B", opts: o)
        XCTAssertEqual(store.active, "B")

        try store.delete(name: "B")
        /* Left pointing at a name that no longer exists, the menu would show a
         * selection that cannot be applied. */
        XCTAssertNil(store.active)
        XCTAssertEqual(store.profiles.count, 1)

        // Deleting the other one does not touch the (already cleared) selection.
        try store.delete(name: "a")
        XCTAssertTrue(store.profiles.isEmpty)
    }

    func testRename() throws {
        let o = sampleOptions()
        let store = ProfileStore.load()

        try store.save(name: "Old", opts: o)
        try store.rename(from: "Old", to: "New")
        XCTAssertNotNil(store.profile(named: "New"))
        XCTAssertNil(store.profile(named: "Old"))
        // The renamed profile was active, so the selection follows it.
        XCTAssertEqual(store.active, "New")

        /* Re-capitalising itself is a rename people actually do, and must not
         * collide with itself. */
        try store.rename(from: "New", to: "NEW")
        XCTAssertEqual(store.active, "NEW")

        try store.save(name: "Other", opts: o)
        XCTAssertThrowsError(try store.rename(from: "Other", to: "NEW"))
    }

    func testActivate() throws {
        let o = sampleOptions()
        let store = ProfileStore.load()
        try store.save(name: "A", opts: o)

        try store.activate(nil)
        XCTAssertNil(store.active)

        // Case-insensitive lookup, but the STORED capitalisation is kept.
        try store.activate("a")
        XCTAssertEqual(store.active, "A")

        /* Only reachable from a stale window, so it is an error rather than a
         * silent no-op. */
        XCTAssertThrowsError(try store.activate("nope"))
    }

    func testBadNamesAreRefused() throws {
        let o = sampleOptions()
        let store = ProfileStore.load()

        XCTAssertThrowsError(try store.save(name: "   ", opts: o))
        XCTAssertThrowsError(
            try store.save(name: String(repeating: "x", count: profileMaxNameLength + 1), opts: o)
        )

        // Trimmed, not rejected: a trailing space is a typo, not another name.
        try store.save(name: "  Padded  ", opts: o)
        XCTAssertNotNil(store.profile(named: "Padded"))
    }

    func testStaleActiveNameIsClearedOnLoad() throws {
        /* Hand-written profiles.json naming an active profile that is not in
         * the list. Left as-is, the menu would select nothing and report a
         * name. */
        try FixtureSupport.write(
            "{ \"active\": \"ghost\", \"profiles\": [] }",
            to: Paths.join(Paths.stateDir(), "profiles.json")
        )
        XCTAssertNil(ProfileStore.load().active)
    }

    func testHandWrittenURLIsDroppedOnLoad() throws {
        /* The URL is dropped on the way IN as well as on the way out, so a
         * profiles.json edited by hand cannot hijack a download. */
        try FixtureSupport.write("""
        { "active": null, "profiles": [ { "name": "H", "saved": 1,
          "opts": { "url": "https://evil.example/x", "mode": "audio-only" } } ] }
        """, to: Paths.join(Paths.stateDir(), "profiles.json"))

        let store = ProfileStore.load()
        let p = try XCTUnwrap(store.profile(named: "H"))
        XCTAssertEqual(p.opts.url, "")
        XCTAssertEqual(p.opts.mode, "audio-only")
    }

    func testMissingFileIsAnEmptyStore() {
        let store = ProfileStore.load()
        XCTAssertTrue(store.profiles.isEmpty)
        XCTAssertNil(store.active)
        XCTAssertNil(store.profile(named: "anything"))
    }

    // MARK: - The default profile a fresh install starts with

    func testFirstRunInstallsTheDefaultProfile() {
        XCTAssertTrue(ProfileStore.seedDefaultIfMissing())

        let store = ProfileStore.load()
        XCTAssertEqual(store.profiles.count, 1)
        XCTAssertEqual(store.profiles[0].name, defaultProfileName)
        XCTAssertGreaterThan(store.profiles[0].saved, 0)

        /* Nothing is SELECTED. The seeded profile is somewhere to go back to,
         * not a preset silently applied to a form the user has not touched
         * yet. */
        XCTAssertNil(store.active)
    }

    func testTheDefaultProfileSetsNothing() throws {
        /* It carries the app's own defaults, so applying it produces exactly
         * the command line a fresh form produces. A shipped profile that picked
         * a quality or a container would be this window deciding pipeline
         * policy, which is run_ytdlp.ps1's job on the far side of the
         * CLI_VERSION pin. */
        XCTAssertTrue(ProfileStore.seedDefaultIfMissing())

        let store = ProfileStore.load()
        let p = try XCTUnwrap(store.profile(named: defaultProfileName))

        let url = "https://example.com/watch?v=aaaaaaaaaaa"
        var fromProfile = p.opts
        fromProfile.url = url
        var fresh = RunOptions()
        fresh.url = url

        XCTAssertEqual(fromProfile.commandPreview(), fresh.commandPreview())
    }

    func testTheDefaultIsSeededOnceAndStaysDeleted() throws {
        XCTAssertTrue(ProfileStore.seedDefaultIfMissing())
        try ProfileStore.load().delete(name: defaultProfileName)

        /* profiles.json still exists -- now holding an empty list -- so this is
         * no longer a fresh install. A default that came back at every launch
         * would be a profile the user cannot get rid of. */
        XCTAssertFalse(ProfileStore.seedDefaultIfMissing())
        XCTAssertTrue(ProfileStore.load().profiles.isEmpty)
    }

    func testSeedingNeverTouchesAnExistingStore() throws {
        let store = ProfileStore.load()
        try store.save(name: "Mine", opts: sampleOptions())

        XCTAssertFalse(ProfileStore.seedDefaultIfMissing())

        let again = ProfileStore.load()
        XCTAssertEqual(again.profiles.count, 1)
        XCTAssertNotNil(again.profile(named: "Mine"))
        XCTAssertNil(again.profile(named: defaultProfileName))
    }

    func testACorruptStoreIsNotReplacedByTheDefault() throws {
        /* This is why the check is "is there a file" rather than "did it
         * parse". An unreadable profiles.json is still somebody's profiles --
         * half-written by a crash, mangled by an editor mid-save -- and
         * overwriting it with a default is the one recovery nobody can undo. */
        let path = Paths.join(Paths.stateDir(), "profiles.json")
        try FixtureSupport.write("{ not json", to: path)

        XCTAssertFalse(ProfileStore.seedDefaultIfMissing())
        XCTAssertEqual(try String(contentsOfFile: path, encoding: .utf8), "{ not json")
    }

    // MARK: - Settings and the atomic write beneath all of it

    func testSettingsRoundTripToApplicationSupport() {
        let s = Settings.load()
        XCTAssertEqual(s.dataRoot, "")
        XCTAssertEqual(s.defaultWorkers, 1)

        s.dataRoot = "~/Movies/yt-dlp"
        s.defaultWorkers = 3
        s.save()

        let path = Paths.join(Paths.stateDir(), "settings.json")
        XCTAssertTrue(Paths.isRegularFile(path), "settings.json is not in Application Support")

        let again = Settings.load()
        XCTAssertEqual(again.dataRoot, "~/Movies/yt-dlp")
        XCTAssertEqual(again.defaultWorkers, 3)
        // The stored value keeps its ~; only what leaves the process is expanded.
        XCTAssertTrue(again.resolvedDataRoot.hasPrefix("/"))
        XCTAssertTrue(again.resolvedDataRoot.hasSuffix("/Movies/yt-dlp"))
    }

    func testAtomicWriteReplacesAndLeavesNoTempBehind() throws {
        let path = Paths.join(home, "state/thing.json")
        XCTAssertTrue(AtomicFile.write(Data("{\"a\":1}".utf8), to: path))
        XCTAssertTrue(AtomicFile.write(Data("{\"a\":2}".utf8), to: path))

        XCTAssertEqual(try String(contentsOfFile: path, encoding: .utf8), "{\"a\":2}")
        XCTAssertFalse(FileManager.default.fileExists(atPath: path + ".tmp"))
    }

    /* The queue survives a restart, and a run left mid-flight by a window that
     * closed is resolved rather than left as a row that never finishes. */
    func testQueueAndHistorySurviveARestart() {
        var queued = RunRecord()
        queued.id = "q1"
        queued.state = "queued"
        queued.opts.url = "https://example.com/one"

        var interrupted = RunRecord()
        interrupted.id = "h1"
        interrupted.state = "running"

        Runner.writeRecords([queued], to: Runner.stateFile("queue.json"))
        Runner.writeRecords([interrupted], to: Runner.stateFile("history.json"))

        let runner = Runner()
        XCTAssertEqual(runner.queue.map { $0.id }, ["q1"])
        XCTAssertEqual(runner.queue.first?.opts.url, "https://example.com/one")
        XCTAssertEqual(runner.history.first?.state, "failed")
        XCTAssertTrue(runner.history.first?.lastLine.contains("Interrupted") ?? false)
    }
}
