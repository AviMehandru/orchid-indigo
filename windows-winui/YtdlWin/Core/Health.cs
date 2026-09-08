/* Health, config, and integrity.
 *
 * Everything here answers a question you would otherwise answer by running a
 * command and reading a file: is pwsh installed, is yt-dlp current, which
 * CONFIG_VERSION is installed, do this video's checksums still verify.
 *
 * DELIBERATELY READ-ONLY. Nothing here installs, updates or repairs anything.
 * `yt-dlp -U` is run by run_ytdlp.ps1's own once-per-24h dependency check, and
 * a second updater racing it from a GUI is exactly the kind of shared-state
 * collision the pipeline spent a release removing.
 */

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace YtdlWin.Core;

public sealed class Dependency
{
    public required string Name { get; init; }
    /// required | recommended | optional
    public required string Importance { get; init; }
    public required string Note { get; init; }
    public required bool Found { get; init; }
    public string? Path { get; init; }
    /// null when the probe timed out or the tool printed nothing -- which is a
    /// different state from missing, and worth saying rather than showing a
    /// blank.
    public string? Version { get; init; }
}

public static class Health
{
    /* How long a `--version` call gets before it is killed.
     *
     * Not defensiveness: `yt-dlp --version` on a machine whose network is being
     * filtered, and `pwsh --version` on a profile that lives on a network share,
     * can both sit for a long time, and a Health pane that hangs on one of them
     * is indistinguishable from a Health pane that is broken.
     *
     * There is a Windows-specific reason it needs to be this generous rather
     * than tighter: on-access antivirus scans an executable the first time it
     * runs, and the first `yt-dlp --version` after an update can spend several
     * seconds inside Defender before it prints anything. A four-second timeout
     * would report a perfectly healthy machine as "version probe timed out"
     * exactly once per update, which is the least useful moment to be wrong. */
    public static readonly TimeSpan ProbeTimeout = TimeSpan.FromSeconds(8);
    private static readonly TimeSpan CacheTtl = TimeSpan.FromMinutes(5);

    private sealed record DepSpec(string Name, string Importance, string[] Args, string Note);

    private static readonly DepSpec[] Specs =
    {
        new("pwsh", "required", new[] { "--version" },
            "Every stage of the pipeline is a PowerShell 7 script. Without it nothing " +
            "downloads. `winget install Microsoft.PowerShell`. Windows PowerShell 5.1 is not " +
            "a substitute — it is what setup.ps1 runs under, and its only job is to install " +
            "this."),
        new("yt-dlp", "required", new[] { "--version" },
            "Does all the actual extraction. run_ytdlp.ps1 runs `yt-dlp -U` on a 24h throttle."),
        new("ffmpeg", "required", new[] { "-version" },
            "Merging, embedding and thumbnails."),
        new("ffprobe", "recommended", new[] { "-version" },
            "Without it the Media tab cannot read stream details. Everything else still works."),
        new("deno", "recommended", new[] { "--version" },
            "YouTube's JS challenge needs a JS runtime. Its absence usually shows up as " +
            "mid-download HTTP 403s rather than an obvious error."),
        new("node", "optional", new[] { "--version" },
            "Runtime for the PO token provider server."),
        new("python", "optional", new[] { "--version" },
            "Runs archive-viewer.py and installs the PO token plugin."),
    };

    private static readonly object CacheLock = new();
    private static List<Dependency>? _cached;
    private static DateTimeOffset _cachedAt;

    /* The probe result, cached.
     *
     * Seven `--version` calls cost seconds -- pwsh and yt-dlp are the better
     * part of one each on their own -- and the Health pane is opened far more
     * often than a toolchain changes. `force` is the Re-probe button: it skips
     * the cache, which is what someone who has just installed a missing
     * dependency expects that button to do.
     *
     * BLOCKS for up to the probe timeout. Call it off the UI thread. */
    public static List<Dependency> Dependencies(bool force)
    {
        lock (CacheLock)
        {
            if (!force && _cached is not null &&
                DateTimeOffset.UtcNow - _cachedAt < CacheTtl)
            {
                return _cached;
            }
        }

        /* Probe every tool AT ONCE. These are seven independent process spawns
         * with nothing shared between them, so running them one after another
         * simply added up their latencies -- that loop was the whole cost of
         * the Health pane's first paint on the GTK app.
         *
         * Results are collected in SPEC order, not completion order, so the
         * table does not reshuffle itself depending on which tool answered
         * first. Task.WhenAll preserves the input ordering in its result array,
         * which is what makes that free here. */
        var tasks = Specs.Select(spec => Task.Run(() => Probe(spec))).ToArray();
        Task.WaitAll(tasks.Cast<Task>().ToArray());
        var results = tasks.Select(t => t.Result).ToList();

        lock (CacheLock)
        {
            _cached = results;
            _cachedAt = DateTimeOffset.UtcNow;
        }
        return results;
    }

    private static Dependency Probe(DepSpec spec)
    {
        var found = spec.Name switch
        {
            "pwsh" => Paths.FindPwsh(),
            // "python" first, then "python3": the python.org installer and the
            // Store build both provide python.exe, and python3.exe exists only
            // in some of them. The Unix ports look for python3 first for the
            // opposite reason.
            "python" => Paths.Which("python") ?? Paths.Which("python3"),
            _ => Paths.FindPipelineTool(spec.Name),
        };

        if (found is null)
        {
            return new Dependency
            {
                Name = spec.Name, Importance = spec.Importance, Note = spec.Note,
                Found = false, Path = null, Version = null,
            };
        }

        return new Dependency
        {
            Name = spec.Name, Importance = spec.Importance, Note = spec.Note,
            Found = true, Path = found, Version = FirstLine(found, spec.Args),
        };
    }

    /* Run `exe args`, kill it on overrun, and return the first non-empty line of
     * its output. ffmpeg and several others print their banner to stderr, so
     * both streams are considered.
     *
     * WINDOWS: the Store build of Python installs an "app execution alias" --
     * a zero-byte reparse point at %LOCALAPPDATA%\Microsoft\WindowsApps\python.exe
     * which, when Python is NOT installed, opens the Store instead of running
     * anything. Probing it is harmless (it prints nothing and exits), so it
     * comes back as "found, version probe timed out" rather than as a hang; it
     * is called out here because "python is found but has no version" on an
     * otherwise clean machine is that alias and not a broken install. */
    private static string? FirstLine(string exe, string[] args)
    {
        using var process = new Process();
        var psi = process.StartInfo;
        psi.FileName = exe;
        foreach (var a in args) psi.ArgumentList.Add(a);
        psi.UseShellExecute = false;
        psi.CreateNoWindow = true;
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError = true;
        psi.RedirectStandardInput = true;
        psi.Environment["PATH"] = Paths.ChildPath();
        // See Media.Probe: a redirected StreamReader decodes with the console
        // code page unless told otherwise, which is not UTF-8 on a default
        // Windows install.
        psi.StandardOutputEncoding = Encoding.UTF8;
        psi.StandardErrorEncoding = Encoding.UTF8;

        try { process.Start(); }
        catch (Exception) { return null; }

        /* Both pipes are read asynchronously rather than one after the other.
         * --version output is small, but a tool that decides to print a page of
         * build flags to stderr -- which `ffmpeg -version` very nearly does --
         * would fill that pipe and block forever against a reader still waiting
         * on stdout. */
        var stdout = new StringBuilder();
        var stderr = new StringBuilder();
        process.OutputDataReceived += (_, e) => { if (e.Data is not null) stdout.AppendLine(e.Data); };
        process.ErrorDataReceived += (_, e) => { if (e.Data is not null) stderr.AppendLine(e.Data); };

        try
        {
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            process.StandardInput.Close();
        }
        catch (Exception) { /* the process died before the reads were armed */ }

        var timedOut = false;
        try
        {
            if (!process.WaitForExit((int)ProbeTimeout.TotalMilliseconds))
            {
                timedOut = true;
                // entireProcessTree: a probe that spawned something is not
                // expected, but leaving one behind on every timeout would be.
                try { process.Kill(entireProcessTree: true); } catch (Exception) { }
                process.WaitForExit(1000);
            }
            else
            {
                /* The parameterless overload after the timed one is not
                 * redundant. Only this form waits for the async output handlers
                 * to be flushed, and without it the version string is
                 * intermittently empty -- the classic .NET redirected-output
                 * bug, and one that would present as "this works on my machine
                 * and shows blanks on yours". */
                process.WaitForExit();
            }
        }
        catch (Exception) { return null; }

        if (timedOut) return null;

        var text = stdout.Length > 0 ? stdout.ToString() : stderr.ToString();
        var line = text.Split('\n').FirstOrDefault()?.Trim() ?? "";
        return line.Length == 0 ? null : line;
    }

    // MARK: - Installed pipeline files

    public sealed class InstalledFile
    {
        public required string Name { get; init; }
        public required string Path { get; init; }
        public required bool Present { get; init; }
        public required long Size { get; init; }
        /// null when absent.
        public DateTimeOffset? Modified { get; init; }
    }

    /* What is INSTALLED, never what is in a checkout.
     *
     * The repo holds the sources; the installer copies them to their runtime
     * locations. Editing a file in a clone has no effect on a live install until
     * it is copied over, which is exactly the confusion this list settles. */
    public static List<InstalledFile> InstalledFiles()
    {
        var scripts = Paths.ScriptsDir();
        var configs = Paths.ConfigsDir();

        var names = new[]
        {
            "run_ytdlp.ps1", "postprocess.ps1", "ytdl.ps1",
            "pot-provider.ps1", "archive-viewer.py",
        };

        var files = names.Select(n => Stat(Paths.Join(scripts, n), n)).ToList();
        files.Add(Stat(Paths.Join(configs, "yt-dlp.conf"), "yt-dlp.conf"));
        return files;
    }

    private static InstalledFile Stat(string path, string name)
    {
        try
        {
            var info = new FileInfo(Paths.Extended(path));
            if (!info.Exists)
                return new InstalledFile { Name = name, Path = path, Present = false, Size = 0 };

            return new InstalledFile
            {
                Name = name, Path = path, Present = true,
                Size = info.Length,
                Modified = new DateTimeOffset(info.LastWriteTimeUtc, TimeSpan.Zero),
            };
        }
        catch (Exception)
        {
            return new InstalledFile { Name = name, Path = path, Present = false, Size = 0 };
        }
    }

    // MARK: - yt-dlp.conf

    public sealed class ConfigInfo
    {
        public required string Path { get; init; }
        public required bool Present { get; init; }
        /// From "# CONFIG_VERSION:".
        public string? ConfigVersion { get; init; }
        public required string Body { get; init; }
        public required int OptionCount { get; init; }
    }

    /// CONFIG_VERSION is recorded in manifest.json and download.log by both
    /// scripts, so the number shown here is the one those files will carry.
    public static ConfigInfo ConfigDetails()
    {
        var path = Paths.Join(Paths.ConfigsDir(), "yt-dlp.conf");
        var present = Paths.IsRegularFile(path);
        var body = Paths.ReadAllTextOrNull(path) ?? "";

        string? version = null;
        var options = 0;
        foreach (var raw in body.Replace("\r\n", "\n").Split('\n'))
        {
            var t = raw.Trim();
            if (t.StartsWith("# CONFIG_VERSION:", StringComparison.Ordinal))
                version = t["# CONFIG_VERSION:".Length..].Trim();
            // Count only real options: a line whose first non-space is "--".
            if (t.StartsWith("--", StringComparison.Ordinal)) options++;
        }

        return new ConfigInfo
        {
            Path = path, Present = present, ConfigVersion = version,
            Body = body, OptionCount = options,
        };
    }

    // MARK: - Archive statistics

    public sealed class ArchiveStats
    {
        public required string DataRoot { get; init; }
        public required int Videos { get; init; }
        public required int Channels { get; init; }
        public required long TotalBytes { get; init; }
        /// -1 when unreadable.
        public required int GlobalManifestEntries { get; init; }
        /// -1 when unreadable.
        public required int ArchiveTxtIds { get; init; }
        public required string LogDir { get; init; }
        public required int HistorySnapshots { get; init; }
    }

    public static ArchiveStats Stats(string dataRoot, int videos, int channels, long totalBytes)
    {
        var logDir = Paths.Join(dataRoot, @"Archive Logs\Logs");

        var globalEntries = -1;
        var globalPath = Paths.Join(dataRoot, @"Youtube Videos\global_manifest.json");
        try
        {
            if (Paths.IsRegularFile(globalPath))
            {
                using var doc = JsonDocument.Parse(File.ReadAllBytes(Paths.Extended(globalPath)));
                globalEntries = doc.RootElement.ValueKind switch
                {
                    JsonValueKind.Array => doc.RootElement.GetArrayLength(),
                    /* A single-video archive serialises as one object, not a
                     * one-element array -- ConvertTo-Json unrolls it. Counting
                     * that as zero would be wrong in exactly the case a new
                     * user sees. */
                    JsonValueKind.Object => 1,
                    _ => -1,
                };
            }
        }
        catch (Exception) { globalEntries = -1; }

        var archiveIds = -1;
        var archiveText = Paths.ReadAllTextOrNull(Paths.Join(logDir, "archive.txt"));
        if (archiveText is not null)
        {
            archiveIds = archiveText.Replace("\r\n", "\n").Split('\n')
                                    .Count(l => l.Trim().Length > 0);
        }

        var historyDir = Paths.Join(dataRoot, @"Archive Logs\Archive History");
        var snapshots = Paths.ListNames(historyDir).Length;

        return new ArchiveStats
        {
            DataRoot = dataRoot, Videos = videos, Channels = channels, TotalBytes = totalBytes,
            GlobalManifestEntries = globalEntries, ArchiveTxtIds = archiveIds,
            LogDir = logDir, HistorySnapshots = snapshots,
        };
    }

    // MARK: - Checksums

    public sealed class ChecksumResult
    {
        public bool Present { get; set; }
        public int Checked { get; set; }
        public int Ok { get; set; }
        public List<string> Failed { get; } = new();
        public List<string> Missing { get; } = new();
    }

    /* Verify a video folder against its own checksums.sha256.
     *
     * Standard sha256sum format ("<hash>  <relative/path>"), written by
     * postprocess.ps1 over every file in the folder EXCEPT
     * Logs/video_postprocessing.log -- excluded because it is still being
     * appended to when the hashes are computed, and a manifest that always
     * reports one failure teaches you to ignore its failures. Its absence is
     * therefore not a failure here either.
     *
     * BLOCKS: hashing every file in a folder is seconds of work on a large
     * video. Call it off the UI thread. */
    public static ChecksumResult VerifyChecksums(string videoDir, CancellationToken cancel = default)
    {
        var r = new ChecksumResult();

        var file = Paths.Join(videoDir, @"Video metadata\checksums.sha256");
        var body = Paths.ReadAllTextOrNull(file);
        if (body is null) return r;   // Present stays false
        r.Present = true;

        foreach (var rawLine in body.Replace("\r\n", "\n").Split('\n'))
        {
            cancel.ThrowIfCancellationRequested();

            var line = rawLine.Trim();
            if (line.Length == 0) continue;

            /* sha256sum format: hash, two spaces, then the path. Splitting on
             * the double space rather than on whitespace matters -- a filename
             * with a space in it is normal here, and this pipeline's filenames
             * are made of video titles. */
            var sep = line.IndexOf("  ", StringComparison.Ordinal);
            if (sep < 0) continue;

            var hash = line[..sep];
            var rel = line[(sep + 2)..];
            r.Checked++;

            /* The path in the manifest is written by PowerShell on Windows, so
             * it may use either separator. Native() normalises it before it is
             * joined, and the join is containment-checked -- a checksums file
             * with "..\..\Windows\System32\x" in it must not send this hashing
             * something outside the folder. */
            var path = Paths.Join(videoDir, Paths.Native(Paths.Slashed(rel)));
            if (!Paths.IsInside(videoDir, path) || !Paths.IsRegularFile(path))
            {
                r.Missing.Add(rel);
                continue;
            }

            var actual = Sha256OfFile(path);
            if (actual is not null && string.Equals(actual, hash, StringComparison.OrdinalIgnoreCase))
                r.Ok++;
            else
                r.Failed.Add(rel);
        }
        return r;
    }

    /// Streamed in 1MB chunks: a video file does not fit in memory twice.
    public static string? Sha256OfFile(string path)
    {
        try
        {
            /* FileShare.ReadWrite, not the default FileShare.Read. A running
             * download has files in this tree open for writing, and the default
             * share mode would fail to open one -- reporting a verify failure
             * that is really just "something else has this file open". */
            using var stream = new FileStream(Paths.Extended(path), FileMode.Open,
                                              FileAccess.Read, FileShare.ReadWrite,
                                              bufferSize: 1024 * 1024);
            using var sha = SHA256.Create();
            var digest = sha.ComputeHash(stream);
            return Convert.ToHexString(digest).ToLowerInvariant();
        }
        catch (Exception) { return null; }
    }

    // MARK: - Log tails

    /* The last `lines` lines of a file, WITHOUT reading the file.
     *
     * download.log is appended to by every run and never rotated by the
     * pipeline, so on a machine that has been archiving for a while it is the
     * largest thing this pane touches. Reading it whole to keep the last 300
     * lines made the pane's cost grow with the age of the install, for a panel
     * whose content is fixed-size. This seeks to the last megabyte and works
     * from there. */
    public static string LogTail(string path, int lines)
    {
        const long maxBytes = 1024 * 1024;

        try
        {
            /* FileShare.ReadWrite again, and here it is not an optimisation but
             * the whole feature: download.log is open for append by the run
             * this pane is being used to watch. Opening it with the default
             * share mode fails every time it would be interesting. */
            using var stream = new FileStream(Paths.Extended(path), FileMode.Open,
                                              FileAccess.Read, FileShare.ReadWrite);
            var size = stream.Length;
            var start = size > maxBytes ? size - maxBytes : 0;
            if (start > 0) stream.Seek(start, SeekOrigin.Begin);

            var buffer = new byte[size - start];
            var read = stream.Read(buffer, 0, buffer.Length);
            var text = Paths.DecodeText(read == buffer.Length ? buffer : buffer[..read]);

            /* The first line of the window is dropped when the window did not
             * start at the beginning of the file, because it is almost
             * certainly half a line -- and on this platform, possibly half a
             * UTF-8 sequence too, which the Latin-1 fallback would render as
             * mojibake rather than dropping. */
            if (start > 0)
            {
                var nl = text.IndexOf('\n');
                if (nl >= 0) text = text[(nl + 1)..];
            }

            var all = text.Replace("\r\n", "\n").Split('\n').ToList();
            /* A trailing newline leaves an empty last element; dropping it keeps
             * "last 20 lines" from silently meaning 19 plus a blank. */
            if (all.Count > 0 && all[^1].Length == 0) all.RemoveAt(all.Count - 1);

            return string.Join("\n", all.TakeLast(lines));
        }
        catch (Exception) { return ""; }
    }
}
