/* Collection-wide comment and transcript search.
 *
 * The tokenizer is the contract, and it is the one function here whose
 * disagreement between the three apps would be INVISIBLE: the same query would
 * quietly return different videos on different platforms, with nothing in any
 * UI saying why. So the strings below are the same ones
 * linux-gtk/tests/test_search.c and the C# suite tokenize, and the expected
 * token lists are written from the rule -- "case-folded runs of letters and
 * digits, everything else a separator" -- rather than from what any one
 * implementation happens to do.
 *
 * The fixture videos, their comments and their captions are the same ones the
 * C suite builds, so a query asserted here is a query asserted there.
 */

import XCTest
@testable import YtdlMac

final class SearchIndexTests: XCTestCase {

    private var tmpdir = ""
    private var root = ""
    private var index = ArchiveIndex()

    private var indexPath: String { Paths.join(tmpdir, "search-index.json") }

    override func setUpWithError() throws {
        tmpdir = try FixtureSupport.makeTempDir(prefix: "ytdl-mac-search")
        Paths.environmentOverride = ["HOME": tmpdir]

        root = Paths.join(tmpdir, "Complete Archive")
        try FixtureSupport.mkdirp(root)

        try makeVideo(channel: "Tom Scott", name: "bridge",
                      title: "The Moving Bridge", date: "20240131",
                      stamp: "2026-01-01T00:00:00Z",
                      commentA: "How does the guardrail stay on when it swings?",
                      commentB: "The counterweight does most of the work.",
                      vtt: Self.vttBridge)

        try makeVideo(channel: "Veritasium", name: "ice",
                      title: "Strange Ice", date: "20230615",
                      stamp: "2026-01-02T00:00:00Z",
                      commentA: "Regelation is the word you are looking for.",
                      commentB: "I was taught this was pressure melting.",
                      vtt: Self.vttIce)

        index = try ArchiveIndex.scan(root: root)
        XCTAssertEqual(index.entries.count, 2)
    }

    override func tearDownWithError() throws {
        Paths.environmentOverride = nil
        try? FileManager.default.removeItem(atPath: tmpdir)
    }

    // MARK: - Fixture

    /* Deliberately NOT auto-generated: no karaoke tags, no cue settings. The
     * rolling-duplication collapse is tested elsewhere and would only obscure
     * what this file is about. */
    private static let vttBridge = """
    WEBVTT

    00:00:01.000 --> 00:00:04.000
    this railway bridge swings sideways

    00:00:04.000 --> 00:00:08.000
    to let the canal boats through

    """

    private static let vttIce = """
    WEBVTT

    00:00:02.000 --> 00:00:06.000
    ice behaves strangely under pressure

    """

    /* The comment shapes are yt-dlp's own: a FLAT array with a `parent` field
     * that is either "root" or the parent's id, deliberately out of order,
     * because that is what the reader has to cope with and an index built on a
     * tidier fixture would not prove it does. */
    private func makeVideo(channel: String, name: String, title: String,
                           date: String, stamp: String,
                           commentA: String, commentB: String,
                           vtt: String?) throws {
        let id = name.padding(toLength: 11, withPad: "_", startingAt: 0)
        let folder = "\(channel) - \(date) - \(id) - \(title)"
        let vdir = Paths.join(Paths.join(root, channel), folder)
        try FixtureSupport.mkdirp(vdir)

        let manifest = """
        {
          "archive_layout_version": 2,
          "archive_creation_time": "\(stamp)",
          "video_id": "\(id)",
          "title": "\(title)",
          "uploader": "\(channel)",
          "upload_date": "\(date)",
          "download_mode": "full",
          "media_file": "Final files/Final Video.mkv"
        }
        """
        try FixtureSupport.write(manifest,
                                 to: Paths.join(vdir, "Video metadata/manifest.json"))

        let info = """
        {
          "id": "\(id)",
          "title": "\(title)",
          "description": "A description mentioning aqueducts.",
          "comments": [
            {"id": "c2", "parent": "c1", "text": "\(commentB)", "author": "Replier"},
            {"id": "c1", "parent": "root", "text": "\(commentA)", "author": "Asker"}
          ]
        }
        """
        try FixtureSupport.write(info,
                                 to: Paths.join(vdir, "Video metadata/Video.info.json"))
        try FixtureSupport.write("x", to: Paths.join(vdir, "Final files/Final Video.mkv"))

        if let vtt {
            try FixtureSupport.write(vtt, to: Paths.join(vdir, "Subtitles/Subtitles.en.vtt"))
        }
    }

    private func entry(_ name: String) throws -> ArchiveEntry {
        try XCTUnwrap(index.entries.first { ($0.videoID ?? "").hasPrefix(name) })
    }

    // MARK: - The tokenizer

    func testTokenizerRule() throws {
        /* Case-folded runs of letters and digits; everything else separates. */
        XCTAssertEqual(SearchIndex.tokenize("Hello, World!"), ["hello", "world"])

        /* Digits are letters as far as this is concerned: "1080p" and "av01"
         * are words someone will search for. */
        XCTAssertEqual(SearchIndex.tokenize("1080p AV01 x264"),
                       ["1080p", "av01", "x264"])

        /* The apostrophe is a SEPARATOR, so "don't" is two tokens. That is a
         * choice rather than an oversight, and the point is that all three
         * apps make the same one. */
        XCTAssertEqual(SearchIndex.tokenize("don't"), ["don", "t"])

        /* Punctuation, hyphens and newlines all separate, and runs of them do
         * not produce empty tokens. */
        XCTAssertEqual(SearchIndex.tokenize("well---known  \n stuff."),
                       ["well", "known", "stuff"])

        /* Non-ASCII letters are letters, and case-folding is not ASCII-only. */
        XCTAssertEqual(SearchIndex.tokenize("Größe ÉCOLE"), ["größe", "école"])

        XCTAssertEqual(SearchIndex.tokenize(""), [])
        XCTAssertEqual(SearchIndex.tokenize("   ...   "), [])
        XCTAssertEqual(SearchIndex.tokenize(nil), [])
    }

    // MARK: - Query semantics

    func testAllTokensMustMatch() throws {
        let ix = SearchIndex(path: indexPath)
        ix.build(over: index.entries)
        XCTAssertEqual(ix.count, 2)

        let bridge = try entry("bridge")

        /* Both words are in the bridge video's comments; only one is in the
         * ice video's. An OR would return both and would return most of an
         * archive for any two common words. */
        XCTAssertEqual(ix.query("counterweight work", scope: .comments),
                       [bridge.key])

        /* Order does not matter. */
        XCTAssertEqual(ix.query("work counterweight", scope: .comments),
                       [bridge.key])

        /* A token that appears in neither excludes everything, even alongside
         * one that appears in both. */
        XCTAssertTrue(ix.query("the zeppelin", scope: .comments).isEmpty)
    }

    func testPrefixMatchingRespectsTokenBoundaries() throws {
        let ix = SearchIndex(path: indexPath)
        ix.build(over: index.entries)

        let bridge = try entry("bridge")

        /* "rail" is a prefix of "railway", which is in the bridge
         * transcript. */
        XCTAssertEqual(ix.query("rail", scope: .transcript), [bridge.key])

        /* THE ONE THAT MATTERS. "rail" must NOT match "guardrail", which is in
         * the bridge video's COMMENTS. A plain substring search over the token
         * blob -- the most likely implementation -- would match it, and that
         * is what makes short queries useless. */
        XCTAssertTrue(ix.query("rail", scope: .comments).isEmpty)

        /* And the whole token still matches itself. */
        XCTAssertEqual(ix.query("guardrail", scope: .comments), [bridge.key])
    }

    func testScopesAreSeparate() throws {
        let ix = SearchIndex(path: indexPath)
        ix.build(over: index.entries)

        /* "sideways" is in the transcript and not in the comments. */
        XCTAssertEqual(ix.query("sideways", scope: .transcript).count, 1)
        XCTAssertTrue(ix.query("sideways", scope: .comments).isEmpty)
        XCTAssertEqual(ix.query("sideways", scope: .everything).count, 1)

        /* The DESCRIPTION is indexed with the comments rather than given a
         * scope of its own -- it is the uploader's own words about the video,
         * which is what someone searching "comments" is reaching for. */
        XCTAssertEqual(ix.query("aqueducts", scope: .comments).count, 2)
    }

    func testEmptyQueryMatchesNothing() throws {
        let ix = SearchIndex(path: indexPath)
        ix.build(over: index.entries)

        /* "Match everything" is the caller's decision to make, not this
         * function's to guess. The Library keeps showing the whole archive
         * when the search field is empty because IT decides that, not because
         * the index said so. */
        XCTAssertTrue(ix.query("   ", scope: .everything).isEmpty)
    }

    // MARK: - Freshness

    func testOnlyChangedVideosAreReparsed() throws {
        let ix = SearchIndex(path: indexPath)

        XCTAssertEqual(ix.outdated(in: index.entries), 2)
        ix.build(over: index.entries)
        XCTAssertEqual(ix.outdated(in: index.entries), 0)

        /* Rewrite ONE video the way `ytdl --refresh` would: a new creation
         * stamp and new comments, everything else the same. */
        try makeVideo(channel: "Tom Scott", name: "bridge",
                      title: "The Moving Bridge", date: "20240131",
                      stamp: "2026-05-05T00:00:00Z",
                      commentA: "Actually the swing is hydraulic these days.",
                      commentB: "Someone said zeppelin and I cannot unsee it.",
                      vtt: Self.vttBridge)

        let after = try ArchiveIndex.scan(root: root)
        XCTAssertEqual(ix.outdated(in: after.entries), 1)

        ix.build(over: after.entries)
        XCTAssertEqual(ix.outdated(in: after.entries), 0)

        /* And the new comment text is what is searchable now. */
        XCTAssertEqual(ix.query("zeppelin", scope: .comments).count, 1)
        XCTAssertTrue(ix.query("counterweight", scope: .comments).isEmpty)
    }

    func testIndexRoundTripsAndDropsRemoved() throws {
        do {
            let ix = SearchIndex(path: indexPath)
            ix.build(over: index.entries)
            ix.save()
        }

        let reloaded = SearchIndex(path: indexPath)
        XCTAssertEqual(reloaded.count, 2)
        XCTAssertEqual(reloaded.outdated(in: index.entries), 0)

        /* A video that has left the archive leaves the index too, or the store
         * grows forever across rescans of a tree somebody reorganises. */
        try FileManager.default.removeItem(atPath: Paths.join(root, "Veritasium"))
        let after = try ArchiveIndex.scan(root: root)
        XCTAssertEqual(after.entries.count, 1)

        reloaded.build(over: after.entries)
        XCTAssertEqual(reloaded.count, 1)
    }

    func testCorruptIndexIsEmptyNotFatal() throws {
        try FixtureSupport.write("{ not json at all", to: indexPath)
        XCTAssertEqual(SearchIndex(path: indexPath).count, 0)
    }

    func testVersionMismatchIsDiscarded() throws {
        /* A store written by a different version of this format is thrown away
         * rather than misread. Without the check, a later change to what a
         * token is would leave every existing user with an index that silently
         * answers the old way. */
        try FixtureSupport.write(
            "{\"version\": 99, \"videos\": {\"abc\": {\"stamp\": \"x\", \"c\": \" hello \"}}}",
            to: indexPath)
        XCTAssertEqual(SearchIndex(path: indexPath).count, 0)
    }

    // MARK: - Snippets

    func testSnippetsComeFromTheRealText() throws {
        let bridge = try entry("bridge")

        /* The index holds no text at all, so a snippet is proof that the
         * matched video's own files were re-read. */
        let comments = SearchIndex.snippets(for: bridge, query: "counterweight",
                                            scope: .comments)
        XCTAssertEqual(comments.count, 1)
        XCTAssertTrue(comments[0].text.contains("counterweight"))
        XCTAssertEqual(comments[0].who, "Replier")
        XCTAssertFalse(comments[0].fromTranscript)

        /* A transcript hit spans cues: "railway bridge swings" and "canal
         * boats" are different cues, and a matcher that worked per cue would
         * find the phrase in neither. The passage carries a timestamp instead
         * of an author. */
        let cues = SearchIndex.snippets(for: bridge, query: "railway canal",
                                        scope: .transcript)
        XCTAssertEqual(cues.count, 1)
        XCTAssertTrue(cues[0].fromTranscript)
        XCTAssertEqual(cues[0].who, "0:01")

        /* A query that matches the index but not any single passage returns no
         * snippets rather than a wrong one. */
        XCTAssertTrue(SearchIndex.snippets(for: bridge, query: "zeppelin",
                                           scope: .everything).isEmpty)
    }

    func testScopeIDsRoundTrip() throws {
        for scope in SearchScope.allCases {
            XCTAssertEqual(SearchScope.from(id: scope.rawValue), scope)
            XCTAssertFalse(scope.label.isEmpty)
        }
        XCTAssertEqual(SearchScope.from(id: "lyrics"), .metadata)
        XCTAssertEqual(SearchScope.from(id: nil), .metadata)

        /* Only the metadata scope can be answered without the index, and the
         * UI keys its "not built yet" message off exactly this. */
        XCTAssertFalse(SearchScope.metadata.needsIndex)
        XCTAssertTrue(SearchScope.comments.needsIndex)
        XCTAssertTrue(SearchScope.transcript.needsIndex)
        XCTAssertTrue(SearchScope.everything.needsIndex)
    }
}
