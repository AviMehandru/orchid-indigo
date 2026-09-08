/* The download engine: what actually starts a run.
 *
 * ONE RUN AT A TIME, deliberately. The queue is strictly sequential because
 * independent `ytdl` invocations race on shared state (global_manifest.json,
 * channel_manifest.json, the Channel Info refresh throttle, download.log).
 * --workers N is the supported way to get real parallelism -- it enumerates
 * every video up front so no two workers are assigned the same one, and
 * postprocess.ps1 has matching file locking. So "run several at once" is the
 * workers control, not a wider queue, and the queue never starts a second
 * process.
 *
 * THREADING. One worker thread owns the queue and the child process. It never
 * touches SwiftUI: lines and state changes go into a buffer under a lock, and
 * a 50ms timer on the main thread drains them into @Published properties. yt-dlp
 * redraws its progress line several times a second, and publishing each line as
 * it arrives would put SwiftUI in a re-render storm for the whole of a long
 * download. The GTK app solved this with an async queue and a g_idle drain; the
 * shape here is the same because the problem is.
 */

import Combine
import Darwin
import Foundation

private let maxLogLines = 4000
private let maxHistory = 300
/// At 50ms the bar still looks continuous and the main thread wakes 20 times a
/// second instead of hundreds.
private let drainInterval = 0.05

final class Runner: ObservableObject {
    // MARK: Published, main thread only

    @Published private(set) var queue: [RunRecord] = []
    @Published private(set) var history: [RunRecord] = []
    @Published private(set) var current: RunRecord?
    @Published private(set) var progress = RunProgress()
    @Published private(set) var paused = false

    /// The permanent lines of the running log, capped.
    @Published private(set) var logLines: [String] = []
    /// The last carriage-return redraw, which REPLACES its predecessor rather
    /// than appending. Without that distinction one download produces thousands
    /// of near-identical rows.
    @Published private(set) var transientLine: String?

    var isRunning: Bool { current != nil }

    // MARK: Shared state, under `lock`

    private let lock = NSCondition()
    private var pendingQueue: [RunRecord] = []
    private var pendingHistory: [RunRecord] = []
    private var pendingCurrent: RunRecord?
    private var pendingProgress = RunProgress()
    private var pendingPaused = false
    private var pendingLines: [(text: String, transient: Bool)] = []
    private var stateDirty = false
    private var childPID: pid_t = 0
    private var cancelRequested = false
    private var stopRequested = false
    private var counter: UInt32 = 0

    private var worker: Thread?
    private var timer: Timer?
    private let stopped = DispatchSemaphore(value: 0)

    // MARK: - Lifecycle

    init() {
        /* Restore what the last session left behind. A queue that survives a
         * restart is the difference between "I queued twelve channels
         * overnight" being a plan and being a thing you have to babysit. */
        pendingHistory = Runner.readRecords(at: Runner.stateFile("history.json"))
        pendingQueue = Runner.readRecords(at: Runner.stateFile("queue.json"))

        /* Anything recorded as running belongs to a process that died with the
         * last window. Left as "running" it would be a row that never
         * resolves. */
        for i in pendingHistory.indices where pendingHistory[i].state == "running" {
            pendingHistory[i].state = "failed"
            if pendingHistory[i].lastLine.isEmpty {
                pendingHistory[i].lastLine =
                    "Interrupted — the window closed while this was running."
            }
        }

        queue = pendingQueue
        history = pendingHistory
    }

    /* Separate from init so the window can be built, and only then begin
     * accepting runs -- rather than having a restored queue start emitting into
     * a view that does not exist yet. */
    func start() {
        guard worker == nil else { return }

        let t = Timer(timeInterval: drainInterval, repeats: true) { [weak self] _ in
            self?.drain()
        }
        RunLoop.main.add(t, forMode: .common)
        timer = t

        let thread = Thread { [weak self] in self?.workerLoop() }
        thread.name = "ytdl-runner"
        thread.start()
        worker = thread
    }

    /* Ask the worker to finish, and take the running download's process tree
     * with it. Safe to call more than once.
     *
     * Called from applicationWillTerminate, which is why the grace period is
     * short and the wait is bounded: macOS gives a quitting app a few seconds
     * in total, and a queue that keeps downloading after its window is gone --
     * with nothing reading its output -- is the thing this has to prevent. */
    func stop() {
        lock.lock()
        stopRequested = true
        lock.broadcast()
        let pid = childPID
        lock.unlock()

        if pid > 0 { Spawn.killTree(pid: pid, grace: 0.4) }

        timer?.invalidate()
        timer = nil
        if worker != nil {
            _ = stopped.wait(timeout: .now() + 2)
            worker = nil
        }
    }

    // MARK: - Public actions

    enum EnqueueError: LocalizedError {
        case noURL
        var errorDescription: String? { "Enter a URL first." }
    }

    @discardableResult
    func enqueue(_ opts: RunOptions) throws -> String {
        guard !opts.url.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            throw EnqueueError.noURL
        }

        var rec = RunRecord()
        rec.opts = opts
        rec.command = opts.commandPreview()
        rec.state = "queued"
        rec.started = Runner.now()

        lock.lock()
        /* Unique enough without a UUID dependency: the clock, the pid and a
         * counter. These only have to be distinct within one history file. */
        counter &+= 1
        rec.id = "\(Runner.now())-\(String(getpid(), radix: 16))\(String(counter, radix: 16))"
        pendingQueue.append(rec)
        persistLocked()
        stateDirty = true
        lock.broadcast()
        lock.unlock()

        return rec.id
    }

    /// True if something was actually running.
    @discardableResult
    func cancel() -> Bool {
        lock.lock()
        let pid = childPID
        if pid > 0 { cancelRequested = true }
        lock.unlock()

        guard pid > 0 else { return false }
        /* Off the main thread: killTree deliberately waits between SIGTERM and
         * SIGKILL, and doing that on the main thread freezes the window for a
         * second and a half at the exact moment the user pressed Cancel. */
        DispatchQueue.global(qos: .userInitiated).async {
            Spawn.killTree(pid: pid)
        }
        return true
    }

    func setPaused(_ value: Bool) {
        lock.lock()
        pendingPaused = value
        stateDirty = true
        lock.broadcast()
        lock.unlock()
    }

    func removeQueued(id: String) {
        lock.lock()
        pendingQueue.removeAll { $0.id == id }
        persistLocked()
        stateDirty = true
        lock.unlock()
    }

    func clearHistory() {
        lock.lock()
        pendingHistory.removeAll()
        persistLocked()
        stateDirty = true
        lock.unlock()
    }

    // MARK: - Draining to the main thread

    private func drain() {
        var lines: [(text: String, transient: Bool)] = []
        var snapshot: (queue: [RunRecord], history: [RunRecord], current: RunRecord?,
                       progress: RunProgress, paused: Bool)?

        lock.lock()
        /* Bounded per tick. A run that produces output faster than the window
         * can draw must not let this loop starve the frame clock -- the backlog
         * simply moves on the next tick, 50ms later. */
        let take = min(pendingLines.count, 400)
        if take > 0 {
            lines = Array(pendingLines.prefix(take))
            pendingLines.removeFirst(take)
        }
        if stateDirty {
            stateDirty = false
            snapshot = (pendingQueue, pendingHistory, pendingCurrent, pendingProgress, pendingPaused)
        }
        lock.unlock()

        for (text, transient) in lines {
            if transient {
                // A redraw REPLACES its predecessor rather than adding a row.
                transientLine = text
            } else {
                /* A permanent line appends BELOW the live redraw rather than
                 * swallowing it, so the last progress reading a download
                 * printed stays in the log instead of vanishing the moment the
                 * next stage announces itself. */
                if let last = transientLine {
                    logLines.append(last)
                    transientLine = nil
                }
                logLines.append(text)
            }
        }
        /* Bounded, for the same reason the runner bounds its own copy: a --sync
         * of a large channel emits far more than anyone will scroll back
         * through. Trimmed in blocks rather than one line at a time so a busy
         * run is not doing an O(n) removeFirst per line. */
        if logLines.count > maxLogLines + 200 {
            logLines.removeFirst(logLines.count - maxLogLines)
        }

        if let s = snapshot {
            queue = s.queue
            history = s.history
            current = s.current
            progress = s.progress
            paused = s.paused
        }
    }

    func clearLog() {
        logLines.removeAll()
        transientLine = nil
    }

    // MARK: - The worker

    private func workerLoop() {
        while true {
            lock.lock()
            while pendingQueue.isEmpty || pendingPaused {
                if stopRequested {
                    lock.unlock()
                    stopped.signal()
                    return
                }
                lock.wait(until: Date().addingTimeInterval(0.5))
            }
            if stopRequested {
                lock.unlock()
                stopped.signal()
                return
            }
            var item = pendingQueue.removeFirst()
            persistLocked()
            stateDirty = true
            lock.unlock()

            item.state = "running"
            item.started = Runner.now()
            runOne(&item)
        }
    }

    private func runOne(_ rec: inout RunRecord) {
        guard let pwsh = Paths.findPwsh() else {
            fail(&rec, "pwsh (PowerShell 7) was not found. Every stage of this pipeline is a "
                + "PowerShell script, so nothing can run without it — install it with "
                + "`brew install --cask powershell` and re-run setup.")
            return
        }

        let script = Paths.join(Paths.scriptsDir(), "ytdl.ps1")
        guard Paths.isRegularFile(script) else {
            fail(&rec, "\(script) does not exist. This app drives the installed pipeline, not a "
                + "checkout — run the installer, or set YTDLP_INSTALL_ROOT to where it lives.")
            return
        }

        /* Where this run will write. --workers > 1 splits into
         * download.worker-<id>.log files instead, which the history row links to
         * by directory rather than by name. */
        let dataRoot = rec.opts.dataRoot.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            ? Paths.installRoot()
            : Paths.expandTilde(rec.opts.dataRoot.trimmingCharacters(in: .whitespacesAndNewlines))
        rec.logPath = Paths.join(Paths.join(dataRoot, "Archive Logs/Logs"), "download.log")

        lock.lock()
        pendingProgress = RunProgress()
        pendingProgress.stage = "starting"
        pendingCurrent = rec
        cancelRequested = false
        stateDirty = true
        lock.unlock()
        DispatchQueue.main.async { [weak self] in self?.clearLog() }

        var env = ProcessInfo.processInfo.environment
        /* A .app bundle inherits launchd's PATH, not a login shell's, so
         * Homebrew's bin directories are added explicitly. ytdl.ps1 runs yt-dlp
         * and ffmpeg BY NAME; without this the pipeline fails to find tools the
         * same command finds in a terminal, and reports it as an extractor
         * error several layers from the cause. */
        env["PATH"] = Paths.childPath()

        let child: SpawnedChild
        do {
            child = try Spawn.run(
                executable: pwsh,
                arguments: ["-NoProfile", "-File", script] + rec.opts.toArgs(),
                environment: env
            )
        } catch {
            fail(&rec, error.localizedDescription)
            return
        }

        lock.lock()
        childPID = child.pid
        lock.unlock()

        /* Both pumps must finish before the status is read: they are what fills
         * in lastLine and the session summary, and a record written while they
         * are still draining would lose the counts the run just reported. */
        let group = DispatchGroup()
        for fd in [child.stdoutFD, child.stderrFD] {
            DispatchQueue.global(qos: .utility).async(group: group) { [weak self] in
                Spawn.pumpLines(fd: fd) { text, transient in
                    self?.ingest(line: text, transient: transient)
                }
            }
        }
        group.wait()

        let code = Spawn.waitExitCode(pid: child.pid)

        lock.lock()
        let cancelled = cancelRequested
        cancelRequested = false
        childPID = 0
        if let live = pendingCurrent {
            rec.videosTouched = live.videosTouched
            rec.archiveSkipped = live.archiveSkipped
            rec.errors = live.errors
            rec.warnings = live.warnings
            rec.lastLine = live.lastLine
        }
        lock.unlock()

        rec.exitCode = code
        rec.finished = Runner.now()
        rec.state = cancelled ? "cancelled" : (code == 0 ? "done" : "failed")
        finish(rec)
    }

    /// Called on a pump thread for every line of the child's output.
    private func ingest(line: String, transient: Bool) {
        lock.lock()
        if OutputParser.parseProgressLine(line, into: &pendingProgress) {
            stateDirty = true
        }
        if var cur = pendingCurrent {
            cur.lastLine = line
            if let summary = OutputParser.parseSessionSummary(line) {
                cur.videosTouched = summary.videos
                cur.archiveSkipped = summary.skipped
                cur.errors = summary.errors
                cur.warnings = summary.warnings
            }
            pendingCurrent = cur
            stateDirty = true
        }
        pendingLines.append((line, transient))
        /* Bounded on the PRODUCER side too. The drain takes at most 400 lines
         * per tick, so a run that outpaces it -- a --sync of a large channel
         * does -- would otherwise grow this without limit for the length of the
         * download. The oldest lines are the ones to lose. */
        if pendingLines.count > maxLogLines {
            pendingLines.removeFirst(pendingLines.count - maxLogLines)
        }
        lock.unlock()
    }

    private func fail(_ rec: inout RunRecord, _ why: String) {
        rec.state = "failed"
        rec.finished = Runner.now()
        rec.lastLine = why

        lock.lock()
        pendingLines.append((why, false))
        lock.unlock()

        finish(rec)
    }

    private func finish(_ rec: RunRecord) {
        lock.lock()
        pendingCurrent = nil
        childPID = 0
        pendingProgress = RunProgress()
        pendingHistory.insert(rec, at: 0)
        if pendingHistory.count > maxHistory {
            pendingHistory.removeLast(pendingHistory.count - maxHistory)
        }
        persistLocked()
        stateDirty = true
        lock.unlock()
    }

    // MARK: - Persistence

    private static func now() -> Int64 { Int64(Date().timeIntervalSince1970) }

    static func stateFile(_ name: String) -> String {
        Paths.join(Paths.stateDir(), name)
    }

    /* Caller holds the lock. Snapshots under it and writes OUTSIDE it.
     *
     * Two JSON files is real disk work, and this lock is taken by both pump
     * threads for every line of a running download. Writing while holding it
     * stalled the whole run -- and the main thread with it, since enqueue and
     * the queue list take the same lock. */
    private func persistLocked() {
        let history = pendingHistory
        let queue = pendingQueue
        Runner.persistQueue.async {
            Runner.writeRecords(history, to: Runner.stateFile("history.json"))
            Runner.writeRecords(queue, to: Runner.stateFile("queue.json"))
        }
    }

    /// Serial, so two snapshots cannot reach the same file out of order.
    private static let persistQueue = DispatchQueue(label: "ytdl.persist")

    /* Written through a temp file and renamed.
     *
     * The queue and the history are the same category of data as profiles.json:
     * losing a queue somebody built up, or the record of what ran overnight, is
     * losing real work. A truncated write from a crash or a full disk would
     * take all of it, and a rename within one filesystem is atomic. */
    static func writeRecords(_ records: [RunRecord], to path: String) {
        AtomicFile.write(JSONFile.data(from: records.map { $0.toJSON() }), to: path)
    }

    static func readRecords(at path: String) -> [RunRecord] {
        guard let data = FileManager.default.contents(atPath: path),
              let raw = (try? JSONSerialization.jsonObject(with: data)) as? [Any]
        else { return [] }
        return raw.compactMap { $0 as? [String: Any] }.map { RunRecord.fromJSON($0) }
    }
}

/// Temp file, then rename. Used by every file this app owns -- settings,
/// profiles, the queue and the history.
enum AtomicFile {
    @discardableResult
    static func write(_ data: Data?, to path: String) -> Bool {
        guard let data else { return false }
        let dir = (path as NSString).deletingLastPathComponent
        try? FileManager.default.createDirectory(
            atPath: dir, withIntermediateDirectories: true
        )

        let tmp = path + ".tmp"
        guard (try? data.write(to: URL(fileURLWithPath: tmp))) != nil else { return false }

        do {
            /* replaceItemAt rather than moveItem: moveItem REFUSES when the
             * destination exists, which after the first save is always. */
            if FileManager.default.fileExists(atPath: path) {
                _ = try FileManager.default.replaceItemAt(
                    URL(fileURLWithPath: path),
                    withItemAt: URL(fileURLWithPath: tmp)
                )
            } else {
                try FileManager.default.moveItem(atPath: tmp, toPath: path)
            }
            return true
        } catch {
            try? FileManager.default.removeItem(atPath: tmp)
            return false
        }
    }
}
