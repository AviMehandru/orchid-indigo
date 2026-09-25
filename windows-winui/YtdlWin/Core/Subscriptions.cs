/* Subscriptions: the pipeline's stored list of sources and its hourly check,
 * as this app sees them.
 *
 * THIS APP RUNS NO TIMER. That is the design, not a gap. A timer here would
 * only fire while this window is open -- the one time nobody needs one -- and
 * this app, the other two in this repository and the Tauri one would each be
 * running their own, racing each other on the same channels and the same
 * manifests. So the list and the schedule live in the pipeline
 * (`ytdl --subscribe`, `ytdl --schedule install`, docs/subscriptions.md in
 * orchid-ochre) -- a Task Scheduler task on this platform -- and every
 * frontend manages them the way it manages everything else: by building a
 * `ytdl` command line.
 *
 * What is here:
 *
 *   - a reader for `ytdl --subscriptions --json`, the contract in that doc;
 *   - the wording every row of the Subscriptions page shows, pinned by one
 *     fixture that YtdlWin.Tests/SubscriptionTests.cs, the GTK app's
 *     tests/test_subscriptions.c and the SwiftUI app's SubscriptionTests.swift
 *     all assert word for word;
 *   - the argument lists for the commands the page sends;
 *   - one "run ytdl with these arguments and tell me what it said", for the
 *     commands that are over in a second. A check itself is NOT run this way:
 *     "Check now" goes through the Downloads queue as
 *     `ytdl --run-subscriptions ID` (RunOptions.SubscriptionId), so it is
 *     sequential with every other run, shows its progress, and can be
 *     cancelled like any of them.
 *
 * Plain .NET with no Microsoft.UI dependency, like everything under Core/, so
 * the test project compiles it from source.
 */

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace YtdlWin.Core;

public sealed class SubscriptionRun
{
    public long Started { get; set; }
    public long Finished { get; set; }
    /// ok | errors | failed
    public string Result { get; set; } = "";
    public int ExitCode { get; set; }
    public long Touched { get; set; }
    public long Skipped { get; set; }
    public long Errors { get; set; }
    public long Warnings { get; set; }
    /// schedule | manual
    public string Trigger { get; set; } = "";
    /// The session's last line when it failed.
    public string? Message { get; set; }
}

public sealed class Subscription
{
    public string Id { get; set; } = "";
    public string Url { get; set; } = "";
    public string? Name { get; set; }
    /// Proxy passwords already masked by the pipeline.
    public List<string> Options { get; set; } = new();
    /// null: the pipeline's default data root.
    public string? DataRoot { get; set; }
    public int EveryHours { get; set; } = 24;
    public bool Enabled { get; set; } = true;
    public long Added { get; set; }
    public long Updated { get; set; }
    public SubscriptionRun? LastRun { get; set; }
    /// 0 when paused.
    public long NextDue { get; set; }
    public bool Due { get; set; }

    /// The name, or the URL when there is none.
    public string Title => string.IsNullOrEmpty(Name) ? Url : Name!;

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
    public string StatusLine(long now)
    {
        if (!Enabled)
        {
            return LastRun == null
                ? "Paused · never checked"
                : "Paused · last checked " + SubscriptionText.Ago(LastRun.Started, now);
        }
        var next = SubscriptionText.In(NextDue > 0 ? NextDue : now, now);
        if (LastRun == null) return "Not checked yet · " + next;

        var ago = SubscriptionText.Ago(LastRun.Started, now);
        // "next due now" reads badly; "due now" on its own says it.
        var tail = next.StartsWith("in ", StringComparison.Ordinal) ? "next " + next : next;
        if (LastRun.Result == "failed") return $"Check failed {ago} · {tail}";

        var found = LastRun.Touched > 0 ? $"{LastRun.Touched} new" : "nothing new";
        if (LastRun.Errors > 0)
            found += $", {LastRun.Errors} error{(LastRun.Errors == 1 ? "" : "s")}";
        return $"Checked {ago} · {found} · {tail}";
    }

    /// A queued run that checks this subscription now. The URL and data root
    /// are carried for the history row and the log path only.
    public RunOptions RunOptions() => new()
    {
        SubscriptionId = Id,
        Url = Url,
        DataRoot = DataRoot ?? "",
    };
}

public sealed class SubscriptionSchedule
{
    /// systemd | launchd | task-scheduler
    public string Mechanism { get; set; } = "";
    public bool Supported { get; set; }
    public bool Installed { get; set; }
    public bool Active { get; set; }
    public bool Running { get; set; }
    /// 0 when the scheduler does not say.
    public long NextCheck { get; set; }
    /// null unknown; Linux only.
    public bool? Linger { get; set; }
    public string Detail { get; set; } = "";

    /* The automatic-checks row:
     *
     *   On · checks hourly · next check in 25 min · only while you are logged in
     *   Off · subscriptions are checked only when you press Check now
     *   Installed but not running · turn it off and on again
     *   <the pipeline's own detail>                  when not supported here
     *
     * with " · checking now" appended while a check is running. */
    public string StatusLine(long now)
    {
        if (!Supported)
            return string.IsNullOrEmpty(Detail) ? "This machine has no scheduler the pipeline can use." : Detail;
        var s = new StringBuilder();
        if (Installed && Active)
        {
            s.Append("On · checks hourly");
            if (NextCheck > 0) s.Append(" · next check ").Append(SubscriptionText.In(NextCheck, now));
            if (Linger == false) s.Append(" · only while you are logged in");
        }
        else if (Installed)
        {
            s.Append("Installed but not running · turn it off and on again");
        }
        else
        {
            s.Append("Off · subscriptions are checked only when you press Check now");
        }
        if (Running) s.Append(" · checking now");
        return s.ToString();
    }
}

public sealed class SubscriptionList
{
    /// The subscriptions_version this app reads. A document declaring a newer
    /// one is refused with a message saying so rather than misread -- the
    /// pipeline bumps it only when a field is removed or changes meaning.
    public const int SupportedVersion = 1;

    public int Version { get; set; }
    /// Compute every "3 h ago" against this, not the clock.
    public long Now { get; set; }
    public SubscriptionSchedule Schedule { get; set; } = new();
    public List<Subscription> Subscriptions { get; set; } = new();

    /// The whole of `ytdl --subscriptions --json`'s stdout. Unknown fields are
    /// ignored; a missing one takes its default rather than failing the list.
    /// Throws FormatException when it is not the document, or declares a newer
    /// version.
    public static SubscriptionList Parse(string json)
    {
        var root = JsonFile.ObjectFrom(json)
            ?? throw new FormatException("The pipeline did not print a subscriptions document.");
        var o = root;
        var version = o.Int("subscriptions_version");
        if (version < 1) throw new FormatException("The pipeline did not print a subscriptions document.");
        if (version > SupportedVersion)
        {
            throw new FormatException(
                $"The pipeline's subscriptions are version {version}; this app reads version "
                + $"{SupportedVersion}. Update the app.");
        }

        var l = new SubscriptionList
        {
            Version = (int)version,
            Now = o.Int("now", DateTimeOffset.UtcNow.ToUnixTimeSeconds()),
        };

        var sc = o.Obj("schedule");
        l.Schedule.Mechanism = sc.Str("mechanism") ?? "";
        l.Schedule.Supported = sc.Bool("supported");
        l.Schedule.Installed = sc.Bool("installed");
        l.Schedule.Active = sc.Bool("active");
        l.Schedule.Running = sc.Bool("running");
        l.Schedule.NextCheck = sc.Int("next_check");
        if (sc.HasValue && sc.Value.TryGetProperty("linger", out var lg)
            && lg.ValueKind is JsonValueKind.True or JsonValueKind.False)
        {
            l.Schedule.Linger = lg.ValueKind == JsonValueKind.True;
        }
        l.Schedule.Detail = sc.Str("detail") ?? "";

        foreach (var so in o.Objects("subscriptions"))
        {
            var s = new Subscription
            {
                Id = so.Str("id") ?? "",
                Url = so.Str("url") ?? "",
            };
            /* Without an id there is nothing to send back; a row that cannot
             * be acted on is left out rather than shown dead. */
            if (s.Id.Length == 0 || s.Url.Length == 0) continue;
            s.Name = so.Str("name");
            s.Options = so.Strings("options");
            s.DataRoot = so.Str("data_root");
            s.EveryHours = (int)so.Int("every_hours", 24);
            s.Enabled = !so.TryGetProperty("enabled", out _) || so.Bool("enabled");
            s.Added = so.Int("added");
            s.Updated = so.Int("updated");
            s.NextDue = so.Int("next_due");
            s.Due = so.Bool("due");
            var lr = so.Obj("last_run");
            if (lr.HasValue)
            {
                s.LastRun = new SubscriptionRun
                {
                    Started = lr.Int("started"),
                    Finished = lr.Int("finished"),
                    Result = lr.Str("result") ?? "",
                    ExitCode = (int)lr.Int("exit_code"),
                    Touched = lr.Int("touched"),
                    Skipped = lr.Int("skipped"),
                    Errors = lr.Int("errors"),
                    Warnings = lr.Int("warnings"),
                    Trigger = lr.Str("trigger") ?? "",
                    Message = lr.Str("message"),
                };
            }
            l.Subscriptions.Add(s);
        }
        return l;
    }
}

/// The relative times and interval labels, exactly as the other two apps
/// write them.
public static class SubscriptionText
{
    /// The intervals the page offers. The pipeline takes any whole number of
    /// hours from 1 to 720; these are the ones worth a menu entry.
    public static readonly int[] IntervalChoices = { 1, 6, 12, 24, 72, 168 };

    /* Half-up integer rounding. Not Math.Round: the three apps must agree to
     * the minute on the same fixture, and Math.Round's default is banker's
     * rounding -- 2.5 h would read "2 h" here and "3 h" in the other two. */
    private static long RoundDiv(long n, long d) => (n + d / 2) / d;

    /// "just now", "4 min ago", "3 h ago", "2 d ago".
    public static string Ago(long then, long now)
    {
        var s = Math.Max(0, now - then);
        if (s < 90) return "just now";
        if (s < 5400) return $"{RoundDiv(s, 60)} min ago";
        if (s < 129_600) return $"{RoundDiv(s, 3600)} h ago";
        return $"{RoundDiv(s, 86400)} d ago";
    }

    /// "due now", "in 25 min", "in 3 h", "in 6 d".
    public static string In(long when, long now)
    {
        var d = when - now;
        if (d <= 0) return "due now";
        if (d < 5400) return $"in {Math.Max(1, RoundDiv(d, 60))} min";
        if (d < 129_600) return $"in {RoundDiv(d, 3600)} h";
        return $"in {RoundDiv(d, 86400)} d";
    }

    /// "Every hour", "Every 6 hours", "Every day", "Every 7 days".
    public static string Every(int hours)
    {
        if (hours > 0 && hours % 24 == 0)
            return hours == 24 ? "Every day" : $"Every {hours / 24} days";
        return hours == 1 ? "Every hour" : $"Every {hours} hours";
    }

    /// --every's value: "6h", or "2d" when it is whole days.
    public static string EveryArgument(int hours)
        => hours % 24 == 0
            ? (hours / 24).ToString(CultureInfo.InvariantCulture) + "d"
            : hours.ToString(CultureInfo.InvariantCulture) + "h";
}

/// The commands the page sends -- everything after ytdl.ps1.
public static class SubscriptionArgs
{
    public static List<string> List() => new() { "--subscriptions", "--json" };

    /* The Downloads form's options, as a subscription. `opts` is the form
     * exactly as a run would send it, connection settings included -- a
     * scheduled check of a members-only playlist needs its cookies as much as
     * the first download did. Built from ToArgs rather than a second list of
     * fields, so an option added to the form is subscribable the day it is
     * added. */
    public static List<string> Subscribe(RunOptions opts, int everyHours, string? name)
    {
        var o = opts.Clone();
        o.SubscriptionId = "";
        o.Refresh = false; // the pipeline refuses to store it
        var v = o.ToArgs();
        v.Add("--subscribe");
        if (everyHours > 0)
        {
            v.Add("--every");
            v.Add(SubscriptionText.EveryArgument(everyHours));
        }
        if (!string.IsNullOrWhiteSpace(name))
        {
            v.Add("--name");
            v.Add(name.Trim());
        }
        return v;
    }

    /// <paramref name="everyHours"/> 0 leaves it alone; <paramref name="pause"/>
    /// null leaves it, false resumes, true pauses.
    public static List<string> Edit(string id, int everyHours = 0, bool? pause = null)
    {
        var v = new List<string> { "--edit-subscription", id };
        if (everyHours > 0)
        {
            v.Add("--every");
            v.Add(SubscriptionText.EveryArgument(everyHours));
        }
        if (pause.HasValue) v.Add(pause.Value ? "--pause" : "--resume");
        return v;
    }

    public static List<string> Unsubscribe(string id) => new() { "--unsubscribe", id };

    public static List<string> Schedule(bool install)
        => new() { "--schedule", install ? "install" : "remove" };
}

/// What a short `ytdl` command printed.
public sealed class YtdlCommandResult
{
    public int ExitCode { get; set; }
    public string StdOut { get; set; } = "";
    public string StdErr { get; set; } = "";

    /* The sentence to show when it failed: the "Error: ..." line if the
     * pipeline printed one -- ytdl.ps1 prints notes before it gets to the
     * refusal, and the note is not why the command failed -- else the first
     * thing it printed, else the exit code. */
    public string Message
    {
        get
        {
            foreach (var text in new[] { StdErr, StdOut })
            {
                foreach (var raw in text.Split('\n'))
                {
                    var line = raw.Trim();
                    if (line.StartsWith("Error:", StringComparison.Ordinal)
                        || line.StartsWith("[subscriptions]", StringComparison.Ordinal))
                        return line;
                }
            }
            foreach (var text in new[] { StdErr, StdOut })
            {
                foreach (var raw in text.Split('\n'))
                {
                    var line = raw.Trim();
                    if (line.Length > 0) return line;
                }
            }
            return $"ytdl exited with code {ExitCode}";
        }
    }

    /* Does this failure mean the installed pipeline predates subscriptions?
     * ytdl.ps1 without them answers "--subscriptions" in the URL position and
     * then refuses --json as an unknown option; one with ytdl.ps1 but no
     * subscriptions.ps1 says so in as many words. */
    public bool MeansTooOld =>
        ExitCode != 0
        && (StdErr.Contains("predates subscriptions", StringComparison.Ordinal)
            || StdErr.Contains("Unknown option: --json", StringComparison.Ordinal)
            || StdErr.Contains("Unknown option: --subscribe", StringComparison.Ordinal));
}

public static class YtdlCommand
{
    /// <c>pwsh -NoProfile -File &lt;installed ytdl.ps1&gt; args</c>, stdout and
    /// stderr captured apart. Throws InvalidOperationException only when the
    /// pipeline could not be started at all; a refusal comes back as a result
    /// with a non-zero exit code.
    public static async Task<YtdlCommandResult> RunAsync(
        IReadOnlyList<string> args, CancellationToken token = default)
    {
        var pwsh = Paths.FindPwsh()
            ?? throw new InvalidOperationException(
                "pwsh (PowerShell 7) was not found, so the pipeline cannot be run.");
        var script = Paths.Join(Paths.ScriptsDir(), "ytdl.ps1");
        if (!Paths.IsRegularFile(script))
        {
            throw new InvalidOperationException(
                $"{script} does not exist. Install the pipeline, or set YTDLP_INSTALL_ROOT "
                + "to where it lives.");
        }

        var psi = new ProcessStartInfo
        {
            FileName = pwsh,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
            UseShellExecute = false,
            /* The one place this matters more than anywhere else in the app:
             * `--schedule install` starts pwsh, and a console window flashing
             * up over the app for every list refresh would be the first thing
             * anybody noticed about this page. */
            CreateNoWindow = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        psi.ArgumentList.Add("-NoProfile");
        psi.ArgumentList.Add("-File");
        psi.ArgumentList.Add(script);
        foreach (var a in args) psi.ArgumentList.Add(a);
        psi.Environment["PATH"] = Paths.ChildPath();

        using var process = new Process { StartInfo = psi };
        if (!process.Start()) throw new InvalidOperationException($"Could not start {pwsh}.");
        process.StandardInput.Close();

        /* Both streams read concurrently: reading one to the end before the
         * other deadlocks as soon as the unread one fills its pipe. */
        var outTask = process.StandardOutput.ReadToEndAsync();
        var errTask = process.StandardError.ReadToEndAsync();
        try
        {
            await process.WaitForExitAsync(token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            try { if (!process.HasExited) process.Kill(entireProcessTree: true); }
            catch (Exception) { /* already gone */ }
            throw;
        }
        return new YtdlCommandResult
        {
            ExitCode = process.ExitCode,
            StdOut = await outTask.ConfigureAwait(false),
            StdErr = await errTask.ConfigureAwait(false),
        };
    }
}
