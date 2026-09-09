/* The archive conformance suite.
 *
 * docs/archive-layout.md in the pipeline repo says what this is for:
 *
 *   "Out-of-repo consumers are expected to keep their own conformance test that
 *    builds a fixture tree in this shape and asserts their reader finds it.
 *    That test is what turns a bump into a build failure on their side rather
 *    than a bug report from a user."
 *
 * So this file is not optional and an app in this repository without one does
 * not belong in it. It covers the cases a LAYOUT-1 reader gets wrong, because
 * those are the ones that fail silently: a folder that indexes fine and shows
 * the wrong file, or none.
 *
 * It also covers the two failures that are this platform's alone -- separators
 * and MAX_PATH -- which the GTK and SwiftUI suites have no reason to test and
 * which would each produce a short library with no error anywhere.
 */

using System;
using System.IO;
using System.Linq;
using Xunit;
using YtdlWin.Core;

namespace YtdlWin.Tests;

public sealed class ArchiveConformanceTests : IDisposable
{
    private readonly string _root;
    private readonly string _archive;

    public ArchiveConformanceTests()
    {
        _root = FixtureSupport.MakeTempDir("ytdl-win-archive");
        _archive = Path.Combine(_root, "Youtube Videos", "Complete Archive");
        Directory.CreateDirectory(_archive);
    }

    public void Dispose() => FixtureSupport.Delete(_root);

    // MARK: - Fixture building

    private string MakeVideo(string channel, string folder)
    {
        var dir = Path.Combine(_archive, channel, folder);
        Directory.CreateDirectory(Path.Combine(dir, "Final files"));
        Directory.CreateDirectory(Path.Combine(dir, "Video metadata"));
        return dir;
    }

    private static void WriteManifest(string videoDir, string body)
        => FixtureSupport.Write(body, Path.Combine(videoDir, "Video metadata", "manifest.json"));

    private static void WriteFile(string videoDir, string relative, string contents = "x")
        => FixtureSupport.Write(contents, Path.Combine(videoDir, relative.Replace('/', Path.DirectorySeparatorChar)));

    private const string Folder = "Rick Astley - 20091025 - dQw4w9WgXcQ - Never Gonna Give You Up";

    private ArchiveEntry ScanOne()
    {
        var index = ArchiveIndex.Scan(_archive);
        return Assert.Single(index.Entries);
    }

    // MARK: - The pin

    /* Two copies of one fact, asserted to agree. The pin is what an installer
     * and a human read; the constant is what actually decides, per video,
     * whether a folder is rendered or flagged. Them disagreeing is how an app
     * ends up claiming to read a layout it does not.
     *
     * Reads the FILE rather than hardcoding the number, which is the whole
     * point -- a test with the number written into it stops being a test of
     * agreement the moment somebody edits one side. */
    [Fact]
    public void SupportedLayoutMatchesTheRepositoryPin()
    {
        var path = FixtureSupport.RepositoryFile("CLI_VERSION");
        Assert.True(path is not null,
            "CLI_VERSION was not found beside the test binary or above it. It is copied by the " +
            "test project; a missing copy means this assertion is not running, not that it passed.");

        var pin = CliVersion.Load(path!);
        Assert.NotNull(pin);
        Assert.True(pin!.RequiresArchiveLayout.HasValue,
            "CLI_VERSION has no REQUIRES_ARCHIVE_LAYOUT line.");
        Assert.Equal(ArchiveLayout.Supported, pin.RequiresArchiveLayout!.Value);
    }

    // MARK: - Layout 2 media rules

    /* THE LAYOUT-1 BUG, stated once. A reader that globs for *.mkv was correct
     * under layout 1 and is wrong under layout 2, where --mode audio-only writes
     * Final Audio.opus and --container writes .mp4 or .webm. Matching on the
     * BASE NAME is the rule. */
    [Theory]
    [InlineData("Final files/Final Video.mkv")]
    [InlineData("Final files/Final Video.mp4")]
    [InlineData("Final files/Final Video.webm")]
    [InlineData("Final files/Final Audio.opus")]
    [InlineData("Final files/Final Audio.m4a")]
    public void MediaIsFoundByBaseNameWhateverTheExtension(string relative)
    {
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, relative);

        var entry = ScanOne();
        Assert.True(entry.MediaIndex >= 0, $"{relative} was not recognised as the media file");
        Assert.Equal(relative, entry.Files[entry.MediaIndex].Rel);
    }

    /* The manifest's media_file wins over the glob, because it is the only
     * answer that stays right when --container changes the extension. */
    [Fact]
    public void ManifestMediaFileWinsOverTheGlob()
    {
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, "Final files/Final Video.mkv");
        WriteFile(dir, "Final files/Final Audio.opus");
        WriteManifest(dir, """
        {
          "archive_layout_version": 2,
          "media_file": "Final files/Final Audio.opus"
        }
        """);

        var entry = ScanOne();
        Assert.Equal("Final files/Final Audio.opus", entry.Files[entry.MediaIndex].Rel);
    }

    /* WINDOWS ONLY, and the reason this suite is not just a translation of the
     * Swift one. postprocess.ps1 runs on Windows here, and a manifest written
     * on this platform spells media_file with BACKSLASHES. Compared against a
     * '/'-separated rel that matches nothing -- and the reader then falls
     * through to the glob, which usually gets the right answer anyway. That is
     * exactly what makes it the kind of bug that survives for a year: it is
     * only visible when the glob and the manifest disagree, which is what this
     * fixture arranges. */
    [Fact]
    public void ManifestMediaFileWithBackslashesStillMatches()
    {
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, "Final files/Final Video.mkv");
        WriteFile(dir, "Final files/Final Audio.opus");
        WriteManifest(dir, """
        {
          "archive_layout_version": 2,
          "media_file": "Final files\\Final Audio.opus"
        }
        """);

        var entry = ScanOne();
        Assert.Equal("Final files/Final Audio.opus", entry.Files[entry.MediaIndex].Rel);
    }

    /* A manifest naming a file somebody has since deleted degrades to the glob,
     * not to a broken path. */
    [Fact]
    public void ManifestNamingAMissingFileFallsBackToTheGlob()
    {
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, "Final files/Final Video.mkv");
        WriteManifest(dir, """
        { "archive_layout_version": 2, "media_file": "Final files/Final Audio.opus" }
        """);

        var entry = ScanOne();
        Assert.Equal("Final files/Final Video.mkv", entry.Files[entry.MediaIndex].Rel);
    }

    /* A folder with NO media at all is VALID. --mode metadata-only,
     * comments-only and subs-only all write one, and so does an interrupted
     * run. A reader that treats this as corruption hides real archive
     * content. */
    [Fact]
    public void AFolderWithNoMediaIsOrdinaryAndStillIndexed()
    {
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, "Video metadata/Info.info.json", "{}");
        WriteManifest(dir, """
        { "archive_layout_version": 2, "download_mode": "metadata-only" }
        """);

        var entry = ScanOne();
        Assert.Equal(-1, entry.MediaIndex);
        Assert.Equal("metadata-only", entry.DownloadMode);
        Assert.Equal("Never Gonna Give You Up", entry.Title);
    }

    /* Pre-merge streams/ holds --keep-video's video-only and audio-only files.
     * Picking one gives a silent video or a black audio track. */
    [Fact]
    public void PreMergeStreamsAreNeverChosen()
    {
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, "Pre-merge streams/Final Video.mkv");
        WriteFile(dir, "Final files/Final Video.mkv");

        var entry = ScanOne();
        Assert.Equal("Final files/Final Video.mkv", entry.Files[entry.MediaIndex].Rel);
    }

    [Fact]
    public void PreMergeStreamsAloneMeansNoMedia()
    {
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, "Pre-merge streams/Final Video.mkv");

        Assert.Equal(-1, ScanOne().MediaIndex);
    }

    /* A format-id segment names a --keep-video leftover, and is excluded even
     * inside Final files/. */
    [Theory]
    [InlineData("Final Video.f137.mp4", true)]
    [InlineData("Final Video.f251.webm", true)]
    [InlineData("Final Video.mkv", false)]
    [InlineData("Final Video.f.mp4", false)]      // "f" with no digits
    [InlineData("Final Video.fx37.mp4", false)]   // not all digits
    [InlineData("Final Video.2023.mp4", false)]   // digits, but no leading f
    public void FormatIdSegmentsAreRecognised(string name, bool expected)
        => Assert.Equal(expected, ArchiveEntry.HasFormatIdSegment("Final files/" + name));

    [Fact]
    public void FormatIdFilesAreNotChosenAsTheMedia()
    {
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, "Final files/Final Video.f137.mp4");

        Assert.Equal(-1, ScanOne().MediaIndex);
    }

    [Fact]
    public void VideoWinsOverAudioWhenAFolderHoldsBoth()
    {
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, "Final files/Final Audio.opus");
        WriteFile(dir, "Final files/Final Video.mkv");

        // One scan, not two: indexing into the files of a second scan with an
        // index taken from the first is a bug waiting for the day the two
        // disagree.
        var entry = ScanOne();
        Assert.Equal("Final files/Final Video.mkv", entry.Files[entry.MediaIndex].Rel);
    }

    // MARK: - Layout version

    [Fact]
    public void AManifestWithNoVersionIsLayoutOneAndNotAnError()
    {
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, "Final files/Final Video.mkv");
        WriteManifest(dir, """{ "video_id": "dQw4w9WgXcQ" }""");

        var entry = ScanOne();
        Assert.Equal(0, entry.LayoutVersion);
        Assert.False(entry.LayoutTooNew);
    }

    /* A video written with a NEWER layout is shown and FLAGGED, never hidden.
     * An empty library with no explanation is the outcome the contract exists
     * to prevent. */
    [Fact]
    public void ANewerLayoutIsFlaggedButStillIndexed()
    {
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, "Final files/Final Video.mkv");
        WriteManifest(dir, $$"""
        { "archive_layout_version": {{ArchiveLayout.Supported + 1}} }
        """);

        var entry = ScanOne();
        Assert.True(entry.LayoutTooNew);
        Assert.True(entry.MediaIndex >= 0);
    }

    // MARK: - The folder-name fallback

    /* The documented fallback when info.json is missing or unparseable. Anchors
     * on the date and the id because BOTH the uploader and the title routinely
     * contain " - " themselves. */
    [Fact]
    public void FolderNameIsParsedWhenThereIsNoMetadata()
    {
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, "Final files/Final Video.mkv");

        var entry = ScanOne();
        Assert.Equal("Rick Astley", entry.Uploader);
        Assert.Equal("20091025", entry.UploadDate);
        Assert.Equal("dQw4w9WgXcQ", entry.VideoId);
        Assert.Equal("Never Gonna Give You Up", entry.Title);
    }

    [Fact]
    public void SeparatorsInsideTheUploaderAndTitleSurvive()
    {
        var parsed = FolderName.Parse("Some - Band - 20240131 - abcdefghijk - A - B - C");
        Assert.NotNull(parsed);
        Assert.Equal("Some - Band", parsed!.Uploader);
        Assert.Equal("20240131", parsed.UploadDate);
        Assert.Equal("abcdefghijk", parsed.Id);
        Assert.Equal("A - B - C", parsed.Title);
    }

    [Theory]
    [InlineData("no separators at all")]
    [InlineData("Uploader - notadate - abcdefghijk - Title")]
    [InlineData("Uploader - 20240131 - tooshort - Title")]
    public void AnUnparseableFolderNameYieldsNull(string name)
        => Assert.Null(FolderName.Parse(name));

    [Fact]
    public void AnUnparseableFolderNameFallsBackToTheWholeName()
    {
        const string odd = "some folder nobody named properly";
        var dir = MakeVideo("Rick Astley", odd);
        WriteFile(dir, "Final files/Final Video.mkv");

        var entry = ScanOne();
        Assert.Equal(odd, entry.Title);
        Assert.Equal("Rick Astley", entry.Uploader);   // falls back to the channel folder
        Assert.Null(entry.VideoId);
    }

    // MARK: - Structure

    [Fact]
    public void ChannelInfoIsNotAVideo()
    {
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, "Final files/Final Video.mkv");
        Directory.CreateDirectory(Path.Combine(_archive, "Rick Astley", "Channel Info"));
        FixtureSupport.Write("{}", Path.Combine(_archive, "Rick Astley", "Channel Info", "avatar.json"));

        var index = ArchiveIndex.Scan(_archive);
        Assert.Single(index.Entries);
        Assert.Single(index.Channels);
    }

    [Fact]
    public void AChannelWithNoVideosIsNotCounted()
    {
        Directory.CreateDirectory(Path.Combine(_archive, "Empty Channel"));
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, "Final files/Final Video.mkv");

        var index = ArchiveIndex.Scan(_archive);
        Assert.Equal(new[] { "Rick Astley" }, index.Channels);
    }

    [Fact]
    public void ScanningANonDirectoryThrowsAndNamesTheRoot()
    {
        var missing = Path.Combine(_root, "not-here");
        var ex = Assert.Throws<ArchiveIndex.ScanException>(() => ArchiveIndex.Scan(missing));
        Assert.Contains(missing, ex.Message, StringComparison.Ordinal);
    }

    // MARK: - The opaque key and path resolution

    /* The key is SHA-256 of the '/'-separated relative path truncated to 8
     * bytes. It must come out byte for byte the same as the key the GTK and
     * SwiftUI apps compute for the same folder -- so this pins the ALGORITHM
     * with a fixed vector rather than merely checking two calls agree, which
     * would pass just as happily if the whole scheme changed. */
    [Fact]
    public void TheKeyIsSixteenHexCharactersOfSha256()
    {
        var key = Paths.Key("Rick Astley/" + Folder);
        Assert.Equal(16, key.Length);
        Assert.All(key, c => Assert.True(char.IsAsciiDigit(c) || (c >= 'a' && c <= 'f'),
            $"'{c}' is not lowercase hex"));

        // Backslashes normalise, so the same folder keys the same whichever
        // platform indexed it. This is the assertion that would fail first if a
        // Path.Combine ever leaked a native separator into a `rel`.
        Assert.Equal(key, Paths.Key(@"Rick Astley\" + Folder));
    }

    [Fact]
    public void PathResolutionRefusesToEscapeTheFolder()
    {
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, "Final files/Final Video.mkv");
        var entry = ScanOne();

        Assert.Null(entry.PathForIndex(-1));
        Assert.Null(entry.PathForIndex(entry.Files.Count));

        var resolved = entry.PathForIndex(entry.MediaIndex);
        Assert.NotNull(resolved);
        Assert.True(Paths.IsInside(entry.Dir, resolved!));

        // A hand-made entry claiming a traversing relative path is refused --
        // the containment re-check, not a filter, is what makes that true.
        var hostile = new ArchiveEntry
        {
            Key = "", Dir = entry.Dir, Rel = "", Channel = "",
            Files = { new ArchiveFile { Rel = "../../escape.txt", Ext = ".txt", Folder = "", Size = 0 } },
        };
        Assert.Null(hostile.PathForIndex(0));
    }

    /* NTFS is case-insensitive, so a containment check that compared ordinally
     * would reject a legitimate path whenever the two halves were spelled with
     * different capitalisation -- which happens routinely, because one came
     * from a folder picker and the other from a typed setting. */
    [Fact]
    public void ContainmentIsCaseInsensitive()
    {
        Assert.True(Paths.IsInside(@"C:\Archive", @"c:\archive\video\file.mkv"));
        Assert.True(Paths.IsInside(@"C:\Archive", @"C:\Archive"));
        // ...and "C:\Archive" does not contain "C:\Archive2".
        Assert.False(Paths.IsInside(@"C:\Archive", @"C:\Archive2\file.mkv"));
    }

    // MARK: - Thumbnails

    [Fact]
    public void ImagesFolderBeatsALargerImageElsewhere()
    {
        var dir = MakeVideo("Rick Astley", Folder);
        WriteFile(dir, "Final files/Final Video.mkv");
        WriteFile(dir, "Images/thumb.jpg", "small");
        WriteFile(dir, "Final files/cover.png", new string('x', 50_000));

        var entry = ScanOne();
        Assert.Equal("Images/thumb.jpg", entry.Files[entry.ThumbnailIndex].Rel);
    }

    // MARK: - MAX_PATH

    /* THIS PLATFORM'S OWN FAILURE, and it has no counterpart in the GTK or
     * SwiftUI suites.
     *
     * The pipeline's per-video paths run to roughly 240 characters before the
     * data root is prefixed. Past 260, an ordinary Win32 path fails -- and it
     * does not throw somewhere visible: the directory enumeration comes back
     * empty, the deepest files (Video metadata/, Pre-merge streams/) are
     * missing from the listing, and the library shows a video with no metadata
     * and no error anywhere.
     *
     * The fixture builds a genuinely over-long path rather than asserting on
     * Paths.Extended's string output, because the string transformation being
     * right is not the claim -- the claim is that the READER finds the file. */
    [Fact]
    public void DeeplyNestedFilesPastMaxPathAreStillFound()
    {
        // A 90-character uploader and a 120-character title, which is long but
        // well within what YouTube allows and what this pipeline produces.
        var channel = new string('U', 90);
        var folder = $"{channel} - 20091025 - dQw4w9WgXcQ - {new string('T', 120)}";

        var dir = Path.Combine(_archive, channel, folder);
        var deep = Path.Combine(dir, "Video metadata");

        try
        {
            Directory.CreateDirectory(Paths.Extended(deep));
            Directory.CreateDirectory(Paths.Extended(Path.Combine(dir, "Final files")));
        }
        catch (Exception ex)
        {
            /* If the fixture itself cannot be built, the machine has neither
             * long-path support nor a working \\?\ path, and this test cannot
             * say anything. Skipping silently would be worse than saying so. */
            Assert.Fail($"Could not build the long-path fixture, so this assertion did not " +
                        $"run: {ex.Message}");
            return;
        }

        FixtureSupport.WriteBytes(new byte[] { 1, 2, 3 },
            Paths.Extended(Path.Combine(dir, "Final files", "Final Video.mkv")));
        FixtureSupport.WriteBytes(System.Text.Encoding.UTF8.GetBytes(
            """{ "archive_layout_version": 2, "media_file": "Final files/Final Video.mkv" }"""),
            Paths.Extended(Path.Combine(deep, "manifest.json")));

        Assert.True(dir.Length > 260,
            $"the fixture path is only {dir.Length} characters, so it does not exercise MAX_PATH");

        var index = ArchiveIndex.Scan(_archive);
        var entry = Assert.Single(index.Entries);

        Assert.True(entry.MediaIndex >= 0, "the media file past MAX_PATH was not found");
        Assert.Equal(2, entry.LayoutVersion);
        Assert.Contains(entry.Files, f => f.Rel == "Video metadata/manifest.json");
    }

    // MARK: - Extensions

    [Theory]
    [InlineData("Final files/Final Video.mkv", ".mkv")]
    [InlineData("Final files/Final Video.f137.mp4", ".mp4")]
    [InlineData("Final files/Final Video.MKV", ".mkv")]
    [InlineData("noextension", "")]
    [InlineData(".hidden", "")]
    public void ExtensionUsesTheLastDotAndLowercases(string rel, string expected)
        => Assert.Equal(expected, ArchiveFile.ExtOf(rel));
}
