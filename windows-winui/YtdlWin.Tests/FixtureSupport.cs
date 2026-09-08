/* Building the fabrications every other test file works against.
 *
 * NOTHING IN THIS SUITE TOUCHES YOUTUBE, and nothing touches the machine's real
 * archive, install or preferences. YouTube is unreachable from the environments
 * this project is developed in and no real download has ever been started
 * through any window in it, so every test here runs against something built on
 * disk a moment earlier: a fixture tree in the documented archive shape, a fake
 * pipeline that emits the SHAPE of real output, a .vtt written by hand.
 *
 * Anything that would write to %LOCALAPPDATA% goes through
 * Paths.EnvironmentOverride with the profile variables pointed at a temp
 * directory, so running the suite cannot clobber the profiles or queue of
 * whoever runs it.
 */

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace YtdlWin.Tests;

public static class FixtureSupport
{
    public static string MakeTempDir(string prefix)
    {
        var path = Path.Combine(Path.GetTempPath(), $"{prefix}-{Guid.NewGuid():N}");
        Directory.CreateDirectory(path);
        return path;
    }

    public static void Mkdirp(string path) => Directory.CreateDirectory(path);

    public static void Write(string contents, string path)
    {
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        /* UTF-8 with NO byte-order mark. File.WriteAllText's default encoding
         * writes one, and a fixture manifest.json that starts with a BOM is not
         * what postprocess.ps1 writes -- testing against a shape the pipeline
         * does not produce is worse than not testing. */
        File.WriteAllText(path, contents, new UTF8Encoding(false));
    }

    public static void WriteBytes(byte[] bytes, string path)
    {
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        File.WriteAllBytes(path, bytes);
    }

    public static void Delete(string path)
    {
        try { if (Directory.Exists(path)) Directory.Delete(path, recursive: true); }
        catch (Exception) { /* a leftover temp directory is not a test failure */ }
    }

    /* A file that belongs to the REPOSITORY rather than to this app -- today
     * that means CLI_VERSION, which all three apps assert against.
     *
     * The build copies it next to the test binary, which is what makes this
     * work from a build anywhere. The walk up from the binary is the fallback
     * for a run where that copy did not happen. If both fail the test says so
     * rather than silently passing. */
    public static string? RepositoryFile(string name)
    {
        var beside = Path.Combine(AppContext.BaseDirectory, name);
        if (File.Exists(beside)) return beside;

        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (var i = 0; i < 8 && dir is not null; i++)
        {
            var candidate = Path.Combine(dir.FullName, name);
            if (File.Exists(candidate)) return candidate;
            dir = dir.Parent;
        }
        return null;
    }

    /* Point the app's notion of "home" at a temp directory.
     *
     * LOCALAPPDATA is what StateDir and CacheDir are derived from; USERPROFILE
     * and HOME are set too so that anything falling back to the profile also
     * lands inside the fixture. YTDLP_INSTALL_ROOT is redirected so a test that
     * probes for the pipeline does not find the real one, and PATH is passed
     * through unchanged so process tests can still find cmd.exe. */
    public static IReadOnlyDictionary<string, string> RedirectedEnvironment(string root)
    {
        return new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["LOCALAPPDATA"] = Path.Combine(root, "AppData", "Local"),
            ["APPDATA"] = Path.Combine(root, "AppData", "Roaming"),
            ["USERPROFILE"] = root,
            ["HOME"] = root,
            ["YTDLP_INSTALL_ROOT"] = Path.Combine(root, "yt-dlp"),
        };
    }
}

/* Every test class that redirects the environment does it through this, so the
 * override is always undone -- including when a test throws. A leaked override
 * would not fail the test that leaked it; it would fail whichever unrelated
 * test ran next, which is the worst kind of test-suite bug to chase. */
public sealed class RedirectedHome : IDisposable
{
    public string Root { get; }

    public RedirectedHome(string prefix)
    {
        Root = FixtureSupport.MakeTempDir(prefix);
        Core.Paths.EnvironmentOverride = FixtureSupport.RedirectedEnvironment(Root);
    }

    public void Dispose()
    {
        Core.Paths.EnvironmentOverride = null;
        FixtureSupport.Delete(Root);
    }
}
