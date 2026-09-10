/* Named option sets.
 *
 * A profile is a YtdlRunOptions with the URL removed. That is the whole
 * design, and it is deliberate: profiles are NOT a second description of what
 * this window can do. Every field the runner accepts becomes profileable the
 * moment it exists, and a field added to YtdlRunOptions cannot be forgotten
 * here, because there is no per-field list in this file to forget it from --
 * the serialisation is ytdl_run_options_build_json, shared with the queue.
 *
 * What that costs is that an old profiles.json may not name a field a newer
 * build added. Every field is optional on read, so a profile written before an
 * option existed keeps working and simply does not set that option.
 *
 * THE URL IS DROPPED, not stored empty at the caller's discretion. A profile
 * carrying a URL would turn "select a profile" into "select a profile and
 * silently replace what I was about to download", which is the one thing a
 * preset must never do.
 *
 * Stored beside settings.json in the STATE dir, not the cache dir, and written
 * through a temp file and a rename: losing a set of profiles built up over
 * months is losing real work, unlike an index one rescan rebuilds.
 */

#ifndef YTDL_PROFILES_H
#define YTDL_PROFILES_H

#include <glib.h>

#include "pipeline.h"

G_BEGIN_DECLS

#define YTDL_PROFILE_MAX_NAME 60
#define YTDL_PROFILE_MAX      100

/* The one profile a fresh install starts with.
 *
 * It carries the app's OWN defaults -- every field unset -- rather than an
 * opinionated preset. That makes it the "put the form back" entry rather than a
 * second place this window decides what a download should look like: quality,
 * codec and container policy lives in run_ytdlp.ps1, on the far side of the
 * CLI_VERSION pin, and a shipped profile disagreeing with it would be exactly
 * the second opinion this app does not have.
 *
 * Ordinary in every other respect -- deletable, renameable, overwritable. */
#define YTDL_PROFILE_DEFAULT_NAME "Default"

typedef struct
{
  char           *name;
  YtdlRunOptions *opts;
  gint64          saved; /* unix seconds, for "last saved" in the UI */
} YtdlProfile;

typedef struct
{
  /* The profile selected when the window last closed, restored at startup.
   * NULL means "no profile", which is a real state -- it is what the window is
   * in before anything has been saved, and what Clear puts it back to. */
  char      *active;
  GPtrArray *profiles; /* YtdlProfile*, in creation order */
} YtdlProfileStore;

YtdlProfileStore *ytdl_profiles_load (void);
void              ytdl_profile_store_free (YtdlProfileStore *store);

/* Install YTDL_PROFILE_DEFAULT_NAME, once, on a machine that has never run
 * this app. Nothing is selected: the seeded profile is somewhere to go back
 * to, not a preset applied to a form the user has not touched yet.
 *
 * Keyed on the ABSENCE OF profiles.json, not on the store being empty.
 * Deleting the default leaves a file behind holding an empty list, so it stays
 * deleted rather than reappearing at the next launch -- a profile that cannot
 * be got rid of is worse than no profile at all. A file that exists but does
 * not parse is left alone for a harder reason: an unreadable store is still
 * somebody's profiles, and replacing it with a default is the one recovery
 * nobody can undo.
 *
 * Deliberately NOT part of ytdl_profiles_load. A reader that writes would seed
 * from any code path that happens to read the store, which is how "I deleted
 * it and it came back" is built. Called once, from main.
 *
 * TRUE when a profile was written. Best-effort: a failure here is not worth
 * refusing to start over, and the next launch tries again. */
gboolean ytdl_profiles_seed_default (void);

/* Case-insensitive, so "Archival" and "archival" are one profile rather than
 * two indistinguishable rows in a dropdown. NULL when there is no such name. */
const YtdlProfile *ytdl_profiles_get (const YtdlProfileStore *store,
                                      const char *name);

/* Create or overwrite by name, and make it active. The URL is cleared from
 * @opts before storing. Returns FALSE with @error set on a bad name or when
 * the limit is reached. */
gboolean ytdl_profiles_save (YtdlProfileStore *store, const char *name,
                             const YtdlRunOptions *opts, GError **error);

gboolean ytdl_profiles_delete (YtdlProfileStore *store, const char *name,
                               GError **error);

gboolean ytdl_profiles_rename (YtdlProfileStore *store, const char *from,
                               const char *to, GError **error);

/* NULL clears the selection. A name that no longer exists is an error rather
 * than a silent no-op, because the only way to reach it is a stale window. */
gboolean ytdl_profiles_activate (YtdlProfileStore *store, const char *name,
                                 GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlProfileStore, ytdl_profile_store_free)

G_END_DECLS

#endif /* YTDL_PROFILES_H */
