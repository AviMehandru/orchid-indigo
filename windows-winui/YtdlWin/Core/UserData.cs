/* What the person did, as opposed to what the pipeline wrote.
 *
 * Watch state, resume positions and hand-made playlists. Three things the
 * archive itself has no opinion about and never will: postprocess.ps1 records
 * what was downloaded, and nothing on disk knows whether anybody has seen it.
 *
 * THIS IS USER DATA, NOT CACHE, and that is why it lives in the STATE
 * directory beside settings.json, the queue and the history rather than in the
 * cache directory beside the archive index and the verification results.
 * Deleting the cache costs a rescan. Deleting this loses the fact that you
 * watched something, which nothing can reconstruct.
 *
 * KEYED BY THE ARCHIVE KEY, for the same reason every other handle in this app
 * is: it survives a rescan, and it is the one identifier that does not change
 * when a manifest is rewritten.
 *
 * AND DELIBERATELY NOT STAMPED. VerifyCache carries the manifest's
 * archive_creation_time and discards a record whose stamp has moved, because
 * "these bytes verify" stops being true when the bytes change. Watch state is
 * the opposite kind of fact: "I have seen this video" is about the person, not
 * about the folder, and `ytdl --refresh` fetching newer comments does not
 * un-watch anything. A resume position survives for the same reason -- a
 * refresh can rewrite the media container's attachments but not its timeline.
 * The one thing that would invalidate a position is a genuinely different
 * video at the same path, and that is a different video with a different key.
 *
 * Nothing here is written inside the archive: checksums.sha256 covers every
 * file in a video folder, so a "watched" marker dropped in there would make
 * that folder stop verifying.
 *
 * The rules, the thresholds and the fixture values are shared with the GTK and
 * SwiftUI suites. The three apps are independent implementations of one
 * contract, exactly as the probe's derivation is.
 */

using System;
using System.Collections.Generic;
using System.Linq;

namespace YtdlWin.Core;

/// <summary>One playlist: a set the user ordered, not a bag.</summary>
public sealed class Playlist
{
    /// Stable across renames; never shown.
    public string Id { get; init; } = "";
    public string Name { get; set; } = "";
    /// Archive keys, in the order the user put them.
    public List<string> Keys { get; init; } = new();
}

public sealed class UserData
{
    /// <summary>How much of a video counts as having watched it.</summary>
    /* 0.9 rather than 1.0 because nobody sits through the end card, and a
     * video you stopped 40 seconds from the end is one you have seen. Every
     * player that tracks this picks something in this region. */
    public const double WatchedFraction = 0.9;

    /// <summary>
    /// A resume point below this is discarded rather than stored. Offering to
    /// resume eight seconds in is worse than not offering: it costs a decision
    /// and saves nothing.
    /// </summary>
    public const double ResumeMinSeconds = 15.0;

    private sealed class WatchState
    {
        public bool Watched { get; set; }
        /// Seconds; 0 means no resume point.
        public double Position { get; set; }
        /// ISO 8601, for a human reading the file.
        public string Updated { get; set; } = "";
    }

    private const int StoreVersion = 1;

    private readonly string _path;
    private readonly Dictionary<string, WatchState> _watch = new(StringComparer.Ordinal);

    /* Just the keys whose record says watched, as a set. Maintained beside
     * _watch rather than derived on demand because the Library's Unwatched
     * facet reads it on every keystroke, and because the filter is
     * deliberately a plain membership test against a borrowed set. Only
     * SetWatchedFlag changes a watched flag, so the two cannot drift. */
    private readonly HashSet<string> _watched = new(StringComparer.Ordinal);

    private readonly List<Playlist> _playlists = new();
    private bool _dirty;

    /// <summary>
    /// True when the store could not be PARSED and is therefore being held
    /// back from saving. The UI says so rather than letting someone mark
    /// videos watched into a void.
    /// </summary>
    public bool IsReadOnly { get; private set; }

    /// <summary>
    /// Loads from the state directory. Never throws.
    /// </summary>
    /* A file that is not there is the ordinary first-launch state and is NOT
     * "unreadable": there is nothing to protect, and the first save should
     * create it. Only a file that exists and will not parse gets the flag. */
    public UserData(string? path = null)
    {
        _path = path ?? Paths.Join(Paths.StateDir(), "userdata.json");
        Load();
    }

    private void Load()
    {
        if (!Paths.IsRegularFile(_path)) return;

        var root = JsonFile.Object(_path);
        if (root is null)
        {
            IsReadOnly = true;
            return;
        }

        /* The version is read and ignored for now, deliberately: writing it
         * from the first release is what makes a future format change able to
         * discard old records instead of misreading them. */
        var watch = root.Value.Obj("watch");
        if (watch is not null)
        {
            foreach (var prop in watch.Value.EnumerateObject())
            {
                if (prop.Value.ValueKind != System.Text.Json.JsonValueKind.Object) continue;
                var w = new WatchState
                {
                    Watched = prop.Value.Bool("watched"),
                    Position = Math.Max(0, prop.Value.Double("position")),
                    Updated = prop.Value.Str("updated") ?? "",
                };
                _watch[prop.Name] = w;
                if (w.Watched) _watched.Add(prop.Name);
            }
        }

        foreach (var rec in root.Value.Objects("playlists"))
        {
            var id = rec.Str("id");
            var name = rec.Str("name");
            if (id is null || name is null) continue;
            _playlists.Add(new Playlist
            {
                Id = id,
                Name = name,
                Keys = rec.Strings("keys"),
            });
        }
    }

    /// <summary>
    /// Write the store back if anything changed.
    /// </summary>
    /* Refuses to write when the load failed to PARSE an existing file, so a
     * corrupt userdata.json is left on disk for its owner to look at instead
     * of being replaced by an empty one the first time anything is touched. */
    public void Save()
    {
        if (!_dirty || IsReadOnly) return;

        var data = JsonFile.Write(w =>
        {
            w.WriteStartObject();
            w.WriteNumber("version", StoreVersion);

            w.WriteStartObject("watch");
            foreach (var (key, rec) in _watch)
            {
                /* A record that says nothing would grow the file for every
                 * video anyone ever opened. */
                if (!rec.Watched && rec.Position <= 0) continue;
                w.WriteStartObject(key);
                w.WriteBoolean("watched", rec.Watched);
                w.WriteNumber("position", rec.Position);
                w.WriteString("updated", rec.Updated);
                w.WriteEndObject();
            }
            w.WriteEndObject();

            w.WriteStartArray("playlists");
            foreach (var pl in _playlists)
            {
                w.WriteStartObject();
                w.WriteString("id", pl.Id);
                w.WriteString("name", pl.Name);
                w.WriteStartArray("keys");
                foreach (var k in pl.Keys) w.WriteStringValue(k);
                w.WriteEndArray();
                w.WriteEndObject();
            }
            w.WriteEndArray();

            w.WriteEndObject();
        });

        try
        {
            AtomicFile.Write(data, _path);
            _dirty = false;
        }
        catch (Exception)
        {
            /* Swallowed, like the caches -- but unlike them this is data that
             * cannot be rebuilt, so the NEXT save is still attempted: _dirty
             * stays set rather than being cleared. */
        }
    }

    // ---------------------------------------------------------------- //
    // Watch state                                                      //
    // ---------------------------------------------------------------- //

    /* The ONE place a watched flag changes, so the set beside _watch cannot
     * drift. Returns true when something actually moved. */
    private bool SetWatchedFlag(string key, WatchState w, bool watched)
    {
        if (w.Watched == watched) return false;

        w.Watched = watched;
        if (watched) _watched.Add(key);
        else _watched.Remove(key);
        return true;
    }

    private WatchState Record(string key)
    {
        if (_watch.TryGetValue(key, out var w)) return w;
        w = new WatchState();
        _watch[key] = w;
        return w;
    }

    public bool IsWatched(string key) =>
        _watch.TryGetValue(key, out var w) && w.Watched;

    public void SetWatched(string key, bool watched)
    {
        var w = Record(key);
        if (!SetWatchedFlag(key, w, watched)) return;

        /* Marking something watched by hand clears a resume point: the two
         * would otherwise disagree, and the flag is the more deliberate
         * statement of the pair. Marking it UNWATCHED leaves the position
         * alone -- "I want to see this again" and "start it over" are
         * different wishes. */
        if (watched) w.Position = 0;

        w.Updated = NowIso();
        _dirty = true;
    }

    /// <summary>
    /// Seconds to resume from, or 0 for "start at the beginning".
    /// </summary>
    /* 0 rather than -1 for absent because every caller wants to seek to it,
     * and a sentinel that has to be tested before use is a sentinel somebody
     * forgets to test. */
    public double Position(string key) =>
        _watch.TryGetValue(key, out var w) ? w.Position : 0;

    /// <summary>
    /// Record where playback got to. <paramref name="duration"/> may be 0 when
    /// it is not known, in which case the position is stored as given and
    /// nothing is inferred about being finished.
    /// </summary>
    /* Three cases, and the second is the one that makes this worth a method
     * rather than a setter. */
    public void SetPosition(string key, double seconds, double duration)
    {
        if (seconds < 0) seconds = 0;

        var finished = duration > 0 && seconds >= duration * WatchedFraction;
        var w = Record(key);

        if (finished)
        {
            /* Watched, and no resume point: re-opening something you finished
             * should start it again rather than drop you back at the end
             * card. */
            SetWatchedFlag(key, w, true);
            w.Position = 0;
        }
        else if (seconds < ResumeMinSeconds)
        {
            /* Opening a video and closing it again must not litter the library
             * with eight-second resume offers. The watched flag is untouched:
             * a video you have already seen does not become unseen because you
             * glanced at the first ten seconds of it. */
            w.Position = 0;
        }
        else
        {
            w.Position = seconds;
        }

        w.Updated = NowIso();
        _dirty = true;
    }

    /// <summary>
    /// How many videos are marked watched, for the UI to say what the facet is
    /// a subset of.
    /// </summary>
    /* The set's size, not a walk of _watch: the two must agree, and reading
     * the answer off the set is what makes a drift between them show up as a
     * wrong count in the tests rather than as a facet that quietly disagrees
     * with the label above it. */
    public int WatchedCount => _watched.Count;

    /// <summary>
    /// The watched keys as a set, BORROWED and LIVE: it is the same set this
    /// store mutates, so a caller that has handed it to a filter does not have
    /// to hand it over again after every change.
    /// </summary>
    /* Live rather than a copy because the Library re-runs its filter on every
     * keystroke, and because a snapshot is the shape of bug where marking a
     * video watched does nothing visible until something else happens to
     * refresh. The cost is that it must not outlive the store. */
    public HashSet<string> WatchedKeys => _watched;

    // ---------------------------------------------------------------- //
    // Playlists                                                        //
    // ---------------------------------------------------------------- //

    /// <summary>Borrowed, in creation order.</summary>
    public IReadOnlyList<Playlist> Playlists => _playlists;

    /* GetPlaylist, not Playlist: a method whose name is also a type's is
     * legal and is exactly the kind of thing that turns one later typo into a
     * confusing diagnostic. The GTK and Swift ports spell it `playlist`
     * because neither language has the ambiguity. */
    public Playlist? GetPlaylist(string id) =>
        _playlists.FirstOrDefault(p => string.Equals(p.Id, id, StringComparison.Ordinal));

    /// <summary>
    /// Returns the new playlist, or null for a blank or whitespace-only name --
    /// an unnamed playlist is unfindable.
    /// </summary>
    /* Duplicate names are ALLOWED: they are the user's to make, ids are what
     * identify a playlist, and refusing "Watch later" twice would be this app
     * deciding something it has no business deciding. */
    public Playlist? CreatePlaylist(string name)
    {
        var trimmed = (name ?? "").Trim();
        if (trimmed.Length == 0) return null;

        var pl = new Playlist { Id = Guid.NewGuid().ToString(), Name = trimmed };
        _playlists.Add(pl);
        _dirty = true;
        return pl;
    }

    public bool RenamePlaylist(string id, string name)
    {
        var trimmed = (name ?? "").Trim();
        if (trimmed.Length == 0) return false;

        var pl = GetPlaylist(id);
        if (pl is null) return false;
        pl.Name = trimmed;
        _dirty = true;
        return true;
    }

    public bool DeletePlaylist(string id)
    {
        var pl = GetPlaylist(id);
        if (pl is null) return false;
        _playlists.Remove(pl);
        _dirty = true;
        return true;
    }

    /// <summary>
    /// Adding a key that is already in the playlist is a no-op rather than a
    /// duplicate: a playlist is a set the user ordered, not a bag.
    /// </summary>
    public bool AddToPlaylist(string id, string key)
    {
        var pl = GetPlaylist(id);
        if (pl is null) return false;
        if (pl.Keys.Contains(key, StringComparer.Ordinal)) return false;
        pl.Keys.Add(key);
        _dirty = true;
        return true;
    }

    public bool RemoveFromPlaylist(string id, string key)
    {
        var pl = GetPlaylist(id);
        if (pl is null) return false;
        if (!pl.Keys.Remove(key)) return false;
        _dirty = true;
        return true;
    }

    public bool PlaylistContains(string id, string key) =>
        GetPlaylist(id)?.Keys.Contains(key, StringComparer.Ordinal) ?? false;

    /// <summary>
    /// A playlist's keys as a set, for the filter's PlaylistKeys. An EMPTY
    /// playlist yields an empty set rather than null: "this playlist has
    /// nothing in it" must show nothing, not everything.
    /// </summary>
    public HashSet<string>? PlaylistKeys(string id)
    {
        var pl = GetPlaylist(id);
        if (pl is null) return null;
        return new HashSet<string>(pl.Keys, StringComparer.Ordinal);
    }

    // ---------------------------------------------------------------- //

    private static string NowIso() => DateTimeOffset.Now.ToString("o");
}
