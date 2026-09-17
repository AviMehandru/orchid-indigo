/* What was learned the last time a video's checksums were verified.
 *
 * Verifying a folder means hashing every file in it -- seconds per video on a
 * large one -- so the answer is worth keeping. It is also worth distrusting:
 * the archive is not immutable. `ytdl --refresh` rewrites a folder's
 * sidecars, its hashes and, when it re-embeds the info.json, the media file
 * itself, all without changing `download_mode`. A cached "verifies" from
 * before that is not a stale opinion, it is a wrong one.
 *
 * So every record is stamped with the manifest's own `archive_creation_time`,
 * which postprocess.ps1 rewrites on every pass over a folder including a
 * refresh. A record whose stamp no longer matches is treated as absent rather
 * than as a result. That is the "key the cache on something that moves" rule
 * docs/archive-layout.md now states, implemented.
 *
 * THIS IS CACHE, NOT USER DATA. It lives under ~/Library/Caches and deleting
 * it costs one re-verify. Nothing here is written inside the archive --
 * checksums.sha256 covers every file in a video folder, so a dropped file
 * there makes that folder stop verifying, which would be a memorable way for
 * a verification cache to work.
 */

import Foundation

final class VerifyCache {
    private struct Record {
        var state: VerifyState
        /// The manifest's archive_creation_time, or "" when it had none.
        var stamp: String
    }

    private static let version = 1

    private let path: String
    private var records: [String: Record] = [:]
    private var dirty = false

    /// Loads from the cache directory. Never fails: an unreadable or corrupt
    /// store yields an empty cache, because the cost of being wrong here is
    /// one re-verify and the cost of refusing to start is the window.
    init(path: String? = nil) {
        self.path = path ?? Paths.join(Paths.cacheDir(), "verify.json")
        load()
    }

    private static func stateID(_ s: VerifyState) -> String {
        switch s {
        case .ok: return "ok"
        case .failed: return "failed"
        case .unknown: return "unknown"
        }
    }

    private static func state(fromID id: String?) -> VerifyState {
        switch id {
        case "ok": return .ok
        case "failed": return .failed
        default: return .unknown
        }
    }

    private func load() {
        guard let obj = JSONFile.object(at: path) else { return }
        /* The version is read and ignored for now, deliberately: writing it
         * from the first release is what makes a future format change able to
         * discard old records instead of misreading them. */
        guard let videos = obj["videos"] as? [String: Any] else { return }

        for (key, value) in videos {
            guard let rec = value as? [String: Any] else { continue }
            records[key] = Record(state: VerifyCache.state(fromID: rec.str("state")),
                                  stamp: rec.str("stamp") ?? "")
        }
    }

    /// The recorded state for a video, or `.unknown` when there is none --
    /// including when there is one but it was recorded against a different
    /// version of the folder.
    func state(for entry: ArchiveEntry) -> VerifyState {
        guard let rec = records[entry.key] else { return .unknown }

        /* The whole point of the file. A record whose stamp no longer matches
         * the manifest was taken before something rewrote this folder -- a
         * refresh, a manual repair, a re-download -- and a verification result
         * from before the files changed is not a weaker answer than none, it
         * is a wrong one. Treated as absent rather than deleted here, because
         * a read should not mutate; the next write over this key replaces it. */
        guard rec.stamp == (entry.creationStamp ?? "") else { return .unknown }
        return rec.state
    }

    /// Record a result. A folder with no manifest is stamped with the empty
    /// string and will only ever satisfy another unstamped read -- it cannot
    /// be cached against a stamp it does not have, and pretending otherwise is
    /// how a stale pass survives a refresh.
    func set(_ state: VerifyState, for entry: ArchiveEntry) {
        records[entry.key] = Record(state: state, stamp: entry.creationStamp ?? "")
        dirty = true
    }

    /// How many videos have a record, for the facet's own label -- the
    /// "failed verification" facet has to be able to say what it is a subset
    /// of, or it reads as a claim about the whole library.
    var knownCount: Int {
        records.values.filter { $0.state != .unknown }.count
    }

    /// Write the store back. Best-effort: a failure is swallowed, because
    /// losing a cache is not worth interrupting anyone over.
    func save() {
        guard dirty else { return }

        var videos: [String: Any] = [:]
        for (key, rec) in records {
            /* An unknown record carries no information and would grow the file
             * for every video anyone ever opened. */
            guard rec.state != .unknown else { continue }
            videos[key] = ["state": VerifyCache.stateID(rec.state), "stamp": rec.stamp]
        }

        let root: [String: Any] = ["version": VerifyCache.version, "videos": videos]
        guard let data = try? JSONSerialization.data(withJSONObject: root,
                                                     options: [.prettyPrinted]) else {
            return
        }

        let dir = (path as NSString).deletingLastPathComponent
        try? FileManager.default.createDirectory(atPath: dir,
                                                 withIntermediateDirectories: true)
        if (try? data.write(to: URL(fileURLWithPath: path), options: .atomic)) != nil {
            dirty = false
        }
    }
}
