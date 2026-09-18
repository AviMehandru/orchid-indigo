/* Watch state, resume points and playlists.
 *
 * The rules asserted here are the same ones linux-gtk/tests/test_userdata.c
 * and YtdlWin.Tests/UserDataTests.cs assert, on the same fixture values,
 * because three independent implementations of one contract is what this
 * repository is and the only thing that makes that survivable is asserting
 * the contract three times.
 *
 * The three that matter most:
 *   - A corrupt store is NOT silently replaced. The caches are rebuilt from
 *     the archive and losing one costs a rescan; losing this loses the fact
 *     that you watched something, which nothing can reconstruct. So a file
 *     that will not parse leaves the session read-only and stays on disk.
 *   - A record is NOT stamped with the manifest's archive_creation_time,
 *     which is exactly what VerifyCache does. "I have seen this" is a fact
 *     about the person, and `ytdl --refresh` fetching newer comments must not
 *     un-watch anything.
 *   - Finishing a video clears its resume point, and glancing at the first ten
 *     seconds does not create one. Both are the difference between a feature
 *     people use and one they turn off.
 */

import XCTest
@testable import YtdlMac

final class UserDataTests: XCTestCase {

    private var tmpdir = ""
    private var storePath: String { Paths.join(tmpdir, "userdata.json") }

    override func setUpWithError() throws {
        tmpdir = try FixtureSupport.makeTempDir(prefix: "ytdl-mac-userdata")
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(atPath: tmpdir)
    }

    private func store() -> UserData { UserData(path: storePath) }

    /// An entry with nothing in it but the fields these facets read. The
    /// watched and playlist facets are pure membership tests on the key, so
    /// they can be asserted without an archive on disk at all -- which is the
    /// point of keeping the filter free of the disk.
    private func bare(_ key: String) -> ArchiveEntry {
        ArchiveEntry(key: key, dir: "", rel: key, channel: "", title: key)
    }

    // MARK: - Watch state

    func testWatchedRoundTrips() {
        let ud = store()
        XCTAssertFalse(ud.isWatched("aaa"))

        ud.setWatched("aaa", true)
        XCTAssertTrue(ud.isWatched("aaa"))
        ud.save()

        XCTAssertTrue(store().isWatched("aaa"))
    }

    func testFinishingClearsTheResumePoint() {
        let ud = store()

        ud.setPosition("aaa", seconds: 400, duration: 1000)
        XCTAssertEqual(ud.position("aaa"), 400, accuracy: 0.001)
        XCTAssertFalse(ud.isWatched("aaa"))

        /* Past 90%: watched, and the resume point goes. Re-opening something
         * you finished should start it again, not drop you at the end card. */
        ud.setPosition("aaa", seconds: 950, duration: 1000)
        XCTAssertTrue(ud.isWatched("aaa"))
        XCTAssertEqual(ud.position("aaa"), 0, accuracy: 0.001)
    }

    func testGlanceIsNotAResumePoint() {
        let ud = store()
        ud.setPosition("aaa", seconds: 8, duration: 1000)
        XCTAssertEqual(ud.position("aaa"), 0, accuracy: 0.001)
        XCTAssertFalse(ud.isWatched("aaa"))

        /* And a glance does not un-watch something already seen. */
        ud.setWatched("aaa", true)
        ud.setPosition("aaa", seconds: 8, duration: 1000)
        XCTAssertTrue(ud.isWatched("aaa"))
    }

    func testUnknownDurationStoresThePositionAndInfersNothing() {
        let ud = store()
        ud.setPosition("aaa", seconds: 600, duration: 0)
        XCTAssertEqual(ud.position("aaa"), 600, accuracy: 0.001)
        XCTAssertFalse(ud.isWatched("aaa"))
    }

    func testMarkingWatchedByHandClearsThePosition() {
        let ud = store()
        ud.setPosition("aaa", seconds: 400, duration: 1000)
        ud.setWatched("aaa", true)
        XCTAssertEqual(ud.position("aaa"), 0, accuracy: 0.001)

        /* Marking it unwatched again does NOT restore one -- "I want to see
         * this again" and "start it over" are different wishes, and the
         * position is simply gone. */
        ud.setWatched("aaa", false)
        XCTAssertEqual(ud.position("aaa"), 0, accuracy: 0.001)
    }

    func testEmptyRecordsAreNotWritten() throws {
        let ud = store()
        /* Below the resume floor and not watched: the record says nothing, and
         * writing it would grow the file for every video anyone ever opened. */
        ud.setPosition("aaa", seconds: 3, duration: 1000)
        ud.setWatched("bbb", true)
        ud.save()

        let obj = try XCTUnwrap(JSONFile.object(at: storePath))
        let watch = try XCTUnwrap(obj.object("watch"))
        XCTAssertNil(watch["aaa"])
        XCTAssertNotNil(watch["bbb"])
    }

    func testWatchedKeysTracksEveryPath() {
        let ud = store()
        XCTAssertTrue(ud.watchedKeys.isEmpty)

        ud.setWatched("aaa", true)
        XCTAssertTrue(ud.watchedKeys.contains("aaa"))
        XCTAssertEqual(ud.watchedCount, 1)

        /* By finishing: crossing the threshold sets the flag from inside
         * setPosition, which is the path a hand-maintained set forgets. */
        ud.setPosition("bbb", seconds: 95, duration: 100)
        XCTAssertTrue(ud.watchedKeys.contains("bbb"))
        XCTAssertEqual(ud.watchedCount, 2)

        /* A resume point is not being watched. */
        ud.setPosition("ccc", seconds: 40, duration: 100)
        XCTAssertFalse(ud.watchedKeys.contains("ccc"))
        XCTAssertEqual(ud.watchedCount, 2)

        ud.setWatched("aaa", false)
        XCTAssertFalse(ud.watchedKeys.contains("aaa"))
        XCTAssertEqual(ud.watchedCount, 1)
    }

    // MARK: - The corrupt-store rule

    func testCorruptStoreIsReadOnlyAndPreserved() throws {
        try FixtureSupport.write("{ this is not json", to: storePath)

        let ud = store()
        XCTAssertTrue(ud.isReadOnly)

        /* The app still runs -- refusing to start over a malformed file would
         * lose the whole application rather than one file. */
        ud.setWatched("aaa", true)
        ud.save()

        /* THE POINT: the unparseable file is still there, byte for byte, for
         * its owner to look at. A cache would have been replaced; this is not
         * a cache. */
        let after = try String(contentsOfFile: storePath, encoding: .utf8)
        XCTAssertEqual(after, "{ this is not json")
    }

    func testMissingStoreIsNotReadOnly() {
        /* A file that is not there is the ordinary first-launch state. It must
         * NOT be treated as corrupt, or a fresh install could never save. */
        let ud = store()
        XCTAssertFalse(ud.isReadOnly)

        ud.setWatched("aaa", true)
        ud.save()
        XCTAssertTrue(store().isWatched("aaa"))
    }

    // MARK: - Playlists

    func testPlaylistCRUD() {
        let ud = store()
        XCTAssertTrue(ud.playlists.isEmpty)

        /* A blank name is refused: an unnamed playlist is unfindable. */
        XCTAssertNil(ud.createPlaylist(named: "   "))

        let pl = ud.createPlaylist(named: "Watch later")
        XCTAssertNotNil(pl)
        let id = pl!.id

        /* Duplicate names are the user's to make. */
        XCTAssertNotNil(ud.createPlaylist(named: "Watch later"))
        XCTAssertEqual(ud.playlists.count, 2)

        XCTAssertTrue(ud.addToPlaylist(id, key: "aaa"))
        /* Adding twice is a no-op, not a duplicate: a playlist is a set the
         * user ordered, not a bag. */
        XCTAssertFalse(ud.addToPlaylist(id, key: "aaa"))
        XCTAssertTrue(ud.playlistContains(id, key: "aaa"))
        XCTAssertEqual(ud.playlist(id)?.keys.count, 1)

        XCTAssertTrue(ud.renamePlaylist(id, to: "Tonight"))
        XCTAssertEqual(ud.playlist(id)?.name, "Tonight")
        XCTAssertFalse(ud.renamePlaylist(id, to: " "))

        XCTAssertTrue(ud.removeFromPlaylist(id, key: "aaa"))
        XCTAssertFalse(ud.removeFromPlaylist(id, key: "aaa"))

        XCTAssertTrue(ud.deletePlaylist(id))
        XCTAssertFalse(ud.deletePlaylist(id))
        XCTAssertEqual(ud.playlists.count, 1)
    }

    func testPlaylistsRoundTrip() {
        let ud = store()
        let pl = ud.createPlaylist(named: "Rail history")!
        ud.addToPlaylist(pl.id, key: "aaa")
        ud.addToPlaylist(pl.id, key: "bbb")
        ud.save()

        let again = store()
        XCTAssertEqual(again.playlists.count, 1)
        /* Order is the user's, so it is preserved rather than sorted. */
        XCTAssertEqual(again.playlist(pl.id)?.keys, ["aaa", "bbb"])
        XCTAssertEqual(again.playlist(pl.id)?.name, "Rail history")
    }

    // MARK: - The facets the store feeds

    func testUnwatchedFacet() {
        var f = LibraryFilter()
        f.flags = [.unwatched]

        let e = bare("aaa")
        XCTAssertTrue(f.matches(e))

        f.watchedKeys = ["aaa"]
        XCTAssertFalse(f.matches(e))

        /* With the facet off, being watched is irrelevant again. */
        f.flags = []
        XCTAssertTrue(f.matches(e))
    }

    func testPlaylistFacetCountAndResetAsymmetry() {
        var f = LibraryFilter()
        let e = bare("aaa")

        /* nil means no playlist filter. */
        XCTAssertTrue(f.matches(e))
        XCTAssertEqual(f.facetCount, 0)

        /* An EMPTY playlist shows nothing rather than everything. */
        f.playlistKeys = []
        XCTAssertFalse(f.matches(e))
        XCTAssertEqual(f.facetCount, 1)

        f.playlistKeys = ["aaa"]
        XCTAssertTrue(f.matches(e))

        /* reset() drops the playlist -- it is a filter -- but must NOT drop
         * watchedKeys, which is the store's answer about the person. */
        f.watchedKeys = ["aaa"]
        f.reset()
        XCTAssertNil(f.playlistKeys)
        XCTAssertEqual(f.watchedKeys, ["aaa"])
    }

    func testEveryFlagHasALabel() {
        /* A flag added to the enum and not named would be a blank checkbox
         * nobody notices. */
        for flag in FacetFlags.all {
            XCTAssertFalse(flag.label.isEmpty)
        }
        XCTAssertEqual(FacetFlags.all.count, 5)
    }
}
