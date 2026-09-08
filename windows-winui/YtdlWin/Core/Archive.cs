/* Reading the archive that postprocess.ps1 writes.
 *
 * This is a FIFTH independent implementation of the layout contract in
 * docs/archive-layout.md -- archive-viewer.py reads it in Python, the Tauri app
 * in Rust, the GTK app in C, the macOS app in Swift, and this in C#. That
 * duplication is the price of standalone apps with no shared engine, and the
 * contract document says plainly how it is paid: "Out-of-repo consumers are
 * expected to keep their own conformance test that builds a fixture tree in
 * this shape and asserts their reader finds it." ArchiveConformanceTests.cs is
 * this app's, and an app in this repo without one does not belong in it.
 *
 * THE ARCHIVE IS READ-ONLY. Nothing in this file creates, moves or modifies
 * anything under Youtube Videos/. postprocess.ps1 writes a checksums.sha256
 * covering every file in a video folder, so a derived file dropped in there
 * makes that manifest stop verifying. Derived state goes to Paths.CacheDir().
 *
 * Layout 2 rules that a layout-1 reader gets WRONG, implemented here:
 *   - the media file may be Final Video.<any ext> or Final Audio.<any ext>;
 *     match on base name, or better, read media_file from the manifest
 *   - a folder with NO media file at all is valid, not corrupt
 *   - Pre-merge streams/ must be skipped when choosing "the" video, or you pick
 *     a silent video or a black audio track
 *
 * WINDOWS: every `rel` on an ArchiveFile is '/'-SEPARATED, and every comparison
 * in this file assumes it. See trap 1 at the top of Paths.cs -- this is where
 * getting it wrong would change every key the app computes.
 */

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;

namespace YtdlWin.Core;

/// <summary>
/// The highest layout version this reader understands. A video written with a
/// higher one is shown on a best-effort basis and flagged, never hidden: an
/// empty library with no explanation is the outcome the contract exists to
/// prevent.
/// <para>
/// Asserted against <c>REQUIRES_ARCHIVE_LAYOUT</c> in the repository's
/// CLI_VERSION by the conformance suite. Two copies of one fact that must not
/// drift.
/// </para>
/// </summary>
public static class ArchiveLayout
{
    public const long Supported = 2;
}

public static class MediaExtensions
{
    /* Lowercased, with the leading dot, so a comparison is one set lookup
     * against the value stored on ArchiveFile. Ordinal comparers throughout:
     * the default string comparer for a HashSet is ordinal already, but saying
     * so stops a future edit from reaching for a culture-aware one, which for
     * ".ts" under a Turkish locale is not the joke it sounds like. */
    public static readonly HashSet<string> Video = new(StringComparer.Ordinal)
        { ".mkv", ".mp4", ".webm", ".m4v", ".mov", ".avi", ".flv", ".ts" };

    public static readonly HashSet<string> Audio = new(StringComparer.Ordinal)
        { ".m4a", ".opus", ".mp3", ".flac", ".ogg", ".wav", ".aac" };

    public static readonly HashSet<string> Image = new(StringComparer.Ordinal)
        { ".png", ".jpg", ".jpeg", ".webp", ".gif", ".avif" };

    /* --write-subs and --write-auto-subs both land in Subtitles/; the container
     * formats yt-dlp can be asked for are these. */
    public static readonly HashSet<string> Subtitle = new(StringComparer.Ordinal)
        { ".vtt", ".srt", ".ass", ".ssa", ".sub", ".lrc" };

    public static bool IsSubtitle(string? ext)
        => !string.IsNullOrEmpty(ext) && Subtitle.Contains(ext);
}

/// One file inside a video folder.
public sealed class ArchiveFile
{
    /// Folder-relative, ALWAYS '/'-separated.
    public required string Rel { get; init; }
    /// Lowercased, WITH the leading dot; "" if none.
    public required string Ext { get; init; }
    /// The top-level subfolder, e.g. "Final files"; "" at the folder root.
    public required string Folder { get; init; }
    public required long Size { get; init; }

    /* Lowercased extension including the dot, or "" when there is none. Uses
     * the LAST dot of the basename only, so "Final Video.f137.mp4" gives
     * ".mp4".
     *
     * ToLowerInvariant, not ToLower: under a Turkish locale ToLower(".TS")
     * produces a dotless i for ".TI"-shaped names, and more to the point a
     * lowercase mapping that varies by machine would mean the same file matched
     * the video-extension set on one PC and not another. */
    public static string ExtOf(string rel)
    {
        var slash = rel.LastIndexOfAny(new[] { '/', '\\' });
        var baseName = slash >= 0 ? rel[(slash + 1)..] : rel;

        var dot = baseName.LastIndexOf('.');
        if (dot <= 0) return "";
        return baseName[dot..].ToLowerInvariant();
    }
}

/// One video folder.
public sealed class ArchiveEntry
{
    /// Opaque; see <see cref="Paths.Key"/>. The only handle the UI ever holds.
    public required string Key { get; init; }
    /// Absolute path to the video folder, in native form.
    public required string Dir { get; init; }
    /// Path relative to the archive root, '/'-separated.
    public required string Rel { get; init; }
    /// The uploader folder's name.
    public required string Channel { get; init; }

    /// 0 means manifest.json carried no archive_layout_version, which the
    /// contract defines as layout 1 -- not an error and not worth a warning.
    public long LayoutVersion { get; set; }
    public bool LayoutTooNew { get; set; }

    /* The YouTube id, which is NOT this entry's identity: it can be absent (an
     * unparseable folder name with no info.json), and two entries with a null
     * id would collide anywhere the id was used as a handle. The opaque key
     * identifies an entry; the video id is data about it. */
    public string? VideoId { get; set; }
    public string Title { get; set; } = "";
    public string Uploader { get; set; } = "";
    public string? UploadDate { get; set; }
    public string? ChannelUrl { get; set; }
    public string? OriginalUrl { get; set; }
    /// Which --mode wrote this folder; null when the manifest does not say.
    public string? DownloadMode { get; set; }
    /// The manifest's media_file, folder-relative; null when absent.
    public string? MediaFile { get; set; }
    public long Timestamp { get; set; } = -1;
    public long ViewCount { get; set; } = -1;
    public double Duration { get; set; } = -1;

    public List<ArchiveFile> Files { get; set; } = new();

    /* "Final Video.f137.mp4" -- a --keep-video leftover that yt-dlp names with
     * a format-id segment. Excluded even when it sits in Final files/, because
     * picking one gives a silent video or a black audio track. */
    public static bool HasFormatIdSegment(string rel)
    {
        var slash = rel.LastIndexOfAny(new[] { '/', '\\' });
        var baseName = slash >= 0 ? rel[(slash + 1)..] : rel;

        var lastDot = baseName.LastIndexOf('.');
        if (lastDot < 0) return false;
        var stem = baseName[..lastDot];

        var prevDot = stem.LastIndexOf('.');
        if (prevDot < 0) return false;

        var segment = stem[(prevDot + 1)..];
        if (segment.Length < 2 || segment[0] != 'f') return false;
        for (var i = 1; i < segment.Length; i++)
        {
            if (segment[i] is < '0' or > '9') return false;
        }
        return true;
    }

    private static bool BasenameStartsWith(string rel, string want)
    {
        var slash = rel.LastIndexOfAny(new[] { '/', '\\' });
        var baseName = slash >= 0 ? rel[(slash + 1)..] : rel;
        return baseName.StartsWith(want, StringComparison.Ordinal);
    }

    /// <summary>
    /// The index into <see cref="Files"/> of the media file, or -1 -- which is
    /// ORDINARY. --mode metadata-only, comments-only and subs-only all write a
    /// complete folder with no media in it, and so does an interrupted run; the
    /// contract says not to try to tell them apart by guessing.
    /// <see cref="DownloadMode"/> says which.
    /// </summary>
    public int MediaIndex
    {
        get
        {
            /* 1. The manifest says so outright. Preferred over globbing at all,
             *    per the contract -- it is the only answer that stays right when
             *    --container or --audio-codec changes the extension.
             *
             *    Slashed() first: postprocess.ps1 runs on Windows here, and a
             *    manifest written on this platform spells media_file with
             *    backslashes. Comparing that against a '/'-separated rel finds
             *    nothing, silently, and falls through to the glob -- which
             *    usually gets the right answer anyway, which is exactly what
             *    makes this the kind of bug that survives for a year. */
            if (MediaFile is not null)
            {
                var want = Paths.Slashed(MediaFile);
                for (var i = 0; i < Files.Count; i++)
                {
                    if (string.Equals(Files[i].Rel, want, StringComparison.Ordinal)) return i;
                }
                /* Named but absent: fall through and glob. A manifest naming a
                 * file somebody has since deleted should degrade to "no media",
                 * not to a broken path. */
            }

            /* 2. Glob by BASE NAME, never by extension. Matching on .mkv was
             *    correct under layout 1 and is a bug under layout 2. Video wins
             *    over audio when a folder somehow holds both. */
            var audioHit = -1;
            for (var i = 0; i < Files.Count; i++)
            {
                var f = Files[i];
                if (f.Folder == "Pre-merge streams") continue;
                if (HasFormatIdSegment(f.Rel)) continue;
                if (!BasenameStartsWith(f.Rel, "Final Video.") &&
                    !BasenameStartsWith(f.Rel, "Final Audio.")) continue;

                if (MediaExtensions.Video.Contains(f.Ext)) return i;
                if (audioHit < 0 && MediaExtensions.Audio.Contains(f.Ext)) audioHit = i;
            }
            if (audioHit >= 0) return audioHit;

            /* 3. None. An ORDINARY state, not corrupt and not to be hidden. */
            return -1;
        }
    }

    /// The index into <see cref="Files"/> of the best thumbnail, or -1.
    public int ThumbnailIndex
    {
        get
        {
            var best = -1;
            long bestWeight = 0;
            for (var i = 0; i < Files.Count; i++)
            {
                var f = Files[i];
                if (!MediaExtensions.Image.Contains(f.Ext)) continue;

                /* Images/ is where postprocess.ps1 puts them; anything elsewhere
                 * is a fallback so a hand-reorganised folder still shows
                 * something. The preference is a constant far larger than any
                 * real file, so an image in Images/ always beats one outside it
                 * however big the outsider is. */
                var preferred = f.Folder == "Images" ? 1L << 50 : 0L;
                var weight = f.Size + preferred;
                if (best < 0 || weight > bestWeight)
                {
                    best = i;
                    bestWeight = weight;
                }
            }
            return best;
        }
    }

    /// <summary>
    /// Resolve a file index to an absolute native path, re-checking that the
    /// result is inside <see cref="Dir"/>. null for an out-of-range index or a
    /// path that escapes the folder. This is the ONLY way a path is produced:
    /// no caller supplies one.
    /// </summary>
    public string? PathForIndex(int idx)
    {
        if (idx < 0 || idx >= Files.Count) return null;
        var joined = Paths.Join(Dir, Paths.Native(Files[idx].Rel));

        /* Re-check containment even though the relative path came from our own
         * listing. It costs one string compare and it is what makes "the caller
         * never supplies a path" an enforced property rather than a
         * convention. */
        if (!Paths.IsInside(Dir, joined)) return null;
        return Paths.Canonical(joined);
    }

    public string? MediaPath => PathForIndex(MediaIndex);
    public string? ThumbnailPath => PathForIndex(ThumbnailIndex);
    public long TotalBytes => Files.Sum(f => f.Size);
}

/// The "&lt;uploader&gt; - &lt;YYYYMMDD&gt; - &lt;id&gt; - &lt;title&gt;" fallback.
public static class FolderName
{
    public sealed record Parsed(string Uploader, string UploadDate, string Id, string Title);

    private static bool IsVideoId(string s)
    {
        if (s.Length != 11) return false;
        foreach (var c in s)
        {
            var ok = c is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or >= '0' and <= '9'
                     or '-' or '_';
            if (!ok) return false;
        }
        return true;
    }

    private static bool IsEightDigits(string s)
    {
        if (s.Length != 8) return false;
        foreach (var c in s)
        {
            if (c is < '0' or > '9') return false;
        }
        return true;
    }

    /* Not a left-to-right split with a field limit, because BOTH the uploader
     * and the title routinely contain " - " themselves. The date and the id are
     * the only two fields with a checkable shape, so the parse anchors on
     * finding an 8-digit run immediately followed by an 11-character id and
     * works outwards from there. Everything left of the date is the uploader;
     * everything right of the id is the title, rejoined with its separators
     * intact.
     *
     * null when the name does not carry a date and an id, in which case the
     * caller falls back to the whole folder name -- the documented fallback,
     * pinned directly by the conformance suite. */
    public static Parsed? Parse(string name)
    {
        var parts = name.Split(" - ");
        if (parts.Length < 3) return null;

        for (var i = 1; i < parts.Length - 1; i++)
        {
            if (!IsEightDigits(parts[i])) continue;
            if (!IsVideoId(parts[i + 1])) continue;

            return new Parsed(
                Uploader: string.Join(" - ", parts[..i]),
                UploadDate: parts[i],
                Id: parts[i + 1],
                Title: string.Join(" - ", parts[(i + 2)..]));
        }
        return null;
    }
}

/// The index.
public sealed class ArchiveIndex
{
    public string Root { get; private set; } = "";
    public List<ArchiveEntry> Entries { get; private set; } = new();
    /// Uploader folder names that hold at least one video, sorted.
    public List<string> Channels { get; private set; } = new();

    private readonly Dictionary<string, int> _byKey = new(StringComparer.Ordinal);

    public ArchiveEntry? Entry(string? key)
    {
        if (string.IsNullOrEmpty(key)) return null;
        return _byKey.TryGetValue(key, out var idx) ? Entries[idx] : null;
    }

    public int VideoCount => Entries.Count;
    public int ChannelCount => Channels.Count;
    public long TotalBytes => Entries.Sum(e => e.TotalBytes);

    public sealed class ScanException : Exception
    {
        public ScanException(string message) : base(message) { }
    }

    /* Walk <root>/<Uploader>/<video folder>/ and build the index.
     *
     * Throws only when the root itself cannot be read. An individual unreadable
     * or malformed video folder is not an error: it is indexed from its folder
     * name, because that is the documented fallback and a real state that real
     * runs produce.
     *
     * BLOCKS. Call it off the UI thread -- `progress` is called once per video
     * folder on whichever thread called this. */
    public static ArchiveIndex Scan(string root, Action<int, int, string>? progress = null,
                                    CancellationToken cancel = default)
    {
        if (!Paths.IsDirectory(root))
        {
            throw new ScanException(
                $"The archive root {root} is not a directory. Point Settings at the same path " +
                "you would pass to `ytdl --path`.");
        }

        var index = new ArchiveIndex { Root = root };
        var (channels, relativePaths) = Discover(root);
        index.Channels = channels;

        for (var i = 0; i < relativePaths.Count; i++)
        {
            cancel.ThrowIfCancellationRequested();

            var rel = relativePaths[i];
            var slash = rel.IndexOf('/');
            if (slash < 0) continue;

            var channel = rel[..slash];
            var folder = rel[(slash + 1)..];

            var entry = BuildEntry(root, channel, folder);
            /* Last writer wins on a key collision, which two identical relative
             * paths cannot produce -- so this is only reachable via a SHA-256
             * truncation collision, and inserting anyway keeps the dictionary
             * and the list consistent. */
            index._byKey[entry.Key] = index.Entries.Count;
            index.Entries.Add(entry);

            progress?.Invoke(i + 1, relativePaths.Count, entry.Title);
        }

        return index;
    }

    /* Channel folders, then video folders inside each. "Channel Info" is a
     * channel-level asset directory, not a video, and is skipped by name. */
    private static (List<string> Channels, List<string> Rels) Discover(string root)
    {
        var channels = new List<string>();
        var rels = new List<string>();

        foreach (var channel in Paths.ListNames(root))
        {
            var cdir = Paths.Join(root, channel);
            if (!Paths.IsDirectory(cdir)) continue;

            var videos = new List<string>();
            foreach (var name in Paths.ListNames(cdir))
            {
                if (name == "Channel Info") continue;
                if (!Paths.IsDirectory(Paths.Join(cdir, name))) continue;
                videos.Add(name);
            }
            if (videos.Count == 0) continue;

            channels.Add(channel);
            foreach (var v in videos) rels.Add(channel + "/" + v);
        }

        return (channels, rels);
    }

    public static ArchiveEntry BuildEntry(string root, string channel, string folderName)
    {
        var rel = channel + "/" + folderName;
        var dir = Paths.Join(Paths.Join(root, channel), folderName);

        var entry = new ArchiveEntry
        {
            Key = Paths.Key(rel),
            Dir = dir,
            Rel = rel,
            Channel = channel,
        };

        /* The folder name FIRST, so that manifest and info.json are corrections
         * to a value that already exists rather than the only source. A missing
         * or unparseable info.json is a documented, ordinary state, and the
         * folder name is the documented fallback -- so the fallback is simply
         * always applied, and the good sources overwrite it. */
        var parsed = FolderName.Parse(folderName);
        if (parsed is not null)
        {
            entry.Uploader = parsed.Uploader;
            entry.UploadDate = parsed.UploadDate;
            entry.VideoId = parsed.Id;
            entry.Title = parsed.Title;
        }
        if (entry.Title.Length == 0) entry.Title = folderName;
        if (entry.Uploader.Length == 0) entry.Uploader = channel;

        var metaDir = Paths.Join(dir, "Video metadata");
        ApplyManifest(entry, metaDir);
        ApplyInfoJson(entry, metaDir);

        entry.Files = ListFiles(dir);
        entry.Files.Sort((a, b) => string.CompareOrdinal(a.Rel, b.Rel));
        return entry;
    }

    private static void ApplyManifest(ArchiveEntry entry, string metaDir)
    {
        var obj = JsonFile.Object(Paths.Join(metaDir, "manifest.json"));
        if (obj is null) return;
        var o = obj.Value;

        /* Absent means the video predates versioning, which the contract
         * defines as layout 1. Not an error, and not something to warn about. */
        entry.LayoutVersion = o.Int("archive_layout_version", 0);
        entry.LayoutTooNew = entry.LayoutVersion > ArchiveLayout.Supported;

        entry.MediaFile = o.Str("media_file");
        entry.DownloadMode = o.Str("download_mode");

        entry.VideoId ??= o.Str("video_id");
        if (entry.Title.Length == 0 && o.Str("title") is { } t) entry.Title = t;
        if (entry.Uploader.Length == 0 && o.Str("uploader") is { } u) entry.Uploader = u;
        entry.UploadDate ??= o.Str("upload_date");
        entry.OriginalUrl ??= o.Str("original_url");
        entry.ChannelUrl ??= o.Str("channel_url");

        /* run_settings may legitimately be absent -- a video written by a
         * standalone postprocess.ps1 invocation has none. */
        if (entry.DownloadMode is null)
        {
            entry.DownloadMode = o.Obj("run_settings").Str("mode");
        }
    }

    /* The richer fields live in yt-dlp's own info.json. Named
     * "<something>.info.json" rather than a fixed name, so the directory is
     * scanned for the suffix. */
    public static string? InfoJsonPath(string metaDir)
    {
        foreach (var name in Paths.ListNames(metaDir))
        {
            if (name.EndsWith(".info.json", StringComparison.OrdinalIgnoreCase))
                return Paths.Join(metaDir, name);
        }
        return null;
    }

    private static void ApplyInfoJson(ArchiveEntry entry, string metaDir)
    {
        var path = InfoJsonPath(metaDir);
        if (path is null) return;
        var obj = JsonFile.Object(path);
        if (obj is null) return;
        var o = obj.Value;

        entry.Duration = o.Double("duration", entry.Duration);
        entry.ViewCount = o.Int("view_count", entry.ViewCount);
        entry.Timestamp = o.Int("timestamp", entry.Timestamp);

        if (o.Str("title") is { } t) entry.Title = t;
        if (o.Str("id") is { } i) entry.VideoId = i;
        if (o.Str("upload_date") is { } d) entry.UploadDate = d;
        /* "uploader" is the display name; "channel" is the fallback some
         * extractors fill instead. */
        if (o.Str("uploader") is { } u) entry.Uploader = u;
        else if (o.Str("channel") is { } c) entry.Uploader = c;

        if (o.Str("channel_url") is { } cu) entry.ChannelUrl = cu;
        if (o.Str("webpage_url") is { } w) entry.OriginalUrl = w;
    }

    // MARK: - Listing a video folder

    private static List<ArchiveFile> ListFiles(string dir)
    {
        var files = new List<ArchiveFile>();
        Walk(dir, prefix: "", folder: "", depth: 0, files);
        return files;
    }

    private static void Walk(string dir, string prefix, string folder, int depth,
                             List<ArchiveFile> into)
    {
        /* The contract fixes the shape at <video folder>/<subfolder>/<file>.
         * Three levels is slack for a subfolder someone nests one deeper;
         * unbounded recursion into a user-chosen directory is not something to
         * offer. */
        if (depth > 3) return;

        foreach (var name in Paths.ListNames(dir))
        {
            var full = Paths.Join(dir, name);
            // '/'-separated, always. See the header.
            var rel = Paths.JoinRel(prefix, name);

            if (Paths.IsDirectory(full))
            {
                /* At depth 0 the child IS the top-level subfolder, and every
                 * file beneath it is attributed to that folder -- which is what
                 * makes "skip Pre-merge streams/" a single comparison later. */
                var childFolder = depth == 0 ? name : folder;
                Walk(full, rel, childFolder, depth + 1, into);
                continue;
            }
            if (!Paths.IsRegularFile(full)) continue;

            into.Add(new ArchiveFile
            {
                Rel = rel,
                Ext = ArchiveFile.ExtOf(rel),
                Folder = folder,
                Size = Paths.FileSize(full),
            });
        }
    }
}
