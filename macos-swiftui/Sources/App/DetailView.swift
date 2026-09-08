/* One video's page.
 *
 * Everything shown here was already on disk and unread: the description and
 * metadata from info.json, the comment tree yt-dlp captured, the subtitle track
 * as prose, the file inventory, and what is actually inside the media
 * container. The Library tells you what you have; this tells you what it is.
 *
 * THE PLAYER PLAYS THE ORIGINAL FILE, or says why it cannot. The webview build
 * remuxed .mkv to WebM into a cache directory because a browser engine cannot
 * play Matroska; AVFoundation cannot either, and the answer here is to degrade
 * honestly rather than to rebuild the remux apparatus this project deleted.
 * Nothing on this page writes anything, anywhere.
 */

import AVKit
import SwiftUI

struct DetailView: View {
    @EnvironmentObject private var model: AppModel
    let key: String

    @State private var loaded: LoadedDetail?
    @State private var loading = true
    @State private var player: AVPlayer?
    @State private var verifyText = ""
    @State private var verifyVariant: Pill.Variant = .neutral
    @State private var tab: Tab = .details

    enum Tab: String, CaseIterable, Identifiable {
        case details, media, comments, transcript, files
        var id: String { rawValue }

        var title: String {
            switch self {
            case .details: return "Details"
            case .media: return "Media"
            case .comments: return "Comments"
            case .transcript: return "Transcript"
            case .files: return "Files"
            }
        }

        var symbol: String {
            switch self {
            case .details: return "info.circle"
            case .media: return "waveform"
            case .comments: return "text.bubble"
            case .transcript: return "text.alignleft"
            case .files: return "folder"
            }
        }
    }

    private var entry: ArchiveEntry? { model.entry(forKey: key) }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            header
            playerArea
            actions
            Picker("", selection: $tab) {
                ForEach(Tab.allCases) { t in
                    Label(t.title, systemImage: t.symbol).tag(t)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .padding(.horizontal, 14)

            Divider()
            tabContent
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .navigationTitle(entry?.title ?? "Video")
        .task(id: key) { await load() }
        .onDisappear {
            /* Leaving the page stops playback. An AVPlayer left holding an item
             * goes on decoding audio after the window has moved elsewhere, and
             * sound continuing after Back is the kind of thing people remember
             * about an application. Hung on the view's own teardown rather than
             * on a back button, because there are several ways out of a page and
             * only one of them is the button. */
            player?.pause()
            player?.replaceCurrentItem(with: nil)
            player = nil
        }
    }

    // MARK: - Header

    @ViewBuilder
    private var header: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 8) {
                Text(subtitleLine)
                    .font(.headline)
                    .fixedSize(horizontal: false, vertical: true)
                if loading {
                    ProgressView().controlSize(.small).scaleEffect(0.7)
                }
                Spacer()
            }

            Text(loading ? "Reading…" : (loaded.map { Media.summary($0.probe) } ?? ""))
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            if let entry {
                HStack(spacing: 6) {
                    if entry.layoutTooNew {
                        Pill(text: "written with a newer archive layout", variant: .warn)
                    }
                    if entry.mediaIndex == nil {
                        Pill(text: entry.downloadMode ?? "no media file")
                    }
                }
            }
        }
        .padding(.horizontal, 14)
        .padding(.top, 10)
    }

    private var subtitleLine: String {
        guard let entry else { return "" }
        let date = Format.uploadDate(entry.uploadDate)
        return date.isEmpty ? entry.uploader : "\(entry.uploader) · \(date)"
    }

    // MARK: - Player

    @ViewBuilder
    private var playerArea: some View {
        if let path = entry?.mediaPath {
            if Media.canPlayInWindow(path: path) {
                /* The player is built in .task, not here. Creating it inside
                 * the body would mean assigning @State during a view update,
                 * which SwiftUI warns about and which builds a second AVPlayer
                 * on every redraw. */
                if let player {
                    VideoPlayer(player: player)
                        .frame(height: 320)
                        .padding(.horizontal, 14)
                } else {
                    Color.clear.frame(height: 320)
                }
            } else {
                /* The honest note, naming the container and the way out. What
                 * it must NOT do is quietly remux into a cache directory: that
                 * is the apparatus this project deleted, and it would be
                 * writing derived state for something nobody asked for. */
                HStack(alignment: .top, spacing: 8) {
                    Image(systemName: "play.slash")
                        .foregroundStyle(.secondary)
                    Text(Media.playbackUnavailableNote(path: path))
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding(12)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 8))
                .padding(.horizontal, 14)
            }
        }
    }

    // MARK: - Actions

    private var actions: some View {
        HStack(spacing: 8) {
            if let path = entry?.mediaPath {
                Button {
                    ExternalOpen.inPlayer(path)
                } label: {
                    Label("Open in \(ExternalOpen.availablePlayerName() ?? "player")",
                          systemImage: "play.rectangle")
                }
                .help("IINA, mpv and VLC play every codec combination this pipeline produces "
                      + "and read the embedded subtitles and chapters.")
            }

            Button {
                if let dir = entry?.dir { ExternalOpen.revealInFinder(dir) }
            } label: {
                Label("Reveal in Finder", systemImage: "folder")
            }

            Button {
                verify()
            } label: {
                Label("Verify checksums", systemImage: "checkmark.seal")
            }
            .help("Re-hashes every file in this folder against the checksums.sha256 "
                  + "postprocess.ps1 wrote.")

            if !verifyText.isEmpty {
                Text(verifyText)
                    .font(.caption)
                    .foregroundStyle(verifyVariant == .error ? Color.red
                                     : verifyVariant == .ok ? Color.green : Color.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer()
        }
        .padding(.horizontal, 14)
    }

    private func verify() {
        guard let dir = entry?.dir else { return }
        verifyText = "Verifying…"
        verifyVariant = .neutral

        /* Hashing every file in a folder is seconds of work on a large video, so
         * it goes off the main thread like everything else here that touches the
         * disk in bulk. */
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Health.verifyChecksums(videoDir: dir)
            DispatchQueue.main.async {
                if !result.present {
                    verifyText = "No checksums.sha256 in this folder."
                    verifyVariant = .neutral
                } else if result.failed.isEmpty && result.missing.isEmpty {
                    verifyText = "All \(result.checked) files verify."
                    verifyVariant = .ok
                } else {
                    /* video_postprocessing.log is EXCLUDED from
                     * checksums.sha256 by postprocess.ps1 because it is still
                     * being appended to when the hashes are computed -- so it is
                     * never one of these, and anything listed here is a real
                     * mismatch. */
                    verifyText = "\(result.ok) of \(result.checked) verify · "
                        + "\(result.failed.count) failed · \(result.missing.count) missing"
                    verifyVariant = .error
                }
            }
        }
    }

    // MARK: - Tabs

    @ViewBuilder
    private var tabContent: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 10) {
                switch tab {
                case .details: detailsTab
                case .media: mediaTab
                case .comments: commentsTab
                case .transcript: transcriptTab
                case .files: filesTab
                }
            }
            .frame(maxWidth: 900, alignment: .leading)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(14)
        }
    }

    @ViewBuilder
    private var detailsTab: some View {
        if let entry {
            KeyValueRow(key: "Channel", value: entry.uploader)
            KeyValueRow(key: "Uploaded", value: Format.uploadDate(entry.uploadDate))
            if let d = loaded, d.viewCount > 0 {
                KeyValueRow(key: "Views", value: Format.count(d.viewCount))
            }
            if let d = loaded, d.likeCount > 0 {
                KeyValueRow(key: "Likes", value: Format.count(d.likeCount))
            }
            if let url = entry.originalURL {
                KeyValueRow(key: "Source", value: url)
            }
            KeyValueRow(
                key: "Archive layout",
                value: entry.layoutVersion == 0
                    ? "1 (predates versioning)"
                    : String(entry.layoutVersion)
            )
            if let mode = entry.downloadMode {
                KeyValueRow(key: "Download mode", value: mode)
            }
            if let d = loaded, !d.tags.isEmpty {
                KeyValueRow(key: "Tags", value: d.tags)
            }
            KeyValueRow(key: "Folder", value: entry.dir, monospaced: true)

            if let d = loaded, !d.description.isEmpty {
                Text("Description").font(.headline).padding(.top, 8)
                Text(d.description)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    @ViewBuilder
    private var mediaTab: some View {
        if let probe = loaded?.probe {
            if !probe.ok {
                Text(probe.error ?? "No media file.")
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                KeyValueRow(key: "Container", value: probe.format)
                if probe.bitRate > 0 {
                    KeyValueRow(
                        key: "Bitrate",
                        value: String(format: "%.2f Mb/s overall", Double(probe.bitRate) / 1_000_000)
                    )
                }
                ForEach(probe.streams) { stream in
                    KeyValueRow(key: "\(stream.kind) #\(stream.index)",
                                value: describe(stream))
                }
                if !probe.chapters.isEmpty {
                    Text("\(probe.chapters.count) chapters").font(.headline).padding(.top, 8)
                    ForEach(probe.chapters) { chapter in
                        KeyValueRow(key: Format.timecode(chapter.start), value: chapter.title)
                    }
                }
            }
        } else if loading {
            Text("Reading the container…").foregroundStyle(.secondary)
        }
    }

    private func describe(_ s: MediaStream) -> String {
        var parts: [String] = [s.codec ?? "unknown codec"]
        if let profile = s.profile { parts[0] += " (\(profile))" }
        if s.width > 0, s.height > 0 { parts.append("\(s.width)×\(s.height)") }
        if s.fps > 0.01 { parts.append(String(format: "%.3g fps", s.fps)) }
        if s.channels > 0 { parts.append("\(s.channels) ch") }
        if s.sampleRate > 0 { parts.append("\(s.sampleRate) Hz") }
        if s.bitRate > 0 { parts.append("\(s.bitRate / 1000) kb/s") }
        if let lang = s.language { parts.append(lang) }
        if let title = s.title { parts.append("“\(title)”") }
        if s.isDefault { parts.append("default") }
        /* Called out because this is exactly what made the webview build serve
         * every thumbnail with a video MIME type: ffprobe reports an attached
         * cover as a one-frame video stream. */
        if s.attachedPic { parts.append("attached cover image, not a video track") }
        return parts.joined(separator: " · ")
    }

    @ViewBuilder
    private var commentsTab: some View {
        if let d = loaded {
            if d.comments.isEmpty {
                Text(noCommentsExplanation)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                let shown = Array(d.comments.prefix(maxCommentsShown))
                Text("\(Comment.totalCount(d.comments)) comments including replies · "
                     + "\(d.comments.count) threads"
                     + (shown.count < d.comments.count ? " (showing the first \(maxCommentsShown))" : ""))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                ForEach(shown) { comment in
                    CommentRow(comment: comment, isReply: false)
                    ForEach(comment.replies) { reply in
                        CommentRow(comment: reply, isReply: true)
                    }
                }
            }
        }
    }

    /* Comment sections run to thousands of entries. Capped, with the total said
     * out loud -- a page that quietly showed 200 of 4,000 would be worse than
     * one that takes a moment. */
    private let maxCommentsShown = 200

    private var noCommentsExplanation: String {
        let mode = entry?.downloadMode
        if mode == "metadata-only" || mode == "subs-only" {
            return "This video was fetched in a mode that does not capture comments."
        }
        return "No comments were captured for this video. That is what --no-comments "
            + "produces, and also what a video with comments disabled produces."
    }

    @ViewBuilder
    private var transcriptTab: some View {
        if let d = loaded {
            if d.subtitlePath == nil {
                Text("No subtitle track in this folder. --no-subs produces that, and so does "
                     + "a video with no captions available.")
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } else if d.cues.isEmpty {
                Text("The subtitle file is present but produced no readable cues.")
                    .foregroundStyle(.secondary)
            } else {
                Text("\(d.subtitleName) · \(d.cues.count) lines"
                     + (d.subtitleIsAuto
                        ? " · auto-generated, rolling duplication collapsed"
                        : " · uploaded track"))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                Text(d.transcriptText)
                    .font(.system(.body))
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    @ViewBuilder
    private var filesTab: some View {
        if let entry {
            ForEach(entry.files) { file in
                KeyValueRow(key: file.rel, value: Format.bytes(file.size), monospaced: false)
            }
        }
    }

    // MARK: - Loading

    private func load() async {
        guard let entry else { return }
        loading = true
        loaded = nil
        verifyText = ""

        /* Built once per video, and never auto-played: opening a page should
         * not start making noise. */
        player?.pause()
        player = nil
        if let media = entry.mediaPath, Media.canPlayInWindow(path: media) {
            player = AVPlayer(url: URL(fileURLWithPath: media))
        }

        let dir = entry.dir
        let files = entry.files

        let result = await withCheckedContinuation { (c: CheckedContinuation<LoadedDetail, Never>) in
            /* Off the main thread: an info.json with a large comment tree takes
             * long enough to stall a click, and ffprobe is a subprocess. */
            DispatchQueue.global(qos: .userInitiated).async {
                var d = LoadedDetail()
                d.probe = Media.probe(path: entry.mediaPath)

                /* Chosen HERE and not before the hop: picking a subtitle track
                 * means reading the head of every subtitle file in the folder
                 * to tell an auto-generated track from a written one, and that
                 * is file I/O on the main thread if it happens in the caller. */
                let subtitle = DetailView.pickSubtitle(dir: dir, files: files)

                if let info = VideoInfo.load(forEntryDir: dir) {
                    d.description = info.string("description") ?? ""
                    d.viewCount = info.int("view_count")
                    d.likeCount = info.int("like_count")
                    d.comments = info.comments
                    d.tags = info.tags
                }

                if let subtitle {
                    d.subtitlePath = subtitle.path
                    d.subtitleName = subtitle.name
                    d.subtitleIsAuto = subtitle.isAuto
                    d.cues = Transcript.cues(atPath: subtitle.path)
                    d.transcriptText = d.cues
                        .map { "[\(Format.timecode($0.start))]  \($0.text)" }
                        .joined(separator: "\n")
                }
                c.resume(returning: d)
            }
        }

        /* The SwiftUI spelling of the GTK app's generation counter: .task(id:)
         * cancels this when the key changes, and a result that arrives after
         * that belongs to a page the user has already left. Rendering it would
         * replace what they are looking at now. */
        if Task.isCancelled { return }
        loaded = result
        loading = false
    }

    /* A human-written track wins over an auto-generated one, since the whole
     * reason the distinction is read from file CONTENTS is that it matters which
     * you are reading. */
    private static func pickSubtitle(
        dir: String, files: [ArchiveFile]
    ) -> (path: String, name: String, isAuto: Bool)? {
        /* A stand-in entry with just the two fields path(forIndex:) reads. The
         * path is still resolved and containment-checked by the index's own
         * code from the index's own directory -- this is not a caller supplying
         * a path. */
        let entry = ArchiveEntry(key: "", dir: dir, rel: "", channel: "", files: files)

        var best: (path: String, name: String, isAuto: Bool)?
        for (i, file) in files.enumerated() {
            guard MediaExtensions.isSubtitle(file.ext) else { continue }
            guard let path = entry.path(forIndex: i) else { continue }
            let isAuto = Transcript.isAutoGenerated(atPath: path)
            if best == nil || (best!.isAuto && !isAuto) {
                best = (path, file.rel, isAuto)
            }
        }
        return best
    }
}

/// Everything the worker read, handed to the view in one piece.
struct LoadedDetail {
    var probe = MediaProbe()
    var comments: [Comment] = []
    var cues: [Cue] = []
    var transcriptText = ""
    var description = ""
    var viewCount: Int64 = -1
    var likeCount: Int64 = -1
    var tags = ""
    var subtitlePath: String?
    var subtitleName = ""
    var subtitleIsAuto = false
}

private struct CommentRow: View {
    let comment: Comment
    let isReply: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 8) {
                Text(comment.author)
                    .font(.caption.weight(.semibold))
                if comment.authorIsUploader { Pill(text: "uploader", variant: .accent) }
                if comment.isPinned { Pill(text: "pinned", variant: .accent) }
                if comment.isFavorited { Pill(text: "hearted", variant: .accent) }
                if comment.likeCount > 0 {
                    Pill(text: "\(Format.count(comment.likeCount)) likes")
                }
                if let time = comment.timeText { Pill(text: time) }
            }
            Text(comment.text)
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.leading, isReply ? 24 : 0)
        /* A rule down the left rather than an indent alone, so a long thread
         * still reads as one conversation once the replies wrap. */
        .overlay(alignment: .leading) {
            if isReply {
                Rectangle()
                    .fill(.quaternary)
                    .frame(width: 2)
                    .padding(.leading, 10)
            }
        }
        .padding(.bottom, isReply ? 4 : 10)
    }
}
