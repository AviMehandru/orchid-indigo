/* What is actually inside the media file, and whether this window can play it.
 *
 * The archive's whole point is that the ORIGINAL file is kept, so the useful
 * question on a video's page is "what did I actually get" -- 1080p VP9 with
 * Opus and three subtitle tracks, or a 360p fallback nobody noticed at the
 * time. That answer is in the container, not in info.json: info.json records
 * what yt-dlp was asked for and what it believed it fetched, while the file
 * records what is on disk now.
 *
 * WHY ffprobe AND NOT AN EBML PARSER. Hand-rolling Matroska would fit this
 * project's dependency principle and would literally "parse mkv" -- but the
 * --container option also produces .mp4 and .webm, and --mode audio-only
 * produces .m4a and .opus. A Matroska parser answers for one of those. ffprobe
 * answers for all of them, is already a documented dependency of the pipeline,
 * and is already probed for on the Health pane. The cost is honest and bounded:
 * no ffprobe, no stream details, and the page says so rather than going blank.
 *
 * Windows Media Foundation is NOT used for this. It would read an .mp4 without
 * spawning anything and then answer nothing useful for the containers whose
 * support depends on which Store media extensions happen to be installed --
 * which is precisely the set this pipeline produces most of.
 *
 * READ-ONLY, like everything else here. This spawns ffprobe. It never spawns
 * ffmpeg: nothing is re-encoded, remuxed or written.
 */

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace YtdlWin.Core;

public sealed class MediaStream
{
    /// video | audio | subtitle | attachment | data
    public required string Kind { get; init; }
    /// Short name, e.g. "vp9", "opus", "webvtt".
    public string? Codec { get; init; }
    public string? Profile { get; init; }
    /// From the track's language tag; null when untagged.
    public string? Language { get; init; }
    /// The track's own title, when it has one.
    public string? Title { get; init; }
    public required int Index { get; init; }

    // Video only; 0 when not applicable.
    public int Width { get; init; }
    public int Height { get; init; }
    public double Fps { get; init; }

    // Audio only; 0 when not applicable.
    public int Channels { get; init; }
    public int SampleRate { get; init; }

    /// Bits/sec, 0 when the container does not say.
    public long BitRate { get; init; }
    public bool IsDefault { get; init; }

    /// An attached cover image. ffprobe reports one as a one-frame video
    /// stream, which is exactly how the webview build ended up serving
    /// thumbnails with a video MIME type -- so it is flagged rather than
    /// counted as video.
    public bool AttachedPic { get; init; }
}

public sealed class MediaChapter
{
    public required string Title { get; init; }
    public required double Start { get; init; }
    public required double End { get; init; }
}

public sealed class MediaProbe
{
    /// false when ffprobe is missing or the file is unreadable.
    public bool Ok { get; set; }
    /// Why, when !Ok -- shown to the user verbatim.
    public string? Error { get; set; }
    /// Container long name.
    public string? ContainerFormat { get; set; }
    public double Duration { get; set; }
    public long Size { get; set; }
    public long BitRate { get; set; }
    public List<MediaStream> Streams { get; set; } = new();
    public List<MediaChapter> Chapters { get; set; } = new();
}

public static class Media
{
    /* Runs ffprobe and parses its JSON. BLOCKS -- call it off the UI thread.
     * Never throws: a failure comes back as a probe with Ok = false and a
     * message, because "ffprobe is not installed" is something the page should
     * say rather than something it should hide. */
    public static MediaProbe Probe(string? path)
    {
        if (path is null || !Paths.IsRegularFile(path))
            return Failed("There is no media file in this folder to inspect.");

        var ffprobe = Paths.FindPipelineTool("ffprobe");
        if (ffprobe is null)
        {
            return Failed(
                "ffprobe is not installed, so the stream details cannot be read. Everything " +
                "else on this page still works, and the file itself is untouched. Install " +
                "ffmpeg to get this section back.");
        }

        using var process = new Process();
        var psi = process.StartInfo;
        psi.FileName = ffprobe;
        foreach (var a in new[]
        {
            "-v", "quiet", "-print_format", "json",
            "-show_format", "-show_streams", "-show_chapters",
            /* The path is passed as its own argument list element, so a title
             * with a space or a quote in it is quoted by .NET's own argument
             * encoder rather than by hand. Every filename in this archive is a
             * video title, so this is the common case rather than the edge. */
            path,
        }) psi.ArgumentList.Add(a);

        psi.UseShellExecute = false;
        psi.CreateNoWindow = true;
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError = true;
        psi.Environment["PATH"] = Paths.ChildPath();

        /* ffprobe writes UTF-8. Without this, the StreamReader decodes with the
         * console's output code page -- which on a default Windows install is
         * 437 or 1252, not UTF-8 -- and every non-ASCII character in a chapter
         * name or a track title comes back mangled. Every filename in this
         * archive is a video title, so that is the common case rather than an
         * edge one. The pump in ProcessTree does not need this because it reads
         * raw bytes and decodes them itself. */
        psi.StandardOutputEncoding = System.Text.Encoding.UTF8;
        psi.StandardErrorEncoding = System.Text.Encoding.UTF8;

        string json;
        try
        {
            process.Start();
            /* stdout is read to the end BEFORE waiting. Waiting first with a
             * full pipe is the deadlock every redirected-process bug is made
             * of, and ffprobe's JSON for a video with a large chapter list is
             * comfortably past a pipe buffer. */
            json = process.StandardOutput.ReadToEnd();
            if (!process.WaitForExit(30_000))
            {
                try { process.Kill(entireProcessTree: true); } catch (Exception) { }
                return Failed("ffprobe did not answer within 30 seconds and was stopped.");
            }
        }
        catch (Exception ex)
        {
            return Failed($"Could not run ffprobe: {ex.Message}");
        }

        if (process.ExitCode != 0)
        {
            return Failed(
                "ffprobe could not read this file. It may be truncated — an interrupted run " +
                "leaves a partial media file behind, which is a state the archive layout says " +
                "to tolerate rather than treat as corruption.");
        }

        var obj = JsonFile.ObjectFrom(json);
        if (obj is null)
            return Failed("ffprobe returned something that is not the JSON it was asked for.");

        var p = new MediaProbe { Ok = true };
        var root = obj.Value;

        if (root.Obj("format") is { } f)
        {
            p.ContainerFormat = f.Str("format_long_name");
            p.Duration = f.Double("duration");
            p.Size = Math.Max(0, f.Int("size"));
            p.BitRate = f.Int("bit_rate");
        }
        p.Streams = root.Objects("streams").Select(ReadStream).ToList();
        p.Chapters = root.Objects("chapters").Select(ReadChapter).ToList();
        p.ContainerFormat ??= "unknown container";
        return p;
    }

    private static MediaProbe Failed(string message) => new() { Ok = false, Error = message };

    private static MediaStream ReadStream(System.Text.Json.JsonElement o)
    {
        var tags = o.Obj("tags");
        var language = tags.Str("language");
        var disposition = o.Obj("disposition");

        return new MediaStream
        {
            Kind = o.Str("codec_type") ?? "data",
            Codec = o.Str("codec_name"),
            Profile = o.Str("profile"),
            /* "und" is Matroska's "undetermined", which is not a language and
             * should not be shown as though it were one. */
            Language = language == "und" ? null : language,
            Title = tags.Str("title"),
            Index = (int)o.Int("index"),
            Width = (int)o.Int("width"),
            Height = (int)o.Int("height"),
            /* avg_frame_rate is an exact rational ("30000/1001"), which is the
             * honest representation and useless to display. */
            Fps = Rational(o.Str("avg_frame_rate")),
            Channels = (int)o.Int("channels"),
            SampleRate = (int)o.Int("sample_rate"),
            BitRate = o.Int("bit_rate"),
            IsDefault = disposition.Int("default") != 0,
            AttachedPic = disposition.Int("attached_pic") != 0,
        };
    }

    private static MediaChapter ReadChapter(System.Text.Json.JsonElement o) => new()
    {
        Title = o.Obj("tags").Str("title") ?? "(untitled chapter)",
        Start = o.Double("start_time"),
        End = o.Double("end_time"),
    };

    public static double Rational(string? s)
    {
        if (string.IsNullOrEmpty(s)) return 0;
        var slash = s.IndexOf('/');
        if (slash < 0)
        {
            return double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var v)
                ? v : 0;
        }
        var n = double.TryParse(s[..slash], NumberStyles.Float, CultureInfo.InvariantCulture, out var a) ? a : 0;
        var d = double.TryParse(s[(slash + 1)..], NumberStyles.Float, CultureInfo.InvariantCulture, out var b) ? b : 0;
        return d != 0 ? n / d : 0;
    }

    /// A one-line summary for a header: "1080p · vp9 / opus · 9:47 · 412 MB".
    public static string Summary(MediaProbe p)
    {
        if (!p.Ok) return "";

        MediaStream? video = null, audio = null;
        var subs = 0;
        foreach (var s in p.Streams)
        {
            if (s.Kind == "video" && !s.AttachedPic && video is null) video = s;
            else if (s.Kind == "audio" && audio is null) audio = s;
            else if (s.Kind == "subtitle") subs++;
        }

        var parts = new List<string>();
        if (video is { Height: > 0 }) parts.Add($"{video.Height}p");
        if (video is not null || audio is not null)
        {
            var codecs = video?.Codec ?? "—";
            if (audio?.Codec is { } ac) codecs += $" / {ac}";
            parts.Add(codecs);
        }
        if (p.Duration > 0) parts.Add(Format.Clock(p.Duration));
        if (p.Size > 0) parts.Add(Format.Bytes(p.Size));

        var output = string.Join(" · ", parts);
        if (subs > 0) output += $" · {subs} subtitle track{(subs == 1 ? "" : "s")}";
        return output;
    }

    // MARK: - Playback

    /* WINDOWS SITS BETWEEN THE OTHER TWO, AND THE STRATEGY IS DIFFERENT BECAUSE
     * OF IT.
     *
     * The GTK app plays everything: GStreamer reads the pipeline's default
     * Matroska directly. The macOS app can play almost none of it: AVFoundation
     * does not read Matroska at all, and because that answer is crisp and
     * permanent, that app decides from a static extension list and shows an
     * honest note for the rest.
     *
     * Neither model fits here. Windows Media Foundation reads MP4/H.264/AAC
     * in-box and has read Matroska since Windows 10 -- but VP9, Opus, AV1 and
     * HEVC each depend on a media extension package from the Store that may or
     * may not be present, may be in-box on one Windows build and not another,
     * and in HEVC's case is a paid item. So "can this play?" has no answer that
     * can be decided from the extension, which is what the static list on macOS
     * relies on.
     *
     * SO THIS APP DOES NOT PREDICT. The player is handed the file, and if the
     * platform cannot decode it, MediaFailed says so and the page swaps in the
     * honest note. Letting the platform answer a question only the platform can
     * answer is both more correct than a guess and less likely to go stale as
     * Windows changes which decoders ship in the box.
     *
     * What the probe below IS for is the WORDING of that note. Because ffprobe
     * has already read the container by the time a failure arrives, the note
     * can name the actual codec that failed -- "this is AV1, which needs the
     * AV1 Video Extension" -- instead of the vague "cannot be opened" the media
     * stack produces. That is a better message than either of the other two
     * apps can give, and it is free.
     *
     * What it must NOT do, on any platform, is quietly remux into a cache
     * directory. That is the apparatus this project deleted, and it would be
     * writing derived state for something nobody asked for. */
    private static readonly Dictionary<string, string> CodecNotes = new(StringComparer.OrdinalIgnoreCase)
    {
        ["av1"] = "AV1 needs the free “AV1 Video Extension” from the Microsoft Store; " +
                  "Windows does not ship an AV1 decoder in the box.",
        ["libaom-av1"] = "AV1 needs the free “AV1 Video Extension” from the Microsoft Store; " +
                         "Windows does not ship an AV1 decoder in the box.",
        ["hevc"] = "HEVC needs the “HEVC Video Extensions” from the Microsoft Store, which is " +
                   "a paid item on most machines.",
        ["vp9"] = "VP9 needs the free “Web Media Extensions” from the Microsoft Store on some " +
                  "Windows builds.",
        ["vp8"] = "VP8 needs the free “Web Media Extensions” from the Microsoft Store on some " +
                  "Windows builds.",
        ["opus"] = "Opus audio needs the free “Web Media Extensions” from the Microsoft Store " +
                   "on some Windows builds.",
        ["vorbis"] = "Vorbis audio needs the free “Web Media Extensions” from the Microsoft " +
                     "Store on some Windows builds.",
        ["flac"] = "FLAC in this container is not decoded by every Windows build.",
    };

    /// <summary>A sentence saying why the player could not show this file,
    /// naming the codec when the probe knows it and the way out either way.
    /// Called only AFTER the media stack has actually refused the file.</summary>
    public static string PlaybackFailedNote(MediaProbe probe, string? path, string? platformMessage)
    {
        var sb = new StringBuilder();

        string? culprit = null;
        if (probe.Ok)
        {
            foreach (var s in probe.Streams)
            {
                if (s.AttachedPic) continue;
                if (s.Kind != "video" && s.Kind != "audio") continue;
                if (s.Codec is not null && CodecNotes.ContainsKey(s.Codec))
                {
                    culprit = s.Codec;
                    break;
                }
            }
        }

        if (culprit is not null)
        {
            sb.Append("Windows could not decode this file. ")
              .Append(CodecNotes[culprit]);
        }
        else
        {
            var ext = path is null ? "" : ArchiveFile.ExtOf(path);
            var name = ext.Length == 0 ? "this file" : $"a {ext[1..]} file";
            sb.Append($"Windows could not play {name} in this window.");
            if (!string.IsNullOrWhiteSpace(platformMessage))
                sb.Append(" The media stack said: ").Append(platformMessage.Trim()).Append('.');
        }

        sb.Append(" The file itself is fine and is untouched — open it in mpv or VLC, which ")
          .Append("read every codec combination this pipeline produces without needing anything ")
          .Append("from the Store. Downloading with --container mp4 --codec avc1 gives you ")
          .Append("in-window playback for future videos.");
        return sb.ToString();
    }
}

/* Opening things in something else.
 *
 * mpv and VLC are looked for on PATH and in their standard install directories,
 * and via the App Paths registry key -- which is how a Windows application
 * declares "here is my executable" without being on PATH, and is what makes a
 * VLC installed by its own installer findable at all. The macOS port asks
 * LaunchServices for a bundle id, which is the same idea in that platform's
 * spelling. */
public static class ExternalOpen
{
    private sealed record PlayerSpec(string Display, string Exe, string[] Fallbacks);

    private static PlayerSpec[] Players()
    {
        var programFiles = Paths.Env("ProgramFiles") ?? @"C:\Program Files";
        var programFilesX86 = Paths.Env("ProgramFiles(x86)") ?? @"C:\Program Files (x86)";
        var home = Paths.HomeDirectory();

        return new[]
        {
            new PlayerSpec("mpv", "mpv.exe", new[]
            {
                Paths.Join(programFiles, @"mpv\mpv.exe"),
                Paths.Join(home, @"scoop\apps\mpv\current\mpv.exe"),
                Paths.Join(home, @"scoop\shims\mpv.exe"),
            }),
            new PlayerSpec("VLC", "vlc.exe", new[]
            {
                Paths.Join(programFiles, @"VideoLAN\VLC\vlc.exe"),
                Paths.Join(programFilesX86, @"VideoLAN\VLC\vlc.exe"),
            }),
            new PlayerSpec("MPC-HC", "mpc-hc64.exe", new[]
            {
                Paths.Join(programFiles, @"MPC-HC\mpc-hc64.exe"),
                Paths.Join(programFilesX86, @"MPC-HC\mpc-hc.exe"),
            }),
        };
    }

    /// The first installed player that reads this pipeline's containers, or
    /// null. Returns the path so the caller can both name it and run it.
    public static (string Display, string Path)? FindPlayer()
    {
        foreach (var spec in Players())
        {
            var onPath = Paths.Which(spec.Exe);
            if (onPath is not null) return (spec.Display, onPath);

            foreach (var candidate in spec.Fallbacks)
            {
                if (Paths.IsRegularFile(candidate)) return (spec.Display, candidate);
            }

            var viaRegistry = AppPathsLookup(spec.Exe);
            if (viaRegistry is not null) return (spec.Display, viaRegistry);
        }
        return null;
    }

    /* HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\<exe>, then the
     * per-user hive. This is the documented place an installed application
     * registers its executable, and it is what `start vlc` uses. Wrapped
     * because the registry APIs throw on a locked-down machine and a missing
     * media player is not worth taking the page down for. */
    private static string? AppPathsLookup(string exeName)
    {
        if (!OperatingSystem.IsWindows()) return null;

        foreach (var hive in new[]
        {
            Microsoft.Win32.Registry.CurrentUser,
            Microsoft.Win32.Registry.LocalMachine,
        })
        {
            try
            {
                using var key = hive.OpenSubKey(
                    $@"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\{exeName}");
                if (key?.GetValue(null) is string raw)
                {
                    var path = raw.Trim('"');
                    if (Paths.IsRegularFile(path)) return path;
                }
            }
            catch (Exception) { /* no access, or no such key */ }
        }
        return null;
    }

    /// Open a media file in a player that can actually decode it, falling back
    /// to whatever the system would use.
    public static void InPlayer(string path)
    {
        if (FindPlayer() is { } player)
        {
            Launch(player.Path, new[] { path });
            return;
        }
        // No known player: hand it to the shell, which opens whatever is
        // registered for the extension. That may itself be a player that
        // cannot decode it, which is why this is the last resort.
        ShellOpen(path);
    }

    /// Show the folder in Explorer with the file SELECTED, which is what a
    /// Windows user means by "show me where this is" -- the same intent as
    /// Reveal in Finder, and not the same as opening the folder.
    public static void RevealInExplorer(string path)
    {
        try
        {
            /* /select, needs the path quoted and, unusually, does NOT accept a
             * trailing separator or a forward slash -- explorer.exe parses its
             * own command line rather than using CommandLineToArgvW, which is
             * why this one place builds an argument string by hand instead of
             * using ArgumentList. */
            var native = Paths.Canonical(path);
            Process.Start(new ProcessStartInfo
            {
                FileName = "explorer.exe",
                Arguments = $"/select,\"{native}\"",
                UseShellExecute = true,
            });
        }
        catch (Exception)
        {
            // If the file is gone, at least open its folder.
            var dir = Path.GetDirectoryName(path);
            if (dir is not null) ShellOpen(dir);
        }
    }

    public static void OpenFolder(string path) => ShellOpen(path);

    private static void ShellOpen(string path)
    {
        try
        {
            Process.Start(new ProcessStartInfo { FileName = path, UseShellExecute = true });
        }
        catch (Exception) { /* nothing registered, or the user cancelled */ }
    }

    private static void Launch(string exe, IReadOnlyList<string> args)
    {
        try
        {
            var psi = new ProcessStartInfo { FileName = exe, UseShellExecute = false };
            foreach (var a in args) psi.ArgumentList.Add(a);
            Process.Start(psi);
        }
        catch (Exception) { /* the player vanished between the probe and the click */ }
    }
}
