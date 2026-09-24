/* Finding out what a URL is before downloading it.
 *
 * The Downloads form used to know nothing about the URL in it until the run
 * failed. The Quality list was a fixed ladder from 2160p down, the codec and
 * container pickers were fixed too, and asking for 1440p AV1 was a request
 * that silently resolved to something else -- discovered, at the earliest, in
 * the Library. This file is the answer to that.
 *
 * WHERE THE ANSWER COMES FROM, and why there are two ways.
 *
 * The pipeline owns a `ytdl <url> --probe` mode that prints one JSON document
 * described by docs/probe-contract.md in orchid-ochre. That is the preferred
 * path and the one normally taken, because the pipeline's copy of the
 * derivation is the authority: it maps yt-dlp's codec spellings onto the
 * --codec vocabulary and works out which --container values a merge could
 * actually produce, under the same PO token provider a real download uses.
 *
 * The fallback exists because this app is pinned to a pipeline ref
 * (CLI_VERSION) that a user's machine may predate. An installed ytdl.ps1
 * without --probe answers with a usage error, and the preview would simply
 * never work -- against a pipeline that downloads perfectly well. So when the
 * pipeline probe fails in the specific way that means "this pipeline is older
 * than the feature", this file runs `yt-dlp -J` itself and derives the same
 * answers here.
 *
 * THE SECOND COPY IS THE COST AND IT IS PAID ON PURPOSE. Two derivations that
 * disagree would be worse than no preview: one pipeline version would offer
 * MP4 where another did not, for the same video, with nothing saying why. So
 * `parse(contract:)` and `fromYtDlp(flat:full:)` are held to the same fixture
 * in Tests/UrlProbeTests.swift and asserted to produce identical heights,
 * codec lists and container lists -- the same obligation the GTK app carries
 * in tests/test_url_probe.c and the WinUI app in ProbeTests.cs, in three
 * languages, for the same reason the archive layout has a conformance suite
 * in each.
 *
 * Read member by member through JSON.swift's tolerant accessors rather than
 * through Codable, for the reason that file already gives: a Decodable struct
 * is a schema, and one unexpected member fails the whole document.
 */

import Foundation

// MARK: - The shape

/// One rendition. Not one row of the UI -- the UI builds its pickers from the
/// derived lists below and shows this list whole only in the details
/// disclosure.
struct UrlProbeFormat: Equatable {
    var formatID: String = ""
    var ext: String = ""
    /// yt-dlp's own spelling, verbatim. "none" where absent.
    var vcodec: String = ""
    var acodec: String = ""
    /// This pipeline's vocabulary, or nil when the codec has no --codec
    /// spelling at all. nil is NOT "any": "any" is a choice the user makes,
    /// and reporting it for a VP8 rendition would be a lie about what was
    /// asked for.
    var videoFamily: String?
    var audioFamily: String?
    var height: Int = 0
    var width: Int = 0
    var fps: Double = 0
    var tbr: Double = 0
    /// Exact, and usually absent.
    var filesize: Int64 = 0
    /// yt-dlp's estimate. Kept apart from `filesize` on purpose: the UI shows
    /// "421 MB" differently from "~421 MB", and merging them presents a guess
    /// as a fact.
    var filesizeApprox: Int64 = 0
    var dynamicRange: String = ""
    var formatNote: String = ""

    var hasVideoStream: Bool { !vcodec.isEmpty && vcodec != "none" }
    var hasAudioStream: Bool { !acodec.isEmpty && acodec != "none" }
}

/// One playlist entry.
struct UrlProbeEntry: Equatable, Identifiable {
    /// The 1-BASED POSITION IN THE PLAYLIST, which is what --playlist-items
    /// counts and therefore the only number that may be written back into
    /// --items. Carried explicitly rather than taken from array position,
    /// because the list is filtered and truncated in the UI and a position
    /// derived from the visible order queues the wrong videos -- a bug whose
    /// first symptom is a successful download of something nobody asked for.
    var index: Int = 0
    var videoID: String = ""
    var title: String = ""
    var uploader: String = ""
    var url: String = ""
    var thumbnail: String = ""
    var duration: Double = 0

    /// Identifiable by playlist position, not by video id: a playlist may
    /// legitimately contain the same video twice, and a duplicated id would
    /// make SwiftUI reuse one row for both.
    var id: Int { index }
}

struct UrlProbe: Equatable {
    /// The contract version this app knows how to read. Compared per document
    /// rather than enforced at startup, the same way the archive layout
    /// version is compared per video.
    static let supportedVersion = 1

    var probeVersion = 0
    /// "video" or "playlist".
    var kind = "video"
    var url = ""
    var id = ""
    var title = ""
    var uploader = ""
    var channel = ""
    var channelURL = ""
    var extractor = ""
    var thumbnail = ""
    var description = ""
    /// YYYYMMDD.
    var uploadDate = ""
    var liveStatus = ""
    var availability = ""
    var webpageURL = ""

    var duration: Double = 0
    var viewCount: Int64 = 0
    var likeCount: Int64 = 0
    var commentCount: Int64 = 0
    var ageLimit = 0

    // Playlists
    var entryCount = 0
    /// What the extractor says the WHOLE list holds, which may exceed
    /// `entryCount`.
    var playlistCount = 0
    var entriesTruncated = false
    var entries: [UrlProbeEntry] = []
    var formatsFromID = ""
    var formatsFromTitle = ""

    // Formats and the lists derived from them
    var formats: [UrlProbeFormat] = []
    /// Descending, distinct.
    var heights: [Int] = []
    var videoCodecs: [String] = []
    var audioCodecs: [String] = []
    var containers: [String] = []
    var subtitleLangs: [String] = []
    var hasVideo = false
    var hasAudio = false

    var potHealthy = false
    var potReason = ""
    var potNote = ""

    /// True when this came from the app's own yt-dlp call rather than from
    /// `ytdl --probe`. Surfaced in the UI, because the two can legitimately
    /// differ: the fallback does not bring up the PO token provider, so its
    /// format table can be the thinner one.
    var fromFallback = false

    var isPlaylist: Bool { kind == "playlist" }
}

// MARK: - Codec families

extension UrlProbe {
    /* The single most confusable part of this contract. yt-dlp spells VP9 as
     * "vp09" and AV1 as "av01" with a ZERO -- it never emits "av1" -- and it
     * calls AAC "mp4a". A codebase that tests only for "vp9" reports the most
     * common codec on YouTube as having no --codec spelling at all, which
     * shows up as a Video codec picker missing its most common entry, on
     * every video. */
    static func videoFamily(_ vcodec: String) -> String? {
        guard !vcodec.isEmpty, vcodec != "none" else { return nil }
        let c = vcodec.lowercased()
        if c.hasPrefix("avc1") || c.hasPrefix("h264") { return "avc1" }
        if c.hasPrefix("vp09") || c.hasPrefix("vp9") { return "vp9" }
        if c.hasPrefix("av01") { return "av01" }
        /* Deliberately nil rather than "any": an unrecognised codec is a
         * rendition this pipeline has no --codec spelling for. */
        return nil
    }

    static func audioFamily(_ acodec: String) -> String? {
        guard !acodec.isEmpty, acodec != "none" else { return nil }
        let c = acodec.lowercased()
        if c.hasPrefix("opus") { return "opus" }
        if c.hasPrefix("mp4a") || c.hasPrefix("aac") { return "aac" }
        if c.hasPrefix("mp3") { return "mp3" }
        if c.hasPrefix("flac") { return "flac" }
        return nil
    }

    /// Canonical order, NOT offer order. yt-dlp's format ordering varies with
    /// client and with its own sorting changes, and a picker whose entries
    /// reshuffle between two probes of the same video looks broken.
    static let videoFamilyOrder = ["avc1", "vp9", "av01"]
    static let audioFamilyOrder = ["opus", "aac", "mp3", "flac"]
}

// MARK: - Reading a format

private func readFormat(_ o: [String: Any], derived: Bool) -> UrlProbeFormat {
    var f = UrlProbeFormat()
    f.formatID = o.str("format_id") ?? ""
    f.ext = o.str("ext") ?? ""
    f.vcodec = o.str("vcodec") ?? ""
    f.acodec = o.str("acodec") ?? ""
    f.height = Int(o.int("height"))
    f.width = Int(o.int("width"))
    f.fps = o.double("fps")
    f.tbr = o.double("tbr")
    f.filesize = o.int("filesize")
    f.filesizeApprox = o.int("filesize_approx")
    f.dynamicRange = o.str("dynamic_range") ?? ""
    f.formatNote = o.str("format_note") ?? ""

    if derived {
        f.videoFamily = o.str("video_family")
        f.audioFamily = o.str("audio_family")
    } else {
        f.videoFamily = UrlProbe.videoFamily(f.vcodec)
        f.audioFamily = UrlProbe.audioFamily(f.acodec)
    }
    return f
}

private func readFormats(_ o: [String: Any], derived: Bool) -> [UrlProbeFormat] {
    var out: [UrlProbeFormat] = []
    for raw in o.objects("formats") {
        /* Storyboards. yt-dlp's .mhtml pseudo-formats carry no streams and
         * are not a rendition of anything; left in, each becomes a phantom row
         * and a phantom height. The pipeline drops them too, so this only ever
         * fires on the fallback path -- but it fires there identically, which
         * is the point. */
        if raw.str("ext") == "mhtml" { continue }
        let f = readFormat(raw, derived: derived)
        guard f.hasVideoStream || f.hasAudioStream else { continue }
        out.append(f)
    }
    return out
}

// MARK: - Deriving the lists

extension UrlProbe {
    /* Everything computed FROM the format list rather than read out of it.
     *
     * This must agree, value for value, with Get-FormatSummary in the
     * pipeline's probe.ps1 -- and with derive_lists() in the GTK app's
     * url_probe.c and DeriveLists() in the WinUI app's Probe.cs. When the
     * pipeline's own document is what was parsed this is not called at all:
     * the derived arrays come from the document, because the pipeline is the
     * authority. It runs only for the fallback path. */
    mutating func deriveLists() {
        /* "Does this format carry video" and "which --codec value is it" are
         * two different questions, and conflating them loses resolutions. A
         * VP8 rendition has a real height the user can ask for and no --codec
         * spelling at all, so the heights come from the first question and the
         * codec lists from the second. */
        hasVideo = formats.contains { $0.hasVideoStream }
        hasAudio = formats.contains { $0.hasAudioStream }

        var seen: [Int] = []
        for f in formats where f.hasVideoStream && f.height > 0 {
            if !seen.contains(f.height) { seen.append(f.height) }
        }
        heights = seen.sorted(by: >)

        videoCodecs = UrlProbe.videoFamilyOrder.filter { fam in
            formats.contains { $0.videoFamily == fam }
        }
        audioCodecs = UrlProbe.audioFamilyOrder.filter { fam in
            formats.contains { $0.audioFamily == fam }
        }
        containers = UrlProbe.containers(video: videoCodecs, audio: audioCodecs)
    }

    /* Which --container values a merge could actually produce.
     *
     * mkv is unconditional: Matroska carries every codec pair YouTube serves,
     * which is why it is this pipeline's default and its archival choice.
     *
     * The other two are real constraints, not preferences, and getting them
     * wrong is the specific failure this whole file exists to prevent --
     * yt-dlp cannot mux Opus into mp4 or AAC into webm, and asking it to
     * produces either a re-encode or a failed merge depending on version. A
     * picker offering one that cannot be made is a mistake the user only
     * discovers afterwards. */
    static func containers(video: [String], audio: [String]) -> [String] {
        var out = ["mkv"]
        let mp4Video = video.contains("avc1") || video.contains("av01")
        let webmVideo = video.contains("vp9") || video.contains("av01")
        if mp4Video && audio.contains("aac") { out.append("mp4") }
        if webmVideo && audio.contains("opus") { out.append("webm") }
        return out
    }

    /// The heights this video offers for `family` ("any" or nil means all of
    /// them), descending. This is the cross-filter the Quality picker needs:
    /// 1440p commonly exists only in VP9, so a list built from the union of
    /// heights offers a combination the video does not have.
    func heights(forCodec family: String?) -> [Int] {
        let all = family == nil || family!.isEmpty || family! == "any"
        var out: [Int] = []
        for f in formats where f.hasVideoStream && f.height > 0 {
            if !all && f.videoFamily != family { continue }
            if !out.contains(f.height) { out.append(f.height) }
        }
        return out.sorted(by: >)
    }
}

// MARK: - Parsing

extension UrlProbe {
    enum ParseError: LocalizedError {
        case empty
        case notAnObject
        case ytdlpEmpty

        var errorDescription: String? {
            switch self {
            case .empty: return "The probe returned nothing."
            case .notAnObject: return "The probe did not return a JSON object."
            case .ytdlpEmpty: return "yt-dlp returned nothing."
            }
        }
    }

    /// Read a probe-contract document. `text` is the whole of the pipeline's
    /// stdout, which may carry leading or trailing whitespace and nothing
    /// else.
    static func parse(contract text: String) throws -> UrlProbe {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { throw ParseError.empty }
        guard let data = trimmed.data(using: .utf8),
              let o = JSONFile.object(from: data)
        else { throw ParseError.notAnObject }

        var p = UrlProbe()
        p.probeVersion = Int(o.int("probe_version"))
        p.kind = o.str("kind") ?? "video"
        p.url = o.str("url") ?? ""
        p.id = o.str("id") ?? ""
        p.title = o.str("title") ?? ""
        p.uploader = o.str("uploader") ?? ""
        p.channel = o.str("channel") ?? ""
        p.channelURL = o.str("channel_url") ?? ""
        p.extractor = o.str("extractor") ?? ""
        p.thumbnail = o.str("thumbnail") ?? ""
        p.description = o.str("description") ?? ""
        p.uploadDate = o.str("upload_date") ?? ""
        p.liveStatus = o.str("live_status") ?? ""
        p.availability = o.str("availability") ?? ""
        p.webpageURL = o.str("webpage_url") ?? ""
        p.formatsFromID = o.str("formats_from_id") ?? ""
        p.formatsFromTitle = o.str("formats_from_title") ?? ""

        p.duration = o.double("duration")
        p.viewCount = o.int("view_count")
        p.likeCount = o.int("like_count")
        p.commentCount = o.int("comment_count")
        p.ageLimit = Int(o.int("age_limit"))
        p.entryCount = Int(o.int("entry_count"))
        p.playlistCount = Int(o.int("playlist_count"))
        p.entriesTruncated = o.bool("entries_truncated")

        var index = 0
        for raw in o.objects("entries") {
            index += 1
            var e = UrlProbeEntry()
            e.index = Int(raw.int("index"))
            /* A document from a pipeline that somehow omitted the index still
             * produces a usable list rather than a list of zeroes that would
             * all write the same --items value. */
            if e.index <= 0 { e.index = index }
            e.videoID = raw.str("id") ?? ""
            e.title = raw.str("title") ?? ""
            e.uploader = raw.str("uploader") ?? ""
            e.url = raw.str("url") ?? ""
            e.thumbnail = raw.str("thumbnail") ?? ""
            e.duration = raw.double("duration")
            p.entries.append(e)
        }

        p.formats = readFormats(o, derived: true)

        /* The derived lists are READ, not recomputed. The pipeline is the
         * authority on them: it made them under the PO token provider a real
         * download will use, and recomputing here would mean this app could
         * disagree with the command it is about to run. */
        p.videoCodecs = o.strings("video_codecs")
        p.audioCodecs = o.strings("audio_codecs")
        p.containers = o.strings("containers")
        p.subtitleLangs = o.strings("subtitle_langs")
        p.hasVideo = o.bool("has_video")
        p.hasAudio = o.bool("has_audio")
        p.heights = (o.array("heights") ?? []).compactMap { raw -> Int? in
            guard let n = raw as? NSNumber else { return nil }
            let v = n.intValue
            return v > 0 ? v : nil
        }

        if let pot = o.object("pot") {
            p.potHealthy = pot.bool("healthy")
            p.potReason = pot.str("reason") ?? ""
            p.potNote = pot.str("note") ?? ""
        }

        /* A document with no containers at all is one from a pipeline whose
         * derivation failed, or a truncated read. Falling back to the local
         * derivation beats a Container picker with nothing in it. */
        if p.containers.isEmpty { p.deriveLists() }
        return p
    }

    /// The fallback. `flat` is `yt-dlp -J --flat-playlist` output and may be
    /// nil for a single video; `full` is `yt-dlp -J` output for the video
    /// whose formats should be reported. Does here, in Swift, exactly what
    /// probe.ps1 does in PowerShell.
    static func fromYtDlp(flat: String?, full: String) throws -> UrlProbe {
        let trimmedFull = full.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedFull.isEmpty else { throw ParseError.ytdlpEmpty }
        guard let fullData = trimmedFull.data(using: .utf8),
              let fo = JSONFile.object(from: fullData)
        else { throw ParseError.notAnObject }

        var p = UrlProbe()
        p.probeVersion = UrlProbe.supportedVersion
        p.fromFallback = true

        /* The fallback never brings up the PO token provider -- doing so would
         * mean reimplementing pot-provider.ps1 in Swift, which is exactly the
         * duplication this app exists to avoid. So it says so, rather than
         * leaving the user to wonder why the format table is thinner than the
         * one they saw yesterday. */
        p.potHealthy = false
        p.potReason = "this app read yt-dlp directly, without the pipeline's "
            + "PO token provider"
        p.potNote = "No PO token provider: a real download may see formats "
            + "this list does not show."

        var lo: [String: Any]?
        if let flat = flat,
           !flat.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
           let d = flat.trimmingCharacters(in: .whitespacesAndNewlines).data(using: .utf8) {
            lo = JSONFile.object(from: d)
        }

        let type = lo?.str("_type") ?? ""
        let isPlaylist = type == "playlist" || type == "multi_video"
        p.kind = isPlaylist ? "playlist" : "video"

        if isPlaylist, let lo = lo {
            p.id = lo.str("id") ?? ""
            p.title = lo.str("title") ?? ""
            p.uploader = lo.str("uploader") ?? lo.str("channel") ?? ""
            p.channel = lo.str("channel") ?? ""
            p.extractor = lo.str("extractor_key") ?? ""
            p.playlistCount = Int(lo.int("playlist_count"))

            var index = 0
            for raw in lo.objects("entries") {
                index += 1
                var e = UrlProbeEntry()
                e.index = index
                e.videoID = raw.str("id") ?? ""
                e.title = raw.str("title") ?? ""
                e.uploader = raw.str("uploader") ?? raw.str("channel") ?? ""
                e.url = raw.str("url") ?? raw.str("webpage_url") ?? ""
                e.duration = raw.double("duration")
                p.entries.append(e)
            }
            p.entryCount = p.entries.count
            p.formatsFromID = fo.str("id") ?? ""
            p.formatsFromTitle = fo.str("title") ?? ""
            p.thumbnail = fo.str("thumbnail") ?? ""
            p.duration = fo.double("duration")
        } else {
            p.id = fo.str("id") ?? ""
            p.title = fo.str("title") ?? ""
            p.uploader = fo.str("uploader") ?? fo.str("channel") ?? ""
            p.channel = fo.str("channel") ?? ""
            p.channelURL = fo.str("channel_url") ?? ""
            p.extractor = fo.str("extractor_key") ?? ""
            p.duration = fo.double("duration")
            p.uploadDate = fo.str("upload_date") ?? ""
            p.viewCount = fo.int("view_count")
            p.likeCount = fo.int("like_count")
            p.commentCount = fo.int("comment_count")
            p.liveStatus = fo.str("live_status") ?? ""
            p.availability = fo.str("availability") ?? ""
            p.ageLimit = Int(fo.int("age_limit"))
            p.thumbnail = fo.str("thumbnail") ?? ""
            p.description = fo.str("description") ?? ""
            p.webpageURL = fo.str("webpage_url") ?? ""
            /* Reported but not selectable: the pipeline's conf hardcodes en.*
             * and changing that is a pipeline change, not a GUI one. A picker
             * here would imply a choice that does not exist yet. */
            p.subtitleLangs = Array((fo.object("subtitles") ?? [:]).keys).sorted()
            p.entryCount = 1
        }

        p.formats = readFormats(fo, derived: false)
        p.deriveLists()
        return p
    }
}

// MARK: - --items ranges

enum ItemsRange {
    /// Compact a set of 1-based playlist positions into --items syntax:
    /// {1,2,3,7,10,11,12} becomes "1-3,7,10-12". `indices` need not be sorted.
    ///
    /// Compacted rather than emitted as a comma list because a channel
    /// selection of 400 videos would otherwise produce a command line
    /// thousands of characters long -- unreadable in the command preview, and
    /// on the Windows app close enough to a real command-line limit to matter.
    static func compact(_ indices: [Int]) -> String {
        /* Duplicates are absorbed rather than rejected: two ticked rows cannot
         * produce the same index, but a range parsed back in from a saved
         * profile can overlap itself, and "1-3,2-4" is a legal thing for a
         * human to have typed. */
        let sorted = indices.filter { $0 > 0 }.sorted()
        guard !sorted.isEmpty else { return "" }

        var parts: [String] = []
        var i = 0
        while i < sorted.count {
            let start = sorted[i]
            var end = start
            var j = i + 1
            while j < sorted.count, sorted[j] == end || sorted[j] == end + 1 {
                end = sorted[j]
                j += 1
            }
            if start == end {
                parts.append("\(start)")
            } else if end == start + 1 {
                /* "1,2" rather than "1-2": the same length, and a two-element
                 * range written as a range reads like it might be
                 * open-ended. */
                parts.append("\(start),\(end)")
            } else {
                parts.append("\(start)-\(end)")
            }
            i = j
        }
        return parts.joined(separator: ",")
    }

    /// The inverse: parse an --items range string back into the positions it
    /// names, so a profile or a re-opened form can tick the right boxes.
    /// Anything unparseable is skipped rather than refused -- ytdl.ps1 is the
    /// validator, and a range this cannot read is still one the pipeline may
    /// accept.
    static func parse(_ spec: String) -> [Int] {
        var out: [Int] = []
        for rawPart in spec.split(separator: ",") {
            let part = rawPart.trimmingCharacters(in: .whitespaces)
            if part.isEmpty { continue }

            guard let dash = part.firstIndex(of: "-") else {
                if let v = Int(part), v > 0 { out.append(v) }
                continue
            }
            /* yt-dlp's own open-ended forms -- "5-" and "-10" -- are
             * deliberately NOT expanded to a guessed bound. A tick list built
             * from a guess would show a selection the pipeline may not agree
             * with, and ytdl.ps1 is the validator here, not this. They parse
             * to nothing and the range stays in the text field as typed. */
            if dash == part.startIndex { continue }
            let after = part.index(after: dash)
            if after == part.endIndex { continue }

            guard let lo = Int(part[part.startIndex..<dash]),
                  let hi = Int(part[after...]),
                  lo > 0, hi >= lo
            else { continue }
            /* A bound on how much a malformed range can allocate. 100,000 is
             * far past any real playlist and far short of a memory problem. */
            if hi - lo > 100_000 { continue }
            out.append(contentsOf: lo...hi)
        }
        return out
    }
}

// MARK: - Running one

/// Cancels an in-flight probe. Held by the view model so that editing the URL,
/// or pressing the button a second time, stops the child rather than racing
/// two answers into the same form.
///
/// `@unchecked Sendable` rather than unaudited: this object is deliberately
/// touched from two threads -- the main actor cancels, the probe's worker
/// attaches and detaches -- and every piece of its mutable state is behind the
/// lock below. The "unchecked" is the accurate claim here, not a silencer.
final class UrlProbeCancellation: @unchecked Sendable {
    private let lock = NSLock()
    private var cancelled = false
    private var current: Process?

    var isCancelled: Bool {
        lock.lock(); defer { lock.unlock() }
        return cancelled
    }

    func cancel() {
        lock.lock()
        cancelled = true
        let p = current
        current = nil
        lock.unlock()
        /* The probe's children are short-lived and write nothing, so unlike a
         * run there is no process group to tear down -- terminate is enough,
         * and SIGKILL follows only if it is ignored. */
        if let p = p, p.isRunning { p.terminate() }
    }

    func attach(_ p: Process) -> Bool {
        lock.lock(); defer { lock.unlock() }
        if cancelled { return false }
        current = p
        return true
    }

    func detach() {
        lock.lock(); current = nil; lock.unlock()
    }
}

enum UrlProbeRunner {
    /// What a captured child produced.
    private struct Capture {
        var stdout = ""
        var stderr = ""
        var status: Int32 = -1
    }

    private final class DataBox {
        private let lock = NSLock()
        private var value = Data()
        func set(_ d: Data) { lock.lock(); value = d; lock.unlock() }
        func get() -> Data { lock.lock(); defer { lock.unlock() }; return value }
    }

    /// One child, stdout and stderr captured SEPARATELY.
    ///
    /// Separately, not merged, and that is the whole reason a probe can be
    /// told from a failure at all: the contract is one JSON document on stdout
    /// and everything conversational on stderr, so merging them would put the
    /// pipeline's own "[pot] ..." notes inside the string about to be parsed.
    private static func capture(
        executable: String,
        arguments: [String],
        cancellation: UrlProbeCancellation
    ) -> Capture? {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = Paths.childPath()
        process.environment = env

        let outPipe = Pipe(), errPipe = Pipe()
        process.standardOutput = outPipe
        process.standardError = errPipe
        process.standardInput = FileHandle.nullDevice

        guard cancellation.attach(process) else { return nil }
        do { try process.run() } catch {
            cancellation.detach()
            return nil
        }

        /* Both pipes drained on their own queues rather than one after the
         * other. A format table for a long video is well past a pipe buffer,
         * and a reader still waiting on stdout while stderr fills would
         * deadlock -- the same reason Health.swift drains its two. */
        let outBox = DataBox(), errBox = DataBox()
        let group = DispatchGroup()
        DispatchQueue.global(qos: .userInitiated).async(group: group) {
            outBox.set(outPipe.fileHandleForReading.readDataToEndOfFile())
        }
        DispatchQueue.global(qos: .userInitiated).async(group: group) {
            errBox.set(errPipe.fileHandleForReading.readDataToEndOfFile())
        }
        process.waitUntilExit()
        group.wait()
        cancellation.detach()

        return Capture(
            stdout: String(data: outBox.get(), encoding: .utf8) ?? "",
            stderr: String(data: errBox.get(), encoding: .utf8) ?? "",
            status: process.terminationStatus
        )
    }

    /// The first non-empty line, or nil. yt-dlp's and ytdl.ps1's useful
    /// sentence is not always the last thing printed once the pipeline's own
    /// notes are in the stream.
    private static func firstLine(_ text: String) -> String? {
        for raw in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let line = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            if !line.isEmpty { return line }
        }
        return nil
    }

    /* Does this failure mean "the installed pipeline is older than --probe"?
     *
     * Narrow on purpose. Falling back on ANY failure would turn "this video is
     * private" into a second, slower attempt that also fails, and would hide a
     * genuinely broken pipeline behind a path that happens to work. The two
     * signals that actually mean "too old" are ytdl.ps1's own unknown-option
     * error and a missing probe.ps1. */
    static func meansPipelineTooOld(_ stderr: String) -> Bool {
        stderr.contains("Unknown option: --probe") || stderr.contains("probe.ps1")
    }

    struct Request {
        var url: String
        var items: String = ""
        var noPot = false
        var potPort = 0
        /// The Advanced box, one argument per element, so a URL that needs
        /// --cookies-from-browser can be probed at all.
        var extraArgs: [String] = []
        /// The Connection settings as ytdl flags --
        /// `settings.connection().connectionArgs(forProbe: true)`, so cookies
        /// and proxy only. A preview taken signed out, or not through the
        /// proxy, describes a different video from the one the download gets.
        var connectionArgs: [String] = []

        /* The connection flags are spelled the same in ytdl and in yt-dlp, so
         * the fallback can use them directly -- except --cookies FILE, which
         * is dropped: yt-dlp writes its jar back to that path on exit, and
         * only the pipeline knows to hand it a private copy instead. A
         * pipeline too old for --probe is too old for --cookies as well. */
        var ytdlpConnectionArgs: [String] {
            var out: [String] = []
            var i = 0
            while i + 1 < connectionArgs.count {
                if connectionArgs[i] != "--cookies" {
                    out += [connectionArgs[i], connectionArgs[i + 1]]
                }
                i += 2
            }
            return out
        }
    }

    enum RunError: LocalizedError {
        case pipelineMissing
        case noYtDlp
        case failed(String)
        case cancelled

        var errorDescription: String? {
            switch self {
            case .pipelineMissing:
                return "The installed pipeline was not found."
            case .noYtDlp:
                return "This pipeline is older than `ytdl --probe`, and yt-dlp "
                    + "is not on PATH either, so there is no way to read the "
                    + "URL. Update the pipeline to get the preview."
            case .failed(let why):
                return why
            case .cancelled:
                return "Cancelled."
            }
        }
    }

    /// Asynchronous because a probe is two network round trips and the form
    /// must stay usable -- including its cancel, which is what `cancellation`
    /// is for.
    ///
    /// `completion` arrives on a BACKGROUND queue, and the caller hops to the
    /// main actor itself. That is not an oversight: the caller here is a
    /// `@MainActor` view model, and hopping with `DispatchQueue.main.async`
    /// would leave the closure body nonisolated while touching actor-isolated
    /// state. `Task { @MainActor in ... }` at the call site is what actually
    /// satisfies the isolation, and is the pattern the rest of this app
    /// already uses for exactly this handoff.
    static func run(
        _ request: Request,
        cancellation: UrlProbeCancellation,
        completion: @escaping (Result<UrlProbe, Error>) -> Void
    ) {
        DispatchQueue.global(qos: .userInitiated).async {
            completion(runSync(request, cancellation: cancellation))
        }
    }

    static func runSync(
        _ request: Request,
        cancellation: UrlProbeCancellation
    ) -> Result<UrlProbe, Error> {
        var tooOld = false
        switch viaPipeline(request, cancellation: cancellation, tooOld: &tooOld) {
        case .success(let p):
            return .success(p)
        case .failure(let error):
            if cancellation.isCancelled { return .failure(RunError.cancelled) }
            /* A pipeline that HAS --probe and failed has told us something
             * real -- the video is private, the URL is wrong, cookies are
             * needed. Trying again without the pipeline would replace that
             * sentence with a worse one from a different program. */
            guard tooOld else { return .failure(error) }
        }
        return viaYtDlp(request, cancellation: cancellation)
    }

    private static func viaPipeline(
        _ request: Request,
        cancellation: UrlProbeCancellation,
        tooOld: inout Bool
    ) -> Result<UrlProbe, Error> {
        tooOld = false
        guard let pwsh = Paths.findPwsh() else {
            tooOld = true
            return .failure(RunError.pipelineMissing)
        }
        let script = Paths.join(Paths.scriptsDir(), "ytdl.ps1")
        guard Paths.isRegularFile(script) else {
            tooOld = true
            return .failure(RunError.pipelineMissing)
        }

        var args = ["-NoProfile", "-File", script,
                    RunOptions.normalizeURL(request.url), "--probe"]
        if !request.items.trimmingCharacters(in: .whitespaces).isEmpty {
            args.append("--items")
            args.append(request.items.trimmingCharacters(in: .whitespaces))
        }
        if request.noPot { args.append("--no-pot") }
        if request.potPort > 0 {
            args.append("--pot-port")
            args.append("\(request.potPort)")
        }
        args += request.connectionArgs
        for a in request.extraArgs {
            args.append("--ytdlp-arg")
            args.append(a)
        }

        guard let cap = capture(executable: pwsh, arguments: args,
                                cancellation: cancellation)
        else { return .failure(RunError.cancelled) }

        if !cap.stdout.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
           let p = try? UrlProbe.parse(contract: cap.stdout) {
            return .success(p)
        }

        tooOld = meansPipelineTooOld(cap.stderr)
        let why = firstLine(cap.stderr) ?? "The pipeline's probe returned nothing."
        return .failure(RunError.failed(why))
    }

    private static func viaYtDlp(
        _ request: Request,
        cancellation: UrlProbeCancellation
    ) -> Result<UrlProbe, Error> {
        guard let ytdlp = Paths.which("yt-dlp") else {
            return .failure(RunError.noYtDlp)
        }

        /* The same two bounded calls probe.ps1 makes, for the same reason:
         * `-J` against a channel dumps the full extraction of every video in
         * it. Flat first to find out what this is; then one full extraction
         * for the format table. --ignore-config, because the pipeline's own
         * conf opens with --update and a preview must not self-update
         * yt-dlp. */
        let base = ["--ignore-config", "--no-progress", "--socket-timeout", "15",
                    "--retries", "2", "--extractor-retries", "2"]
        let itemSpec = request.items.trimmingCharacters(in: .whitespaces).isEmpty
            ? "1:501"
            : request.items.trimmingCharacters(in: .whitespaces)
        let url = RunOptions.normalizeURL(request.url)

        var flatArgs = ["-J"] + base + request.ytdlpConnectionArgs + request.extraArgs
        flatArgs += ["--flat-playlist", "--playlist-items", itemSpec]
        /* The end-of-options marker, same as every call site in the pipeline:
         * about one YouTube id in thirty starts with "-" or "_". */
        flatArgs += ["--", url]

        guard let flatCap = capture(executable: ytdlp, arguments: flatArgs,
                                    cancellation: cancellation)
        else { return .failure(RunError.cancelled) }
        let flat = flatCap.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
        if flat.isEmpty {
            let why = firstLine(flatCap.stderr) ?? "yt-dlp could not read that URL."
            return .failure(RunError.failed(why))
        }

        // Which URL the format table should come from.
        var target = url
        if let data = flat.data(using: .utf8),
           let lo = JSONFile.object(from: data) {
            let type = lo.str("_type") ?? ""
            if type == "playlist" || type == "multi_video",
               let first = lo.objects("entries").first {
                target = first.str("url") ?? first.str("webpage_url") ?? url
            }
        }

        var fullArgs = ["-J"] + base + request.ytdlpConnectionArgs + request.extraArgs
        fullArgs += ["--no-playlist", "--", target]

        guard let fullCap = capture(executable: ytdlp, arguments: fullArgs,
                                    cancellation: cancellation)
        else { return .failure(RunError.cancelled) }
        let full = fullCap.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
        if full.isEmpty {
            let why = firstLine(fullCap.stderr) ?? "yt-dlp could not read that URL."
            return .failure(RunError.failed(why))
        }

        do {
            var p = try UrlProbe.fromYtDlp(flat: flat, full: full)
            if p.url.isEmpty { p.url = url }
            return .success(p)
        } catch {
            return .failure(error)
        }
    }
}
