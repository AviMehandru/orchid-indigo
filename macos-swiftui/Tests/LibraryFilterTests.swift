/* Library sorting, faceting, and the verification cache.
 *
 * THE FIXTURE IS THE SAME ONE linux-gtk/tests/test_library.c builds, value for
 * value -- the same six videos, the same channels, dates, modes, sizes and
 * creation stamps. That is deliberate and is the whole point: when this file
 * and that one both pass, the Swift and C rule sets have been shown to agree
 * on the same input rather than each being self-consistent. It is the same
 * mitigation the probe's derivation uses, and the only thing that makes three
 * blind implementations of one contract survivable.
 *
 * Most of what is asserted below is not "does the filter filter". It is the
 * handful of decisions that are invisible in a screenshot and wrong in a way
 * nobody reports:
 *
 *   - an empty channel set means EVERY channel, not none, or the grid goes
 *     blank the first time someone opens the facet list and unticks the one
 *     thing they had ticked;
 *   - a video whose uploadDate is not eight digits is EXCLUDED by a date
 *     bound rather than kept, because it has no date and claiming it falls
 *     inside a range invents one;
 *   - the sort is TOTAL, so equal-keyed videos do not swap places between
 *     rebuilds;
 *   - a missing title or date sorts last in BOTH directions, so flipping the
 *     order does not fill the first screen with blanks;
 *   - the "failed verification" facet never shows a video nobody has checked;
 *   - and a cached verification result does not survive the folder being
 *     rewritten underneath it, which is exactly what `ytdl --refresh` does.
 *
 * What is NOT asserted, and must not be: a specific collation of two present
 * titles. This app uses the system's localized comparison and the other two
 * use their platforms'. The inputs below are chosen so that every reasonable
 * collation orders them the same way.
 */

import XCTest
@testable import YtdlMac

final class LibraryFilterTests: XCTestCase {

    private var tmpdir = ""
    private var root = ""
    private var index = ArchiveIndex()

    override func setUpWithError() throws {
        tmpdir = try FixtureSupport.makeTempDir(prefix: "ytdl-mac-library")
        Paths.environmentOverride = ["HOME": tmpdir]

        root = Paths.join(tmpdir, "Complete Archive")
        try FixtureSupport.mkdirp(root)

        try makeVideo(channel: "Alpha Channel", name: "alpha", title: "Zulu title",
                      date: "20240131", mode: "full",
                      mediaRel: "Final files/Final Video.mkv", bytes: 600,
                      layout: 2, stamp: "2026-01-01T00:00:00Z", refreshes: 0)
        try makeVideo(channel: "Alpha Channel", name: "bravo", title: "Yankee title",
                      date: "20230615", mode: "full",
                      mediaRel: "Final files/Final Video.mkv", bytes: 200,
                      layout: 2, stamp: "2026-01-02T00:00:00Z", refreshes: 0)
        try makeVideo(channel: "Bravo Channel", name: "charlie", title: "Xray title",
                      date: "20251102", mode: "audio-only",
                      mediaRel: "Final files/Final Audio.opus", bytes: 100,
                      layout: 2, stamp: "2026-01-03T00:00:00Z", refreshes: 2)
        try makeVideo(channel: "Bravo Channel", name: "delta", title: "Whiskey title",
                      date: "20220308", mode: "comments-only",
                      mediaRel: nil, bytes: 0,
                      layout: 2, stamp: "2026-01-04T00:00:00Z", refreshes: 0)
        try makeVideo(channel: "Bravo Channel", name: "echo", title: "Victor title",
                      date: "20240601", mode: "full",
                      mediaRel: "Final files/Final Video.mkv", bytes: 50,
                      layout: 3, stamp: "2026-01-05T00:00:00Z", refreshes: 0)
        try makeVideo(channel: "Charlie Channel", name: "foxtrot", title: "Uniform title",
                      date: "", mode: "full",
                      mediaRel: "Final files/Final Video.mkv", bytes: 10,
                      layout: 2, stamp: "2026-01-06T00:00:00Z", refreshes: 0)

        index = try ArchiveIndex.scan(root: root)
        XCTAssertEqual(index.entries.count, 6)
    }

    override func tearDownWithError() throws {
        Paths.environmentOverride = nil
        try? FileManager.default.removeItem(atPath: tmpdir)
    }

    // MARK: - Fixture

    /* The FOLDER NAME is built in the documented
     * "<uploader> - <YYYYMMDD> - <id> - <title>" form, and the id is padded to
     * exactly 11 base64url characters, neither of which is decoration. The
     * folder-name parse anchors on "8 digits followed by an 11-character id",
     * so a fixture with a short slug does not parse -- the reader then falls
     * back to using the whole folder name as the title, and the suite ends up
     * asserting a sort over folder names while appearing to assert one over
     * titles. An empty date produces a name that deliberately does not parse,
     * which is how a video with no usable uploadDate is made. */
    private func makeVideo(channel: String, name: String, title: String,
                           date: String, mode: String, mediaRel: String?,
                           bytes: Int, layout: Int, stamp: String,
                           refreshes: Int) throws {
        let id = name.padding(toLength: 11, withPad: "_", startingAt: 0)
        let folder = date.isEmpty
            ? "Undated \(title)"
            : "\(channel) - \(date) - \(id) - \(title)"

        let vdir = Paths.join(Paths.join(root, channel), folder)
        try FixtureSupport.mkdirp(vdir)

        var history = ""
        if refreshes > 0 {
            let records = (0..<refreshes).map {
                "{\"time\": \"2026-0\($0 + 1)-01\", \"mode\": \"comments-only\"}"
            }
            history = ",\n  \"refresh_history\": [\(records.joined(separator: ", "))]"
        }

        let mediaField = mediaRel.map { "\"\($0)\"" } ?? "null"
        let manifest = """
        {
          "archive_layout_version": \(layout),
          "archive_creation_time": "\(stamp)",
          "video_id": "\(id)",
          "title": "\(title)",
          "uploader": "\(channel)",
          "upload_date": "\(date)",
          "download_mode": "\(mode)",
          "media_file": \(mediaField)\(history)
        }
        """
        try FixtureSupport.write(manifest,
                                 to: Paths.join(vdir, "Video metadata/manifest.json"))

        if let mediaRel {
            try FixtureSupport.write(String(repeating: "x", count: bytes),
                                     to: Paths.join(vdir, mediaRel))
        }
    }

    /// The short name a video was made under, recovered from its padded id --
    /// the shortest readable way to assert an ordering.
    private func shortName(_ e: ArchiveEntry) -> String {
        guard let id = e.videoID else { return "(no id)" }
        var s = id
        while s.hasSuffix("_") { s.removeLast() }
        return s
    }

    private func names(_ got: [ArchiveEntry]) -> [String] {
        got.map { shortName($0) }
    }

    // MARK: - Sorting

    func testDefaultIsNewestFirst() throws {
        let f = LibraryFilter()
        XCTAssertEqual(f.sort, .date)
        XCTAssertTrue(f.descending)

        let got = names(f.apply(to: index.entries))
        XCTAssertEqual(got.count, 6)
        XCTAssertEqual(Array(got.prefix(5)),
                       ["charlie", "echo", "alpha", "bravo", "delta"])

        /* No date at all, and therefore LAST -- not first, which is where a
         * plain reversed string compare would put an empty string. */
        XCTAssertEqual(got.last, "foxtrot")
    }

    func testMissingFieldSortsLastBothWays() throws {
        /* The half of the rule above that is easy to get wrong. If "missing
         * sorts last" were implemented by letting the empty value compare
         * naturally, flipping the direction would move every dateless video to
         * the TOP -- which means the first screen after a flip is blanks. */
        var f = LibraryFilter()
        f.descending = false

        let got = names(f.apply(to: index.entries))
        XCTAssertEqual(got.first, "delta")
        XCTAssertEqual(got.last, "foxtrot")
    }

    func testEveryKeySorts() throws {
        let cases: [(SortKey, String)] = [
            (.title, "alpha"),     // "Zulu title"
            (.channel, "foxtrot"), // "Charlie Channel"
            (.size, "alpha"),      // 600 bytes of media
        ]
        for (key, expected) in cases {
            var f = LibraryFilter()
            f.sort = key
            f.descending = true
            XCTAssertEqual(names(f.apply(to: index.entries)).first, expected,
                           "descending \(key.rawValue) should start at \(expected)")
        }
    }

    func testSortIsTotal() throws {
        /* Nothing in the fixture has a duration -- there are no info.json
         * files -- so every entry ties on the primary key. The comparison must
         * still be a strict order, or the grid reshuffles equal videos between
         * rebuilds and reads as a rendering bug. */
        var f = LibraryFilter()
        f.sort = .duration

        let a = f.apply(to: index.entries)
        let b = f.apply(to: index.entries)
        XCTAssertEqual(names(a), names(b))

        for i in 0..<(a.count - 1) {
            XCTAssertNotEqual(f.compare(a[i], a[i + 1]), 0,
                              "no two distinct videos may compare equal")
        }
    }

    func testSortIDsRoundTrip() throws {
        /* The setting is persisted by ID, never by the case's position:
         * inserting a key in the middle would otherwise silently change what
         * every saved setting means. */
        for key in SortKey.allCases {
            XCTAssertEqual(SortKey.from(id: key.rawValue), key)
            XCTAssertFalse(key.label.isEmpty)
        }
        /* A key written by a newer build falls back rather than refusing. */
        XCTAssertEqual(SortKey.from(id: "popularity"), .date)
        XCTAssertEqual(SortKey.from(id: nil), .date)
    }

    // MARK: - Facets

    func testEmptyChannelSetMeansAll() throws {
        let f = LibraryFilter()
        XCTAssertTrue(f.channels.isEmpty)
        XCTAssertEqual(f.apply(to: index.entries).count, 6)
        XCTAssertFalse(f.isNarrowing)
    }

    func testChannelFacetIsAUnion() throws {
        var f = LibraryFilter()
        f.setChannel("Alpha Channel", on: true)
        XCTAssertEqual(f.apply(to: index.entries).count, 2)

        /* Two channels is MORE videos, not fewer: within one facet the
         * selections are an OR. Between facets they are an AND. Getting that
         * backwards makes every multi-select facet show nothing. */
        f.setChannel("Bravo Channel", on: true)
        XCTAssertEqual(f.apply(to: index.entries).count, 5)

        /* A Set makes ticking twice idempotent by construction, which is the
         * property the C version has to maintain by hand. */
        f.setChannel("Bravo Channel", on: true)
        XCTAssertEqual(f.channels.count, 2)
        f.setChannel("Bravo Channel", on: false)
        XCTAssertFalse(f.channels.contains("Bravo Channel"))
    }

    func testDateRangeExcludesUndated() throws {
        var f = LibraryFilter()
        f.dateFrom = "20230101"
        f.dateTo = "20241231"

        let got = Set(names(f.apply(to: index.entries)))
        XCTAssertEqual(got, ["alpha", "bravo", "echo"])

        /* The one that matters. foxtrot has no usable date, so it is not
         * inside this range -- it is a video whose date is unknown, and
         * keeping it would be inventing one. */
        XCTAssertFalse(got.contains("foxtrot"))

        /* Bounds are inclusive at both ends. */
        f.dateFrom = "20240131"
        f.dateTo = "20240131"
        XCTAssertEqual(names(f.apply(to: index.entries)), ["alpha"])
    }

    func testFlagFacets() throws {
        let cases: [(FacetFlags, String)] = [
            (.audioOnly, "charlie"),
            (.noMedia, "delta"),
            (.layoutTooNew, "echo"),
        ]
        for (flag, expected) in cases {
            var f = LibraryFilter()
            f.flags = flag
            XCTAssertEqual(names(f.apply(to: index.entries)), [expected])
        }
    }

    func testAudioOnlyIsNotMediaLess() throws {
        /* Two different states that a reader keying off "no Final Video.mkv"
         * would collapse into one. charlie has media; delta does not. */
        var f = LibraryFilter()
        f.flags = [.audioOnly, .noMedia]
        XCTAssertTrue(f.apply(to: index.entries).isEmpty)
    }

    func testFacetCountAndReset() throws {
        var f = LibraryFilter()
        f.sort = .size
        f.descending = false
        f.setChannel("Alpha Channel", on: true)
        f.dateFrom = "20230101"
        f.dateTo = "20241231"
        f.flags = [.audioOnly]
        f.needle = "zulu"

        /* A date RANGE is one facet, not two: it is a single idea the user
         * had, and counting it twice makes the count overstate the
         * narrowing. */
        XCTAssertEqual(f.facetCount, 3)
        XCTAssertTrue(f.isNarrowing)

        f.reset()
        XCTAssertEqual(f.facetCount, 0)
        XCTAssertFalse(f.isNarrowing)

        /* The sort survives a reset on purpose: it is a view preference, not a
         * filter, and throwing it away would be a surprise. */
        XCTAssertEqual(f.sort, .size)
        XCTAssertFalse(f.descending)
    }

    func testNeedleStillMatchesFourFields() throws {
        var f = LibraryFilter()
        f.needle = "BRAVO CHANNEL"
        /* Case-folded, and matching the uploader as well as the title -- the
         * behaviour that was already there and must not regress. */
        XCTAssertEqual(f.apply(to: index.entries).count, 3)
    }

    // MARK: - Verification facet and cache

    private func entry(_ name: String) throws -> ArchiveEntry {
        try XCTUnwrap(index.entries.first { shortName($0) == name })
    }

    func testVerifyFacetIgnoresUnchecked() throws {
        let cache = VerifyCache(path: Paths.join(tmpdir, "verify.json"))
        XCTAssertEqual(cache.knownCount, 0)

        var f = LibraryFilter()
        f.flags = [.verifyFailed]

        /* Nothing has been verified, so the facet shows nothing. It must NOT
         * show everything: "these failed" and "these might have failed" are
         * different claims and only one of them is true here. */
        XCTAssertTrue(f.apply(to: index.entries, verify: cache.state(for:)).isEmpty)

        cache.set(.failed, for: try entry("alpha"))
        cache.set(.ok, for: try entry("bravo"))

        XCTAssertEqual(names(f.apply(to: index.entries, verify: cache.state(for:))),
                       ["alpha"])
        XCTAssertEqual(cache.knownCount, 2)
    }

    func testVerifyResultDoesNotSurviveARefresh() throws {
        /* The reason every record carries the manifest's
         * archive_creation_time. `ytdl --refresh` rewrites a folder's
         * sidecars, its hashes and -- when it re-embeds the info.json -- the
         * media file itself, all without changing download_mode. A cached
         * "verifies" from before that is not a stale opinion, it is a wrong
         * one. */
        let cache = VerifyCache(path: Paths.join(tmpdir, "verify.json"))
        let alpha = try entry("alpha")
        XCTAssertEqual(alpha.creationStamp, "2026-01-01T00:00:00Z")

        cache.set(.ok, for: alpha)
        XCTAssertEqual(cache.state(for: alpha), .ok)

        /* Rewrite the folder the way a refresh would: a new creation stamp and
         * a refresh_history, with download_mode and media_file preserved. */
        try makeVideo(channel: "Alpha Channel", name: "alpha", title: "Zulu title",
                      date: "20240131", mode: "full",
                      mediaRel: "Final files/Final Video.mkv", bytes: 600,
                      layout: 2, stamp: "2026-05-05T00:00:00Z", refreshes: 1)

        let after = try ArchiveIndex.scan(root: root)
        let again = try XCTUnwrap(after.entries.first { shortName($0) == "alpha" })
        XCTAssertEqual(again.refreshCount, 1)
        XCTAssertEqual(again.downloadMode, "full")

        /* Same key, same video, different folder -- so the old result is
         * gone. */
        XCTAssertEqual(again.key, alpha.key)
        XCTAssertEqual(cache.state(for: again), .unknown)
    }

    func testVerifyCacheRoundTrips() throws {
        let path = Paths.join(tmpdir, "verify.json")
        let alpha = try entry("alpha")

        let first = VerifyCache(path: path)
        first.set(.failed, for: alpha)
        first.save()

        let reloaded = VerifyCache(path: path)
        XCTAssertEqual(reloaded.state(for: alpha), .failed)
    }

    func testCorruptCacheIsEmptyNotFatal() throws {
        let path = Paths.join(tmpdir, "verify.json")
        try FixtureSupport.write("{ this is not json", to: path)

        /* A cache is worth one re-verify. There is no version of "refuse to
         * open the Library because a cache file is malformed" that is the
         * right call. */
        let cache = VerifyCache(path: path)
        XCTAssertEqual(cache.knownCount, 0)
    }

    func testTotalSizeCountsTheFolder() throws {
        /* The whole folder, not just the media file: that is what it costs on
         * the disk it is sitting on, which is the question someone sorting by
         * size is asking. So it is strictly greater than the media alone --
         * the manifest is in there too. */
        XCTAssertGreaterThan(try entry("alpha").totalSize, 600)
    }
}
