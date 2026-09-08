/* info.json, comments and the transcript.
 *
 * Read ON DEMAND, when a video's page is opened -- never during the scan.
 * info.json carries the full comment tree and routinely runs to several
 * megabytes; parsing every one of them to draw a grid of thumbnails would make
 * opening the app cost what opening every video costs.
 *
 * Both parsers here fail QUIETLY when they are wrong -- a mis-threaded comment
 * section still renders, and a transcript with the rolling duplication left in
 * still looks like a transcript. Neither throws. That is why both are pinned by
 * tests rather than trusted.
 */

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;

namespace YtdlWin.Core;

// MARK: - info.json

public sealed class VideoInfo
{
    public JsonElement Root { get; }

    private VideoInfo(JsonElement root) => Root = root;

    /// null when there is no readable info.json -- an ordinary state the layout
    /// contract names, not an error.
    public static VideoInfo? Load(string entryDir)
    {
        var metaDir = Paths.Join(entryDir, "Video metadata");
        var path = ArchiveIndex.InfoJsonPath(metaDir);
        if (path is null) return null;
        var obj = JsonFile.Object(path);
        return obj is null ? null : new VideoInfo(obj.Value);
    }

    public string? String(string key) => Root.Str(key);
    public long Int(string key) => Root.Int(key);

    /// The threaded comments. Empty when the video was fetched with
    /// --no-comments, which is ordinary.
    public List<Comment> Comments => Comment.Thread(Root.Objects("comments"));

    /// Up to 40 tags, joined for display.
    public string Tags => string.Join(", ", Root.Strings("tags").Take(40));
}

// MARK: - Comments

public sealed class Comment
{
    /* yt-dlp's own comment id, used to thread replies onto parents. Deliberately
     * not treated as an identity anywhere: it is "" whenever the extractor did
     * not send one, and a list keyed on it would collapse every such comment
     * into one row. */
    public string CommentId { get; }
    public string Text { get; }
    public string Author { get; }
    public string? AuthorId { get; }
    public string? TimeText { get; }
    /// -1 when absent.
    public long Timestamp { get; }
    /// -1 when absent.
    public long LikeCount { get; }
    public bool IsFavorited { get; }
    public bool AuthorIsUploader { get; }
    public bool IsPinned { get; }
    public List<Comment> Replies { get; } = new();

    public Comment(JsonElement o)
    {
        CommentId = o.Str("id") ?? "";
        Text = o.Str("text") ?? "";
        Author = o.Str("author") ?? "(unknown)";
        AuthorId = o.Str("author_id");
        TimeText = o.Str("_time_text");
        Timestamp = o.Int("timestamp", -1);
        LikeCount = o.Int("like_count", -1);
        IsFavorited = o.Bool("is_favorited");
        AuthorIsUploader = o.Bool("author_is_uploader");
        IsPinned = o.Bool("is_pinned");
    }

    /* Thread yt-dlp's flat comment list.
     *
     * yt-dlp writes comments as a FLAT array with a `parent` field that is
     * either "root" or the parent's id, IN NO GUARANTEED ORDER -- a reply can
     * appear before its parent, so this is a two-pass job rather than a fold.
     *
     * Top level is pinned first, then most-liked: the same order YouTube itself
     * shows, which matters because a transcript of a comment section in
     * arbitrary order is a different document from the one people read. */
    public static List<Comment> Thread(List<JsonElement> raw)
    {
        var tops = new List<Comment>();
        var index = new Dictionary<string, int>(StringComparer.Ordinal);
        var orphans = new List<(string Parent, Comment Comment)>();

        foreach (var o in raw)
        {
            var parent = o.Str("parent");
            var c = new Comment(o);

            if (parent is null || parent == "root")
            {
                // First writer wins on a duplicate id: a later comment claiming
                // an id an earlier one already used must not silently steal its
                // replies.
                if (c.CommentId.Length > 0 && !index.ContainsKey(c.CommentId))
                    index[c.CommentId] = tops.Count;
                tops.Add(c);
            }
            else
            {
                orphans.Add((parent, c));
            }
        }

        foreach (var (parent, c) in orphans)
        {
            int slot = -1;
            if (index.TryGetValue(parent, out var found)) slot = found;
            else
            {
                /* yt-dlp's reply ids are "<parent>.<reply>", so the parent id is
                 * recoverable even when the parent field itself is unhelpful. */
                var dot = parent.IndexOf('.');
                if (dot > 0 && index.TryGetValue(parent[..dot], out var viaDot)) slot = viaDot;
            }

            if (slot >= 0)
            {
                tops[slot].Replies.Add(c);
            }
            else
            {
                /* A reply whose parent is genuinely absent -- a deleted comment,
                 * or a truncated fetch -- is shown at top level rather than
                 * dropped. Silently losing archived text would be the worse
                 * failure. */
                tops.Add(c);
            }
        }

        foreach (var c in tops)
        {
            // OrderBy is a stable sort in LINQ, so replies with equal (or
            // absent) timestamps keep the order yt-dlp wrote them in.
            var ordered = c.Replies.OrderBy(r => r.Timestamp < 0 ? 0 : r.Timestamp).ToList();
            c.Replies.Clear();
            c.Replies.AddRange(ordered);
        }

        /* Pinned first, then most-liked. List.Sort is an UNSTABLE introsort, so
         * the original position is carried as an explicit final tie-break --
         * without it the order of equally-liked comments, which is most of a
         * long thread, would differ between two reads of the same file.
         * (OrderBy would be stable and would not need the tie-break; the
         * explicit comparison is kept because it says out loud what is being
         * relied on.) */
        return tops
            .Select((c, offset) => (Comment: c, Offset: offset))
            .OrderByDescending(x => x.Comment.IsPinned)
            .ThenByDescending(x => Math.Max(x.Comment.LikeCount, 0))
            .ThenBy(x => x.Offset)
            .Select(x => x.Comment)
            .ToList();
    }

    /// This comment plus every reply under it.
    public static int TotalCount(List<Comment> tops) => tops.Sum(c => 1 + c.Replies.Count);
}

// MARK: - Transcript

public sealed class Cue
{
    public double Start { get; init; }
    public double End { get; set; }
    public string Text { get; init; } = "";
}

public static class Transcript
{
    /* A .vtt or .srt turned into a readable transcript.
     *
     * YouTube's auto-generated VTT is a ROLLING TWO-LINE DISPLAY: nearly every
     * cue repeats the previous cue's last line, and words carry inline karaoke
     * timestamps. Read as-is it is unusable as prose, so tags are stripped and
     * the repetition is collapsed. */
    public static List<Cue> Cues(string path)
    {
        byte[] bytes;
        try { bytes = File.ReadAllBytes(Paths.Extended(path)); }
        catch (Exception) { return new List<Cue>(); }

        var raw = Paths.DecodeText(bytes);

        /* Normalise line endings FIRST. A .vtt written on Windows -- which on
         * this platform is most of them -- would otherwise never match the
         * blank-line block separator, and the whole file would parse as one
         * block with no timeline. */
        var normalised = raw.Replace("\r\n", "\n").Replace('\r', '\n');

        var parsed = new List<Cue>();
        foreach (var block in normalised.Split("\n\n"))
        {
            var lines = block.Split('\n')
                             .Where(l => l.Trim().Length > 0)
                             .ToList();
            if (lines.Count == 0) continue;

            var timeIdx = lines.FindIndex(l => l.Contains("-->", StringComparison.Ordinal));
            if (timeIdx < 0) continue;

            var timeline = lines[timeIdx];
            var arrow = timeline.IndexOf("-->", StringComparison.Ordinal);
            if (arrow < 0) continue;

            var start = Seconds(timeline[..arrow]);
            if (start is null) continue;
            var end = Seconds(timeline[(arrow + 3)..]) ?? (start.Value + 3.0);

            var body = string.Join(" ", lines.Skip(timeIdx + 1));
            var text = CollapseWhitespace(UnescapeEntities(StripInlineTags(body)));
            if (text.Length == 0) continue;

            parsed.Add(new Cue { Start = start.Value, End = end, Text = text });
        }

        return CollapseRolling(parsed);
    }

    /* The rolling-display collapse.
     *
     * The comparison is against the previous cue's FULL text, not against what
     * was last emitted. That distinction is the whole algorithm:
     *
     *   cue 1  "the quick brown fox"
     *   cue 2  "the quick brown fox jumps over"
     *   cue 3  "the quick brown fox jumps over the lazy dog"
     *
     * Emitting the tail of cue 2 puts "jumps over" in the output. Comparing cue
     * 3 against THAT finds no common prefix, so cue 3 is emitted whole and the
     * duplication the collapse exists to remove comes straight back on the
     * third line. Keeping lastFull separate from the output is what makes the
     * third line "the lazy dog".
     *
     * The same defect was in orchid-cobalt's src-tauri/src/archive.rs and was
     * found by the C port's own test. It works for two lines and then silently
     * stops, which is why the test in this suite is three lines deep and not
     * two. */
    private static List<Cue> CollapseRolling(List<Cue> cues)
    {
        var output = new List<Cue>();
        string? lastFull = null;

        foreach (var cue in cues)
        {
            if (lastFull is { } previous && output.Count > 0)
            {
                if (string.Equals(cue.Text, previous, StringComparison.Ordinal))
                {
                    output[^1].End = Math.Max(output[^1].End, cue.End);
                    continue;
                }
                /* The 12-character floor keeps a genuinely repeated short line
                 * ("Yeah." then "Yeah. Right.") from being chopped into
                 * fragments. */
                if (cue.Text.StartsWith(previous, StringComparison.Ordinal) && previous.Length > 12)
                {
                    var tail = cue.Text[previous.Length..].Trim();
                    if (tail.Length == 0)
                    {
                        output[^1].End = Math.Max(output[^1].End, cue.End);
                    }
                    else
                    {
                        output.Add(new Cue { Start = cue.Start, End = cue.End, Text = tail });
                    }
                    lastFull = cue.Text;
                    continue;
                }
            }

            output.Add(cue);
            lastFull = cue.Text;
        }
        return output;
    }

    /* [hh:]mm:ss[.,]mmm -- the leading run of digits, colons and a decimal
     * mark. Anything after it (VTT cue settings such as "align:start
     * position:0%") is ignored. */
    public static double? Seconds(string text)
    {
        var trimmed = text.Trim();
        var head = new StringBuilder();
        foreach (var ch in trimmed)
        {
            if (char.IsAsciiDigit(ch) || ch == ':' || ch == '.' || ch == ',') head.Append(ch);
            else break;
        }
        if (head.Length == 0) return null;

        var headText = head.ToString();
        var whole = headText;
        var fraction = "";
        var mark = headText.IndexOfAny(new[] { '.', ',' });
        if (mark >= 0)
        {
            whole = headText[..mark];
            fraction = headText[(mark + 1)..];
        }

        // Not RemoveEmptyEntries: "::30" must fail rather than silently become
        // 30 seconds by having its empty fields dropped.
        var parts = whole.Split(':');
        double h = 0, m = 0, s = 0;
        if (parts.Length == 3)
        {
            h = ParseOrZero(parts[0]);
            m = ParseOrZero(parts[1]);
            s = ParseOrZero(parts[2]);
        }
        else if (parts.Length == 2)
        {
            m = ParseOrZero(parts[0]);
            s = ParseOrZero(parts[1]);
        }
        else
        {
            return null;
        }

        /* ".5" is 500ms, not 5ms -- pad on the RIGHT to three digits. */
        double ms = 0;
        if (fraction.Length > 0)
        {
            var padded = (fraction + "000")[..3];
            ms = ParseOrZero(padded);
        }

        return h * 3600 + m * 60 + s + ms / 1000;
    }

    private static double ParseOrZero(string s)
        => double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : 0;

    /* Removes <c>, </c>, <v Name>, and the per-word <00:00:01.234> karaoke
     * timestamps YouTube's ASR emits. A hand-rolled scanner rather than a
     * regular expression: the grammar is "everything between < and >", and a
     * Regex over a multi-megabyte transcript is not free. */
    public static string StripInlineTags(string s)
    {
        var output = new StringBuilder(s.Length);
        var depth = 0;
        foreach (var ch in s)
        {
            if (ch == '<') depth++;
            else if (ch == '>') { if (depth > 0) depth--; }
            else if (depth == 0) output.Append(ch);
        }
        return output.ToString();
    }

    public static string UnescapeEntities(string s)
    {
        // &amp; is done LAST, so "&amp;lt;" comes out as "&lt;" and not as "<".
        // Doing it first turns an escaped entity into a real one, which is the
        // classic ordering bug in a hand-rolled unescaper.
        var output = s
            .Replace("&lt;", "<")
            .Replace("&gt;", ">")
            .Replace("&quot;", "\"")
            .Replace("&#39;", "'")
            .Replace("&nbsp;", " ")
            .Replace("&amp;", "&");
        return output;
    }

    public static string CollapseWhitespace(string s)
        => string.Join(" ", s.Split(new[] { ' ', '\t', '\n', '\r' },
                                    StringSplitOptions.RemoveEmptyEntries));

    /* The filenames cannot tell an auto-generated track from a human-written
     * one -- --write-subs and --write-auto-subs both land in Subtitles/ under
     * the same base name. The CONTENTS can: ASR output carries per-word karaoke
     * tags and cue-positioning directives that uploaded tracks do not.
     *
     * Only the head is read, and these files can be large. */
    public static bool IsAutoGenerated(string path)
    {
        try
        {
            using var stream = new FileStream(Paths.Extended(path), FileMode.Open,
                                              FileAccess.Read, FileShare.ReadWrite);
            var buffer = new byte[8000];
            var read = stream.Read(buffer, 0, buffer.Length);
            if (read <= 0) return false;

            var head = Paths.DecodeText(buffer[..read]);
            return head.Contains("<c.", StringComparison.Ordinal)
                || head.Contains("<c>", StringComparison.Ordinal)
                || head.Contains("align:start position:", StringComparison.Ordinal);
        }
        catch (Exception) { return false; }
    }
}
