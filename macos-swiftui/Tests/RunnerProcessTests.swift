/* The spawn/pump/cancel path, exercised against a fake pipeline.
 *
 * The requirement it exists for, stated once so it cannot be lost: test the
 * cancel path with a fake that spawns a GRANDCHILD, or the test passes against
 * a single-process fake while the real thing leaks.
 *
 * ytdl.ps1 starts run_ytdlp.ps1 as a child pwsh, which starts yt-dlp, which
 * starts postprocess.ps1 and ffmpeg. Cancel has to reach all of them. A fake
 * that is one process cannot tell "killed the child" from "killed the group",
 * and those two are the difference between a cancelled download and a download
 * that goes on writing to the archive with nothing reading its output. So the
 * fake here spawns a `sleep` of its own and the assertion is about THAT
 * process.
 *
 * No YouTube, no pwsh, no network: /bin/sh emitting the SHAPE of real output.
 */

import Darwin
import XCTest
@testable import YtdlMac

final class RunnerProcessTests: XCTestCase {
    private var dir = ""

    override func setUpWithError() throws {
        dir = try FixtureSupport.makeTempDir(prefix: "ytdl-macos-spawn")
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(atPath: dir)
    }

    private func makeScript(_ body: String, named name: String) throws -> String {
        let path = Paths.join(dir, name)
        try FixtureSupport.write("#!/bin/sh\n" + body, to: path)
        try FileManager.default.setAttributes(
            [.posixPermissions: 0o755], ofItemAtPath: path
        )
        return path
    }

    private func isAlive(_ pid: pid_t) -> Bool {
        kill(pid, 0) == 0 || errno == EPERM
    }

    /// The one that matters: cancel kills the GROUP, so the grandchild dies too.
    func testCancelKillsTheGrandchildNotJustTheChild() throws {
        let script = try makeScript("""
        sleep 600 &
        echo "grandchild $!"
        sleep 600
        """, named: "with-grandchild.sh")

        let child = try Spawn.run(
            executable: "/bin/sh", arguments: [script],
            environment: ProcessInfo.processInfo.environment
        )

        let found = expectation(description: "grandchild pid announced")
        let box = PIDBox()
        DispatchQueue.global().async {
            Spawn.pumpLines(fd: child.stdoutFD) { line, _ in
                if line.hasPrefix("grandchild "), let pid = pid_t(line.dropFirst(11)) {
                    box.set(pid)
                    found.fulfill()
                }
            }
        }
        // stderr is drained so the pipe cannot fill; nothing is asserted on it.
        DispatchQueue.global().async { Spawn.pumpLines(fd: child.stderrFD) { _, _ in } }

        wait(for: [found], timeout: 10)
        let grandchild = try XCTUnwrap(box.get())
        XCTAssertTrue(isAlive(grandchild), "the fake never started its grandchild")

        Spawn.killTree(pid: child.pid)
        _ = Spawn.waitExitCode(pid: child.pid)

        /* Reparented to launchd once the shell dies, so it is reaped rather than
         * left a zombie -- kill(pid, 0) is the honest question here. A second of
         * slack: SIGKILL is delivered asynchronously. */
        var alive = true
        for _ in 0..<20 {
            if !isAlive(grandchild) { alive = false; break }
            Thread.sleep(forTimeInterval: 0.05)
        }
        XCTAssertFalse(alive, "the grandchild outlived the cancel — the group was not signalled")
    }

    /// The child is in its OWN group, which is what makes kill(-pid) safe to
    /// call at all: signalling this app's own group would take the app with it.
    func testChildGetsItsOwnProcessGroup() throws {
        let script = try makeScript("sleep 5\n", named: "sleeper.sh")
        let child = try Spawn.run(
            executable: "/bin/sh", arguments: [script],
            environment: ProcessInfo.processInfo.environment
        )
        defer {
            Spawn.killTree(pid: child.pid)
            _ = Spawn.waitExitCode(pid: child.pid)
            close(child.stdoutFD)
            close(child.stderrFD)
        }

        XCTAssertEqual(getpgid(child.pid), child.pid,
                       "the child is not the leader of a new process group")
        XCTAssertNotEqual(getpgid(child.pid), getpgid(0),
                          "the child shares this app's process group")
    }

    func testExitCodeIsReported() throws {
        let script = try makeScript("exit 3\n", named: "fails.sh")
        let child = try Spawn.run(
            executable: "/bin/sh", arguments: [script],
            environment: ProcessInfo.processInfo.environment
        )
        DispatchQueue.global().async { Spawn.pumpLines(fd: child.stdoutFD) { _, _ in } }
        DispatchQueue.global().async { Spawn.pumpLines(fd: child.stderrFD) { _, _ in } }

        XCTAssertEqual(Spawn.waitExitCode(pid: child.pid), 3)
    }

    /* Output is split on BOTH \n and \r, and a \r line is marked transient.
     * yt-dlp redraws its progress with a carriage return and yt-dlp.conf sets no
     * --newline: a line-oriented reader either blocks until the download
     * finishes or delivers one enormous line. */
    func testProgressRedrawsArriveAsTransientLines() throws {
        let script = try makeScript("""
        printf '[youtube] dQw4w9WgXcQ: Downloading webpage\\n'
        printf '[download]   1.0%% of 10.00MiB at 1.00MiB/s ETA 00:10\\r'
        printf '[download] 100.0%% of 10.00MiB at 1.00MiB/s ETA 00:00\\r'
        printf -- '-- Session summary: 1 video(s) touched, 0 already archived (skipped), 0 error(s), 0 warning(s) --\\n'
        """, named: "progress.sh")

        let child = try Spawn.run(
            executable: "/bin/sh", arguments: [script],
            environment: ProcessInfo.processInfo.environment
        )
        DispatchQueue.global().async { Spawn.pumpLines(fd: child.stderrFD) { _, _ in } }

        var lines: [(String, Bool)] = []
        Spawn.pumpLines(fd: child.stdoutFD) { line, transient in
            lines.append((line, transient))
        }
        _ = Spawn.waitExitCode(pid: child.pid)

        XCTAssertEqual(lines.count, 4)
        XCTAssertFalse(lines[0].1, "a \\n line is permanent")
        XCTAssertTrue(lines[1].1, "a \\r redraw is transient")
        XCTAssertTrue(lines[2].1)
        XCTAssertFalse(lines[3].1)

        // And the parsers read what the fake emitted.
        var p = RunProgress()
        for (line, _) in lines { _ = OutputParser.parseProgressLine(line, into: &p) }
        XCTAssertEqual(p.videoID, "dQw4w9WgXcQ")
        XCTAssertEqual(p.percent, 100.0, accuracy: 0.001)

        let summary = OutputParser.parseSessionSummary(lines[3].0)
        XCTAssertEqual(summary?.videos, 1)
    }

    /* A missing executable comes back as a thrown error rather than a spawn
     * that reports success and a child that never was. posix_spawn reports this
     * in its return value, not in errno. */
    func testMissingExecutableThrows() {
        XCTAssertThrowsError(try Spawn.run(
            executable: Paths.join(dir, "does-not-exist"),
            arguments: [],
            environment: [:]
        ))
    }
}

/// A pid handed from a pump thread back to the test.
private final class PIDBox {
    private let lock = NSLock()
    private var value: pid_t?

    func set(_ pid: pid_t) { lock.lock(); value = pid; lock.unlock() }
    func get() -> pid_t? { lock.lock(); defer { lock.unlock() }; return value }
}
