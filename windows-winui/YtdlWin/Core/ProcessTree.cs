/* Spawning a child that can actually be cancelled, and reading its output.
 *
 * THIS FILE EXISTS BECAUSE OF ONE REQUIREMENT: cancel must kill the whole
 * process TREE, not the child.
 *
 * ytdl.ps1 starts run_ytdlp.ps1 as a CHILD pwsh process, which starts yt-dlp,
 * which starts postprocess.ps1 and ffmpeg. Killing only the process this app
 * spawned leaves a download running with nothing reading its output -- a
 * cancelled run that goes on writing to the archive for another ten minutes.
 *
 * ============================ WHY A JOB OBJECT ==============================
 *
 * The GTK app gets this from setpgid(0,0) plus kill(-pid); the macOS app from
 * posix_spawn with POSIX_SPAWN_SETPGROUP. Windows has no process groups in that
 * sense, and the three candidate answers are not equal:
 *
 *   taskkill /T /F      What claude/native-frontend-architecture.md assumed
 *                       Windows would use, before anyone looked at it. It walks
 *                       the parent/child chain from a point-in-time snapshot,
 *                       so it misses a descendant spawned while the walk is
 *                       running -- and worse, it misses any descendant whose
 *                       parent already exited, because the chain it walks is
 *                       broken at that point. yt-dlp exiting before ffmpeg
 *                       finishes is not a rare case; it is the normal shape of
 *                       the end of a download. It is also a subprocess spawn,
 *                       so cancel would depend on a tool being on PATH.
 *
 *   Ctrl-Break to a     Would give a graceful shutdown, which is the one thing
 *   process group       a job object cannot. It is not available: it requires
 *                       the sender to share a console with the target, and this
 *                       is a GUI process with no console at all -- and the child
 *                       is deliberately spawned with none, since a console
 *                       window flashing up on every run is not acceptable.
 *                       Allocating a hidden console to route a signal through
 *                       would mean AttachConsole, disabling our own handler, and
 *                       a race with anything else on that console. Not worth it.
 *
 *   A job object        What this uses. Every descendant is in the job by
 *                       construction -- a child of a process in a job is in the
 *                       same job, with no snapshot and no chain to walk -- and
 *                       TerminateJobObject kills all of them in one call with
 *                       no dependency on anything being installed.
 *
 * WHAT THE JOB BUYS THAT THE OTHER TWO PLATFORMS DO NOT HAVE.
 * JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE means the tree dies when the last handle
 * to the job closes -- which includes the case where this app is killed outright
 * rather than quitting. On Linux and macOS, an app killed with SIGKILL orphans
 * its download and it runs to completion with nothing reading it. Here it does
 * not. That is a genuine platform advantage and it is free.
 *
 * WHAT IT COSTS, said plainly: there is no SIGTERM. The macOS and GTK apps send
 * SIGTERM, wait, then SIGKILL, which gives ffmpeg a moment to close its output
 * file. TerminateJobObject is SIGKILL to everything, immediately, with no
 * graceful option. So a cancelled run on Windows can leave a partially written
 * file behind where the same cancel on Linux would not. That is tolerable
 * rather than good: the pipeline writes into _incomplete/ and sweeps it, and
 * the layout contract already tells every consumer to tolerate a folder with a
 * truncated media file. It is not something to paper over -- if a graceful
 * cancel is ever wanted here, it needs a real answer, not a shorter timeout.
 *
 * ======================== THE ASSIGNMENT RACE, HONESTLY ======================
 *
 * The child is assigned to the job immediately AFTER Process.Start, not before
 * it runs. The airtight version is CreateProcess with CREATE_SUSPENDED, assign,
 * then ResumeThread -- and that means hand-rolling CreatePipe, handle
 * inheritance and STARTUPINFO instead of letting .NET build the pipes, which is
 * a couple of hundred lines of P/Invoke whose failure mode is leaked handles
 * and a hang.
 *
 * This code has never been compiled, let alone run. Trading correct,
 * well-tested pipe plumbing for a hand-rolled copy in order to close a window
 * that is measured in microseconds -- and that the child cannot use, because
 * pwsh spends its first hundred-odd milliseconds starting a runtime before it
 * can execute a line of script, never mind spawn anything -- is the wrong risk
 * to take here. The window is real and it is written down rather than hidden.
 * If this app ever grows a test that can catch it, CREATE_SUSPENDED is the fix.
 */

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace YtdlWin.Core;

public sealed class SpawnException : Exception
{
    public SpawnException(string message, Exception? inner = null) : base(message, inner) { }
}

/// <summary>A running child plus the job object holding its whole tree.</summary>
public sealed class SpawnedChild : IDisposable
{
    public required Process Process { get; init; }
    public required JobObject Job { get; init; }

    /// The pid, captured at construction. Process.Id throws once the Process
    /// object has been disposed, and this is read for logging after that.
    public required int Id { get; init; }

    /// <summary>True when the child could not be put in the job, so cancel can
    /// only reach the top process. Not fatal -- the download still runs -- but
    /// the UI says so rather than promising a cancel it cannot deliver.</summary>
    public bool JobAssignmentFailed { get; init; }

    /// Block until the child exits and return its exit code.
    /// A killed process reports a code that means nothing in particular, which
    /// is why the runner decides "cancelled" from its own flag rather than from
    /// this number -- exactly as the macOS app does with a signal death.
    public int WaitExitCode()
    {
        try
        {
            Process.WaitForExit();
            return Process.ExitCode;
        }
        catch (Exception) { return -1; }
    }

    public void Dispose()
    {
        /* The job is disposed LAST and deliberately: closing the last handle to
         * a KILL_ON_JOB_CLOSE job terminates whatever is still in it, so this
         * is also the backstop that stops a forgotten child outliving the app.
         * Disposing the Process first only releases this process's handle to
         * it, which does not kill anything. */
        try { Process.Dispose(); } catch (Exception) { /* best effort */ }
        Job.Dispose();
    }
}

/// <summary>A Win32 job object. Every descendant of the assigned process is in
/// it, and closing it kills them all.</summary>
public sealed class JobObject : IDisposable
{
    private IntPtr _handle;
    private bool _disposed;

    private JobObject(IntPtr handle) => _handle = handle;

    public static JobObject Create()
    {
        var handle = Native.CreateJobObjectW(IntPtr.Zero, null);
        if (handle == IntPtr.Zero)
        {
            throw new SpawnException(
                "Could not create the job object that cancel uses to stop a download's whole " +
                $"process tree (Win32 error {Marshal.GetLastWin32Error()}).");
        }

        var job = new JobObject(handle);
        try
        {
            job.ConfigureKillOnClose();
            return job;
        }
        catch
        {
            job.Dispose();
            throw;
        }
    }

    private void ConfigureKillOnClose()
    {
        var info = new Native.JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        info.BasicLimitInformation.LimitFlags = Native.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

        var size = Marshal.SizeOf<Native.JOBOBJECT_EXTENDED_LIMIT_INFORMATION>();
        var buffer = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(info, buffer, false);
            var ok = Native.SetInformationJobObject(
                _handle, Native.JobObjectExtendedLimitInformation, buffer, (uint)size);
            if (!ok)
            {
                throw new SpawnException(
                    "Could not configure the job object (Win32 error " +
                    $"{Marshal.GetLastWin32Error()}).");
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    /* Assign a process. Windows 8 and later allow nested jobs, so this succeeds
     * even when the app is itself running inside a job -- which it is whenever
     * it was started from a debugger, from some terminal hosts, or under a test
     * runner. On Windows 7 that was an outright failure, and code that assumed
     * it could not happen is exactly why "works on my machine, fails under the
     * IDE" was a known shape of this bug. The minimum target here is Windows 10,
     * so nesting is always available. */
    public bool Assign(Process process)
    {
        try
        {
            return Native.AssignProcessToJobObject(_handle, process.Handle);
        }
        catch (Exception) { return false; }
    }

    /* The pids currently in the job.
     *
     * This exists FOR THE CONFORMANCE SUITE, and it is worth saying so. The
     * requirement "cancel kills the whole tree" needs a test, and the POSIX
     * ports assert it by watching a grandchild's pid with kill(pid, 0). Windows
     * has no equivalent question to ask about an arbitrary pid -- a pid is
     * reused, and a handle to a dead process still answers. Asking the JOB what
     * it contains is the direct question: the tree is gone exactly when the job
     * is empty, whatever the descendants were or how they were spawned.
     *
     * Returns an empty list on failure, which for a job whose processes have all
     * exited is also the correct answer -- so a caller cannot tell "the query
     * failed" from "nothing left". That is acceptable here because the only
     * caller is a test asserting emptiness AFTER first asserting non-emptiness
     * on the same job, which a broken query could not have produced. */
    public IReadOnlyList<int> ProcessIds()
    {
        if (_disposed || _handle == IntPtr.Zero) return System.Array.Empty<int>();

        /* The struct is variable-length: a fixed header followed by an inline
         * array of pids. There is no way to ask how many there will be, so this
         * allocates room for a generous number and retries once at four times
         * the size if the call reports the buffer was too small. A download tree
         * is single figures; 256 is already far past anything real. */
        for (var capacity = 256; capacity <= 4096; capacity *= 4)
        {
            /* JOBOBJECT_BASIC_PROCESS_ID_LIST is:
             *
             *     DWORD     NumberOfAssignedProcesses;   // 4 bytes
             *     DWORD     NumberOfProcessIdsInList;    // 4 bytes
             *     ULONG_PTR ProcessIdList[1];            // 8 on x64, 4 on x86
             *
             * so the header is EIGHT bytes on both architectures -- the two
             * DWORDs already land the array on its natural alignment, so no
             * padding is inserted. Reading them as pointer-sized values instead
             * (IntPtr.Size * 2) is wrong twice over: it merges the two counts
             * into one number and then starts the array 8 bytes late, which
             * hands back pointer-shaped garbage rather than pids. */
            const int headerSize = 8;
            var size = headerSize + IntPtr.Size * capacity;
            var buffer = Marshal.AllocHGlobal(size);
            try
            {
                if (!Native.QueryInformationJobObject(
                        _handle, Native.JobObjectBasicProcessIdList, buffer, (uint)size, out _))
                {
                    // ERROR_MORE_DATA means the list did not fit; anything else
                    // means the job is gone or inaccessible.
                    if (Marshal.GetLastWin32Error() == Native.ErrorMoreData) continue;
                    return System.Array.Empty<int>();
                }

                // Both counts are DWORDs, so Int32 -- not IntPtr.
                var returned = Marshal.ReadInt32(buffer, 4);
                var count = System.Math.Max(0, System.Math.Min(returned, capacity));

                var pids = new List<int>(count);
                for (var i = 0; i < count; i++)
                {
                    var pid = Marshal.ReadIntPtr(buffer, headerSize + i * IntPtr.Size).ToInt64();
                    pids.Add((int)pid);
                }
                return pids;
            }
            catch (Exception) { return System.Array.Empty<int>(); }
            finally { Marshal.FreeHGlobal(buffer); }
        }
        return System.Array.Empty<int>();
    }

    /// Kill everything in the job. Safe to call more than once, and safe to
    /// call when the tree has already exited.
    public void TerminateAll()
    {
        if (_disposed || _handle == IntPtr.Zero) return;
        try { Native.TerminateJobObject(_handle, 1); }
        catch (Exception) { /* the tree is already gone */ }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_handle != IntPtr.Zero)
        {
            Native.CloseHandle(_handle);
            _handle = IntPtr.Zero;
        }
    }

    /* Nested in JobObject, and it has to be. Every call site above is in this
     * class and ProcessTree never touches it, so living down there as a private
     * member of ProcessTree put it out of scope for its only consumer -- which
     * is exactly what it did on the first compile, twelve times over. */
    private static class Native
    {
        public const int JobObjectBasicProcessIdList = 3;
        public const int JobObjectExtendedLimitInformation = 9;
        public const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;
        public const int ErrorMoreData = 234;

        [StructLayout(LayoutKind.Sequential)]
        public struct IO_COUNTERS
        {
            public ulong ReadOperationCount;
            public ulong WriteOperationCount;
            public ulong OtherOperationCount;
            public ulong ReadTransferCount;
            public ulong WriteTransferCount;
            public ulong OtherTransferCount;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct JOBOBJECT_BASIC_LIMIT_INFORMATION
        {
            public long PerProcessUserTimeLimit;
            public long PerJobUserTimeLimit;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSetSize;
            public UIntPtr MaximumWorkingSetSize;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass;
            public uint SchedulingClass;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
        {
            public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
            public IO_COUNTERS IoInfo;
            public UIntPtr ProcessMemoryLimit;
            public UIntPtr JobMemoryLimit;
            public UIntPtr PeakProcessMemoryUsed;
            public UIntPtr PeakJobMemoryUsed;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr CreateJobObjectW(IntPtr lpJobAttributes, string? lpName);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool SetInformationJobObject(
            IntPtr hJob, int JobObjectInformationClass,
            IntPtr lpJobObjectInformation, uint cbJobObjectInformationLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool AssignProcessToJobObject(IntPtr hJob, IntPtr hProcess);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool QueryInformationJobObject(
            IntPtr hJob, int JobObjectInformationClass,
            IntPtr lpJobObjectInformation, uint cbJobObjectInformationLength,
            out uint lpReturnLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool TerminateJobObject(IntPtr hJob, uint uExitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool CloseHandle(IntPtr hObject);
    }
}

public static class ProcessTree
{
    /// <summary>
    /// Start <paramref name="executable"/> inside a fresh job object, with its
    /// stdout and stderr on pipes and no console window.
    /// </summary>
    public static SpawnedChild Run(string executable, IReadOnlyList<string> arguments,
                                   IReadOnlyDictionary<string, string> environment)
    {
        if (!Paths.IsRegularFile(executable))
        {
            // Checked before the spawn so the failure names the file. A
            // Win32Exception out of Process.Start says "The system cannot find
            // the file specified" and does not say which one.
            throw new SpawnException($"Could not start {executable}: it does not exist.");
        }

        var job = JobObject.Create();
        var process = new Process();
        try
        {
            var psi = process.StartInfo;
            psi.FileName = executable;
            psi.UseShellExecute = false;
            psi.RedirectStandardOutput = true;
            psi.RedirectStandardError = true;
            /* stdin is redirected and then closed rather than inherited. A GUI
             * has no terminal to answer with, and an inherited stdin leaves
             * yt-dlp able to block forever waiting on one. On Windows there is
             * a second reason: without redirection the child would inherit this
             * process's standard handles, which for a GUI app are invalid, and
             * pwsh reading from an invalid handle behaves differently across
             * versions. */
            psi.RedirectStandardInput = true;
            psi.CreateNoWindow = true;

            /* ArgumentList, not Arguments. .NET quotes each element for the
             * Windows command-line parser itself, which is the whole game here:
             * a video title in a --ytdlp-arg, or a destination path with a
             * space, hand-joined into one string is the classic Windows
             * argument bug, and the backslash-before-quote rule that CommandLine
             * ToArgv uses is not something to reimplement per call site. */
            foreach (var arg in arguments) psi.ArgumentList.Add(arg);

            /* The child's environment is REPLACED, not merged, so the caller
             * decides exactly what the pipeline sees. SystemRoot is then put
             * back if the caller left it out, and that is not defensiveness:
             * a Windows process started without SystemRoot fails to initialise
             * its networking stack and several other subsystems, and the
             * failure surfaces as an unrelated error from deep inside whatever
             * it was doing. The equivalent trap on Unix does not exist, which
             * is why neither of the other two ports has this line. */
            psi.Environment.Clear();
            foreach (var (k, v) in environment) psi.Environment[k] = v;

            foreach (var essential in new[] { "SystemRoot", "SystemDrive", "windir" })
            {
                if (psi.Environment.ContainsKey(essential)) continue;
                var value = Environment.GetEnvironmentVariable(essential);
                if (!string.IsNullOrEmpty(value)) psi.Environment[essential] = value;
            }

            /* The working directory is the install root rather than wherever
             * this app happens to have been started. The pipeline resolves some
             * paths relative to the process's working directory, and a GUI's
             * working directory is whatever Explorer handed it -- often
             * C:\Windows\system32. */
            var install = Paths.InstallRoot();
            psi.WorkingDirectory = Paths.IsDirectory(install)
                ? install
                : Path.GetDirectoryName(executable) ?? "";

            process.Start();
        }
        catch (Exception ex)
        {
            process.Dispose();
            job.Dispose();
            throw new SpawnException($"Could not start {executable}: {ex.Message}", ex);
        }

        if (!job.Assign(process))
        {
            /* Not fatal, and deliberately not fatal. The download would work
             * fine; only cancel would be reduced to killing the top process and
             * leaving its descendants. Refusing to start at all would be worse
             * than starting with a cancel that is less thorough -- but the
             * caller is told, so it can say so rather than silently promising a
             * cancel it cannot deliver. */
            return new SpawnedChild
            {
                Process = process,
                Job = job,
                Id = process.Id,
                JobAssignmentFailed = true,
            };
        }

        return new SpawnedChild { Process = process, Job = job, Id = process.Id };
    }

    /* Kill the whole tree.
     *
     * There is no grace period and no two-phase stop, for the reason in the
     * header: Windows offers no way to ask a console-less process tree to stop
     * politely. The `grace` parameter the other two ports take would be a lie
     * here, so it is not offered. */
    public static void KillTree(SpawnedChild child)
    {
        child.Job.TerminateAll();

        /* The fallback for the case where the job assignment failed. Kills the
         * one process this app started, which is better than nothing and is
         * exactly as much as taskkill would reliably manage anyway. */
        if (child.JobAssignmentFailed)
        {
            try
            {
                if (!child.Process.HasExited) child.Process.Kill(entireProcessTree: true);
            }
            catch (Exception) { /* already gone, or access denied */ }
        }
    }

    /* Read a stream to EOF, handing every complete line to onLine with a flag
     * saying whether it was a carriage-return redraw.
     *
     * SPLITS ON BOTH \n AND \r, and this is not defensiveness: yt-dlp redraws
     * its progress line with a carriage return and yt-dlp.conf sets no
     * --newline, so a line-oriented reader would either block until the
     * download finished or deliver one enormous line. Splitting on \r as well
     * is what makes the progress bar move.
     *
     * WHY NOT OutputDataReceived / BeginOutputReadLine, which is the obvious
     * .NET answer: it splits on \r as well as \n, but it does not tell the
     * caller WHICH terminator ended a line -- and that single bit is the entire
     * difference between a progress bar that redraws in place and a log with
     * four thousand near-identical rows in it. The distinction cannot be
     * recovered afterwards, so the raw stream is read here instead.
     *
     * \r\n IS ONE TERMINATOR, not two, and on this platform that matters far
     * more than it did in the ports this is based on. Everything the pipeline
     * emits through PowerShell's own output ends \r\n; treating the \r as a
     * redraw and the \n as an end-of-line would mark every ordinary log line
     * transient, and the log would show one line at a time overwriting itself.
     * So a \r is only a redraw when the byte after it is not \n -- which needs
     * one byte of lookahead across read boundaries, hence pendingCr.
     *
     * Blocks until EOF. Runs on its own thread. */
    public static void PumpLines(Stream stream, Action<string, bool> onLine)
    {
        var buffer = new List<byte>(256);
        var chunk = new byte[8192];
        var pendingCr = false;
        var atStart = true;
        var overflowed = false;

        void Emit(bool transient)
        {
            /* A line that ran past the cap is dropped ENTIRELY, not down to
             * whatever happened to arrive after the last clear. Clearing the
             * buffer on overflow and carrying on -- which is what this did, and
             * what the GTK app at pipeline.c:982 and the macOS app's
             * Spawn.pumpLines still do -- leaves a tail fragment that then gets
             * emitted as though it were a real line: it starts mid-token, it
             * looks like genuine output, and its length is an artefact of where
             * the 64KB boundary happened to fall. Nothing is better than a
             * plausible lie. */
            if (overflowed)
            {
                overflowed = false;
                buffer.Clear();
                atStart = false;
                return;
            }

            var bytes = buffer.ToArray();
            buffer.Clear();

            /* A UTF-8 BOM at the very start of the stream. PowerShell can emit
             * one depending on how its output encoding is configured, and it
             * would otherwise render as a stray glyph on the first line of
             * every run. Only checked at the start, so a legitimate U+FEFF
             * later in the output is left alone. */
            if (atStart && bytes.Length >= 3 &&
                bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
            {
                bytes = bytes[3..];
            }
            atStart = false;

            var text = OutputParser.StripAnsi(Paths.DecodeText(bytes));
            if (text.Trim().Length == 0) return;
            onLine(text, transient);
        }

        try
        {
            while (true)
            {
                int n;
                try { n = stream.Read(chunk, 0, chunk.Length); }
                catch (IOException) { break; }
                catch (ObjectDisposedException) { break; }
                if (n <= 0) break;

                for (var i = 0; i < n; i++)
                {
                    var b = chunk[i];

                    if (pendingCr)
                    {
                        pendingCr = false;
                        // The \n half of a \r\n: the line was already emitted as
                        // permanent, so this byte is consumed and nothing else
                        // happens.
                        if (b == (byte)'\n') continue;
                    }

                    if (b == (byte)'\r')
                    {
                        /* Look one byte ahead within this chunk. At a chunk
                         * boundary the answer is not here yet, so the decision
                         * is deferred to the next read via pendingCr -- and the
                         * line is emitted as a REDRAW, because that is the
                         * reading that degrades gracefully: a permanent line
                         * shown as transient is briefly replaced, while a
                         * transient line shown as permanent is a row that never
                         * goes away. */
                        var nextIsNewline = i + 1 < n && chunk[i + 1] == (byte)'\n';
                        if (nextIsNewline)
                        {
                            Emit(transient: false);
                            i++;  // consume the \n
                        }
                        else if (i + 1 < n)
                        {
                            Emit(transient: true);
                        }
                        else
                        {
                            Emit(transient: true);
                            pendingCr = true;
                        }
                        continue;
                    }

                    if (b == (byte)'\n')
                    {
                        Emit(transient: false);
                        continue;
                    }

                    /* Once a line has overflowed, every remaining byte of it
                     * is discarded on arrival -- there is nothing to accumulate
                     * into, and the flag is what makes Emit drop it rather than
                     * ship a fragment. */
                    if (overflowed) continue;

                    buffer.Add(b);
                    /* A single line this long is not a line; drop it rather than
                     * let a runaway stream grow the buffer without bound. */
                    if (buffer.Count > 64 * 1024)
                    {
                        overflowed = true;
                        buffer.Clear();
                    }
                }
            }
        }
        catch (Exception) { /* the pipe went away; the run is over */ }

        /* The last write of a child that exits without a trailing newline is
         * still in the buffer here, and it is the line that matters: a failure
         * message is what fills in a history row's LastLine. Dropping it at EOF
         * loses exactly the output somebody would go looking for. */
        if (buffer.Count > 0 || overflowed) Emit(transient: false);
    }
}
