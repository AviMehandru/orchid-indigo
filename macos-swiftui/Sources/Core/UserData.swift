/* What the person did, as opposed to what the pipeline wrote.
 *
 * Watch state, resume positions and hand-made playlists. Three things the
 * archive itself has no opinion about and never will: postprocess.ps1 records
 * what was downloaded, and nothing on disk knows whether anybody has seen it.
 *
 * THIS IS USER DATA, NOT CACHE, and that is why it lives in the state
 * directory beside settings.json, the queue and the history rather than under
 * ~/Library/Caches beside the archive index and the verification results.
 * Deleting the cache costs a rescan -- and macOS empties Caches on its own
 * when the disk fills. Deleting this loses the fact that you watched
 * something, which nothing can reconstruct.
 *
 * KEYED BY THE ARCHIVE KEY, for the same reason every other handle in this app
 * is: it survives a rescan, and it is the one identifier that does not change
 * when a manifest is rewritten.
 *
 * AND DELIBERATELY NOT STAMPED. VerifyCache carries the manifest's
 * archive_creation_time and discards a record whose stamp has moved, because
 * "these bytes verify" stops being true when the bytes change. Watch state is
 * the opposite kind of fact: "I have seen this video" is about the person, not
 * about the folder, and `ytdl --refresh` fetching newer comments does not
 * un-watch anything. A resume position survives for the same reason -- a
 * refresh can rewrite the media container's attachments but not its timeline.
 * The one thing that would invalidate a position is a genuinely different
 * video at the same path, and that is a different video with a different key.
 *
 * Nothing here is written inside the archive: checksums.sha256 covers every
 * file in a video folder, so a "watched" marker dropped in there would make
 * that folder stop verifying.
 *
 * The rules, the thresholds and the fixture values are shared with the GTK and
 * WinUI suites. The three apps are independent implementations of one
 * contract, exactly as the probe's derivation is.
 */

import Foundation

/// One playlist: a set the user ordered, not a bag.
struct Playlist: Identifiable, Equatable {
    /// Stable across renames; never shown.
    let id: String
    var name: String
    /// Archive keys, in the order the user put them.
    var keys: [String]
}

final class UserData {
    /// How much of a video counts as having watched it.
    ///
    /// 0.9 rather than 1.0 because nobody sits through the end card, and a
    /// video you stopped 40 seconds from the end is one you have seen. Every
    /// player that tracks this picks something in this region.
    static let watchedFraction = 0.9

    /// A resume point below this is discarded rather than stored. Offering to
    /// resume eight seconds in is worse than not offering: it costs a decision
    /// and saves nothing.
    static let resumeMinSeconds = 15.0

    private struct WatchState {
        var watched = false
        /// Seconds; 0 means no resume point.
        var position = 0.0
        /// ISO 8601, for a human reading the file.
        var updated = ""
    }

    private static let version = 1

    private let path: String
    private var watch: [String: WatchState] = [:]
    private var playlistsByOrder: [Playlist] = []
    private var dirty = false

    /// Set when a file existed and could not be parsed. Blocks saving, so a
    /// corrupt store is left for its owner rather than overwritten.
    private(set) var isReadOnly = false

    /// Loads from the state directory. Never fails.
    ///
    /// A file that is not there is the ordinary first-launch state and is NOT
    /// "unreadable": there is nothing to protect, and the first save should
    /// create it. Only a file that exists and will not parse gets the flag.
    init(path: String? = nil) {
        self.path = path ?? Paths.join(Paths.stateDir(), "userdata.json")
        load()
    }

    private func load() {
        guard Paths.isRegularFile(self.path) else { return }

        guard let obj = JSONFile.object(at: self.path) else {
            isReadOnly = true
            return
        }

        /* The version is read and ignored for now, deliberately: writing it
         * from the first release is what makes a future format change able to
         * discard old records instead of misreading them. */
        if let videos = obj.object("watch") {
            for (key, value) in videos {
                guard let rec = value as? [String: Any] else { continue }
                var w = WatchState()
                w.watched = rec.bool("watched")
                w.position = max(0, rec.double("position"))
                w.updated = rec.str("updated") ?? ""
                watch[key] = w
            }
        }

        for rec in obj.objects("playlists") {
            guard let id = rec.str("id"), let name = rec.str("name") else { continue }
            playlistsByOrder.append(Playlist(id: id, name: name,
                                             keys: rec.strings("keys")))
        }
    }

    /// Write the store back if anything changed.
    ///
    /// Refuses to write when the load failed to PARSE an existing file, so a
    /// corrupt userdata.json is left on disk for its owner to look at instead
    /// of being replaced by an empty one the first time anything is touched.
    func save() {
        guard dirty, !isReadOnly else { return }

        var videos: [String: Any] = [:]
        for (key, w) in watch {
            /* A record that says nothing would grow the file for every video
             * anyone ever opened. */
            guard w.watched || w.position > 0 else { continue }
            videos[key] = ["watched": w.watched,
                           "position": w.position,
                           "updated": w.updated]
        }

        let lists: [Any] = playlistsByOrder.map {
            ["id": $0.id, "name": $0.name, "keys": $0.keys]
        }

        let root: [String: Any] = ["version": UserData.version,
                                   "watch": videos,
                                   "playlists": lists]
        guard let data = JSONFile.data(from: root) else { return }

        let dir = (path as NSString).deletingLastPathComponent
        try? FileManager.default.createDirectory(atPath: dir,
                                                 withIntermediateDirectories: true)
        if (try? data.write(to: URL(fileURLWithPath: path), options: .atomic)) != nil {
            dirty = false
        }
    }

    // MARK: - Watch state

    func isWatched(_ key: String) -> Bool {
        watch[key]?.watched ?? false
    }

    func setWatched(_ key: String, _ watched: Bool) {
        var w = watch[key] ?? WatchState()
        guard w.watched != watched else { return }

        w.watched = watched
        /* Marking something watched by hand clears a resume point: the two
         * would otherwise disagree, and the flag is the more deliberate
         * statement of the pair. Marking it UNWATCHED leaves the position
         * alone -- "I want to see this again" and "start it over" are
         * different wishes. */
        if watched { w.position = 0 }
        w.updated = UserData.nowISO()
        watch[key] = w
        dirty = true
    }

    /// Seconds to resume from, or 0 for "start at the beginning".
    ///
    /// 0 rather than -1 for absent because every caller wants to seek to it,
    /// and a sentinel that has to be tested before use is a sentinel somebody
    /// forgets to test.
    func position(_ key: String) -> Double {
        watch[key]?.position ?? 0
    }

    /// Record where playback got to.
    ///
    /// `duration` may be 0 when it is not known, in which case the position is
    /// stored as given and nothing is inferred about being finished.
    ///
    /// Three cases, and the second is the one that makes this worth a function
    /// rather than a setter.
    func setPosition(_ key: String, seconds rawSeconds: Double, duration: Double) {
        let seconds = max(0, rawSeconds)
        var w = watch[key] ?? WatchState()

        let finished = duration > 0 && seconds >= duration * UserData.watchedFraction

        if finished {
            /* Watched, and no resume point: re-opening something you finished
             * should start it again rather than drop you back at the end
             * card. */
            w.watched = true
            w.position = 0
        } else if seconds < UserData.resumeMinSeconds {
            /* Opening a video and closing it again must not litter the library
             * with eight-second resume offers. The watched flag is untouched:
             * a video you have already seen does not become unseen because you
             * glanced at the first ten seconds of it. */
            w.position = 0
        } else {
            w.position = seconds
        }

        w.updated = UserData.nowISO()
        watch[key] = w
        dirty = true
    }

    /// How many videos are marked watched, for the UI to say what the facet is
    /// a subset of.
    var watchedCount: Int { watchedKeys.count }

    /// The watched keys as a set, for the filter's `watchedKeys`. Rebuilt on
    /// demand rather than maintained beside `watch`, because Swift's value
    /// semantics make the derived answer cheap and a second stored copy of a
    /// fact is a thing that can drift.
    var watchedKeys: Set<String> {
        Set(watch.filter { $0.value.watched }.keys)
    }

    // MARK: - Playlists

    /// In creation order.
    var playlists: [Playlist] { playlistsByOrder }

    func playlist(_ id: String) -> Playlist? {
        playlistsByOrder.first { $0.id == id }
    }

    /// Returns the new playlist, or nil for a blank or whitespace-only name --
    /// an unnamed playlist is unfindable.
    ///
    /// Duplicate names are ALLOWED: they are the user's to make, ids are what
    /// identify a playlist, and refusing "Watch later" twice would be this app
    /// deciding something it has no business deciding.
    @discardableResult
    func createPlaylist(named rawName: String) -> Playlist? {
        let name = rawName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else { return nil }

        let pl = Playlist(id: UUID().uuidString, name: name, keys: [])
        playlistsByOrder.append(pl)
        dirty = true
        return pl
    }

    @discardableResult
    func renamePlaylist(_ id: String, to rawName: String) -> Bool {
        let name = rawName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else { return false }
        guard let i = playlistsByOrder.firstIndex(where: { $0.id == id }) else {
            return false
        }
        playlistsByOrder[i].name = name
        dirty = true
        return true
    }

    @discardableResult
    func deletePlaylist(_ id: String) -> Bool {
        guard let i = playlistsByOrder.firstIndex(where: { $0.id == id }) else {
            return false
        }
        playlistsByOrder.remove(at: i)
        dirty = true
        return true
    }

    /// Adding a key that is already in the playlist is a no-op rather than a
    /// duplicate: a playlist is a set the user ordered, not a bag.
    @discardableResult
    func addToPlaylist(_ id: String, key: String) -> Bool {
        guard let i = playlistsByOrder.firstIndex(where: { $0.id == id }) else {
            return false
        }
        guard !playlistsByOrder[i].keys.contains(key) else { return false }
        playlistsByOrder[i].keys.append(key)
        dirty = true
        return true
    }

    @discardableResult
    func removeFromPlaylist(_ id: String, key: String) -> Bool {
        guard let i = playlistsByOrder.firstIndex(where: { $0.id == id }) else {
            return false
        }
        guard let k = playlistsByOrder[i].keys.firstIndex(of: key) else {
            return false
        }
        playlistsByOrder[i].keys.remove(at: k)
        dirty = true
        return true
    }

    func playlistContains(_ id: String, key: String) -> Bool {
        playlist(id)?.keys.contains(key) ?? false
    }

    /// A playlist's keys as a set, for the filter's `playlistKeys`. An EMPTY
    /// playlist yields an empty set rather than nil: "this playlist has
    /// nothing in it" must show nothing, not everything.
    func playlistKeys(_ id: String) -> Set<String>? {
        guard let pl = playlist(id) else { return nil }
        return Set(pl.keys)
    }

    // MARK: - Helpers

    private static func nowISO() -> String {
        let f = ISO8601DateFormatter()
        return f.string(from: Date())
    }
}
