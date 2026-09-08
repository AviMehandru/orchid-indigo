/* Spawning a child that can actually be cancelled.
 *
 * THIS FILE EXISTS BECAUSE OF ONE REQUIREMENT: cancel must kill the process
 * GROUP, not the child.
 *
 * ytdl.ps1 starts run_ytdlp.ps1 as a CHILD pwsh process, which starts yt-dlp,
 * which starts postprocess.ps1 and ffmpeg. Killing only the process this app
 * spawned leaves a download running with nothing reading its output -- a
 * cancelled run that goes on writing to the archive for another ten minutes.
 *
 * The GTK app gets this from setpgid(0,0) in GLib's child-setup callback plus
 * kill(-pid). Foundation.Process has NO child-setup hook and no way to ask for
 * a new process group, so `Process` cannot express the requirement at all.
 * The two ways out are a `/bin/sh -c 'set -m; exec …'` wrapper and
 * posix_spawn with POSIX_SPAWN_SETPGROUP; this is the second, because the
 * first does not actually work: `exec` replaces the shell without forking, so
 * no job-control setpgid ever happens and the child stays in THIS app's
 * process group -- where kill(-pid) would take the app down with it.
 *
 * So: posix_spawn, with the group set by the spawn attributes. About sixty
 * lines, and the only C-level code in the app.
 *
 * The conformance suite spawns a fake script that itself spawns a
 * GRANDCHILD, because a test against a single-process fake passes just as
 * happily when only the child is killed.
 */

import Darwin
import Foundation

struct SpawnedChild {
    let pid: pid_t
    /// Read ends. The caller owns them and must close them.
    let stdoutFD: Int32
    let stderrFD: Int32
}

enum SpawnError: LocalizedError {
    case pipeFailed(Int32)
    case spawnFailed(String, Int32)

    var errorDescription: String? {
        switch self {
        case .pipeFailed(let code):
            return "Could not create a pipe for the download's output (errno \(code))."
        case .spawnFailed(let path, let code):
            return "Could not start \(path): \(String(cString: strerror(code)))"
        }
    }
}

enum Spawn {
    /// Spawn `executable` in a NEW PROCESS GROUP with its stdout and stderr on
    /// pipes and its stdin on /dev/null.
    ///
    /// stdin is /dev/null and not inherited deliberately: a GUI has no terminal
    /// to answer with, and an inherited stdin leaves yt-dlp able to block
    /// forever waiting on one.
    static func run(
        executable: String,
        arguments: [String],
        environment: [String: String]
    ) throws -> SpawnedChild {
        var outPipe: [Int32] = [-1, -1]
        var errPipe: [Int32] = [-1, -1]
        guard pipe(&outPipe) == 0 else { throw SpawnError.pipeFailed(errno) }
        guard pipe(&errPipe) == 0 else {
            /* errno is read BEFORE the closes: close(2) sets it too, so
             * capturing it afterwards reports whichever error the cleanup had
             * rather than the one that actually happened. */
            let failure = errno
            close(outPipe[0]); close(outPipe[1])
            throw SpawnError.pipeFailed(failure)
        }

        var actions: posix_spawn_file_actions_t?
        posix_spawn_file_actions_init(&actions)
        defer { posix_spawn_file_actions_destroy(&actions) }

        // Applied in order, so each dup2 comes before the close of its source.
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0)
        posix_spawn_file_actions_adddup2(&actions, outPipe[1], 1)
        posix_spawn_file_actions_adddup2(&actions, errPipe[1], 2)
        posix_spawn_file_actions_addclose(&actions, outPipe[0])
        posix_spawn_file_actions_addclose(&actions, errPipe[0])
        posix_spawn_file_actions_addclose(&actions, outPipe[1])
        posix_spawn_file_actions_addclose(&actions, errPipe[1])

        var attrs: posix_spawnattr_t?
        posix_spawnattr_init(&attrs)
        defer { posix_spawnattr_destroy(&attrs) }
        /* pgroup 0 means "a new group whose id is the child's pid", which is
         * what makes kill(-pid) reach every descendant and nothing else. */
        posix_spawnattr_setpgroup(&attrs, 0)
        posix_spawnattr_setflags(&attrs, Int16(POSIX_SPAWN_SETPGROUP))

        var argv: [UnsafeMutablePointer<CChar>?] =
            ([executable] + arguments).map { strdup($0) }
        argv.append(nil)
        var envp: [UnsafeMutablePointer<CChar>?] =
            environment.map { strdup("\($0.key)=\($0.value)") }
        envp.append(nil)
        defer {
            for p in argv where p != nil { free(p) }
            for p in envp where p != nil { free(p) }
        }

        var pid: pid_t = 0
        let rc = posix_spawn(&pid, executable, &actions, &attrs, &argv, &envp)

        // The parent holds only the read ends; leaving a write end open here
        // means the pumps never see EOF and the run never appears to finish.
        close(outPipe[1])
        close(errPipe[1])

        guard rc == 0 else {
            close(outPipe[0])
            close(errPipe[0])
            throw SpawnError.spawnFailed(executable, rc)
        }

        return SpawnedChild(pid: pid, stdoutFD: outPipe[0], stderrFD: errPipe[0])
    }

    /// Kill the whole tree: SIGTERM to the group, a moment to let ffmpeg close
    /// its output file, then SIGKILL to whatever is left.
    /// `grace` is how long ffmpeg gets to close its output file before the
    /// SIGKILL. Shorter at quit than at Cancel: macOS gives a terminating app a
    /// few seconds in total, and spending 1.5 of them here is visible.
    static func killTree(pid: pid_t, grace: TimeInterval = 1.5) {
        guard pid > 0 else { return }
        kill(-pid, SIGTERM)
        Thread.sleep(forTimeInterval: grace)
        kill(-pid, SIGKILL)
    }

    /// Block until the child exits and return its exit code, or -1 when it was
    /// killed by a signal.
    ///
    /// WIFEXITED and WEXITSTATUS are C macros and are not imported into Swift,
    /// so the status word is decoded here: the low seven bits are the signal
    /// that killed it (zero for a normal exit) and the next eight are the exit
    /// code.
    static func waitExitCode(pid: pid_t) -> Int32 {
        var status: Int32 = 0
        while waitpid(pid, &status, 0) < 0 {
            if errno != EINTR { return -1 }
        }
        if status & 0x7f == 0 {
            return (status >> 8) & 0xff
        }
        return -1
    }

    /// Read a file descriptor to EOF, handing every complete line to `onLine`
    /// with a flag saying whether it was a carriage-return redraw.
    ///
    /// SPLITS ON BOTH \n AND \r, and this is not defensiveness: yt-dlp redraws
    /// its progress line with a carriage return and yt-dlp.conf sets no
    /// --newline, so a line-oriented reader would either block until the
    /// download finished or deliver one enormous line. Splitting on \r as well
    /// is what makes the progress bar move.
    ///
    /// Blocks until EOF. Runs on its own thread.
    static func pumpLines(fd: Int32, onLine: (String, Bool) -> Void) {
        var buffer = [UInt8]()
        var chunk = [UInt8](repeating: 0, count: 8192)
        let capacity = chunk.count

        while true {
            let n = read(fd, &chunk, capacity)
            if n < 0 {
                if errno == EINTR { continue }
                break
            }
            if n == 0 { break }

            for i in 0..<n {
                let byte = chunk[i]
                if byte != UInt8(ascii: "\n") && byte != UInt8(ascii: "\r") {
                    buffer.append(byte)
                    /* A single line this long is not a line; drop it rather
                     * than let a runaway stream grow the buffer without
                     * bound. */
                    if buffer.count > 64 * 1024 { buffer.removeAll(keepingCapacity: true) }
                    continue
                }

                let transient = byte == UInt8(ascii: "\r")
                let text = String(decoding: buffer, as: UTF8.self)
                buffer.removeAll(keepingCapacity: true)

                let stripped = OutputParser.stripANSI(text)
                if stripped.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { continue }
                onLine(stripped, transient)
            }
        }

        /* The last write of a child that exits without a trailing newline is
         * still in the buffer here, and it is the line that matters: a failure
         * message is what fills in a history row's lastLine. Dropping it at EOF
         * loses exactly the output somebody would go looking for. */
        if !buffer.isEmpty {
            let stripped = OutputParser.stripANSI(String(decoding: buffer, as: UTF8.self))
            if !stripped.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                onLine(stripped, false)
            }
        }
        close(fd)
    }
}
