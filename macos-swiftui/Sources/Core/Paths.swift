/* Where everything lives.
 *
 * Every path rule here is a copy of one that already exists in the pipeline,
 * and the copy is deliberate: this process is started by the Finder or by
 * launchd, not by ytdl, so it inherits nothing. The rules it mirrors:
 *
 *   install root   run_ytdlp.ps1 / ytdl.ps1 platform block
 *   data root      defaults to the install root, per -DataRoot handling
 *   archive root   <dataRoot>/Youtube Videos/Complete Archive
 *   cache dir      archive-viewer.py's default_cache_dir()
 *
 * This is the macOS app, so there is no XDG story and no C:/yt-dlp -- what
 * replaces them is ~/Library, which is where a macOS application's derived
 * and persisted state belongs.
 *
 * ~/Library/Caches/ytdl-macos and ~/Library/Application Support/ytdl-macos
 * are ITS OWN directories, not shared with the GTK app or with
 * archive-viewer.py. Three readers, three index formats: a shared directory
 * would mean each treating the others' files as corrupt.
 *
 * The one rule that must NEVER be relaxed: nothing derived is written inside
 * the archive tree. postprocess.ps1 writes a checksums.sha256 over every file
 * in a video folder, so a stray file there makes that manifest stop verifying.
 */

import CryptoKit
import Foundation

enum Paths {
    // MARK: - The environment

    /* The seam the conformance suite uses to point HOME at a fixture tree.
     *
     * The GTK app does this by calling g_setenv in its tests. Doing the same
     * here would be a lie on macOS: setenv(3) races anything else reading the
     * environment, XCTest runs test cases on a shared process, and
     * NSHomeDirectory() answers from the process's container and ignores the
     * variable entirely. An explicit override is honest about what it is, and
     * it is the only mutable state in this file.
     *
     * nil in every build that is not running tests. Values set here WIN over
     * the real environment; anything not named falls through to it, so a test
     * that redirects HOME still gets a working PATH. */
    static var environmentOverride: [String: String]?

    static func env(_ name: String) -> String? {
        if let value = environmentOverride?[name] { return value }
        return ProcessInfo.processInfo.environment[name]
    }

    // MARK: - Roots

    /* $HOME, then the passwd entry, never empty.
     *
     * The environment is read FIRST and directly, rather than going straight
     * to NSHomeDirectory(). Two reasons, and neither is style: the pipeline
     * scripts resolve the same variable, and the conformance suite points
     * HOME at a fixture tree -- NSHomeDirectory() answers from the process's
     * container and cannot be redirected, so a test written against it would
     * be writing into the real user's Library. */
    static func homeDirectory() -> String {
        if let home = env("HOME"), !home.trimmingCharacters(in: .whitespaces).isEmpty {
            return home
        }
        return NSHomeDirectory()
    }

    /* $YTDLP_INSTALL_ROOT, else ~/yt-dlp. Must agree with the platform block
     * at the top of run_ytdlp.ps1 and ytdl.ps1. */
    static func installRoot() -> String {
        if let root = env("YTDLP_INSTALL_ROOT"), !root.isEmpty {
            return root
        }
        return join(homeDirectory(), "yt-dlp")
    }

    static func scriptsDir() -> String { join(installRoot(), "scripts") }

    /* Note the plural. The INSTALLED config directory is configs/ while the
     * repo directory is config/ -- not a typo. The installed name predates the
     * repo restructure and is baked into run_ytdlp.ps1 and postprocess.ps1. */
    static func configsDir() -> String { join(installRoot(), "configs") }

    /* Derived and disposable: the thumbnail cache and anything else that one
     * rescan rebuilds. Deleting it costs a re-index and nothing else. */
    static func cacheDir() -> String {
        join(homeDirectory(), "Library/Caches/\(appDirectoryName)")
    }

    /* NOT disposable: settings, profiles, the queue and the run history.
     * Losing this loses real user data, which is why it is not under Caches
     * -- macOS empties that directory on its own when the disk fills.
     *
     * Derived from HOME rather than from
     * FileManager.url(for: .applicationSupportDirectory): that call answers
     * from the process's container, which the conformance suite cannot
     * redirect, so the tests would write into the real user's Library. */
    static func stateDir() -> String {
        join(homeDirectory(), "Library/Application Support/\(appDirectoryName)")
    }

    /// The single leaf name both directories use. Deliberately not shared with
    /// `ytdl-gtk` or `ytdlp-archive-viewer`.
    static let appDirectoryName = "ytdl-macos"

    // MARK: - Path helpers

    /* "~" and "~/..." only. A bare "~user" is deliberately not expanded: the
     * pipeline does not expand it either, and silently resolving it here would
     * reintroduce exactly the two-different-folders bug that made the library
     * index one path while downloads went to another. */
    static func expandTilde(_ path: String) -> String {
        if path == "~" { return homeDirectory() }
        if path.hasPrefix("~/") {
            return join(homeDirectory(), String(path.dropFirst(2)))
        }
        return path
    }

    static func join(_ base: String, _ leaf: String) -> String {
        (base as NSString).appendingPathComponent(leaf)
    }

    /* Lexical canonicalisation: "." and ".." are resolved, symlinks are not.
     * That is what standardizedFileURL does and what is wanted -- a symlinked
     * archive root should stay spelled the way the user gave it. */
    static func canonical(_ path: String) -> String {
        URL(fileURLWithPath: path).standardizedFileURL.path
    }

    static func isDirectory(_ path: String) -> Bool {
        var isDir: ObjCBool = false
        let ok = FileManager.default.fileExists(atPath: path, isDirectory: &isDir)
        return ok && isDir.boolValue
    }

    static func isRegularFile(_ path: String) -> Bool {
        var isDir: ObjCBool = false
        let ok = FileManager.default.fileExists(atPath: path, isDirectory: &isDir)
        return ok && !isDir.boolValue
    }

    // MARK: - Finding the archive

    /* A directory is "a channel folder" if any of its first 60 children is
     * either named "Channel Info" or holds a Final files/ or Video metadata/
     * subfolder.
     *
     * The cap is not laziness: a user can point this at their home directory,
     * and the answer is decided by the first handful of entries in every real
     * case. Reading an unbounded directory to say "no" is how a folder picker
     * hangs. */
    private static func looksLikeChannelDir(_ path: String) -> Bool {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: path) else {
            return false
        }
        var seen = 0
        for name in names.sorted() {
            if seen >= 60 { break }
            let child = join(path, name)
            if !isDirectory(child) { continue }
            seen += 1

            if name == "Channel Info" { return true }
            if isDirectory(join(child, "Final files")) { return true }
            if isDirectory(join(child, "Video metadata")) { return true }
        }
        return false
    }

    /* Accept anything reasonable the user might point at -- a data root, the
     * "Youtube Videos" folder, "Complete Archive" itself, a channel folder, or
     * a reorganised tree -- and find the real Complete Archive directory. Same
     * acceptance set as archive-viewer.py's --root, so a path that works for
     * one works for both. nil if nothing plausible is there. */
    static func resolveArchiveRoot(_ candidate: String?) -> String? {
        guard let candidate else { return nil }

        let expanded = expandTilde(candidate)
        guard FileManager.default.fileExists(atPath: expanded) else { return nil }
        let p = canonical(expanded)

        let nested = join(join(p, "Youtube Videos"), "Complete Archive")
        let direct = join(p, "Complete Archive")
        for try_ in [nested, direct, p] {
            if isDirectory(try_), (try_ as NSString).lastPathComponent == "Complete Archive" {
                return try_
            }
        }

        // Pointed at a channel folder, or somewhere below the root: walk up.
        var cur = p
        while cur != "/" && cur != "." && !cur.isEmpty {
            if isDirectory(cur), (cur as NSString).lastPathComponent == "Complete Archive" {
                return cur
            }
            let parent = (cur as NSString).deletingLastPathComponent
            if parent == cur || parent.isEmpty { break }
            cur = parent
        }

        /* Last resort: a directory whose children look like channel folders is
         * good enough to index, whatever it happens to be called. */
        if isDirectory(p) {
            if let names = try? FileManager.default.contentsOfDirectory(atPath: p) {
                var seen = 0
                for name in names.sorted() {
                    if seen >= 60 { break }
                    let child = join(p, name)
                    if !isDirectory(child) { continue }
                    seen += 1
                    if looksLikeChannelDir(child) { return p }
                }
            }
            if looksLikeChannelDir(p) { return p }
        }

        return nil
    }

    /* The usual install locations, in order. nil if none of them hold one. */
    static func autodetectArchiveRoot() -> String? {
        let home = homeDirectory()
        let candidates = [
            installRoot(),
            join(home, "yt-dlp"),
            join(home, "Documents/yt-dlp"),
            /* Movies is on this list and not on Linux's: it is where a macOS
             * user is most likely to have put a video archive, and it costs
             * one stat to check. */
            join(home, "Movies/yt-dlp"),
            home,
        ]
        for candidate in candidates {
            if let found = resolveArchiveRoot(candidate) { return found }
        }
        return nil
    }

    // MARK: - The opaque key

    /* The opaque key a video folder is addressed by.
     *
     * The UI never holds a filesystem path: it holds one of these plus an
     * index into the entry's own file list, and the path is resolved from the
     * index. Traversal is off the table because no route accepts a path, not
     * because a filter has to be right. SHA-256 of the '/'-separated relative
     * path, truncated to 8 bytes -- 16 hex characters, byte for byte the same
     * key the GTK app computes for the same folder.
     *
     * Backslashes are normalised because manifests written on Windows use them
     * in places the contract does not cover, and the same folder must produce
     * the same key whichever platform indexed it. */
    static func key(for rel: String) -> String {
        let normalised = rel.replacingOccurrences(of: "\\", with: "/")
        let digest = SHA256.hash(data: Data(normalised.utf8))
        return digest.prefix(8).map { String(format: "%02x", $0) }.joined()
    }

    // MARK: - Executables

    /* Locate an executable on PATH. Deliberately not a which(1) subprocess,
     * which would be one more thing that can be missing. */
    static func which(_ name: String) -> String? {
        /* An explicit path is used as given, so a configured absolute pwsh is
         * not second-guessed against PATH. */
        if name.contains("/") {
            return FileManager.default.isExecutableFile(atPath: name) ? name : nil
        }

        guard let pathVar = env("PATH") else { return nil }
        for dir in pathVar.split(separator: ":", omittingEmptySubsequences: true) {
            let candidate = join(String(dir), name)
            if FileManager.default.isExecutableFile(atPath: candidate),
               !isDirectory(candidate) {
                return candidate
            }
        }
        return nil
    }

    /* pwsh, or nil. Every stage of this pipeline is a PowerShell 7 script, so
     * a missing pwsh is not a degraded mode -- no download can run at all, and
     * the UI says exactly that rather than failing at spawn time with a
     * confusing OS error.
     *
     * The extra locations matter MORE here than they do on Linux. A process
     * launched from the Finder inherits launchd's PATH -- /usr/bin:/bin:
     * /usr/sbin:/sbin -- and not the login shell's, so a Homebrew pwsh is
     * invisible to `which` inside a .app bundle even though it is on the PATH
     * of every terminal on the machine. Checking the install locations
     * directly is what stops the app reporting "PowerShell is not installed"
     * on a machine where the user just ran it in Terminal. */
    static func findPwsh() -> String? {
        if let found = which("pwsh") { return found }

        let extra = [
            "/opt/homebrew/bin/pwsh",              // Homebrew, Apple silicon
            "/usr/local/bin/pwsh",                 // Homebrew, Intel
            "/usr/local/microsoft/powershell/7/pwsh", // the .pkg installer
            "/opt/microsoft/powershell/7/pwsh",
            "/usr/bin/pwsh",
        ]
        for candidate in extra where FileManager.default.isExecutableFile(atPath: candidate) {
            return candidate
        }
        return nil
    }

    /* The directories a Homebrew or MacPorts install lives in, appended to
     * PATH for every child this app spawns. ytdl.ps1 runs yt-dlp and ffmpeg by
     * name, so a pipeline started from a .app bundle would otherwise fail to
     * find tools the same command finds in a terminal -- reported as an
     * extractor error, several layers away from the actual cause. */
    static func childPath() -> String {
        let inherited = env("PATH") ?? "/usr/bin:/bin:/usr/sbin:/sbin"
        var parts = inherited.split(separator: ":").map(String.init)
        for extra in ["/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin"] {
            if !parts.contains(extra), isDirectory(extra) {
                parts.append(extra)
            }
        }
        return parts.joined(separator: ":")
    }
}
