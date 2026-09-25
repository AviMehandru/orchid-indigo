/* Subscriptions: the pipeline's stored list of sources and its hourly check,
 * as this app sees them.
 *
 * THIS APP RUNS NO TIMER. That is the design, not a gap. A timer here would
 * only fire while this window is open -- the one time nobody needs one -- and
 * this app, the other two in this repository and the Tauri one would each be
 * running their own, racing each other on the same channels and the same
 * manifests. So the list and the schedule live in the pipeline
 * (`ytdl --subscribe`, `ytdl --schedule install`, docs/subscriptions.md in
 * orchid-ochre) -- a launchd agent on this platform -- and every frontend
 * manages them the way it manages everything else: by building a `ytdl`
 * command line.
 *
 * What is here:
 *
 *   - a reader for `ytdl --subscriptions --json`, the contract in that doc;
 *   - the wording every row of the Subscriptions pane shows, pinned by one
 *     fixture that Tests/SubscriptionTests.swift, the GTK app's
 *     tests/test_subscriptions.c and the WinUI app's SubscriptionTests.cs all
 *     assert word for word;
 *   - the argument lists for the commands the pane sends;
 *   - one "run ytdl with these arguments and tell me what it said", for the
 *     commands that are over in a second. A check itself is NOT run this way:
 *     "Check now" goes through the Downloads queue as
 *     `ytdl --run-subscriptions ID` (RunOptions.subscriptionID), so it is
 *     sequential with every other run, shows its progress, and can be
 *     cancelled like any of them.
 */

import Foundation

/// The subscriptions_version this app reads. A document declaring a newer one
/// is refused with a message saying so rather than misread -- the pipeline
/// bumps it only when a field is removed or changes meaning.
let supportedSubscriptionsVersion = 1

struct SubscriptionRun: Equatable {
    var started: Int64 = 0
    var finished: Int64 = 0
    /// ok | errors | failed
    var result: String = ""
    var exitCode: Int = 0
    var touched: Int64 = 0
    var skipped: Int64 = 0
    var errors: Int64 = 0
    var warnings: Int64 = 0
    /// schedule | manual
    var trigger: String = ""
    /// The session's last line when it failed.
    var message: String?
}

struct Subscription: Identifiable, Equatable {
    var id: String = ""
    var url: String = ""
    var name: String?
    /// Proxy passwords already masked by the pipeline.
    var options: [String] = []
    /// nil: the pipeline's default data root.
    var dataRoot: String?
    var everyHours: Int = 24
    var enabled = true
    var added: Int64 = 0
    var updated: Int64 = 0
    var lastRun: SubscriptionRun?
    /// 0 when paused.
    var nextDue: Int64 = 0
    var due = false

    /// The name, or the URL when there is none.
    var title: String {
        if let n = name, !n.isEmpty { return n }
        return url
    }

    /* One line for the row:
     *
     *   Not checked yet · due now
     *   Checked 3 h ago · 2 new · next in 3 h
     *   Checked 24 h ago · nothing new · next in 6 d
     *   Checked just now · 3 new, 1 error · next in 60 min
     *   Check failed 2 h ago · next in 10 h
     *   Paused · last checked 3 d ago
     *
     * Relative to `now`, which should be the document's own. */
    func statusLine(now: Int64) -> String {
        if !enabled {
            guard let r = lastRun else { return "Paused · never checked" }
            return "Paused · last checked \(SubscriptionText.ago(r.started, now: now))"
        }
        let next = SubscriptionText.inFuture(nextDue > 0 ? nextDue : now, now: now)
        guard let r = lastRun else { return "Not checked yet · \(next)" }

        let ago = SubscriptionText.ago(r.started, now: now)
        // "next due now" reads badly; "due now" on its own says it.
        let tail = next.hasPrefix("in ") ? "next \(next)" : next
        if r.result == "failed" { return "Check failed \(ago) · \(tail)" }

        var found = r.touched > 0 ? "\(r.touched) new" : "nothing new"
        if r.errors > 0 { found += ", \(r.errors) error\(r.errors == 1 ? "" : "s")" }
        return "Checked \(ago) · \(found) · \(tail)"
    }

    /// A queued run that checks this subscription now. The URL and data root
    /// are carried for the history row and the log path only.
    func runOptions() -> RunOptions {
        var o = RunOptions()
        o.subscriptionID = id
        o.url = url
        o.dataRoot = dataRoot ?? ""
        return o
    }
}

struct SubscriptionSchedule: Equatable {
    /// systemd | launchd | task-scheduler
    var mechanism: String = ""
    var supported = false
    var installed = false
    var active = false
    var running = false
    /// 0 when the scheduler does not say (launchd never does).
    var nextCheck: Int64 = 0
    /// nil unknown; Linux only.
    var linger: Bool?
    var detail: String = ""

    /* The automatic-checks row:
     *
     *   On · checks hourly · next check in 25 min · only while you are logged in
     *   Off · subscriptions are checked only when you press Check now
     *   Installed but not running · turn it off and on again
     *   <the pipeline's own detail>                  when not supported here
     *
     * with " · checking now" appended while a check is running. */
    func statusLine(now: Int64) -> String {
        if !supported {
            return detail.isEmpty ? "This machine has no scheduler the pipeline can use." : detail
        }
        var out: String
        if installed && active {
            out = "On · checks hourly"
            if nextCheck > 0 {
                out += " · next check \(SubscriptionText.inFuture(nextCheck, now: now))"
            }
            if linger == false { out += " · only while you are logged in" }
        } else if installed {
            out = "Installed but not running · turn it off and on again"
        } else {
            out = "Off · subscriptions are checked only when you press Check now"
        }
        if running { out += " · checking now" }
        return out
    }
}

struct SubscriptionList: Equatable {
    var version = 0
    /// Compute every "3 h ago" against this, not the clock.
    var now: Int64 = 0
    var schedule = SubscriptionSchedule()
    var subscriptions: [Subscription] = []

    enum ParseError: LocalizedError, Equatable {
        case notADocument
        case newerVersion(Int64)

        var errorDescription: String? {
            switch self {
            case .notADocument:
                return "The pipeline did not print a subscriptions document."
            case .newerVersion(let v):
                return "The pipeline's subscriptions are version \(v); this app reads version "
                    + "\(supportedSubscriptionsVersion). Update the app."
            }
        }
    }

    /// The whole of `ytdl --subscriptions --json`'s stdout. Unknown fields are
    /// ignored; a missing one takes its default rather than failing the list.
    static func parse(_ json: String) throws -> SubscriptionList {
        guard let data = json.data(using: .utf8),
              let o = JSONFile.object(from: data)
        else { throw ParseError.notADocument }

        let version = o.int("subscriptions_version")
        guard version >= 1 else { throw ParseError.notADocument }
        guard version <= Int64(supportedSubscriptionsVersion) else {
            throw ParseError.newerVersion(version)
        }

        var l = SubscriptionList()
        l.version = Int(version)
        l.now = o.int("now", default: Int64(Date().timeIntervalSince1970))

        let sc = o.object("schedule") ?? [:]
        l.schedule.mechanism = sc.str("mechanism") ?? ""
        l.schedule.supported = sc.bool("supported")
        l.schedule.installed = sc.bool("installed")
        l.schedule.active = sc.bool("active")
        l.schedule.running = sc.bool("running")
        l.schedule.nextCheck = sc.int("next_check")
        if let v = sc["linger"], !(v is NSNull) { l.schedule.linger = sc.bool("linger") }
        l.schedule.detail = sc.str("detail") ?? ""

        for so in o.objects("subscriptions") {
            var s = Subscription()
            s.id = so.str("id") ?? ""
            s.url = so.str("url") ?? ""
            /* Without an id there is nothing to send back; a row that cannot
             * be acted on is left out rather than shown dead. */
            guard !s.id.isEmpty, !s.url.isEmpty else { continue }
            s.name = so.str("name")
            s.options = so.strings("options")
            s.dataRoot = so.str("data_root")
            s.everyHours = Int(so.int("every_hours", default: 24))
            s.enabled = so["enabled"] == nil ? true : so.bool("enabled")
            s.added = so.int("added")
            s.updated = so.int("updated")
            s.nextDue = so.int("next_due")
            s.due = so.bool("due")
            if let lr = so.object("last_run") {
                var r = SubscriptionRun()
                r.started = lr.int("started")
                r.finished = lr.int("finished")
                r.result = lr.str("result") ?? ""
                r.exitCode = Int(lr.int("exit_code"))
                r.touched = lr.int("touched")
                r.skipped = lr.int("skipped")
                r.errors = lr.int("errors")
                r.warnings = lr.int("warnings")
                r.trigger = lr.str("trigger") ?? ""
                r.message = lr.str("message")
                s.lastRun = r
            }
            l.subscriptions.append(s)
        }
        return l
    }
}

/// The relative times and interval labels, exactly as the other two apps
/// write them.
enum SubscriptionText {
    /// The intervals the pane offers. The pipeline takes any whole number of
    /// hours from 1 to 720; these are the ones worth a menu entry.
    static let intervalChoices = [1, 6, 12, 24, 72, 168]

    /* Half-up integer rounding. Not rounded(): the three apps must agree to
     * the minute on the same fixture, and floating-point rounding of x.5 is
     * the one place C, Swift and C# are allowed to differ. */
    private static func roundDiv(_ n: Int64, _ d: Int64) -> Int64 { (n + d / 2) / d }

    /// "just now", "4 min ago", "3 h ago", "2 d ago".
    static func ago(_ then: Int64, now: Int64) -> String {
        let s = max(0, now - then)
        if s < 90 { return "just now" }
        if s < 5400 { return "\(roundDiv(s, 60)) min ago" }
        if s < 129_600 { return "\(roundDiv(s, 3600)) h ago" }
        return "\(roundDiv(s, 86400)) d ago"
    }

    /// "due now", "in 25 min", "in 3 h", "in 6 d".
    static func inFuture(_ when: Int64, now: Int64) -> String {
        let d = when - now
        if d <= 0 { return "due now" }
        if d < 5400 { return "in \(max(1, roundDiv(d, 60))) min" }
        if d < 129_600 { return "in \(roundDiv(d, 3600)) h" }
        return "in \(roundDiv(d, 86400)) d"
    }

    /// "Every hour", "Every 6 hours", "Every day", "Every 7 days".
    static func every(_ hours: Int) -> String {
        if hours > 0 && hours % 24 == 0 {
            return hours == 24 ? "Every day" : "Every \(hours / 24) days"
        }
        return hours == 1 ? "Every hour" : "Every \(hours) hours"
    }

    /// --every's value: "6h", or "2d" when it is whole days.
    static func everyArgument(_ hours: Int) -> String {
        hours % 24 == 0 ? "\(hours / 24)d" : "\(hours)h"
    }
}

/// The commands the pane sends -- everything after ytdl.ps1.
enum SubscriptionArgs {
    static let list = ["--subscriptions", "--json"]

    /* The Downloads form's options, as a subscription. `opts` is the form
     * exactly as a run would send it, connection settings included -- a
     * scheduled check of a members-only playlist needs its cookies as much as
     * the first download did. Built from toArgs rather than a second list of
     * fields, so an option added to the form is subscribable the day it is
     * added. */
    static func subscribe(_ opts: RunOptions, everyHours: Int, name: String?) -> [String] {
        var o = opts
        o.subscriptionID = ""
        o.refresh = false // the pipeline refuses to store it
        var v = o.toArgs() + ["--subscribe"]
        if everyHours > 0 { v += ["--every", SubscriptionText.everyArgument(everyHours)] }
        if let n = name?.trimmingCharacters(in: .whitespacesAndNewlines), !n.isEmpty {
            v += ["--name", n]
        }
        return v
    }

    /// `everyHours` 0 leaves it alone; `pause` nil leaves it, false resumes,
    /// true pauses.
    static func edit(id: String, everyHours: Int = 0, pause: Bool? = nil) -> [String] {
        var v = ["--edit-subscription", id]
        if everyHours > 0 { v += ["--every", SubscriptionText.everyArgument(everyHours)] }
        if let p = pause { v.append(p ? "--pause" : "--resume") }
        return v
    }

    static func unsubscribe(_ id: String) -> [String] { ["--unsubscribe", id] }

    static func schedule(install: Bool) -> [String] {
        ["--schedule", install ? "install" : "remove"]
    }
}

/// What a short `ytdl` command printed.
struct YtdlCommandResult: Equatable {
    var exitCode: Int32 = 0
    var stdout = ""
    var stderr = ""

    /* The sentence to show when it failed: the "Error: ..." line if the
     * pipeline printed one -- ytdl.ps1 prints notes before it gets to the
     * refusal, and the note is not why the command failed -- else the first
     * thing it printed, else the exit code. */
    var message: String {
        for text in [stderr, stdout] {
            for raw in text.split(separator: "\n") {
                let line = raw.trimmingCharacters(in: .whitespacesAndNewlines)
                if line.hasPrefix("Error:") || line.hasPrefix("[subscriptions]") { return line }
            }
        }
        for text in [stderr, stdout] {
            for raw in text.split(separator: "\n") {
                let line = raw.trimmingCharacters(in: .whitespacesAndNewlines)
                if !line.isEmpty { return line }
            }
        }
        return "ytdl exited with code \(exitCode)"
    }

    /* Does this failure mean the installed pipeline predates subscriptions?
     * ytdl.ps1 without them answers "--subscriptions" in the URL position and
     * then refuses --json as an unknown option; one with ytdl.ps1 but no
     * subscriptions.ps1 says so in as many words. */
    var meansTooOld: Bool {
        exitCode != 0
            && (stderr.contains("predates subscriptions")
                || stderr.contains("Unknown option: --json")
                || stderr.contains("Unknown option: --subscribe"))
    }
}

enum YtdlCommand {
    enum RunError: LocalizedError {
        case pipelineMissing(String)

        var errorDescription: String? {
            switch self {
            case .pipelineMissing(let why): return why
            }
        }
    }

    private final class DataBox: @unchecked Sendable {
        private let lock = NSLock()
        private var value = Data()
        func set(_ d: Data) { lock.lock(); value = d; lock.unlock() }
        func get() -> Data { lock.lock(); defer { lock.unlock() }; return value }
    }

    /// `pwsh -NoProfile -File <installed ytdl.ps1> args`, stdout and stderr
    /// captured apart. Fails only when the pipeline could not be started at
    /// all; a refusal comes back as a result with a non-zero exit code.
    static func runSync(_ args: [String]) -> Result<YtdlCommandResult, Error> {
        guard let pwsh = Paths.findPwsh() else {
            return .failure(RunError.pipelineMissing(
                "pwsh (PowerShell 7) was not found, so the pipeline cannot be run."))
        }
        let script = Paths.join(Paths.scriptsDir(), "ytdl.ps1")
        guard Paths.isRegularFile(script) else {
            return .failure(RunError.pipelineMissing(
                "\(script) does not exist. Install the pipeline, or set "
                    + "YTDLP_INSTALL_ROOT to where it lives."))
        }

        let process = Process()
        process.executableURL = URL(fileURLWithPath: pwsh)
        process.arguments = ["-NoProfile", "-File", script] + args
        var env = ProcessInfo.processInfo.environment
        /* launchd's PATH, which a .app inherits, has neither Homebrew's bin
         * directories nor ~/.local/bin; the Runner adds them for the same
         * reason. `--schedule install` records this PATH in the agent, so it
         * matters more here than anywhere: it is the PATH every scheduled
         * check will run with. */
        env["PATH"] = Paths.childPath()
        process.environment = env
        let outPipe = Pipe(), errPipe = Pipe()
        process.standardOutput = outPipe
        process.standardError = errPipe
        process.standardInput = FileHandle.nullDevice
        do { try process.run() } catch { return .failure(error) }

        /* Both pipes drained on their own queues: a reader waiting on stdout
         * while stderr fills would deadlock. */
        let outBox = DataBox(), errBox = DataBox()
        let group = DispatchGroup()
        DispatchQueue.global(qos: .userInitiated).async(group: group) {
            outBox.set(outPipe.fileHandleForReading.readDataToEndOfFile())
        }
        DispatchQueue.global(qos: .userInitiated).async(group: group) {
            errBox.set(errPipe.fileHandleForReading.readDataToEndOfFile())
        }
        process.waitUntilExit()
        group.wait()
        return .success(YtdlCommandResult(
            exitCode: process.terminationStatus,
            stdout: String(data: outBox.get(), encoding: .utf8) ?? "",
            stderr: String(data: errBox.get(), encoding: .utf8) ?? ""
        ))
    }

    /// Off the main thread. `completion` arrives on a BACKGROUND queue; the
    /// caller hops to the main actor with `Task { @MainActor in ... }`, the
    /// pattern UrlProbeRunner.run documents.
    static func run(
        _ args: [String],
        completion: @escaping @Sendable (Result<YtdlCommandResult, Error>) -> Void
    ) {
        DispatchQueue.global(qos: .userInitiated).async {
            completion(runSync(args))
        }
    }
}
