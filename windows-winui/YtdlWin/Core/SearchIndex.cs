/* Searching every comment and every caption line in the archive at once.
 *
 * The pipeline archives every comment and every subtitle cue, three apps parse
 * both, and until now the only way to find a phrase in them was to open one
 * video at a time. The data was already on disk and already understood; what
 * was missing was an index.
 *
 * WHY NOT SQLITE FTS5, which is the obvious answer and which
 * claude/orchid-indigo-gui-comparison.md guessed at. Two reasons, and the
 * first one lands hardest HERE:
 *
 *   1. This app would need Microsoft.Data.Sqlite. "No third-party package
 *      anywhere" is a structural claim this repository makes and keeps -- this
 *      project references exactly the Windows App SDK and nothing else -- and
 *      spending it on a feature that does not need it would be a poor trade.
 *   2. Three FTS engines means three TOKENIZERS that would have to agree about
 *      what a word is, forever, or the same query would return different
 *      videos on different platforms with nothing saying why. That is a fourth
 *      cross-language contract to hold in agreement, and the probe's shared
 *      fixture exists precisely because holding one is already expensive.
 *
 * WHAT IT STORES. Per video, per field, the SORTED UNIQUE TOKENS of that
 * field, space-joined and space-padded. It does not store the text: an index
 * that held every comment would be a second copy of the archive's bulkiest
 * content, and snippets are produced by re-reading the matched videos' own
 * files, which is cheap because a query matches a handful of videos rather
 * than all of them.
 *
 * MATCHING IS BY TOKEN PREFIX, AND ALL TOKENS MUST MATCH. Phrases are NOT
 * supported by the index -- a token set cannot answer "these words, adjacent,
 * in this order" -- and the UI must not imply they are.
 *
 * FRESHNESS. Every record carries the same stamp the verification cache uses:
 * the manifest's archive_creation_time, which postprocess.ps1 rewrites on
 * every pass over a folder including `ytdl --refresh`.
 *
 * THIS IS CACHE. It lives under %LOCALAPPDATA%\ytdl-win\cache, nothing is
 * written inside the archive, and deleting it costs one rebuild.
 */

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace YtdlWin.Core;

/// <summary>Which text a search looks at.</summary>
/// <remarks>
/// Metadata is the substring match over title, uploader, id and channel that
/// the Library already had, and it is the default: it needs no index, it
/// answers instantly, and it is what almost every search is.
/// </remarks>
public enum SearchScope
{
    Metadata = 0,
    Comments,
    Transcript,
    Everything,
}

public static class SearchScopes
{
    public static readonly SearchScope[] All =
    {
        SearchScope.Metadata, SearchScope.Comments,
        SearchScope.Transcript, SearchScope.Everything,
    };

    public static string Label(SearchScope scope) => scope switch
    {
        SearchScope.Comments => "Comments",
        SearchScope.Transcript => "Transcript",
        SearchScope.Everything => "Everything",
        _ => "Title and channel",
    };

    public static string Id(SearchScope scope) => scope switch
    {
        SearchScope.Comments => "comments",
        SearchScope.Transcript => "transcript",
        SearchScope.Everything => "everything",
        _ => "metadata",
    };

    public static SearchScope FromId(string? id) => id switch
    {
        "comments" => SearchScope.Comments,
        "transcript" => SearchScope.Transcript,
        "everything" => SearchScope.Everything,
        _ => SearchScope.Metadata,
    };

    /// <summary>True for every scope that cannot be answered without the index.</summary>
    public static bool NeedsIndex(SearchScope scope) => scope != SearchScope.Metadata;
}

public sealed record SearchSnippet(string Text, string Who, bool FromTranscript);

public sealed class SearchIndex
{
    private sealed class Record
    {
        public string Stamp { get; init; } = "";
        /// " tok tok tok ", sorted and unique.
        public string Comments { get; init; } = "";
        public string Transcript { get; init; } = "";
    }

    private const int IndexVersion = 1;

    /* One caption cue is a few words, so a two-word query almost never falls
     * inside one. Consecutive cues are glued into passages of about this many
     * characters before matching. */
    private const int TranscriptPassage = 240;

    /// The minimum token length the index keeps. One-character tokens match
    /// almost everything and cost the most to store.
    public const int MinToken = 2;

    private readonly string _path;
    private readonly Dictionary<string, Record> _records = new(StringComparer.Ordinal);
    private bool _dirty;

    public SearchIndex(string? path = null)
    {
        _path = path ?? Paths.Join(Paths.CacheDir(), "search-index.json");
        Load();
    }

    // ---------------------------------------------------------------- //
    // Tokenizing                                                       //
    // ---------------------------------------------------------------- //

    /// <summary>
    /// Case-folded runs of letters and digits, everything else a separator.
    /// </summary>
    /// <remarks>
    /// Pinned by the test suite against the same strings the C and Swift
    /// suites use, because this is the one function whose disagreement between
    /// the three apps would be invisible: the same query would quietly return
    /// different videos on different platforms. Apostrophes are separators, so
    /// "don't" is two tokens in every app.
    ///
    /// ToLowerInvariant, not ToLower: the machine's culture must not decide
    /// what is stored in a file another launch reads back, and the Turkish
    /// dotless-i would otherwise make an index built on one machine answer
    /// differently from the same archive indexed on another.
    /// </remarks>
    public static List<string> Tokenize(string? text)
    {
        var outTokens = new List<string>();
        if (string.IsNullOrEmpty(text)) return outTokens;

        var current = new StringBuilder();
        foreach (var ch in text)
        {
            if (char.IsLetterOrDigit(ch))
            {
                current.Append(char.ToLowerInvariant(ch));
            }
            else if (current.Length > 0)
            {
                outTokens.Add(current.ToString());
                current.Clear();
            }
        }
        if (current.Length > 0) outTokens.Add(current.ToString());
        return outTokens;
    }

    /* " tok1 tok2 tok3 " -- sorted, unique, and padded at both ends.
     *
     * The padding is what makes a query match on a token BOUNDARY with a plain
     * substring search: looking for " rail" finds the token "railway" and does
     * not find "guardrail". */
    private static string Blob(IEnumerable<string> tokens)
    {
        var kept = tokens.Where(t => t.Length >= MinToken)
                         .Distinct(StringComparer.Ordinal)
                         .OrderBy(t => t, StringComparer.Ordinal)
                         .ToList();
        /* A single space means "parsed, found nothing" -- a real answer, and
         * one that must be told apart from "not parsed yet". */
        return kept.Count == 0 ? " " : " " + string.Join(" ", kept) + " ";
    }

    // ---------------------------------------------------------------- //
    // Load and save                                                    //
    // ---------------------------------------------------------------- //

    private void Load()
    {
        var root = JsonFile.Object(_path);
        if (root is null) return;

        /* A store written by a different version of this format is discarded
         * rather than misread. Without it, a later change to what a token is
         * would leave every existing user with an index that silently answers
         * the old way. */
        if (root.Value.Int("version", -1) != IndexVersion) return;

        var videos = root.Value.Obj("videos");
        if (videos is null) return;

        foreach (var prop in videos.Value.EnumerateObject())
        {
            if (prop.Value.ValueKind != System.Text.Json.JsonValueKind.Object) continue;
            _records[prop.Name] = new Record
            {
                Stamp = prop.Value.Str("stamp") ?? "",
                Comments = prop.Value.Str("c") ?? "",
                Transcript = prop.Value.Str("t") ?? "",
            };
        }
    }

    /// <summary>Best-effort write to the cache directory.</summary>
    public void Save()
    {
        if (!_dirty) return;

        /* NOT indented, unlike every other file this app writes. This one is
         * machine-read only and is the largest thing in the cache directory;
         * indenting it would add a byte per token for nobody's benefit. */
        var data = JsonFile.Write(w =>
        {
            w.WriteStartObject();
            w.WriteNumber("version", IndexVersion);
            w.WriteStartObject("videos");
            foreach (var (key, rec) in _records)
            {
                w.WriteStartObject(key);
                w.WriteString("stamp", rec.Stamp);
                w.WriteString("c", rec.Comments);
                w.WriteString("t", rec.Transcript);
                w.WriteEndObject();
            }
            w.WriteEndObject();
            w.WriteEndObject();
        }, indented: false);

        try
        {
            AtomicFile.Write(data, _path);
            _dirty = false;
        }
        catch (Exception)
        {
            /* Swallowed on purpose. This is a cache; the next run rebuilds. */
        }
    }

    // ---------------------------------------------------------------- //
    // Freshness                                                        //
    // ---------------------------------------------------------------- //

    private static string Stamp(ArchiveEntry entry)
    {
        if (!string.IsNullOrEmpty(entry.CreationStamp)) return entry.CreationStamp!;

        /* Falling back to the info.json's size and mtime covers a folder with
         * no manifest, which the layout contract names as an ordinary state. */
        var metaDir = Paths.Join(entry.Dir, "Video metadata");
        var info = ArchiveIndex.InfoJsonPath(metaDir);
        if (info is not null)
        {
            try
            {
                var fi = new FileInfo(info);
                return string.Create(CultureInfo.InvariantCulture,
                    $"{fi.LastWriteTimeUtc.Ticks}:{fi.Length}");
            }
            catch (Exception)
            {
                /* Unreadable: fall through to the empty stamp. */
            }
        }

        /* Nothing to key on. Such a folder is re-parsed on every build, which
         * costs nothing because there is nothing in it to parse. */
        return "";
    }

    private bool IsCurrent(ArchiveEntry entry) =>
        _records.TryGetValue(entry.Key, out var rec) &&
        string.Equals(rec.Stamp, Stamp(entry), StringComparison.Ordinal);

    /// <summary>How many videos the index currently covers.</summary>
    public int Count => _records.Count;

    /// <summary>
    /// How many of the archive's videos are missing from the index or stale.
    /// Drives the "index N videos" prompt, which has to be honest about what a
    /// search can currently see.
    /// </summary>
    public int Outdated(IReadOnlyList<ArchiveEntry> entries) =>
        entries.Count(e => !IsCurrent(e));

    // ---------------------------------------------------------------- //
    // Building                                                         //
    // ---------------------------------------------------------------- //

    /* The human-written track if there is one, exactly as the detail page
     * picks it: the auto/human distinction is read from file CONTENTS because
     * it matters which you are reading, and an index built over the auto track
     * when a real one exists would answer differently from the page. */
    private static string? BestSubtitlePath(ArchiveEntry entry)
    {
        string? best = null;
        var bestIsAuto = true;

        for (var i = 0; i < entry.Files.Count; i++)
        {
            if (!MediaExtensions.IsSubtitle(entry.Files[i].Ext)) continue;
            var path = entry.PathForIndex(i);
            if (path is null) continue;
            var isAuto = Transcript.IsAutoGenerated(path);
            if (best is null || (bestIsAuto && !isAuto))
            {
                best = path;
                bestIsAuto = isAuto;
            }
        }
        return best;
    }

    private static void Flatten(List<Comment> comments, List<string> outTexts)
    {
        foreach (var c in comments)
        {
            outTexts.Add(c.Text);
            outTexts.Add(c.Author);
            if (c.Replies.Count > 0) Flatten(c.Replies, outTexts);
        }
    }

    private void IndexOne(ArchiveEntry entry)
    {
        var commentTokens = new List<string>();
        var info = VideoInfo.Load(entry.Dir);
        if (info is not null)
        {
            var texts = new List<string>();
            Flatten(info.Comments, texts);

            /* The description is indexed with the comments rather than given a
             * scope of its own. It is the uploader's own words about the
             * video, which is what someone searching "comments" is reaching
             * for when they half-remember something said about it. */
            if (info.String("description") is { } description) texts.Add(description);

            foreach (var t in texts) commentTokens.AddRange(Tokenize(t));
        }

        var transcriptTokens = new List<string>();
        if (BestSubtitlePath(entry) is { } sub)
        {
            foreach (var cue in Transcript.Cues(sub)) transcriptTokens.AddRange(Tokenize(cue.Text));
        }

        _records[entry.Key] = new Record
        {
            Stamp = Stamp(entry),
            Comments = Blob(commentTokens),
            Transcript = Blob(transcriptTokens),
        };
        _dirty = true;
    }

    /// <summary>
    /// Parse every video whose stamp is missing or stale and record its tokens.
    /// </summary>
    /// <remarks>
    /// Runs on whichever thread calls it and is the expensive operation here:
    /// it parses every changed info.json, which is the same cost the detail
    /// page pays per video, paid once for all of them. Videos that have gone
    /// from the archive are dropped, so the store does not grow forever across
    /// rescans.
    /// </remarks>
    public void Build(IReadOnlyList<ArchiveEntry> entries,
                      Action<int, int>? progress = null,
                      Func<bool>? isCancelled = null)
    {
        for (var i = 0; i < entries.Count; i++)
        {
            if (isCancelled?.Invoke() == true) return;
            if (!IsCurrent(entries[i])) IndexOne(entries[i]);
            progress?.Invoke(i + 1, entries.Count);
        }

        var live = new HashSet<string>(entries.Select(e => e.Key), StringComparer.Ordinal);
        var gone = _records.Keys.Where(k => !live.Contains(k)).ToList();
        foreach (var key in gone)
        {
            _records.Remove(key);
            _dirty = true;
        }
    }

    // ---------------------------------------------------------------- //
    // Querying                                                         //
    // ---------------------------------------------------------------- //

    /* The leading space on the needle is what pins it to a token boundary:
     * " rail" matches the stored " railway " and does not match " guardrail ".
     * Prefix rather than whole-token because this runs as the user types, and a
     * search that returns nothing until the last letter of a word is one people
     * stop using before they finish typing. */
    private static List<string> Needles(string query) =>
        /* A one-character token is kept HERE even though the index does not
         * store one-character tokens: as a prefix it is a perfectly good filter
         * (" a" matches " apple "), and dropping it would make a query narrower
         * as the user typed its first letter and then wider again. */
        Tokenize(query).Select(t => " " + t).ToList();

    private static bool Matches(string blob, List<string> needles)
    {
        if (blob.Length == 0) return false;
        foreach (var n in needles)
        {
            if (!blob.Contains(n, StringComparison.Ordinal)) return false;
        }
        return true;
    }

    /// <summary>
    /// The keys of the videos whose <paramref name="scope"/> text matches every
    /// token of <paramref name="query"/>. An empty query yields an empty set,
    /// because "match everything" is the caller's business to decide.
    /// </summary>
    public HashSet<string> Query(string query, SearchScope scope)
    {
        var hits = new HashSet<string>(StringComparer.Ordinal);
        var needles = Needles(query);
        if (needles.Count == 0) return hits;

        foreach (var (key, rec) in _records)
        {
            var hit = false;
            if (scope is SearchScope.Comments or SearchScope.Everything)
            {
                hit = Matches(rec.Comments, needles);
            }
            if (!hit && scope is SearchScope.Transcript or SearchScope.Everything)
            {
                hit = Matches(rec.Transcript, needles);
            }
            if (hit) hits.Add(key);
        }
        return hits;
    }

    // ---------------------------------------------------------------- //
    // Snippets                                                         //
    // ---------------------------------------------------------------- //

    /* The same rule Matches applies, against text rather than a stored blob --
     * so a passage shown as a hit is one that would have matched had it been
     * indexed alone. */
    private static bool TextMatches(string text, List<string> needles)
    {
        var tokens = Tokenize(text);
        if (tokens.Count == 0) return false;
        return Matches(" " + string.Join(" ", tokens) + " ", needles);
    }

    private static string Clock(double seconds)
    {
        if (seconds < 0) return "";
        var total = (int)seconds;
        int h = total / 3600, m = total % 3600 / 60, s = total % 60;
        return h > 0
            ? string.Create(CultureInfo.InvariantCulture, $"{h}:{m:D2}:{s:D2}")
            : string.Create(CultureInfo.InvariantCulture, $"{m}:{s:D2}");
    }

    /// <summary>
    /// Read the entry's own comments and captions and return the passages that
    /// match.
    /// </summary>
    /// <remarks>
    /// Deliberately NOT served from the index, which holds no text. This
    /// re-reads the video's files, which is affordable precisely because it is
    /// called for the handful of videos a query matched rather than for all of
    /// them. Runs on whichever thread calls it and should not be the UI one.
    /// </remarks>
    public static List<SearchSnippet> Snippets(ArchiveEntry entry, string query,
                                               SearchScope scope, int max = 5)
    {
        var outSnippets = new List<SearchSnippet>();
        var needles = Needles(query);
        if (needles.Count == 0 || max <= 0) return outSnippets;

        if (scope is SearchScope.Comments or SearchScope.Everything &&
            VideoInfo.Load(entry.Dir) is { } info)
        {
            void Walk(List<Comment> comments)
            {
                foreach (var c in comments)
                {
                    if (outSnippets.Count >= max) return;
                    if (TextMatches(c.Text, needles))
                    {
                        outSnippets.Add(new SearchSnippet(c.Text, c.Author, false));
                    }
                    if (c.Replies.Count > 0) Walk(c.Replies);
                }
            }
            Walk(info.Comments);
        }

        if (outSnippets.Count < max &&
            scope is SearchScope.Transcript or SearchScope.Everything &&
            BestSubtitlePath(entry) is { } sub)
        {
            /* Cues are glued into passages before matching: one cue is a few
             * words, so a two-word query almost never falls inside one, and
             * matching per cue would report "no transcript hits" for a video
             * whose transcript plainly contains the phrase. */
            var cues = Transcript.Cues(sub);
            var passage = new StringBuilder();
            var start = -1.0;

            for (var i = 0; i < cues.Count && outSnippets.Count < max; i++)
            {
                if (cues[i].Text.Length == 0) continue;
                if (start < 0) start = cues[i].Start;
                if (passage.Length > 0) passage.Append(' ');
                passage.Append(cues[i].Text);

                var last = i + 1 == cues.Count;
                if (passage.Length < TranscriptPassage && !last) continue;

                var text = passage.ToString();
                if (TextMatches(text, needles))
                {
                    outSnippets.Add(new SearchSnippet(text, Clock(start), true));
                }
                passage.Clear();
                start = -1;
            }
        }

        return outSnippets;
    }
}
