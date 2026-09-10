/* Named option sets.
 *
 * A profile is a RunOptions with the URL removed. That is the whole design, and
 * it is deliberate: profiles are NOT a second description of what this window
 * can do. Every field the runner accepts becomes profileable the moment it
 * exists, and a field added to RunOptions cannot be forgotten here, because
 * there is no per-field list in this file to forget it from -- the
 * serialisation is RunOptions.WriteTo, shared with the queue.
 *
 * What that costs is that an old profiles.json may not name a field a newer
 * build added. Every field is optional on read, so a profile written before an
 * option existed keeps working and simply does not set that option.
 *
 * THE URL IS DROPPED, not stored empty at the caller's discretion. A profile
 * carrying a URL would turn "select a profile" into "select a profile and
 * silently replace what I was about to download", which is the one thing a
 * preset must never do. It is dropped on the way IN as well as on the way out,
 * so a profiles.json edited by hand cannot hijack a download.
 *
 * Stored beside settings.json and written through a temp file and a replace:
 * losing a set of profiles built up over months is losing real work, unlike an
 * index one rescan rebuilds.
 */

using System;
using System.Collections.Generic;
using System.Linq;

namespace YtdlWin.Core;

public sealed class Profile
{
    public required string Name { get; set; }
    public required RunOptions Opts { get; set; }
    /// Unix seconds, for "last saved" in the UI.
    public required long Saved { get; set; }
}

public sealed class ProfileStore
{
    public const int MaxNameLength = 60;
    public const int MaxCount = 100;

    /* The one profile a fresh install starts with.
     *
     * It carries the app's OWN defaults -- every field unset -- rather than an
     * opinionated preset. That makes it the "put the form back" entry rather
     * than a second place this window decides what a download should look like:
     * quality, codec and container policy lives in run_ytdlp.ps1, on the far
     * side of the CLI_VERSION pin, and a shipped profile disagreeing with it
     * would be exactly the second opinion this app does not have.
     *
     * Ordinary in every other respect -- deletable, renameable, overwritable. */
    public const string DefaultName = "Default";

    /// The profile selected when the window last closed, restored at startup.
    /// null means "no profile", which is a real state -- it is what the window
    /// is in before anything has been saved.
    public string? Active { get; private set; }

    /// In creation order. Appended rather than sorted: a menu that reshuffles
    /// itself under the pointer is worse than one in an arbitrary but stable
    /// order.
    public List<Profile> Profiles { get; } = new();

    public sealed class ProfileException : Exception
    {
        public ProfileException(string message) : base(message) { }
    }

    private static string Path() => Paths.Join(Paths.StateDir(), "profiles.json");

    // MARK: - Loading

    public static ProfileStore Load()
    {
        var store = new ProfileStore();
        var obj = JsonFile.Object(Path());
        if (obj is null) return store;

        store.Active = obj.Value.Str("active");

        foreach (var po in obj.Value.Objects("profiles"))
        {
            var name = po.Str("name");
            if (name is null) continue;

            var opts = RunOptions.FromJson(po.Obj("opts"));
            /* A stored URL is dropped on read as well as on write. A
             * profiles.json hand-edited to carry one must not be able to hijack
             * a download. */
            opts.Url = "";
            store.Profiles.Add(new Profile { Name = name, Opts = opts, Saved = po.Int("saved") });
        }

        /* An active name that no longer matches anything is cleared rather than
         * left pointing at nothing. */
        if (store.Active is not null && store.Position(store.Active) < 0) store.Active = null;
        return store;
    }

    /* Keyed on the ABSENCE OF profiles.json, not on the store being empty.
     * Deleting the default leaves a file behind holding an empty list, so it
     * stays deleted rather than reappearing at the next launch -- a profile
     * that cannot be got rid of is worse than no profile at all. A file that
     * exists but does not parse is left alone for a harder reason: an
     * unreadable store is still somebody's profiles, and replacing it with a
     * default is the one recovery nobody can undo.
     *
     * Deliberately NOT part of Load. A reader that writes would seed from any
     * code path that happens to read the store, which is how "I deleted it and
     * it came back" is built. Called once, from AppModel's constructor. */

    /// Install DefaultName, once, on a machine that has never run this app.
    /// Nothing is selected: the seeded profile is somewhere to go back to, not
    /// a preset applied to a form the user has not touched yet. Returns true
    /// when a profile was written. Best-effort -- a failure here is not worth
    /// refusing to launch over, and the next launch tries again.
    public static bool SeedDefaultIfMissing()
    {
        /* Paths.IsRegularFile rather than "did it parse": see above. A
         * profiles.json that exists is never overwritten here, even when
         * nothing in it can be read. */
        if (Paths.IsRegularFile(Path())) return false;

        var store = new ProfileStore();
        /* The app's own defaults, not a preset. Every field unset, which is
         * what makes selecting it equivalent to a form nobody has touched. */
        store.Profiles.Add(new Profile
        {
            Name = DefaultName, Opts = new RunOptions(), Saved = Format.NowUnix(),
        });

        // store.Active stays null.
        try { store.Write(); }
        catch (ProfileException) { return false; }
        return true;
    }

    // MARK: - Lookup

    /* Case-insensitive, so "Archival" and "archival" are one profile rather
     * than two indistinguishable rows in a menu.
     *
     * OrdinalIgnoreCase rather than a culture-aware comparison, which is the
     * kind of thing that looks like pedantry until it is not: under a Turkish
     * locale a culture-aware compare says "ARCHIVAL" and "archival" differ,
     * because the uppercase of "i" there is not "I". A profile that cannot be
     * found by its own name on one machine and can on another is worse than
     * either behaviour consistently. */
    private int Position(string? name)
    {
        if (name is null) return -1;
        return Profiles.FindIndex(p =>
            string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase));
    }

    public Profile? Get(string? name)
    {
        var i = Position(name);
        return i < 0 ? null : Profiles[i];
    }

    // MARK: - Operations

    private static string CleanName(string name)
    {
        var n = name.Trim();
        if (n.Length == 0) throw new ProfileException("A profile needs a name.");
        if (n.Length > MaxNameLength)
            throw new ProfileException($"Profile names are limited to {MaxNameLength} characters.");
        return n;
    }

    /// Create or overwrite by name, and make it active. The URL is cleared from
    /// <paramref name="opts"/> before storing; the caller's own options are
    /// untouched, because saving a profile must not clear the URL box the user
    /// is still working in.
    public void Save(string name, RunOptions opts)
    {
        var clean = CleanName(name);

        var copy = opts.Clone();
        copy.Url = "";

        var i = Position(clean);
        if (i >= 0)
        {
            Profiles[i].Opts = copy;
            Profiles[i].Saved = Format.NowUnix();
            /* Keep the name as newly typed, so re-saving "Archival" over
             * "archival" fixes the capitalisation rather than ignoring it. */
            Profiles[i].Name = clean;
        }
        else
        {
            if (Profiles.Count >= MaxCount)
                throw new ProfileException($"That would be more than {MaxCount} profiles. Delete one first.");
            Profiles.Add(new Profile { Name = clean, Opts = copy, Saved = Format.NowUnix() });
        }

        Active = clean;
        Write();
    }

    public void Delete(string name)
    {
        var i = Position(name);
        if (i < 0) throw new ProfileException($"There is no profile called “{name}”.");

        var wasActive = string.Equals(Active, Profiles[i].Name, StringComparison.OrdinalIgnoreCase);
        Profiles.RemoveAt(i);
        /* Left pointing at a name that no longer exists, the menu would show a
         * selection that cannot be applied. */
        if (wasActive) Active = null;
        Write();
    }

    public void Rename(string from, string to)
    {
        var clean = CleanName(to);
        var i = Position(from);
        if (i < 0) throw new ProfileException($"There is no profile called “{from}”.");

        /* Renaming onto an existing name collides -- unless it is this same
         * profile being re-capitalised, which is a rename people actually do. */
        var j = Position(clean);
        if (j >= 0 && j != i)
            throw new ProfileException($"There is already a profile called “{clean}”.");

        var wasActive = string.Equals(Active, Profiles[i].Name, StringComparison.OrdinalIgnoreCase);
        Profiles[i].Name = clean;
        if (wasActive) Active = clean;
        Write();
    }

    /// null clears the selection. A name that no longer exists is an error
    /// rather than a silent no-op, because the only way to reach it is a stale
    /// window.
    public void Activate(string? name)
    {
        if (name is null)
        {
            Active = null;
            Write();
            return;
        }
        var p = Get(name) ?? throw new ProfileException($"There is no profile called “{name}”.");
        Active = p.Name;
        Write();
    }

    // MARK: - Writing

    private void Write()
    {
        var data = JsonFile.Write(w =>
        {
            w.WriteStartObject();
            if (Active is null) w.WriteNull("active");
            else w.WriteString("active", Active);

            w.WriteStartArray("profiles");
            foreach (var p in Profiles)
            {
                w.WriteStartObject();
                w.WriteString("name", p.Name);
                w.WriteNumber("saved", p.Saved);
                w.WritePropertyName("opts");
                p.Opts.WriteTo(w);
                w.WriteEndObject();
            }
            w.WriteEndArray();
            w.WriteEndObject();
        });

        if (!AtomicFile.Write(data, Path()))
            throw new ProfileException($"Could not write {Path()}.");
    }
}
