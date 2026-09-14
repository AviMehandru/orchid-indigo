/* The URL preview.
 *
 * The thing most worth defending here is not that either parser works. It is
 * that the TWO OF THEM AGREE.
 *
 * Probe reads a probe-contract document when the installed pipeline can
 * produce one, and derives the same answers from raw `yt-dlp -J` when it
 * cannot. Two derivations that disagree would be worse than having no preview
 * at all: one pipeline version would offer MP4 for a video where another did
 * not, with nothing in the UI saying why, and the difference would only ever
 * be visible to someone who upgraded mid-session. So the fixture below is fed
 * through both paths and the derived lists compared field by field.
 *
 * THE FIXTURE IS THE SAME ONE the pipeline's 085-probe suite, the GTK app's
 * tests/test_url_probe.c and the SwiftUI app's Tests/UrlProbeTests.swift use,
 * value for value. That is deliberate and is the whole point: when all four
 * pass, the C#, PowerShell, C and Swift derivations have been shown to agree
 * on the same input rather than each being self-consistent. The expected
 * values were written from docs/probe-contract.md, not from any one
 * implementation.
 *
 * The format list is chosen so that a plausible wrong answer fails. yt-dlp
 * spells VP9 as "vp09" and AV1 as "av01" with a zero; it calls AAC "mp4a"; its
 * storyboards are .mhtml entries with no streams at all; and a VP8 rendition
 * has a real height and no --codec spelling whatsoever.
 *
 * Nothing here starts a process or touches the network, so unlike the runner
 * and job-object suites this file is not actually Windows-bound.
 */

using System.Collections.Generic;
using System.Linq;
using Xunit;
using YtdlWin.Core;

namespace YtdlWin.Tests;

public class ProbeTests
{
    /// The nine renditions, in yt-dlp's own shape.
    private const string RawFormats = """
    {"id":"dQw4w9WgXcQ","title":"Test Video","uploader":"Test Channel",
     "channel":"Test Channel","duration":212.0,"upload_date":"20250114",
     "view_count":1234567,"thumbnail":"https://i.ytimg.com/vi/x/maxres.jpg",
     "subtitles":{"en":[],"de":[]},
     "formats":[
      {"format_id":"sb0","ext":"mhtml","vcodec":"none","acodec":"none","height":180},
      {"format_id":"137","ext":"mp4","vcodec":"avc1.640028","acodec":"none",
       "height":1080,"width":1920,"fps":30,"tbr":4412.5,"filesize":112233445},
      {"format_id":"248","ext":"webm","vcodec":"vp09.00.40.08","acodec":"none",
       "height":1080,"width":1920,"fps":30},
      {"format_id":"271","ext":"webm","vcodec":"vp09.00.50.08","acodec":"none",
       "height":1440,"width":2560,"filesize_approx":220000000},
      {"format_id":"401","ext":"mp4","vcodec":"av01.0.12M.08","acodec":"none",
       "height":2160,"width":3840,"dynamic_range":"SDR"},
      {"format_id":"330","ext":"webm","vcodec":"vp08.00.10.08","acodec":"none","height":480},
      {"format_id":"140","ext":"m4a","vcodec":"none","acodec":"mp4a.40.2"},
      {"format_id":"251","ext":"webm","vcodec":"none","acodec":"opus"},
      {"format_id":"18","ext":"mp4","vcodec":"avc1.42001E","acodec":"mp4a.40.2","height":360}
     ]}
    """;

    /// The same video as the pipeline would have described it.
    private const string ContractDoc = """
    {"probe_version":1,"kind":"video","url":"https://youtu.be/dQw4w9WgXcQ",
     "id":"dQw4w9WgXcQ","title":"Test Video","uploader":"Test Channel",
     "duration":212.0,"upload_date":"20250114","entry_count":1,"entries":[],
     "entries_truncated":false,"subtitle_langs":["en","de"],
     "heights":[2160,1440,1080,480,360],
     "video_codecs":["avc1","vp9","av01"],
     "audio_codecs":["opus","aac"],
     "containers":["mkv","mp4","webm"],
     "has_video":true,"has_audio":true,
     "formats":[
      {"format_id":"137","ext":"mp4","vcodec":"avc1.640028","acodec":"none",
       "video_family":"avc1","height":1080,"filesize":112233445},
      {"format_id":"271","ext":"webm","vcodec":"vp09.00.50.08","acodec":"none",
       "video_family":"vp9","height":1440},
      {"format_id":"401","ext":"mp4","vcodec":"av01.0.12M.08","acodec":"none",
       "video_family":"av01","height":2160},
      {"format_id":"251","ext":"webm","vcodec":"none","acodec":"opus",
       "audio_family":"opus"}
     ],
     "pot":{"healthy":true,"reason":"provider healthy",
            "note":"Formats were read with the same player clients a download will use."}}
    """;

    // ------------------------------------------------------------------
    // Codec families
    // ------------------------------------------------------------------

    [Fact]
    public void CodecFamiliesMapYtDlpSpellings()
    {
        // The four mappings, and the near-misses that would each produce a
        // visibly wrong list.
        Assert.Equal("avc1", Probe.VideoFamilyOf("avc1.640028"));
        Assert.Equal("avc1", Probe.VideoFamilyOf("h264"));

        // yt-dlp emits the four-character form. A test for "vp9" alone reports
        // the most common codec on YouTube as having no spelling at all.
        Assert.Equal("vp9", Probe.VideoFamilyOf("vp09.00.50.08"));
        Assert.Equal("vp9", Probe.VideoFamilyOf("vp9"));

        // With a zero. "av1" is not a thing yt-dlp ever writes.
        Assert.Equal("av01", Probe.VideoFamilyOf("av01.0.12M.08"));
        Assert.Null(Probe.VideoFamilyOf("av1"));
        Assert.Null(Probe.VideoFamilyOf("vp08.00.10.08"));
        Assert.Null(Probe.VideoFamilyOf("none"));
        Assert.Null(Probe.VideoFamilyOf(null));

        Assert.Equal("aac", Probe.AudioFamilyOf("mp4a.40.2"));
        Assert.Equal("opus", Probe.AudioFamilyOf("opus"));
        Assert.Equal("mp3", Probe.AudioFamilyOf("mp3"));
        Assert.Equal("flac", Probe.AudioFamilyOf("flac"));
        Assert.Null(Probe.AudioFamilyOf("none"));
    }

    // ------------------------------------------------------------------
    // The two paths
    // ------------------------------------------------------------------

    [Fact]
    public void FallbackDerivesTheListsFromRawYtDlp()
    {
        var p = Probe.FromYtDlp(null, RawFormats);
        Assert.NotNull(p);

        Assert.Equal("video", p!.Kind);
        Assert.Equal("dQw4w9WgXcQ", p.Id);
        Assert.Equal("Test Video", p.Title);
        Assert.True(p.FromFallback);

        // The storyboard is gone; the eight real renditions are not.
        Assert.Equal(8, p.Formats.Count);

        // 1080 appears twice in the input and once here; 480 is the VP8
        // rendition, whose height is real even though this pipeline has no
        // --codec spelling for its codec; 180 was the storyboard's.
        Assert.Equal(new[] { 2160, 1440, 1080, 480, 360 }, p.Heights);
        Assert.Equal(new[] { "avc1", "vp9", "av01" }, p.VideoCodecs);
        Assert.Equal(new[] { "opus", "aac" }, p.AudioCodecs);
        Assert.Equal(new[] { "mkv", "mp4", "webm" }, p.Containers);
        Assert.True(p.HasVideo);
        Assert.True(p.HasAudio);

        // An exact size and an estimate must not be collapsed into one number:
        // the UI shows "421 MB" differently from "~421 MB", and merging them
        // presents a guess as a fact.
        var f137 = p.Formats.Single(f => f.FormatId == "137");
        Assert.Equal(112233445L, f137.Filesize);
        Assert.Equal(0L, f137.FilesizeApprox);

        var f271 = p.Formats.Single(f => f.FormatId == "271");
        Assert.Equal(0L, f271.Filesize);
        Assert.Equal(220000000L, f271.FilesizeApprox);

        // The VP8 rendition is listed, and has no family. Not "any": "any" is
        // a choice the user makes.
        Assert.Null(p.Formats.Single(f => f.FormatId == "330").VideoFamily);

        Assert.Equal(new[] { "en", "de" }, p.SubtitleLangs);
    }

    [Fact]
    public void ContractDocumentIsReadNotRecomputed()
    {
        var p = Probe.ParseContract(ContractDoc);
        Assert.NotNull(p);

        Assert.Equal(Probe.SupportedVersion, p!.ProbeVersion);
        Assert.False(p.FromFallback);
        Assert.Equal("Test Video", p.Title);
        Assert.True(p.PotHealthy);
        Assert.NotEmpty(p.PotNote);

        // The derived lists are READ from the document. The pipeline made them
        // under the PO token provider a real download will use, and
        // recomputing here would let this app disagree with the command it is
        // about to run -- note the formats array in the document is
        // deliberately shorter than the heights array it ships with.
        Assert.Equal(new[] { 2160, 1440, 1080, 480, 360 }, p.Heights);
        Assert.Equal(4, p.Formats.Count);
    }

    /// The reason this file exists.
    [Fact]
    public void BothPathsAgree()
    {
        var fallback = Probe.FromYtDlp(null, RawFormats);
        var contract = Probe.ParseContract(ContractDoc);
        Assert.NotNull(fallback);
        Assert.NotNull(contract);

        Assert.Equal(contract!.Heights, fallback!.Heights);
        Assert.Equal(contract.VideoCodecs, fallback.VideoCodecs);
        Assert.Equal(contract.AudioCodecs, fallback.AudioCodecs);
        Assert.Equal(contract.Containers, fallback.Containers);
    }

    // ------------------------------------------------------------------
    // Containers
    // ------------------------------------------------------------------

    [Fact]
    public void ContainersAreARealConstraintNotAPreference()
    {
        // Opus-only audio. yt-dlp cannot mux Opus into mp4, so offering it
        // produces a re-encode or a failed merge depending on version -- which
        // the user discovers afterwards. This is the assertion that earns the
        // whole derivation its place.
        const string opusOnly = """
        {"id":"x","formats":[
         {"format_id":"248","ext":"webm","vcodec":"vp09.00.40.08","acodec":"none","height":1080},
         {"format_id":"251","ext":"webm","vcodec":"none","acodec":"opus"}]}
        """;
        var p = Probe.FromYtDlp(null, opusOnly);
        Assert.NotNull(p);
        Assert.Equal(new[] { "mkv", "webm" }, p!.Containers);

        // And the mirror image: AAC with no Opus gets mp4 and not webm.
        const string aacOnly = """
        {"id":"x","formats":[
         {"format_id":"137","ext":"mp4","vcodec":"avc1.640028","acodec":"none","height":1080},
         {"format_id":"140","ext":"m4a","vcodec":"none","acodec":"mp4a.40.2"}]}
        """;
        var q = Probe.FromYtDlp(null, aacOnly);
        Assert.NotNull(q);
        Assert.Equal(new[] { "mkv", "mp4" }, q!.Containers);
    }

    [Fact]
    public void HeightsCrossFilterByCodec()
    {
        // 1440p exists only in VP9 in the fixture, and 2160p only in AV1. A
        // Quality list built from the union of heights therefore offers
        // combinations the video does not have -- the same class of silent
        // wrong answer the static list had, just with better numbers in it.
        var p = Probe.FromYtDlp(null, RawFormats);
        Assert.NotNull(p);

        Assert.Equal(new[] { 1440, 1080 }, p!.HeightsForCodec("vp9"));
        Assert.Equal(new[] { 2160 }, p.HeightsForCodec("av01"));
        Assert.Equal(new[] { 1080, 360 }, p.HeightsForCodec("avc1"));

        // "any" and null both mean every height the video has, VP8's included.
        Assert.Equal(5, p.HeightsForCodec("any").Count);
        Assert.Equal(5, p.HeightsForCodec(null).Count);
    }

    // ------------------------------------------------------------------
    // Playlists
    // ------------------------------------------------------------------

    [Fact]
    public void PlaylistEntriesCarryTheirPlaylistPositions()
    {
        const string flat = """
        {"_type":"playlist","id":"PL1","title":"A Playlist","channel":"Test Channel",
         "playlist_count":3,"entries":[
          {"id":"a","title":"One","duration":60,"url":"https://youtu.be/a"},
          {"id":"b","title":"Two","duration":120,"url":"https://youtu.be/b"},
          {"id":"c","title":"Three","url":"https://youtu.be/c"}]}
        """;
        var p = Probe.FromYtDlp(flat, RawFormats);
        Assert.NotNull(p);

        Assert.Equal("playlist", p!.Kind);
        Assert.True(p.IsPlaylist);
        Assert.Equal(3, p.EntryCount);
        Assert.Equal(3, p.PlaylistCount);

        // 1-based, because that is what --playlist-items counts. An off-by-one
        // here queues the wrong videos and the run succeeds.
        Assert.Equal(new[] { 1, 2, 3 }, p.Entries.Select(e => e.Index));
        Assert.Equal("Two", p.Entries[1].Title);
        Assert.Equal("b", p.Entries[1].VideoId);

        // The format table came from the first entry and says so.
        Assert.Equal("dQw4w9WgXcQ", p.FormatsFromId);
        Assert.Equal(8, p.Formats.Count);
    }

    // ------------------------------------------------------------------
    // --items ranges
    // ------------------------------------------------------------------

    [Fact]
    public void ItemsRangeCompacts()
    {
        // Compacted rather than emitted as a comma list: a 400-video channel
        // selection would otherwise be a command line thousands of characters
        // long, which is unreadable in the command preview and close enough to
        // a real limit on Windows to matter.
        Assert.Equal("1-3,7,10-12", ItemsRange.Compact(new[] { 1, 2, 3, 7, 10, 11, 12 }));
        Assert.Equal("5", ItemsRange.Compact(new[] { 5 }));

        // A pair stays a pair. "4-5" is the same length as "4,5" and reads
        // like it might be open-ended.
        Assert.Equal("4,5", ItemsRange.Compact(new[] { 4, 5 }));

        // Unsorted input, and duplicates, both produce the same answer as
        // sorted unique input would -- a saved range parsed back in can
        // overlap itself.
        Assert.Equal("1-3,10-12", ItemsRange.Compact(new[] { 12, 3, 1, 2, 11, 10, 3 }));
        Assert.Equal("", ItemsRange.Compact(new int[0]));
        Assert.Equal("", ItemsRange.Compact(new[] { 0, -4 }));
    }

    [Fact]
    public void ItemsRangeRoundTrips()
    {
        var got = ItemsRange.Parse("1-3,7,10-12");
        Assert.Equal(new[] { 1, 2, 3, 7, 10, 11, 12 }, got);
        Assert.Equal("1-3,7,10-12", ItemsRange.Compact(got));

        // yt-dlp's open-ended forms are NOT expanded to a guessed bound: a tick
        // list built from a guess would show a selection the pipeline may not
        // agree with. They parse to nothing and the typed text stays.
        Assert.Empty(ItemsRange.Parse("5-"));
        Assert.Empty(ItemsRange.Parse("-10"));
        Assert.Empty(ItemsRange.Parse("not a range"));
        Assert.Empty(ItemsRange.Parse(""));
        Assert.Empty(ItemsRange.Parse(null));
    }

    // ------------------------------------------------------------------
    // Bad input
    // ------------------------------------------------------------------

    [Fact]
    public void BadInputIsNullNotAnException()
    {
        Assert.Null(Probe.ParseContract(""));
        Assert.Null(Probe.ParseContract(null));
        Assert.Null(Probe.ParseContract("not json at all"));
        // A bare array is valid JSON and not a probe document.
        Assert.Null(Probe.ParseContract("[1,2,3]"));
        Assert.Null(Probe.FromYtDlp(null, ""));
    }

    [Fact]
    public void EmptyObjectParsesWithUsableDefaults()
    {
        // An object with nothing in it parses, and every list comes back empty
        // rather than null -- the form walks these unconditionally, and a
        // Container list with no rows renders as a blank control that cannot
        // be opened.
        var bare = Probe.ParseContract("{}");
        Assert.NotNull(bare);
        Assert.Equal("video", bare!.Kind);
        // No formats at all still leaves mkv, which is always muxable.
        Assert.Equal(new[] { "mkv" }, bare.Containers);
        Assert.Empty(bare.Entries);
        Assert.Empty(bare.Heights);
    }

    // ------------------------------------------------------------------
    // The fallback trigger
    // ------------------------------------------------------------------

    [Fact]
    public void OnlyAnOldPipelineTriggersTheFallback()
    {
        // Narrow on purpose. Falling back on ANY failure would turn "this
        // video is private" into a second, slower attempt that also fails, and
        // would hide a genuinely broken pipeline behind a path that happens to
        // work.
        Assert.True(ProbeRunner.MeansPipelineTooOld(
            "Unknown option: --probe\nUsage: ytdl <youtube-url> ..."));
        Assert.True(ProbeRunner.MeansPipelineTooOld(
            @"ERROR: probe.ps1 not found at C:\yt-dlp\scripts"));

        Assert.False(ProbeRunner.MeansPipelineTooOld(
            "ERROR: could not read the URL -- Video unavailable"));
        Assert.False(ProbeRunner.MeansPipelineTooOld(
            "ERROR: Sign in to confirm your age"));
        Assert.False(ProbeRunner.MeansPipelineTooOld(""));
        Assert.False(ProbeRunner.MeansPipelineTooOld(null));
    }

    [Fact]
    public void FirstLineReportsTheUsefulSentence()
    {
        // yt-dlp's useful sentence is not always the last thing printed once
        // the pipeline's own notes are in the stream.
        Assert.Equal("ERROR: Video unavailable",
            ProbeRunner.FirstLine("\n  \nERROR: Video unavailable\nmore noise\n"));
        Assert.Null(ProbeRunner.FirstLine(""));
        Assert.Null(ProbeRunner.FirstLine(null));
    }
}
