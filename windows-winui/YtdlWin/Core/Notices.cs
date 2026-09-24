/* What the queue tells you when you are not looking at the window.
 *
 * This file decides WHAT to say and WHEN. It does not send anything: Notifier
 * turns a Notice into an AppNotification, and only while the window is not
 * the foreground one. Kept in Core, with no Microsoft.UI or App SDK
 * dependency, so the rules are tested by the plain `dotnet test` suite and
 * read side by side with the C and Swift copies -- linux-gtk/src/notify.c and
 * macos-swiftui/Sources/Core/Notices.swift -- which are asserted against the
 * same fixture values (YtdlWin.Tests/NoticeTests.cs).
 *
 * THE RULES. A "queue" is everything the runner does between two moments
 * when it has nothing left to run; thirty re-fetches from one bulk press are
 * one queue, and so is a single Add to queue.
 *
 *  1. When a queue ends, ONE summary, not one notification per run. A bulk
 *     re-fetch would otherwise put thirty toasts on screen, which teaches
 *     people to switch notifications off -- after which the 3am failure this
 *     exists for goes unannounced again.
 *  2. When a run FAILS and more are still to run, say so at once: the end of
 *     the queue may be hours away. A success mid-queue says nothing.
 *  3. Both use ONE tag (NoticeTracker.Tag), so the summary replaces a
 *     mid-queue failure toast instead of stacking under it. The summary
 *     repeats the failure, so nothing is lost.
 *  4. A queue in which every run was cancelled says nothing. Cancelling is
 *     done at the window, by somebody looking at it.
 *  5. Runs restored from the last session's history are never announced.
 *
 * Whether the window is in front, and whether the setting is on, are the
 * caller's to check -- deliberately AFTER Update, so the counts stay right
 * while the window has focus and a summary sent later still covers the whole
 * queue.
 */

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace YtdlWin.Core;

public sealed record Notice(string Title, string Body, bool Failure);

public sealed class NoticeTracker
{
    /// The one tag this app shows toasts under. See rule 3.
    public const string Tag = "queue";
    /// Longest failure reason and run target carried into a notice, in
    /// characters. A yt-dlp error line can be several hundred characters of
    /// URL and traceback; a toast shows two or three lines, and a body the
    /// shell truncates mid-word reads worse than one cut here.
    public const int ReasonMax = 160;
    public const int TargetMax = 100;

    /// Every run id already accounted for. Rebuilt from the history on each
    /// update, so it never outgrows the capped history.
    private HashSet<string> _seen;

    // The queue in progress. Reset when it ends.
    private int _done, _failed, _cancelled;
    /// Any run reported a session summary.
    private bool _haveCounts;
    private long _touched, _skipped, _errors;
    private string _lastDoneTarget = "", _lastFailureTarget = "", _lastFailureReason = "";

    /// `history` is what the Runner restored at startup. Every id in it counts
    /// as already announced -- rule 5.
    public NoticeTracker(IEnumerable<RunRecord> history)
    {
        _seen = new HashSet<string>(history.Select(r => r.Id).Where(id => id.Length > 0));
    }

    private static bool IsAsciiSpace(int c) => c is ' ' or '\t' or '\n' or '\r' or '\v' or '\f';

    /* Whitespace trimmed, then cut at `max` CHARACTERS -- Unicode code points,
     * to count exactly what the C copy's g_utf8_strlen counts, and never
     * splitting a surrogate pair -- with an ellipsis standing in for what was
     * dropped. Line breaks and tabs inside become spaces: the body's own line
     * breaks separate the target from the reason, and a reason that brought
     * its own would scramble that. */
    public static string Clip(string? text, int max)
    {
        if (text is null) return "";
        var runes = text.EnumerateRunes().Select(r => r.Value).ToList();
        while (runes.Count > 0 && IsAsciiSpace(runes[0])) runes.RemoveAt(0);
        while (runes.Count > 0 && IsAsciiSpace(runes[^1])) runes.RemoveAt(runes.Count - 1);
        for (var i = 0; i < runes.Count; i++)
            if (runes[i] is '\n' or '\r' or '\t') runes[i] = ' ';

        static string Join(IEnumerable<int> cps)
        {
            var sb = new StringBuilder();
            foreach (var cp in cps) sb.Append(char.ConvertFromUtf32(cp));
            return sb.ToString();
        }

        if (runes.Count <= max) return Join(runes);

        /* One character goes to the ellipsis, so the result is never longer
         * than asked for; trailing space before it is trimmed so the cut
         * never reads "word …". */
        var head = runes.Take(max - 1).ToList();
        while (head.Count > 0 && IsAsciiSpace(head[^1])) head.RemoveAt(head.Count - 1);
        return Join(head) + "…";
    }

    /// What the run was OF: the URL someone pasted, which is what they would
    /// recognise. The command stands in for a record restored from a store
    /// written before options were saved.
    private static string Target(RunRecord r)
    {
        var t = r.Opts.Url.Length > 0 ? r.Opts.Url
            : r.Command.Length > 0 ? r.Command
            : "a run";
        return Clip(t, TargetMax);
    }

    /// Why it failed: the last line it printed, almost always yt-dlp's ERROR
    /// line or the reason the app could not start it. Only a run that died
    /// silently falls back to its exit code.
    private static string Reason(RunRecord r)
    {
        var clipped = Clip(r.LastLine, ReasonMax);
        if (clipped.Length > 0) return clipped;
        if (r.ExitCode > 0) return $"ytdl exited with code {r.ExitCode}.";
        return "It printed no error before it stopped.";
    }

    private void ResetQueue()
    {
        _done = _failed = _cancelled = 0;
        _haveCounts = false;
        _touched = _skipped = _errors = 0;
        _lastDoneTarget = _lastFailureTarget = _lastFailureReason = "";
    }

    /// "3 touched, 12 already archived, 1 error": the pipeline's own session
    /// summary in its own words, summed. null when no run printed one,
    /// because "0 touched" would read as "it ran and found nothing".
    private string? CountsLine()
    {
        if (!_haveCounts) return null;
        var s = $"{_touched} touched, {_skipped} already archived";
        if (_errors > 0) s += $", {_errors} {(_errors == 1 ? "error" : "errors")}";
        return s;
    }

    private Notice Summary()
    {
        var runs = _done + _failed + _cancelled;
        var counts = CountsLine();
        /* One run: say what it was. "Queue finished: 1 done" would be a
         * strange way to put the common case. */
        if (runs == 1)
        {
            if (_failed == 1)
                return new Notice("Download failed",
                                  $"{_lastFailureTarget}\n{_lastFailureReason}", true);
            return new Notice("Download finished",
                              counts is null ? _lastDoneTarget : $"{_lastDoneTarget}\n{counts}",
                              false);
        }

        var parts = new List<string>();
        if (_done > 0) parts.Add($"{_done} done");
        if (_failed > 0) parts.Add($"{_failed} failed");
        if (_cancelled > 0) parts.Add($"{_cancelled} cancelled");

        var lines = new List<string>();
        if (counts is not null) lines.Add(counts);
        if (_failed > 0) lines.Add($"Last failure: {_lastFailureTarget} — {_lastFailureReason}");
        return new Notice("Queue finished: " + string.Join(", ", parts),
                          lines.Count == 0 ? "Nothing failed." : string.Join("\n", lines),
                          _failed > 0);
    }

    /* Feed it every change, with the history (newest first) and how many runs
     * are still to run -- Runner.Settled, which reads both under one lock.
     * Returns what to announce now, or null.
     *
     * Coalescing is expected: the notifier polls, so one call may carry
     * several newly finished runs, or a run finishing and the queue going
     * idle at once. The second produces only the summary. */
    public Notice? Update(IReadOnlyList<RunRecord> history, int remaining)
    {
        var failedNow = false;

        /* Newest first, so walked backwards: when two runs finish between
         * polls, the later one's failure is the one a notice should name. */
        for (var i = history.Count - 1; i >= 0; i--)
        {
            var r = history[i];
            if (r.Id.Length == 0 || _seen.Contains(r.Id)) continue;

            switch (r.State)
            {
                case "done":
                    _done++;
                    _lastDoneTarget = Target(r);
                    break;
                case "failed":
                    _failed++;
                    failedNow = true;
                    _lastFailureTarget = Target(r);
                    _lastFailureReason = Reason(r);
                    break;
                case "cancelled":
                    _cancelled++;
                    break;
            }
            // -1 means the run never printed a summary; see CountsLine.
            if (r.VideosTouched >= 0)
            {
                _haveCounts = true;
                _touched += r.VideosTouched;
                _skipped += Math.Max(r.ArchiveSkipped, 0);
                _errors += Math.Max(r.Errors, 0);
            }
        }
        _seen = new HashSet<string>(history.Select(r => r.Id).Where(id => id.Length > 0));

        if (remaining == 0)
        {
            // Rule 4: a queue that only ever got cancelled ends silently.
            var notice = _done + _failed > 0 ? Summary() : null;
            ResetQueue();
            return notice;
        }

        if (failedNow)
        {
            var title = _failed == 1 ? "A download failed" : $"{_failed} downloads failed so far";
            return new Notice(title,
                              $"{_lastFailureTarget}\n{_lastFailureReason}\n"
                              + $"The queue goes on: {remaining} still to run.",
                              true);
        }
        return null;
    }
}
