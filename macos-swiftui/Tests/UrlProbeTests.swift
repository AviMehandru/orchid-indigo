/* The URL preview.
 *
 * The thing most worth defending here is not that either parser works. It is
 * that the TWO OF THEM AGREE.
 *
 * UrlProbe reads a probe-contract document when the installed pipeline can
 * produce one, and derives the same answers from raw `yt-dlp -J` when it
 * cannot. Two derivations that disagree would be worse than having no preview
 * at all: one pipeline version would offer MP4 for a video where another did
 * not, with nothing in the UI saying why, and the difference would only ever
 * be visible to someone who upgraded mid-session. So the fixture below is fed
 * through both paths and the derived lists compared field by field.
 *
 * THE FIXTURE IS THE SAME ONE the pipeline's 085-probe suite and the GTK app's
 * tests/test_url_probe.c use, byte for byte in its values. That is deliberate
 * and is the whole point: when this file, that suite and that file all pass,
 * the Swift, PowerShell and C derivations have been shown to agree on the same
 * input rather than each being self-consistent. The expected values were
 * written from docs/probe-contract.md, not from any one implementation.
 *
 * The format list is chosen so that a plausible wrong answer fails. yt-dlp
 * spells VP9 as "vp09" and AV1 as "av01" with a zero; it calls AAC "mp4a"; its
 * storyboards are .mhtml entries with no streams at all; and a VP8 rendition
 * has a real height and no --codec spelling whatsoever.
 *
 * Nothing here spawns a process or touches the network.
 */

import XCTest
@testable import YtdlMac

final class UrlProbeTests: XCTestCase {

    /// The nine renditions, in yt-dlp's own shape.
    private let rawFormats = """
    {"id":"dQw4w9WgXcQ","title":"Test Video","uploader":"Test Channel",
     "channel":"Test Channel","duration":212.0,"upload_date":"20250114",
     "view_count":1234567,"thumbnail":"https://i.ytimg.com/vi/x/maxres.jpg",
     "subtitles":{"en":[],"de":[]},
     "formats":[
      {"format_id":"sb0","ext":"mhtml","vcodec":"none","acodec":"none","height":180},
      {"format_id":"137","ext":"mp4","vcodec":"avc1.640028","acodec":"none",
       "height":1080,"width":1920,"fps":30,"tbr":4412.5,"filesize":112233445},
      {"format_id":"248","ext":"webm","vcodec":"vp09.00.40.08","acodec":"none",
       "height":1080,"width":1920,"fps":30},
      {"format_id":"271","ext":"webm","vcodec":"vp09.00.50.08","acodec":"none",
       "height":1440,"width":2560,"filesize_approx":220000000},
      {"format_id":"401","ext":"mp4","vcodec":"av01.0.12M.08","acodec":"none",
       "height":2160,"width":3840,"dynamic_range":"SDR"},
      {"format_id":"330","ext":"webm","vcodec":"vp08.00.10.08","acodec":"none","height":480},
      {"format_id":"140","ext":"m4a","vcodec":"none","acodec":"mp4a.40.2"},
      {"format_id":"251","ext":"webm","vcodec":"none","acodec":"opus"},
      {"format_id":"18","ext":"mp4","vcodec":"avc1.42001E","acodec":"mp4a.40.2","height":360}
     ]}
    """

    /// The same video as the pipeline would have described it: the derived
    /// lists are present and video_family/audio_family are already filled in.
    private let contractDoc = """
    {"probe_version":1,"kind":"video","url":"https://youtu.be/dQw4w9WgXcQ",
     "id":"dQw4w9WgXcQ","title":"Test Video","uploader":"Test Channel",
     "duration":212.0,"upload_date":"20250114","entry_count":1,"entries":[],
     "entries_truncated":false,"subtitle_langs":["en","de"],
     "heights":[2160,1440,1080,480,360],
     "video_codecs":["avc1","vp9","av01"],
     "audio_codecs":["opus","aac"],
     "containers":["mkv","mp4","webm"],
     "has_video":true,"has_audio":true,
     "formats":[
      {"format_id":"137","ext":"mp4","vcodec":"avc1.640028","acodec":"none",
       "video_family":"avc1","height":1080,"filesize":112233445},
      {"format_id":"271","ext":"webm","vcodec":"vp09.00.50.08","acodec":"none",
       "video_family":"vp9","height":1440},
      {"format_id":"401","ext":"mp4","vcodec":"av01.0.12M.08","acodec":"none",
       "video_family":"av01","height":2160},
      {"format_id":"251","ext":"webm","vcodec":"none","acodec":"opus",
       "audio_family":"opus"}
     ],
     "pot":{"healthy":true,"reason":"provider healthy",
            "note":"Formats were read with the same player clients a download will use."}}
    """

    // MARK: - Codec families

    func testCodecFamilies() {
        /* The four mappings, and the near-misses that would each produce a
         * visibly wrong picker. */
        XCTAssertEqual(UrlProbe.videoFamily("avc1.640028"), "avc1")
        XCTAssertEqual(UrlProbe.videoFamily("h264"), "avc1")
        /* yt-dlp emits the four-character form. A test for "vp9" alone reports
         * the most common codec on YouTube as having no spelling at all. */
        XCTAssertEqual(UrlProbe.videoFamily("vp09.00.50.08"), "vp9")
        XCTAssertEqual(UrlProbe.videoFamily("vp9"), "vp9")
        /* With a zero. "av1" is not a thing yt-dlp ever writes. */
        XCTAssertEqual(UrlProbe.videoFamily("av01.0.12M.08"), "av01")
        XCTAssertNil(UrlProbe.videoFamily("av1"))
        XCTAssertNil(UrlProbe.videoFamily("vp08.00.10.08"))
        XCTAssertNil(UrlProbe.videoFamily("none"))
        XCTAssertNil(UrlProbe.videoFamily(""))

        XCTAssertEqual(UrlProbe.audioFamily("mp4a.40.2"), "aac")
        XCTAssertEqual(UrlProbe.audioFamily("opus"), "opus")
        XCTAssertEqual(UrlProbe.audioFamily("mp3"), "mp3")
        XCTAssertEqual(UrlProbe.audioFamily("flac"), "flac")
        XCTAssertNil(UrlProbe.audioFamily("none"))
    }

    // MARK: - The two paths

    func testFallbackDerivation() throws {
        let p = try UrlProbe.fromYtDlp(flat: nil, full: rawFormats)

        XCTAssertEqual(p.kind, "video")
        XCTAssertEqual(p.id, "dQw4w9WgXcQ")
        XCTAssertEqual(p.title, "Test Video")
        XCTAssertTrue(p.fromFallback)

        // The storyboard is gone; the eight real renditions are not.
        XCTAssertEqual(p.formats.count, 8)

        /* 1080 appears twice in the input and once here; 480 is the VP8
         * rendition, whose height is real even though this pipeline has no
         * --codec spelling for its codec; 180 was the storyboard's. */
        XCTAssertEqual(p.heights, [2160, 1440, 1080, 480, 360])
        XCTAssertEqual(p.videoCodecs, ["avc1", "vp9", "av01"])
        XCTAssertEqual(p.audioCodecs, ["opus", "aac"])
        XCTAssertEqual(p.containers, ["mkv", "mp4", "webm"])
        XCTAssertTrue(p.hasVideo)
        XCTAssertTrue(p.hasAudio)

        /* An exact size and an estimate must not be collapsed into one number:
         * the UI shows "421 MB" differently from "~421 MB", and merging them
         * presents a guess as a fact. */
        let f137 = try XCTUnwrap(p.formats.first { $0.formatID == "137" })
        XCTAssertEqual(f137.filesize, 112233445)
        XCTAssertEqual(f137.filesizeApprox, 0)

        let f271 = try XCTUnwrap(p.formats.first { $0.formatID == "271" })
        XCTAssertEqual(f271.filesize, 0)
        XCTAssertEqual(f271.filesizeApprox, 220000000)

        /* The VP8 rendition is listed, and has no family. Not "any": "any" is
         * a choice the user makes. */
        let vp8 = try XCTUnwrap(p.formats.first { $0.formatID == "330" })
        XCTAssertNil(vp8.videoFamily)

        XCTAssertEqual(p.subtitleLangs.sorted(), ["de", "en"])
    }

    func testContractDocument() throws {
        let p = try UrlProbe.parse(contract: contractDoc)

        XCTAssertEqual(p.probeVersion, UrlProbe.supportedVersion)
        XCTAssertFalse(p.fromFallback)
        XCTAssertEqual(p.title, "Test Video")
        XCTAssertTrue(p.potHealthy)
        XCTAssertFalse(p.potNote.isEmpty)

        /* The derived lists are READ from the document, not recomputed. The
         * pipeline made them under the PO token provider a real download will
         * use, and recomputing here would let this app disagree with the
         * command it is about to run -- note the formats array in the document
         * is deliberately shorter than the heights array it ships with. */
        XCTAssertEqual(p.heights, [2160, 1440, 1080, 480, 360])
        XCTAssertEqual(p.formats.count, 4)
    }

    /// The reason this file exists.
    func testBothPathsAgree() throws {
        let fallback = try UrlProbe.fromYtDlp(flat: nil, full: rawFormats)
        let contract = try UrlProbe.parse(contract: contractDoc)

        XCTAssertEqual(fallback.heights, contract.heights)
        XCTAssertEqual(fallback.videoCodecs, contract.videoCodecs)
        XCTAssertEqual(fallback.audioCodecs, contract.audioCodecs)
        XCTAssertEqual(fallback.containers, contract.containers)
    }

    // MARK: - Containers

    func testContainersAreARealConstraint() throws {
        /* Opus-only audio. yt-dlp cannot mux Opus into mp4, so offering it
         * produces a re-encode or a failed merge depending on version -- which
         * the user discovers afterwards. This is the assertion that earns the
         * whole derivation its place. */
        let opusOnly = """
        {"id":"x","formats":[
         {"format_id":"248","ext":"webm","vcodec":"vp09.00.40.08","acodec":"none","height":1080},
         {"format_id":"251","ext":"webm","vcodec":"none","acodec":"opus"}]}
        """
        let p = try UrlProbe.fromYtDlp(flat: nil, full: opusOnly)
        XCTAssertEqual(p.containers, ["mkv", "webm"])

        // And the mirror image: AAC with no Opus gets mp4 and not webm.
        let aacOnly = """
        {"id":"x","formats":[
         {"format_id":"137","ext":"mp4","vcodec":"avc1.640028","acodec":"none","height":1080},
         {"format_id":"140","ext":"m4a","vcodec":"none","acodec":"mp4a.40.2"}]}
        """
        let q = try UrlProbe.fromYtDlp(flat: nil, full: aacOnly)
        XCTAssertEqual(q.containers, ["mkv", "mp4"])
    }

    func testHeightsCrossFilter() throws {
        /* 1440p exists only in VP9 in the fixture, and 2160p only in AV1. A
         * Quality list built from the union of heights therefore offers
         * combinations the video does not have -- the same class of silent
         * wrong answer the static list had, just with better numbers in it. */
        let p = try UrlProbe.fromYtDlp(flat: nil, full: rawFormats)

        XCTAssertEqual(p.heights(forCodec: "vp9"), [1440, 1080])
        XCTAssertEqual(p.heights(forCodec: "av01"), [2160])
        XCTAssertEqual(p.heights(forCodec: "avc1"), [1080, 360])

        // "any" and nil both mean every height the video has, VP8's included.
        XCTAssertEqual(p.heights(forCodec: "any").count, 5)
        XCTAssertEqual(p.heights(forCodec: nil).count, 5)
    }

    // MARK: - Playlists

    func testPlaylistEntries() throws {
        let flat = """
        {"_type":"playlist","id":"PL1","title":"A Playlist","channel":"Test Channel",
         "playlist_count":3,"entries":[
          {"id":"a","title":"One","duration":60,"url":"https://youtu.be/a"},
          {"id":"b","title":"Two","duration":120,"url":"https://youtu.be/b"},
          {"id":"c","title":"Three","url":"https://youtu.be/c"}]}
        """
        let p = try UrlProbe.fromYtDlp(flat: flat, full: rawFormats)

        XCTAssertEqual(p.kind, "playlist")
        XCTAssertTrue(p.isPlaylist)
        XCTAssertEqual(p.entryCount, 3)
        XCTAssertEqual(p.playlistCount, 3)

        /* 1-based, because that is what --playlist-items counts. An
         * off-by-one here queues the wrong videos and the run succeeds. */
        XCTAssertEqual(p.entries.map { $0.index }, [1, 2, 3])
        XCTAssertEqual(p.entries[1].title, "Two")
        XCTAssertEqual(p.entries[1].videoID, "b")

        // The format table came from the first entry and says so.
        XCTAssertEqual(p.formatsFromID, "dQw4w9WgXcQ")
        XCTAssertEqual(p.formats.count, 8)
    }

    func testEntriesAreIdentifiedByPosition() throws {
        /* A playlist may legitimately contain the same video twice. Identity
         * by video id would make SwiftUI reuse one row for both and the tick
         * boxes would move together. */
        let flat = """
        {"_type":"playlist","id":"PL1","title":"Dupes","entries":[
         {"id":"a","title":"One","url":"https://youtu.be/a"},
         {"id":"a","title":"One again","url":"https://youtu.be/a"}]}
        """
        let p = try UrlProbe.fromYtDlp(flat: flat, full: rawFormats)
        XCTAssertEqual(p.entries.count, 2)
        XCTAssertNotEqual(p.entries[0].id, p.entries[1].id)
    }

    // MARK: - --items ranges

    func testItemsRangeCompaction() {
        /* Compacted rather than emitted as a comma list: a 400-video channel
         * selection would otherwise be a command line thousands of characters
         * long, which is unreadable in the command preview and close enough to
         * a real limit on Windows to matter. */
        XCTAssertEqual(ItemsRange.compact([1, 2, 3, 7, 10, 11, 12]), "1-3,7,10-12")
        XCTAssertEqual(ItemsRange.compact([5]), "5")

        /* A pair stays a pair. "4-5" is the same length as "4,5" and reads
         * like it might be open-ended. */
        XCTAssertEqual(ItemsRange.compact([4, 5]), "4,5")

        /* Unsorted input, and duplicates, both produce the same answer as
         * sorted unique input would -- a saved range parsed back in can
         * overlap itself. */
        XCTAssertEqual(ItemsRange.compact([12, 3, 1, 2, 11, 10, 3]), "1-3,10-12")
        XCTAssertEqual(ItemsRange.compact([]), "")
        XCTAssertEqual(ItemsRange.compact([0, -4]), "")
    }

    func testItemsRangeRoundTrip() {
        let got = ItemsRange.parse("1-3,7,10-12")
        XCTAssertEqual(got, [1, 2, 3, 7, 10, 11, 12])
        XCTAssertEqual(ItemsRange.compact(got), "1-3,7,10-12")

        /* yt-dlp's open-ended forms are NOT expanded to a guessed bound: a
         * tick list built from a guess would show a selection the pipeline may
         * not agree with. They parse to nothing and the typed text stays. */
        XCTAssertEqual(ItemsRange.parse("5-"), [])
        XCTAssertEqual(ItemsRange.parse("-10"), [])
        XCTAssertEqual(ItemsRange.parse("not a range"), [])
        XCTAssertEqual(ItemsRange.parse(""), [])
    }

    // MARK: - Bad input

    func testBadInputIsAnErrorNotACrash() {
        XCTAssertThrowsError(try UrlProbe.parse(contract: ""))
        XCTAssertThrowsError(try UrlProbe.parse(contract: "not json at all"))
        // A bare array is valid JSON and not a probe document.
        XCTAssertThrowsError(try UrlProbe.parse(contract: "[1,2,3]"))
        XCTAssertThrowsError(try UrlProbe.fromYtDlp(flat: nil, full: ""))
    }

    func testEmptyObjectParsesWithUsableDefaults() throws {
        /* An object with nothing in it parses, and every list comes back empty
         * rather than nil -- the form walks these unconditionally, and a
         * Container picker with no rows renders as a blank control that cannot
         * be opened. */
        let bare = try UrlProbe.parse(contract: "{}")
        XCTAssertEqual(bare.kind, "video")
        // No formats at all still leaves mkv, which is always muxable.
        XCTAssertEqual(bare.containers, ["mkv"])
        XCTAssertTrue(bare.entries.isEmpty)
        XCTAssertTrue(bare.heights.isEmpty)
    }

    // MARK: - The fallback trigger

    func testOnlyAnOldPipelineTriggersTheFallback() {
        /* Narrow on purpose. Falling back on ANY failure would turn "this
         * video is private" into a second, slower attempt that also fails, and
         * would hide a genuinely broken pipeline behind a path that happens to
         * work. */
        XCTAssertTrue(UrlProbeRunner.meansPipelineTooOld(
            "Unknown option: --probe\nUsage: ytdl <youtube-url> ..."))
        XCTAssertTrue(UrlProbeRunner.meansPipelineTooOld(
            "ERROR: probe.ps1 not found at /Users/x/yt-dlp/scripts"))

        XCTAssertFalse(UrlProbeRunner.meansPipelineTooOld(
            "ERROR: could not read the URL -- Video unavailable"))
        XCTAssertFalse(UrlProbeRunner.meansPipelineTooOld(
            "ERROR: Sign in to confirm your age"))
        XCTAssertFalse(UrlProbeRunner.meansPipelineTooOld(""))
    }
}
