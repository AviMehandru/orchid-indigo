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
 * KEYED BY THE ARCHIVE KEY -- the SHA-256 of the archive-relative path -- for
 * the same reason every other handle in this app is: it survives a rescan, and
 * it is the one identifier that does not change when a manifest is rewritten.
 *
 * AND DELIBERATELY NOT STAMPED. The verification cache carries the manifest's
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
 */

#ifndef YTDL_USER_DATA_H
#define YTDL_USER_DATA_H

#include <glib.h>

G_BEGIN_DECLS

/* How much of a video counts as having watched it.
 *
 * 0.9 rather than 1.0 because nobody sits through the end card, and a video
 * you stopped 40 seconds from the end is one you have seen. Every player that
 * tracks this picks something in this region for the same reason. */
#define YTDL_WATCHED_FRACTION 0.9

/* A resume point below this is discarded rather than stored. Offering to
 * resume eight seconds in is worse than not offering: it costs a decision and
 * saves nothing. */
#define YTDL_RESUME_MIN_SECONDS 15.0

typedef struct
{
  char      *id;   /* stable across renames; never shown */
  char      *name;
  GPtrArray *keys; /* char*, archive keys, in the order the user put them */
} YtdlPlaylist;

typedef struct _YtdlUserData YtdlUserData;

/* Loads from the state directory. Never NULL: an unreadable or corrupt store
 * yields an empty one.
 *
 * That is a harsher trade than it is for a cache and it is still the right
 * one. Refusing to start because a JSON file is malformed would lose the user
 * the whole application rather than one file, and the alternative -- carrying
 * on with the file untouched so it can be recovered by hand -- is what the
 * first save would destroy anyway. What this DOES do is never write an empty
 * store over a file it failed to read; see ytdl_user_data_save. */
YtdlUserData *ytdl_user_data_load (void);
void          ytdl_user_data_free (YtdlUserData *ud);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlUserData, ytdl_user_data_free)

/* Write the store back if anything changed.
 *
 * Refuses to write when the load failed to PARSE an existing file, so a
 * corrupt userdata.json is left on disk for its owner to look at instead of
 * being replaced by an empty one the first time anything is touched. */
void ytdl_user_data_save (YtdlUserData *ud);

/* TRUE when the store could not be parsed and is therefore being held back
 * from saving. The UI says so rather than letting someone mark videos watched
 * into a void. */
gboolean ytdl_user_data_is_read_only (YtdlUserData *ud);

/* ---------------------------------------------------------------------- */
/* Watch state                                                            */
/* ---------------------------------------------------------------------- */

gboolean ytdl_user_data_is_watched (YtdlUserData *ud, const char *key);
void     ytdl_user_data_set_watched (YtdlUserData *ud, const char *key,
                                     gboolean watched);

/* Seconds to resume from, or 0 for "start at the beginning".
 *
 * 0 rather than -1 for absent because every caller wants to seek to it, and a
 * sentinel that has to be tested before use is a sentinel somebody forgets to
 * test. */
double ytdl_user_data_position (YtdlUserData *ud, const char *key);

/* Record where playback got to.
 *
 * @duration may be 0 when it is not known, in which case the position is
 * stored as given and nothing is inferred about being finished.
 *
 * Three cases, and the second is the one that makes this worth a function
 * rather than a setter:
 *
 *   past YTDL_WATCHED_FRACTION   marks the video WATCHED and clears the
 *                                resume point. Re-opening something you
 *                                finished should start it again, not drop you
 *                                back at the end card.
 *   below YTDL_RESUME_MIN_SECONDS  clears the resume point without touching
 *                                the watched flag. Opening a video and
 *                                closing it again must not litter the library
 *                                with eight-second resume offers.
 *   anything else                stores the position. */
void ytdl_user_data_set_position (YtdlUserData *ud, const char *key,
                                  double seconds, double duration);

/* How many videos have any state at all, for the UI to say what a facet is a
 * subset of. */
guint ytdl_user_data_watched_count (YtdlUserData *ud);

/* The watched keys as a set, BORROWED and LIVE: it is the same table the
 * store mutates, so a caller that has handed it to a filter does not have to
 * hand it over again after every change.
 *
 * Live rather than a copy because the Library re-runs its filter on every
 * keystroke, and because a snapshot is the shape of bug where marking a video
 * watched does nothing visible until something else happens to refresh. The
 * cost is that it must not outlive the store; the application owns both. */
GHashTable *ytdl_user_data_watched_keys (YtdlUserData *ud);

/* ---------------------------------------------------------------------- */
/* Playlists                                                              */
/* ---------------------------------------------------------------------- */

/* Borrowed, in creation order. Never NULL. */
GPtrArray *ytdl_user_data_playlists (YtdlUserData *ud); /* YtdlPlaylist* */

/* Borrowed, or NULL. */
YtdlPlaylist *ytdl_user_data_playlist (YtdlUserData *ud, const char *id);

/* Returns the new playlist, borrowed. A blank or whitespace-only name is
 * refused and returns NULL -- an unnamed playlist is unfindable. Duplicate
 * names are ALLOWED: they are the user's to make, ids are what identify a
 * playlist, and refusing "Watch later" twice would be this app deciding
 * something it has no business deciding. */
YtdlPlaylist *ytdl_user_data_playlist_create (YtdlUserData *ud,
                                              const char *name);

gboolean ytdl_user_data_playlist_rename (YtdlUserData *ud, const char *id,
                                         const char *name);
gboolean ytdl_user_data_playlist_delete (YtdlUserData *ud, const char *id);

/* Adding a key that is already in the playlist is a no-op rather than a
 * duplicate: a playlist is a set the user ordered, not a bag. */
gboolean ytdl_user_data_playlist_add (YtdlUserData *ud, const char *id,
                                      const char *key);
gboolean ytdl_user_data_playlist_remove (YtdlUserData *ud, const char *id,
                                         const char *key);
gboolean ytdl_user_data_playlist_contains (YtdlUserData *ud, const char *id,
                                           const char *key);

G_END_DECLS

#endif /* YTDL_USER_DATA_H */
