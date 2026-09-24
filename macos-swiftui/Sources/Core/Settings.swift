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

    /* How YouTube is reached: the Connection section on the Downloads pane.
     *
     * Settings, not form fields and not profile fields. A cookie source or a
     * proxy is a fact about you and your network, not about the video you are
     * about to download -- so it is kept once, here, and the Runner stamps it
     * onto every run the app starts. The cookie SOURCE is stored apart from
     * the two values it chooses between, so flipping to "none" and back does
     * not lose a browser profile or a path somebody typed once. */
    /// "none" | "browser" | "file"
    @Published var cookiesSource: String = "none"
    @Published var cookiesBrowser: String = "safari"
    @Published var cookiesProfile: String = ""
    @Published var cookiesFile: String = ""
    @Published var proxy: String = ""
    @Published var limitRate: String = ""
    /// "native" | "aria2c"
    @Published var downloader: String = "native"

    /// Whether a finished queue or a failed run is announced as a notification
    /// while the app is in the background. On by default: the point is to
    /// hear about the run that failed at 3am, and a setting that has to be
    /// found first is one most people would never turn on. See Notices.swift.
    @Published var notify: Bool = true

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
        s.cookiesSource = obj.str("cookies_source").flatMap { $0.isEmpty ? nil : $0 } ?? "none"
        s.cookiesBrowser = obj.str("cookies_browser").flatMap { $0.isEmpty ? nil : $0 } ?? "safari"
        s.cookiesProfile = obj.str("cookies_profile") ?? ""
        s.cookiesFile = obj.str("cookies_file") ?? ""
        s.proxy = obj.str("proxy") ?? ""
        s.limitRate = obj.str("limit_rate") ?? ""
        s.downloader = obj.str("downloader").flatMap { $0.isEmpty ? nil : $0 } ?? "native"
        /* Absent reads as TRUE, for the upgrade case: a settings.json written
         * before this key existed must not switch notifications off. */
        s.notify = obj["notify"] == nil ? true : obj.bool("notify")
        return s
    }

    /* The connection settings as a RunOptions with only its five connection
     * fields set -- the shape Runner.setConnection and the probe take. The
     * cookie source decides which of browser/file is emitted; a source whose
     * value is empty emits nothing rather than a flag with no argument. */
    func connection() -> RunOptions {
        func trimmed(_ s: String) -> String { s.trimmingCharacters(in: .whitespacesAndNewlines) }
        var o = RunOptions()
        if cookiesSource == "browser", !trimmed(cookiesBrowser).isEmpty {
            let profile = trimmed(cookiesProfile)
            o.cookiesFromBrowser = profile.isEmpty
                ? trimmed(cookiesBrowser)
                : "\(trimmed(cookiesBrowser)):\(profile)"
        } else if cookiesSource == "file", !trimmed(cookiesFile).isEmpty {
            /* Expanded here for the same reason --path is: ytdl.ps1 has no
             * notion of "~". */
            o.cookiesFile = Paths.expandTilde(trimmed(cookiesFile))
        }
        o.proxy = trimmed(proxy)
        o.limitRate = trimmed(limitRate)
        o.downloader = downloader == "native" ? "" : downloader
        return o
    }

    func save() {
        let obj: [String: Any] = [
            "data_root": dataRoot,
            "archive_root": archiveRoot,
            "default_workers": defaultWorkers,
            "library_sort": librarySort,
            "library_sort_descending": librarySortDescending,
            "cookies_source": cookiesSource,
            "cookies_browser": cookiesBrowser,
            "cookies_profile": cookiesProfile,
            "cookies_file": cookiesFile,
            "proxy": proxy,
            "limit_rate": limitRate,
            "downloader": downloader,
            "notify": notify,
        ]
        /* Owner-only, because since the Connection settings this file can
         * hold a proxy password. */
        AtomicFile.write(JSONFile.data(from: obj), to: Settings.path(), ownerOnly: true)
    }

    /// The data root as a real path: the configured value with ~ expanded, or
    /// the pipeline's own default. Never empty.
    var resolvedDataRoot: String {
        let trimmed = dataRoot.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? Paths.installRoot() : Paths.expandTilde(trimmed)
    }
}
