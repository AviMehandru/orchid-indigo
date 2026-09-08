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

    private static string Path() => Paths.Join(Paths.StateDir(), "settings.json");

    public static Settings Load()
    {
        var s = new Settings();
        var obj = JsonFile.Object(Path());
        if (obj is null) return s;

        s.DataRoot = obj.Value.Str("data_root") ?? "";
        s.ArchiveRoot = obj.Value.Str("archive_root") ?? "";
        s.DefaultWorkers = (int)Math.Max(1, obj.Value.Int("default_workers", 1));
        return s;
    }

    public void Save()
    {
        var data = JsonFile.Write(w =>
        {
            w.WriteStartObject();
            w.WriteString("data_root", DataRoot);
            w.WriteString("archive_root", ArchiveRoot);
            w.WriteNumber("default_workers", DefaultWorkers);
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
