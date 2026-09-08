/* Reading the archive that postprocess.ps1 writes.
 *
 * This is a FOURTH independent implementation of the layout contract in
 * docs/archive-layout.md -- archive-viewer.py reads it in Python, the Tauri
 * app in Rust, the GTK app in C, and this in Swift. That duplication is the
 * price of standalone apps with no shared engine, and the contract document
 * says plainly how it is paid: "Out-of-repo consumers are expected to keep
 * their own conformance test that builds a fixture tree in this shape and
 * asserts their reader finds it." Tests/ArchiveConformanceTests.swift is this
 * app's, and an app in this repo without one does not belong in it.
 *
 * THE ARCHIVE IS READ-ONLY. Nothing in this file creates, moves or modifies
 * anything under Youtube Videos/. postprocess.ps1 writes a checksums.sha256
 * covering every file in a video folder, so a derived file dropped in there
 * makes that manifest stop verifying. Derived state goes to ~/Library/Caches.
 *
 * Layout 2 rules that a layout-1 reader gets WRONG, implemented here:
 *   - the media file may be Final Video.<any ext> or Final Audio.<any ext>;
 *     match on base name, or better, read media_file from the manifest
 *   - a folder with NO media file at all is valid, not corrupt
 *   - Pre-merge streams/ must be skipped when choosing "the" video, or you
 *     pick a silent video or a black audio track
 */

import Foundation

/// The highest layout version this reader understands. A video written with a
/// higher one is shown on a best-effort basis and flagged, never hidden: an
/// empty library with no explanation is the outcome the contract exists to
/// prevent.
///
/// Asserted against `REQUIRES_ARCHIVE_LAYOUT` in the repository's CLI_VERSION
/// by the conformance suite. Two copies of one fact that must not drift.
let supportedArchiveLayout: Int64 = 2

// MARK: - Extension sets

enum MediaExtensions {
    /* Lowercased, with the leading dot, so a comparison is one Set lookup
     * against the value stored on ArchiveFile. */
    static let video: Set<String> = [".mkv", ".mp4", ".webm", ".m4v", ".mov", ".avi", ".flv", ".ts"]
    static let audio: Set<String> = [".m4a", ".opus", ".mp3", ".flac", ".ogg", ".wav", ".aac"]
    static let image: Set<String> = [".png", ".jpg", ".jpeg", ".webp", ".gif", ".avif"]
    /* --write-subs and --write-auto-subs both land in Subtitles/; the
     * container formats yt-dlp can be asked for are these. */
    static let subtitle: Set<String> = [".vtt", ".srt", ".ass", ".ssa", ".sub", ".lrc"]

    /// Whether an extension (lowercased, with the leading dot, as ArchiveFile
    /// stores it) names a subtitle file.
    static func isSubtitle(_ ext: String?) -> Bool {
        guard let ext, !ext.isEmpty else { return false }
        return subtitle.contains(ext)
    }
}

// MARK: - One file inside a video folder

struct ArchiveFile: Identifiable, Hashable {
    /// Folder-relative, always '/'-separated.
    let rel: String
    /// Lowercased, WITH the leading dot; "" if none.
    let ext: String
    /// The top-level subfolder, e.g. "Final files"; "" at the folder root.
    let folder: String
    let size: UInt64

    var id: String { rel }

    /// Lowercased extension including the dot, or "" when there is none. Uses
    /// the LAST dot of the basename only, so "Final Video.f137.mp4" gives
    /// ".mp4".
    static func ext(of rel: String) -> String {
        let base = (rel as NSString).lastPathComponent
        guard let dot = base.lastIndex(of: "."), dot != base.startIndex else { return "" }
        return String(base[dot...]).lowercased()
    }
}

// MARK: - One video folder

struct ArchiveEntry: Identifiable, Hashable {
    /// Opaque; see `Paths.key(for:)`. The only handle the UI ever holds.
    let key: String
    /// Absolute path to the video folder.
    let dir: String
    /// Path relative to the archive root.
    let rel: String
    /// The uploader folder's name.
    let channel: String

    /// 0 means manifest.json carried no archive_layout_version, which the
    /// contract defines as layout 1 -- not an error and not worth a warning.
    var layoutVersion: Int64 = 0
    var layoutTooNew: Bool = false

    /* The YouTube id, which is NOT this struct's Identifiable id: it can be
     * absent (an unparseable folder name with no info.json), and two entries
     * with a nil id would collide in a ForEach. The opaque key identifies an
     * entry; the video id is data about it. */
    var videoID: String?
    var title: String = ""
    var uploader: String = ""
    var uploadDate: String?
    var channelURL: String?
    var originalURL: String?
    /// Which --mode wrote this folder; nil when the manifest does not say.
    var downloadMode: String?
    /// The manifest's media_file, folder-relative; nil when absent.
    var mediaFile: String?
    var timestamp: Int64 = -1
    var viewCount: Int64 = -1
    var duration: Double = -1

    var files: [ArchiveFile] = []

    /// Identifiable, and deliberately by the opaque key.
    var id: String { key }

    static func == (lhs: ArchiveEntry, rhs: ArchiveEntry) -> Bool { lhs.key == rhs.key }
    func hash(into hasher: inout Hasher) { hasher.combine(key) }
}

extension ArchiveEntry {
    /* "Final Video.f137.mp4" -- a --keep-video leftover that yt-dlp names with
     * a format-id segment. Excluded even when it sits in Final files/, because
     * picking one gives a silent video or a black audio track. */
    static func hasFormatIDSegment(_ rel: String) -> Bool {
        let base = (rel as NSString).lastPathComponent
        let stem = (base as NSString).deletingPathExtension
        guard base.contains("."), stem.contains(".") else { return false }
        guard let prevDot = stem.lastIndex(of: ".") else { return false }

        let segment = stem[stem.index(after: prevDot)...]
        guard segment.hasPrefix("f") else { return false }
        let digits = segment.dropFirst()
        if digits.isEmpty { return false }
        return digits.allSatisfy { $0.isASCII && $0.isNumber }
    }

    private static func basenameStartsWith(_ rel: String, _ want: String) -> Bool {
        (rel as NSString).lastPathComponent.hasPrefix(want)
    }

    /// The index into `files` of the media file, or nil -- which is ORDINARY.
    /// --mode metadata-only, comments-only and subs-only all write a complete
    /// folder with no media in it, and so does an interrupted run; the contract
    /// says not to try to tell them apart by guessing. `downloadMode` says
    /// which.
    var mediaIndex: Int? {
        /* 1. The manifest says so outright. Preferred over globbing at all,
         *    per the contract -- it is the only answer that stays right when
         *    --container or --audio-codec changes the extension. */
        if let named = mediaFile {
            let want = named.replacingOccurrences(of: "\\", with: "/")
            if let hit = files.firstIndex(where: { $0.rel == want }) {
                return hit
            }
            /* Named but absent: fall through and glob. A manifest naming a file
             * somebody has since deleted should degrade to "no media", not to a
             * broken path. */
        }

        /* 2. Glob by BASE NAME, never by extension. Matching on .mkv was
         *    correct under layout 1 and is a bug under layout 2. Video wins
         *    over audio when a folder somehow holds both. */
        var audioHit: Int?
        for (i, f) in files.enumerated() {
            if f.folder == "Pre-merge streams" { continue }
            if ArchiveEntry.hasFormatIDSegment(f.rel) { continue }
            if !ArchiveEntry.basenameStartsWith(f.rel, "Final Video."),
               !ArchiveEntry.basenameStartsWith(f.rel, "Final Audio.") {
                continue
            }

            if MediaExtensions.video.contains(f.ext) { return i }
            if audioHit == nil, MediaExtensions.audio.contains(f.ext) { audioHit = i }
        }
        if let audioHit { return audioHit }

        /* 3. None. An ORDINARY state, not corrupt and not to be hidden. */
        return nil
    }

    /// The index into `files` of the best thumbnail, or nil.
    var thumbnailIndex: Int? {
        var best: Int?
        var bestWeight: UInt64 = 0
        for (i, f) in files.enumerated() {
            guard MediaExtensions.image.contains(f.ext) else { continue }
            /* Images/ is where postprocess.ps1 puts them; anything elsewhere is
             * a fallback so a hand-reorganised folder still shows something. */
            let preferred: UInt64 = f.folder == "Images" ? (UInt64(1) << 40) : 0
            let weight = f.size &+ preferred
            if best == nil || weight > bestWeight {
                best = i
                bestWeight = weight
            }
        }
        return best
    }

    /// Resolve a file index to an absolute path, re-checking that the result is
    /// inside `dir`. nil for an out-of-range index or a path that escapes the
    /// folder. This is the ONLY way a path is produced: no caller supplies one.
    func path(forIndex idx: Int) -> String? {
        guard idx >= 0, idx < files.count else { return nil }
        let joined = Paths.join(dir, files[idx].rel)

        /* Re-check containment even though the relative path came from our own
         * listing. It costs one string compare and it is what makes "the caller
         * never supplies a path" an enforced property rather than a
         * convention. */
        let canon = Paths.canonical(joined)
        let base = Paths.canonical(dir)
        guard canon.hasPrefix(base + "/") else { return nil }
        return canon
    }

    var mediaPath: String? {
        guard let idx = mediaIndex else { return nil }
        return path(forIndex: idx)
    }

    var thumbnailPath: String? {
        guard let idx = thumbnailIndex else { return nil }
        return path(forIndex: idx)
    }

    var totalBytes: UInt64 {
        files.reduce(UInt64(0)) { $0 &+ $1.size }
    }
}

// MARK: - Folder-name fallback

enum FolderName {
    struct Parsed {
        var uploader: String
        var uploadDate: String
        var id: String
        var title: String
    }

    private static func isVideoID(_ s: Substring) -> Bool {
        guard s.count == 11 else { return false }
        return s.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-" || $0 == "_") }
    }

    private static func isEightDigits(_ s: Substring) -> Bool {
        guard s.count == 8 else { return false }
        return s.allSatisfy { $0.isASCII && $0.isNumber }
    }

    /* "<uploader> - <YYYYMMDD> - <id> - <title>".
     *
     * Not a left-to-right split with a field limit, because BOTH the uploader
     * and the title routinely contain " - " themselves. The date and the id are
     * the only two fields with a checkable shape, so the parse anchors on
     * finding an 8-digit run immediately followed by an 11-character id and
     * works outwards from there. Everything left of the date is the uploader;
     * everything right of the id is the title, rejoined with its separators
     * intact.
     *
     * nil when the name does not carry a date and an id, in which case the
     * caller falls back to the whole folder name -- the documented fallback,
     * pinned directly by the conformance suite. */
    static func parse(_ name: String) -> Parsed? {
        let parts = name.components(separatedBy: " - ")
        guard parts.count >= 3 else { return nil }

        for i in 1..<(parts.count - 1) {
            guard isEightDigits(Substring(parts[i])) else { continue }
            guard isVideoID(Substring(parts[i + 1])) else { continue }

            return Parsed(
                uploader: parts[0..<i].joined(separator: " - "),
                uploadDate: parts[i],
                id: parts[i + 1],
                title: parts[(i + 2)...].joined(separator: " - ")
            )
        }
        return nil
    }
}

// MARK: - The index

struct ArchiveIndex {
    var root: String = ""
    var entries: [ArchiveEntry] = []
    /// Uploader folder names that hold at least one video, sorted.
    var channels: [String] = []

    private var byKey: [String: Int] = [:]

    func entry(forKey key: String?) -> ArchiveEntry? {
        guard let key, let idx = byKey[key] else { return nil }
        return entries[idx]
    }

    var videoCount: Int { entries.count }
    var channelCount: Int { channels.count }
    var totalBytes: UInt64 { entries.reduce(UInt64(0)) { $0 &+ $1.totalBytes } }

    // MARK: Scanning

    enum ScanError: LocalizedError {
        case rootNotADirectory(String)

        var errorDescription: String? {
            switch self {
            case .rootNotADirectory(let root):
                return "The archive root \(root) is not a directory. Point Settings at "
                    + "the same path you would pass to `ytdl --path`."
            }
        }
    }

    /* Walk <root>/<Uploader>/<video folder>/ and build the index.
     *
     * Throws only when the root itself cannot be read. An individual unreadable
     * or malformed video folder is not an error: it is indexed from its folder
     * name, because that is the documented fallback and a real state that real
     * runs produce.
     *
     * BLOCKS. Call it off the main thread -- `progress` is called once per
     * video folder on whichever thread called this. */
    static func scan(
        root: String,
        progress: ((Int, Int, String) -> Void)? = nil
    ) throws -> ArchiveIndex {
        guard Paths.isDirectory(root) else {
            throw ScanError.rootNotADirectory(root)
        }

        var index = ArchiveIndex()
        index.root = root

        let discovered = discover(root: root)
        index.channels = discovered.channels

        for (i, rel) in discovered.relativePaths.enumerated() {
            guard let slash = rel.firstIndex(of: "/") else { continue }
            let channel = String(rel[rel.startIndex..<slash])
            let folder = String(rel[rel.index(after: slash)...])

            let entry = buildEntry(root: root, channel: channel, folderName: folder)
            /* Last writer wins on a key collision, which two identical relative
             * paths cannot produce -- so this is only reachable via a SHA-256
             * truncation collision, and inserting anyway keeps the dictionary
             * and the array consistent. */
            index.byKey[entry.key] = index.entries.count
            index.entries.append(entry)

            progress?(i + 1, discovered.relativePaths.count, entry.title)
        }

        return index
    }

    /* Channel folders, then video folders inside each. "Channel Info" is a
     * channel-level asset directory, not a video, and is skipped by name. */
    private static func discover(root: String) -> (channels: [String], relativePaths: [String]) {
        let fm = FileManager.default
        guard let channelNames = try? fm.contentsOfDirectory(atPath: root) else {
            return ([], [])
        }

        var channels: [String] = []
        var rels: [String] = []

        for channel in channelNames.sorted() {
            let cdir = Paths.join(root, channel)
            guard Paths.isDirectory(cdir) else { continue }
            guard let videoNames = try? fm.contentsOfDirectory(atPath: cdir) else { continue }

            var videos: [String] = []
            for name in videoNames.sorted() {
                if name == "Channel Info" { continue }
                guard Paths.isDirectory(Paths.join(cdir, name)) else { continue }
                videos.append(name)
            }
            if videos.isEmpty { continue }

            channels.append(channel)
            for v in videos { rels.append("\(channel)/\(v)") }
        }

        return (channels, rels)
    }

    // MARK: Building one entry

    static func buildEntry(root: String, channel: String, folderName: String) -> ArchiveEntry {
        let rel = "\(channel)/\(folderName)"
        let dir = Paths.join(Paths.join(root, channel), folderName)

        var entry = ArchiveEntry(
            key: Paths.key(for: rel),
            dir: dir,
            rel: rel,
            channel: channel
        )

        /* The folder name FIRST, so that manifest and info.json are corrections
         * to a value that already exists rather than the only source. A missing
         * or unparseable info.json is a documented, ordinary state, and the
         * folder name is the documented fallback -- so the fallback is simply
         * always applied, and the good sources overwrite it. */
        if let parsed = FolderName.parse(folderName) {
            entry.uploader = parsed.uploader
            entry.uploadDate = parsed.uploadDate
            entry.videoID = parsed.id
            entry.title = parsed.title
        }
        if entry.title.isEmpty { entry.title = folderName }
        if entry.uploader.isEmpty { entry.uploader = channel }

        let metaDir = Paths.join(dir, "Video metadata")
        applyManifest(&entry, metaDir: metaDir)
        applyInfoJSON(&entry, metaDir: metaDir)

        entry.files = listFiles(dir: dir).sorted { $0.rel < $1.rel }
        return entry
    }

    private static func applyManifest(_ entry: inout ArchiveEntry, metaDir: String) {
        guard let obj = JSONFile.object(at: Paths.join(metaDir, "manifest.json")) else { return }

        /* Absent means the video predates versioning, which the contract
         * defines as layout 1. Not an error, and not something to warn about. */
        entry.layoutVersion = obj.int("archive_layout_version", default: 0)
        entry.layoutTooNew = entry.layoutVersion > supportedArchiveLayout

        entry.mediaFile = obj.str("media_file")
        entry.downloadMode = obj.str("download_mode")

        if entry.videoID == nil { entry.videoID = obj.str("video_id") }
        if let t = obj.str("title"), entry.title.isEmpty { entry.title = t }
        if entry.uploader.isEmpty, let u = obj.str("uploader") { entry.uploader = u }
        if entry.uploadDate == nil { entry.uploadDate = obj.str("upload_date") }
        if entry.originalURL == nil { entry.originalURL = obj.str("original_url") }
        if entry.channelURL == nil { entry.channelURL = obj.str("channel_url") }

        /* run_settings may legitimately be absent -- a video written by a
         * standalone postprocess.ps1 invocation has none. */
        if entry.downloadMode == nil, let run = obj.object("run_settings") {
            entry.downloadMode = run.str("mode")
        }
    }

    /* The richer fields live in yt-dlp's own info.json. Named
     * "<something>.info.json" rather than a fixed name, so the directory is
     * scanned for the suffix. */
    static func infoJSONPath(inMetaDir metaDir: String) -> String? {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: metaDir) else {
            return nil
        }
        guard let found = names.sorted().first(where: { $0.hasSuffix(".info.json") }) else {
            return nil
        }
        return Paths.join(metaDir, found)
    }

    private static func applyInfoJSON(_ entry: inout ArchiveEntry, metaDir: String) {
        guard let path = infoJSONPath(inMetaDir: metaDir),
              let obj = JSONFile.object(at: path) else { return }

        entry.duration = obj.double("duration", default: entry.duration)
        entry.viewCount = obj.int("view_count", default: entry.viewCount)
        entry.timestamp = obj.int("timestamp", default: entry.timestamp)

        if let t = obj.str("title") { entry.title = t }
        if let i = obj.str("id") { entry.videoID = i }
        if let d = obj.str("upload_date") { entry.uploadDate = d }
        /* "uploader" is the display name; "channel" is the fallback some
         * extractors fill instead. */
        if let u = obj.str("uploader") {
            entry.uploader = u
        } else if let c = obj.str("channel") {
            entry.uploader = c
        }
        if let c = obj.str("channel_url") { entry.channelURL = c }
        if let w = obj.str("webpage_url") { entry.originalURL = w }
    }

    // MARK: Listing a video folder

    private static func listFiles(dir: String) -> [ArchiveFile] {
        var out: [ArchiveFile] = []
        walk(dir: dir, prefix: "", folder: "", depth: 0, into: &out)
        return out
    }

    private static func walk(
        dir: String,
        prefix: String,
        folder: String,
        depth: Int,
        into out: inout [ArchiveFile]
    ) {
        /* The contract fixes the shape at <video folder>/<subfolder>/<file>.
         * Three levels is slack for a subfolder someone nests one deeper;
         * unbounded recursion into a user-chosen directory is not something to
         * offer. */
        if depth > 3 { return }

        let fm = FileManager.default
        guard let names = try? fm.contentsOfDirectory(atPath: dir) else { return }

        for name in names {
            let full = Paths.join(dir, name)
            let rel = prefix.isEmpty ? name : "\(prefix)/\(name)"

            if Paths.isDirectory(full) {
                /* At depth 0 the child IS the top-level subfolder, and every
                 * file beneath it is attributed to that folder -- which is what
                 * makes "skip Pre-merge streams/" a single comparison later. */
                let childFolder = depth == 0 ? name : folder
                walk(dir: full, prefix: rel, folder: childFolder, depth: depth + 1, into: &out)
                continue
            }
            guard Paths.isRegularFile(full) else { continue }

            let attrs = try? fm.attributesOfItem(atPath: full)
            let size = (attrs?[.size] as? NSNumber)?.uint64Value ?? 0

            out.append(ArchiveFile(
                rel: rel,
                ext: ArchiveFile.ext(of: rel),
                folder: folder,
                size: size
            ))
        }
    }
}
