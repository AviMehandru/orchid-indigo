/* Profiles, settings, and the atomic write everything they share goes through.
 *
 * Every one of these runs with the app's notion of "home" pointed at a temp
 * directory, so running the suite cannot clobber the profiles, queue or history
 * of whoever runs it. RedirectedHome undoes the redirection even when a test
 * throws -- a leaked override would not fail the test that leaked it, it would
 * fail whichever unrelated test ran next.
 */

using System;
using System.IO;
using System.Linq;
using Xunit;
using YtdlWin.Core;

namespace YtdlWin.Tests;

public sealed class ProfileTests : IDisposable
{
    private readonly RedirectedHome _home;

    public ProfileTests() => _home = new RedirectedHome("ytdl-win-profiles");
    public void Dispose() => _home.Dispose();

    private static RunOptions SampleOptions() => new()
    {
        Url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
        Mode = "audio-only", Quality = "1080", Container = "mp4", Workers = 4, Sync = true,
        YtdlpArgs = { "--match-filter", "duration > 60" },
    };

    // MARK: - The URL rule

    /* THE ONE THING A PRESET MUST NEVER DO. A profile carrying a URL would turn
     * "select a profile" into "select a profile and silently replace what I was
     * about to download". */
    [Fact]
    public void SavingAProfileDropsTheUrl()
    {
        var store = ProfileStore.Load();
        var opts = SampleOptions();
        store.Save("Archival", opts);

        Assert.Equal("", store.Get("Archival")!.Opts.Url);
        // ...and the caller's own options are untouched, because saving a
        // profile must not clear the URL box the user is still working in.
        Assert.Equal("https://www.youtube.com/watch?v=dQw4w9WgXcQ", opts.Url);
    }

    /* Dropped on the way IN as well, so a profiles.json edited by hand cannot
     * hijack a download. */
    [Fact]
    public void AHandEditedUrlIsDroppedOnRead()
    {
        var path = Paths.Join(Paths.StateDir(), "profiles.json");
        FixtureSupport.Write("""
        {
          "active": "Hostile",
          "profiles": [
            { "name": "Hostile", "saved": 0,
              "opts": { "url": "https://example.com/not-what-you-asked-for", "mode": "full" } }
          ]
        }
        """, path);

        var store = ProfileStore.Load();
        Assert.Equal("", store.Get("Hostile")!.Opts.Url);
    }

    // MARK: - Round trip

    [Fact]
    public void ProfilesSurviveAReload()
    {
        var store = ProfileStore.Load();
        store.Save("Archival", SampleOptions());
        store.Save("Quick", new RunOptions { Mode = "video-only", Quality = "720" });

        var reloaded = ProfileStore.Load();
        Assert.Equal(2, reloaded.Profiles.Count);
        Assert.Equal("Quick", reloaded.Active);

        var archival = reloaded.Get("Archival")!;
        Assert.Equal("audio-only", archival.Opts.Mode);
        Assert.Equal(4, archival.Opts.Workers);
        Assert.True(archival.Opts.Sync);
        Assert.Equal(new[] { "--match-filter", "duration > 60" }, archival.Opts.YtdlpArgs);
    }

    /* In CREATION order, not sorted. A menu that reshuffles itself under the
     * pointer is worse than one in an arbitrary but stable order. */
    [Fact]
    public void ProfilesKeepCreationOrder()
    {
        var store = ProfileStore.Load();
        foreach (var name in new[] { "Zulu", "Alpha", "Mike" })
            store.Save(name, new RunOptions());

        Assert.Equal(new[] { "Zulu", "Alpha", "Mike" },
                     ProfileStore.Load().Profiles.Select(p => p.Name).ToArray());
    }

    // MARK: - The default profile a fresh install starts with

    [Fact]
    public void FirstRunInstallsTheDefaultProfile()
    {
        Assert.True(ProfileStore.SeedDefaultIfMissing());

        var store = ProfileStore.Load();
        var p = Assert.Single(store.Profiles);
        Assert.Equal(ProfileStore.DefaultName, p.Name);
        Assert.True(p.Saved > 0);

        /* Nothing is SELECTED. The seeded profile is somewhere to go back to,
         * not a preset silently applied to a form the user has not touched
         * yet. */
        Assert.Null(store.Active);
    }

    /* It carries the app's own defaults, so applying it produces exactly the
     * command line a fresh form produces. A shipped profile that picked a
     * quality or a container would be this window deciding pipeline policy,
     * which is run_ytdlp.ps1's job on the far side of the CLI_VERSION pin. */
    [Fact]
    public void TheDefaultProfileSetsNothing()
    {
        Assert.True(ProfileStore.SeedDefaultIfMissing());
        var p = ProfileStore.Load().Get(ProfileStore.DefaultName)!;

        const string url = "https://example.com/watch?v=aaaaaaaaaaa";
        var fromProfile = p.Opts.Clone();
        fromProfile.Url = url;
        var fresh = new RunOptions { Url = url };

        Assert.Equal(fresh.CommandPreview(), fromProfile.CommandPreview());
    }

    /* profiles.json still exists after a delete -- now holding an empty list --
     * so that is no longer a fresh install. A default that came back at every
     * launch would be a profile the user cannot get rid of. */
    [Fact]
    public void TheDefaultIsSeededOnceAndStaysDeleted()
    {
        Assert.True(ProfileStore.SeedDefaultIfMissing());
        ProfileStore.Load().Delete(ProfileStore.DefaultName);

        Assert.False(ProfileStore.SeedDefaultIfMissing());
        Assert.Empty(ProfileStore.Load().Profiles);
    }

    [Fact]
    public void SeedingNeverTouchesAnExistingStore()
    {
        var store = ProfileStore.Load();
        store.Save("Mine", SampleOptions());

        Assert.False(ProfileStore.SeedDefaultIfMissing());

        var again = ProfileStore.Load();
        Assert.Equal("Mine", Assert.Single(again.Profiles).Name);
        Assert.Null(again.Get(ProfileStore.DefaultName));
    }

    /* This is why the check is "is there a file" rather than "did it parse". An
     * unreadable profiles.json is still somebody's profiles -- half-written by
     * a crash, mangled by an editor mid-save -- and overwriting it with a
     * default is the one recovery nobody can undo. */
    [Fact]
    public void ACorruptStoreIsNotReplacedByTheDefault()
    {
        var path = Paths.Join(Paths.StateDir(), "profiles.json");
        FixtureSupport.Write("{ not json", path);

        Assert.False(ProfileStore.SeedDefaultIfMissing());
        Assert.Equal("{ not json", File.ReadAllText(path));
    }

    // MARK: - Names

    /* Case-insensitive, so "Archival" and "archival" are one profile rather
     * than two indistinguishable rows in a menu -- and re-saving fixes the
     * capitalisation rather than ignoring it. */
    [Fact]
    public void NamesAreCaseInsensitiveAndRecapitalisationSticks()
    {
        var store = ProfileStore.Load();
        store.Save("archival", new RunOptions { Mode = "full" });
        store.Save("Archival", new RunOptions { Mode = "audio-only" });

        Assert.Single(store.Profiles);
        Assert.Equal("Archival", store.Profiles[0].Name);
        Assert.Equal("audio-only", store.Profiles[0].Opts.Mode);
    }

    [Fact]
    public void RenamingOntoAnExistingNameCollides()
    {
        var store = ProfileStore.Load();
        store.Save("One", new RunOptions());
        store.Save("Two", new RunOptions());

        Assert.Throws<ProfileStore.ProfileException>(() => store.Rename("One", "Two"));
    }

    /* ...unless it is the same profile being re-capitalised, which is a rename
     * people actually do. */
    [Fact]
    public void RecapitalisingViaRenameIsAllowed()
    {
        var store = ProfileStore.Load();
        store.Save("archival", new RunOptions());
        store.Rename("archival", "Archival");

        Assert.Equal("Archival", Assert.Single(store.Profiles).Name);
        Assert.Equal("Archival", store.Active);
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    public void AnEmptyNameIsRefused(string name)
    {
        var store = ProfileStore.Load();
        Assert.Throws<ProfileStore.ProfileException>(() => store.Save(name, new RunOptions()));
    }

    [Fact]
    public void AnOverlongNameIsRefused()
    {
        var store = ProfileStore.Load();
        var tooLong = new string('x', ProfileStore.MaxNameLength + 1);
        Assert.Throws<ProfileStore.ProfileException>(() => store.Save(tooLong, new RunOptions()));
    }

    [Fact]
    public void NamesAreTrimmed()
    {
        var store = ProfileStore.Load();
        store.Save("  Archival  ", new RunOptions());
        Assert.Equal("Archival", Assert.Single(store.Profiles).Name);
    }

    // MARK: - The active selection

    /* Left pointing at a name that no longer exists, the menu would show a
     * selection that cannot be applied. */
    [Fact]
    public void DeletingTheActiveProfileClearsTheSelection()
    {
        var store = ProfileStore.Load();
        store.Save("Archival", new RunOptions());
        Assert.Equal("Archival", store.Active);

        store.Delete("Archival");
        Assert.Null(store.Active);
    }

    [Fact]
    public void AStaleActiveNameIsClearedOnLoad()
    {
        FixtureSupport.Write("""
        { "active": "Gone", "profiles": [] }
        """, Paths.Join(Paths.StateDir(), "profiles.json"));

        Assert.Null(ProfileStore.Load().Active);
    }

    [Fact]
    public void DeletingSomethingThatIsNotThereIsAnError()
    {
        var store = ProfileStore.Load();
        Assert.Throws<ProfileStore.ProfileException>(() => store.Delete("Nope"));
    }

    // MARK: - Settings

    [Fact]
    public void SettingsSurviveAReload()
    {
        var settings = new Settings { DataRoot = @"D:\Archive", DefaultWorkers = 6 };
        settings.Save();

        var reloaded = Settings.Load();
        Assert.Equal(@"D:\Archive", reloaded.DataRoot);
        Assert.Equal(6, reloaded.DefaultWorkers);
    }

    /* Empty means "wherever the pipeline puts it by default", which is the
     * install root -- a real choice, not a missing value. */
    [Fact]
    public void AnEmptyDataRootResolvesToTheInstallRoot()
    {
        var settings = new Settings { DataRoot = "" };
        Assert.Equal(Paths.InstallRoot(), settings.ResolvedDataRoot);
    }

    [Fact]
    public void DefaultWorkersIsNeverBelowOne()
    {
        FixtureSupport.Write("""{ "default_workers": 0 }""",
                             Paths.Join(Paths.StateDir(), "settings.json"));
        Assert.Equal(1, Settings.Load().DefaultWorkers);
    }

    // MARK: - The queue and history files

    [Fact]
    public void RunRecordsSurviveARoundTrip()
    {
        var records = new[]
        {
            new RunRecord
            {
                Id = "1", Command = "ytdl \"x\"", State = "done", ExitCode = 0,
                Started = 1_700_000_000, Finished = 1_700_000_100,
                VideosTouched = 12, ArchiveSkipped = 3, Errors = 0, Warnings = 1,
                LogPath = @"C:\yt-dlp\Archive Logs\Logs\download.log",
                LastLine = "-- Session summary: 12 video(s) touched --",
                Opts = new RunOptions { Url = "https://example.com/x", Mode = "audio-only" },
            },
        };

        var path = Paths.Join(Paths.StateDir(), "history.json");
        Runner.WriteRecords(records, path);

        var read = Assert.Single(Runner.ReadRecords(path));
        Assert.Equal("done", read.State);
        Assert.Equal(12, read.VideosTouched);
        Assert.Equal("audio-only", read.Opts.Mode);
        Assert.Equal(records[0].LastLine, read.LastLine);
    }

    /* The four counts default to -1, not 0. A run that was cancelled or died
     * early never printed a session summary, and showing four zeroes would read
     * as "it ran and found nothing". */
    [Fact]
    public void UnreportedCountsReadBackAsMinusOne()
    {
        var path = Paths.Join(Paths.StateDir(), "history.json");
        FixtureSupport.Write("""[ { "id": "1", "state": "cancelled" } ]""", path);

        var read = Assert.Single(Runner.ReadRecords(path));
        Assert.Equal(-1, read.VideosTouched);
        Assert.Equal(-1, read.Errors);
    }

    [Fact]
    public void AMissingOrMalformedStateFileReadsAsEmpty()
    {
        Assert.Empty(Runner.ReadRecords(Paths.Join(Paths.StateDir(), "nothing-here.json")));

        var bad = Paths.Join(Paths.StateDir(), "broken.json");
        FixtureSupport.Write("{ not json", bad);
        Assert.Empty(Runner.ReadRecords(bad));
    }

    // MARK: - The atomic write

    /* Temp file, then File.Replace. Move REFUSES when the destination exists,
     * which after the first save is always -- so the first save takes the Move
     * path and every one after it takes Replace. Both are exercised here
     * because getting only the first one right produces an app that saves once
     * and then silently stops. */
    [Fact]
    public void WritingTwiceReplacesRatherThanFailing()
    {
        var path = Paths.Join(Paths.StateDir(), "atomic.json");

        Assert.True(AtomicFile.Write(System.Text.Encoding.UTF8.GetBytes("first"), path));
        Assert.Equal("first", File.ReadAllText(path));

        Assert.True(AtomicFile.Write(System.Text.Encoding.UTF8.GetBytes("second"), path));
        Assert.Equal("second", File.ReadAllText(path));

        // No .tmp is left behind on the happy path.
        Assert.False(File.Exists(path + ".tmp"));
    }

    [Fact]
    public void WritingCreatesMissingDirectories()
    {
        var path = Paths.Join(Paths.StateDir(), @"nested\deeper\file.json");
        Assert.True(AtomicFile.Write(System.Text.Encoding.UTF8.GetBytes("x"), path));
        Assert.True(File.Exists(path));
    }
}

public sealed class PathTests : IDisposable
{
    private readonly RedirectedHome _home;

    public PathTests() => _home = new RedirectedHome("ytdl-win-paths");
    public void Dispose() => _home.Dispose();

    /* The install root must agree with the platform block at the top of
     * run_ytdlp.ps1 and ytdl.ps1. C:\yt-dlp rather than something under the
     * user profile is not this app's choice -- it is MAX_PATH, and pointing
     * this app somewhere else would mean the library indexing one tree while
     * downloads went to another. */
    [Fact]
    public void TheInstallRootHonoursTheEnvironmentAndOtherwiseIsCYtDlp()
    {
        // The fixture sets YTDLP_INSTALL_ROOT, so that path is what comes back.
        Assert.Equal(Paths.Join(_home.Root, "yt-dlp"), Paths.InstallRoot());

        Paths.EnvironmentOverride = new System.Collections.Generic.Dictionary<string, string>
        {
            ["USERPROFILE"] = _home.Root,
        };
        Assert.Equal(@"C:\yt-dlp", Paths.InstallRoot());
    }

    /* "~" and "~/..." only. A bare "~user" is deliberately NOT expanded,
     * because the pipeline does not expand it either -- and silently resolving
     * it here would reintroduce the two-different-folders bug. */
    [Theory]
    [InlineData("~", true)]
    [InlineData("~/Videos", true)]
    [InlineData(@"~\Videos", true)]
    [InlineData("~someoneelse/Videos", false)]
    [InlineData(@"D:\Archive", false)]
    public void TildeExpansionIsNarrow(string input, bool shouldExpand)
    {
        var expanded = Paths.ExpandTilde(input);
        if (shouldExpand) Assert.StartsWith(_home.Root, expanded, StringComparison.Ordinal);
        else Assert.Equal(input, expanded);
    }

    /* Relative paths inside an entry are ALWAYS '/'-separated, whatever the
     * platform. This is the helper that keeps that true, and the key algorithm
     * depends on it. */
    [Fact]
    public void RelativeJoinsUseForwardSlashes()
    {
        Assert.Equal("Final files/Final Video.mkv",
                     Paths.JoinRel("Final files", "Final Video.mkv"));
        Assert.Equal("Final Video.mkv", Paths.JoinRel("", "Final Video.mkv"));
    }

    [Fact]
    public void CanonicalResolvesDotSegmentsAndTrimsTrailingSeparators()
    {
        Assert.Equal(@"C:\Archive\video", Paths.Canonical(@"C:\Archive\.\video\"));
        Assert.Equal(@"C:\video", Paths.Canonical(@"C:\Archive\..\video"));
        // A bare drive root keeps its separator: "C:" alone means something else.
        Assert.Equal(@"C:\", Paths.Canonical(@"C:\"));
    }

    /* \\?\ is applied only past the threshold. Below it, it buys nothing and
     * costs compatibility -- some tools and some shell APIs reject a prefixed
     * path, and it turns up in error messages the user reads. */
    [Fact]
    public void TheExtendedPrefixIsOnlyAppliedToLongPaths()
    {
        Assert.Equal(@"C:\short\path", Paths.Extended(@"C:\short\path"));

        var long_ = @"C:\" + string.Join(@"\", Enumerable.Repeat(new string('x', 40), 8));
        Assert.True(long_.Length > 240);
        Assert.StartsWith(@"\\?\C:\", Paths.Extended(long_), StringComparison.Ordinal);

        // A UNC path spells it \\?\UNC\server\share, dropping one backslash.
        var unc = @"\\server\share\" + string.Join(@"\", Enumerable.Repeat(new string('y', 40), 8));
        Assert.StartsWith(@"\\?\UNC\server\share", Paths.Extended(unc), StringComparison.Ordinal);

        // Already prefixed is left alone rather than doubled.
        Assert.Equal(@"\\?\C:\x", Paths.Extended(@"\\?\C:\x"));
    }
}
