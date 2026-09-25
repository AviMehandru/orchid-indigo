/* The Subscriptions page's reading of `ytdl --subscriptions --json`, and every
 * word it shows -- as fixtures.
 *
 * THE SAME DOCUMENT AND THE SAME STRINGS are asserted by
 * linux-gtk/tests/test_subscriptions.c and
 * macos-swiftui/Tests/SubscriptionTests.swift. Three apps describing the same
 * subscription three different ways -- "3 h ago" here, "3 hours ago" there,
 * "2 h ago" in the third because it rounded down -- is the drift this file
 * exists to stop, so the expected strings are spelled out in full. C# is the
 * one of the three where that last one is a real risk: Math.Round rounds to
 * even by default.
 *
 * The document is the contract in orchid-ochre's docs/subscriptions.md, with
 * every state a row can be in: never checked, fine, nothing new, found errors,
 * failed, paused. `now` is fixed, so every relative time is exact.
 */

using System;
using System.Collections.Generic;
using System.Text.Json;
using Xunit;
using YtdlWin.Core;

namespace YtdlWin.Tests;

public sealed class SubscriptionTests
{
    /// now = 1790000000. Each started/next_due is now plus or minus a round
    /// number of seconds, so the expectations below can be checked by eye.
    private const string Fixture = """
    {"subscriptions_version":1,"now":1790000000,
     "schedule":{"mechanism":"systemd","supported":true,"installed":true,"active":true,
       "running":false,"next_check":1790001500,"linger":false,
       "detail":"systemd user timer ytdl-subscriptions.timer, hourly"},
     "subscriptions":[
      {"id":"a1b2c3d4","url":"https://www.youtube.com/@Never/videos","name":null,
       "options":["--sync"],"data_root":null,"every_hours":24,"enabled":true,
       "added":1789990000,"updated":1789990000,"last_run":null,"next_due":1790000000,"due":true},
      {"id":"b2c3d4e5","url":"https://www.youtube.com/@Fine/videos","name":"Fine Channel",
       "options":["--sync","--quality","1080","--proxy","socks5://***@127.0.0.1:1080"],
       "data_root":"/mnt/archive","every_hours":6,"enabled":true,"added":1789000000,"updated":1789000000,
       "last_run":{"started":1789989200,"finished":1789989500,"result":"ok","exit_code":0,
         "touched":2,"skipped":1,"errors":0,"warnings":0,"trigger":"schedule","message":null},
       "next_due":1790010800,"due":false},
      {"id":"c3d4e5f6","url":"https://www.youtube.com/@Quiet/videos","name":"Quiet","options":[],
       "data_root":null,"every_hours":168,"enabled":true,"added":1788000000,"updated":1788000000,
       "last_run":{"started":1789913600,"finished":1789913660,"result":"ok","exit_code":0,
         "touched":0,"skipped":1,"errors":0,"warnings":0,"trigger":"schedule","message":null},
       "next_due":1790518400,"due":false},
      {"id":"d4e5f6a7","url":"https://www.youtube.com/@Errs/videos","name":"Some Errors",
       "options":["--sync"],"data_root":null,"every_hours":1,"enabled":true,"added":1788000000,
       "updated":1788000000,
       "last_run":{"started":1789999970,"finished":1789999990,"result":"errors","exit_code":0,
         "touched":3,"skipped":0,"errors":1,"warnings":2,"trigger":"manual","message":null},
       "next_due":1790003570,"due":false},
      {"id":"e5f6a7b8","url":"https://www.youtube.com/@Broken/videos","name":"Broken",
       "options":["--codec","av1"],"data_root":null,"every_hours":12,"enabled":true,
       "added":1788000000,"updated":1788000000,
       "last_run":{"started":1789992800,"finished":1789992802,"result":"failed","exit_code":1,
         "touched":0,"skipped":0,"errors":0,"warnings":0,"trigger":"schedule",
         "message":"Error: --codec must be one of: any, avc1, vp9, av01 (got: 'av1'). Note av01 is spelled with a zero."},
       "next_due":1790036000,"due":false},
      {"id":"f6a7b8c9","url":"https://www.youtube.com/@Paused/videos","name":"Paused One",
       "options":["--sync"],"data_root":null,"every_hours":48,"enabled":false,
       "added":1788000000,"updated":1788000000,"a_future_field":{"x":1},
       "last_run":{"started":1789740800,"finished":1789740900,"result":"ok","exit_code":0,
         "touched":1,"skipped":0,"errors":0,"warnings":0,"trigger":"schedule","message":null},
       "next_due":null,"due":false}
     ]}
    """;

    private static readonly (string Id, string Title, string Every, string Status)[] Expected =
    {
        ("a1b2c3d4", "https://www.youtube.com/@Never/videos", "Every day",
         "Not checked yet · due now"),
        ("b2c3d4e5", "Fine Channel", "Every 6 hours",
         "Checked 3 h ago · 2 new · next in 3 h"),
        ("c3d4e5f6", "Quiet", "Every 7 days",
         "Checked 24 h ago · nothing new · next in 6 d"),
        ("d4e5f6a7", "Some Errors", "Every hour",
         "Checked just now · 3 new, 1 error · next in 60 min"),
        ("e5f6a7b8", "Broken", "Every 12 hours",
         "Check failed 2 h ago · next in 10 h"),
        ("f6a7b8c9", "Paused One", "Every 2 days",
         "Paused · last checked 3 d ago"),
    };

    [Fact]
    public void ParsesTheFixture()
    {
        var l = SubscriptionList.Parse(Fixture);
        Assert.Equal(1, l.Version);
        Assert.Equal(1_790_000_000, l.Now);
        Assert.Equal(Expected.Length, l.Subscriptions.Count);

        Assert.Equal("systemd", l.Schedule.Mechanism);
        Assert.True(l.Schedule.Supported);
        Assert.True(l.Schedule.Installed);
        Assert.True(l.Schedule.Active);
        Assert.False(l.Schedule.Running);
        Assert.Equal(1_790_001_500, l.Schedule.NextCheck);
        Assert.Equal(false, l.Schedule.Linger);

        var never = l.Subscriptions[0];
        Assert.Null(never.Name);
        Assert.Null(never.LastRun);
        Assert.Null(never.DataRoot);
        Assert.True(never.Due);

        var fine = l.Subscriptions[1];
        Assert.Equal("/mnt/archive", fine.DataRoot);
        Assert.Equal(5, fine.Options.Count);
        Assert.Equal("socks5://***@127.0.0.1:1080", fine.Options[4]);
        Assert.Equal(2, fine.LastRun!.Touched);
        Assert.Equal(1, fine.LastRun.Skipped);
        Assert.Equal("schedule", fine.LastRun.Trigger);

        Assert.Empty(l.Subscriptions[2].Options);

        var broken = l.Subscriptions[4];
        Assert.Equal("failed", broken.LastRun!.Result);
        Assert.Equal(1, broken.LastRun.ExitCode);
        Assert.StartsWith("Error: --codec must be one of", broken.LastRun.Message);

        var paused = l.Subscriptions[5];
        Assert.False(paused.Enabled);
        Assert.Equal(0, paused.NextDue);
    }

    [Fact]
    public void RowWording()
    {
        var l = SubscriptionList.Parse(Fixture);
        for (var i = 0; i < Expected.Length; i++)
        {
            var s = l.Subscriptions[i];
            Assert.Equal(Expected[i].Id, s.Id);
            Assert.Equal(Expected[i].Title, s.Title);
            Assert.Equal(Expected[i].Every, SubscriptionText.Every(s.EveryHours));
            Assert.Equal(Expected[i].Status, s.StatusLine(l.Now));
        }
    }

    [Fact]
    public void ScheduleWording()
    {
        var l = SubscriptionList.Parse(Fixture);
        var s = l.Schedule;
        Assert.Equal("On · checks hourly · next check in 25 min · only while you are logged in",
                     s.StatusLine(l.Now));
        s.Linger = true;
        s.Running = true;
        Assert.Equal("On · checks hourly · next check in 25 min · checking now", s.StatusLine(l.Now));
        s.Running = false;
        s.NextCheck = 0;
        s.Linger = null;
        Assert.Equal("On · checks hourly", s.StatusLine(l.Now));
        s.Active = false;
        Assert.Equal("Installed but not running · turn it off and on again", s.StatusLine(l.Now));
        s.Installed = false;
        Assert.Equal("Off · subscriptions are checked only when you press Check now",
                     s.StatusLine(l.Now));
        s.Supported = false;
        Assert.Equal("systemd user timer ytdl-subscriptions.timer, hourly", s.StatusLine(l.Now));
    }

    [Fact]
    public void RelativeTimes()
    {
        const long now = 1_790_000_000;
        var cases = new (long Delta, string Ago, string In)[]
        {
            (0, "just now", "due now"),
            (89, "just now", "in 1 min"),
            (90, "2 min ago", "in 2 min"),
            (150, "3 min ago", "in 3 min"),     // 2.5 rounds UP, in all three
            (5399, "90 min ago", "in 90 min"),
            (5400, "2 h ago", "in 2 h"),
            (9000, "3 h ago", "in 3 h"),        // 2.5 h rounds up -- not to even
            (129_599, "36 h ago", "in 36 h"),
            (129_600, "2 d ago", "in 2 d"),     // 1.5 d rounds up
        };
        foreach (var c in cases)
        {
            Assert.Equal(c.Ago, SubscriptionText.Ago(now - c.Delta, now));
            Assert.Equal(c.In, SubscriptionText.In(now + c.Delta, now));
        }
        // A clock that went backwards is "just now", not "-3 min ago".
        Assert.Equal("just now", SubscriptionText.Ago(now + 600, now));
        Assert.Equal("due now", SubscriptionText.In(now - 600, now));
    }

    [Fact]
    public void EveryLabels()
    {
        var cases = new (int Hours, string Label)[]
        {
            (1, "Every hour"), (2, "Every 2 hours"), (23, "Every 23 hours"),
            (24, "Every day"), (48, "Every 2 days"), (36, "Every 36 hours"),
            (168, "Every 7 days"), (720, "Every 30 days"),
        };
        foreach (var c in cases) Assert.Equal(c.Label, SubscriptionText.Every(c.Hours));
    }

    [Fact]
    public void RefusesWhatItCannotRead()
    {
        var newer = Assert.Throws<FormatException>(() => SubscriptionList.Parse(
            """{"subscriptions_version":2,"subscriptions":[]}"""));
        Assert.Contains("version 2", newer.Message);
        Assert.Throws<FormatException>(() => SubscriptionList.Parse("[]"));
        Assert.Throws<FormatException>(() => SubscriptionList.Parse("not json"));

        // An empty list is a list.
        var empty = SubscriptionList.Parse(
            """{"subscriptions_version":1,"now":1,"schedule":{"supported":true,"detail":"not installed"},"subscriptions":[]}""");
        Assert.Empty(empty.Subscriptions);
        Assert.Null(empty.Schedule.Linger);
    }

    [Fact]
    public void Arguments()
    {
        Assert.Equal(new[] { "--subscriptions", "--json" }, SubscriptionArgs.List());

        var o = new RunOptions
        {
            Url = "https://www.youtube.com/@Chan/videos",
            Sync = true,
            Quality = "1080",
            Proxy = "socks5://me:pw@127.0.0.1:1080",
            Refresh = true, // never stored: the pipeline refuses it
        };
        Assert.Equal(
            "https://www.youtube.com/@Chan/videos --sync --quality 1080 "
            + "--proxy socks5://me:pw@127.0.0.1:1080 --subscribe --every 12h --name My Channel",
            string.Join(" ", SubscriptionArgs.Subscribe(o, 12, "My Channel")));
        Assert.EndsWith("--subscribe --every 2d",
                        string.Join(" ", SubscriptionArgs.Subscribe(o, 48, null)));
        Assert.True(o.Refresh, "Subscribe must not change the form's own options");

        Assert.Equal(new[] { "--edit-subscription", "b2c3d4e5", "--pause" },
                     SubscriptionArgs.Edit("b2c3d4e5", pause: true));
        Assert.Equal(new[] { "--edit-subscription", "b2c3d4e5", "--every", "7d", "--resume" },
                     SubscriptionArgs.Edit("b2c3d4e5", 168, false));
        Assert.Equal(new[] { "--edit-subscription", "b2c3d4e5", "--every", "6h" },
                     SubscriptionArgs.Edit("b2c3d4e5", 6));
        Assert.Equal(new[] { "--unsubscribe", "b2c3d4e5" }, SubscriptionArgs.Unsubscribe("b2c3d4e5"));
        Assert.Equal(new[] { "--schedule", "install" }, SubscriptionArgs.Schedule(true));
        Assert.Equal(new[] { "--schedule", "remove" }, SubscriptionArgs.Schedule(false));
    }

    [Fact]
    public void CheckNowGoesThroughTheQueue()
    {
        var l = SubscriptionList.Parse(Fixture);
        var o = l.Subscriptions[1].RunOptions();

        /* What the Runner stamps on at enqueue, and what a Run again copies:
         * none of it may reach the command, because the subscription has its
         * own. */
        o.Proxy = "http://other:1";
        o.Quality = "480";
        Assert.Equal(new[] { "--run-subscriptions", "b2c3d4e5" }, o.ToArgs());
        Assert.Equal("ytdl --run-subscriptions b2c3d4e5", o.CommandPreview());

        // Survives the queue file, and a copy.
        var buffer = new System.IO.MemoryStream();
        using (var w = new Utf8JsonWriter(buffer)) o.WriteTo(w);
        using var doc = JsonDocument.Parse(buffer.ToArray());
        var back = RunOptions.FromJson(doc.RootElement.Clone());
        Assert.Equal("b2c3d4e5", back.SubscriptionId);
        Assert.Equal("https://www.youtube.com/@Fine/videos", back.Url);
        Assert.Equal("/mnt/archive", back.DataRoot);
        Assert.Equal("b2c3d4e5", back.Clone().SubscriptionId);

        // A queue file written before subscriptions existed reads as a plain
        // download.
        using var oldDoc = JsonDocument.Parse("""{"url":"https://youtu.be/x","sync":false}""");
        var old = RunOptions.FromJson(oldDoc.RootElement.Clone());
        Assert.Equal("", old.SubscriptionId);
        Assert.Equal("https://youtu.be/x", old.ToArgs()[0]);
    }

    [Fact]
    public void FailureMessages()
    {
        var r = new YtdlCommandResult
        {
            ExitCode = 1,
            StdErr = "Note: without --sync, every check walks the whole listing.\n"
                     + "Error: --codec must be one of: any, avc1, vp9, av01\n",
        };
        Assert.Equal("Error: --codec must be one of: any, avc1, vp9, av01", r.Message);

        r = new YtdlCommandResult
        {
            ExitCode = 3,
            StdErr = "[subscriptions] Another subscription check is already running; not "
                     + "starting a second one.\n",
        };
        Assert.StartsWith("[subscriptions] Another", r.Message);
        Assert.Equal("ytdl exited with code 3", new YtdlCommandResult { ExitCode = 3 }.Message);

        // The two ways an older pipeline answers.
        Assert.True(new YtdlCommandResult
        {
            ExitCode = 1,
            StdErr = "Warning: '--subscriptions' does not look like a YouTube URL.\n"
                     + "Unknown option: --json\n",
        }.MeansTooOld);
        Assert.True(new YtdlCommandResult
        {
            ExitCode = 1,
            StdErr = "Error: /x/scripts/subscriptions.ps1 is missing -- this install predates "
                     + "subscriptions.\n",
        }.MeansTooOld);
        Assert.False(new YtdlCommandResult { ExitCode = 1, StdErr = "Error: no subscription 'x'.\n" }
            .MeansTooOld);
    }
}
