/* The conformance test docs/archive-layout.md asks every out-of-repo consumer
 * to keep: build a fixture tree in the documented shape and assert this reader
 * finds it.
 *
 * The point is not coverage. It is that a layout bump in the pipeline becomes a
 * FAILING BUILD here rather than a user reporting an empty library. This app's
 * tests passing and the pipeline's tests passing says nothing about the two
 * agreeing; only a fixture in the documented shape does.
 *
 * Every case below is a state the contract names as ordinary and that a naive
 * reader gets wrong -- particularly the layout 1 -> 2 changes, where matching on
 * ".mkv" was correct and is now a bug. The thirteen the GTK suite carries are
 * the floor, not the target; the last two here are macOS's own.
 */

import XCTest
@testable import YtdlMac

final class ArchiveConformanceTests: XCTestCase {
    var tmpdir: String = ""
    /// The fake "Complete Archive".
    var root: String = ""

    override func setUpWithError() throws {
        tmpdir = try FixtureSupport.makeTempDir(prefix: "ytdl-macos-archive")
        root = Paths.join(tmpdir, "Youtube Videos/Complete Archive")
        try FixtureSupport.mkdirp(root)
    }

    override func tearDownWithError() throws {
        Paths.environmentOverride = nil
        try? FileManager.default.removeItem(atPath: tmpdir)
    }

    // MARK: - Fixture

    /// One video folder in the documented shape. `manifestExtra` is spliced into
    /// manifest.json; `info` is written verbatim as the info.json, or nil to
    /// exercise the folder-name fallback.
    @discardableResult
    private func makeVideo(
        channel: String,
        folder: String,
        manifestExtra: String? = nil,
        info: String? = nil
    ) throws -> String {
        let vdir = Paths.join(Paths.join(root, channel), folder)
        try FixtureSupport.mkdirp(vdir)

        let extra = manifestExtra.map { ",\n  \($0)" } ?? ""
        let manifest = """
        {
          "archive_layout_version": 2,
          "video_id": "dQw4w9WgXcQ",
          "title": "from the manifest",
          "uploader": "\(channel)"\(extra)
        }
        """
        try FixtureSupport.write(
            manifest, to: Paths.join(vdir, "Video metadata/manifest.json")
        )
        if let info {
            try FixtureSupport.write(
                info, to: Paths.join(vdir, "Video metadata/Video.info.json")
            )
        }
        return vdir
    }

    private func scan() throws -> ArchiveIndex {
        try ArchiveIndex.scan(root: root)
    }

    private func onlyEntry(_ index: ArchiveIndex) throws -> ArchiveEntry {
        XCTAssertEqual(index.entries.count, 1)
        return try XCTUnwrap(index.entries.first)
    }

    // MARK: - Cases

    func testDiscoversContractLayout() throws {
        let v = try makeVideo(
            channel: "Rick Astley",
            folder: "Rick Astley - 20091025 - dQw4w9WgXcQ - Never Gonna Give You Up",
            manifestExtra: "\"media_file\": \"Final files/Final Video.mkv\""
        )
        try FixtureSupport.write(
            "not really a video", to: Paths.join(v, "Final files/Final Video.mkv")
        )

        let index = try scan()
        let e = try onlyEntry(index)

        XCTAssertEqual(e.channel, "Rick Astley")
        XCTAssertEqual(e.videoID, "dQw4w9WgXcQ")
        XCTAssertEqual(e.uploadDate, "20091025")
        XCTAssertNotNil(e.mediaIndex)

        // The key round-trips: it is the only handle the UI ever holds.
        XCTAssertEqual(index.entry(forKey: e.key)?.key, e.key)

        XCTAssertEqual(index.videoCount, 1)
        XCTAssertEqual(index.channelCount, 1)
    }

    func testFolderNameFallback() throws {
        // No info.json at all -- the documented fallback, and a real state.
        try makeVideo(
            channel: "Some Channel",
            folder: "Some Channel - 20240101 - abcdefghijk - A Title - With Dashes"
        )

        let e = try onlyEntry(try scan())
        XCTAssertEqual(e.videoID, "abcdefghijk")
        XCTAssertEqual(e.uploadDate, "20240101")
        /* The title keeps its own " - ", which a naive four-field split would
         * truncate to "A Title". */
        XCTAssertEqual(e.title, "A Title - With Dashes")
    }

    func testMediaFoundWhateverContainer() throws {
        for (i, ext) in ["mkv", "mp4", "webm"].enumerated() {
            /* No media_file in the manifest: this is the GLOB path, which is the
             * one that was wrong under layout 1. */
            let v = try makeVideo(
                channel: "Chan",
                folder: "Chan - 2024010\(i + 1) - abcdefghij\(i) - T"
            )
            try FixtureSupport.write("x", to: Paths.join(v, "Final files/Final Video.\(ext)"))
        }

        let index = try scan()
        XCTAssertEqual(index.entries.count, 3)
        for e in index.entries {
            XCTAssertNotNil(e.mediaIndex, "no media found for \(e.rel)")
        }
    }

    func testAudioOnlyDownload() throws {
        /* Layout 2's headline change: Final AUDIO, and an extension a
         * video-only reader would never glob for. */
        let v = try makeVideo(
            channel: "Chan",
            folder: "Chan - 20240101 - abcdefghijk - T",
            manifestExtra: "\"download_mode\": \"audio-only\""
        )
        try FixtureSupport.write("x", to: Paths.join(v, "Final files/Final Audio.opus"))

        let e = try onlyEntry(try scan())
        let mi = try XCTUnwrap(e.mediaIndex)
        XCTAssertEqual(e.files[mi].ext, ".opus")
        XCTAssertEqual(e.downloadMode, "audio-only")
    }

    func testNoMediaIsValid() throws {
        /* --mode metadata-only writes a complete folder with no media in it.
         * The entry must EXIST and report nil, not be hidden and not be an
         * error. */
        try makeVideo(
            channel: "Chan",
            folder: "Chan - 20240101 - abcdefghijk - T",
            manifestExtra: "\"download_mode\": \"metadata-only\", \"media_file\": null"
        )

        let e = try onlyEntry(try scan())
        XCTAssertNil(e.mediaIndex)
        XCTAssertEqual(e.downloadMode, "metadata-only")
    }

    func testSkipsPreMergeStreams() throws {
        /* --keep-video leaves video-only and audio-only files behind. Picking
         * one gives a silent video or a black audio track, so the folder is
         * skipped outright -- and the format-id name is refused even in Final
         * files/. */
        let v = try makeVideo(channel: "Chan", folder: "Chan - 20240101 - abcdefghijk - T")
        try FixtureSupport.write(
            "silent video", to: Paths.join(v, "Pre-merge streams/Final Video.f137.mp4")
        )
        try FixtureSupport.write(
            "audio only", to: Paths.join(v, "Final files/Final Video.f251.webm")
        )

        XCTAssertNil(try onlyEntry(try scan()).mediaIndex)

        // Add the real merged output and it wins.
        try FixtureSupport.write(
            "the actual video", to: Paths.join(v, "Final files/Final Video.mkv")
        )
        let e = try onlyEntry(try scan())
        let mi = try XCTUnwrap(e.mediaIndex)
        XCTAssertEqual(e.files[mi].rel, "Final files/Final Video.mkv")
    }

    func testVideoWinsOverAudio() throws {
        let v = try makeVideo(channel: "Chan", folder: "Chan - 20240101 - abcdefghijk - T")
        /* Written in an order where a directory listing could hand back either
         * first; the choice must not depend on that. */
        try FixtureSupport.write("a", to: Paths.join(v, "Final files/Final Audio.m4a"))
        try FixtureSupport.write("v", to: Paths.join(v, "Final files/Final Video.mp4"))

        let e = try onlyEntry(try scan())
        let mi = try XCTUnwrap(e.mediaIndex)
        XCTAssertEqual(e.files[mi].ext, ".mp4")
    }

    func testManifestMediaFileWins() throws {
        /* The contract says prefer media_file over globbing. Here globbing would
         * find the .mkv; the manifest names the .mp4. */
        let v = try makeVideo(
            channel: "Chan",
            folder: "Chan - 20240101 - abcdefghijk - T",
            manifestExtra: "\"media_file\": \"Final files/Final Video.mp4\""
        )
        try FixtureSupport.write("x", to: Paths.join(v, "Final files/Final Video.mkv"))
        try FixtureSupport.write("y", to: Paths.join(v, "Final files/Final Video.mp4"))

        let e = try onlyEntry(try scan())
        let mi = try XCTUnwrap(e.mediaIndex)
        XCTAssertEqual(e.files[mi].rel, "Final files/Final Video.mp4")
    }

    func testMissingLayoutVersionIsLayoutOne() throws {
        /* An absent field means the video predates versioning, which the
         * contract defines as layout 1 -- readable, not an error. */
        let vdir = Paths.join(root, "Chan/Chan - 20240101 - abcdefghijk - T")
        try FixtureSupport.write(
            "{ \"video_id\": \"abcdefghijk\" }",
            to: Paths.join(vdir, "Video metadata/manifest.json")
        )
        try FixtureSupport.write("x", to: Paths.join(vdir, "Final files/Final Video.mkv"))

        let e = try onlyEntry(try scan())
        XCTAssertEqual(e.layoutVersion, 0)
        XCTAssertFalse(e.layoutTooNew)
        XCTAssertNotNil(e.mediaIndex)
    }

    func testFlagsNewerLayout() throws {
        let vdir = Paths.join(root, "Chan/Chan - 20240101 - abcdefghijk - T")
        try FixtureSupport.write(
            "{ \"archive_layout_version\": \(supportedArchiveLayout + 1) }",
            to: Paths.join(vdir, "Video metadata/manifest.json")
        )

        /* Flagged, but still PRESENT. An empty library with no explanation is
         * the outcome the whole contract exists to prevent. */
        XCTAssertTrue(try onlyEntry(try scan()).layoutTooNew)
    }

    func testChannelInfoIsNotAVideo() throws {
        try makeVideo(channel: "Chan", folder: "Chan - 20240101 - abcdefghijk - T")
        try FixtureSupport.write(
            "not a video", to: Paths.join(root, "Chan/Channel Info/avatar.png")
        )

        XCTAssertEqual(try scan().entries.count, 1)
    }

    func testPathForIndexStaysInside() throws {
        let v = try makeVideo(channel: "Chan", folder: "Chan - 20240101 - abcdefghijk - T")
        try FixtureSupport.write("x", to: Paths.join(v, "Final files/Final Video.mkv"))

        let e = try onlyEntry(try scan())
        let mi = try XCTUnwrap(e.mediaIndex)
        let path = try XCTUnwrap(e.path(forIndex: mi))
        XCTAssertTrue(path.hasPrefix(e.dir))
        XCTAssertTrue(Paths.isRegularFile(path))

        // Out of range returns nil rather than reading past the array.
        XCTAssertNil(e.path(forIndex: e.files.count + 99))
        XCTAssertNil(e.path(forIndex: -1))
    }

    func testScanReportsAMissingRoot() {
        XCTAssertThrowsError(
            try ArchiveIndex.scan(root: Paths.join(tmpdir, "no-such-archive"))
        )
    }

    /* Not a fixture test: pure parsing, and the case that motivated anchoring
     * the parse on the date+id shape rather than splitting into four fields. */
    func testFolderNameParsing() throws {
        let parsed = try XCTUnwrap(FolderName.parse("A - B - 20240101 - abcdefghijk - T - U"))
        XCTAssertEqual(parsed.uploader, "A - B")
        XCTAssertEqual(parsed.uploadDate, "20240101")
        XCTAssertEqual(parsed.id, "abcdefghijk")
        XCTAssertEqual(parsed.title, "T - U")

        /* Nothing resembling a date and an id: the caller falls back to the
         * whole folder name, so this must say so rather than half-fill. */
        XCTAssertNil(FolderName.parse("just a folder"))
    }

    /* CLI_VERSION's REQUIRES_ARCHIVE_LAYOUT and Archive.swift's
     * supportedArchiveLayout are two copies of one fact. The pin is what an
     * installer and a human read; the constant is what actually decides, per
     * video, whether a folder is rendered or flagged.
     *
     * ONE NUMBER, ALL THREE APPS -- CLI_VERSION belongs to the repository, not
     * to this app, and the GTK suite asserts against the same line. */
    func testPinMatchesSupportedLayout() throws {
        let path = try XCTUnwrap(
            FixtureSupport.repositoryFile(named: "CLI_VERSION"),
            "CLI_VERSION not found; the test target's resource reference is missing"
        )
        let pin = try XCTUnwrap(CLIVersion.load(atPath: path))
        let declared = try XCTUnwrap(pin.requiresArchiveLayout)

        XCTAssertGreaterThan(declared, 0)
        XCTAssertEqual(declared, supportedArchiveLayout)
    }

    // MARK: - macOS's own two

    /* The key is not local to this app: it is a hash of the archive-relative
     * folder path, and the GTK app computes the same one for the same folder.
     * Pinned to a literal so a "harmless" change to the hashing -- a different
     * truncation, a normalisation, hashing the absolute path -- fails here
     * rather than silently giving this app its own private addressing. */
    func testKeyMatchesTheOtherApps() {
        XCTAssertEqual(
            Paths.key(for: "Rick Astley/Rick Astley - 20091025 - dQw4w9WgXcQ - Never Gonna Give You Up"),
            "e3732acf5aa3ba15"
        )
        XCTAssertEqual(Paths.key(for: "Chan/Chan - 20240101 - abcdefghijk - T"),
                       "e75480efc59e3e13")
        // Windows-written manifests spell the separator the other way.
        XCTAssertEqual(Paths.key(for: "Chan\\Chan - 20240101 - abcdefghijk - T"),
                       Paths.key(for: "Chan/Chan - 20240101 - abcdefghijk - T"))
    }

    /* Derived state goes to this app's OWN directories under ~/Library. A
     * shared cache would mean three readers with three index formats each
     * treating the others' files as corrupt, and a state directory under
     * Caches would mean macOS deleting the user's profiles when the disk
     * fills. */
    func testCacheAndStateAreSeparateAndOurs() {
        Paths.environmentOverride = ["HOME": tmpdir]
        defer { Paths.environmentOverride = nil }

        XCTAssertEqual(Paths.cacheDir(), tmpdir + "/Library/Caches/ytdl-macos")
        XCTAssertEqual(Paths.stateDir(), tmpdir + "/Library/Application Support/ytdl-macos")
        XCTAssertNotEqual(Paths.cacheDir(), Paths.stateDir())
        XCTAssertFalse(Paths.stateDir().contains("/Caches/"))
        XCTAssertFalse(Paths.cacheDir().contains("ytdl-gtk"))
    }

    /* The acceptance set for --archive-root, which must match
     * archive-viewer.py's: a path that works for one works for both. */
    func testArchiveRootAcceptanceSet() throws {
        try makeVideo(channel: "Chan", folder: "Chan - 20240101 - abcdefghijk - T")

        let dataRoot = tmpdir
        let youtubeVideos = Paths.join(tmpdir, "Youtube Videos")
        let channelDir = Paths.join(root, "Chan")

        XCTAssertEqual(Paths.resolveArchiveRoot(dataRoot), root)
        XCTAssertEqual(Paths.resolveArchiveRoot(youtubeVideos), root)
        XCTAssertEqual(Paths.resolveArchiveRoot(root), root)
        XCTAssertEqual(Paths.resolveArchiveRoot(channelDir), root)
        XCTAssertNil(Paths.resolveArchiveRoot(Paths.join(tmpdir, "nowhere")))
    }
}
