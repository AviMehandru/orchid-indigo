/* Library sorting, faceting, and the verification cache.
 *
 * THE FIXTURE IS THE SAME ONE linux-gtk/tests/test_library.c and
 * macos-swiftui/Tests/LibraryFilterTests.swift build, value for value -- the
 * same six videos, the same channels, dates, modes, sizes and creation
 * stamps. That is deliberate and is the whole point: when all three pass, the
 * C#, Swift and C rule sets have been shown to agree on the same input rather
 * than each being self-consistent. It is the same mitigation the probe's
 * derivation uses, and the only thing that makes three blind implementations
 * of one contract survivable.
 *
 * Most of what is asserted below is not "does the filter filter". It is the
 * handful of decisions that are invisible in a screenshot and wrong in a way
 * nobody reports:
 *
 *   - an empty channel set means EVERY channel, not none, or the grid goes
 *     blank the first time someone opens the facet list and unticks the one
 *     thing they had ticked;
 *   - a video whose UploadDate is not eight digits is EXCLUDED by a date
 *     bound rather than kept, because it has no date and claiming it falls
 *     inside a range invents one;
 *   - the sort is TOTAL, so equal-keyed videos do not swap places between
 *     rebuilds;
 *   - a missing title or date sorts last in BOTH directions, so flipping the
 *     order does not fill the first screen with blanks;
 *   - the "failed verification" facet never shows a video nobody has checked;
 *   - and a cached verification result does not survive the folder being
 *     rewritten underneath it, which is exactly what `ytdl --refresh` does.
 *
 * What is NOT asserted, and must not be: a specific collation of two present
 * titles. This app uses .NET's culture-aware comparison and the other two use
 * their platforms'. The inputs below are chosen so that every reasonable
 * collation orders them the same way.
 */

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Xunit;
using YtdlWin.Core;

namespace YtdlWin.Tests;

public sealed class LibraryFilterTests : IDisposable
{
    private readonly string _root;
    private readonly string _archive;
    private readonly ArchiveIndex _index;

    public LibraryFilterTests()
    {
        _root = FixtureSupport.MakeTempDir("ytdl-win-library");
        _archive = Path.Combine(_root, "Youtube Videos", "Complete Archive");
        Directory.CreateDirectory(_archive);

        MakeVideo("Alpha Channel", "alpha", "Zulu title", "20240131", "full",
                  "Final files/Final Video.mkv", 600, 2, "2026-01-01T00:00:00Z", 0);
        MakeVideo("Alpha Channel", "bravo", "Yankee title", "20230615", "full",
                  "Final files/Final Video.mkv", 200, 2, "2026-01-02T00:00:00Z", 0);
        MakeVideo("Bravo Channel", "charlie", "Xray title", "20251102", "audio-only",
                  "Final files/Final Audio.opus", 100, 2, "2026-01-03T00:00:00Z", 2);
        MakeVideo("Bravo Channel", "delta", "Whiskey title", "20220308", "comments-only",
                  null, 0, 2, "2026-01-04T00:00:00Z", 0);
        MakeVideo("Bravo Channel", "echo", "Victor title", "20240601", "full",
                  "Final files/Final Video.mkv", 50, 3, "2026-01-05T00:00:00Z", 0);
        MakeVideo("Charlie Channel", "foxtrot", "Uniform title", "", "full",
                  "Final files/Final Video.mkv", 10, 2, "2026-01-06T00:00:00Z", 0);

        _index = ArchiveIndex.Scan(_archive);
        Assert.Equal(6, _index.Entries.Count);
    }

    public void Dispose() => FixtureSupport.Delete(_root);

    // ---------------------------------------------------------------- //
    // Fixture                                                          //
    // ---------------------------------------------------------------- //

    /* The FOLDER NAME is built in the documented
     * "<uploader> - <YYYYMMDD> - <id> - <title>" form, and the id is padded to
     * exactly 11 base64url characters, neither of which is decoration. The
     * folder-name parse anchors on "8 digits followed by an 11-character id",
     * so a fixture with a short slug does not parse -- the reader then falls
     * back to using the whole folder name as the title, and the suite ends up
     * asserting a sort over folder names while appearing to assert one over
     * titles. An empty date produces a name that deliberately does not parse,
     * which is how a video with no usable UploadDate is made. */
    private void MakeVideo(string channel, string name, string title, string date,
                           string mode, string? mediaRel, int bytes, int layout,
                           string stamp, int refreshes)
    {
        var id = name.PadRight(11, '_');
        var folder = date.Length == 0
            ? $"Undated {title}"
            : $"{channel} - {date} - {id} - {title}";

        var vdir = Path.Combine(_archive, channel, folder);
        Directory.CreateDirectory(vdir);

        var history = "";
        if (refreshes > 0)
        {
            var records = Enumerable.Range(0, refreshes)
                .Select(i => $"{{\"time\": \"2026-0{i + 1}-01\", \"mode\": \"comments-only\"}}");
            history = ",\n  \"refresh_history\": [" + string.Join(", ", records) + "]";
        }

        var mediaField = mediaRel is null ? "null" : $"\"{mediaRel}\"";
        var manifest =
            "{\n" +
            $"  \"archive_layout_version\": {layout},\n" +
            $"  \"archive_creation_time\": \"{stamp}\",\n" +
            $"  \"video_id\": \"{id}\",\n" +
            $"  \"title\": \"{title}\",\n" +
            $"  \"uploader\": \"{channel}\",\n" +
            $"  \"upload_date\": \"{date}\",\n" +
            $"  \"download_mode\": \"{mode}\",\n" +
            $"  \"media_file\": {mediaField}{history}\n" +
            "}\n";

        FixtureSupport.Write(manifest,
            Path.Combine(vdir, "Video metadata", "manifest.json"));

        if (mediaRel is not null)
        {
            FixtureSupport.Write(new string('x', bytes),
                Path.Combine(vdir, mediaRel.Replace('/', Path.DirectorySeparatorChar)));
        }
    }

    /// The short name a video was made under, recovered from its padded id --
    /// the shortest readable way to assert an ordering.
    private static string ShortName(ArchiveEntry e) =>
        (e.VideoId ?? "(no id)").TrimEnd('_');

    private static List<string> Names(IEnumerable<ArchiveEntry> got) =>
        got.Select(ShortName).ToList();

    private ArchiveEntry Entry(string name) =>
        _index.Entries.First(e => ShortName(e) == name);

    // ---------------------------------------------------------------- //
    // Sorting                                                          //
    // ---------------------------------------------------------------- //

    [Fact]
    public void DefaultIsNewestFirst()
    {
        var f = new LibraryFilter();
        Assert.Equal(SortKey.Date, f.Sort);
        Assert.True(f.Descending);

        var got = Names(f.Apply(_index.Entries));
        Assert.Equal(6, got.Count);
        Assert.Equal(new[] { "charlie", "echo", "alpha", "bravo", "delta" },
                     got.Take(5));

        /* No date at all, and therefore LAST -- not first, which is where a
         * plain reversed string compare would put an empty string. */
        Assert.Equal("foxtrot", got[^1]);
    }

    [Fact]
    public void MissingFieldSortsLastBothWays()
    {
        /* The half of the rule above that is easy to get wrong. If "missing
         * sorts last" were implemented by letting the empty value compare
         * naturally, flipping the direction would move every dateless video to
         * the TOP -- which means the first screen after a flip is blanks. */
        var f = new LibraryFilter { Descending = false };

        var got = Names(f.Apply(_index.Entries));
        Assert.Equal("delta", got[0]);
        Assert.Equal("foxtrot", got[^1]);
    }

    [Fact]
    public void EveryKeySorts()
    {
        var cases = new (SortKey Key, string First)[]
        {
            (SortKey.Title, "alpha"),     // "Zulu title"
            (SortKey.Channel, "foxtrot"), // "Charlie Channel"
            (SortKey.Size, "alpha"),      // 600 bytes of media
        };

        foreach (var (key, first) in cases)
        {
            var f = new LibraryFilter { Sort = key, Descending = true };
            Assert.Equal(first, Names(f.Apply(_index.Entries))[0]);
        }
    }

    [Fact]
    public void SortIsTotal()
    {
        /* Nothing in the fixture has a duration -- there are no info.json
         * files -- so every entry ties on the primary key. The comparison must
         * still be a strict order, or the grid reshuffles equal videos between
         * rebuilds and reads as a rendering bug. */
        var f = new LibraryFilter { Sort = SortKey.Duration };

        var a = f.Apply(_index.Entries);
        var b = f.Apply(_index.Entries);
        Assert.Equal(Names(a), Names(b));

        for (var i = 0; i + 1 < a.Count; i++)
        {
            Assert.NotEqual(0, f.Compare(a[i], a[i + 1]));
        }
    }

    [Fact]
    public void SortIdsRoundTrip()
    {
        /* The setting is persisted by ID, never by the enum's number:
         * inserting a key in the middle would otherwise silently change what
         * every saved setting means. */
        foreach (var key in SortKeys.All)
        {
            Assert.Equal(key, SortKeys.FromId(SortKeys.Id(key)));
            Assert.NotEqual("", SortKeys.Label(key));
        }
        /* A key written by a newer build falls back rather than refusing. */
        Assert.Equal(SortKey.Date, SortKeys.FromId("popularity"));
        Assert.Equal(SortKey.Date, SortKeys.FromId(null));
    }

    // ---------------------------------------------------------------- //
    // Facets                                                           //
    // ---------------------------------------------------------------- //

    [Fact]
    public void EmptyChannelSetMeansAll()
    {
        var f = new LibraryFilter();
        Assert.Empty(f.Channels);
        Assert.Equal(6, f.Apply(_index.Entries).Count);
        Assert.False(f.IsNarrowing);
    }

    [Fact]
    public void ChannelFacetIsAUnion()
    {
        var f = new LibraryFilter();
        f.SetChannel("Alpha Channel", true);
        Assert.Equal(2, f.Apply(_index.Entries).Count);

        /* Two channels is MORE videos, not fewer: within one facet the
         * selections are an OR. Between facets they are an AND. Getting that
         * backwards makes every multi-select facet show nothing. */
        f.SetChannel("Bravo Channel", true);
        Assert.Equal(5, f.Apply(_index.Entries).Count);

        /* A HashSet makes ticking twice idempotent by construction, which is
         * the property the C version has to maintain by hand. */
        f.SetChannel("Bravo Channel", true);
        Assert.Equal(2, f.Channels.Count);
        f.SetChannel("Bravo Channel", false);
        Assert.DoesNotContain("Bravo Channel", f.Channels);
    }

    [Fact]
    public void DateRangeExcludesUndated()
    {
        var f = new LibraryFilter { DateFrom = "20230101", DateTo = "20241231" };

        var got = Names(f.Apply(_index.Entries)).ToHashSet();
        Assert.Equal(new HashSet<string> { "alpha", "bravo", "echo" }, got);

        /* The one that matters. foxtrot has no usable date, so it is not
         * inside this range -- it is a video whose date is unknown, and
         * keeping it would be inventing one. */
        Assert.DoesNotContain("foxtrot", got);

        /* Bounds are inclusive at both ends. */
        f.DateFrom = "20240131";
        f.DateTo = "20240131";
        Assert.Equal(new[] { "alpha" }, Names(f.Apply(_index.Entries)));
    }

    [Fact]
    public void FlagFacets()
    {
        var cases = new (FacetFlags Flag, string Expected)[]
        {
            (FacetFlags.AudioOnly, "charlie"),
            (FacetFlags.NoMedia, "delta"),
            (FacetFlags.LayoutTooNew, "echo"),
        };

        foreach (var (flag, expected) in cases)
        {
            var f = new LibraryFilter { Flags = flag };
            Assert.Equal(new[] { expected }, Names(f.Apply(_index.Entries)));
        }
    }

    [Fact]
    public void AudioOnlyIsNotMediaLess()
    {
        /* Two different states that a reader keying off "no Final Video.mkv"
         * would collapse into one. charlie has media; delta does not. */
        var f = new LibraryFilter
        {
            Flags = FacetFlags.AudioOnly | FacetFlags.NoMedia,
        };
        Assert.Empty(f.Apply(_index.Entries));
    }

    [Fact]
    public void FacetCountAndReset()
    {
        var f = new LibraryFilter
        {
            Sort = SortKey.Size,
            Descending = false,
            DateFrom = "20230101",
            DateTo = "20241231",
            Flags = FacetFlags.AudioOnly,
            Needle = "zulu",
        };
        f.SetChannel("Alpha Channel", true);

        /* A date RANGE is one facet, not two: it is a single idea the user
         * had, and counting it twice makes the count overstate the
         * narrowing. */
        Assert.Equal(3, f.FacetCount);
        Assert.True(f.IsNarrowing);

        f.Reset();
        Assert.Equal(0, f.FacetCount);
        Assert.False(f.IsNarrowing);

        /* The sort survives a reset on purpose: it is a view preference, not a
         * filter, and throwing it away would be a surprise. */
        Assert.Equal(SortKey.Size, f.Sort);
        Assert.False(f.Descending);
    }

    [Fact]
    public void NeedleStillMatchesFourFields()
    {
        var f = new LibraryFilter { Needle = "BRAVO CHANNEL" };
        /* Case-folded, and matching the uploader as well as the title -- the
         * behaviour that was already there and must not regress. */
        Assert.Equal(3, f.Apply(_index.Entries).Count);
    }

    // ---------------------------------------------------------------- //
    // Verification facet and cache                                     //
    // ---------------------------------------------------------------- //

    [Fact]
    public void VerifyFacetIgnoresUnchecked()
    {
        var cache = new VerifyCache(Path.Combine(_root, "verify.json"));
        Assert.Equal(0, cache.KnownCount);

        var f = new LibraryFilter { Flags = FacetFlags.VerifyFailed };

        /* Nothing has been verified, so the facet shows nothing. It must NOT
         * show everything: "these failed" and "these might have failed" are
         * different claims and only one of them is true here. */
        Assert.Empty(f.Apply(_index.Entries, cache.State));

        cache.Set(Entry("alpha"), VerifyState.Failed);
        cache.Set(Entry("bravo"), VerifyState.Ok);

        Assert.Equal(new[] { "alpha" }, Names(f.Apply(_index.Entries, cache.State)));
        Assert.Equal(2, cache.KnownCount);
    }

    [Fact]
    public void VerifyResultDoesNotSurviveARefresh()
    {
        /* The reason every record carries the manifest's
         * archive_creation_time. `ytdl --refresh` rewrites a folder's
         * sidecars, its hashes and -- when it re-embeds the info.json -- the
         * media file itself, all without changing download_mode. A cached
         * "verifies" from before that is not a stale opinion, it is a wrong
         * one. */
        var cache = new VerifyCache(Path.Combine(_root, "verify.json"));
        var alpha = Entry("alpha");
        Assert.Equal("2026-01-01T00:00:00Z", alpha.CreationStamp);

        cache.Set(alpha, VerifyState.Ok);
        Assert.Equal(VerifyState.Ok, cache.State(alpha));

        /* Rewrite the folder the way a refresh would: a new creation stamp and
         * a refresh_history, with download_mode and media_file preserved. */
        MakeVideo("Alpha Channel", "alpha", "Zulu title", "20240131", "full",
                  "Final files/Final Video.mkv", 600, 2, "2026-05-05T00:00:00Z", 1);

        var after = ArchiveIndex.Scan(_archive);
        var again = after.Entries.First(e => ShortName(e) == "alpha");
        Assert.Equal(1, again.RefreshCount);
        Assert.Equal("full", again.DownloadMode);

        /* Same key, same video, different folder -- so the old result is
         * gone. */
        Assert.Equal(alpha.Key, again.Key);
        Assert.Equal(VerifyState.Unknown, cache.State(again));
    }

    [Fact]
    public void VerifyCacheRoundTrips()
    {
        var path = Path.Combine(_root, "verify.json");
        var alpha = Entry("alpha");

        var first = new VerifyCache(path);
        first.Set(alpha, VerifyState.Failed);
        first.Save();

        var reloaded = new VerifyCache(path);
        Assert.Equal(VerifyState.Failed, reloaded.State(alpha));
    }

    [Fact]
    public void CorruptCacheIsEmptyNotFatal()
    {
        var path = Path.Combine(_root, "verify.json");
        FixtureSupport.Write("{ this is not json", path);

        /* A cache is worth one re-verify. There is no version of "refuse to
         * open the Library because a cache file is malformed" that is the
         * right call. */
        var cache = new VerifyCache(path);
        Assert.Equal(0, cache.KnownCount);
    }

    [Fact]
    public void TotalBytesCountsTheFolder()
    {
        /* The whole folder, not just the media file: that is what it costs on
         * the disk it is sitting on, which is the question someone sorting by
         * size is asking. So it is strictly greater than the media alone --
         * the manifest is in there too. */
        Assert.True(Entry("alpha").TotalBytes > 600);
    }
}
