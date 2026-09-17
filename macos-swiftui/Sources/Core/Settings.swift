/* The persisted preferences.
 *
 * Kept in ~/Library/Application Support/ytdl-macos/settings.json -- NOT in
 * UserDefaults and NOT under Caches. UserDefaults would be the macOS-idiomatic
 * home for five values, and it is deliberately not used: the settings file sits
 * beside profiles.json, queue.json and history.json, which are files by
 * necessity, and one visible directory holding all of an app's state is worth
 * more here than idiom. It is also what makes "delete this folder and start
 * over" a complete answer.
 *
 * Written through a temp file and renamed, like everything else this app owns.
 */

import Combine
import Foundation

final class Settings: ObservableObject {
    /// The `ytdl --path` equivalent. Empty means "wherever the pipeline puts it
    /// by default", which is the install root -- a real choice, not a missing
    /// value.
    @Published var dataRoot: String = ""
    /// An explicitly chosen archive root, if autodetection guessed wrong.
    @Published var archiveRoot: String = ""
    @Published var defaultWorkers: Int = 1

    /* How the Library is ordered. Persisted as the sort key's stable ID
     * string, never as its case position: inserting a key in the middle would
     * otherwise silently change what every saved setting means.
     *
     * The FACETS are deliberately not persisted. A sort is a standing
     * preference for how you like to read a list; a facet is a question you
     * asked once, and an app that reopens showing a fifth of the archive with
     * no visible reason is an app that looks like it lost your videos. */
    @Published var librarySort: String = SortKey.date.rawValue
    /// Newest first. An archive is added to at the newest end, so what someone
    /// wants to see when the window opens is what arrived last.
    @Published var librarySortDescending: Bool = true

    private static func path() -> String {
        Paths.join(Paths.stateDir(), "settings.json")
    }

    static func load() -> Settings {
        let s = Settings()
        guard let obj = JSONFile.object(at: path()) else { return s }
        s.dataRoot = obj.str("data_root") ?? ""
        s.archiveRoot = obj.str("archive_root") ?? ""
        s.defaultWorkers = max(1, Int(obj.int("default_workers", default: 1)))
        s.librarySort = obj.str("library_sort") ?? SortKey.date.rawValue
        /* Absent reads as TRUE rather than false, which is what a plain
         * bool lookup on a missing key would give: an upgrade from a
         * settings.json written before this existed must not silently flip
         * every Library to oldest-first. */
        s.librarySortDescending = obj["library_sort_descending"] == nil
            ? true
            : obj.bool("library_sort_descending")
        return s
    }

    func save() {
        let obj: [String: Any] = [
            "data_root": dataRoot,
            "archive_root": archiveRoot,
            "default_workers": defaultWorkers,
            "library_sort": librarySort,
            "library_sort_descending": librarySortDescending,
        ]
        AtomicFile.write(JSONFile.data(from: obj), to: Settings.path())
    }

    /// The data root as a real path: the configured value with ~ expanded, or
    /// the pipeline's own default. Never empty.
    var resolvedDataRoot: String {
        let trimmed = dataRoot.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? Paths.installRoot() : Paths.expandTilde(trimmed)
    }
}
