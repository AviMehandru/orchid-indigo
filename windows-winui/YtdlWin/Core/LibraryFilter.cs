/* Which videos the Library shows, and in what order.
 *
 * The C# half of a rule set three independent apps have to agree on. The C
 * version is linux-gtk/src/library_filter.{c,h} and the Swift one is
 * macos-swiftui/Sources/Core/LibraryFilter.swift; all three are asserted
 * against the SAME fixture values, which is the same mitigation the probe's
 * derivation uses and the only thing that makes three blind implementations of
 * one contract survivable.
 *
 * NOTHING HERE READS THE DISK. The index is already in memory, every field
 * this consults was populated by the scan, and TotalBytes sums the sizes the
 * scan recorded. That is what makes it safe to re-run the whole filter on
 * every keystroke.
 *
 * WHAT THE THREE APPS AGREE ON, AND WHAT THEY DO NOT. The rules are the
 * contract: which videos a facet admits, that an empty channel set means all
 * of them, that an unparseable date is excluded by a range rather than kept,
 * that a missing key sorts last in BOTH directions, and that the order is
 * total. The collation of two present titles is NOT: this uses .NET's
 * culture-aware comparison, the GTK app uses GLib's and the Swift app the
 * system's. That is deliberate rather than an oversight -- a German user's
 * Library should sort the way the rest of their desktop sorts, and three apps
 * forced to agree byte-for-byte would all have to be wrong somewhere to do it.
 * So the shared fixtures assert the RULES on inputs whose ordering every
 * reasonable collation agrees on, and never assert a specific collation.
 *
 * ONE WINDOWS-SPECIFIC NOTE. String.Compare with the current culture is the
 * right call for TITLES, which a person reads. It is the wrong call for the
 * eight-digit upload_date and for the archive-relative path, which are data:
 * those use Ordinal, because a culture-aware compare of "20240131" is a slower
 * way to get the same answer and, for the path, a way to get a different one.
 */

using System;
using System.Collections.Generic;
using System.Linq;

namespace YtdlWin.Core;

/// <summary>
/// The sort keys, in the order they are offered.
/// </summary>
/// <remarks>
/// Date first because it is what the grid was already implicitly ordered by
/// and what someone opening the app expects. Every key falls back to title,
/// then to the archive-relative path, so the order is TOTAL: two videos with
/// the same duration do not swap places between one rebuild and the next,
/// which is the kind of flicker that reads as a bug.
/// </remarks>
public enum SortKey
{
    Date = 0,
    Title,
    Channel,
    Duration,
    Size,
}

public static class SortKeys
{
    public static readonly SortKey[] All =
    {
        SortKey.Date, SortKey.Title, SortKey.Channel, SortKey.Duration, SortKey.Size,
    };

    /// <summary>A short label for the sort control.</summary>
    public static string Label(SortKey key) => key switch
    {
        SortKey.Title => "Title",
        SortKey.Channel => "Channel",
        SortKey.Duration => "Duration",
        SortKey.Size => "Size",
        _ => "Upload date",
    };

    /// <summary>
    /// The stable string a sort key is persisted as. Persisting the enum's
    /// NUMBER would mean inserting a key in the middle silently changes what a
    /// saved setting means.
    /// </summary>
    public static string Id(SortKey key) => key switch
    {
        SortKey.Title => "title",
        SortKey.Channel => "channel",
        SortKey.Duration => "duration",
        SortKey.Size => "size",
        _ => "date",
    };

    /// <summary>
    /// An id this build does not know is a setting written by a newer one.
    /// Falling back to the default is the same rule the profile store uses for
    /// an option it has never heard of: ignore it, do not refuse to start.
    /// </summary>
    public static SortKey FromId(string? id) => id switch
    {
        "title" => SortKey.Title,
        "channel" => SortKey.Channel,
        "duration" => SortKey.Duration,
        "size" => SortKey.Size,
        _ => SortKey.Date,
    };
}

/// <summary>The flag facets, as a bitmask.</summary>
/// <remarks>
/// All of them are AND-ed: asking for "audio only" and "no media file"
/// together is asking for something no folder can be, and it correctly shows
/// nothing rather than quietly becoming an OR.
///
/// VerifyFailed is different in kind from the other three and the difference
/// is worth stating, because it is the one a user can misread. The other three
/// are properties of the manifest and are known for every video the moment it
/// is indexed. Whether a folder still verifies is only knowable by hashing
/// every file in it, which takes seconds per video -- so this facet filters on
/// RECORDED results only, and a video that has never been verified is not
/// shown by it. It never claims an unverified video passes; it says "of the
/// ones I have checked, these failed". The UI has to say so too.
/// </remarks>
[Flags]
public enum FacetFlags
{
    None = 0,
    AudioOnly = 1 << 0,
    NoMedia = 1 << 1,
    LayoutTooNew = 1 << 2,
    VerifyFailed = 1 << 3,
}

public static class Facets
{
    public static readonly FacetFlags[] All =
    {
        FacetFlags.AudioOnly, FacetFlags.NoMedia,
        FacetFlags.LayoutTooNew, FacetFlags.VerifyFailed,
    };

    public static string Label(FacetFlags flag) => flag switch
    {
        FacetFlags.AudioOnly => "Audio only",
        FacetFlags.NoMedia => "No media file",
        FacetFlags.LayoutTooNew => "Newer archive layout",
        FacetFlags.VerifyFailed => "Failed verification",
        _ => "",
    };
}

/// <summary>What is known about a video's last checksum verification.</summary>
public enum VerifyState
{
    /// Never checked, or checked before the folder changed.
    Unknown = 0,
    Ok,
    Failed,
}

public sealed class LibraryFilter
{
    /// The substring search: title, uploader, id, channel, case-folded. Empty
    /// matches everything.
    public string Needle { get; set; } = "";

    /// Selected uploader folder names. EMPTY MEANS ALL, not none -- a facet
    /// nobody has touched must not hide the whole library.
    public HashSet<string> Channels { get; } = new(StringComparer.Ordinal);

    /// "YYYYMMDD" bounds, inclusive, either may be null. Compared as strings,
    /// which is correct for this format and is also why a folder whose
    /// UploadDate came from the folder-name fallback still sorts and filters
    /// sensibly: the fallback produces the same eight digits. An UploadDate
    /// that is not eight digits is EXCLUDED by any date bound rather than
    /// silently kept -- it is a video whose date is unknown, and claiming it
    /// falls inside a range would be inventing one.
    public string? DateFrom { get; set; }
    public string? DateTo { get; set; }

    public FacetFlags Flags { get; set; } = FacetFlags.None;

    /// <summary>
    /// When non-null, only videos whose key is in this set pass -- ANDed with
    /// everything else.
    /// </summary>
    /// <remarks>
    /// This is the result of a collection-wide comment or transcript search,
    /// which this class knows nothing about and must not, because the filter
    /// has to stay free of the disk and of the search index's build state. The
    /// caller is also what decides whether a scope's search is a union with
    /// the metadata match ("Everything") or a replacement for it ("Comments").
    ///
    /// Note the asymmetry with <see cref="Channels"/> and it is deliberate: an
    /// EMPTY set here means "a search ran and matched nothing", which must show
    /// nothing, while null means no search is running.
    /// </remarks>
    public HashSet<string>? KeyAllow { get; set; }

    public SortKey Sort { get; set; } = SortKey.Date;

    /// Newest first. The only default that is not a coin toss: an archive is
    /// added to at the newest end, so what someone wants to see when the
    /// window opens is what arrived last.
    public bool Descending { get; set; } = true;

    // ---------------------------------------------------------------- //
    // Narrowing                                                        //
    // ---------------------------------------------------------------- //

    /// <summary>
    /// Drop every facet and the needle. The SORT is deliberately left alone:
    /// it is a view preference, not a filter, and "Clear filters" throwing
    /// away someone's chosen ordering would be a surprise.
    /// </summary>
    public void Reset()
    {
        Needle = "";
        Channels.Clear();
        DateFrom = null;
        DateTo = null;
        Flags = FacetFlags.None;
        KeyAllow = null;
    }

    /// <summary>
    /// How many facets are set, for the count beside the filter control. The
    /// needle is not counted -- it has its own visible search box.
    /// </summary>
    public int FacetCount
    {
        get
        {
            var n = 0;
            if (Channels.Count > 0) n++;
            /* One date facet, not two: "2024 only" is a single idea the user
             * had, and counting it twice makes the count read as more
             * narrowing than it is. */
            if (DateFrom is not null || DateTo is not null) n++;
            foreach (var f in Facets.All)
            {
                if (Flags.HasFlag(f)) n++;
            }
            return n;
        }
    }

    /// <summary>
    /// True when anything at all is narrowing the library. Drives the "filters
    /// active" indicator, so it must NOT count the sort.
    /// </summary>
    /* KeyAllow counts: a collection-wide search narrows without putting
     * anything in the needle, so the empty state would otherwise say "there is
     * no archive here" when a comment search simply found nothing. */
    public bool IsNarrowing => Needle.Length > 0 || KeyAllow is not null || FacetCount > 0;

    public void SetChannel(string channel, bool on)
    {
        if (on) Channels.Add(channel);
        else Channels.Remove(channel);
    }

    // ---------------------------------------------------------------- //
    // Matching                                                         //
    // ---------------------------------------------------------------- //

    /// <summary>
    /// The eight-digit form the layout contract specifies, and the same form
    /// the folder-name fallback produces. Anything else is a date this reader
    /// does not have, which is not the same as a date outside the range.
    /// </summary>
    public static bool IsYyyyMmDd(string? s)
    {
        if (s is null || s.Length != 8) return false;
        foreach (var c in s)
        {
            if (c < '0' || c > '9') return false;
        }
        return true;
    }

    /// <summary>
    /// Whether the entry's title, uploader, id or channel contains
    /// <paramref name="needle"/>, case-folded.
    /// </summary>
    /// <remarks>
    /// The substring search the Library has always had, exposed because the
    /// "Everything" search scope has to union it with the index's hits and
    /// cannot do that from inside <see cref="Matches"/>. An empty needle
    /// matches everything, as it does there.
    /// </remarks>
    public static bool MetadataMatches(ArchiveEntry e, string needle)
    {
        if (needle.Length == 0) return true;
        foreach (var field in new[] { e.Title, e.Uploader, e.VideoId ?? "", e.Channel })
        {
            if (field.Length == 0) continue;
            if (field.Contains(needle, StringComparison.OrdinalIgnoreCase)) return true;
        }
        return false;
    }

    private bool MatchesNeedle(ArchiveEntry e)
    {
        if (Needle.Length == 0) return true;

        /* OrdinalIgnoreCase, which is what AppModel.FilteredEntries already
         * used before this filter existed and what its comment argues for:
         * same answer for the Latin text that is most of it, no allocation per
         * field per keystroke, and it does not depend on the machine's locale
         * for the answer -- searching "TITLE" on a Turkish machine should find
         * "title", and a culture-aware IgnoreCase there does not.
         *
         * This is the one place this port deliberately differs from the C and
         * Swift ones, which lowercase both sides. Carried over rather than
         * quietly "unified" when the search moved in here: it was a decision
         * with a reason attached, and the shared fixtures do not test a needle
         * where the two approaches disagree. */
        foreach (var field in new[] { e.Title, e.Uploader, e.VideoId ?? "", e.Channel })
        {
            if (field.Length == 0) continue;
            if (field.Contains(Needle, StringComparison.OrdinalIgnoreCase)) return true;
        }
        return false;
    }

    /// <summary>
    /// One entry against the filter. <paramref name="verify"/> may be null, in
    /// which case every video reads as Unknown and the verify facet matches
    /// nothing.
    /// </summary>
    public bool Matches(ArchiveEntry e, Func<ArchiveEntry, VerifyState>? verify = null)
    {
        if (!MatchesNeedle(e)) return false;

        /* Empty set means "every channel". The alternative -- empty means
         * none -- would make the library go blank the instant someone opened
         * the facet list and unticked the one channel they had ticked. */
        if (Channels.Count > 0 && !Channels.Contains(e.Channel)) return false;

        /* ANDed with everything else, and checked early because it is one hash
         * lookup and it is the narrowest thing in the filter when set. */
        if (KeyAllow is not null && !KeyAllow.Contains(e.Key)) return false;

        if (DateFrom is not null || DateTo is not null)
        {
            if (!IsYyyyMmDd(e.UploadDate)) return false;
            var d = e.UploadDate!;
            /* Ordinal, not culture-aware: this is an eight-digit token, not
             * prose, and its ordering must not depend on where the machine
             * is. */
            if (DateFrom is not null &&
                string.CompareOrdinal(d, DateFrom) < 0) return false;
            if (DateTo is not null &&
                string.CompareOrdinal(d, DateTo) > 0) return false;
        }

        if (Flags.HasFlag(FacetFlags.AudioOnly) && !e.IsAudioOnly) return false;
        if (Flags.HasFlag(FacetFlags.NoMedia) && e.MediaIndex >= 0) return false;
        if (Flags.HasFlag(FacetFlags.LayoutTooNew) && !e.LayoutTooNew) return false;

        if (Flags.HasFlag(FacetFlags.VerifyFailed))
        {
            /* Unknown is not a match. A video nobody has verified has not
             * passed and has not failed, and showing it here would turn "these
             * are broken" into "these might be broken", which is a different
             * and much less useful claim. */
            var state = verify?.Invoke(e) ?? VerifyState.Unknown;
            if (state != VerifyState.Failed) return false;
        }

        return true;
    }

    // ---------------------------------------------------------------- //
    // Sorting                                                          //
    // ---------------------------------------------------------------- //

    private static bool HasText(string? s) => !string.IsNullOrEmpty(s);

    /// <summary>
    /// The sort field a key reads, as "is it there at all". Kept separate from
    /// the comparison because absence must NOT be reversed along with the
    /// direction -- see <see cref="Compare"/>.
    /// </summary>
    private bool KeyIsPresent(ArchiveEntry e) => Sort switch
    {
        SortKey.Date => HasText(e.UploadDate),
        SortKey.Title => HasText(e.Title),
        SortKey.Channel => HasText(e.Uploader) || HasText(e.Channel),
        /* No info.json, or one with no duration, reads as <= 0. That is "not
         * known", not "a zero-second video" -- there is no such thing in an
         * archive. */
        SortKey.Duration => e.Duration > 0,
        SortKey.Size => e.TotalBytes > 0,
        _ => true,
    };

    /// Culture-aware, for text a person reads.
    private static int CompareText(string? a, string? b) =>
        string.Compare(a ?? "", b ?? "", StringComparison.CurrentCulture);

    /// <summary>
    /// The comparison a given key implies, with <see cref="Descending"/>
    /// already applied. Public because sorting is the half of this file where
    /// an error is invisible in a screenshot and obvious in an assertion.
    /// </summary>
    public int Compare(ArchiveEntry a, ArchiveEntry b)
    {
        /* ABSENCE IS DECIDED BEFORE THE DIRECTION FLIP, and this is the single
         * subtlest thing in the file. A video with no upload date is missing
         * information, and information that is missing belongs at the BOTTOM
         * of the list in both directions. Folding that into the comparison and
         * then negating the result for a descending sort flips it too, so
         * pressing the sort-direction control fills the first screen with
         * blank cards -- which reads as a rendering bug, not as an ordering
         * choice. Caught by a test rather than by reading this code. */
        var aHas = KeyIsPresent(a);
        var bHas = KeyIsPresent(b);
        if (aHas != bHas) return aHas ? -1 : 1;

        var r = 0;
        if (aHas)
        {
            r = Sort switch
            {
                /* Ordinal for the eight-digit date: it is already
                 * lexicographically ordered, and an entry whose date came from
                 * the folder-name fallback is in the same form. */
                SortKey.Date => string.CompareOrdinal(a.UploadDate ?? "", b.UploadDate ?? ""),
                SortKey.Title => CompareText(a.Title, b.Title),
                SortKey.Channel => CompareText(
                    a.Uploader.Length > 0 ? a.Uploader : a.Channel,
                    b.Uploader.Length > 0 ? b.Uploader : b.Channel),
                SortKey.Duration => a.Duration.CompareTo(b.Duration),
                SortKey.Size => a.TotalBytes.CompareTo(b.TotalBytes),
                _ => 0,
            };
        }

        if (Descending) r = -r;

        /* The tie-breakers are NOT reversed with the direction, and that is
         * the point of them: they exist to make the order TOTAL so the grid
         * does not reshuffle equal-keyed videos between rebuilds. Title first
         * because it is what a person would expect to see grouped; then the
         * archive-relative path, which is unique by construction, so the
         * comparison can never return 0 for two different videos. Ordinal for
         * the path, which is data rather than prose. */
        if (r == 0) r = CompareText(a.Title, b.Title);
        if (r == 0) r = string.CompareOrdinal(a.Rel, b.Rel);
        return r;
    }

    /// <summary>Filter and sort a whole index in one pass.</summary>
    public List<ArchiveEntry> Apply(IEnumerable<ArchiveEntry> entries,
                                    Func<ArchiveEntry, VerifyState>? verify = null)
    {
        var kept = entries.Where(e => Matches(e, verify)).ToList();
        /* List.Sort is unstable, which does not matter here and must not be
         * relied on either way: Compare never returns 0 for two different
         * entries, so the result is the same whatever the algorithm does with
         * ties. */
        kept.Sort(Compare);
        return kept;
    }
}
