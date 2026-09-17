/* What was learned the last time a video's checksums were verified.
 *
 * Verifying a folder means hashing every file in it -- seconds per video on a
 * large one -- so the answer is worth keeping. It is also worth distrusting:
 * the archive is not immutable. `ytdl --refresh` rewrites a folder's
 * sidecars, its hashes and, when it re-embeds the info.json, the media file
 * itself, all without changing download_mode. A cached "verifies" from before
 * that is not a stale opinion, it is a wrong one.
 *
 * So every record is stamped with the manifest's own archive_creation_time,
 * which postprocess.ps1 rewrites on every pass over a folder including a
 * refresh. A record whose stamp no longer matches is treated as absent rather
 * than as a result. That is the "key the cache on something that moves" rule
 * docs/archive-layout.md now states, implemented.
 *
 * THIS IS CACHE, NOT USER DATA, and that is why it lives under
 * %LOCALAPPDATA%\ytdl-win\cache rather than beside settings.json in state\.
 * Deleting it costs one re-verify. Nothing here is written inside the archive
 * -- checksums.sha256 covers every file in a video folder, so a dropped file
 * there makes that folder stop verifying, which would be a memorable way for a
 * verification cache to work.
 */

using System;
using System.Collections.Generic;
using System.Linq;

namespace YtdlWin.Core;

public sealed class VerifyCache
{
    private sealed class Record
    {
        public VerifyState State { get; init; }
        /// The manifest's archive_creation_time, or "" when it had none.
        public string Stamp { get; init; } = "";
    }

    private const int CacheVersion = 1;

    private readonly string _path;
    private readonly Dictionary<string, Record> _records = new(StringComparer.Ordinal);
    private bool _dirty;

    /// <summary>
    /// Loads from the cache directory. Never throws: an unreadable or corrupt
    /// store yields an empty cache, because the cost of being wrong here is one
    /// re-verify and the cost of refusing to start is the window.
    /// </summary>
    public VerifyCache(string? path = null)
    {
        _path = path ?? Paths.Join(Paths.CacheDir(), "verify.json");
        Load();
    }

    private static string StateId(VerifyState s) => s switch
    {
        VerifyState.Ok => "ok",
        VerifyState.Failed => "failed",
        _ => "unknown",
    };

    private static VerifyState StateFromId(string? id) => id switch
    {
        "ok" => VerifyState.Ok,
        "failed" => VerifyState.Failed,
        _ => VerifyState.Unknown,
    };

    private void Load()
    {
        var root = JsonFile.Object(_path);
        if (root is null) return;

        /* The version is read and ignored for now, deliberately: writing it
         * from the first release is what makes a future format change able to
         * discard old records instead of misreading them. A cache that cannot
         * say what shape it is in has to be thrown away wholesale the first
         * time the shape changes. */
        var videos = root.Value.Obj("videos");
        if (videos is null) return;

        foreach (var prop in videos.Value.EnumerateObject())
        {
            if (prop.Value.ValueKind != System.Text.Json.JsonValueKind.Object) continue;
            _records[prop.Name] = new Record
            {
                State = StateFromId(prop.Value.Str("state")),
                Stamp = prop.Value.Str("stamp") ?? "",
            };
        }
    }

    /// <summary>
    /// The recorded state for a video, or Unknown when there is none --
    /// including when there is one but it was recorded against a different
    /// version of the folder.
    /// </summary>
    public VerifyState State(ArchiveEntry entry)
    {
        if (!_records.TryGetValue(entry.Key, out var rec)) return VerifyState.Unknown;

        /* The whole point of the file. A record whose stamp no longer matches
         * the manifest was taken before something rewrote this folder -- a
         * refresh, a manual repair, a re-download -- and a verification result
         * from before the files changed is not a weaker answer than none, it is
         * a wrong one. Treated as absent rather than deleted here, because a
         * read should not mutate; the next write over this key replaces it. */
        if (!string.Equals(rec.Stamp, entry.CreationStamp ?? "", StringComparison.Ordinal))
        {
            return VerifyState.Unknown;
        }
        return rec.State;
    }

    /// <summary>
    /// Record a result. A folder with no manifest is stamped with the empty
    /// string and will only ever satisfy another unstamped read -- it cannot be
    /// cached against a stamp it does not have, and pretending otherwise is how
    /// a stale pass survives a refresh.
    /// </summary>
    public void Set(ArchiveEntry entry, VerifyState state)
    {
        _records[entry.Key] = new Record
        {
            State = state,
            Stamp = entry.CreationStamp ?? "",
        };
        _dirty = true;
    }

    /// <summary>
    /// How many videos have a record, for the facet's own label -- the "failed
    /// verification" facet has to be able to say what it is a subset of, or it
    /// reads as a claim about the whole library.
    /// </summary>
    public int KnownCount => _records.Values.Count(r => r.State != VerifyState.Unknown);

    /// <summary>
    /// Write the store back. Best-effort: a failure is swallowed, because
    /// losing a cache is not worth interrupting anyone over.
    /// </summary>
    public void Save()
    {
        if (!_dirty) return;

        var data = JsonFile.Write(w =>
        {
            w.WriteStartObject();
            w.WriteNumber("version", CacheVersion);
            w.WriteStartObject("videos");
            foreach (var (key, rec) in _records)
            {
                /* An Unknown record carries no information and would grow the
                 * file for every video anyone ever opened. */
                if (rec.State == VerifyState.Unknown) continue;
                w.WriteStartObject(key);
                w.WriteString("state", StateId(rec.State));
                w.WriteString("stamp", rec.Stamp);
                w.WriteEndObject();
            }
            w.WriteEndObject();
            w.WriteEndObject();
        });

        try
        {
            AtomicFile.Write(data, _path);
            _dirty = false;
        }
        catch (Exception)
        {
            /* Swallowed on purpose. This is a cache; the next run re-verifies,
             * and an exception escaping here would take down whatever was
             * closing the window. */
        }
    }
}
