/* The persisted preferences.
 *
 * Kept in %LOCALAPPDATA%\ytdl-win\state\settings.json -- NOT in the registry and
 * NOT in ApplicationData.Current.LocalSettings. The registry would be the
 * Windows-idiomatic home for four values, and LocalSettings would be the WinUI
 * one, and neither is used: the settings file sits beside profiles.json,
 * queue.json and history.json, which are files by necessity, and one visible
 * directory holding all of an app's state is worth more here than idiom. It is
 * also what makes "delete this folder and start over" a complete answer, which
 * neither of the other two homes can offer.
 *
 * ApplicationData.Current has a second problem specific to this app: it throws
 * outright in an UNPACKAGED process, and this app is built to run unpackaged as
 * well as packaged. A settings store that works only in one of the two
 * deployment shapes is not a settings store.
 *
 * Written through a temp file and replaced, like everything else this app owns.
 */

using System;

namespace YtdlWin.Core;

public sealed class Settings
{
    /// The `ytdl --path` equivalent. Empty means "wherever the pipeline puts it
    /// by default", which is the install root -- a real choice, not a missing
    /// value.
    public string DataRoot { get; set; } = "";

    /// An explicitly chosen archive root, if autodetection guessed wrong.
    public string ArchiveRoot { get; set; } = "";

    public int DefaultWorkers { get; set; } = 1;

    /* How the Library is ordered. Persisted as the sort key's stable ID
     * string, never as the enum's number: inserting a key in the middle would
     * otherwise silently change what every saved setting means.
     *
     * The FACETS are deliberately not persisted. A sort is a standing
     * preference for how you like to read a list; a facet is a question you
     * asked once, and an app that reopens showing a fifth of the archive with
     * no visible reason is an app that looks like it lost your videos. */
    public string LibrarySort { get; set; } = "date";

    /// Newest first. An archive is added to at the newest end, so what someone
    /// wants to see when the window opens is what arrived last.
    public bool LibrarySortDescending { get; set; } = true;

    /* How YouTube is reached: the Connection section on the Downloads page.
     *
     * Settings, not form fields and not profile fields. A cookie source or a
     * proxy is a fact about you and your network, not about the video you are
     * about to download -- so it is kept once, here, and the Runner stamps it
     * onto every run the app starts. The cookie SOURCE is stored apart from
     * the two values it chooses between, so flipping to "none" and back does
     * not lose a browser profile or a path somebody typed once.
     *
     * No file-mode tightening, unlike the Linux and macOS apps: this file lives
     * under %LOCALAPPDATA%, whose ACL already admits only this user (and
     * SYSTEM and Administrators), so a proxy password here is as private as
     * the user's own profile directory. */
    /// "none" | "browser" | "file"
    public string CookiesSource { get; set; } = "none";
    public string CookiesBrowser { get; set; } = "edge";
    public string CookiesProfile { get; set; } = "";
    public string CookiesFile { get; set; } = "";
    public string Proxy { get; set; } = "";
    public string LimitRate { get; set; } = "";
    /// "native" | "aria2c"
    public string Downloader { get; set; } = "native";

    /// Whether a finished queue or a failed run is announced as a toast while
    /// the window is in the background. On by default: the point is to hear
    /// about the run that failed at 3am, and a setting that has to be found
    /// first is one most people would never turn on. See Notices.cs.
    public bool Notify { get; set; } = true;

    private static string Path() => Paths.Join(Paths.StateDir(), "settings.json");

    public static Settings Load()
    {
        var s = new Settings();
        var obj = JsonFile.Object(Path());
        if (obj is null) return s;

        s.DataRoot = obj.Value.Str("data_root") ?? "";
        s.ArchiveRoot = obj.Value.Str("archive_root") ?? "";
        s.DefaultWorkers = (int)Math.Max(1, obj.Value.Int("default_workers", 1));
        s.LibrarySort = obj.Value.Str("library_sort") ?? "date";
        /* Absent reads as TRUE rather than false, which is what a plain Bool
         * lookup on a missing key would give: an upgrade from a settings.json
         * written before this existed must not silently flip every Library to
         * oldest-first. */
        s.LibrarySortDescending = obj.Value.TryGetProperty("library_sort_descending", out _)
            ? obj.Value.Bool("library_sort_descending")
            : true;
        s.CookiesSource = NonEmpty(obj.Value.Str("cookies_source")) ?? "none";
        s.CookiesBrowser = NonEmpty(obj.Value.Str("cookies_browser")) ?? "edge";
        s.CookiesProfile = obj.Value.Str("cookies_profile") ?? "";
        s.CookiesFile = obj.Value.Str("cookies_file") ?? "";
        s.Proxy = obj.Value.Str("proxy") ?? "";
        s.LimitRate = obj.Value.Str("limit_rate") ?? "";
        s.Downloader = NonEmpty(obj.Value.Str("downloader")) ?? "native";
        /* Absent reads as TRUE, for the upgrade case: a settings.json written
         * before this key existed must not switch notifications off. */
        s.Notify = obj.Value.TryGetProperty("notify", out _) ? obj.Value.Bool("notify") : true;
        return s;
    }

    private static string? NonEmpty(string? v) => string.IsNullOrEmpty(v) ? null : v;

    /* The connection settings as a RunOptions with only its five connection
     * fields set -- the shape Runner.SetConnection and the probe take. The
     * cookie source decides which of browser/file is emitted; a source whose
     * value is empty emits nothing rather than a flag with no argument. */
    public RunOptions Connection()
    {
        var o = new RunOptions();
        if (CookiesSource == "browser" && CookiesBrowser.Trim().Length > 0)
        {
            var profile = CookiesProfile.Trim();
            o.CookiesFromBrowser = profile.Length == 0
                ? CookiesBrowser.Trim()
                : CookiesBrowser.Trim() + ":" + profile;
        }
        else if (CookiesSource == "file" && CookiesFile.Trim().Length > 0)
        {
            /* Expanded here for the same reason --path is: ytdl.ps1 has no
             * notion of "~". */
            o.CookiesFile = Paths.ExpandTilde(CookiesFile.Trim());
        }
        o.Proxy = Proxy.Trim();
        o.LimitRate = LimitRate.Trim();
        o.Downloader = Downloader == "native" ? "" : Downloader;
        return o;
    }

    public void Save()
    {
        var data = JsonFile.Write(w =>
        {
            w.WriteStartObject();
            w.WriteString("data_root", DataRoot);
            w.WriteString("archive_root", ArchiveRoot);
            w.WriteNumber("default_workers", DefaultWorkers);
            w.WriteString("library_sort", LibrarySort);
            w.WriteBoolean("library_sort_descending", LibrarySortDescending);
            w.WriteString("cookies_source", CookiesSource);
            w.WriteString("cookies_browser", CookiesBrowser);
            w.WriteString("cookies_profile", CookiesProfile);
            w.WriteString("cookies_file", CookiesFile);
            w.WriteString("proxy", Proxy);
            w.WriteString("limit_rate", LimitRate);
            w.WriteString("downloader", Downloader);
            w.WriteBoolean("notify", Notify);
            w.WriteEndObject();
        });
        AtomicFile.Write(data, Path());
    }

    /// The data root as a real path: the configured value with ~ expanded, or
    /// the pipeline's own default. Never empty.
    public string ResolvedDataRoot
    {
        get
        {
            var trimmed = DataRoot.Trim();
            return trimmed.Length == 0 ? Paths.InstallRoot() : Paths.ExpandTilde(trimmed);
        }
    }
}
