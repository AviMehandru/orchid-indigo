/* Where everything lives, and the two path problems that are Windows' alone.
 *
 * Every rule here is a copy of one that already exists in the pipeline, and the
 * copy is deliberate: this process is started by Explorer, not by ytdl, so it
 * inherits nothing. The rules it mirrors:
 *
 *   install root   run_ytdlp.ps1 / ytdl.ps1 platform block  -> C:/yt-dlp
 *   data root      defaults to the install root, per -DataRoot handling
 *   archive root   <dataRoot>/Youtube Videos/Complete Archive
 *
 * C:\yt-dlp rather than something under the user profile is NOT this app's
 * choice and must not be "improved". run_ytdlp.ps1 explains it: MAX_PATH. The
 * per-video paths this pipeline builds run to about 240 characters before the
 * data root is prefixed, so "C:/yt-dlp" at 9 characters versus
 * "C:/Users/<name>/yt-dlp" at twice that is the difference between a real video
 * fitting and not. Pointing this app at a different root than the pipeline uses
 * is the bug where the library indexes one tree and downloads go to another.
 *
 * ============================ THE TWO WINDOWS TRAPS ==========================
 *
 * 1. SEPARATORS. Every relative path inside an archive entry is kept
 *    '/'-SEPARATED, always, even here. Two reasons, both load-bearing:
 *    the opaque key is SHA-256 over the relative path and must come out byte
 *    for byte identical to the key the GTK and SwiftUI apps compute for the
 *    same folder; and manifest.json's `media_file` is compared against it.
 *    Native separators appear only at the moment a path is handed to the
 *    filesystem, which is Native() and nowhere else. A Path.Combine that leaks
 *    a backslash into a `rel` is not a cosmetic slip -- it silently changes
 *    every key this app computes.
 *
 * 2. MAX_PATH. Windows still caps most paths at 260 characters unless long-path
 *    support is switched on, and this pipeline's paths are long by design. The
 *    app.manifest declares longPathAware, but that only helps when the machine's
 *    LongPathsEnabled policy is also set, which on a default install it is not.
 *    So Extended() prefixes \\?\ for anything near the limit and the scan uses
 *    it. This is the one place a Windows reader has to do work the GTK and
 *    SwiftUI readers do not, and getting it wrong does not throw -- it drops
 *    exactly the deeply nested files (Pre-merge streams/, Video metadata/) that
 *    matter, and the library comes up short with no error anywhere.
 */

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

namespace YtdlWin.Core;

public static class Paths
{
    /// <summary>The single leaf name this app's directories use. Deliberately
    /// not shared with <c>ytdl-gtk</c>, <c>ytdl-macos</c>, <c>ytdlp-gui</c> or
    /// <c>ytdlp-archive-viewer</c>: the index formats differ, and a shared
    /// directory would mean each treating the others' files as corrupt.</summary>
    public const string AppDirectoryName = "ytdl-win";

    // MARK: - The environment

    /* The seam the conformance suite uses to point the home directory at a
     * fixture tree.
     *
     * The GTK app does this with g_setenv. Environment.SetEnvironmentVariable
     * would work here too, but it is process-wide mutable state that xUnit --
     * which runs test classes in parallel by default -- would let two tests
     * fight over. An explicit override is honest about what it is, and it is
     * the only mutable state in this file. Null in every build that is not
     * running tests; values set here WIN, and anything not named falls through
     * to the real environment so a test that redirects HOME still gets a
     * working PATH. */
    public static IReadOnlyDictionary<string, string>? EnvironmentOverride { get; set; }

    public static string? Env(string name)
    {
        if (EnvironmentOverride is not null && EnvironmentOverride.TryGetValue(name, out var v))
            return v;
        var real = Environment.GetEnvironmentVariable(name);
        return string.IsNullOrEmpty(real) ? null : real;
    }

    // MARK: - Roots

    /* The user profile. USERPROFILE first and directly, rather than going
     * straight to Environment.GetFolderPath: the pipeline scripts resolve
     * $HOME (which pwsh maps to the profile on Windows), and the conformance
     * suite points it at a fixture tree. SpecialFolder answers from the OS and
     * cannot be redirected, so a test written against it would be writing into
     * the real user's AppData.
     *
     * HOME is checked as well because pwsh defines it, so a machine configured
     * for the pipeline may well have it set, and disagreeing with the pipeline
     * about where home is would be the same class of bug as disagreeing about
     * the install root. */
    public static string HomeDirectory()
    {
        foreach (var name in new[] { "USERPROFILE", "HOME" })
        {
            var value = Env(name);
            if (!string.IsNullOrWhiteSpace(value)) return value;
        }

        var drive = Env("HOMEDRIVE");
        var path = Env("HOMEPATH");
        if (!string.IsNullOrWhiteSpace(drive) && !string.IsNullOrWhiteSpace(path))
            return drive + path;

        return Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
    }

    /// <summary>$YTDLP_INSTALL_ROOT, else C:\yt-dlp. Must agree with the platform
    /// block at the top of run_ytdlp.ps1 and ytdl.ps1 -- see the header.</summary>
    public static string InstallRoot()
    {
        var configured = Env("YTDLP_INSTALL_ROOT");
        return string.IsNullOrWhiteSpace(configured) ? @"C:\yt-dlp" : configured;
    }

    public static string ScriptsDir() => Join(InstallRoot(), "scripts");

    /* Note the plural. The INSTALLED config directory is configs/ while the
     * repo directory is config/ -- not a typo. The installed name predates the
     * repo restructure and is baked into run_ytdlp.ps1 and postprocess.ps1. */
    public static string ConfigsDir() => Join(InstallRoot(), "configs");

    /* Both under LOCALAPPDATA, and not one under Roaming.
     *
     * The macOS app splits these across Application Support (durable) and
     * Caches (disposable), because macOS empties Caches by itself when the disk
     * fills. Windows empties neither, so the split here carries less weight --
     * but it is kept anyway, because "delete the cache folder" stays a safe
     * instruction and "delete the state folder" stays a destructive one, and
     * that distinction is worth more than the directory it costs.
     *
     * LOCAL rather than ROAMING for the durable half, which is the interesting
     * choice. Roaming is where user settings that should follow a person
     * between machines belong, and profiles would qualify -- but queue.json and
     * history.json are full of absolute paths, and roaming a queue to a machine
     * where C:\Users\...\Videos does not exist produces a queue that fails one
     * item at a time for reasons nothing explains. One directory that is
     * honestly machine-local beats a split that would roam half a coherent
     * state. */
    private static string LocalAppData()
    {
        var configured = Env("LOCALAPPDATA");
        if (!string.IsNullOrWhiteSpace(configured)) return configured;
        return Join(Join(HomeDirectory(), "AppData"), "Local");
    }

    /// Derived and disposable: the thumbnail cache and anything one rescan
    /// rebuilds. Deleting it costs a re-index and nothing else.
    public static string CacheDir() => Join(Join(LocalAppData(), AppDirectoryName), "cache");

    /// NOT disposable: settings, profiles, the queue and the run history.
    /// Losing this loses real user work.
    public static string StateDir() => Join(Join(LocalAppData(), AppDirectoryName), "state");

    // MARK: - Path helpers

    /* "~" and "~/..." only. A bare "~user" is deliberately not expanded: the
     * pipeline does not expand it either, and silently resolving it here would
     * reintroduce exactly the two-different-folders bug where the library
     * indexed one path while downloads went to another.
     *
     * Both separators are accepted after the tilde because a path typed by
     * somebody who also uses the CLI will have a forward slash in it. */
    public static string ExpandTilde(string path)
    {
        if (string.IsNullOrEmpty(path)) return path;
        if (path == "~") return HomeDirectory();
        if (path.StartsWith("~/", StringComparison.Ordinal) ||
            path.StartsWith(@"~\", StringComparison.Ordinal))
        {
            return Join(HomeDirectory(), path.Substring(2));
        }
        return path;
    }

    /// Join two path fragments with the NATIVE separator. For a folder-relative
    /// path that will be stored on an <see cref="ArchiveFile"/>, use
    /// <see cref="JoinRel"/> instead -- those stay '/'-separated.
    public static string Join(string basePath, string leaf)
    {
        if (string.IsNullOrEmpty(basePath)) return leaf;
        if (string.IsNullOrEmpty(leaf)) return basePath;
        if (Path.IsPathRooted(leaf)) return leaf;
        return Path.Combine(basePath, leaf.Replace('/', Path.DirectorySeparatorChar));
    }

    /// Join two fragments of a FOLDER-RELATIVE path, which are always
    /// '/'-separated regardless of platform. See trap 1 in the header.
    public static string JoinRel(string prefix, string leaf)
        => string.IsNullOrEmpty(prefix) ? leaf : prefix + "/" + leaf;

    /// A '/'-separated relative path as the filesystem wants it.
    public static string Native(string rel) => rel.Replace('/', Path.DirectorySeparatorChar);

    /// A native path as a relative path is stored. Applied to anything read out
    /// of a manifest, since a manifest written on Windows uses backslashes in
    /// places the layout contract does not cover.
    public static string Slashed(string path) => path.Replace('\\', '/');

    /* Lexical canonicalisation: "." and ".." are resolved and the separators
     * are normalised, but symlinks and junctions are NOT followed. That is what
     * is wanted -- a junctioned archive root should stay spelled the way the
     * user gave it -- and it is also the only version that is cheap enough to
     * call once per file during a scan.
     *
     * GetFullPath is relative to the process's working directory for a relative
     * input, which is a footgun this app never fires because every path reaching
     * here is already absolute. The trailing separator is trimmed so that
     * "C:\a\" and "C:\a" compare equal, which the containment check depends on.
     * A bare drive root ("C:\") keeps its separator, because "C:" alone means
     * "the current directory on C:" and is a different thing. */
    public static string Canonical(string path)
    {
        if (string.IsNullOrEmpty(path)) return path;
        string full;
        try { full = Path.GetFullPath(path); }
        catch (Exception) { return path; }

        if (full.Length > 3 && (full.EndsWith('\\') || full.EndsWith('/')))
            full = full.TrimEnd('\\', '/');
        return full;
    }

    /* Whether `child` is inside `parent`, or is `parent`.
     *
     * ORDINAL-IGNORE-CASE, and that is not sloppiness. NTFS is case-insensitive
     * by default, so "C:\Archive\x" and "c:\archive\x" are one file; an
     * ordinal-sensitive comparison would reject a legitimate path whenever the
     * two halves were spelled with different capitalisation, which happens
     * routinely because one came from a folder picker and the other from a
     * typed setting.
     *
     * The separator on the end of the prefix is what stops "C:\Archive" from
     * containing "C:\Archive2". */
    public static bool IsInside(string parent, string child)
    {
        var p = Canonical(parent);
        var c = Canonical(child);
        if (string.Equals(p, c, StringComparison.OrdinalIgnoreCase)) return true;

        var prefix = p.EndsWith(Path.DirectorySeparatorChar) ? p : p + Path.DirectorySeparatorChar;
        return c.StartsWith(prefix, StringComparison.OrdinalIgnoreCase);
    }

    // MARK: - Long paths

    /* The \\?\ escape hatch. See trap 2 in the header.
     *
     * \\?\ tells Win32 to skip path parsing entirely, which is what lifts the
     * 260-character limit -- and it means the path handed over must ALREADY be
     * fully qualified, backslash-separated and free of "." and ".." segments,
     * because nothing downstream will fix it. Hence Canonical() first, always.
     *
     * Applied only past a threshold rather than to everything. A prefixed path
     * is not accepted everywhere (some shell APIs and some third-party tools
     * reject it), it turns up in error messages the user reads, and it changes
     * how a trailing space or dot in a filename behaves. Below the limit it
     * buys nothing, so it is not paid for.
     *
     * 240 rather than 260: a directory handed to an enumeration needs room for
     * the longest child name underneath it, and 260 is the limit on the
     * complete path, not on the directory. */
    private const int LongPathThreshold = 240;

    public static string Extended(string path)
    {
        if (string.IsNullOrEmpty(path)) return path;
        if (path.StartsWith(@"\\?\", StringComparison.Ordinal)) return path;
        if (path.Length < LongPathThreshold) return path;

        var full = Canonical(path);
        if (full.StartsWith(@"\\", StringComparison.Ordinal))
        {
            // A UNC path spells it \\?\UNC\server\share, dropping one leading
            // backslash. \\?\\\server\share is not the same thing and does not
            // resolve.
            return @"\\?\UNC\" + full.Substring(2);
        }
        // Only a fully-qualified drive path can be prefixed; a relative one
        // would silently become garbage, so it is left alone to fail honestly.
        return full.Length > 1 && full[1] == ':' ? @"\\?\" + full : full;
    }

    // MARK: - Filesystem predicates

    /* Every one of these swallows its exceptions and answers false.
     *
     * A scan walks a user-chosen tree, and a tree that is being written to by a
     * running download, or that holds a file the user has open, or that lives
     * on a disconnected network drive, produces IOException and
     * UnauthorizedAccessException as ORDINARY events. The layout contract says
     * a consumer tolerates a folder it cannot read; an exception escaping to
     * the top of a scan would empty the whole library because of one locked
     * file. */
    public static bool IsDirectory(string path)
    {
        if (string.IsNullOrEmpty(path)) return false;
        try { return Directory.Exists(Extended(path)); }
        catch (Exception) { return false; }
    }

    public static bool IsRegularFile(string path)
    {
        if (string.IsNullOrEmpty(path)) return false;
        try
        {
            var full = Extended(path);
            return File.Exists(full) && !Directory.Exists(full);
        }
        catch (Exception) { return false; }
    }

    public static long FileSize(string path)
    {
        try { return new FileInfo(Extended(path)).Length; }
        catch (Exception) { return 0; }
    }

    /// Directory entries, sorted, or empty when the directory cannot be read.
    public static string[] ListNames(string dir)
    {
        try
        {
            var names = Directory.EnumerateFileSystemEntries(Extended(dir))
                .Select(Path.GetFileName)
                .Where(n => !string.IsNullOrEmpty(n))
                .Select(n => n!)
                .ToArray();
            Array.Sort(names, StringComparer.Ordinal);
            return names;
        }
        catch (Exception) { return Array.Empty<string>(); }
    }

    /// Read a whole text file, or null. UTF-8 with a Latin-1 fallback, because
    /// a .vtt or a description written years ago by a different tool is not
    /// always valid UTF-8 and losing the whole file over one byte is worse than
    /// showing one wrong character.
    public static string? ReadAllTextOrNull(string path)
    {
        try
        {
            var bytes = File.ReadAllBytes(Extended(path));
            return DecodeText(bytes);
        }
        catch (Exception) { return null; }
    }

    public static string DecodeText(byte[] bytes)
    {
        try
        {
            return new UTF8Encoding(false, throwOnInvalidBytes: true).GetString(bytes);
        }
        catch (DecoderFallbackException)
        {
            return Encoding.Latin1.GetString(bytes);
        }
    }

    // MARK: - Finding the archive

    /* A directory is "a channel folder" if any of its first 60 children is
     * either named "Channel Info" or holds a Final files/ or Video metadata/
     * subfolder.
     *
     * The cap is not laziness: a user can point this at C:\, and the answer is
     * decided by the first handful of entries in every real case. Reading an
     * unbounded directory to say "no" is how a folder picker hangs. */
    private static bool LooksLikeChannelDir(string path)
    {
        var seen = 0;
        foreach (var name in ListNames(path))
        {
            if (seen >= 60) break;
            var child = Join(path, name);
            if (!IsDirectory(child)) continue;
            seen++;

            if (name == "Channel Info") return true;
            if (IsDirectory(Join(child, "Final files"))) return true;
            if (IsDirectory(Join(child, "Video metadata"))) return true;
        }
        return false;
    }

    /* Accept anything reasonable the user might point at -- a data root, the
     * "Youtube Videos" folder, "Complete Archive" itself, a channel folder, or
     * a reorganised tree -- and find the real Complete Archive directory. Same
     * acceptance set as archive-viewer.py's --root, so a path that works for
     * one works for both. null if nothing plausible is there. */
    public static string? ResolveArchiveRoot(string? candidate)
    {
        if (string.IsNullOrWhiteSpace(candidate)) return null;

        var expanded = ExpandTilde(candidate.Trim());
        if (!IsDirectory(expanded) && !IsRegularFile(expanded)) return null;
        var p = Canonical(expanded);

        var nested = Join(Join(p, "Youtube Videos"), "Complete Archive");
        var direct = Join(p, "Complete Archive");
        foreach (var attempt in new[] { nested, direct, p })
        {
            if (IsDirectory(attempt) &&
                string.Equals(Path.GetFileName(attempt), "Complete Archive",
                              StringComparison.OrdinalIgnoreCase))
            {
                return attempt;
            }
        }

        // Pointed at a channel folder, or somewhere below the root: walk up.
        var cur = p;
        while (!string.IsNullOrEmpty(cur))
        {
            if (IsDirectory(cur) &&
                string.Equals(Path.GetFileName(cur), "Complete Archive",
                              StringComparison.OrdinalIgnoreCase))
            {
                return cur;
            }
            var parent = Path.GetDirectoryName(cur);
            if (string.IsNullOrEmpty(parent) || parent == cur) break;
            cur = parent;
        }

        // Last resort: a directory whose children look like channel folders is
        // good enough to index, whatever it happens to be called.
        if (IsDirectory(p))
        {
            var seen = 0;
            foreach (var name in ListNames(p))
            {
                if (seen >= 60) break;
                var child = Join(p, name);
                if (!IsDirectory(child)) continue;
                seen++;
                if (LooksLikeChannelDir(child)) return p;
            }
            if (LooksLikeChannelDir(p)) return p;
        }

        return null;
    }

    /* The usual install locations, in order. null if none of them hold one.
     *
     * C:\yt-dlp leads because that is where the pipeline puts it. The user
     * profile's Videos folder is on this list where macOS has Movies, for the
     * same reason: it is where a Windows user is most likely to have put a
     * video archive, and it costs one stat to check. */
    public static string? AutodetectArchiveRoot()
    {
        var home = HomeDirectory();
        var candidates = new[]
        {
            InstallRoot(),
            @"C:\yt-dlp",
            Join(home, "yt-dlp"),
            Join(home, @"Videos\yt-dlp"),
            Join(home, @"Documents\yt-dlp"),
            home,
        };
        foreach (var candidate in candidates)
        {
            var found = ResolveArchiveRoot(candidate);
            if (found is not null) return found;
        }
        return null;
    }

    // MARK: - The opaque key

    /* The opaque key a video folder is addressed by.
     *
     * The UI never holds a filesystem path: it holds one of these plus an index
     * into the entry's own file list, and the path is resolved from the index.
     * Traversal is off the table because no route accepts a path, not because a
     * filter has to be right.
     *
     * SHA-256 of the '/'-separated relative path, truncated to 8 bytes -- 16
     * hex characters, byte for byte the same key the GTK and SwiftUI apps
     * compute for the same folder. Backslashes are normalised first, which on
     * THIS platform is the difference between agreeing with the other two apps
     * and not: everything native here is backslash-separated, and a key
     * computed over "Rick\video" would match nothing any other reader
     * produces. */
    public static string Key(string rel)
    {
        var normalised = Slashed(rel);
        var digest = SHA256.HashData(Encoding.UTF8.GetBytes(normalised));
        return Convert.ToHexString(digest, 0, 8).ToLowerInvariant();
    }

    // MARK: - Executables

    /* Locate an executable on PATH. Deliberately not a `where.exe` subprocess,
     * which would be one more thing that can be missing.
     *
     * PATHEXT is consulted rather than ".exe" being assumed. yt-dlp installs as
     * yt-dlp.exe, but a scoop or npm shim is a .cmd or a .bat, and those are
     * executables here in a way they are not on Unix -- a probe that only
     * looked for .exe would report a working install as missing. A name that
     * already carries an extension is tried as given first, so an explicit
     * "pwsh.exe" is not turned into "pwsh.exe.exe". */
    private static string[] PathExtensions()
    {
        var raw = Env("PATHEXT") ?? ".COM;.EXE;.BAT;.CMD";
        var parts = raw.Split(';', StringSplitOptions.RemoveEmptyEntries |
                                   StringSplitOptions.TrimEntries);
        return parts.Length == 0 ? new[] { ".EXE" } : parts;
    }

    public static string? Which(string name)
    {
        if (string.IsNullOrWhiteSpace(name)) return null;

        // An explicit path is used as given, so a configured absolute pwsh is
        // not second-guessed against PATH.
        if (name.Contains('\\') || name.Contains('/'))
            return IsRegularFile(name) ? name : null;

        var pathVar = Env("PATH");
        if (string.IsNullOrEmpty(pathVar)) return null;

        var exts = PathExtensions();
        var hasExt = Path.HasExtension(name) &&
                     exts.Any(e => name.EndsWith(e, StringComparison.OrdinalIgnoreCase));

        foreach (var dir in pathVar.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var trimmed = dir.Trim().Trim('"');
            if (trimmed.Length == 0) continue;

            if (hasExt)
            {
                var direct = Join(trimmed, name);
                if (IsRegularFile(direct)) return direct;
                continue;
            }
            foreach (var ext in exts)
            {
                var candidate = Join(trimmed, name + ext);
                if (IsRegularFile(candidate)) return candidate;
            }
        }
        return null;
    }

    /* pwsh, or null. Every stage of this pipeline is a PowerShell 7 script, so
     * a missing pwsh is not a degraded mode -- no download can run at all, and
     * the UI says exactly that rather than failing at spawn time with a
     * confusing OS error.
     *
     * The extra locations matter LESS here than they do on macOS, and it is
     * worth saying why rather than copying the macOS list without thinking. A
     * .app bundle inherits launchd's PATH and cannot see Homebrew; a Windows
     * GUI process inherits the PATH Explorer was started with, which IS the
     * user's, so the ordinary case just works. What it does not see is a PATH
     * entry added since the user last signed in -- and "I just installed
     * PowerShell and this still says it is missing" is exactly the moment
     * somebody reaches for this pane. So the standard install locations of the
     * three ways pwsh actually arrives on a Windows machine are checked
     * directly.
     *
     * WINDOWS POWERSHELL 5.1 IS NOT A FALLBACK and must never be added to this
     * list. powershell.exe is on every Windows machine, which makes it exactly
     * the tempting wrong answer: the pipeline's scripts use pwsh 7 syntax and
     * $IsWindows, and 5.1 would fail partway through a run with a parse error
     * rather than not starting. setup.ps1 is the one file in the whole project
     * written for 5.1, and its job is to install this. */
    public static string? FindPwsh()
    {
        var onPath = Which("pwsh");
        if (onPath is not null) return onPath;

        var programFiles = Env("ProgramFiles") ?? @"C:\Program Files";
        var programFilesX86 = Env("ProgramFiles(x86)") ?? @"C:\Program Files (x86)";
        var localAppData = LocalAppData();

        var extra = new List<string>();
        // The MSI installer, which is what winget and the .msi both use. The
        // major version is a directory name, so 7 and 8 are both looked for
        // rather than only the one that existed when this was written.
        foreach (var major in new[] { "7", "8" })
        {
            extra.Add(Join(programFiles, $@"PowerShell\{major}\pwsh.exe"));
            extra.Add(Join(programFilesX86, $@"PowerShell\{major}\pwsh.exe"));
        }
        // The Microsoft Store build, which installs an app-execution alias.
        extra.Add(Join(localAppData, @"Microsoft\WindowsApps\pwsh.exe"));
        // Chocolatey and scoop, the two package managers that put a shim on a
        // path the installer may not have refreshed yet.
        extra.Add(@"C:\ProgramData\chocolatey\bin\pwsh.exe");
        extra.Add(Join(HomeDirectory(), @"scoop\shims\pwsh.exe"));

        foreach (var candidate in extra)
        {
            if (IsRegularFile(candidate)) return candidate;
        }
        return null;
    }

    /* Locate one of the pipeline's own tools, checking the install root before
     * giving up. setup.ps1 places yt-dlp.exe and deno.exe in the install root's
     * bin directory, and a run started before the user has signed out and back
     * in has a PATH that does not include it yet. */
    public static string? FindPipelineTool(string name)
    {
        var onPath = Which(name);
        if (onPath is not null) return onPath;

        var install = InstallRoot();
        foreach (var dir in new[] { Join(install, "bin"), install })
        {
            foreach (var ext in new[] { ".exe", ".cmd", ".bat", "" })
            {
                var candidate = Join(dir, name + ext);
                if (IsRegularFile(candidate)) return candidate;
            }
        }

        // deno's own installer puts it under %USERPROFILE%\.deno\bin, which
        // run_ytdlp.ps1's $denoCandidates also checks.
        if (name.Equals("deno", StringComparison.OrdinalIgnoreCase))
        {
            foreach (var rel in new[] { @".deno\bin\deno.exe", @".local\bin\deno.exe" })
            {
                var candidate = Join(HomeDirectory(), rel);
                if (IsRegularFile(candidate)) return candidate;
            }
        }
        return null;
    }

    /* The PATH every child this app spawns is given.
     *
     * On macOS this function does real work, because a bundled app cannot see
     * Homebrew. Here it mostly passes the inherited PATH through -- with the
     * install root's bin directory appended, for the sign-out case above.
     * ytdl.ps1 runs yt-dlp and ffmpeg BY NAME, so a pipeline started from this
     * window would otherwise fail to find a tool the same command finds in a
     * terminal, and report it as an extractor error several layers from the
     * cause. */
    public static string ChildPath()
    {
        var inherited = Env("PATH") ?? "";
        var parts = inherited.Split(';', StringSplitOptions.RemoveEmptyEntries)
                             .Select(p => p.Trim())
                             .Where(p => p.Length > 0)
                             .ToList();

        foreach (var extra in new[] { Join(InstallRoot(), "bin"), InstallRoot() })
        {
            if (IsDirectory(extra) &&
                !parts.Any(p => string.Equals(p, extra, StringComparison.OrdinalIgnoreCase)))
            {
                parts.Add(extra);
            }
        }
        return string.Join(';', parts);
    }
}
