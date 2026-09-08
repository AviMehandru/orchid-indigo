/* The argument builder and the two output parsers.
 *
 * These are pure functions over a string, which is why they can be pinned
 * without a download -- and they are the place a change in yt-dlp's output
 * would first bite, which is why they are pinned at all.
 *
 * Nothing here touches YouTube, and nothing here starts a process.
 */

using System;
using System.Linq;
using Xunit;
using YtdlWin.Core;

namespace YtdlWin.Tests;

public sealed class PipelineTests
{
    // MARK: - The argument builder

    /* A plain download produces exactly the command line it produced before any
     * of these options existed. --quality best, --codec any and --mode full are
     * no-ops the pipeline would accept and ignore, and emitting them would put
     * four extra flags in the preview of every ordinary download. */
    [Fact]
    public void DefaultsAreNotEmitted()
    {
        var opts = new RunOptions
        {
            Url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
            Mode = "full", Quality = "best", Codec = "any", AudioCodec = "any", Container = "mkv",
            Workers = 1,
        };

        var args = opts.ToArgs();
        Assert.Single(args);
        Assert.Equal("https://www.youtube.com/watch?v=dQw4w9WgXcQ", args[0]);
    }

    [Fact]
    public void NonDefaultsAreEmittedAsTheirFlags()
    {
        var opts = new RunOptions
        {
            Url = "dQw4w9WgXcQ",
            Mode = "audio-only", Quality = "1080", Codec = "vp9",
            AudioCodec = "opus", Container = "mp4",
            Sync = true, Lazy = true, Workers = 4, NoComments = true,
        };

        var args = opts.ToArgs();
        Assert.Contains("--mode", args);
        Assert.Equal("audio-only", args[args.IndexOf("--mode") + 1]);
        Assert.Equal("1080", args[args.IndexOf("--quality") + 1]);
        Assert.Equal("4", args[args.IndexOf("--workers") + 1]);
        Assert.Contains("--sync", args);
        Assert.Contains("--lazy", args);
        Assert.Contains("--no-comments", args);
    }

    /* --workers is emitted only above 1, because 1 is the pipeline's own
     * default and "--workers 1" in the preview of every download would suggest
     * a setting where there is none. */
    [Theory]
    [InlineData(0, false)]
    [InlineData(1, false)]
    [InlineData(2, true)]
    public void WorkersIsOnlyEmittedAboveOne(int workers, bool expected)
    {
        var opts = new RunOptions { Url = "x", Workers = workers };
        Assert.Equal(expected, opts.ToArgs().Contains("--workers"));
    }

    /* Each extra argument is its own --ytdlp-arg. A real --match-filter
     * expression contains commas and spaces, so there is no separator these
     * could safely be joined on. */
    [Fact]
    public void ExtraArgumentsAreRepeatedNotJoined()
    {
        var opts = new RunOptions
        {
            Url = "x",
            YtdlpArgs = { "--match-filter", "duration > 60 & !is_live" },
        };

        var args = opts.ToArgs();
        Assert.Equal(2, args.Count(a => a == "--ytdlp-arg"));
        Assert.Contains("duration > 60 & !is_live", args);
    }

    /* The URL is always a full URL. ytdl.ps1 accepts a bare 11-character id,
     * but a leading-hyphen id would be bound as a parameter before the script
     * ever saw it -- and about one YouTube id in thirty starts with "-" or
     * "_". */
    [Theory]
    [InlineData("dQw4w9WgXcQ", "https://www.youtube.com/watch?v=dQw4w9WgXcQ")]
    [InlineData("-Qw4w9WgXcQ", "https://www.youtube.com/watch?v=-Qw4w9WgXcQ")]
    [InlineData("_Qw4w9WgXcQ", "https://www.youtube.com/watch?v=_Qw4w9WgXcQ")]
    [InlineData("youtu.be/dQw4w9WgXcQ", "https://youtu.be/dQw4w9WgXcQ")]
    [InlineData("https://example.com/x", "https://example.com/x")]
    [InlineData("not an id at all", "not an id at all")]
    public void UrlsAreNormalised(string input, string expected)
        => Assert.Equal(expected, RunOptions.NormalizeUrl(input));

    /* THE PREVIEW IS QUOTED FOR POWERSHELL, not for a POSIX shell, because a
     * pwsh prompt is what somebody would paste it into -- `ytdl` IS a pwsh
     * script. Inside a double-quoted pwsh string a quote is escaped by DOUBLING
     * it; a backslash escape, which is what the macOS port would want, would
     * produce a different argument here and no error. */
    [Fact]
    public void ThePreviewQuotesForPowerShell()
    {
        var opts = new RunOptions
        {
            Url = "https://example.com/x",
            YtdlpArgs = { "--match-filter", "title ~= \"live\"" },
        };

        var preview = opts.CommandPreview();
        Assert.StartsWith("ytdl \"https://example.com/x\"", preview, StringComparison.Ordinal);
        Assert.Contains("\"\"live\"\"", preview, StringComparison.Ordinal);
    }

    // MARK: - Progress parsing

    [Fact]
    public void AProgressLineIsScannedIntoItsFields()
    {
        var p = new RunProgress();
        Assert.True(OutputParser.ParseProgressLine(
            "[download]  45.2% of  120.00MiB at   2.00MiB/s ETA 00:30", p));

        Assert.Equal(45.2, p.Percent, 3);
        Assert.Equal("120.00MiB", p.Total);
        Assert.Equal("2.00MiB/s", p.Speed);
        Assert.Equal("00:30", p.Eta);
        Assert.Equal("downloading", p.Stage);
    }

    [Fact]
    public void TheDestinationLineIsRecognised()
    {
        var p = new RunProgress();
        Assert.True(OutputParser.ParseProgressLine(
            @"[download] Destination: C:\yt-dlp\Youtube Videos\_incomplete\x.f137.mp4", p));
        Assert.Equal(@"C:\yt-dlp\Youtube Videos\_incomplete\x.f137.mp4", p.Destination);
    }

    /* A stage marker CLEARS the percentage. 45% of the download is not 45% of
     * the merge, and leaving the old number up reads as a stalled bar. */
    [Fact]
    public void AStageMarkerClearsThePercentage()
    {
        var p = new RunProgress();
        OutputParser.ParseProgressLine("[download]  45.2% of 120.00MiB", p);
        Assert.Equal(45.2, p.Percent, 3);

        Assert.True(OutputParser.ParseProgressLine("[Merger] Merging formats into \"x.mkv\"", p));
        Assert.Equal("merging", p.Stage);
        Assert.True(p.Percent < 0);
    }

    [Fact]
    public void TheVideoIdIsReadFromTheExtractorLine()
    {
        var p = new RunProgress();
        Assert.True(OutputParser.ParseProgressLine("[youtube] dQw4w9WgXcQ: Downloading webpage", p));
        Assert.Equal("dQw4w9WgXcQ", p.VideoId);
    }

    [Theory]
    [InlineData("[youtube] tooshort: Downloading webpage")]
    [InlineData("[youtube] has a space: Downloading")]
    public void AMalformedExtractorLineDoesNotSetAnId(string line)
    {
        var p = new RunProgress();
        OutputParser.ParseProgressLine(line, p);
        Assert.Null(p.VideoId);
    }

    /* The percentage is parsed with the INVARIANT culture. On a machine whose
     * locale uses a comma for the decimal separator, a culture-aware parse of
     * "45.2" fails -- and the bar would sit at zero for the whole download with
     * nothing reporting an error. */
    [Fact]
    public void ThePercentageParsesRegardlessOfTheMachineLocale()
    {
        var previous = System.Globalization.CultureInfo.CurrentCulture;
        try
        {
            System.Globalization.CultureInfo.CurrentCulture =
                new System.Globalization.CultureInfo("de-DE");

            var p = new RunProgress();
            Assert.True(OutputParser.ParseProgressLine("[download]  45.2% of 120.00MiB", p));
            Assert.Equal(45.2, p.Percent, 3);
        }
        finally { System.Globalization.CultureInfo.CurrentCulture = previous; }
    }

    // MARK: - The session summary

    [Fact]
    public void TheSessionSummaryYieldsFourCounts()
    {
        var summary = OutputParser.ParseSessionSummary(
            "-- Session summary: 12 video(s) touched, 3 already archived (skipped), " +
            "1 error(s), 2 warning(s) --");

        Assert.NotNull(summary);
        Assert.Equal(12, summary!.Value.Videos);
        Assert.Equal(3, summary.Value.Skipped);
        Assert.Equal(1, summary.Value.Errors);
        Assert.Equal(2, summary.Value.Warnings);
    }

    [Theory]
    [InlineData("an ordinary log line")]
    [InlineData("-- Session summary: 1 video(s) touched --")]   // too few numbers
    public void ANonSummaryLineYieldsNothing(string line)
        => Assert.Null(OutputParser.ParseSessionSummary(line));

    // MARK: - ANSI

    /* PowerShell 7.2+ enables virtual terminal processing and emits colour on
     * Windows 10+ by default, so escapes arrive here even when yt-dlp's own
     * output has none. */
    [Fact]
    public void AnsiSequencesAreStripped()
    {
        const string coloured = "\u001b[32m[download]\u001b[0m  45.2% of 120.00MiB";
        Assert.Equal("[download]  45.2% of 120.00MiB", OutputParser.StripAnsi(coloured));
    }

    [Fact]
    public void APlainLineIsReturnedUnchanged()
    {
        const string plain = "[download]  45.2% of 120.00MiB";
        Assert.Same(plain, OutputParser.StripAnsi(plain));
    }

    // MARK: - Round-tripping

    /* A field added to RunOptions becomes profileable, queueable and
     * history-recordable by being added to WriteTo -- there is no second list
     * anywhere to forget it from. This asserts the round trip so that a field
     * added to one half and not the other is caught. */
    [Fact]
    public void OptionsSurviveARoundTripThroughJson()
    {
        var original = new RunOptions
        {
            Url = "https://example.com/x", DataRoot = @"D:\Archive",
            Sync = true, Items = "1-10", After = "20240101", Lazy = true, Workers = 4,
            NoPot = true, SkipPotUpdate = true, PotPort = 4416,
            Mode = "audio-only", Quality = "1080", Codec = "vp9",
            AudioCodec = "opus", Container = "mp4",
            NoComments = true, NoSubs = true, NoThumbnail = true, NoMetadata = true,
            YtdlpArgs = { "--match-filter", "duration > 60" },
        };

        var bytes = JsonFile.Write(w => original.WriteTo(w));
        var parsed = RunOptions.FromJson(JsonFile.ObjectFrom(bytes));

        Assert.Equal(original.Url, parsed.Url);
        Assert.Equal(original.DataRoot, parsed.DataRoot);
        Assert.Equal(original.Items, parsed.Items);
        Assert.Equal(original.After, parsed.After);
        Assert.Equal(original.Workers, parsed.Workers);
        Assert.Equal(original.PotPort, parsed.PotPort);
        Assert.Equal(original.Mode, parsed.Mode);
        Assert.Equal(original.Container, parsed.Container);
        Assert.True(parsed.Sync && parsed.Lazy && parsed.NoPot && parsed.SkipPotUpdate);
        Assert.True(parsed.NoComments && parsed.NoSubs && parsed.NoThumbnail && parsed.NoMetadata);
        Assert.Equal(original.YtdlpArgs, parsed.YtdlpArgs);
    }

    /* Every field is optional on read: a profile or queued run written by an
     * older build keeps working and simply does not set what it did not know
     * about. */
    [Fact]
    public void AnEmptyObjectReadsAsDefaults()
    {
        var parsed = RunOptions.FromJson(JsonFile.ObjectFrom("{}"));
        Assert.Equal("", parsed.Url);
        Assert.Equal(0, parsed.Workers);
        Assert.False(parsed.Sync);
        Assert.Empty(parsed.YtdlpArgs);
    }

    // MARK: - Tolerant JSON

    /* ffprobe reports numbers as JSON strings more often than not
     * ("bit_rate": "128000") and omits a key rather than sending null when it
     * does not know. Both are normal, neither is an error. */
    [Fact]
    public void NumbersAreReadWhetherTheyAreNumbersOrStrings()
    {
        var o = JsonFile.ObjectFrom("""
        { "a": 128000, "b": "128000", "c": "23.976", "d": true, "e": null }
        """);
        Assert.NotNull(o);

        Assert.Equal(128000, o.Int("a"));
        Assert.Equal(128000, o.Int("b"));
        Assert.Equal(23.976, o.Double("c"), 5);
        // A JSON boolean is not a count.
        Assert.Equal(-1, o.Int("d", -1));
        Assert.Equal(-1, o.Int("e", -1));
        Assert.Equal(-1, o.Int("missing", -1));
    }

    /* A decimal written the way ffprobe writes it must parse the same on every
     * machine. Under a locale whose decimal separator is a comma, a
     * culture-aware parse of "23.976" gives 23976 -- a frame rate three orders
     * of magnitude wrong, with nothing throwing. */
    [Fact]
    public void DecimalsParseRegardlessOfTheMachineLocale()
    {
        var previous = System.Globalization.CultureInfo.CurrentCulture;
        try
        {
            System.Globalization.CultureInfo.CurrentCulture =
                new System.Globalization.CultureInfo("de-DE");

            var o = JsonFile.ObjectFrom("""{ "fps": "23.976" }""");
            Assert.Equal(23.976, o.Double("fps"), 5);
            Assert.Equal(23.976, Media.Rational("24000/1001"), 2);
        }
        finally { System.Globalization.CultureInfo.CurrentCulture = previous; }
    }

    [Fact]
    public void AnEmptyStringMemberReadsAsAbsent()
    {
        var o = JsonFile.ObjectFrom("""{ "title": "" }""");
        Assert.Null(o.Str("title"));
    }

    [Fact]
    public void MalformedJsonYieldsNullRatherThanThrowing()
    {
        Assert.Null(JsonFile.ObjectFrom("{ not json at all"));
        Assert.Null(JsonFile.ObjectFrom("[1, 2, 3]"));   // an array is not an object
        Assert.Null(JsonFile.ObjectFrom(""));
    }

    // MARK: - Formatting

    /* Counts and sizes are formatted culture-invariantly, so the same number
     * reads the same on every machine -- and so these expectations stay true
     * wherever the suite runs. The GTK app hit the other side of this: printf's
     * "%'" grouping flag does nothing under the C locale, which is what a
     * desktop launcher gets, and a view count came out as 24913882. */
    [Theory]
    [InlineData(0, "0")]
    [InlineData(999, "999")]
    [InlineData(1000, "1,000")]
    [InlineData(24913882, "24,913,882")]
    [InlineData(-1234, "-1,234")]
    public void CountsAreGroupedInvariantly(long n, string expected)
        => Assert.Equal(expected, Format.Count(n));

    [Fact]
    public void CountsHandleTheExtremeOfLong()
    {
        // long.MinValue has no positive counterpart, which is the one input a
        // naive negate-and-format would throw on.
        Assert.Equal("-9,223,372,036,854,775,808", Format.Count(long.MinValue));
    }

    /* A duration ROUNDS and a timecode TRUNCATES. A chapter that starts at
     * 90.7s is at 1:30, not 1:31: rounding a timestamp up names a moment the
     * thing has not started yet, which is wrong in the one way a seek point can
     * be wrong. */
    [Fact]
    public void DurationsRoundAndTimecodesTruncate()
    {
        Assert.Equal("1:31", Format.Clock(90.7));
        Assert.Equal("1:30", Format.Timecode(90.7));
        Assert.Equal("1:02:03", Format.Clock(3723));
        Assert.Equal("", Format.Duration(0));
    }

    [Theory]
    [InlineData("20240131", "31 Jan 2024")]
    [InlineData("20091025", "25 Oct 2009")]
    [InlineData("not a date", "not a date")]
    [InlineData("20241331", "20241331")]   // month 13
    public void UploadDatesAreFormattedOrLeftAlone(string input, string expected)
        => Assert.Equal(expected, Format.UploadDate(input));

    /* BINARY units, matching what Explorer shows, so a file this app calls
     * "412 MB" is the "412 MB" in a Properties dialog. This deliberately
     * disagrees with the GTK app, which is SI. */
    [Theory]
    [InlineData(0, "0 bytes")]
    [InlineData(1, "1 byte")]
    [InlineData(1024, "1.0 KB")]
    [InlineData(1536, "1.5 KB")]
    [InlineData(1048576, "1.0 MB")]
    [InlineData(15728640, "15 MB")]
    public void SizesUseBinaryUnitsLikeExplorer(long n, string expected)
        => Assert.Equal(expected, Format.Bytes(n));
}
