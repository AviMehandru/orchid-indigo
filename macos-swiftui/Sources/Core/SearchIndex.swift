/* Searching every comment and every caption line in the archive at once.
 *
 * The pipeline archives every comment and every subtitle cue, three apps parse
 * both, and until now the only way to find a phrase in them was to open one
 * video at a time. The data was already on disk and already understood; what
 * was missing was an index.
 *
 * WHY NOT A REAL FTS ENGINE, which is the obvious answer and which
 * claude/orchid-indigo-gui-comparison.md guessed at. Two reasons, and the
 * second is the one that decided it:
 *
 *   1. The WinUI app would need Microsoft.Data.Sqlite. "No third-party package
 *      anywhere" is a structural claim this repository makes and keeps, and
 *      spending it on a feature that does not need it would be a poor trade.
 *   2. Three FTS engines means three TOKENIZERS that would have to agree about
 *      what a word is, forever, or the same query would return different
 *      videos on different platforms with nothing saying why. That is a fourth
 *      cross-language contract to hold in agreement, and the probe's shared
 *      fixture exists precisely because holding one is already expensive.
 *
 * So the index is a plain one, written here and identically in the other two
 * apps, and the tokenizer is a dozen lines that all three are checked against
 * the same fixture strings.
 *
 * WHAT IT STORES. Per video, per field, the SORTED UNIQUE TOKENS of that
 * field, space-joined and space-padded. It does not store the text: an index
 * that held every comment would be a second copy of the archive's bulkiest
 * content, and snippets are produced by re-reading the matched videos' own
 * files, which is cheap because a query matches a handful of videos rather
 * than all of them.
 *
 * MATCHING IS BY TOKEN PREFIX, AND ALL TOKENS MUST MATCH. "rail brid" finds a
 * comment containing "railway" and "bridge" in any order. Phrases are NOT
 * supported by the index -- a token set cannot answer "these words, adjacent,
 * in this order" -- and the UI must not imply they are.
 *
 * FRESHNESS. Every record carries the same stamp the verification cache uses:
 * the manifest's archive_creation_time, which postprocess.ps1 rewrites on
 * every pass over a folder including `ytdl --refresh`. A video whose stamp
 * still matches is not re-parsed, so a rebuild after a rescan costs only the
 * videos that actually changed -- which, after a refresh that fetched new
 * comments, is exactly the right set.
 *
 * THIS IS CACHE. It lives under ~/Library/Caches, nothing is written inside
 * the archive, and deleting it costs one rebuild.
 */

import Foundation

// MARK: - Scopes

/// Which text a search looks at.
///
/// `.metadata` is the substring match over title, uploader, id and channel
/// that the Library already had, and it is the default: it needs no index, it
/// answers instantly, and it is what almost every search is. The other three
/// need the index and say so in the UI when it has not been built.
enum SearchScope: String, CaseIterable, Identifiable {
    case metadata = "metadata"
    case comments = "comments"
    case transcript = "transcript"
    case everything = "everything"

    var id: String { rawValue }

    var label: String {
        switch self {
        case .metadata: return "Title and channel"
        case .comments: return "Comments"
        case .transcript: return "Transcript"
        case .everything: return "Everything"
        }
    }

    /// True for every scope that cannot be answered without the index.
    var needsIndex: Bool { self != .metadata }

    static func from(id: String?) -> SearchScope {
        guard let id, let s = SearchScope(rawValue: id) else { return .metadata }
        return s
    }
}

// MARK: - Snippets

struct SearchSnippet: Identifiable {
    let id = UUID()
    /// One comment, or one run of caption lines.
    let text: String
    /// The comment's author, or a timestamp for a passage of captions.
    let who: String
    let fromTranscript: Bool
}

// MARK: - The index

final class SearchIndex {
    private struct Record {
        var stamp: String
        /// " tok tok tok ", sorted and unique.
        var comments: String
        var transcript: String
    }

    private static let version = 1

    /* One caption cue is a few words, so a two-word query almost never falls
     * inside one. Consecutive cues are glued into passages of about this many
     * characters before matching, which is roughly a sentence or two -- long
     * enough for a phrase to land in one, short enough to read as a snippet. */
    private static let transcriptPassage = 240

    /// The minimum token length the index keeps. One-character tokens match
    /// almost everything and cost the most to store.
    static let minToken = 2

    private let path: String
    private var records: [String: Record] = [:]
    private var dirty = false

    init(path: String? = nil) {
        self.path = path ?? Paths.join(Paths.cacheDir(), "search-index.json")
        load()
    }

    // MARK: Tokenizing

    /// Case-folded runs of letters and digits, everything else a separator.
    ///
    /// Pinned by the test suite against the same strings the C and C# suites
    /// use, because this is the one function whose disagreement between the
    /// three apps would be invisible: the same query would quietly return
    /// different videos on different platforms. Apostrophes are separators, so
    /// "don't" is two tokens in every app -- which is a choice, not an
    /// oversight, and it is the choice all three make.
    static func tokenize(_ text: String?) -> [String] {
        guard let text, !text.isEmpty else { return [] }

        var out: [String] = []
        var current = ""
        for ch in text {
            if ch.isLetter || ch.isNumber {
                current.append(contentsOf: String(ch).lowercased())
            } else if !current.isEmpty {
                out.append(current)
                current = ""
            }
        }
        if !current.isEmpty { out.append(current) }
        return out
    }

    /* " tok1 tok2 tok3 " -- sorted, unique, and padded at both ends.
     *
     * The padding is what makes a query match on a token BOUNDARY with a plain
     * substring search: looking for " rail" finds the token "railway" and does
     * not find "guardrail". Storing it padded rather than padding at query
     * time means the concatenation happens once per video per build instead of
     * once per video per keystroke. */
    private static func blob(_ tokens: [String]) -> String {
        let kept = Set(tokens.filter { $0.count >= minToken }).sorted()
        /* A single space means "parsed, found nothing" -- a real answer, and
         * one that must be told apart from "not parsed yet". */
        return kept.isEmpty ? " " : " " + kept.joined(separator: " ") + " "
    }

    // MARK: Load and save

    private func load() {
        guard let obj = JSONFile.object(at: path) else { return }

        /* A store written by a different version of this format is discarded
         * rather than misread. That is the whole reason the version is written
         * from the first release: without it, a later change to what a token
         * is would leave every existing user with an index that silently
         * answers the old way. */
        guard obj.int("version", default: -1) == Int64(SearchIndex.version) else { return }
        guard let videos = obj["videos"] as? [String: Any] else { return }

        for (key, value) in videos {
            guard let rec = value as? [String: Any] else { continue }
            records[key] = Record(stamp: rec.str("stamp") ?? "",
                                  comments: rec.str("c") ?? "",
                                  transcript: rec.str("t") ?? "")
        }
    }

    /// Best-effort write to the cache directory.
    func save() {
        guard dirty else { return }

        var videos: [String: Any] = [:]
        for (key, rec) in records {
            videos[key] = ["stamp": rec.stamp, "c": rec.comments, "t": rec.transcript]
        }
        let root: [String: Any] = ["version": SearchIndex.version, "videos": videos]

        /* NOT pretty-printed, unlike every other file this app writes. This one
         * is machine-read only and is the largest thing in the cache
         * directory; indenting it would add a byte per token for nobody's
         * benefit. */
        guard let data = try? JSONSerialization.data(withJSONObject: root, options: []) else {
            return
        }

        let dir = (path as NSString).deletingLastPathComponent
        try? FileManager.default.createDirectory(atPath: dir,
                                                 withIntermediateDirectories: true)
        if (try? data.write(to: URL(fileURLWithPath: path), options: .atomic)) != nil {
            dirty = false
        }
    }

    // MARK: Freshness

    /* What identifies "this folder, as it is now".
     *
     * The manifest's archive_creation_time when there is one, which
     * postprocess.ps1 rewrites on every pass including `ytdl --refresh` -- so
     * a video whose comments were re-fetched re-indexes and one that was
     * merely rescanned does not. Falling back to the info.json's size and
     * mtime covers a folder with no manifest, which the layout contract names
     * as an ordinary state. */
    private func stamp(for entry: ArchiveEntry) -> String {
        if let s = entry.creationStamp, !s.isEmpty { return s }

        let metaDir = Paths.join(entry.dir, "Video metadata")
        if let info = ArchiveIndex.infoJSONPath(inMetaDir: metaDir),
           let attrs = try? FileManager.default.attributesOfItem(atPath: info) {
            let size = (attrs[.size] as? NSNumber)?.uint64Value ?? 0
            let mtime = (attrs[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
            return "\(Int64(mtime)):\(size)"
        }

        /* Nothing to key on. Such a folder is re-parsed on every build, which
         * costs nothing because there is nothing in it to parse. */
        return ""
    }

    private func isCurrent(_ entry: ArchiveEntry) -> Bool {
        guard let rec = records[entry.key] else { return false }
        return rec.stamp == stamp(for: entry)
    }

    /// How many videos the index currently covers.
    var count: Int { records.count }

    /// How many of the archive's videos are missing from the index or stale.
    /// Drives the "index N videos" prompt, which has to be honest about what a
    /// search can currently see.
    func outdated(in entries: [ArchiveEntry]) -> Int {
        entries.reduce(0) { $0 + (isCurrent($1) ? 0 : 1) }
    }

    // MARK: Building

    /* The human-written track if there is one, exactly as the detail page
     * picks it: the auto/human distinction is read from file CONTENTS because
     * it matters which you are reading, and an index built over the auto track
     * when a real one exists would answer differently from the page. */
    private func bestSubtitlePath(_ entry: ArchiveEntry) -> String? {
        var best: String?
        var bestIsAuto = true

        for (i, f) in entry.files.enumerated() {
            guard MediaExtensions.isSubtitle(f.ext) else { continue }
            guard let path = entry.path(forIndex: i) else { continue }
            let isAuto = Transcript.isAutoGenerated(atPath: path)
            if best == nil || (bestIsAuto && !isAuto) {
                best = path
                bestIsAuto = isAuto
            }
        }
        return best
    }

    private func flatten(_ comments: [Comment], into out: inout [String]) {
        for c in comments {
            out.append(c.text)
            out.append(c.author)
            if !c.replies.isEmpty { flatten(c.replies, into: &out) }
        }
    }

    private func indexOne(_ entry: ArchiveEntry) {
        var commentTokens: [String] = []
        if let info = VideoInfo.load(forEntryDir: entry.dir) {
            var texts: [String] = []
            flatten(info.comments, into: &texts)

            /* The description is indexed with the comments rather than given a
             * scope of its own. It is the uploader's own words about the
             * video, which is what someone searching "comments" is reaching
             * for when they half-remember something said about it -- and a
             * fifth scope for one field would be a menu nobody reads. */
            if let description = info.string("description") { texts.append(description) }

            for t in texts { commentTokens.append(contentsOf: SearchIndex.tokenize(t)) }
        }

        var transcriptTokens: [String] = []
        if let sub = bestSubtitlePath(entry) {
            for cue in Transcript.cues(atPath: sub) {
                transcriptTokens.append(contentsOf: SearchIndex.tokenize(cue.text))
            }
        }

        records[entry.key] = Record(stamp: stamp(for: entry),
                                    comments: SearchIndex.blob(commentTokens),
                                    transcript: SearchIndex.blob(transcriptTokens))
        dirty = true
    }

    /// Parse every video whose stamp is missing or stale and record its tokens.
    ///
    /// Runs on whichever thread calls it and is the expensive operation here:
    /// it parses every changed info.json, which is the same cost the detail
    /// page pays per video, paid once for all of them.
    ///
    /// Videos that have gone from the archive are dropped, so the store does
    /// not grow forever across rescans.
    func build(over entries: [ArchiveEntry],
               progress: ((Int, Int) -> Void)? = nil,
               isCancelled: (() -> Bool)? = nil) {
        for (i, entry) in entries.enumerated() {
            if isCancelled?() == true { return }
            if !isCurrent(entry) { indexOne(entry) }
            progress?(i + 1, entries.count)
        }

        let live = Set(entries.map(\.key))
        let stale = records.keys.filter { !live.contains($0) }
        if !stale.isEmpty {
            for key in stale { records.removeValue(forKey: key) }
            dirty = true
        }
    }

    // MARK: Querying

    /* Every query token must appear as the PREFIX of some token in the blob.
     *
     * The leading space on the needle is what pins it to a token boundary:
     * " rail" matches the stored " railway " and does not match " guardrail ".
     * Prefix rather than whole-token because this runs as the user types, and
     * a search that returns nothing until the last letter of a word is one
     * people stop using before they finish typing. */
    private static func needles(for query: String) -> [String] {
        /* A one-character token is kept HERE even though the index does not
         * store one-character tokens: as a prefix it is a perfectly good
         * filter (" a" matches " apple "), and dropping it would make a query
         * narrower as the user typed its first letter and then wider again. */
        tokenize(query).map { " " + $0 }
    }

    private static func matches(_ blob: String, _ needles: [String]) -> Bool {
        guard !blob.isEmpty else { return false }
        for n in needles where blob.range(of: n) == nil { return false }
        return true
    }

    /// The keys of the videos whose `scope` text matches every token of
    /// `query`. An empty query yields an empty set, because "match everything"
    /// is the caller's business to decide and not this function's to guess.
    func query(_ query: String, scope: SearchScope) -> Set<String> {
        let needles = SearchIndex.needles(for: query)
        guard !needles.isEmpty else { return [] }

        var hits: Set<String> = []
        for (key, rec) in records {
            var hit = false
            if scope == .comments || scope == .everything {
                hit = SearchIndex.matches(rec.comments, needles)
            }
            if !hit, scope == .transcript || scope == .everything {
                hit = SearchIndex.matches(rec.transcript, needles)
            }
            if hit { hits.insert(key) }
        }
        return hits
    }

    // MARK: Snippets

    /* The same rule `matches` applies, against text rather than a stored blob
     * -- so a passage shown as a hit is one that would have matched had it
     * been indexed alone. */
    private static func textMatches(_ text: String, _ needles: [String]) -> Bool {
        let tokens = tokenize(text)
        guard !tokens.isEmpty else { return false }
        return matches(" " + tokens.joined(separator: " ") + " ", needles)
    }

    private static func clock(_ seconds: Double) -> String {
        guard seconds >= 0 else { return "" }
        let total = Int(seconds)
        let h = total / 3600, m = (total % 3600) / 60, s = total % 60
        return h > 0
            ? String(format: "%d:%02d:%02d", h, m, s)
            : String(format: "%d:%02d", m, s)
    }

    /// Read `entry`'s own comments and captions and return the passages that
    /// match.
    ///
    /// Deliberately NOT served from the index, which holds no text. This
    /// re-reads the video's files, which is affordable precisely because it is
    /// called for the handful of videos a query matched rather than for all of
    /// them. Runs on whichever thread calls it and should not be the main one.
    static func snippets(for entry: ArchiveEntry, query: String,
                         scope: SearchScope, max: Int = 5) -> [SearchSnippet] {
        let needles = SearchIndex.needles(for: query)
        guard !needles.isEmpty, max > 0 else { return [] }

        var out: [SearchSnippet] = []

        if scope == .comments || scope == .everything,
           let info = VideoInfo.load(forEntryDir: entry.dir) {
            func walk(_ comments: [Comment]) {
                for c in comments where out.count < max {
                    if textMatches(c.text, needles) {
                        out.append(SearchSnippet(text: c.text, who: c.author,
                                                 fromTranscript: false))
                    }
                    if !c.replies.isEmpty { walk(c.replies) }
                }
            }
            walk(info.comments)
        }

        if out.count < max, scope == .transcript || scope == .everything {
            var best: String?
            var bestIsAuto = true
            for (i, f) in entry.files.enumerated() {
                guard MediaExtensions.isSubtitle(f.ext),
                      let path = entry.path(forIndex: i) else { continue }
                let isAuto = Transcript.isAutoGenerated(atPath: path)
                if best == nil || (bestIsAuto && !isAuto) {
                    best = path
                    bestIsAuto = isAuto
                }
            }

            if let sub = best {
                /* Cues are glued into passages before matching: one cue is a
                 * few words, so a two-word query almost never falls inside
                 * one, and matching per cue would report "no transcript hits"
                 * for a video whose transcript plainly contains the phrase. */
                var passage = ""
                var start = -1.0
                let cues = Transcript.cues(atPath: sub)
                for (i, cue) in cues.enumerated() where out.count < max {
                    if cue.text.isEmpty { continue }
                    if start < 0 { start = cue.start }
                    if !passage.isEmpty { passage += " " }
                    passage += cue.text

                    let last = i + 1 == cues.count
                    if passage.count < transcriptPassage && !last { continue }

                    if textMatches(passage, needles) {
                        out.append(SearchSnippet(text: passage, who: clock(start),
                                                 fromTranscript: true))
                    }
                    passage = ""
                    start = -1
                }
            }
        }

        return out
    }
}
