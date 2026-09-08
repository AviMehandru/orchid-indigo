/* What a run IS: the option set, the command line it becomes, and the parsers
 * that turn the pipeline's output back into something to show.
 *
 * This does NOT reimplement any part of the pipeline. It builds a `ytdl`
 * command line and hands it to ytdl.ps1 -- the single argument parser, on every
 * platform -- exactly as a terminal would. Every option below maps one-to-one
 * onto a flag ytdl.ps1 already accepts, and nothing here knows what
 * run_ytdlp.ps1 or postprocess.ps1 do with them.
 *
 * Why ytdl.ps1 and not run_ytdlp.ps1 directly: because then there would be two
 * argument surfaces to keep in agreement, which is the exact problem the
 * pipeline collapsed into one file. A flag added to ytdl.ps1 becomes available
 * here by adding a control, and a flag it rejects fails the same way it fails
 * in a terminal.
 *
 * Validation is deliberately NOT duplicated here. ytdl.ps1 checks these at the
 * point they reach it and exits non-zero naming the option, which surfaces in
 * the run log like any other pipeline error. A second copy of the
 * accepted-value lists in Swift would be a second thing to keep in step across
 * two repositories, for nothing the user can see.
 */

import Foundation

// MARK: - Options

/// Every field maps to exactly one ytdl.ps1 flag. nil and 0 mean "not set"
/// throughout, which is what keeps a plain download producing exactly the
/// command line it produced before these options existed.
struct RunOptions: Equatable {
    var url: String = ""
    var dataRoot: String = ""

    var sync = false
    var items: String = ""
    var after: String = ""
    var lazy = false
    /// 0 = unset; only emitted when > 1.
    var workers: Int = 0
    var noPot = false
    var skipPotUpdate = false
    /// 0 = unset.
    var potPort: Int = 0

    /* Content options -- the only ones that change what ends up in the archive
     * rather than how the session is scheduled. The GUI deliberately does not
     * build a yt-dlp format selector or decide what audio-only means; all of
     * that lives in run_ytdlp.ps1, on the far side of the CLI_VERSION pin. */
    /// full | video-only | audio-only | metadata-only | comments-only | subs-only
    var mode: String = ""
    /// a height in pixels, or "best"
    var quality: String = ""
    /// any | avc1 | vp9 | av01
    var codec: String = ""
    /// any | opus | aac | mp3 | flac
    var audioCodec: String = ""
    /// mkv | mp4 | webm
    var container: String = ""
    var noComments = false
    var noSubs = false
    var noThumbnail = false
    var noMetadata = false

    /// Each emitted as its own --ytdlp-arg.
    var ytdlpArgs: [String] = []
}

extension RunOptions {
    private static func isNonEmpty(_ s: String) -> Bool {
        !s.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    /* Emitted only when it differs from the pipeline's own default, so a plain
     * download produces exactly the command line it produced before these
     * options existed. That matters for more than tidiness: --quality best,
     * --codec any and --mode full are no-ops the pipeline would accept and
     * ignore, but emitting them would put four extra flags in the preview of
     * every ordinary download and make the common case look complicated. */
    private static func pushNonDefault(
        _ v: inout [String], _ flag: String, _ value: String, _ dflt: String
    ) {
        guard isNonEmpty(value) else { return }
        let t = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard t != dflt else { return }
        v.append(flag)
        v.append(t)
    }

    /* Everything after the script path. The URL is always first and always a
     * full URL: ytdl.ps1 accepts a bare 11-character id, but a leading-hyphen
     * id would be bound as a parameter before the script ever saw it, and about
     * one YouTube id in thirty starts with "-" or "_". */
    func toArgs() -> [String] {
        var v: [String] = []
        v.append(RunOptions.normalizeURL(url))

        if RunOptions.isNonEmpty(dataRoot) {
            /* Expand ~ HERE, at the one point a path leaves this process.
             *
             * run_ytdlp.ps1 resolves -DataRoot with
             * [System.IO.Path]::GetFullPath, which has no notion of a home
             * directory: "~/Movies" reaches it as a literal "~" folder under
             * the pipeline process's working directory. Meanwhile this app's
             * own expandTilde DID expand it, so the same typed path pointed at
             * two different places -- the library indexed one and the downloads
             * went to the other, with nothing reporting an error. */
            v.append("--path")
            v.append(Paths.expandTilde(dataRoot.trimmingCharacters(in: .whitespacesAndNewlines)))
        }
        if sync { v.append("--sync") }
        if RunOptions.isNonEmpty(items) {
            v.append("--items")
            v.append(items.trimmingCharacters(in: .whitespacesAndNewlines))
        }
        if RunOptions.isNonEmpty(after) {
            v.append("--after")
            v.append(after.trimmingCharacters(in: .whitespacesAndNewlines))
        }
        if lazy { v.append("--lazy") }
        if workers > 1 {
            v.append("--workers")
            v.append(String(workers))
        }
        if noPot { v.append("--no-pot") }
        if skipPotUpdate { v.append("--skip-pot-update") }
        if potPort > 0 {
            v.append("--pot-port")
            v.append(String(potPort))
        }

        RunOptions.pushNonDefault(&v, "--mode", mode, "full")
        RunOptions.pushNonDefault(&v, "--quality", quality, "best")
        RunOptions.pushNonDefault(&v, "--codec", codec, "any")
        RunOptions.pushNonDefault(&v, "--audio-codec", audioCodec, "any")
        RunOptions.pushNonDefault(&v, "--container", container, "mkv")

        if noComments { v.append("--no-comments") }
        if noSubs { v.append("--no-subs") }
        if noThumbnail { v.append("--no-thumbnail") }
        if noMetadata { v.append("--no-metadata") }

        /* Repeated rather than joined: ytdl.ps1 takes one value per occurrence,
         * and a real --match-filter expression contains commas and spaces, so
         * no separator would be safe to join on. */
        for raw in ytdlpArgs where RunOptions.isNonEmpty(raw) {
            v.append("--ytdlp-arg")
            v.append(raw.trimmingCharacters(in: .whitespacesAndNewlines))
        }

        return v
    }

    /* What the equivalent terminal command would be. Shown above the Add
     * button, because a GUI that hides the command it runs makes the CLI harder
     * to learn rather than easier -- and every problem report is easier to
     * answer when the user can paste the exact line the window would have run.
     *
     * What is shown must be pasteable into a terminal as-is: the URL is always
     * quoted, anything containing a space is quoted, nothing else is. */
    func commandPreview() -> String {
        var s = "ytdl"
        for (i, arg) in toArgs().enumerated() {
            s += " "
            if i == 0 || arg.contains(" ") {
                s += "\"\(arg)\""
            } else {
                s += arg
            }
        }
        return s
    }

    /* A bare video id becomes a watch URL; anything already URL-shaped is left
     * exactly as typed. Deliberately not a validator -- ytdl.ps1 has its own
     * "that does not look like a URL" warning and yt-dlp's extractor is the
     * real authority on what is downloadable. */
    static func normalizeURL(_ url: String) -> String {
        let u = url.trimmingCharacters(in: .whitespacesAndNewlines)
        if u.contains("://") { return u }

        let looksLikeID = u.count == 11 && u.allSatisfy {
            $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-" || $0 == "_")
        }
        if looksLikeID { return "https://www.youtube.com/watch?v=\(u)" }

        for prefix in ["youtube.com", "www.youtube.com", "youtu.be"] where u.hasPrefix(prefix) {
            return "https://\(u)"
        }
        return u
    }

    // MARK: Serialisation

    /* Shared with the profile store and with the queue and history files.
     *
     * There is deliberately NO second list of profileable fields anywhere: a
     * field added to RunOptions becomes profileable by being added here, and
     * there is no other place to forget it. */
    func toJSON() -> [String: Any] {
        var o: [String: Any] = [
            "sync": sync,
            "lazy": lazy,
            "workers": workers,
            "no_pot": noPot,
            "skip_pot_update": skipPotUpdate,
            "pot_port": potPort,
            "no_comments": noComments,
            "no_subs": noSubs,
            "no_thumbnail": noThumbnail,
            "no_metadata": noMetadata,
            "ytdlp_args": ytdlpArgs,
        ]
        for (k, v) in [
            ("url", url), ("data_root", dataRoot), ("items", items), ("after", after),
            ("mode", mode), ("quality", quality), ("codec", codec),
            ("audio_codec", audioCodec), ("container", container),
        ] where !v.isEmpty {
            o[k] = v
        }
        return o
    }

    /* Every field is optional on read. A queue, history or profiles file
     * written by an older build simply does not set what it did not know about
     * -- which is the reason a profile or a queued run survives an upgrade
     * instead of failing the whole file. */
    static func fromJSON(_ obj: [String: Any]?) -> RunOptions {
        var o = RunOptions()
        guard let obj else { return o }

        o.url = obj.str("url") ?? ""
        o.dataRoot = obj.str("data_root") ?? ""
        o.sync = obj.bool("sync")
        o.items = obj.str("items") ?? ""
        o.after = obj.str("after") ?? ""
        o.lazy = obj.bool("lazy")
        o.workers = Int(obj.int("workers"))
        o.noPot = obj.bool("no_pot")
        o.skipPotUpdate = obj.bool("skip_pot_update")
        o.potPort = Int(obj.int("pot_port"))
        o.mode = obj.str("mode") ?? ""
        o.quality = obj.str("quality") ?? ""
        o.codec = obj.str("codec") ?? ""
        o.audioCodec = obj.str("audio_codec") ?? ""
        o.container = obj.str("container") ?? ""
        o.noComments = obj.bool("no_comments")
        o.noSubs = obj.bool("no_subs")
        o.noThumbnail = obj.bool("no_thumbnail")
        o.noMetadata = obj.bool("no_metadata")
        o.ytdlpArgs = obj.strings("ytdlp_args")
        return o
    }
}

// MARK: - Progress

struct RunProgress: Equatable {
    /// < 0 when unknown.
    var percent: Double = -1
    var speed: String?
    var eta: String?
    var total: String?
    var stage: String?
    var videoID: String?
    var destination: String?

    mutating func clear() { self = RunProgress() }
}

// MARK: - A queued or finished run

struct RunRecord: Identifiable, Equatable {
    var id: String = ""
    var command: String = ""
    var started: Int64 = 0
    /// 0 while running.
    var finished: Int64 = 0
    /// queued | running | done | failed | cancelled
    var state: String = "queued"
    var exitCode: Int32 = 0
    /// -1 = not reported. The four counts exist ONLY in run_ytdlp.ps1's session
    /// summary line; a run that was cancelled or died early never printed one.
    var videosTouched: Int64 = -1
    var archiveSkipped: Int64 = -1
    var errors: Int64 = -1
    var warnings: Int64 = -1
    var logPath: String = ""
    var lastLine: String = ""
    var opts = RunOptions()

    func toJSON() -> [String: Any] {
        [
            "id": id,
            "command": command,
            "started": started,
            "finished": finished,
            "state": state,
            "exit_code": Int(exitCode),
            "videos_touched": videosTouched,
            "archive_skipped": archiveSkipped,
            "errors": errors,
            "warnings": warnings,
            "log_path": logPath,
            "last_line": lastLine,
            "opts": opts.toJSON(),
        ]
    }

    static func fromJSON(_ obj: [String: Any]) -> RunRecord {
        var r = RunRecord()
        r.id = obj.str("id") ?? ""
        r.command = obj.str("command") ?? ""
        r.started = obj.int("started")
        r.finished = obj.int("finished")
        r.state = obj.str("state") ?? "done"
        r.exitCode = Int32(truncatingIfNeeded: obj.int("exit_code"))
        r.videosTouched = obj.int("videos_touched", default: -1)
        r.archiveSkipped = obj.int("archive_skipped", default: -1)
        r.errors = obj.int("errors", default: -1)
        r.warnings = obj.int("warnings", default: -1)
        r.logPath = obj.str("log_path") ?? ""
        r.lastLine = obj.str("last_line") ?? ""
        r.opts = RunOptions.fromJSON(obj.object("opts"))
        return r
    }
}

// MARK: - Output parsing

/* The two parsers that turn the pipeline's stdout into everything the UI
 * shows, and the place a change in yt-dlp's output would first bite. Both are
 * pure functions over one line so the suite can pin them without a download. */
enum OutputParser {
    /* Strip ANSI escape sequences. yt-dlp turns colour off when stdout is not a
     * terminal, but pwsh does not always, and a progress line full of escape
     * bytes renders as garbage in a label. */
    static func stripANSI(_ s: String) -> String {
        var out = ""
        out.reserveCapacity(s.count)
        let chars = Array(s)
        var i = 0
        while i < chars.count {
            if chars[i] == "\u{1b}" {
                i += 1
                if i < chars.count, chars[i] == "[" {
                    i += 1
                    while i < chars.count, !(chars[i].isASCII && chars[i].isLetter) { i += 1 }
                    if i < chars.count { i += 1 }
                }
                continue
            }
            out.append(chars[i])
            i += 1
        }
        return out
    }

    private static let stages: [(needle: String, stage: String)] = [
        ("[Merger]", "merging"),
        ("[Metadata]", "embedding metadata"),
        ("[EmbedSubtitle]", "embedding subtitles"),
        ("[ThumbnailsConvertor]", "thumbnail"),
        ("[postprocess]", "post-processing"),
        ("Fetching comments", "fetching comments"),
        ("[info] Writing video subtitles", "subtitles"),
        ("-- Enumerating videos", "enumerating"),
    ]

    /* yt-dlp's own progress line, e.g.
     *   [download]  45.2% of  120.00MiB at   2.00MiB/s ETA 00:30
     * Scanned by token rather than with a regular expression, which keeps the
     * cost per line low: a long run produces thousands of these a minute.
     * Returns true when @p was changed. */
    static func parseProgressLine(_ line: String, into p: inout RunProgress) -> Bool {
        let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)

        if trimmed.hasPrefix("[download]") {
            let rest = String(trimmed.dropFirst("[download]".count))
            let rtrim = rest.trimmingCharacters(in: .whitespacesAndNewlines)

            if rtrim.hasPrefix("Destination:") {
                p.destination = String(rtrim.dropFirst("Destination:".count))
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                p.stage = "downloading"
                return true
            }

            /* Runs of spaces are normal in this line ("45.2% of  120.00MiB at
             * 2.00MiB/s"), so empty tokens are dropped rather than compacted in
             * place. The C port had a double free here from compacting the
             * token array; Swift cannot have that bug, but the shape of the
             * parse is the same one and worth keeping recognisable. */
            let tokens = rest.split(whereSeparator: { $0 == " " || $0 == "\t" }).map(String.init)
            var matched = false

            for (i, tok) in tokens.enumerated() {
                if tok.count > 1, tok.hasSuffix("%"),
                   let v = Double(String(tok.dropLast())) {
                    p.percent = v
                    p.stage = "downloading"
                    matched = true
                }
                if i + 1 < tokens.count {
                    switch tok {
                    case "of": p.total = tokens[i + 1]
                    case "at": p.speed = tokens[i + 1]
                    case "ETA": p.eta = tokens[i + 1]
                    default: break
                    }
                }
            }
            if matched { return true }
        }

        /* Stage markers worth surfacing, all emitted by the pipeline itself or
         * by yt-dlp's own extractor chatter. The percentage is CLEARED with
         * each: 45% of the download is not 45% of the merge, and leaving the
         * old number up reads as a stalled bar. */
        for (needle, stage) in stages where line.contains(needle) {
            p.stage = stage
            p.percent = -1
            return true
        }

        /* "[youtube] dQw4w9WgXcQ: Downloading webpage" -- the id of the video
         * the session is currently on, which is the only reliable per-video
         * marker in a multi-video run. */
        if trimmed.hasPrefix("[youtube] ") {
            let rest = String(trimmed.dropFirst("[youtube] ".count))
            if let colon = rest.firstIndex(of: ":") {
                let id = String(rest[rest.startIndex..<colon])
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                if id.count == 11, !id.contains(" ") {
                    p.videoID = id
                    return true
                }
            }
        }

        return false
    }

    struct SessionSummary: Equatable {
        let videos: Int64
        let skipped: Int64
        let errors: Int64
        let warnings: Int64
    }

    /* run_ytdlp.ps1 closes every session with
     *   -- Session summary: N video(s) touched, M already archived (skipped),
     *      E error(s), W warning(s) --
     * which is the only place those counts exist. Parsed here so a history row
     * can show them without re-reading download.log.
     *
     * Each whitespace token has its non-digit edges trimmed before parsing, so
     * "3," and "(skipped)" behave -- the latter yielding nothing. */
    static func parseSessionSummary(_ line: String) -> SessionSummary? {
        guard line.contains("Session summary:") else { return nil }

        var nums: [Int64] = []
        for token in line.split(whereSeparator: { $0 == " " || $0 == "\t" }) {
            if nums.count == 4 { break }

            let chars = Array(token)
            var start = 0
            while start < chars.count, !(chars[start].isASCII && chars[start].isNumber) {
                start += 1
            }
            var end = chars.count
            while end > start, !(chars[end - 1].isASCII && chars[end - 1].isNumber) {
                end -= 1
            }
            if end <= start { continue }

            let s = String(chars[start..<end])
            guard s.allSatisfy({ $0.isASCII && $0.isNumber }), let n = Int64(s) else { continue }
            nums.append(n)
        }

        guard nums.count == 4 else { return nil }
        return SessionSummary(
            videos: nums[0], skipped: nums[1], errors: nums[2], warnings: nums[3]
        )
    }
}
