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

    private static func path() -> String {
        Paths.join(Paths.stateDir(), "settings.json")
    }

    static func load() -> Settings {
        let s = Settings()
        guard let obj = JSONFile.object(at: path()) else { return s }
        s.dataRoot = obj.str("data_root") ?? ""
        s.archiveRoot = obj.str("archive_root") ?? ""
        s.defaultWorkers = max(1, Int(obj.int("default_workers", default: 1)))
        return s
    }

    func save() {
        let obj: [String: Any] = [
            "data_root": dataRoot,
            "archive_root": archiveRoot,
            "default_workers": defaultWorkers,
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
