/* What a run IS: the option set, the command line it becomes, and the parsers
 * that turn the pipeline's output back into something to show.
 *
 * This does NOT reimplement any part of the pipeline. It builds a `ytdl`
 * command line and hands it to ytdl.ps1 -- the single argument parser, on every
 * platform -- exactly as a terminal would. Every option below maps one-to-one
 * onto a flag ytdl.ps1 already accepts, and nothing here knows what
 * run_ytdlp.ps1 or postprocess.ps1 do with them.
 *
 * Why ytdl.ps1 and not run_ytdlp.ps1 directly: because then there would be two
 * argument surfaces to keep in agreement, which is the exact problem the
 * pipeline collapsed into one file. A flag added to ytdl.ps1 becomes available
 * here by adding a control, and a flag it rejects fails the same way it fails
 * in a terminal.
 *
 * Validation is deliberately NOT duplicated here. ytdl.ps1 checks these at the
 * point they reach it and exits non-zero naming the option, which surfaces in
 * the run log like any other pipeline error. A second copy of the
 * accepted-value lists in C# would be a second thing to keep in step across two
 * repositories, for nothing the user can see.
 */

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.Json;

namespace YtdlWin.Core;

// MARK: - Options

/// Every field maps to exactly one ytdl.ps1 flag. Empty and 0 mean "not set"
/// throughout, which is what keeps a plain download producing exactly the
/// command line it produced before these options existed.
public sealed class RunOptions
{
    public string Url { get; set; } = "";
    public string DataRoot { get; set; } = "";

    public bool Sync { get; set; }
    public string Items { get; set; } = "";
    public string After { get; set; } = "";
    public bool Lazy { get; set; }
    /// 0 = unset; only emitted when > 1.
    public int Workers { get; set; }
    public bool NoPot { get; set; }
    public bool SkipPotUpdate { get; set; }
    /// 0 = unset.
    public int PotPort { get; set; }

    /* Content options -- the only ones that change what ends up in the archive
     * rather than how the session is scheduled. The GUI deliberately does not
     * build a yt-dlp format selector or decide what audio-only means; all of
     * that lives in run_ytdlp.ps1, on the far side of the CLI_VERSION pin. */
    /// full | video-only | audio-only | metadata-only | comments-only | subs-only
    public string Mode { get; set; } = "";
    /// a height in pixels, or "best"
    public string Quality { get; set; } = "";
    /// any | avc1 | vp9 | av01
    public string Codec { get; set; } = "";
    /// any | opus | aac | mp3 | flac
    public string AudioCodec { get; set; } = "";
    /// mkv | mp4 | webm
    public string Container { get; set; } = "";
    public bool NoComments { get; set; }
    public bool NoSubs { get; set; }
    public bool NoThumbnail { get; set; }
    public bool NoMetadata { get; set; }

    /// Each emitted as its own --ytdlp-arg.
    public List<string> YtdlpArgs { get; set; } = new();

    public RunOptions Clone() => new()
    {
        Url = Url, DataRoot = DataRoot, Sync = Sync, Items = Items, After = After,
        Lazy = Lazy, Workers = Workers, NoPot = NoPot, SkipPotUpdate = SkipPotUpdate,
        PotPort = PotPort, Mode = Mode, Quality = Quality, Codec = Codec,
        AudioCodec = AudioCodec, Container = Container, NoComments = NoComments,
        NoSubs = NoSubs, NoThumbnail = NoThumbnail, NoMetadata = NoMetadata,
        YtdlpArgs = new List<string>(YtdlpArgs),
    };

    private static bool IsSet(string s) => !string.IsNullOrWhiteSpace(s);

    /* Emitted only when it differs from the pipeline's own default, so a plain
     * download produces exactly the command line it produced before these
     * options existed. That matters for more than tidiness: --quality best,
     * --codec any and --mode full are no-ops the pipeline would accept and
     * ignore, but emitting them would put four extra flags in the preview of
     * every ordinary download and make the common case look complicated. */
    private static void PushNonDefault(List<string> v, string flag, string value, string dflt)
    {
        if (!IsSet(value)) return;
        var t = value.Trim();
        if (string.Equals(t, dflt, StringComparison.Ordinal)) return;
        v.Add(flag);
        v.Add(t);
    }

    /* Everything after the script path. The URL is always first and always a
     * full URL: ytdl.ps1 accepts a bare 11-character id, but a leading-hyphen
     * id would be bound as a parameter before the script ever saw it, and about
     * one YouTube id in thirty starts with "-" or "_". */
    public List<string> ToArgs()
    {
        var v = new List<string> { NormalizeUrl(Url) };

        if (IsSet(DataRoot))
        {
            /* Expand ~ HERE, at the one point a path leaves this process.
             *
             * run_ytdlp.ps1 resolves -DataRoot with
             * [System.IO.Path]::GetFullPath, which has no notion of a home
             * directory: "~/Videos" reaches it as a literal "~" folder under
             * the pipeline process's working directory. Meanwhile this app's
             * own ExpandTilde DID expand it, so the same typed path pointed at
             * two different places -- the library indexed one and the downloads
             * went to the other, with nothing reporting an error. */
            v.Add("--path");
            v.Add(Paths.ExpandTilde(DataRoot.Trim()));
        }
        if (Sync) v.Add("--sync");
        if (IsSet(Items)) { v.Add("--items"); v.Add(Items.Trim()); }
        if (IsSet(After)) { v.Add("--after"); v.Add(After.Trim()); }
        if (Lazy) v.Add("--lazy");
        if (Workers > 1)
        {
            v.Add("--workers");
            v.Add(Workers.ToString(CultureInfo.InvariantCulture));
        }
        if (NoPot) v.Add("--no-pot");
        if (SkipPotUpdate) v.Add("--skip-pot-update");
        if (PotPort > 0)
        {
            v.Add("--pot-port");
            v.Add(PotPort.ToString(CultureInfo.InvariantCulture));
        }

        PushNonDefault(v, "--mode", Mode, "full");
        PushNonDefault(v, "--quality", Quality, "best");
        PushNonDefault(v, "--codec", Codec, "any");
        PushNonDefault(v, "--audio-codec", AudioCodec, "any");
        PushNonDefault(v, "--container", Container, "mkv");

        if (NoComments) v.Add("--no-comments");
        if (NoSubs) v.Add("--no-subs");
        if (NoThumbnail) v.Add("--no-thumbnail");
        if (NoMetadata) v.Add("--no-metadata");

        /* Repeated rather than joined: ytdl.ps1 takes one value per occurrence,
         * and a real --match-filter expression contains commas and spaces, so
         * no separator would be safe to join on. */
        foreach (var raw in YtdlpArgs)
        {
            if (!IsSet(raw)) continue;
            v.Add("--ytdlp-arg");
            v.Add(raw.Trim());
        }

        return v;
    }

    /* What the equivalent terminal command would be. Shown above the Add
     * button, because a GUI that hides the command it runs makes the CLI harder
     * to learn rather than easier -- and every problem report is easier to
     * answer when the user can paste the exact line the window would have run.
     *
     * QUOTED FOR POWERSHELL, not for cmd.exe, and this is a real decision
     * rather than a copy of the macOS one. The thing a user would paste this
     * into is a pwsh prompt, because `ytdl` IS a pwsh script and the shim that
     * launches it is on PATH for pwsh. So a double quote inside a value is
     * doubled -- pwsh's own escape inside a double-quoted string -- rather than
     * backslash-escaped, which is what a POSIX shell would want and what would
     * quietly produce the wrong argument here. */
    public string CommandPreview()
    {
        var s = new StringBuilder("ytdl");
        var args = ToArgs();
        for (var i = 0; i < args.Count; i++)
        {
            var arg = args[i];
            s.Append(' ');
            if (i == 0 || arg.Any(c => c is ' ' or '\t' or '"' or '\'' or '`' or '$' or ';'))
            {
                s.Append('"').Append(arg.Replace("\"", "\"\"")).Append('"');
            }
            else
            {
                s.Append(arg);
            }
        }
        return s.ToString();
    }

    /* A bare video id becomes a watch URL; anything already URL-shaped is left
     * exactly as typed. Deliberately not a validator -- ytdl.ps1 has its own
     * "that does not look like a URL" warning and yt-dlp's extractor is the
     * real authority on what is downloadable. */
    public static string NormalizeUrl(string url)
    {
        var u = url.Trim();
        if (u.Contains("://", StringComparison.Ordinal)) return u;

        var looksLikeId = u.Length == 11 && u.All(c =>
            c is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or >= '0' and <= '9' or '-' or '_');
        if (looksLikeId) return "https://www.youtube.com/watch?v=" + u;

        foreach (var prefix in new[] { "youtube.com", "www.youtube.com", "youtu.be" })
        {
            if (u.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) return "https://" + u;
        }
        return u;
    }

    // MARK: Serialisation

    /* Shared with the profile store and with the queue and history files.
     *
     * There is deliberately NO second list of profileable fields anywhere: a
     * field added to RunOptions becomes profileable by being added here, and
     * there is no other place to forget it. */
    public void WriteTo(Utf8JsonWriter w)
    {
        w.WriteStartObject();
        WriteIfSet(w, "url", Url);
        WriteIfSet(w, "data_root", DataRoot);
        w.WriteBoolean("sync", Sync);
        WriteIfSet(w, "items", Items);
        WriteIfSet(w, "after", After);
        w.WriteBoolean("lazy", Lazy);
        w.WriteNumber("workers", Workers);
        w.WriteBoolean("no_pot", NoPot);
        w.WriteBoolean("skip_pot_update", SkipPotUpdate);
        w.WriteNumber("pot_port", PotPort);
        WriteIfSet(w, "mode", Mode);
        WriteIfSet(w, "quality", Quality);
        WriteIfSet(w, "codec", Codec);
        WriteIfSet(w, "audio_codec", AudioCodec);
        WriteIfSet(w, "container", Container);
        w.WriteBoolean("no_comments", NoComments);
        w.WriteBoolean("no_subs", NoSubs);
        w.WriteBoolean("no_thumbnail", NoThumbnail);
        w.WriteBoolean("no_metadata", NoMetadata);

        w.WriteStartArray("ytdlp_args");
        foreach (var a in YtdlpArgs) w.WriteStringValue(a);
        w.WriteEndArray();
        w.WriteEndObject();
    }

    private static void WriteIfSet(Utf8JsonWriter w, string name, string value)
    {
        if (!string.IsNullOrEmpty(value)) w.WriteString(name, value);
    }

    /* Every field is optional on read. A queue, history or profiles file
     * written by an older build simply does not set what it did not know about
     * -- which is the reason a profile or a queued run survives an upgrade
     * instead of failing the whole file. */
    public static RunOptions FromJson(JsonElement? obj)
    {
        var o = new RunOptions();
        if (obj is null) return o;
        var e = obj.Value;

        o.Url = e.Str("url") ?? "";
        o.DataRoot = e.Str("data_root") ?? "";
        o.Sync = e.Bool("sync");
        o.Items = e.Str("items") ?? "";
        o.After = e.Str("after") ?? "";
        o.Lazy = e.Bool("lazy");
        o.Workers = (int)e.Int("workers");
        o.NoPot = e.Bool("no_pot");
        o.SkipPotUpdate = e.Bool("skip_pot_update");
        o.PotPort = (int)e.Int("pot_port");
        o.Mode = e.Str("mode") ?? "";
        o.Quality = e.Str("quality") ?? "";
        o.Codec = e.Str("codec") ?? "";
        o.AudioCodec = e.Str("audio_codec") ?? "";
        o.Container = e.Str("container") ?? "";
        o.NoComments = e.Bool("no_comments");
        o.NoSubs = e.Bool("no_subs");
        o.NoThumbnail = e.Bool("no_thumbnail");
        o.NoMetadata = e.Bool("no_metadata");
        o.YtdlpArgs = e.Strings("ytdlp_args");
        return o;
    }
}

// MARK: - Progress

public sealed class RunProgress
{
    /// &lt; 0 when unknown.
    public double Percent { get; set; } = -1;
    public string? Speed { get; set; }
    public string? Eta { get; set; }
    public string? Total { get; set; }
    public string? Stage { get; set; }
    public string? VideoId { get; set; }
    public string? Destination { get; set; }

    public RunProgress Clone() => new()
    {
        Percent = Percent, Speed = Speed, Eta = Eta, Total = Total,
        Stage = Stage, VideoId = VideoId, Destination = Destination,
    };
}

// MARK: - A queued or finished run

public sealed class RunRecord
{
    public string Id { get; set; } = "";
    public string Command { get; set; } = "";
    public long Started { get; set; }
    /// 0 while running.
    public long Finished { get; set; }
    /// queued | running | done | failed | cancelled
    public string State { get; set; } = "queued";
    public int ExitCode { get; set; }
    /// -1 = not reported. The four counts exist ONLY in run_ytdlp.ps1's session
    /// summary line; a run that was cancelled or died early never printed one.
    public long VideosTouched { get; set; } = -1;
    public long ArchiveSkipped { get; set; } = -1;
    public long Errors { get; set; } = -1;
    public long Warnings { get; set; } = -1;
    public string LogPath { get; set; } = "";
    public string LastLine { get; set; } = "";
    public RunOptions Opts { get; set; } = new();

    public RunRecord Clone() => new()
    {
        Id = Id, Command = Command, Started = Started, Finished = Finished, State = State,
        ExitCode = ExitCode, VideosTouched = VideosTouched, ArchiveSkipped = ArchiveSkipped,
        Errors = Errors, Warnings = Warnings, LogPath = LogPath, LastLine = LastLine,
        Opts = Opts.Clone(),
    };

    public void WriteTo(Utf8JsonWriter w)
    {
        w.WriteStartObject();
        w.WriteString("id", Id);
        w.WriteString("command", Command);
        w.WriteNumber("started", Started);
        w.WriteNumber("finished", Finished);
        w.WriteString("state", State);
        w.WriteNumber("exit_code", ExitCode);
        w.WriteNumber("videos_touched", VideosTouched);
        w.WriteNumber("archive_skipped", ArchiveSkipped);
        w.WriteNumber("errors", Errors);
        w.WriteNumber("warnings", Warnings);
        w.WriteString("log_path", LogPath);
        w.WriteString("last_line", LastLine);
        w.WritePropertyName("opts");
        Opts.WriteTo(w);
        w.WriteEndObject();
    }

    public static RunRecord FromJson(JsonElement o) => new()
    {
        Id = o.Str("id") ?? "",
        Command = o.Str("command") ?? "",
        Started = o.Int("started"),
        Finished = o.Int("finished"),
        State = o.Str("state") ?? "done",
        ExitCode = (int)o.Int("exit_code"),
        VideosTouched = o.Int("videos_touched", -1),
        ArchiveSkipped = o.Int("archive_skipped", -1),
        Errors = o.Int("errors", -1),
        Warnings = o.Int("warnings", -1),
        LogPath = o.Str("log_path") ?? "",
        LastLine = o.Str("last_line") ?? "",
        Opts = RunOptions.FromJson(o.Obj("opts")),
    };
}

// MARK: - Output parsing

/* The two parsers that turn the pipeline's stdout into everything the UI shows,
 * and the place a change in yt-dlp's output would first bite. Both are pure
 * functions over one line so the suite can pin them without a download. */
public static class OutputParser
{
    /* Strip ANSI escape sequences. yt-dlp turns colour off when stdout is not a
     * terminal, but pwsh does not always, and a progress line full of escape
     * bytes renders as garbage in a label.
     *
     * This matters MORE on Windows than on the other two platforms, and not
     * less as one might assume: PowerShell 7.2 and later enable virtual
     * terminal processing and emit ANSI colour by default on Windows 10+, so
     * the pipeline's own Write-Host output arrives here with escapes in it even
     * when yt-dlp's does not. */
    /// U+001B, written as an escape rather than as a literal control byte: a
    /// raw ESC in a source file survives a compiler and does not survive an
    /// editor, a diff viewer or a copy through a terminal.
    private const char Esc = '\u001b';

    public static string StripAnsi(string s)
    {
        if (s.IndexOf(Esc) < 0) return s;

        var output = new StringBuilder(s.Length);
        var i = 0;
        while (i < s.Length)
        {
            if (s[i] == Esc)
            {
                i++;
                if (i < s.Length && s[i] == '[')
                {
                    i++;
                    while (i < s.Length && !char.IsAsciiLetter(s[i])) i++;
                    if (i < s.Length) i++;
                }
                continue;
            }
            output.Append(s[i]);
            i++;
        }
        return output.ToString();
    }

    private static readonly (string Needle, string Stage)[] Stages =
    {
        ("[Merger]", "merging"),
        ("[Metadata]", "embedding metadata"),
        ("[EmbedSubtitle]", "embedding subtitles"),
        ("[ThumbnailsConvertor]", "thumbnail"),
        ("[postprocess]", "post-processing"),
        ("Fetching comments", "fetching comments"),
        ("[info] Writing video subtitles", "subtitles"),
        ("-- Enumerating videos", "enumerating"),
    };

    /* yt-dlp's own progress line, e.g.
     *   [download]  45.2% of  120.00MiB at   2.00MiB/s ETA 00:30
     * Scanned by token rather than with a regular expression, which keeps the
     * cost per line low: a long run produces thousands of these a minute.
     * Returns true when @p was changed. */
    public static bool ParseProgressLine(string line, RunProgress p)
    {
        var trimmed = line.Trim();

        if (trimmed.StartsWith("[download]", StringComparison.Ordinal))
        {
            var rest = trimmed["[download]".Length..];
            var rtrim = rest.Trim();

            if (rtrim.StartsWith("Destination:", StringComparison.Ordinal))
            {
                p.Destination = rtrim["Destination:".Length..].Trim();
                p.Stage = "downloading";
                return true;
            }

            /* Runs of spaces are normal in this line ("45.2% of  120.00MiB at
             * 2.00MiB/s"), so empty tokens are dropped rather than compacted in
             * place. The C port had a double free here from compacting the
             * token array; C# cannot have that bug, but the shape of the parse
             * is the same one and worth keeping recognisable. */
            var tokens = rest.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            var matched = false;

            for (var i = 0; i < tokens.Length; i++)
            {
                var tok = tokens[i];
                if (tok.Length > 1 && tok[^1] == '%' &&
                    double.TryParse(tok[..^1], NumberStyles.Float,
                                    CultureInfo.InvariantCulture, out var v))
                {
                    p.Percent = v;
                    p.Stage = "downloading";
                    matched = true;
                }
                if (i + 1 < tokens.Length)
                {
                    switch (tok)
                    {
                        case "of": p.Total = tokens[i + 1]; break;
                        case "at": p.Speed = tokens[i + 1]; break;
                        case "ETA": p.Eta = tokens[i + 1]; break;
                    }
                }
            }
            if (matched) return true;
        }

        /* Stage markers worth surfacing, all emitted by the pipeline itself or
         * by yt-dlp's own extractor chatter. The percentage is CLEARED with
         * each: 45% of the download is not 45% of the merge, and leaving the
         * old number up reads as a stalled bar. */
        foreach (var (needle, stage) in Stages)
        {
            if (!line.Contains(needle, StringComparison.Ordinal)) continue;
            p.Stage = stage;
            p.Percent = -1;
            return true;
        }

        /* "[youtube] dQw4w9WgXcQ: Downloading webpage" -- the id of the video
         * the session is currently on, which is the only reliable per-video
         * marker in a multi-video run. */
        if (trimmed.StartsWith("[youtube] ", StringComparison.Ordinal))
        {
            var rest = trimmed["[youtube] ".Length..];
            var colon = rest.IndexOf(':');
            if (colon > 0)
            {
                var id = rest[..colon].Trim();
                if (id.Length == 11 && !id.Contains(' '))
                {
                    p.VideoId = id;
                    return true;
                }
            }
        }

        return false;
    }

    public readonly record struct SessionSummary(long Videos, long Skipped, long Errors, long Warnings);

    /* run_ytdlp.ps1 closes every session with
     *   -- Session summary: N video(s) touched, M already archived (skipped),
     *      E error(s), W warning(s) --
     * which is the only place those counts exist. Parsed here so a history row
     * can show them without re-reading download.log.
     *
     * Each whitespace token has its non-digit edges trimmed before parsing, so
     * "3," and "(skipped)" behave -- the latter yielding nothing. */
    public static SessionSummary? ParseSessionSummary(string line)
    {
        if (!line.Contains("Session summary:", StringComparison.Ordinal)) return null;

        var nums = new List<long>(4);
        foreach (var token in line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries))
        {
            if (nums.Count == 4) break;

            var start = 0;
            while (start < token.Length && !char.IsAsciiDigit(token[start])) start++;
            var end = token.Length;
            while (end > start && !char.IsAsciiDigit(token[end - 1])) end--;
            if (end <= start) continue;

            var s = token[start..end];
            if (!s.All(char.IsAsciiDigit)) continue;
            if (long.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out var n))
                nums.Add(n);
        }

        if (nums.Count != 4) return null;
        return new SessionSummary(nums[0], nums[1], nums[2], nums[3]);
    }
}
