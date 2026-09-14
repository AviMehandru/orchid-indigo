/* Finding out what a URL is before downloading it.
 *
 * The Downloads form used to know nothing about the URL in it until the run
 * failed. The Quality list was a fixed ladder from 2160p down, the codec and
 * container lists were fixed too, and asking for 1440p AV1 was a request that
 * silently resolved to something else -- discovered, at the earliest, in the
 * Library. This file is the answer to that.
 *
 * WHERE THE ANSWER COMES FROM, and why there are two ways.
 *
 * The pipeline owns a `ytdl <url> --probe` mode that prints one JSON document
 * described by docs/probe-contract.md in orchid-ochre. That is the preferred
 * path and the one normally taken, because the pipeline's copy of the
 * derivation is the authority: it maps yt-dlp's codec spellings onto the
 * --codec vocabulary and works out which --container values a merge could
 * actually produce, under the same PO token provider a real download uses.
 *
 * The fallback exists because this app is pinned to a pipeline ref
 * (CLI_VERSION) that a user's machine may predate. An installed ytdl.ps1
 * without --probe answers with a usage error, and the preview would simply
 * never work -- against a pipeline that downloads perfectly well. So when the
 * pipeline probe fails in the specific way that means "this pipeline is older
 * than the feature", this file runs `yt-dlp -J` itself and derives the same
 * answers here.
 *
 * THE SECOND COPY IS THE COST AND IT IS PAID ON PURPOSE. Two derivations that
 * disagree would be worse than no preview: one pipeline version would offer
 * MP4 where another did not, for the same video, with nothing saying why. So
 * ParseContract and FromYtDlp are held to the same fixture in
 * YtdlWin.Tests/ProbeTests.cs and asserted to produce identical heights, codec
 * lists and container lists -- the same obligation the GTK app carries in
 * tests/test_url_probe.c and the SwiftUI app in Tests/UrlProbeTests.swift, in
 * three languages, for the same reason the archive layout has a conformance
 * suite in each.
 *
 * Like everything else under Core/, this has NO Microsoft.UI dependency: it
 * compiles and is tested without a window, a UI thread or the Windows App SDK.
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

/// <summary>One rendition.</summary>
public sealed class ProbeFormat
{
    public string FormatId { get; set; } = "";
    public string Ext { get; set; } = "";

    /// <summary>yt-dlp's own spelling, verbatim. "none" where absent.</summary>
    public string VCodec { get; set; } = "";
    public string ACodec { get; set; } = "";

    /* This pipeline's vocabulary, or null when the codec has no --codec
     * spelling at all. Null is NOT "any": "any" is a choice the user makes,
     * and reporting it for a VP8 rendition would be a lie about what was
     * asked for. */
    public string? VideoFamily { get; set; }
    public string? AudioFamily { get; set; }

    public int Height { get; set; }
    public int Width { get; set; }
    public double Fps { get; set; }
    public double Tbr { get; set; }

    /// <summary>Exact, and usually absent.</summary>
    public long Filesize { get; set; }

    /* yt-dlp's estimate. Kept apart from Filesize on purpose: the UI shows
     * "421 MB" differently from "~421 MB", and merging them presents a guess
     * as a fact. */
    public long FilesizeApprox { get; set; }

    public string DynamicRange { get; set; } = "";
    public string FormatNote { get; set; } = "";

    public bool HasVideoStream =>
        !string.IsNullOrEmpty(VCodec) && VCodec != "none";

    public bool HasAudioStream =>
        !string.IsNullOrEmpty(ACodec) && ACodec != "none";
}

/// <summary>One playlist entry.</summary>
public sealed class ProbeEntry
{
    /* The 1-BASED POSITION IN THE PLAYLIST, which is what --playlist-items
     * counts and therefore the only number that may be written back into
     * --items. Carried explicitly rather than taken from list position,
     * because the list is filtered and truncated in the UI and a position
     * derived from the visible order queues the wrong videos -- a bug whose
     * first symptom is a successful download of something nobody asked for. */
    public int Index { get; set; }

    public string VideoId { get; set; } = "";
    public string Title { get; set; } = "";
    public string Uploader { get; set; } = "";
    public string Url { get; set; } = "";
    public string Thumbnail { get; set; } = "";
    public double Duration { get; set; }
}

public sealed class Probe
{
    /* The contract version this app knows how to read. Compared per document
     * rather than enforced at startup, the same way the archive layout version
     * is compared per video. */
    public const int SupportedVersion = 1;

    public int ProbeVersion { get; set; }

    /// <summary>"video" or "playlist".</summary>
    public string Kind { get; set; } = "video";

    public string Url { get; set; } = "";
    public string Id { get; set; } = "";
    public string Title { get; set; } = "";
    public string Uploader { get; set; } = "";
    public string Channel { get; set; } = "";
    public string ChannelUrl { get; set; } = "";
    public string Extractor { get; set; } = "";
    public string Thumbnail { get; set; } = "";
    public string Description { get; set; } = "";

    /// <summary>YYYYMMDD.</summary>
    public string UploadDate { get; set; } = "";

    public string LiveStatus { get; set; } = "";
    public string Availability { get; set; } = "";
    public string WebpageUrl { get; set; } = "";

    public double Duration { get; set; }
    public long ViewCount { get; set; }
    public long LikeCount { get; set; }
    public long CommentCount { get; set; }
    public int AgeLimit { get; set; }

    // Playlists
    public int EntryCount { get; set; }

    /// <summary>What the extractor says the WHOLE list holds.</summary>
    public int PlaylistCount { get; set; }

    public bool EntriesTruncated { get; set; }
    public List<ProbeEntry> Entries { get; } = new();
    public string FormatsFromId { get; set; } = "";
    public string FormatsFromTitle { get; set; } = "";

    // Formats and the lists derived from them
    public List<ProbeFormat> Formats { get; } = new();

    /// <summary>Descending, distinct.</summary>
    public List<int> Heights { get; set; } = new();

    public List<string> VideoCodecs { get; set; } = new();
    public List<string> AudioCodecs { get; set; } = new();
    public List<string> Containers { get; set; } = new();
    public List<string> SubtitleLangs { get; set; } = new();
    public bool HasVideo { get; set; }
    public bool HasAudio { get; set; }

    public bool PotHealthy { get; set; }
    public string PotReason { get; set; } = "";
    public string PotNote { get; set; } = "";

    /* True when this came from the app's own yt-dlp call rather than from
     * `ytdl --probe`. Surfaced in the UI, because the two can legitimately
     * differ: the fallback does not bring up the PO token provider, so its
     * format table can be the thinner one. */
    public bool FromFallback { get; set; }

    public bool IsPlaylist => Kind == "playlist";

    // ------------------------------------------------------------------
    // Codec families
    // ------------------------------------------------------------------

    /* The single most confusable part of this contract. yt-dlp spells VP9 as
     * "vp09" and AV1 as "av01" with a ZERO -- it never emits "av1" -- and it
     * calls AAC "mp4a". A codebase that tests only for "vp9" reports the most
     * common codec on YouTube as having no --codec spelling at all, which
     * shows up as a Video codec list missing its most common entry, on every
     * video. */
    public static string? VideoFamilyOf(string? vcodec)
    {
        if (string.IsNullOrEmpty(vcodec) || vcodec == "none") return null;
        var c = vcodec!.ToLowerInvariant();
        if (c.StartsWith("avc1", StringComparison.Ordinal)
            || c.StartsWith("h264", StringComparison.Ordinal)) return "avc1";
        if (c.StartsWith("vp09", StringComparison.Ordinal)
            || c.StartsWith("vp9", StringComparison.Ordinal)) return "vp9";
        if (c.StartsWith("av01", StringComparison.Ordinal)) return "av01";
        return null;
    }

    public static string? AudioFamilyOf(string? acodec)
    {
        if (string.IsNullOrEmpty(acodec) || acodec == "none") return null;
        var c = acodec!.ToLowerInvariant();
        if (c.StartsWith("opus", StringComparison.Ordinal)) return "opus";
        if (c.StartsWith("mp4a", StringComparison.Ordinal)
            || c.StartsWith("aac", StringComparison.Ordinal)) return "aac";
        if (c.StartsWith("mp3", StringComparison.Ordinal)) return "mp3";
        if (c.StartsWith("flac", StringComparison.Ordinal)) return "flac";
        return null;
    }

    /* Canonical order, NOT offer order. yt-dlp's format ordering varies with
     * client and with its own sorting changes, and a dropdown whose entries
     * reshuffle between two probes of the same video looks broken. */
    public static readonly string[] VideoFamilyOrder = { "avc1", "vp9", "av01" };
    public static readonly string[] AudioFamilyOrder = { "opus", "aac", "mp3", "flac" };

    // ------------------------------------------------------------------
    // Derivation
    // ------------------------------------------------------------------

    /* Everything computed FROM the format list rather than read out of it.
     *
     * This must agree, value for value, with Get-FormatSummary in the
     * pipeline's probe.ps1, derive_lists() in the GTK app's url_probe.c and
     * deriveLists() in the SwiftUI app's UrlProbe.swift. When the pipeline's
     * own document is what was parsed this is not called at all: the derived
     * arrays come from the document, because the pipeline is the authority. It
     * runs only for the fallback path. */
    public void DeriveLists()
    {
        /* "Does this format carry video" and "which --codec value is it" are
         * two different questions, and conflating them loses resolutions. A
         * VP8 rendition has a real height the user can ask for and no --codec
         * spelling at all, so the heights come from the first question and the
         * codec lists from the second. */
        HasVideo = Formats.Any(f => f.HasVideoStream);
        HasAudio = Formats.Any(f => f.HasAudioStream);

        Heights = Formats
            .Where(f => f.HasVideoStream && f.Height > 0)
            .Select(f => f.Height)
            .Distinct()
            .OrderByDescending(h => h)
            .ToList();

        VideoCodecs = VideoFamilyOrder
            .Where(fam => Formats.Any(f => f.VideoFamily == fam))
            .ToList();
        AudioCodecs = AudioFamilyOrder
            .Where(fam => Formats.Any(f => f.AudioFamily == fam))
            .ToList();
        Containers = ContainersFor(VideoCodecs, AudioCodecs);
    }

    /* Which --container values a merge could actually produce.
     *
     * mkv is unconditional: Matroska carries every codec pair YouTube serves,
     * which is why it is this pipeline's default and its archival choice.
     *
     * The other two are real constraints, not preferences, and getting them
     * wrong is the specific failure this whole file exists to prevent --
     * yt-dlp cannot mux Opus into mp4 or AAC into webm, and asking it to
     * produces either a re-encode or a failed merge depending on version. A
     * list offering one that cannot be made is a mistake the user only
     * discovers afterwards. */
    public static List<string> ContainersFor(
        IReadOnlyCollection<string> video, IReadOnlyCollection<string> audio)
    {
        var outList = new List<string> { "mkv" };
        var mp4Video = video.Contains("avc1") || video.Contains("av01");
        var webmVideo = video.Contains("vp9") || video.Contains("av01");
        if (mp4Video && audio.Contains("aac")) outList.Add("mp4");
        if (webmVideo && audio.Contains("opus")) outList.Add("webm");
        return outList;
    }

    /// <summary>
    /// The heights this video offers for <paramref name="family"/> ("any" or
    /// null means all of them), descending. This is the cross-filter the
    /// Quality list needs: 1440p commonly exists only in VP9, so a list built
    /// from the union of heights offers a combination the video does not have.
    /// </summary>
    public List<int> HeightsForCodec(string? family)
    {
        var all = string.IsNullOrEmpty(family) || family == "any";
        return Formats
            .Where(f => f.HasVideoStream && f.Height > 0)
            .Where(f => all || f.VideoFamily == family)
            .Select(f => f.Height)
            .Distinct()
            .OrderByDescending(h => h)
            .ToList();
    }

    // ------------------------------------------------------------------
    // Parsing
    // ------------------------------------------------------------------

    /* Read member by member through Json.cs's tolerant accessors rather than
     * by deserialising into a type. A schema's failure mode against output
     * like this is that ONE unexpected member fails the whole document. */
    private static ProbeFormat ReadFormat(JsonElement o, bool derived)
    {
        var f = new ProbeFormat
        {
            FormatId = o.Str("format_id") ?? "",
            Ext = o.Str("ext") ?? "",
            VCodec = o.Str("vcodec") ?? "",
            ACodec = o.Str("acodec") ?? "",
            Height = (int)o.Int("height"),
            Width = (int)o.Int("width"),
            Fps = o.Double("fps"),
            Tbr = o.Double("tbr"),
            Filesize = o.Int("filesize"),
            FilesizeApprox = o.Int("filesize_approx"),
            DynamicRange = o.Str("dynamic_range") ?? "",
            FormatNote = o.Str("format_note") ?? "",
        };

        if (derived)
        {
            f.VideoFamily = o.Str("video_family");
            f.AudioFamily = o.Str("audio_family");
        }
        else
        {
            f.VideoFamily = VideoFamilyOf(f.VCodec);
            f.AudioFamily = AudioFamilyOf(f.ACodec);
        }
        return f;
    }

    private static void ReadFormats(Probe p, JsonElement o, bool derived)
    {
        foreach (var raw in o.Objects("formats"))
        {
            /* Storyboards. yt-dlp's .mhtml pseudo-formats carry no streams and
             * are not a rendition of anything; left in, each becomes a phantom
             * row and a phantom height. The pipeline drops them too, so this
             * only ever fires on the fallback path -- but it fires there
             * identically, which is the point. */
            if (raw.Str("ext") == "mhtml") continue;

            var f = ReadFormat(raw, derived);
            if (!f.HasVideoStream && !f.HasAudioStream) continue;
            p.Formats.Add(f);
        }
    }

    /// <summary>
    /// Read a probe-contract document, or null when it does not parse.
    /// </summary>
    public static Probe? ParseContract(string? text)
    {
        if (string.IsNullOrWhiteSpace(text)) return null;
        var root = JsonFile.ObjectFrom(text!.Trim());
        if (root == null) return null;
        var o = root.Value;

        var p = new Probe
        {
            ProbeVersion = (int)o.Int("probe_version"),
            Kind = o.Str("kind") ?? "video",
            Url = o.Str("url") ?? "",
            Id = o.Str("id") ?? "",
            Title = o.Str("title") ?? "",
            Uploader = o.Str("uploader") ?? "",
            Channel = o.Str("channel") ?? "",
            ChannelUrl = o.Str("channel_url") ?? "",
            Extractor = o.Str("extractor") ?? "",
            Thumbnail = o.Str("thumbnail") ?? "",
            Description = o.Str("description") ?? "",
            UploadDate = o.Str("upload_date") ?? "",
            LiveStatus = o.Str("live_status") ?? "",
            Availability = o.Str("availability") ?? "",
            WebpageUrl = o.Str("webpage_url") ?? "",
            FormatsFromId = o.Str("formats_from_id") ?? "",
            FormatsFromTitle = o.Str("formats_from_title") ?? "",
            Duration = o.Double("duration"),
            ViewCount = o.Int("view_count"),
            LikeCount = o.Int("like_count"),
            CommentCount = o.Int("comment_count"),
            AgeLimit = (int)o.Int("age_limit"),
            EntryCount = (int)o.Int("entry_count"),
            PlaylistCount = (int)o.Int("playlist_count"),
            EntriesTruncated = o.Bool("entries_truncated"),
        };

        var index = 0;
        foreach (var raw in o.Objects("entries"))
        {
            index++;
            var e = new ProbeEntry
            {
                Index = (int)raw.Int("index"),
                VideoId = raw.Str("id") ?? "",
                Title = raw.Str("title") ?? "",
                Uploader = raw.Str("uploader") ?? "",
                Url = raw.Str("url") ?? "",
                Thumbnail = raw.Str("thumbnail") ?? "",
                Duration = raw.Double("duration"),
            };
            /* A document from a pipeline that somehow omitted the index still
             * produces a usable list rather than a list of zeroes that would
             * all write the same --items value. */
            if (e.Index <= 0) e.Index = index;
            p.Entries.Add(e);
        }

        ReadFormats(p, o, derived: true);

        /* The derived lists are READ, not recomputed. The pipeline is the
         * authority on them: it made them under the PO token provider a real
         * download will use, and recomputing here would mean this app could
         * disagree with the command it is about to run. */
        p.VideoCodecs = o.Strings("video_codecs");
        p.AudioCodecs = o.Strings("audio_codecs");
        p.Containers = o.Strings("containers");
        p.SubtitleLangs = o.Strings("subtitle_langs");
        p.HasVideo = o.Bool("has_video");
        p.HasAudio = o.Bool("has_audio");
        p.Heights = IntArray(o, "heights");

        var pot = o.Obj("pot");
        if (pot != null)
        {
            p.PotHealthy = pot.Bool("healthy");
            p.PotReason = pot.Str("reason") ?? "";
            p.PotNote = pot.Str("note") ?? "";
        }

        /* A document with no containers at all is one from a pipeline whose
         * derivation failed, or a truncated read. Falling back to the local
         * derivation beats a Container list with nothing in it. */
        if (p.Containers.Count == 0) p.DeriveLists();
        return p;
    }

    /* Json.cs has Strings() for a string array and nothing for a number one,
     * because until now nothing needed one. Kept local rather than added
     * there: this is the only caller, and a shared helper that one file uses
     * is a worse place to look for it. */
    private static List<int> IntArray(JsonElement o, string key)
    {
        var outList = new List<int>();
        if (!o.TryGetProperty(key, out var arr)) return outList;
        if (arr.ValueKind != JsonValueKind.Array) return outList;
        foreach (var item in arr.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.Number) continue;
            if (item.TryGetInt32(out var v) && v > 0) outList.Add(v);
        }
        return outList;
    }

    /// <summary>
    /// The fallback. <paramref name="flat"/> is
    /// <c>yt-dlp -J --flat-playlist</c> output and may be null for a single
    /// video; <paramref name="full"/> is <c>yt-dlp -J</c> output for the video
    /// whose formats should be reported. Does here, in C#, exactly what
    /// probe.ps1 does in PowerShell.
    /// </summary>
    public static Probe? FromYtDlp(string? flat, string? full)
    {
        if (string.IsNullOrWhiteSpace(full)) return null;
        var fullRoot = JsonFile.ObjectFrom(full!.Trim());
        if (fullRoot == null) return null;
        var fo = fullRoot.Value;

        var p = new Probe
        {
            ProbeVersion = SupportedVersion,
            FromFallback = true,
            /* The fallback never brings up the PO token provider -- doing so
             * would mean reimplementing pot-provider.ps1 in C#, which is
             * exactly the duplication this app exists to avoid. So it says so,
             * rather than leaving the user to wonder why the format table is
             * thinner than the one they saw yesterday. */
            PotHealthy = false,
            PotReason = "this app read yt-dlp directly, without the pipeline's "
                + "PO token provider",
            PotNote = "No PO token provider: a real download may see formats "
                + "this list does not show.",
        };

        JsonElement? flatRoot = null;
        if (!string.IsNullOrWhiteSpace(flat))
            flatRoot = JsonFile.ObjectFrom(flat!.Trim());

        var type = flatRoot.Str("_type") ?? "";
        var isPlaylist = type == "playlist" || type == "multi_video";
        p.Kind = isPlaylist ? "playlist" : "video";

        if (isPlaylist && flatRoot != null)
        {
            var lo = flatRoot.Value;
            p.Id = lo.Str("id") ?? "";
            p.Title = lo.Str("title") ?? "";
            p.Uploader = lo.Str("uploader") ?? lo.Str("channel") ?? "";
            p.Channel = lo.Str("channel") ?? "";
            p.Extractor = lo.Str("extractor_key") ?? "";
            p.PlaylistCount = (int)lo.Int("playlist_count");

            var index = 0;
            foreach (var raw in lo.Objects("entries"))
            {
                index++;
                p.Entries.Add(new ProbeEntry
                {
                    Index = index,
                    VideoId = raw.Str("id") ?? "",
                    Title = raw.Str("title") ?? "",
                    Uploader = raw.Str("uploader") ?? raw.Str("channel") ?? "",
                    Url = raw.Str("url") ?? raw.Str("webpage_url") ?? "",
                    Duration = raw.Double("duration"),
                });
            }
            p.EntryCount = p.Entries.Count;
            p.FormatsFromId = fo.Str("id") ?? "";
            p.FormatsFromTitle = fo.Str("title") ?? "";
            p.Thumbnail = fo.Str("thumbnail") ?? "";
            p.Duration = fo.Double("duration");
        }
        else
        {
            p.Id = fo.Str("id") ?? "";
            p.Title = fo.Str("title") ?? "";
            p.Uploader = fo.Str("uploader") ?? fo.Str("channel") ?? "";
            p.Channel = fo.Str("channel") ?? "";
            p.ChannelUrl = fo.Str("channel_url") ?? "";
            p.Extractor = fo.Str("extractor_key") ?? "";
            p.Duration = fo.Double("duration");
            p.UploadDate = fo.Str("upload_date") ?? "";
            p.ViewCount = fo.Int("view_count");
            p.LikeCount = fo.Int("like_count");
            p.CommentCount = fo.Int("comment_count");
            p.LiveStatus = fo.Str("live_status") ?? "";
            p.Availability = fo.Str("availability") ?? "";
            p.AgeLimit = (int)fo.Int("age_limit");
            p.Thumbnail = fo.Str("thumbnail") ?? "";
            p.Description = fo.Str("description") ?? "";
            p.WebpageUrl = fo.Str("webpage_url") ?? "";
            /* Reported but not selectable: the pipeline's conf hardcodes en.*
             * and changing that is a pipeline change, not a GUI one. A picker
             * here would imply a choice that does not exist yet. */
            p.SubtitleLangs = ObjectKeys(fo, "subtitles");
            p.EntryCount = 1;
        }

        ReadFormats(p, fo, derived: false);
        p.DeriveLists();
        return p;
    }

    private static List<string> ObjectKeys(JsonElement o, string key)
    {
        var outList = new List<string>();
        if (!o.TryGetProperty(key, out var inner)) return outList;
        if (inner.ValueKind != JsonValueKind.Object) return outList;
        foreach (var prop in inner.EnumerateObject()) outList.Add(prop.Name);
        return outList;
    }
}

// ----------------------------------------------------------------------
// --items ranges
// ----------------------------------------------------------------------

public static class ItemsRange
{
    /// <summary>
    /// Compact a set of 1-based playlist positions into --items syntax:
    /// {1,2,3,7,10,11,12} becomes "1-3,7,10-12". Need not be sorted.
    /// </summary>
    /* Compacted rather than emitted as a comma list because a channel
     * selection of 400 videos would otherwise produce a command line thousands
     * of characters long -- unreadable in the command preview, and on Windows
     * close enough to a real command-line limit to matter. */
    public static string Compact(IEnumerable<int> indices)
    {
        /* Duplicates are absorbed rather than rejected: two ticked rows cannot
         * produce the same index, but a range parsed back in from a saved
         * profile can overlap itself, and "1-3,2-4" is a legal thing for a
         * human to have typed. */
        var sorted = indices.Where(v => v > 0).Distinct().OrderBy(v => v).ToList();
        if (sorted.Count == 0) return "";

        var parts = new List<string>();
        var i = 0;
        while (i < sorted.Count)
        {
            var start = sorted[i];
            var end = start;
            var j = i + 1;
            while (j < sorted.Count && (sorted[j] == end || sorted[j] == end + 1))
            {
                end = sorted[j];
                j++;
            }
            if (start == end)
            {
                parts.Add(start.ToString(CultureInfo.InvariantCulture));
            }
            else if (end == start + 1)
            {
                /* "1,2" rather than "1-2": the same length, and a two-element
                 * range written as a range reads like it might be
                 * open-ended. */
                parts.Add(string.Format(CultureInfo.InvariantCulture, "{0},{1}", start, end));
            }
            else
            {
                parts.Add(string.Format(CultureInfo.InvariantCulture, "{0}-{1}", start, end));
            }
            i = j;
        }
        return string.Join(",", parts);
    }

    /// <summary>
    /// The inverse: parse an --items range back into the positions it names,
    /// so a profile or a re-opened form can tick the right boxes.
    /// </summary>
    /* Anything unparseable is skipped rather than refused -- ytdl.ps1 is the
     * validator, and a range this cannot read is still one the pipeline may
     * accept. Parsed with InvariantCulture throughout: .NET parses against
     * CurrentCulture by default, which is the same trap the frame-rate parsing
     * in this app already documents. */
    public static List<int> Parse(string? spec)
    {
        var outList = new List<int>();
        if (string.IsNullOrWhiteSpace(spec)) return outList;

        foreach (var rawPart in spec!.Split(','))
        {
            var part = rawPart.Trim();
            if (part.Length == 0) continue;

            var dash = part.IndexOf('-');
            if (dash < 0)
            {
                if (int.TryParse(part, NumberStyles.Integer,
                                 CultureInfo.InvariantCulture, out var v) && v > 0)
                    outList.Add(v);
                continue;
            }

            /* yt-dlp's own open-ended forms -- "5-" and "-10" -- are
             * deliberately NOT expanded to a guessed bound. A tick list built
             * from a guess would show a selection the pipeline may not agree
             * with, and ytdl.ps1 is the validator here, not this. They parse to
             * nothing and the range stays in the text field as typed. */
            if (dash == 0 || dash == part.Length - 1) continue;

            if (!int.TryParse(part.Substring(0, dash), NumberStyles.Integer,
                              CultureInfo.InvariantCulture, out var lo)) continue;
            if (!int.TryParse(part.Substring(dash + 1), NumberStyles.Integer,
                              CultureInfo.InvariantCulture, out var hi)) continue;
            if (lo <= 0 || hi < lo) continue;
            /* A bound on how much a malformed range can allocate. 100,000 is
             * far past any real playlist and far short of a memory problem. */
            if (hi - lo > 100000) continue;

            for (var v = lo; v <= hi; v++) outList.Add(v);
        }
        return outList;
    }
}

// ----------------------------------------------------------------------
// Running one
// ----------------------------------------------------------------------

public sealed class ProbeRequest
{
    public string Url { get; set; } = "";
    public string Items { get; set; } = "";
    public bool NoPot { get; set; }
    public int PotPort { get; set; }

    /// <summary>
    /// The Advanced box, one argument per element, so a URL that needs
    /// --cookies-from-browser can be probed at all.
    /// </summary>
    public List<string> ExtraArgs { get; set; } = new();
}

public sealed class ProbeResult
{
    public Probe? Probe { get; set; }
    public string Error { get; set; } = "";
    public bool Cancelled { get; set; }
    public bool Ok => Probe != null;
}

public static class ProbeRunner
{
    /* Does this failure mean "the installed pipeline is older than --probe"?
     *
     * Narrow on purpose. Falling back on ANY failure would turn "this video is
     * private" into a second, slower attempt that also fails, and would hide a
     * genuinely broken pipeline behind a path that happens to work. The two
     * signals that actually mean "too old" are ytdl.ps1's own unknown-option
     * error and a missing probe.ps1. */
    public static bool MeansPipelineTooOld(string? stderr)
    {
        if (string.IsNullOrEmpty(stderr)) return false;
        return stderr!.Contains("Unknown option: --probe", StringComparison.Ordinal)
            || stderr.Contains("probe.ps1", StringComparison.Ordinal);
    }

    /// <summary>
    /// The first non-empty line, or null. yt-dlp's and ytdl.ps1's useful
    /// sentence is not always the last thing printed once the pipeline's own
    /// notes are in the stream.
    /// </summary>
    public static string? FirstLine(string? text)
    {
        if (string.IsNullOrEmpty(text)) return null;
        foreach (var raw in text!.Split('\n'))
        {
            var line = raw.Trim();
            if (line.Length > 0) return line;
        }
        return null;
    }

    private sealed class Capture
    {
        public string StdOut = "";
        public string StdErr = "";
        public int ExitCode = -1;
    }

    /* One child, stdout and stderr captured SEPARATELY.
     *
     * Separately, not merged, and that is the whole reason a probe can be told
     * from a failure at all: the contract is one JSON document on stdout and
     * everything conversational on stderr, so merging them would put the
     * pipeline's own "[pot] ..." notes inside the string about to be parsed.
     *
     * No job object, unlike Runner: a probe's children are short-lived, write
     * nothing and spawn no grandchildren worth chasing, so the whole
     * ProcessTree apparatus would be cost without a reason. */
    private static async Task<Capture?> CaptureAsync(
        string exe, IReadOnlyList<string> args, CancellationToken token)
    {
        var psi = new ProcessStartInfo
        {
            FileName = exe,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
            UseShellExecute = false,
            CreateNoWindow = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (var a in args) psi.ArgumentList.Add(a);
        psi.Environment["PATH"] = Paths.ChildPath();

        using var process = new Process { StartInfo = psi };
        try
        {
            if (!process.Start()) return null;
        }
        catch (Exception)
        {
            return null;
        }

        /* stdin closed rather than inherited: a GUI has no terminal to answer
         * with, and leaving it open lets yt-dlp block forever waiting on
         * one. */
        process.StandardInput.Close();

        /* Both streams read concurrently. A format table for a long video is
         * well past a pipe buffer, and reading one to the end before starting
         * the other deadlocks as soon as the unread one fills. */
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
            return null;
        }

        var stdout = await outTask.ConfigureAwait(false);
        var stderr = await errTask.ConfigureAwait(false);
        return new Capture
        {
            StdOut = stdout,
            StdErr = stderr,
            ExitCode = process.ExitCode,
        };
    }

    public static async Task<ProbeResult> RunAsync(
        ProbeRequest request, CancellationToken token)
    {
        var (result, tooOld) = await ViaPipelineAsync(request, token)
            .ConfigureAwait(false);
        if (result.Ok) return result;
        if (token.IsCancellationRequested)
            return new ProbeResult { Cancelled = true };

        /* A pipeline that HAS --probe and failed has told us something real --
         * the video is private, the URL is wrong, cookies are needed. Trying
         * again without the pipeline would replace that sentence with a worse
         * one from a different program. */
        if (!tooOld) return result;

        return await ViaYtDlpAsync(request, token).ConfigureAwait(false);
    }

    private static async Task<(ProbeResult, bool)> ViaPipelineAsync(
        ProbeRequest request, CancellationToken token)
    {
        var pwsh = Paths.FindPwsh();
        var script = pwsh == null ? null : Paths.Join(Paths.ScriptsDir(), "ytdl.ps1");
        if (pwsh == null || script == null || !Paths.IsRegularFile(script))
        {
            return (new ProbeResult
            {
                Error = "The installed pipeline was not found.",
            }, true);
        }

        var args = new List<string>
        {
            "-NoProfile", "-File", script,
            RunOptions.NormalizeUrl(request.Url), "--probe",
        };
        if (!string.IsNullOrWhiteSpace(request.Items))
        {
            args.Add("--items");
            args.Add(request.Items.Trim());
        }
        if (request.NoPot) args.Add("--no-pot");
        if (request.PotPort > 0)
        {
            args.Add("--pot-port");
            args.Add(request.PotPort.ToString(CultureInfo.InvariantCulture));
        }
        foreach (var a in request.ExtraArgs)
        {
            args.Add("--ytdlp-arg");
            args.Add(a);
        }

        var cap = await CaptureAsync(pwsh, args, token).ConfigureAwait(false);
        if (cap == null) return (new ProbeResult { Cancelled = true }, false);

        var parsed = Probe.ParseContract(cap.StdOut);
        if (parsed != null) return (new ProbeResult { Probe = parsed }, false);

        return (new ProbeResult
        {
            Error = FirstLine(cap.StdErr) ?? "The pipeline's probe returned nothing.",
        }, MeansPipelineTooOld(cap.StdErr));
    }

    private static async Task<ProbeResult> ViaYtDlpAsync(
        ProbeRequest request, CancellationToken token)
    {
        var ytdlp = Paths.Which("yt-dlp");
        if (ytdlp == null)
        {
            return new ProbeResult
            {
                Error = "This pipeline is older than `ytdl --probe`, and yt-dlp "
                    + "is not on PATH either, so there is no way to read the URL. "
                    + "Update the pipeline to get the preview.",
            };
        }

        /* The same two bounded calls probe.ps1 makes, for the same reason: -J
         * against a channel dumps the full extraction of every video in it.
         * Flat first to find out what this is; then one full extraction for
         * the format table. --ignore-config, because the pipeline's own conf
         * opens with --update and a preview must not self-update yt-dlp. */
        var baseArgs = new[]
        {
            "--ignore-config", "--no-progress", "--socket-timeout", "15",
            "--retries", "2", "--extractor-retries", "2",
        };
        var itemSpec = string.IsNullOrWhiteSpace(request.Items)
            ? "1:501"
            : request.Items.Trim();
        var url = RunOptions.NormalizeUrl(request.Url);

        var flatArgs = new List<string> { "-J" };
        flatArgs.AddRange(baseArgs);
        flatArgs.AddRange(request.ExtraArgs);
        flatArgs.AddRange(new[] { "--flat-playlist", "--playlist-items", itemSpec });
        /* The end-of-options marker, same as every call site in the pipeline:
         * about one YouTube id in thirty starts with "-" or "_". */
        flatArgs.Add("--");
        flatArgs.Add(url);

        var flatCap = await CaptureAsync(ytdlp, flatArgs, token).ConfigureAwait(false);
        if (flatCap == null) return new ProbeResult { Cancelled = true };
        var flat = flatCap.StdOut.Trim();
        if (flat.Length == 0)
        {
            return new ProbeResult
            {
                Error = FirstLine(flatCap.StdErr) ?? "yt-dlp could not read that URL.",
            };
        }

        // Which URL the format table should come from.
        var target = url;
        var flatRoot = JsonFile.ObjectFrom(flat);
        if (flatRoot != null)
        {
            var type = flatRoot.Str("_type") ?? "";
            if (type == "playlist" || type == "multi_video")
            {
                var entries = flatRoot.Value.Objects("entries");
                if (entries.Count > 0)
                {
                    target = entries[0].Str("url")
                        ?? entries[0].Str("webpage_url")
                        ?? url;
                }
            }
        }

        var fullArgs = new List<string> { "-J" };
        fullArgs.AddRange(baseArgs);
        fullArgs.AddRange(request.ExtraArgs);
        fullArgs.Add("--no-playlist");
        fullArgs.Add("--");
        fullArgs.Add(target);

        var fullCap = await CaptureAsync(ytdlp, fullArgs, token).ConfigureAwait(false);
        if (fullCap == null) return new ProbeResult { Cancelled = true };
        var full = fullCap.StdOut.Trim();
        if (full.Length == 0)
        {
            return new ProbeResult
            {
                Error = FirstLine(fullCap.StdErr) ?? "yt-dlp could not read that URL.",
            };
        }

        var probe = Probe.FromYtDlp(flat, full);
        if (probe == null)
        {
            return new ProbeResult { Error = "yt-dlp returned something that is not JSON." };
        }
        if (probe.Url.Length == 0) probe.Url = url;
        return new ProbeResult { Probe = probe };
    }
}
