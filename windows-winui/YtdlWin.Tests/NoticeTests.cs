/* What the queue announces, and when -- Core/Notices.cs's rules as fixtures.
 *
 * THE SAME VALUES are asserted by linux-gtk/tests/test_notify.c and
 * macos-swiftui/Tests/NoticeTests.swift. A title or a rule changed here and
 * not there is three apps announcing the same queue three different ways, so
 * the strings are spelled out in full rather than matched loosely.
 *
 * The last test drives the real Runner, to pin the one thing the tracker
 * cannot check for itself: that Runner.Settled never reports nothing left to
 * run while a run is about to start.
 */

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using Xunit;
using YtdlWin.Core;

namespace YtdlWin.Tests;

public sealed class NoticeTests
{
    private static RunRecord Rec(string id, string state, string lastLine = "",
                                 long touched = -1, long skipped = -1, long errors = -1)
    {
        var url = $"https://youtu.be/{id}";
        return new RunRecord
        {
            Id = id, State = state, Command = $"ytdl {url}", LastLine = lastLine,
            VideosTouched = touched, ArchiveSkipped = skipped, Errors = errors, Warnings = 0,
            ExitCode = state == "failed" ? 1 : 0,
            Opts = new RunOptions { Url = url },
        };
    }

    [Fact]
    public void SingleDone()
    {
        var t = new NoticeTracker(Array.Empty<RunRecord>());
        Assert.Equal(new Notice("Download finished",
                                "https://youtu.be/aaaaaaaaaaa\n3 touched, 12 already archived",
                                false),
                     t.Update(new[] { Rec("aaaaaaaaaaa", "done", touched: 3, skipped: 12, errors: 0) }, 0));
    }

    [Fact]
    public void SingleFailed()
    {
        var t = new NoticeTracker(Array.Empty<RunRecord>());
        Assert.Equal(new Notice("Download failed",
                                "https://youtu.be/bbbbbbbbbbb\n"
                                + "ERROR: [youtube] bbbbbbbbbbb: Private video",
                                true),
                     t.Update(new[] { Rec("bbbbbbbbbbb", "failed",
                                          "ERROR: [youtube] bbbbbbbbbbb: Private video") }, 0));
    }

    /// Three runs, one at a time, the middle one failing: silence, the failure
    /// at once, then one summary that repeats it.
    [Fact]
    public void QueueOfThree()
    {
        var t = new NoticeTracker(Array.Empty<RunRecord>());
        var c1 = Rec("c1", "done", touched: 1, skipped: 0, errors: 0);
        var c2 = Rec("c2", "failed", "ERROR: HTTP Error 403: Forbidden", touched: 0, skipped: 0, errors: 1);
        var c3 = Rec("c3", "done", touched: 2, skipped: 5, errors: 0);

        Assert.Null(t.Update(new[] { c1 }, 2));
        Assert.Equal(new Notice("A download failed",
                                "https://youtu.be/c2\nERROR: HTTP Error 403: Forbidden\n"
                                + "The queue goes on: 1 still to run.",
                                true),
                     t.Update(new[] { c2, c1 }, 1));
        // Nothing new: nothing to say, and the failure is not announced twice.
        Assert.Null(t.Update(new[] { c2, c1 }, 1));
        Assert.Equal(new Notice("Queue finished: 2 done, 1 failed",
                                "3 touched, 5 already archived, 1 error\n"
                                + "Last failure: https://youtu.be/c2 — "
                                + "ERROR: HTTP Error 403: Forbidden",
                                true),
                     t.Update(new[] { c3, c2, c1 }, 0));
    }

    [Fact]
    public void SecondFailureCounts()
    {
        var t = new NoticeTracker(Array.Empty<RunRecord>());
        var f1 = Rec("f1", "failed", "ERROR: one");
        var f2 = Rec("f2", "failed", "ERROR: two");
        Assert.Equal("A download failed", t.Update(new[] { f1 }, 3)?.Title);
        Assert.Equal(new Notice("2 downloads failed so far",
                                "https://youtu.be/f2\nERROR: two\nThe queue goes on: 2 still to run.",
                                true),
                     t.Update(new[] { f2, f1 }, 2));
    }

    /// A run finishing and the queue going idle between two polls: the
    /// summary only, never a failure toast followed by a summary.
    [Fact]
    public void CoalescedTick()
    {
        var t = new NoticeTracker(Array.Empty<RunRecord>());
        Assert.Equal(new Notice("Queue finished: 1 done, 1 failed",
                                "4 touched, 0 already archived\n"
                                + "Last failure: https://youtu.be/d2 — ERROR: gone",
                                true),
                     t.Update(new[] { Rec("d2", "failed", "ERROR: gone"),
                                      Rec("d1", "done", touched: 4, skipped: 0, errors: 0) }, 0));
    }

    [Fact]
    public void CancelledOnlyIsSilent()
    {
        var t = new NoticeTracker(Array.Empty<RunRecord>());
        var e1 = Rec("e1", "cancelled");
        Assert.Null(t.Update(new[] { e1 }, 0));
        // ...and the queue it ended is closed: the next one does not count it.
        Assert.Equal(new Notice("Download finished",
                                "https://youtu.be/e2\n1 touched, 0 already archived", false),
                     t.Update(new[] { Rec("e2", "done", touched: 1, skipped: 0, errors: 0), e1 }, 0));
    }

    [Fact]
    public void CancelledIsNamedInAQueue()
    {
        var t = new NoticeTracker(Array.Empty<RunRecord>());
        // No run printed a summary, so there are no counts to give.
        Assert.Equal(new Notice("Queue finished: 1 done, 1 cancelled", "Nothing failed.", false),
                     t.Update(new[] { Rec("g2", "cancelled"), Rec("g1", "done") }, 0));
    }

    [Fact]
    public void RestoredHistoryIsNotAnnounced()
    {
        var restored = new[] { Rec("r1", "failed", "Interrupted") };
        var t = new NoticeTracker(restored);
        Assert.Null(t.Update(restored, 0));
    }

    [Fact]
    public void FailureReasons()
    {
        var t = new NoticeTracker(Array.Empty<RunRecord>());
        var silent = Rec("h1", "failed");
        silent.ExitCode = 2;
        Assert.Equal("https://youtu.be/h1\nytdl exited with code 2.",
                     t.Update(new[] { silent }, 0)?.Body);

        var killed = Rec("h2", "failed", "  \n");
        killed.ExitCode = -1;
        Assert.Equal("https://youtu.be/h2\nIt printed no error before it stopped.",
                     t.Update(new[] { killed, silent }, 0)?.Body);
    }

    /// A record from a store written before options were saved has no URL;
    /// the command stands in for it.
    [Fact]
    public void TargetFallsBackToCommand()
    {
        var t = new NoticeTracker(Array.Empty<RunRecord>());
        var r = Rec("i1", "done");
        r.Opts = new RunOptions();
        Assert.Equal(new Notice("Download finished", "ytdl https://youtu.be/i1", false),
                     t.Update(new[] { r }, 0));
    }

    [Fact]
    public void Clip()
    {
        var c1 = NoticeTracker.Clip(new string('x', 200), 160);
        Assert.Equal(160, c1.EnumerateRunes().Count());
        Assert.EndsWith("x…", c1, StringComparison.Ordinal);

        // Code points, not UTF-16 units -- the count the C copy makes -- and a
        // character outside the BMP is never cut in half.
        var c2 = NoticeTracker.Clip(string.Concat(Enumerable.Repeat("é", 200)), 160);
        Assert.Equal(160, c2.EnumerateRunes().Count());
        var c3 = NoticeTracker.Clip(string.Concat(Enumerable.Repeat("😀", 200)), 160);
        Assert.Equal(160, c3.EnumerateRunes().Count());

        Assert.Equal("ERROR: a b c", NoticeTracker.Clip("  ERROR: a\nb\tc  ", 160));
        Assert.Equal("abc…", NoticeTracker.Clip("abc defgh", 5));
        Assert.Equal("short", NoticeTracker.Clip("short", 160));
    }
}

/* The Runner's side of it. Its own class, because it redirects the state
 * directory for its whole life. */
public sealed class RunnerSettledTests : IDisposable
{
    private readonly RedirectedHome _home;

    public RunnerSettledTests() => _home = new RedirectedHome("ytdl-win-settled");
    public void Dispose() => _home.Dispose();

    /* Settled must never report nothing left to run while a run is between
     * leaving the queue and being recorded as current. The worker takes the
     * item, drops the lock, and only then does RunOne set it current; in that
     * gap the queue is empty and nothing is running.
     *
     * Driven with runs that fail at once: the install root is an empty
     * directory, so each stops at "pwsh was not found" or "ytdl.ps1 does not
     * exist" having gone through the same take / Finish path a real run does,
     * spawning nothing. Polled from this thread as fast as it will go, like
     * the GTK copy.
     *
     * Probabilistic in the direction that matters: it cannot fail on correct
     * code, and it catches the bug some runs rather than every one. When it
     * was written, counting `_current is not null` instead of _inFlight failed
     * it 1 run in 5 here (the GTK copy: 3 in 5). */
    [Fact]
    public void SettledNeverReportsFinishedEarly()
    {
        const int n = 40;
        var runner = new Runner();
        var first = runner.Settled(0)!;
        Assert.Equal(0, first.Remaining);
        Assert.Empty(first.History);
        // Nothing changed since: nothing to report.
        Assert.Null(runner.Settled(first.Version));

        runner.SetPaused(true);
        for (var i = 0; i < n; i++)
            runner.Enqueue(new RunOptions { Url = "https://youtu.be/abcdefghijk" });
        // Paused with runs waiting is not finished.
        Assert.Equal(n, runner.Settled(0)!.Remaining);

        runner.Start();
        runner.SetPaused(false);

        var premature = 0;
        var finished = false;
        var clock = Stopwatch.StartNew();
        while (!finished && clock.Elapsed < TimeSpan.FromSeconds(20))
        {
            var s = runner.Settled(0)!;
            if (s.Remaining == 0)
            {
                if (s.History.Count < n) premature++;
                else finished = true;
            }
        }
        runner.Stop();

        Assert.True(finished);
        Assert.Equal(0, premature);
        var last = runner.Settled(0)!;
        Assert.Equal(n, last.History.Count);
        Assert.All(last.History, r => Assert.Equal("failed", r.State));
    }
}
