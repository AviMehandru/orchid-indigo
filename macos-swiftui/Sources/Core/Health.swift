/* Health, config, and integrity.
 *
 * Everything here answers a question you would otherwise answer by running a
 * command and reading a file: is pwsh installed, is yt-dlp current, which
 * CONFIG_VERSION is installed, do this video's checksums still verify.
 *
 * DELIBERATELY READ-ONLY. Nothing here installs, updates or repairs anything.
 * `yt-dlp -U` is run by run_ytdlp.ps1's own once-per-24h dependency check, and
 * a second updater racing it from a GUI is exactly the kind of shared-state
 * collision the pipeline spent a release removing.
 */

import CryptoKit
import Darwin
import Foundation

// MARK: - Dependencies

struct Dependency: Identifiable {
    let name: String
    /// required | recommended | optional
    let importance: String
    let note: String
    let found: Bool
    let path: String?
    /// nil when the probe timed out or the tool printed nothing -- which is a
    /// different state from missing, and worth saying rather than showing a
    /// blank.
    let version: String?

    var id: String { name }
}

enum Health {
    /* How long a `--version` call gets before it is killed.
     *
     * Not defensiveness: `yt-dlp --version` on a machine whose network is being
     * filtered, and `pwsh --version` with a slow profile on a network share, can
     * both sit for a long time, and a Health pane that hangs on one of them is
     * indistinguishable from a Health pane that is broken. */
    static let probeTimeout: TimeInterval = 8
    private static let cacheTTL: TimeInterval = 300

    private struct DepSpec {
        let name: String
        let importance: String
        let args: [String]
        let note: String
    }

    private static let specs: [DepSpec] = [
        DepSpec(name: "pwsh", importance: "required", args: ["--version"],
                note: "Every stage of the pipeline is a PowerShell 7 script. Without it "
                    + "nothing downloads. `brew install --cask powershell`."),
        DepSpec(name: "yt-dlp", importance: "required", args: ["--version"],
                note: "Does all the actual extraction. run_ytdlp.ps1 runs `yt-dlp -U` on a "
                    + "24h throttle."),
        DepSpec(name: "ffmpeg", importance: "required", args: ["-version"],
                note: "Merging, embedding and thumbnails."),
        DepSpec(name: "ffprobe", importance: "recommended", args: ["-version"],
                note: "Without it the Media tab cannot read stream details. Everything else "
                    + "still works."),
        DepSpec(name: "deno", importance: "recommended", args: ["--version"],
                note: "YouTube's JS challenge needs a JS runtime. Its absence usually shows "
                    + "up as mid-download HTTP 403s rather than an obvious error."),
        DepSpec(name: "node", importance: "optional", args: ["--version"],
                note: "Runtime for the PO token provider server."),
        DepSpec(name: "python3", importance: "optional", args: ["--version"],
                note: "Runs archive-viewer.py and installs the PO token plugin."),
    ]

    private static let cacheLock = NSLock()
    private static var cached: [Dependency]?
    private static var cachedAt: Date?

    /* The probe result, cached.
     *
     * Seven `--version` calls cost seconds -- pwsh and yt-dlp are the better
     * part of one each on their own -- and the Health pane is opened far more
     * often than a toolchain changes. `force` is the Re-probe button: it skips
     * the cache, which is what someone who has just installed a missing
     * dependency expects that button to do.
     *
     * BLOCKS for up to the probe timeout. Call it off the main thread. */
    static func dependencies(force: Bool) -> [Dependency] {
        cacheLock.lock()
        if !force, let hit = cached, let at = cachedAt,
           Date().timeIntervalSince(at) < cacheTTL {
            cacheLock.unlock()
            return hit
        }
        cacheLock.unlock()

        /* Probe every tool AT ONCE. These are seven independent subprocess
         * spawns with nothing shared between them, so running them one after
         * another simply added up their latencies -- that loop was the whole
         * cost of the Health pane's first paint on the GTK app. */
        var results = [Dependency?](repeating: nil, count: specs.count)
        let resultsLock = NSLock()
        let group = DispatchGroup()

        for (i, spec) in specs.enumerated() {
            DispatchQueue.global(qos: .userInitiated).async(group: group) {
                let dep = probe(spec)
                resultsLock.lock()
                results[i] = dep
                resultsLock.unlock()
            }
        }
        group.wait()

        /* Collected in SPEC order, not completion order, so the table does not
         * reshuffle itself depending on which tool answered first. */
        let out = results.compactMap { $0 }

        cacheLock.lock()
        cached = out
        cachedAt = Date()
        cacheLock.unlock()
        return out
    }

    private static func probe(_ spec: DepSpec) -> Dependency {
        var found = Paths.which(spec.name)
        if found == nil {
            if spec.name == "pwsh" {
                found = Paths.findPwsh()
            } else if spec.name == "python3" {
                found = Paths.which("python")
            } else {
                /* A .app bundle's PATH is launchd's, so a Homebrew tool is
                 * invisible to `which` even when every terminal on the machine
                 * finds it. Checking the two Homebrew prefixes directly is what
                 * stops this pane reporting a healthy machine as broken. */
                for prefix in ["/opt/homebrew/bin", "/usr/local/bin"] {
                    let candidate = Paths.join(prefix, spec.name)
                    if FileManager.default.isExecutableFile(atPath: candidate) {
                        found = candidate
                        break
                    }
                }
            }
        }

        guard let path = found else {
            return Dependency(name: spec.name, importance: spec.importance, note: spec.note,
                              found: false, path: nil, version: nil)
        }

        let version = firstLine(of: path, args: spec.args)
        return Dependency(name: spec.name, importance: spec.importance, note: spec.note,
                          found: true, path: path, version: version)
    }

    private final class DataBox {
        private let lock = NSLock()
        private var value = Data()
        func set(_ d: Data) { lock.lock(); value = d; lock.unlock() }
        func get() -> Data { lock.lock(); defer { lock.unlock() }; return value }
    }

    /// Run `exe args`, kill it on overrun, and return the first non-empty line
    /// of its output. ffmpeg and several others print their banner to stderr,
    /// so both streams are considered.
    private static func firstLine(of exe: String, args: [String]) -> String? {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: exe)
        process.arguments = args
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = Paths.childPath()
        process.environment = env

        let outPipe = Pipe(), errPipe = Pipe()
        process.standardOutput = outPipe
        process.standardError = errPipe
        process.standardInput = FileHandle.nullDevice

        do { try process.run() } catch { return nil }

        /* Both pipes are drained on their own queues rather than one after the
         * other. --version output is small, but a tool that decides to print a
         * page of build flags to stderr would fill that pipe and block forever
         * against a reader still waiting on stdout. */
        let outBox = DataBox(), errBox = DataBox()
        let group = DispatchGroup()
        DispatchQueue.global(qos: .userInitiated).async(group: group) {
            outBox.set(outPipe.fileHandleForReading.readDataToEndOfFile())
        }
        DispatchQueue.global(qos: .userInitiated).async(group: group) {
            errBox.set(errPipe.fileHandleForReading.readDataToEndOfFile())
        }

        /* Waited on with a semaphore rather than polled. Seven of these run at
         * once, and seven threads each waking every 20ms for up to eight
         * seconds is a lot of scheduler traffic for a pane that is mostly
         * waiting. */
        let exited = DispatchSemaphore(value: 0)
        process.terminationHandler = { _ in exited.signal() }

        var timedOut = false
        if exited.wait(timeout: .now() + probeTimeout) == .timedOut {
            timedOut = true
            kill(process.processIdentifier, SIGKILL)
            _ = exited.wait(timeout: .now() + 1)
        }
        group.wait()

        if timedOut { return nil }

        let out = String(decoding: outBox.get(), as: UTF8.self)
        let err = String(decoding: errBox.get(), as: UTF8.self)
        let text = out.isEmpty ? err : out
        let line = text.split(separator: "\n", omittingEmptySubsequences: false)
            .first.map { $0.trimmingCharacters(in: .whitespaces) } ?? ""
        return line.isEmpty ? nil : line
    }

    // MARK: - Installed pipeline files

    struct InstalledFile: Identifiable {
        let name: String
        let path: String
        let present: Bool
        let size: UInt64
        /// nil when absent.
        let modified: Date?

        var id: String { name }
    }

    /* What is INSTALLED, never what is in a checkout.
     *
     * The repo holds the sources; the installer copies them to their runtime
     * locations. Editing a file in a clone has no effect on a live install until
     * it is copied over, which is exactly the confusion this list settles. */
    static func installedFiles() -> [InstalledFile] {
        let scripts = Paths.scriptsDir()
        let configs = Paths.configsDir()

        var out: [InstalledFile] = []
        let names = [
            "run_ytdlp.ps1", "postprocess.ps1", "ytdl.ps1",
            "pot-provider.ps1", "archive-viewer.py",
        ]
        for name in names {
            out.append(stat(path: Paths.join(scripts, name), name: name))
        }
        out.append(stat(path: Paths.join(configs, "yt-dlp.conf"), name: "yt-dlp.conf"))
        return out
    }

    private static func stat(path: String, name: String) -> InstalledFile {
        let attrs = try? FileManager.default.attributesOfItem(atPath: path)
        let present = attrs != nil && Paths.isRegularFile(path)
        return InstalledFile(
            name: name,
            path: path,
            present: present,
            size: (attrs?[.size] as? NSNumber)?.uint64Value ?? 0,
            modified: present ? attrs?[.modificationDate] as? Date : nil
        )
    }

    // MARK: - yt-dlp.conf

    struct ConfigInfo {
        let path: String
        let present: Bool
        /// From "# CONFIG_VERSION:".
        let configVersion: String?
        let body: String
        let optionCount: Int
    }

    /// CONFIG_VERSION is recorded in manifest.json and download.log by both
    /// scripts, so the number shown here is the one those files will carry.
    static func configInfo() -> ConfigInfo {
        let path = Paths.join(Paths.configsDir(), "yt-dlp.conf")
        let present = Paths.isRegularFile(path)
        let body = (try? String(contentsOfFile: path, encoding: .utf8)) ?? ""

        var version: String?
        var options = 0
        for line in body.components(separatedBy: "\n") {
            let t = line.trimmingCharacters(in: .whitespaces)
            if t.hasPrefix("# CONFIG_VERSION:") {
                version = String(t.dropFirst("# CONFIG_VERSION:".count))
                    .trimmingCharacters(in: .whitespaces)
            }
            // Count only real options: a line whose first non-space is "--".
            if t.hasPrefix("--") { options += 1 }
        }

        return ConfigInfo(
            path: path, present: present, configVersion: version,
            body: body, optionCount: options
        )
    }

    // MARK: - Archive statistics

    struct ArchiveStats {
        let dataRoot: String
        let videos: Int
        let channels: Int
        let totalBytes: UInt64
        /// -1 when unreadable.
        let globalManifestEntries: Int
        /// -1 when unreadable.
        let archiveTxtIDs: Int
        let logDir: String
        let historySnapshots: Int
    }

    static func archiveStats(
        dataRoot: String, videos: Int, channels: Int, totalBytes: UInt64
    ) -> ArchiveStats {
        let logDir = Paths.join(dataRoot, "Archive Logs/Logs")

        var globalEntries = -1
        let globalPath = Paths.join(dataRoot, "Youtube Videos/global_manifest.json")
        if let data = FileManager.default.contents(atPath: globalPath),
           let parsed = try? JSONSerialization.jsonObject(with: data) {
            if let array = parsed as? [Any] {
                globalEntries = array.count
            } else if parsed is [String: Any] {
                /* A single-video archive serialises as one object, not a
                 * one-element array -- ConvertTo-Json unrolls it. Counting that
                 * as zero would be wrong in exactly the case a new user sees. */
                globalEntries = 1
            }
        }

        var archiveIDs = -1
        if let text = try? String(contentsOfFile: Paths.join(logDir, "archive.txt"), encoding: .utf8) {
            archiveIDs = text.components(separatedBy: "\n")
                .filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty }.count
        }

        let historyDir = Paths.join(dataRoot, "Archive Logs/Archive History")
        let snapshots = (try? FileManager.default.contentsOfDirectory(atPath: historyDir))?.count ?? 0

        return ArchiveStats(
            dataRoot: dataRoot, videos: videos, channels: channels, totalBytes: totalBytes,
            globalManifestEntries: globalEntries, archiveTxtIDs: archiveIDs,
            logDir: logDir, historySnapshots: snapshots
        )
    }

    // MARK: - Checksums

    struct ChecksumResult {
        var present = false
        var checked = 0
        var ok = 0
        var failed: [String] = []
        var missing: [String] = []
    }

    /* Verify a video folder against its own checksums.sha256.
     *
     * Standard sha256sum format ("<hash>  <relative/path>"), written by
     * postprocess.ps1 over every file in the folder EXCEPT
     * Logs/video_postprocessing.log -- excluded because it is still being
     * appended to when the hashes are computed, and a manifest that always
     * reports one failure teaches you to ignore its failures. Its absence is
     * therefore not a failure here either.
     *
     * BLOCKS: hashing every file in a folder is seconds of work on a large
     * video. Call it off the main thread. */
    static func verifyChecksums(videoDir: String) -> ChecksumResult {
        var r = ChecksumResult()

        let file = Paths.join(videoDir, "Video metadata/checksums.sha256")
        guard let body = try? String(contentsOfFile: file, encoding: .utf8) else {
            return r  // present stays false
        }
        r.present = true

        for rawLine in body.components(separatedBy: "\n") {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.isEmpty { continue }

            /* sha256sum format: hash, two spaces, then the path. Splitting on
             * the double space rather than on whitespace matters -- a filename
             * with a space in it is normal here. */
            guard let sep = line.range(of: "  ") else { continue }
            let hash = String(line[line.startIndex..<sep.lowerBound])
            let rel = String(line[sep.upperBound...])
            r.checked += 1

            let path = Paths.join(videoDir, rel)
            guard Paths.isRegularFile(path) else {
                r.missing.append(rel)
                continue
            }

            if let actual = sha256(ofFile: path), actual.caseInsensitiveCompare(hash) == .orderedSame {
                r.ok += 1
            } else {
                r.failed.append(rel)
            }
        }
        return r
    }

    /// Streamed in 1MB chunks: a video file does not fit in memory twice.
    static func sha256(ofFile path: String) -> String? {
        guard let handle = FileHandle(forReadingAtPath: path) else { return nil }
        defer { try? handle.close() }

        var hasher = SHA256()
        while true {
            let chunk = handle.readData(ofLength: 1024 * 1024)
            if chunk.isEmpty { break }
            hasher.update(data: chunk)
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    // MARK: - Log tails

    /* The last `lines` lines of a file, WITHOUT reading the file.
     *
     * download.log is appended to by every run and never rotated by the
     * pipeline, so on a machine that has been archiving for a while it is the
     * largest thing this pane touches. Reading it whole to keep the last 300
     * lines made the pane's cost grow with the age of the install, for a panel
     * whose content is fixed-size. This seeks to the last megabyte and works
     * from there. */
    static func logTail(path: String, lines: Int) -> String {
        let maxBytes: UInt64 = 1024 * 1024

        guard let handle = FileHandle(forReadingAtPath: path) else { return "" }
        defer { try? handle.close() }

        let attrs = try? FileManager.default.attributesOfItem(atPath: path)
        let size = (attrs?[.size] as? NSNumber)?.uint64Value ?? 0
        let start = size > maxBytes ? size - maxBytes : 0
        if start > 0 { handle.seek(toFileOffset: start) }

        let data = handle.readDataToEndOfFile()
        var text = String(decoding: data, as: UTF8.self)

        /* The first line of the window is dropped when the window did not start
         * at the beginning of the file, because it is almost certainly half a
         * line. */
        if start > 0, let nl = text.firstIndex(of: "\n") {
            text = String(text[text.index(after: nl)...])
        }

        var all = text.components(separatedBy: "\n")
        /* A trailing newline leaves an empty last element; dropping it keeps
         * "last 20 lines" from silently meaning 19 plus a blank. */
        if all.last?.isEmpty == true { all.removeLast() }
        return all.suffix(lines).joined(separator: "\n")
    }
}
