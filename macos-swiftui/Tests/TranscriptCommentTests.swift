/* The two parsers the detail page is built on.
 *
 * Both turn something yt-dlp wrote for a machine into something a person can
 * read, and both fail QUIETLY when they are wrong -- a mis-threaded comment
 * section still renders, and a transcript with the rolling duplication left in
 * still looks like a transcript. Neither throws. So they are pinned here.
 */

import XCTest
@testable import YtdlMac

final class CommentThreadingTests: XCTestCase {
    private func objects(_ json: String) throws -> [[String: Any]] {
        let data = Data(json.utf8)
        let parsed = try XCTUnwrap(try JSONSerialization.jsonObject(with: data) as? [Any])
        return parsed.compactMap { $0 as? [String: Any] }
    }

    func testThreadsRepliesUnderParents() throws {
        /* The reply comes FIRST, before the parent it belongs to. yt-dlp gives
         * no ordering guarantee, which is the whole reason this is a two-pass
         * job rather than a fold. */
        let tops = Comment.thread(try objects("""
        [
          {"id":"a.1","parent":"a","text":"reply","author":"R","timestamp":200},
          {"id":"a","parent":"root","text":"top","author":"T"},
          {"id":"a.0","parent":"a","text":"earlier","author":"E","timestamp":100}
        ]
        """))

        XCTAssertEqual(tops.count, 1)
        XCTAssertEqual(tops[0].text, "top")
        XCTAssertEqual(tops[0].replies.count, 2)
        // Replies in time order, not arrival order.
        XCTAssertEqual(tops[0].replies[0].text, "earlier")
        XCTAssertEqual(tops[0].replies[1].text, "reply")
    }

    func testRecoversParentFromCompoundID() throws {
        /* The parent field carries the compound reply id rather than the bare
         * parent. yt-dlp's reply ids are "<parent>.<reply>", so the parent is
         * recoverable from the prefix. */
        let tops = Comment.thread(try objects("""
        [
          {"id":"top1","parent":"root","text":"t","author":"A"},
          {"id":"top1.9","parent":"top1.9","text":"r","author":"B"}
        ]
        """))

        XCTAssertEqual(tops.count, 1)
        XCTAssertEqual(tops[0].replies.count, 1)
    }

    func testOrphanReplyIsPromotedNotDropped() throws {
        /* The parent is genuinely absent -- a deleted comment, or a fetch that
         * was truncated. Silently losing archived text would be the worse
         * failure, so the reply is shown at top level instead. */
        let tops = Comment.thread(try objects("""
        [{"id":"zz.1","parent":"gone","text":"orphan","author":"O"}]
        """))

        XCTAssertEqual(tops.count, 1)
        XCTAssertEqual(tops[0].text, "orphan")
    }

    func testPinnedFirstThenLikes() throws {
        let tops = Comment.thread(try objects("""
        [
          {"id":"a","parent":"root","text":"few","author":"A","like_count":5},
          {"id":"b","parent":"root","text":"many","author":"B","like_count":900},
          {"id":"c","parent":"root","text":"pinned","author":"C","like_count":1,
           "is_pinned":true}
        ]
        """))

        XCTAssertEqual(tops.count, 3)
        /* The order YouTube itself shows. A comment section in arbitrary order
         * is a different document from the one people actually read. */
        XCTAssertEqual(tops[0].text, "pinned")
        XCTAssertEqual(tops[1].text, "many")
        XCTAssertEqual(tops[2].text, "few")
    }

    func testEqualLikesKeepTheirOriginalOrder() throws {
        /* Swift's sort is not stable, so equally-liked comments -- most of a
         * long thread -- need an explicit tie-break or the section reshuffles
         * itself between two reads of the same file. */
        let json = "[" + (0..<20).map {
            "{\"id\":\"c\($0)\",\"parent\":\"root\",\"text\":\"\($0)\",\"author\":\"A\"}"
        }.joined(separator: ",") + "]"

        let tops = Comment.thread(try objects(json))
        XCTAssertEqual(tops.map { $0.text }, (0..<20).map { String($0) })
    }

    func testMissingFieldsAreTolerated() throws {
        let tops = Comment.thread(try objects("[{\"id\":\"a\"},{\"parent\":\"root\"}]"))

        XCTAssertEqual(tops.count, 2)
        /* No author is "(unknown)", not empty -- every field is optional
         * because the shape varies by extractor and by yt-dlp version. */
        XCTAssertEqual(tops[0].author, "(unknown)")
        XCTAssertEqual(tops[0].likeCount, -1)
    }

    func testEmptyCommentList() {
        XCTAssertTrue(Comment.thread([]).isEmpty)
    }

    func testTotalCountsRepliesToo() throws {
        let tops = Comment.thread(try objects("""
        [
          {"id":"a","parent":"root","text":"t","author":"A"},
          {"id":"a.1","parent":"a","text":"r","author":"B"},
          {"id":"a.2","parent":"a","text":"r2","author":"C"}
        ]
        """))
        XCTAssertEqual(Comment.totalCount(tops), 3)
    }
}

final class TranscriptTests: XCTestCase {
    private var dir = ""

    override func setUpWithError() throws {
        dir = try FixtureSupport.makeTempDir(prefix: "ytdl-macos-vtt")
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(atPath: dir)
    }

    private func cues(_ vtt: String) throws -> [Cue] {
        let path = Paths.join(dir, "Subtitles.en.vtt")
        try FixtureSupport.write(vtt, to: path)
        return Transcript.cues(atPath: path)
    }

    func testBasicVTT() throws {
        let c = try cues("""
        WEBVTT

        00:00:01.000 --> 00:00:03.500
        First line

        00:00:04.000 --> 00:00:06.000
        Second line
        """)

        XCTAssertEqual(c.count, 2)
        XCTAssertEqual(c[0].text, "First line")
        XCTAssertEqual(c[0].start, 1.0, accuracy: 0.01)
        XCTAssertEqual(c[0].end, 3.5, accuracy: 0.01)
    }

    func testCollapsesRollingAutoCaptions() throws {
        /* YouTube's ASR output is a rolling two-line display: each cue repeats
         * the previous cue's text and adds a few words. Read as-is it is
         * unusable as prose -- this is the single most important thing this
         * parser does.
         *
         * THREE lines, not two, and that is the whole test. Comparing each cue
         * against the last EMITTED text instead of the previous FULL cue works
         * for two lines and then silently stops: line three shares no prefix
         * with the tail emitted for line two, so it comes out whole and the
         * duplication returns for every line after. The same defect was in
         * orchid-cobalt's src-tauri/src/archive.rs. */
        let c = try cues("""
        WEBVTT

        00:00:01.000 --> 00:00:03.000
        the quick brown fox

        00:00:03.000 --> 00:00:05.000
        the quick brown fox jumps over

        00:00:05.000 --> 00:00:07.000
        the quick brown fox jumps over the lazy dog
        """)

        XCTAssertEqual(c.count, 3)
        XCTAssertEqual(c[0].text, "the quick brown fox")
        XCTAssertEqual(c[1].text, "jumps over")
        XCTAssertEqual(c[2].text, "the lazy dog")
    }

    func testIdenticalCueExtendsRatherThanRepeats() throws {
        let c = try cues("""
        WEBVTT

        00:00:01.000 --> 00:00:02.000
        same text here

        00:00:02.000 --> 00:00:09.000
        same text here
        """)

        XCTAssertEqual(c.count, 1)
        XCTAssertGreaterThan(c[0].end, 8.99)
    }

    func testShortRepeatsAreNotChopped() throws {
        /* The 12-character floor. Without it a genuinely repeated short line is
         * cut into fragments that read as a stutter. */
        let c = try cues("""
        WEBVTT

        00:00:01.000 --> 00:00:02.000
        Yeah.

        00:00:02.000 --> 00:00:03.000
        Yeah. Right.
        """)

        XCTAssertEqual(c.count, 2)
        XCTAssertEqual(c[1].text, "Yeah. Right.")
    }

    func testStripsKaraokeTagsAndEntities() throws {
        let c = try cues("""
        WEBVTT

        00:00:01.000 --> 00:00:03.000 align:start position:0%
        <c>hello</c><00:00:01.500><c> there</c> &amp; welcome
        """)

        XCTAssertEqual(c.count, 1)
        // Tags gone, entity decoded, runs of whitespace collapsed to one space.
        XCTAssertEqual(c[0].text, "hello there & welcome")
    }

    func testTimestampForms() throws {
        /* mm:ss with a comma decimal (SRT style), and a one-digit fraction:
         * ".5" is 500ms, not 5ms, which is what padding on the right is for. */
        let c = try cues("""
        WEBVTT

        01:02,5 --> 01:04,250
        short form
        """)

        XCTAssertEqual(c.count, 1)
        XCTAssertEqual(c[0].start, 62.5, accuracy: 0.01)
        XCTAssertEqual(c[0].end, 64.25, accuracy: 0.01)
    }

    func testBlocksWithoutTimingAreSkipped() throws {
        /* The WEBVTT header, NOTE blocks and cue numbers all arrive as blocks
         * with no "-->" in them. None of them is a cue. */
        let c = try cues("""
        WEBVTT
        Kind: captions
        Language: en

        NOTE this is a comment

        1
        00:00:01.000 --> 00:00:02.000
        only real cue
        """)

        XCTAssertEqual(c.count, 1)
        XCTAssertEqual(c[0].text, "only real cue")
    }

    func testWindowsLineEndings() throws {
        /* A .vtt written on Windows would never match the blank-line block
         * separator without normalisation first. */
        let path = Paths.join(dir, "crlf.vtt")
        try FixtureSupport.write(
            "WEBVTT\r\n\r\n00:00:01.000 --> 00:00:02.000\r\nwith carriage returns\r\n",
            to: path
        )

        let c = Transcript.cues(atPath: path)
        XCTAssertEqual(c.count, 1)
        XCTAssertEqual(c[0].text, "with carriage returns")
    }

    func testMissingFileIsEmptyNotACrash() {
        XCTAssertTrue(Transcript.cues(atPath: "/definitely/not/here.vtt").isEmpty)
    }

    func testDetectsAutoGeneratedTrack() throws {
        /* The filenames cannot tell these apart -- --write-subs and
         * --write-auto-subs both land in Subtitles/ under the same base name.
         * The contents can. */
        let auto = Paths.join(dir, "auto.vtt")
        let human = Paths.join(dir, "human.vtt")
        try FixtureSupport.write(
            "WEBVTT\n\n00:00:01.000 --> 00:00:02.000 align:start position:0%\n<c>word</c>\n",
            to: auto
        )
        try FixtureSupport.write(
            "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nA written line.\n",
            to: human
        )

        XCTAssertTrue(Transcript.isAutoGenerated(atPath: auto))
        XCTAssertFalse(Transcript.isAutoGenerated(atPath: human))
    }

    func testSubtitleExtensionPredicate() {
        XCTAssertTrue(MediaExtensions.isSubtitle(".vtt"))
        XCTAssertTrue(MediaExtensions.isSubtitle(".srt"))
        XCTAssertFalse(MediaExtensions.isSubtitle(".mkv"))
        XCTAssertFalse(MediaExtensions.isSubtitle(""))
        XCTAssertFalse(MediaExtensions.isSubtitle(nil))
    }
}
