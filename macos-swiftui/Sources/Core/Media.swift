/* What is actually inside the media file, and whether this window can play it.
 *
 * The archive's whole point is that the ORIGINAL file is kept, so the useful
 * question on a video's page is "what did I actually get" -- 1080p VP9 with
 * Opus and three subtitle tracks, or a 360p fallback nobody noticed at the
 * time. That answer is in the container, not in info.json: info.json records
 * what yt-dlp was asked for and what it believed it fetched, while the file
 * records what is on disk now.
 *
 * WHY ffprobe AND NOT AN EBML PARSER. Hand-rolling Matroska would fit this
 * project's dependency principle and would literally "parse mkv" -- but the
 * --container option also produces .mp4 and .webm, and --mode audio-only
 * produces .m4a and .opus. A Matroska parser answers for one of those. ffprobe
 * answers for all of them, is already a documented dependency of the pipeline,
 * and is already probed for on the Health pane. The cost is honest and bounded:
 * no ffprobe, no stream details, and the page says so rather than going blank.
 *
 * AVFoundation is NOT used for this. It would read an .mp4 without spawning
 * anything, and then answer nothing at all for the .mkv that is this
 * pipeline's default container -- which is the majority of any real archive.
 *
 * READ-ONLY, like everything else here. This spawns ffprobe. It never spawns
 * ffmpeg: nothing is re-encoded, remuxed or written.
 */

import AVFoundation
import Foundation

struct MediaStream: Identifiable {
    /// video | audio | subtitle | attachment | data
    let kind: String
    /// Short name, e.g. "vp9", "opus", "webvtt".
    let codec: String?
    let profile: String?
    /// From the track's language tag; nil when untagged.
    let language: String?
    /// The track's own title, when it has one.
    let title: String?
    let index: Int

    // Video only; 0 when not applicable.
    let width: Int
    let height: Int
    let fps: Double

    // Audio only; 0 when not applicable.
    let channels: Int
    let sampleRate: Int

    /// Bits/sec, 0 when the container does not say.
    let bitRate: Int64
    let isDefault: Bool
    /// An attached cover image. ffprobe reports one as a one-frame video
    /// stream, which is exactly how the webview build ended up serving
    /// thumbnails with a video MIME type -- so it is flagged rather than
    /// counted as video.
    let attachedPic: Bool

    var id: Int { index }
}

struct MediaChapter: Identifiable {
    let title: String
    let start: Double
    let end: Double

    var id: String { "\(start)-\(title)" }
}

struct MediaProbe {
    /// false when ffprobe is missing or the file is unreadable.
    var ok = false
    /// Why, when !ok -- shown to the user verbatim.
    var error: String?
    /// Container long name.
    var format: String?
    var duration: Double = 0
    var size: UInt64 = 0
    var bitRate: Int64 = 0
    var streams: [MediaStream] = []
    var chapters: [MediaChapter] = []
}

enum Media {
    /* Runs ffprobe and parses its JSON. BLOCKS -- call it off the main thread.
     * Never fails: a failure comes back as a probe with ok = false and a
     * message, because "ffprobe is not installed" is something the page should
     * say rather than something it should hide. */
    static func probe(path: String?) -> MediaProbe {
        guard let path, Paths.isRegularFile(path) else {
            return failed("There is no media file in this folder to inspect.")
        }
        guard let ffprobe = findFFprobe() else {
            return failed(
                "ffprobe is not installed, so the stream details cannot be read. Everything "
                + "else on this page still works, and the file itself is untouched. Install "
                + "ffmpeg to get this section back."
            )
        }

        let process = Process()
        process.executableURL = URL(fileURLWithPath: ffprobe)
        process.arguments = [
            "-v", "quiet", "-print_format", "json",
            "-show_format", "-show_streams", "-show_chapters",
            path,
        ]
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = Paths.childPath()
        process.environment = env

        let outPipe = Pipe()
        process.standardOutput = outPipe
        process.standardError = FileHandle.nullDevice
        process.standardInput = FileHandle.nullDevice

        do {
            try process.run()
        } catch {
            return failed("Could not run ffprobe: \(error.localizedDescription)")
        }
        let data = outPipe.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()

        guard process.terminationStatus == 0 else {
            return failed(
                "ffprobe could not read this file. It may be truncated — an interrupted run "
                + "leaves a partial media file behind, which is a state the archive layout "
                + "says to tolerate rather than treat as corruption."
            )
        }
        guard let obj = JSONFile.object(from: data) else {
            return failed("ffprobe returned something that is not the JSON it was asked for.")
        }

        var p = MediaProbe()
        p.ok = true

        if let f = obj.object("format") {
            p.format = f.str("format_long_name")
            p.duration = f.double("duration")
            p.size = UInt64(max(0, f.int("size")))
            p.bitRate = f.int("bit_rate")
        }
        p.streams = obj.objects("streams").map(readStream)
        p.chapters = obj.objects("chapters").map(readChapter)
        if p.format == nil { p.format = "unknown container" }
        return p
    }

    private static func failed(_ message: String) -> MediaProbe {
        var p = MediaProbe()
        p.error = message
        return p
    }

    private static func findFFprobe() -> String? {
        if let onPath = Paths.which("ffprobe") { return onPath }
        for prefix in ["/opt/homebrew/bin", "/usr/local/bin"] {
            let candidate = Paths.join(prefix, "ffprobe")
            if FileManager.default.isExecutableFile(atPath: candidate) { return candidate }
        }
        return nil
    }

    private static func readStream(_ o: [String: Any]) -> MediaStream {
        let tags = o.object("tags")
        let language = tags?.str("language")
        let disposition = o.object("disposition")

        return MediaStream(
            kind: o.str("codec_type") ?? "data",
            codec: o.str("codec_name"),
            profile: o.str("profile"),
            /* "und" is Matroska's "undetermined", which is not a language and
             * should not be shown as though it were one. */
            language: language == "und" ? nil : language,
            title: tags?.str("title"),
            index: Int(o.int("index")),
            width: Int(o.int("width")),
            height: Int(o.int("height")),
            /* avg_frame_rate is an exact rational ("30000/1001"), which is the
             * honest representation and useless to display. */
            fps: rational(o.str("avg_frame_rate")),
            channels: Int(o.int("channels")),
            sampleRate: Int(o.int("sample_rate")),
            bitRate: o.int("bit_rate"),
            isDefault: (disposition?.int("default") ?? 0) != 0,
            attachedPic: (disposition?.int("attached_pic") ?? 0) != 0
        )
    }

    private static func readChapter(_ o: [String: Any]) -> MediaChapter {
        MediaChapter(
            title: o.object("tags")?.str("title") ?? "(untitled chapter)",
            start: o.double("start_time"),
            end: o.double("end_time")
        )
    }

    static func rational(_ s: String?) -> Double {
        guard let s else { return 0 }
        guard let slash = s.firstIndex(of: "/") else { return Double(s) ?? 0 }
        let n = Double(s[s.startIndex..<slash]) ?? 0
        let d = Double(s[s.index(after: slash)...]) ?? 0
        return d != 0 ? n / d : 0
    }

    /// A one-line summary for a header: "1080p · vp9 / opus · 9:47 · 412 MB".
    static func summary(_ p: MediaProbe) -> String {
        guard p.ok else { return "" }

        var video: MediaStream?
        var audio: MediaStream?
        var subs = 0
        for s in p.streams {
            if s.kind == "video", !s.attachedPic, video == nil {
                video = s
            } else if s.kind == "audio", audio == nil {
                audio = s
            } else if s.kind == "subtitle" {
                subs += 1
            }
        }

        var parts: [String] = []
        if let v = video, v.height > 0 { parts.append("\(v.height)p") }
        if video != nil || audio != nil {
            var codecs = video?.codec ?? "—"
            if let a = audio?.codec { codecs += " / \(a)" }
            parts.append(codecs)
        }
        if p.duration > 0 { parts.append(Format.clock(p.duration)) }
        if p.size > 0 { parts.append(Format.bytes(p.size)) }

        var out = parts.joined(separator: " · ")
        if subs > 0 {
            out += " · \(subs) subtitle track\(subs == 1 ? "" : "s")"
        }
        return out
    }

    // MARK: - Playback

    /* Whether AVFoundation can be expected to play this file in-window.
     *
     * THIS IS THE ONE PLACE macOS IS HARDER THAN LINUX. GStreamer plays the
     * pipeline's default .mkv directly, so the GTK app shows the original file.
     * AVFoundation does not read Matroska at all -- and it will not say so
     * usefully either: an AVPlayer handed an .mkv shows a black rectangle and
     * reports a generic "cannot be opened" long after the page has drawn.
     *
     * The webview build's answer was to remux .mkv to WebM into a cache
     * directory before it could show anything. That apparatus is exactly what
     * this project deleted, and rebuilding it here would mean writing derived
     * state for something the user did not ask for. So the answer is to degrade
     * HONESTLY: play what AVFoundation plays, and for everything else say why
     * and offer to open the file in IINA, mpv or VLC, which read it directly.
     * `--container mp4` is a supported pipeline option, so a user who wants
     * in-window playback for everything has a real answer.
     *
     * Decided from the extension rather than by asking AVAsset. AVURLAsset's
     * `isPlayable` requires an async load, answers optimistically for some
     * containers it then fails on, and cannot be consulted while the page is
     * being built. The extension is what actually determines this, and a short
     * explicit list is honest about what was decided. */
    static let inWindowPlayableExtensions: Set<String> = [
        ".mp4", ".m4v", ".mov", ".m4a", ".mp3", ".wav", ".aac", ".flac",
    ]

    static func canPlayInWindow(path: String?) -> Bool {
        guard let path else { return false }
        let ext = ArchiveFile.ext(of: path)
        return inWindowPlayableExtensions.contains(ext)
    }

    /// A sentence saying why the player is not showing, naming the container.
    static func playbackUnavailableNote(path: String?) -> String {
        let ext = path.map { ArchiveFile.ext(of: $0) } ?? ""
        let name = ext.isEmpty ? "this file" : "a \(ext.dropFirst()) file"
        return "AVFoundation does not play \(name), so it cannot play in this window. "
            + "The file itself is fine and is untouched — open it in IINA, mpv or VLC, which "
            + "read it directly. Downloading with --container mp4 gives you in-window "
            + "playback for future videos."
    }
}
