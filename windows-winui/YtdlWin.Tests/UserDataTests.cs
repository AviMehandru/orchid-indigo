/* Watch state, resume points and playlists.
 *
 * The rules asserted here are the same ones linux-gtk/tests/test_userdata.c
 * and macos-swiftui/Tests/UserDataTests.swift assert, on the same fixture
 * values, for the same reason the library and search fixtures are shared:
 * three independent implementations of one contract are only survivable if
 * the contract is asserted three times on the same input.
 *
 * The three that matter most:
 *
 *   - A corrupt store is NOT silently replaced. The caches are rebuilt from
 *     the archive and losing one costs a rescan; losing this loses the fact
 *     that you watched something, which nothing can reconstruct. So a file
 *     that will not parse leaves the session read-only and stays on disk,
 *     byte for byte.
 *   - A record is NOT stamped with the manifest's archive_creation_time,
 *     which is exactly what VerifyCache does. "I have seen this" is a fact
 *     about the person, and `ytdl --refresh` fetching newer comments must not
 *     un-watch anything.
 *   - Finishing a video clears its resume point, and glancing at the first
 *     ten seconds does not create one. Both are the difference between a
 *     feature people use and one they turn off.
 */

using System;
using System.Collections.Generic;
using System.IO;
using Xunit;
using YtdlWin.Core;

namespace YtdlWin.Tests;

public sealed class UserDataTests : IDisposable
{
    private readonly string _tmpdir;
    private readonly string _storePath;

    public UserDataTests()
    {
        _tmpdir = FixtureSupport.MakeTempDir("ytdl-win-userdata");
        _storePath = Path.Combine(_tmpdir, "userdata.json");
    }

    public void Dispose()
    {
        try { Directory.Delete(_tmpdir, recursive: true); }
        catch (Exception) { /* a leftover temp directory is not a test failure */ }
    }

    private UserData Store() => new(_storePath);

    /* An entry with nothing in it but the fields these facets read. The
     * watched and playlist facets are pure membership tests on the key, so
     * they can be asserted without an archive on disk at all -- which is the
     * point of keeping the filter free of the disk. */
    private static ArchiveEntry Bare(string key) => new()
    {
        Key = key,
        Dir = "",
        Rel = key,
        Channel = "",
        Title = key,
    };

    // ---------------------------------------------------------------- //
    // Watch state                                                      //
    // ---------------------------------------------------------------- //

    [Fact]
    public void WatchedRoundTrips()
    {
        var ud = Store();
        Assert.False(ud.IsWatched("aaa"));

        ud.SetWatched("aaa", true);
        Assert.True(ud.IsWatched("aaa"));
        ud.Save();

        Assert.True(Store().IsWatched("aaa"));
    }

    [Fact]
    public void FinishingClearsTheResumePoint()
    {
        var ud = Store();

        ud.SetPosition("aaa", 400, 1000);
        Assert.Equal(400d, ud.Position("aaa"), 3);
        Assert.False(ud.IsWatched("aaa"));

        /* Past 90%: watched, and the resume point goes. Re-opening something
         * you finished should start it again, not drop you at the end card. */
        ud.SetPosition("aaa", 950, 1000);
        Assert.True(ud.IsWatched("aaa"));
        Assert.Equal(0d, ud.Position("aaa"), 3);
    }

    [Fact]
    public void GlanceIsNotAResumePoint()
    {
        var ud = Store();
        ud.SetPosition("aaa", 8, 1000);
        Assert.Equal(0d, ud.Position("aaa"), 3);
        Assert.False(ud.IsWatched("aaa"));

        /* And a glance does not un-watch something already seen. */
        ud.SetWatched("aaa", true);
        ud.SetPosition("aaa", 8, 1000);
        Assert.True(ud.IsWatched("aaa"));
    }

    [Fact]
    public void UnknownDurationStoresThePositionAndInfersNothing()
    {
        var ud = Store();
        ud.SetPosition("aaa", 600, 0);
        Assert.Equal(600d, ud.Position("aaa"), 3);
        Assert.False(ud.IsWatched("aaa"));
    }

    [Fact]
    public void MarkingWatchedByHandClearsThePosition()
    {
        var ud = Store();
        ud.SetPosition("aaa", 400, 1000);
        ud.SetWatched("aaa", true);
        Assert.Equal(0d, ud.Position("aaa"), 3);

        /* Marking it unwatched again does NOT restore one -- "I want to see
         * this again" and "start it over" are different wishes, and the
         * position is simply gone. */
        ud.SetWatched("aaa", false);
        Assert.Equal(0d, ud.Position("aaa"), 3);
    }

    [Fact]
    public void EmptyRecordsAreNotWritten()
    {
        var ud = Store();
        /* Below the resume floor and not watched: the record says nothing, and
         * writing it would grow the file for every video anyone ever opened. */
        ud.SetPosition("aaa", 3, 1000);
        ud.SetWatched("bbb", true);
        ud.Save();

        var root = JsonFile.Object(_storePath);
        Assert.NotNull(root);
        var watch = root!.Value.Obj("watch");
        Assert.NotNull(watch);

        var keys = new List<string>();
        foreach (var p in watch!.Value.EnumerateObject()) keys.Add(p.Name);

        Assert.DoesNotContain("aaa", keys);
        Assert.Contains("bbb", keys);
    }

    /* The set the facet reads is maintained beside the watch records rather
     * than derived from them each time, which is a duplicated fact and
     * therefore a thing that can drift. Every path that can set or clear a
     * watched flag is driven here. */
    [Fact]
    public void WatchedKeysTracksEveryPath()
    {
        var ud = Store();
        var set = ud.WatchedKeys;
        Assert.Empty(set);

        ud.SetWatched("aaa", true);
        Assert.Contains("aaa", set);
        Assert.Equal(1, ud.WatchedCount);

        /* By finishing: crossing the threshold sets the flag from inside
         * SetPosition, which is the path a hand-maintained set forgets. */
        ud.SetPosition("bbb", 95, 100);
        Assert.Contains("bbb", set);
        Assert.Equal(2, ud.WatchedCount);

        /* A resume point is not being watched. */
        ud.SetPosition("ccc", 40, 100);
        Assert.DoesNotContain("ccc", set);
        Assert.Equal(2, ud.WatchedCount);

        ud.SetWatched("aaa", false);
        Assert.DoesNotContain("aaa", set);
        Assert.Equal(1, ud.WatchedCount);

        /* The set is LIVE, not a snapshot: the same reference reflects all of
         * the above. That is what lets the Library hold it for a session. */
        Assert.Same(set, ud.WatchedKeys);

        ud.Save();
        Assert.Contains("bbb", Store().WatchedKeys);
        Assert.Equal(1, Store().WatchedCount);
    }

    // ---------------------------------------------------------------- //
    // The corrupt-store rule                                           //
    // ---------------------------------------------------------------- //

    [Fact]
    public void CorruptStoreIsReadOnlyAndPreserved()
    {
        File.WriteAllText(_storePath, "{ this is not json");

        var ud = Store();
        Assert.True(ud.IsReadOnly);

        /* The app still runs -- refusing to start over a malformed file would
         * lose the whole application rather than one file. */
        ud.SetWatched("aaa", true);
        ud.Save();

        /* THE POINT: the unparseable file is still there, byte for byte, for
         * its owner to look at. A cache would have been replaced; this is not
         * a cache. */
        Assert.Equal("{ this is not json", File.ReadAllText(_storePath));
    }

    [Fact]
    public void MissingStoreIsNotReadOnly()
    {
        /* A file that is not there is the ordinary first-launch state. It must
         * NOT be treated as corrupt, or a fresh install could never save. */
        var ud = Store();
        Assert.False(ud.IsReadOnly);

        ud.SetWatched("aaa", true);
        ud.Save();
        Assert.True(Store().IsWatched("aaa"));
    }

    // ---------------------------------------------------------------- //
    // Playlists                                                        //
    // ---------------------------------------------------------------- //

    [Fact]
    public void PlaylistCrud()
    {
        var ud = Store();
        Assert.Empty(ud.Playlists);

        /* A blank name is refused: an unnamed playlist is unfindable. */
        Assert.Null(ud.CreatePlaylist("   "));

        var pl = ud.CreatePlaylist("Watch later");
        Assert.NotNull(pl);
        var id = pl!.Id;

        /* Duplicate names are the user's to make. */
        Assert.NotNull(ud.CreatePlaylist("Watch later"));
        Assert.Equal(2, ud.Playlists.Count);

        Assert.True(ud.AddToPlaylist(id, "aaa"));
        /* Adding twice is a no-op, not a duplicate: a playlist is a set the
         * user ordered, not a bag. */
        Assert.False(ud.AddToPlaylist(id, "aaa"));
        Assert.True(ud.PlaylistContains(id, "aaa"));
        Assert.Single(ud.GetPlaylist(id)!.Keys);

        Assert.True(ud.RenamePlaylist(id, "Tonight"));
        Assert.Equal("Tonight", ud.GetPlaylist(id)!.Name);
        Assert.False(ud.RenamePlaylist(id, " "));

        Assert.True(ud.RemoveFromPlaylist(id, "aaa"));
        Assert.False(ud.RemoveFromPlaylist(id, "aaa"));

        Assert.True(ud.DeletePlaylist(id));
        Assert.False(ud.DeletePlaylist(id));
        Assert.Single(ud.Playlists);
    }

    [Fact]
    public void PlaylistsRoundTrip()
    {
        var ud = Store();
        var pl = ud.CreatePlaylist("Rail history")!;
        ud.AddToPlaylist(pl.Id, "aaa");
        ud.AddToPlaylist(pl.Id, "bbb");
        ud.Save();

        var again = Store();
        Assert.Single(again.Playlists);
        /* Order is the user's, so it is preserved rather than sorted. */
        Assert.Equal(new[] { "aaa", "bbb" }, again.GetPlaylist(pl.Id)!.Keys);
        Assert.Equal("Rail history", again.GetPlaylist(pl.Id)!.Name);
    }

    // ---------------------------------------------------------------- //
    // The facets the store feeds                                       //
    // ---------------------------------------------------------------- //

    [Fact]
    public void UnwatchedFacet()
    {
        var f = new LibraryFilter { Flags = FacetFlags.Unwatched };
        var e = Bare("aaa");

        /* Null reads as "nothing is watched", which is exactly right for a
         * fresh install: every video is unwatched. */
        Assert.True(f.Matches(e));

        f.WatchedKeys = new HashSet<string>(StringComparer.Ordinal) { "aaa" };
        Assert.False(f.Matches(e));

        /* With the facet off, being watched is irrelevant again. */
        f.Flags = FacetFlags.None;
        Assert.True(f.Matches(e));
    }

    [Fact]
    public void PlaylistFacetCountAndResetAsymmetry()
    {
        var f = new LibraryFilter();
        var e = Bare("aaa");

        /* Null means no playlist filter. */
        Assert.True(f.Matches(e));
        Assert.Equal(0, f.FacetCount);

        /* An EMPTY playlist shows nothing rather than everything. */
        f.PlaylistKeys = new HashSet<string>(StringComparer.Ordinal);
        Assert.False(f.Matches(e));
        Assert.Equal(1, f.FacetCount);

        f.PlaylistKeys = new HashSet<string>(StringComparer.Ordinal) { "aaa" };
        Assert.True(f.Matches(e));

        /* Reset drops the playlist -- it is a filter -- but must NOT drop
         * WatchedKeys, which is the store's answer about the person. */
        f.WatchedKeys = new HashSet<string>(StringComparer.Ordinal) { "aaa" };
        f.Reset();
        Assert.Null(f.PlaylistKeys);
        Assert.NotNull(f.WatchedKeys);
        Assert.Contains("aaa", f.WatchedKeys!);
    }

    [Fact]
    public void EveryFlagHasALabel()
    {
        /* A flag added to the enum and not named would be a blank checkbox
         * nobody notices. */
        foreach (var flag in Facets.All)
        {
            Assert.False(string.IsNullOrEmpty(Facets.Label(flag)));
        }
        Assert.Equal(5, Facets.All.Length);
    }
}
