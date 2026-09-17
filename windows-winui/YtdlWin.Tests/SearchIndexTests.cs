/* Collection-wide comment and transcript search.
 *
 * The tokenizer is the contract, and it is the one function here whose
 * disagreement between the three apps would be INVISIBLE: the same query would
 * quietly return different videos on different platforms, with nothing in any
 * UI saying why. So the strings below are the same ones
 * linux-gtk/tests/test_search.c and macos-swiftui/Tests/SearchIndexTests.swift
 * tokenize, and the expected token lists are written from the rule --
 * "case-folded runs of letters and digits, everything else a separator" --
 * rather than from what any one implementation happens to do.
 *
 * The fixture videos, their comments and their captions are the same ones
 * those suites build, so a query asserted here is a query asserted there.
 */

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Xunit;
using YtdlWin.Core;

namespace YtdlWin.Tests;

public sealed class SearchIndexTests : IDisposable
{
    private readonly string _root;
    private readonly string _archive;
    private readonly ArchiveIndex _index;

    private string IndexPath => Path.Combine(_root, "search-index.json");

    public SearchIndexTests()
    {
        _root = FixtureSupport.MakeTempDir("ytdl-win-search");
        _archive = Path.Combine(_root, "Youtube Videos", "Complete Archive");
        Directory.CreateDirectory(_archive);

        MakeVideo("Tom Scott", "bridge", "The Moving Bridge", "20240131",
                  "2026-01-01T00:00:00Z",
                  "How does the guardrail stay on when it swings?",
                  "The counterweight does most of the work.", VttBridge);

        MakeVideo("Veritasium", "ice", "Strange Ice", "20230615",
                  "2026-01-02T00:00:00Z",
                  "Regelation is the word you are looking for.",
                  "I was taught this was pressure melting.", VttIce);

        _index = ArchiveIndex.Scan(_archive);
        Assert.Equal(2, _index.Entries.Count);
    }

    public void Dispose() => FixtureSupport.Delete(_root);

    // ---------------------------------------------------------------- //
    // Fixture                                                          //
    // ---------------------------------------------------------------- //

    /* Deliberately NOT auto-generated: no karaoke tags, no cue settings. The
     * rolling-duplication collapse is tested elsewhere and would only obscure
     * what this file is about. */
    private const string VttBridge =
        "WEBVTT\n\n" +
        "00:00:01.000 --> 00:00:04.000\n" +
        "this railway bridge swings sideways\n\n" +
        "00:00:04.000 --> 00:00:08.000\n" +
        "to let the canal boats through\n";

    private const string VttIce =
        "WEBVTT\n\n" +
        "00:00:02.000 --> 00:00:06.000\n" +
        "ice behaves strangely under pressure\n";

    /* The comment shapes are yt-dlp's own: a FLAT array with a `parent` field
     * that is either "root" or the parent's id, deliberately out of order,
     * because that is what the reader has to cope with. */
    private void MakeVideo(string channel, string name, string title, string date,
                           string stamp, string commentA, string commentB, string? vtt)
    {
        var id = name.PadRight(11, '_');
        var folder = $"{channel} - {date} - {id} - {title}";
        var vdir = Path.Combine(_archive, channel, folder);
        Directory.CreateDirectory(vdir);

        var manifest =
            "{\n" +
            "  \"archive_layout_version\": 2,\n" +
            $"  \"archive_creation_time\": \"{stamp}\",\n" +
            $"  \"video_id\": \"{id}\",\n" +
            $"  \"title\": \"{title}\",\n" +
            $"  \"uploader\": \"{channel}\",\n" +
            $"  \"upload_date\": \"{date}\",\n" +
            "  \"download_mode\": \"full\",\n" +
            "  \"media_file\": \"Final files/Final Video.mkv\"\n" +
            "}\n";
        FixtureSupport.Write(manifest, Path.Combine(vdir, "Video metadata", "manifest.json"));

        var info =
            "{\n" +
            $"  \"id\": \"{id}\",\n" +
            $"  \"title\": \"{title}\",\n" +
            "  \"description\": \"A description mentioning aqueducts.\",\n" +
            "  \"comments\": [\n" +
            $"    {{\"id\": \"c2\", \"parent\": \"c1\", \"text\": \"{commentB}\", \"author\": \"Replier\"}},\n" +
            $"    {{\"id\": \"c1\", \"parent\": \"root\", \"text\": \"{commentA}\", \"author\": \"Asker\"}}\n" +
            "  ]\n" +
            "}\n";
        FixtureSupport.Write(info, Path.Combine(vdir, "Video metadata", "Video.info.json"));
        FixtureSupport.Write("x", Path.Combine(vdir, "Final files", "Final Video.mkv"));

        if (vtt is not null)
        {
            FixtureSupport.Write(vtt, Path.Combine(vdir, "Subtitles", "Subtitles.en.vtt"));
        }
    }

    private ArchiveEntry Entry(string name) =>
        _index.Entries.First(e => (e.VideoId ?? "").StartsWith(name, StringComparison.Ordinal));

    // ---------------------------------------------------------------- //
    // The tokenizer                                                    //
    // ---------------------------------------------------------------- //

    [Fact]
    public void TokenizerRule()
    {
        /* Case-folded runs of letters and digits; everything else separates. */
        Assert.Equal(new[] { "hello", "world" }, SearchIndex.Tokenize("Hello, World!"));

        /* Digits are letters as far as this is concerned: "1080p" and "av01"
         * are words someone will search for. */
        Assert.Equal(new[] { "1080p", "av01", "x264" }, SearchIndex.Tokenize("1080p AV01 x264"));

        /* The apostrophe is a SEPARATOR, so "don't" is two tokens. That is a
         * choice rather than an oversight, and the point is that all three
         * apps make the same one. */
        Assert.Equal(new[] { "don", "t" }, SearchIndex.Tokenize("don't"));

        /* Punctuation, hyphens and newlines all separate, and runs of them do
         * not produce empty tokens. */
        Assert.Equal(new[] { "well", "known", "stuff" },
                     SearchIndex.Tokenize("well---known  \n stuff."));

        /* Non-ASCII letters are letters, and case-folding is not ASCII-only. */
        Assert.Equal(new[] { "größe", "école" }, SearchIndex.Tokenize("Größe ÉCOLE"));

        Assert.Empty(SearchIndex.Tokenize(""));
        Assert.Empty(SearchIndex.Tokenize("   ...   "));
        Assert.Empty(SearchIndex.Tokenize(null));
    }

    // ---------------------------------------------------------------- //
    // Query semantics                                                  //
    // ---------------------------------------------------------------- //

    [Fact]
    public void AllTokensMustMatch()
    {
        var ix = new SearchIndex(IndexPath);
        ix.Build(_index.Entries);
        Assert.Equal(2, ix.Count);

        var bridge = Entry("bridge");

        /* Both words are in the bridge video's comments; only one is in the
         * ice video's. An OR would return both and would return most of an
         * archive for any two common words. */
        Assert.Equal(new[] { bridge.Key },
                     ix.Query("counterweight work", SearchScope.Comments));

        /* Order does not matter. */
        Assert.Equal(new[] { bridge.Key },
                     ix.Query("work counterweight", SearchScope.Comments));

        /* A token that appears in neither excludes everything, even alongside
         * one that appears in both. */
        Assert.Empty(ix.Query("the zeppelin", SearchScope.Comments));
    }

    [Fact]
    public void PrefixMatchingRespectsTokenBoundaries()
    {
        var ix = new SearchIndex(IndexPath);
        ix.Build(_index.Entries);

        var bridge = Entry("bridge");

        /* "rail" is a prefix of "railway", which is in the bridge
         * transcript. */
        Assert.Equal(new[] { bridge.Key }, ix.Query("rail", SearchScope.Transcript));

        /* THE ONE THAT MATTERS. "rail" must NOT match "guardrail", which is in
         * the bridge video's COMMENTS. A plain substring search over the token
         * blob -- the most likely implementation -- would match it, and that
         * is what makes short queries useless. */
        Assert.Empty(ix.Query("rail", SearchScope.Comments));

        /* And the whole token still matches itself. */
        Assert.Equal(new[] { bridge.Key }, ix.Query("guardrail", SearchScope.Comments));
    }

    [Fact]
    public void ScopesAreSeparate()
    {
        var ix = new SearchIndex(IndexPath);
        ix.Build(_index.Entries);

        /* "sideways" is in the transcript and not in the comments. */
        Assert.Single(ix.Query("sideways", SearchScope.Transcript));
        Assert.Empty(ix.Query("sideways", SearchScope.Comments));
        Assert.Single(ix.Query("sideways", SearchScope.Everything));

        /* The DESCRIPTION is indexed with the comments rather than given a
         * scope of its own -- it is the uploader's own words about the video,
         * which is what someone searching "comments" is reaching for. */
        Assert.Equal(2, ix.Query("aqueducts", SearchScope.Comments).Count);
    }

    [Fact]
    public void EmptyQueryMatchesNothing()
    {
        var ix = new SearchIndex(IndexPath);
        ix.Build(_index.Entries);

        /* "Match everything" is the caller's decision to make, not this
         * method's to guess. */
        Assert.Empty(ix.Query("   ", SearchScope.Everything));
    }

    // ---------------------------------------------------------------- //
    // Freshness                                                        //
    // ---------------------------------------------------------------- //

    [Fact]
    public void OnlyChangedVideosAreReparsed()
    {
        var ix = new SearchIndex(IndexPath);

        Assert.Equal(2, ix.Outdated(_index.Entries));
        ix.Build(_index.Entries);
        Assert.Equal(0, ix.Outdated(_index.Entries));

        /* Rewrite ONE video the way `ytdl --refresh` would: a new creation
         * stamp and new comments, everything else the same. */
        MakeVideo("Tom Scott", "bridge", "The Moving Bridge", "20240131",
                  "2026-05-05T00:00:00Z",
                  "Actually the swing is hydraulic these days.",
                  "Someone said zeppelin and I cannot unsee it.", VttBridge);

        var after = ArchiveIndex.Scan(_archive);
        Assert.Equal(1, ix.Outdated(after.Entries));

        ix.Build(after.Entries);
        Assert.Equal(0, ix.Outdated(after.Entries));

        /* And the new comment text is what is searchable now. */
        Assert.Single(ix.Query("zeppelin", SearchScope.Comments));
        Assert.Empty(ix.Query("counterweight", SearchScope.Comments));
    }

    [Fact]
    public void IndexRoundTripsAndDropsRemoved()
    {
        var first = new SearchIndex(IndexPath);
        first.Build(_index.Entries);
        first.Save();

        var reloaded = new SearchIndex(IndexPath);
        Assert.Equal(2, reloaded.Count);
        Assert.Equal(0, reloaded.Outdated(_index.Entries));

        /* A video that has left the archive leaves the index too, or the store
         * grows forever across rescans of a tree somebody reorganises. */
        Directory.Delete(Path.Combine(_archive, "Veritasium"), recursive: true);
        var after = ArchiveIndex.Scan(_archive);
        Assert.Single(after.Entries);

        reloaded.Build(after.Entries);
        Assert.Equal(1, reloaded.Count);
    }

    [Fact]
    public void CorruptIndexIsEmptyNotFatal()
    {
        FixtureSupport.Write("{ not json at all", IndexPath);
        Assert.Equal(0, new SearchIndex(IndexPath).Count);
    }

    [Fact]
    public void VersionMismatchIsDiscarded()
    {
        /* A store written by a different version of this format is thrown away
         * rather than misread. Without the check, a later change to what a
         * token is would leave every existing user with an index that silently
         * answers the old way. */
        FixtureSupport.Write(
            "{\"version\": 99, \"videos\": {\"abc\": {\"stamp\": \"x\", \"c\": \" hello \"}}}",
            IndexPath);
        Assert.Equal(0, new SearchIndex(IndexPath).Count);
    }

    // ---------------------------------------------------------------- //
    // Snippets                                                         //
    // ---------------------------------------------------------------- //

    [Fact]
    public void SnippetsComeFromTheRealText()
    {
        var bridge = Entry("bridge");

        /* The index holds no text at all, so a snippet is proof that the
         * matched video's own files were re-read. */
        var comments = SearchIndex.Snippets(bridge, "counterweight", SearchScope.Comments);
        var comment = Assert.Single(comments);
        Assert.Contains("counterweight", comment.Text, StringComparison.Ordinal);
        Assert.Equal("Replier", comment.Who);
        Assert.False(comment.FromTranscript);

        /* A transcript hit spans cues: "railway bridge swings" and "canal
         * boats" are different cues, and a matcher that worked per cue would
         * find the phrase in neither. The passage carries a timestamp instead
         * of an author. */
        var cues = SearchIndex.Snippets(bridge, "railway canal", SearchScope.Transcript);
        var cue = Assert.Single(cues);
        Assert.True(cue.FromTranscript);
        Assert.Equal("0:01", cue.Who);

        /* A query that matches the index but not any single passage returns no
         * snippets rather than a wrong one. */
        Assert.Empty(SearchIndex.Snippets(bridge, "zeppelin", SearchScope.Everything));
    }

    [Fact]
    public void ScopeIdsRoundTrip()
    {
        foreach (var scope in SearchScopes.All)
        {
            Assert.Equal(scope, SearchScopes.FromId(SearchScopes.Id(scope)));
            Assert.NotEqual("", SearchScopes.Label(scope));
        }
        Assert.Equal(SearchScope.Metadata, SearchScopes.FromId("lyrics"));
        Assert.Equal(SearchScope.Metadata, SearchScopes.FromId(null));

        /* Only the metadata scope can be answered without the index, and the
         * UI keys its "not built yet" message off exactly this. */
        Assert.False(SearchScopes.NeedsIndex(SearchScope.Metadata));
        Assert.True(SearchScopes.NeedsIndex(SearchScope.Comments));
        Assert.True(SearchScopes.NeedsIndex(SearchScope.Transcript));
        Assert.True(SearchScopes.NeedsIndex(SearchScope.Everything));
    }
}
