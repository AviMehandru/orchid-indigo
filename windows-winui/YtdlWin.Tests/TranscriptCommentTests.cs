/* The transcript collapse and the comment tree.
 *
 * Both parsers fail QUIETLY when they are wrong -- a mis-threaded comment
 * section still renders, and a transcript with the rolling duplication left in
 * still looks like a transcript. Neither throws. That is exactly why they are
 * pinned here rather than trusted.
 */

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Xunit;
using YtdlWin.Core;

namespace YtdlWin.Tests;

public sealed class TranscriptTests : IDisposable
{
    private readonly string _dir;

    public TranscriptTests() => _dir = FixtureSupport.MakeTempDir("ytdl-win-vtt");
    public void Dispose() => FixtureSupport.Delete(_dir);

    private string WriteVtt(string body, string name = "captions.vtt")
    {
        var path = Path.Combine(_dir, name);
        FixtureSupport.Write(body, path);
        return path;
    }

    /* THE TEST THAT IS THE WHOLE POINT OF THIS FILE, and it is three lines deep
     * rather than two on purpose.
     *
     * The collapse must compare each cue against the previous cue's FULL text,
     * not against what was last emitted. Emitting the tail of cue 2 puts "jumps
     * over" in the output; comparing cue 3 against THAT finds no common prefix,
     * so cue 3 is emitted whole and the duplication comes straight back on the
     * third line and every line after.
     *
     * A two-line fixture passes against the broken version. That defect was
     * real -- it shipped in orchid-cobalt's src-tauri/src/archive.rs and was
     * found by the C port's own test -- which is why this fixture is three
     * lines and why this comment exists. */
    [Fact]
    public void RollingCaptionsCollapseThreeLinesDeep()
    {
        var path = WriteVtt("""
        WEBVTT

        00:00:01.000 --> 00:00:03.000
        the quick brown fox

        00:00:03.000 --> 00:00:05.000
        the quick brown fox jumps over

        00:00:05.000 --> 00:00:07.000
        the quick brown fox jumps over the lazy dog
        """);

        var cues = Transcript.Cues(path);
        Assert.Equal(3, cues.Count);
        Assert.Equal("the quick brown fox", cues[0].Text);
        Assert.Equal("jumps over", cues[1].Text);
        Assert.Equal("the lazy dog", cues[2].Text);
    }

    /* An identical repeat extends the previous cue rather than adding a line. */
    [Fact]
    public void AnIdenticalRepeatExtendsTheCue()
    {
        var path = WriteVtt("""
        WEBVTT

        00:00:01.000 --> 00:00:03.000
        the quick brown fox jumps

        00:00:03.000 --> 00:00:06.000
        the quick brown fox jumps
        """);

        var cues = Transcript.Cues(path);
        Assert.Single(cues);
        Assert.Equal(6.0, cues[0].End, 3);
    }

    /* The 12-character floor. Without it, "Yeah." followed by "Yeah. Right."
     * is chopped into fragments -- a genuinely repeated short line is not a
     * rolling caption. */
    [Fact]
    public void ShortRepeatsAreNotChopped()
    {
        var path = WriteVtt("""
        WEBVTT

        00:00:01.000 --> 00:00:02.000
        Yeah.

        00:00:02.000 --> 00:00:03.000
        Yeah. Right.
        """);

        var cues = Transcript.Cues(path);
        Assert.Equal(2, cues.Count);
        Assert.Equal("Yeah. Right.", cues[1].Text);
    }

    /* WINDOWS: a .vtt written on this platform has CRLF endings, and on this
     * platform most of them do. Without normalising first, the blank-line block
     * separator never matches, the whole file parses as one block with no
     * timeline, and the transcript tab shows nothing at all -- with no error. */
    [Fact]
    public void CrlfLineEndingsParse()
    {
        var body = "WEBVTT\r\n\r\n00:00:01.000 --> 00:00:03.000\r\nhello there\r\n";
        var path = Path.Combine(_dir, "crlf.vtt");
        FixtureSupport.WriteBytes(Encoding.UTF8.GetBytes(body), path);

        var cues = Transcript.Cues(path);
        Assert.Single(cues);
        Assert.Equal("hello there", cues[0].Text);
    }

    /* YouTube's ASR emits per-word karaoke timestamps and <c> spans. Read as-is
     * the text is unusable as prose. */
    [Fact]
    public void InlineTagsAndEntitiesAreStripped()
    {
        var path = WriteVtt("""
        WEBVTT

        00:00:01.000 --> 00:00:03.000 align:start position:0%
        <00:00:01.200><c>rock</c> <00:00:01.800><c>&amp; roll</c>
        """);

        var cues = Transcript.Cues(path);
        Assert.Single(cues);
        Assert.Equal("rock & roll", cues[0].Text);
    }

    /* &amp; must be unescaped LAST, or "&amp;lt;" becomes "<" instead of
     * "&lt;" -- the classic ordering bug in a hand-rolled unescaper. */
    [Fact]
    public void EntitiesAreUnescapedInTheRightOrder()
        => Assert.Equal("&lt;", Transcript.UnescapeEntities("&amp;lt;"));

    [Theory]
    [InlineData("00:00:01.500", 1.5)]
    [InlineData("00:01:30.000", 90.0)]
    [InlineData("01:02:03.000", 3723.0)]
    [InlineData("01:30.000", 90.0)]
    [InlineData("00:00:01,500", 1.5)]        // SRT's comma
    [InlineData("00:00:01.5", 1.5)]          // padded RIGHT: .5 is 500ms
    [InlineData("00:00:01.05", 1.05)]
    [InlineData("00:00:02.000 align:start position:0%", 2.0)]
    public void TimestampsParse(string text, double expected)
    {
        var seconds = Transcript.Seconds(text);
        Assert.NotNull(seconds);
        Assert.Equal(expected, seconds!.Value, 4);
    }

    [Theory]
    [InlineData("not a timestamp")]
    [InlineData("90")]                       // one field is not a timestamp
    public void AMalformedTimestampYieldsNull(string text)
        => Assert.Null(Transcript.Seconds(text));

    /* A file with no readable cue is empty, not a crash. */
    [Fact]
    public void AFileWithNoCuesYieldsNothing()
        => Assert.Empty(Transcript.Cues(WriteVtt("WEBVTT\n\nnothing useful here\n")));

    [Fact]
    public void AMissingFileYieldsNothing()
        => Assert.Empty(Transcript.Cues(Path.Combine(_dir, "does-not-exist.vtt")));

    /* The filenames cannot tell an auto-generated track from a written one --
     * --write-subs and --write-auto-subs land in the same folder under the same
     * base name. The contents can. */
    [Fact]
    public void AutoGeneratedTracksAreRecognisedByTheirContents()
    {
        var auto = WriteVtt("""
        WEBVTT

        00:00:01.000 --> 00:00:03.000 align:start position:0%
        <00:00:01.200><c>hello</c>
        """, "auto.vtt");

        var written = WriteVtt("""
        WEBVTT

        00:00:01.000 --> 00:00:03.000
        hello there
        """, "written.vtt");

        Assert.True(Transcript.IsAutoGenerated(auto));
        Assert.False(Transcript.IsAutoGenerated(written));
    }
}

public sealed class CommentThreadTests
{
    private static List<System.Text.Json.JsonElement> Parse(string json)
        => JsonFile.ObjectFrom(json).Objects("comments");

    /* yt-dlp writes comments as a FLAT array with a `parent` field, IN NO
     * GUARANTEED ORDER. A reply can appear before its parent, so threading is a
     * two-pass job rather than a fold -- and this fixture puts the reply first
     * precisely so a fold would fail it. */
    [Fact]
    public void ARepliesBeforeItsParentStillThreads()
    {
        var comments = Comment.Thread(Parse("""
        { "comments": [
          { "id": "a.1", "parent": "a", "text": "the reply", "author": "B" },
          { "id": "a", "parent": "root", "text": "the parent", "author": "A" }
        ] }
        """));

        var top = Assert.Single(comments);
        Assert.Equal("the parent", top.Text);
        Assert.Equal("the reply", Assert.Single(top.Replies).Text);
    }

    /* A reply whose parent is genuinely absent -- a deleted comment, or a
     * truncated fetch -- is shown at top level rather than dropped. Silently
     * losing archived text would be the worse failure. */
    [Fact]
    public void AnOrphanIsShownRatherThanDropped()
    {
        var comments = Comment.Thread(Parse("""
        { "comments": [
          { "id": "z.1", "parent": "nobody", "text": "orphan", "author": "B" }
        ] }
        """));

        Assert.Single(comments);
        Assert.Equal("orphan", comments[0].Text);
    }

    /* yt-dlp's reply ids are "<parent>.<reply>", so the parent is recoverable
     * even when the parent field itself is unhelpful. */
    [Fact]
    public void AParentIsRecoveredFromTheReplyIdShape()
    {
        var comments = Comment.Thread(Parse("""
        { "comments": [
          { "id": "a", "parent": "root", "text": "parent", "author": "A" },
          { "id": "a.1", "parent": "a.something-odd", "text": "reply", "author": "B" }
        ] }
        """));

        var top = Assert.Single(comments);
        Assert.Single(top.Replies);
    }

    /* Pinned first, then most-liked -- the order YouTube itself shows, which
     * matters because a comment section in arbitrary order is a different
     * document from the one people read. The pinned comment here has FEW likes,
     * so a sort on likes alone would fail this. */
    [Fact]
    public void PinnedComesFirstThenMostLiked()
    {
        var comments = Comment.Thread(Parse("""
        { "comments": [
          { "id": "a", "parent": "root", "text": "popular", "like_count": 5000 },
          { "id": "b", "parent": "root", "text": "pinned", "like_count": 3, "is_pinned": true },
          { "id": "c", "parent": "root", "text": "middling", "like_count": 100 }
        ] }
        """));

        Assert.Equal(new[] { "pinned", "popular", "middling" },
                     comments.Select(c => c.Text).ToArray());
    }

    /* The tie-break on original position. List.Sort is an unstable introsort,
     * so without an explicit final comparison the order of equally-liked
     * comments -- which is most of a long thread -- would differ between two
     * reads of the same file. */
    [Fact]
    public void EquallyLikedCommentsKeepTheirOriginalOrder()
    {
        const string json = """
        { "comments": [
          { "id": "a", "parent": "root", "text": "first", "like_count": 7 },
          { "id": "b", "parent": "root", "text": "second", "like_count": 7 },
          { "id": "c", "parent": "root", "text": "third", "like_count": 7 },
          { "id": "d", "parent": "root", "text": "fourth", "like_count": 7 }
        ] }
        """;

        var once = Comment.Thread(Parse(json)).Select(c => c.Text).ToArray();
        var twice = Comment.Thread(Parse(json)).Select(c => c.Text).ToArray();

        Assert.Equal(new[] { "first", "second", "third", "fourth" }, once);
        Assert.Equal(once, twice);
    }

    /* Replies are ordered oldest first, and a reply with no timestamp sorts as
     * zero rather than being dropped. */
    [Fact]
    public void RepliesAreOrderedOldestFirst()
    {
        var comments = Comment.Thread(Parse("""
        { "comments": [
          { "id": "a", "parent": "root", "text": "parent" },
          { "id": "a.2", "parent": "a", "text": "later", "timestamp": 2000 },
          { "id": "a.1", "parent": "a", "text": "earlier", "timestamp": 1000 }
        ] }
        """));

        var replies = comments[0].Replies.Select(r => r.Text).ToArray();
        Assert.Equal(new[] { "earlier", "later" }, replies);
    }

    [Fact]
    public void MissingFieldsGetSensibleDefaults()
    {
        var comments = Comment.Thread(Parse("""{ "comments": [ { "parent": "root" } ] }"""));
        var c = Assert.Single(comments);

        Assert.Equal("", c.CommentId);
        Assert.Equal("", c.Text);
        Assert.Equal("(unknown)", c.Author);
        Assert.Equal(-1, c.LikeCount);
        Assert.Equal(-1, c.Timestamp);
        Assert.False(c.IsPinned);
    }

    [Fact]
    public void TotalCountIncludesReplies()
    {
        var comments = Comment.Thread(Parse("""
        { "comments": [
          { "id": "a", "parent": "root" },
          { "id": "a.1", "parent": "a" },
          { "id": "a.2", "parent": "a" },
          { "id": "b", "parent": "root" }
        ] }
        """));

        Assert.Equal(2, comments.Count);
        Assert.Equal(4, Comment.TotalCount(comments));
    }

    [Fact]
    public void NoCommentsIsAnEmptyListNotAFailure()
        => Assert.Empty(Comment.Thread(Parse("""{ "title": "x" }""")));
}
