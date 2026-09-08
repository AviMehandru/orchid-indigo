/* info.json, comments and the transcript.
 *
 * Read ON DEMAND, when a video's page is opened -- never during the scan.
 * info.json carries the full comment tree and routinely runs to several
 * megabytes; parsing every one of them to draw a grid of thumbnails would make
 * opening the app cost what opening every video costs.
 *
 * Both parsers here fail QUIETLY when they are wrong -- a mis-threaded comment
 * section still renders, and a transcript with the rolling duplication left in
 * still looks like a transcript. Neither throws. That is why both are pinned
 * by tests rather than trusted.
 */

import Foundation

// MARK: - info.json

struct VideoInfo {
    let root: [String: Any]

    /// nil when there is no readable info.json -- an ordinary state the layout
    /// contract names, not an error.
    static func load(forEntryDir dir: String) -> VideoInfo? {
        let metaDir = Paths.join(dir, "Video metadata")
        guard let path = ArchiveIndex.infoJSONPath(inMetaDir: metaDir),
              let obj = JSONFile.object(at: path) else { return nil }
        return VideoInfo(root: obj)
    }

    func string(_ key: String) -> String? { root.str(key) }
    func int(_ key: String) -> Int64 { root.int(key) }

    /// The threaded comments. Empty when the video was fetched with
    /// --no-comments, which is ordinary.
    var comments: [Comment] {
        Comment.thread(root.objects("comments"))
    }

    /// Up to 40 tags, joined for display.
    var tags: String {
        root.strings("tags").prefix(40).joined(separator: ", ")
    }
}

// MARK: - Comments

final class Comment: Identifiable {
    /* yt-dlp's own comment id, used to thread replies onto parents. NOT this
     * class's Identifiable id: it is "" whenever the extractor did not send
     * one, and a ForEach over rows with duplicate ids silently drops all but
     * one of them. */
    let commentID: String
    let text: String
    let author: String
    let authorID: String?
    let timeText: String?
    /// -1 when absent.
    let timestamp: Int64
    /// -1 when absent.
    let likeCount: Int64
    let isFavorited: Bool
    let authorIsUploader: Bool
    let isPinned: Bool
    var replies: [Comment] = []

    /// Identity is the object, which is unique by construction.
    var id: ObjectIdentifier { ObjectIdentifier(self) }

    init(from o: [String: Any]) {
        commentID = o.str("id") ?? ""
        text = o.str("text") ?? ""
        author = o.str("author") ?? "(unknown)"
        authorID = o.str("author_id")
        timeText = o.str("_time_text")
        timestamp = o.int("timestamp", default: -1)
        likeCount = o.int("like_count", default: -1)
        isFavorited = o.bool("is_favorited")
        authorIsUploader = o.bool("author_is_uploader")
        isPinned = o.bool("is_pinned")
    }

    /* Thread yt-dlp's flat comment list.
     *
     * yt-dlp writes comments as a FLAT array with a `parent` field that is
     * either "root" or the parent's id, IN NO GUARANTEED ORDER -- a reply can
     * appear before its parent, so this is a two-pass job rather than a fold.
     *
     * Top level is pinned first, then most-liked: the same order YouTube itself
     * shows, which matters because a transcript of a comment section in
     * arbitrary order is a different document from the one people read. */
    static func thread(_ raw: [[String: Any]]) -> [Comment] {
        var tops: [Comment] = []
        var index: [String: Int] = [:]      // id -> position in tops
        var orphans: [(parent: String, comment: Comment)] = []

        for o in raw {
            let parent = o.str("parent")
            let c = Comment(from: o)

            if parent == nil || parent == "root" {
                index[c.commentID] = tops.count
                tops.append(c)
            } else {
                orphans.append((parent!, c))
            }
        }

        for (parent, c) in orphans {
            var slot = index[parent]
            if slot == nil, let dot = parent.firstIndex(of: ".") {
                /* yt-dlp's reply ids are "<parent>.<reply>", so the parent id is
                 * recoverable even when the parent field itself is unhelpful. */
                slot = index[String(parent[parent.startIndex..<dot])]
            }

            if let slot {
                tops[slot].replies.append(c)
            } else {
                /* A reply whose parent is genuinely absent -- a deleted comment,
                 * or a truncated fetch -- is shown at top level rather than
                 * dropped. Silently losing archived text would be the worse
                 * failure. */
                tops.append(c)
            }
        }

        for c in tops {
            c.replies.sort { a, b in
                (a.timestamp < 0 ? 0 : a.timestamp) < (b.timestamp < 0 ? 0 : b.timestamp)
            }
        }

        /* Pinned first, then most-liked, with an explicit tie-break on the
         * original position. Swift's sort is NOT stable, and without that
         * tie-break the order of equally-liked comments -- which is most of a
         * long thread -- would differ between two reads of the same file. */
        return tops.enumerated()
            .sorted { a, b in
                if a.element.isPinned != b.element.isPinned { return a.element.isPinned }
                let al = max(a.element.likeCount, 0)
                let bl = max(b.element.likeCount, 0)
                if al != bl { return al > bl }
                return a.offset < b.offset
            }
            .map { $0.element }
    }

    /// This comment plus every reply under it.
    static func totalCount(_ tops: [Comment]) -> Int {
        tops.reduce(0) { $0 + 1 + $1.replies.count }
    }
}

// MARK: - Transcript

struct Cue: Identifiable {
    let start: Double
    var end: Double
    let text: String

    var id: String { "\(start)-\(text.prefix(24))" }
}

enum Transcript {
    /* A .vtt or .srt turned into a readable transcript.
     *
     * YouTube's auto-generated VTT is a ROLLING TWO-LINE DISPLAY: nearly every
     * cue repeats the previous cue's last line, and words carry inline karaoke
     * timestamps. Read as-is it is unusable as prose, so tags are stripped and
     * the repetition is collapsed. */
    static func cues(atPath path: String) -> [Cue] {
        guard let data = FileManager.default.contents(atPath: path),
              let raw = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1)
        else { return [] }

        /* Normalise line endings first: a .vtt written on Windows would
         * otherwise never match the blank-line block separator. */
        let normalised = raw
            .replacingOccurrences(of: "\r\n", with: "\n")
            .replacingOccurrences(of: "\r", with: "\n")

        var parsed: [Cue] = []
        for block in normalised.components(separatedBy: "\n\n") {
            let lines = block
                .components(separatedBy: "\n")
                .filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty }
            if lines.isEmpty { continue }

            guard let timeIdx = lines.firstIndex(where: { $0.contains("-->") }) else { continue }
            let timeline = lines[timeIdx]
            guard let arrow = timeline.range(of: "-->") else { continue }

            guard let start = seconds(String(timeline[timeline.startIndex..<arrow.lowerBound])) else {
                continue
            }
            let end = seconds(String(timeline[arrow.upperBound...])) ?? (start + 3.0)

            let body = lines[(timeIdx + 1)...].joined(separator: " ")
            let text = collapseWhitespace(unescapeEntities(stripInlineTags(body)))
            if text.isEmpty { continue }

            parsed.append(Cue(start: start, end: end, text: text))
        }

        return collapseRolling(parsed)
    }

    /* The rolling-display collapse.
     *
     * The comparison is against the previous cue's FULL text, not against what
     * was last emitted. That distinction is the whole algorithm:
     *
     *   cue 1  "the quick brown fox"
     *   cue 2  "the quick brown fox jumps over"
     *   cue 3  "the quick brown fox jumps over the lazy dog"
     *
     * Emitting the tail of cue 2 puts "jumps over" in the output. Comparing cue
     * 3 against THAT finds no common prefix, so cue 3 is emitted whole and the
     * duplication the collapse exists to remove comes straight back on the
     * third line. Keeping lastFull separate from the output is what makes the
     * third line "the lazy dog".
     *
     * The same defect was in orchid-cobalt's src-tauri/src/archive.rs. It works
     * for two lines and then silently stops, which is why the test in this
     * repo's suite is three lines deep and not two. */
    private static func collapseRolling(_ cues: [Cue]) -> [Cue] {
        var out: [Cue] = []
        var lastFull: String?

        for cue in cues {
            if let previous = lastFull, !out.isEmpty {
                if cue.text == previous {
                    out[out.count - 1].end = max(out[out.count - 1].end, cue.end)
                    continue
                }
                /* The 12-character floor keeps a genuinely repeated short line
                 * ("Yeah." then "Yeah. Right.") from being chopped into
                 * fragments. */
                if cue.text.hasPrefix(previous), previous.count > 12 {
                    let tail = String(cue.text.dropFirst(previous.count))
                        .trimmingCharacters(in: .whitespaces)
                    if tail.isEmpty {
                        out[out.count - 1].end = max(out[out.count - 1].end, cue.end)
                    } else {
                        out.append(Cue(start: cue.start, end: cue.end, text: tail))
                    }
                    lastFull = cue.text
                    continue
                }
            }

            out.append(cue)
            lastFull = cue.text
        }
        return out
    }

    /* [hh:]mm:ss[.,]mmm -- the leading run of digits, colons and a decimal
     * mark. Anything after it (VTT cue settings such as "align:start
     * position:0%") is ignored. */
    static func seconds(_ text: String) -> Double? {
        let trimmed = text.trimmingCharacters(in: .whitespaces)
        var head = ""
        for ch in trimmed {
            if ch.isNumber || ch == ":" || ch == "." || ch == "," {
                head.append(ch)
            } else {
                break
            }
        }
        if head.isEmpty { return nil }

        var whole = head
        var fraction = ""
        if let mark = head.firstIndex(where: { $0 == "." || $0 == "," }) {
            whole = String(head[head.startIndex..<mark])
            fraction = String(head[head.index(after: mark)...])
        }

        let parts = whole.split(separator: ":", omittingEmptySubsequences: false).map(String.init)
        var h = 0.0, m = 0.0, s = 0.0
        if parts.count == 3 {
            h = Double(parts[0]) ?? 0
            m = Double(parts[1]) ?? 0
            s = Double(parts[2]) ?? 0
        } else if parts.count == 2 {
            m = Double(parts[0]) ?? 0
            s = Double(parts[1]) ?? 0
        } else {
            return nil
        }

        /* ".5" is 500ms, not 5ms -- pad on the RIGHT to three digits. */
        var ms = 0.0
        if !fraction.isEmpty {
            let padded = String((fraction + "000").prefix(3))
            ms = Double(padded) ?? 0
        }

        return h * 3600 + m * 60 + s + ms / 1000
    }

    /* Removes <c>, </c>, <v Name>, and the per-word <00:00:01.234> karaoke
     * timestamps YouTube's ASR emits. A hand-rolled scanner rather than a
     * regular expression: the grammar is "everything between < and >", and
     * NSRegularExpression on a multi-megabyte transcript is not free. */
    static func stripInlineTags(_ s: String) -> String {
        var out = ""
        out.reserveCapacity(s.count)
        var depth = 0
        for ch in s {
            if ch == "<" {
                depth += 1
            } else if ch == ">" {
                if depth > 0 { depth -= 1 }
            } else if depth == 0 {
                out.append(ch)
            }
        }
        return out
    }

    static func unescapeEntities(_ s: String) -> String {
        var out = s
        let pairs = [
            ("&amp;", "&"), ("&lt;", "<"), ("&gt;", ">"),
            ("&quot;", "\""), ("&#39;", "'"), ("&nbsp;", " "),
        ]
        for (from, to) in pairs {
            out = out.replacingOccurrences(of: from, with: to)
        }
        return out
    }

    static func collapseWhitespace(_ s: String) -> String {
        s.split(whereSeparator: { $0 == " " || $0 == "\t" || $0 == "\n" || $0 == "\r" })
            .joined(separator: " ")
    }

    /* The filenames cannot tell an auto-generated track from a human-written
     * one -- --write-subs and --write-auto-subs both land in Subtitles/ under
     * the same base name. The CONTENTS can: ASR output carries per-word karaoke
     * tags and cue-positioning directives that uploaded tracks do not.
     *
     * Only the head is read, and these files can be large. */
    static func isAutoGenerated(atPath path: String) -> Bool {
        guard let handle = FileHandle(forReadingAtPath: path) else { return false }
        defer { try? handle.close() }
        let data = handle.readData(ofLength: 8000)
        guard let head = String(data: data, encoding: .utf8)
            ?? String(data: data, encoding: .isoLatin1) else { return false }

        return head.contains("<c.") || head.contains("<c>")
            || head.contains("align:start position:")
    }
}
