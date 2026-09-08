/* The spawn, the pump and the cancel path, exercised against a fake pipeline.
 *
 * The requirement this file exists for, stated once so it cannot be lost:
 * CANCEL MUST KILL THE WHOLE TREE, so the fake spawns a GRANDCHILD. ytdl.ps1
 * starts run_ytdlp.ps1 as a child pwsh, which starts yt-dlp, which starts
 * postprocess.ps1 and ffmpeg. A fake that is one process cannot tell "killed
 * the child" from "killed the tree", and those two are the difference between a
 * cancelled download and a download that goes on writing to the archive with
 * nothing reading its output.
 *
 * HOW THE ASSERTION IS MADE IS THIS PLATFORM'S OWN. The POSIX ports watch the
 * grandchild's pid with kill(pid, 0). Windows has no equivalent question to ask
 * about an arbitrary pid -- pids are reused, and a handle to a dead process
 * still answers -- so the question is put to the JOB instead: the tree is gone
 * exactly when the job is empty, whatever the descendants were or how they were
 * spawned. That is a stronger assertion than the pid watch, not a weaker
 * substitute for it.
 *
 * No YouTube, no pwsh, no network: cmd.exe emitting the SHAPE of real output.
 * The line-splitting tests need no process at all -- they run the pump over a
 * MemoryStream, which is what makes the highest-risk logic in this app testable
 * without spawning anything.
 */

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;
using Xunit;
using YtdlWin.Core;

namespace YtdlWin.Tests;

public sealed class PumpLineTests
{
    private static List<(string Text, bool Transient)> Pump(string raw)
        => Pump(Encoding.UTF8.GetBytes(raw));

    private static List<(string Text, bool Transient)> Pump(byte[] bytes)
    {
        var lines = new List<(string, bool)>();
        using var stream = new MemoryStream(bytes);
        ProcessTree.PumpLines(stream, (text, transient) => lines.Add((text, transient)));
        return lines;
    }

    /* THE ONE THAT MATTERS MOST ON THIS PLATFORM.
     *
     * \r\n is ONE terminator, not two. Everything the pipeline emits through
     * PowerShell's own output ends \r\n; treating the \r as a redraw and the \n
     * as an end-of-line would mark every ordinary log line transient, and the
     * log would show one line at a time overwriting itself. Neither of the
     * other two ports has this problem, and neither has a test for it. */
    [Fact]
    public void CrlfIsOneTerminatorAndTheLineIsPermanent()
    {
        var lines = Pump("first line\r\nsecond line\r\n");

        Assert.Equal(2, lines.Count);
        Assert.Equal(("first line", false), lines[0]);
        Assert.Equal(("second line", false), lines[1]);
    }

    /* A bare \r is yt-dlp redrawing its progress line in place, and yt-dlp.conf
     * sets no --newline. That single bit is the whole difference between a
     * progress bar that moves and a log with four thousand near-identical rows
     * in it -- and it is why this app reads the raw stream rather than using
     * BeginOutputReadLine, which splits on \r but will not say that it did. */
    [Fact]
    public void ABareCarriageReturnIsARedraw()
    {
        var lines = Pump("[download]  1.0%\r[download] 50.0%\r[download] 100.0%\rdone\n");

        Assert.Equal(4, lines.Count);
        Assert.True(lines[0].Transient);
        Assert.True(lines[1].Transient);
        Assert.True(lines[2].Transient);
        Assert.False(lines[3].Transient);
    }

    [Fact]
    public void MixedRedrawsAndPermanentLinesKeepTheirKinds()
    {
        var lines = Pump("[youtube] dQw4w9WgXcQ: Downloading webpage\r\n" +
                         "[download]  45.2%\r" +
                         "[Merger] Merging formats\r\n");

        Assert.Equal(3, lines.Count);
        Assert.False(lines[0].Transient);
        Assert.True(lines[1].Transient);
        Assert.False(lines[2].Transient);
    }

    /* A \r at the very end of a read has no lookahead available, so the
     * decision is deferred -- and the line is emitted as a redraw, because that
     * is the reading that degrades gracefully. A permanent line shown as
     * transient is briefly replaced; a transient line shown as permanent is a
     * row that never goes away.
     *
     * The stream here is deliberately larger than the 8192-byte read buffer, so
     * the \r\n genuinely straddles a chunk boundary rather than the test merely
     * claiming it does. */
    [Fact]
    public void ACrlfStraddlingAChunkBoundaryIsStillOneTerminator()
    {
        // Land the \r exactly on the last byte of the first 8192-byte read.
        var filler = new string('x', 8191);
        var lines = Pump(filler + "\r\nafter the boundary\n");

        Assert.Equal(2, lines.Count);
        Assert.Equal(filler, lines[0].Text);
        Assert.Equal("after the boundary", lines[1].Text);
        Assert.False(lines[1].Transient);
    }

    /* The last write of a child that exits without a trailing newline is still
     * in the buffer at EOF, and it is the line that matters: a failure message
     * is what fills in a history row's LastLine. Dropping it loses exactly the
     * output somebody would go looking for. */
    [Fact]
    public void ALastLineWithNoTerminatorIsStillEmitted()
    {
        var lines = Pump("this run failed and said why");
        Assert.Equal(("this run failed and said why", false), Assert.Single(lines));
    }

    [Fact]
    public void BlankLinesAreDropped()
    {
        var lines = Pump("real\n\n   \n\r\nalso real\n");
        Assert.Equal(2, lines.Count);
    }

    /* A UTF-8 BOM at the very start of the stream. PowerShell can emit one
     * depending on how its output encoding is configured, and it would
     * otherwise render as a stray glyph on the first line of every run. */
    [Fact]
    public void AByteOrderMarkIsStrippedFromTheFirstLineOnly()
    {
        var bytes = new List<byte> { 0xEF, 0xBB, 0xBF };
        bytes.AddRange(Encoding.UTF8.GetBytes("first\nsecond\n"));

        var lines = Pump(bytes.ToArray());
        Assert.Equal("first", lines[0].Text);
        Assert.Equal("second", lines[1].Text);
    }

    /* ANSI is stripped in the pump, so nothing downstream ever sees an escape.
     * PowerShell 7.2+ emits colour on Windows 10+ by default. */
    [Fact]
    public void AnsiIsStrippedBeforeTheLineIsHandedOver()
    {
        var lines = Pump("\u001b[32mgreen text\u001b[0m\n");
        Assert.Equal("green text", Assert.Single(lines).Text);
    }

    /* Not valid UTF-8. A log line carrying a filename in some legacy encoding
     * must not take the pump down or lose the line -- it falls back to Latin-1,
     * which mangles one character rather than dropping a line. */
    [Fact]
    public void InvalidUtf8FallsBackRatherThanThrowing()
    {
        var bytes = new List<byte>();
        bytes.AddRange(Encoding.UTF8.GetBytes("caf"));
        bytes.Add(0xE9);                                    // Latin-1 'é', invalid UTF-8
        bytes.AddRange(Encoding.UTF8.GetBytes(" latte\n"));

        var lines = Pump(bytes.ToArray());
        Assert.Single(lines);
        Assert.Contains("caf", lines[0].Text, StringComparison.Ordinal);
    }

    /* A runaway stream with no terminator must not grow the buffer without
     * bound. The line is dropped rather than kept, which is the right trade:
     * something producing a 64KB "line" is not producing log output. */
    [Fact]
    public void AnAbsurdlyLongLineIsDroppedRatherThanBuffered()
    {
        var lines = Pump(new string('x', 200_000) + "\nsane line\n");
        Assert.Equal("sane line", Assert.Single(lines).Text);
    }
}

/* The tests that actually start processes. Windows-only by construction -- they
 * spawn cmd.exe and query a job object. */
public sealed class ProcessTreeTests : IDisposable
{
    private readonly string _dir;

    public ProcessTreeTests() => _dir = FixtureSupport.MakeTempDir("ytdl-win-spawn");
    public void Dispose() => FixtureSupport.Delete(_dir);

    private static string Cmd =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "cmd.exe");

    private string MakeScript(string body, string name)
    {
        var path = Path.Combine(_dir, name);
        FixtureSupport.Write("@echo off\r\n" + body, path);
        return path;
    }

    private static IReadOnlyDictionary<string, string> Env()
    {
        var env = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (System.Collections.DictionaryEntry e in Environment.GetEnvironmentVariables())
        {
            if (e.Key is string k && e.Value is string v) env[k] = v;
        }
        return env;
    }

    private SpawnedChild Run(string script)
        => ProcessTree.Run(Cmd, new[] { "/c", script }, Env());

    /* THE ONE THAT MATTERS: the grandchild is in the job, and cancel takes it.
     *
     * `start /b` launches a second process from inside the fake; because its
     * parent is already in the job, it is in the job too -- by construction,
     * with no snapshot and no parent chain to walk. That is the property
     * taskkill /T cannot offer, and it is why this app uses a job object. */
    [Fact]
    public void CancelKillsTheGrandchildNotJustTheChild()
    {
        // ping -n 600 127.0.0.1 is a ten-minute sleep that exists on every
        // Windows install; timeout.exe refuses to run with redirected input.
        var script = MakeScript(
            "echo starting\r\n" +
            "start /b \"\" ping -n 600 127.0.0.1 > nul\r\n" +
            "ping -n 600 127.0.0.1 > nul\r\n",
            "with-grandchild.cmd");

        var child = Run(script);
        try
        {
            // Give the fake a moment to reach its `start` line.
            var sawTwo = false;
            for (var i = 0; i < 100 && !sawTwo; i++)
            {
                if (child.Job.ProcessIds().Count >= 2) sawTwo = true;
                else Thread.Sleep(50);
            }

            Assert.True(sawTwo,
                "the fake never started its grandchild, so this test could not tell killing " +
                "the child from killing the tree — which is the only thing it exists to check.");

            ProcessTree.KillTree(child);

            /* SIGKILL's Windows equivalent is asynchronous too: TerminateJobObject
             * returns before the kernel has finished tearing every process down. */
            var empty = false;
            for (var i = 0; i < 40 && !empty; i++)
            {
                if (child.Job.ProcessIds().Count == 0) empty = true;
                else Thread.Sleep(50);
            }

            Assert.True(empty,
                "something outlived the cancel — the job was not terminated, or a descendant " +
                "escaped it.");
        }
        finally
        {
            ProcessTree.KillTree(child);
            child.Dispose();
        }
    }

    /* The child is in a job of its OWN, containing only its tree. If it shared
     * one with this process, terminating it would take the test runner -- and,
     * in the app, the window -- down with it. */
    [Fact]
    public void TheChildIsInItsOwnJobAndThisProcessIsNot()
    {
        var script = MakeScript("ping -n 600 127.0.0.1 > nul\r\n", "sleeper.cmd");
        var child = Run(script);
        try
        {
            var pids = child.Job.ProcessIds();
            Assert.Contains(child.Id, pids);
            Assert.DoesNotContain(Environment.ProcessId, pids);
        }
        finally
        {
            ProcessTree.KillTree(child);
            child.Dispose();
        }
    }

    /* KILL_ON_JOB_CLOSE. Disposing the job -- which is what happens when the
     * app exits, cleanly or not -- takes the tree with it. This is the one
     * thing this platform does BETTER than the other two: on Linux and macOS an
     * app killed outright orphans its download and it runs to completion with
     * nothing reading it. */
    [Fact]
    public void DisposingTheJobKillsTheTree()
    {
        var script = MakeScript("ping -n 600 127.0.0.1 > nul\r\n", "outlive.cmd");
        var child = Run(script);
        var process = child.Process;

        child.Dispose();

        var exited = false;
        for (var i = 0; i < 40 && !exited; i++)
        {
            try { exited = process.HasExited; }
            catch (Exception) { exited = true; }   // the handle is gone, so it is
            if (!exited) Thread.Sleep(50);
        }
        Assert.True(exited, "the child outlived the job handle being closed");
    }

    [Fact]
    public void TheExitCodeIsReported()
    {
        var script = MakeScript("exit /b 3\r\n", "fails.cmd");
        using var child = Run(script);

        // Both pipes are drained, or a child that filled one would never exit.
        var pumps = new[]
        {
            StartPump(child.Process.StandardOutput.BaseStream),
            StartPump(child.Process.StandardError.BaseStream),
        };
        foreach (var t in pumps) t.Join(10_000);

        Assert.Equal(3, child.WaitExitCode());
    }

    /* Output split on both terminators, end to end through a real process, with
     * the parsers reading what the fake emitted. The MemoryStream tests above
     * pin the splitting itself; this one pins that a real redirected pipe
     * behaves the same way. */
    [Fact]
    public void ARealChildsProgressOutputParsesEndToEnd()
    {
        /* <nul is what stops `set /p` waiting for input, and the trailing dot
         * after the prompt is the cmd idiom for printing without a newline --
         * which is the only way to emit a bare \r from a batch file. */
        var script = MakeScript(
            "echo [youtube] dQw4w9WgXcQ: Downloading webpage\r\n" +
            "<nul set /p \"=[download]   1.0%% of 10.00MiB at 1.00MiB/s ETA 00:10\"\r\n" +
            "<nul set /p \"=\"\r\n" +
            "echo.\r\n" +
            "echo -- Session summary: 1 video(s) touched, 0 already archived (skipped), " +
            "0 error(s), 0 warning(s) --\r\n",
            "progress.cmd");

        using var child = Run(script);
        var lines = new List<(string Text, bool Transient)>();
        var stderrPump = StartPump(child.Process.StandardError.BaseStream);
        ProcessTree.PumpLines(child.Process.StandardOutput.BaseStream,
                              (t, transient) => { lock (lines) lines.Add((t, transient)); });
        stderrPump.Join(10_000);
        child.WaitExitCode();

        var progress = new RunProgress();
        foreach (var (text, _) in lines) OutputParser.ParseProgressLine(text, progress);
        Assert.Equal("dQw4w9WgXcQ", progress.VideoId);

        var summaryLine = lines.Find(l => l.Text.Contains("Session summary:", StringComparison.Ordinal));
        Assert.NotNull(summaryLine.Text);
        var summary = OutputParser.ParseSessionSummary(summaryLine.Text);
        Assert.NotNull(summary);
        Assert.Equal(1, summary!.Value.Videos);
    }

    /* A missing executable is a thrown SpawnException that NAMES THE FILE,
     * rather than a spawn that reports success and a child that never was. A
     * raw Win32Exception says "The system cannot find the file specified" and
     * does not say which one, which is the least useful possible version of
     * this message. */
    [Fact]
    public void AMissingExecutableThrowsAndNamesIt()
    {
        var missing = Path.Combine(_dir, "does-not-exist.exe");
        var ex = Assert.Throws<SpawnException>(
            () => ProcessTree.Run(missing, Array.Empty<string>(), Env()));
        Assert.Contains(missing, ex.Message, StringComparison.Ordinal);
    }

    private static Thread StartPump(Stream stream)
    {
        var t = new Thread(() => ProcessTree.PumpLines(stream, (_, _) => { }))
        {
            IsBackground = true,
        };
        t.Start();
        return t;
    }
}
