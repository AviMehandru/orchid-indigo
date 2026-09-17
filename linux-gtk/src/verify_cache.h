/* What was learned the last time a video's checksums were verified.
 *
 * Verifying a folder means hashing every file in it -- seconds per video on a
 * large one -- so the answer is worth keeping. It is also worth distrusting:
 * the archive is not immutable. `ytdl --refresh` rewrites a folder's
 * sidecars, its hashes and, when it re-embeds the info.json, the media file
 * itself, all without changing `download_mode`. A cached "verifies" from
 * before that is not a stale opinion, it is a wrong one.
 *
 * So every record is stamped with the manifest's own
 * `archive_creation_time`, which postprocess.ps1 rewrites on every pass over
 * a folder including a refresh. A record whose stamp no longer matches is
 * treated as absent rather than as a result. That is the "key the cache on
 * something that moves" rule docs/archive-layout.md now states, implemented.
 *
 * THIS IS CACHE, NOT USER DATA. It lives in the cache directory and deleting
 * it costs one re-verify. Nothing here is written inside the archive --
 * checksums.sha256 covers every file in a video folder, so a dropped file
 * there makes that folder stop verifying, which would be a memorable way for
 * a verification cache to work.
 */

#ifndef YTDL_VERIFY_CACHE_H
#define YTDL_VERIFY_CACHE_H

#include <glib.h>

#include "archive.h"
#include "library_filter.h"

G_BEGIN_DECLS

typedef struct _YtdlVerifyCache YtdlVerifyCache;

/* Loads from the cache directory. Never NULL and never fails: an unreadable
 * or corrupt store yields an empty cache, because the cost of being wrong
 * here is one re-verify and the cost of refusing to start is the window. */
YtdlVerifyCache *ytdl_verify_cache_load (void);
void             ytdl_verify_cache_free (YtdlVerifyCache *cache);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlVerifyCache, ytdl_verify_cache_free)

/* The recorded state for a video, or YTDL_VERIFY_UNKNOWN when there is none
 * -- including when there is one but it was recorded against a different
 * version of the folder. */
YtdlVerifyState ytdl_verify_cache_get (YtdlVerifyCache *cache,
                                       const YtdlEntry *entry);

/* Record a result. @stamp is the manifest's archive_creation_time and may be
 * NULL, in which case the record is stamped with the empty string and will
 * only ever satisfy another NULL-stamped read -- a folder with no manifest
 * cannot be cached against one, and pretending otherwise is how a stale pass
 * survives a refresh. */
void ytdl_verify_cache_set (YtdlVerifyCache *cache, const YtdlEntry *entry,
                            YtdlVerifyState state);

/* Write the store back. Best-effort: a failure is logged and swallowed,
 * because losing a cache is not worth interrupting anyone over. */
void ytdl_verify_cache_save (YtdlVerifyCache *cache);

/* How many videos have a usable record, for the facet's own label -- the
 * "failed verification" facet has to be able to say what it is a subset of,
 * or it reads as a claim about the whole library. */
guint ytdl_verify_cache_known (YtdlVerifyCache *cache);

/* The YtdlVerifyLookup adapter, so a cache can be handed straight to
 * ytdl_library_filter_apply. @user_data is the YtdlVerifyCache. */
YtdlVerifyState ytdl_verify_cache_lookup (const YtdlEntry *entry,
                                          gpointer user_data);

G_END_DECLS

#endif /* YTDL_VERIFY_CACHE_H */
