/* Searching every comment and every caption line in the archive at once.
 *
 * The pipeline archives every comment and every subtitle cue, three apps parse
 * both, and until now the only way to find a phrase in them was to open one
 * video at a time. The data was already on disk and already understood; what
 * was missing was an index.
 *
 * WHY NOT SQLITE FTS5, which is the obvious answer and which
 * claude/orchid-indigo-gui-comparison.md guessed at. Two reasons, and the
 * second is the one that decided it:
 *
 *   1. The WinUI app would need Microsoft.Data.Sqlite. "No third-party package
 *      anywhere" is a structural claim this repository makes and keeps, and
 *      spending it on a feature that does not need it would be a poor trade.
 *   2. Three FTS engines means three TOKENIZERS. SQLite's unicode61, whatever
 *      the Swift wrapper configures, and whatever the .NET one does would have
 *      to agree about what a word is, forever, or the same query would return
 *      different videos on different platforms with nothing saying why. That
 *      is a fourth cross-language contract to hold in agreement, and the
 *      probe's shared fixture exists precisely because holding one is already
 *      expensive.
 *
 * So the index is a plain one, written here and identically in the other two
 * apps, and the tokenizer is eleven lines that all three can be checked
 * against the same fixture strings.
 *
 * WHAT IT STORES, and what it deliberately does not. Per video, per field, the
 * SORTED UNIQUE TOKENS of that field, space-joined and space-padded. It does
 * not store the text: an index that held every comment would be a second copy
 * of the archive's bulkiest content, and snippets are produced by re-reading
 * the matched videos' own files, which is cheap because a query matches a
 * handful of videos rather than all of them.
 *
 * The cost of that choice is a file that scales with the archive -- roughly a
 * few kilobytes of tokens per video, so single-digit megabytes for a few
 * hundred videos and tens for a few thousand. That is the deliberate trade
 * against an engine this project would otherwise have to take a dependency on.
 *
 * MATCHING IS BY TOKEN PREFIX, AND ALL TOKENS MUST MATCH. "rail brid" finds a
 * comment containing "railway" and "bridge" in any order, anywhere in the
 * field. Phrases are NOT supported by the index -- a token set cannot answer
 * "these words, adjacent, in this order" -- and the UI must not imply they
 * are. The snippets, which are read from the real text, do show where the
 * words actually fell.
 *
 * FRESHNESS. Every video's record carries the same stamp the verification
 * cache uses: the manifest's archive_creation_time, which postprocess.ps1
 * rewrites on every pass over a folder including `ytdl --refresh`. A video
 * whose stamp still matches is not re-parsed, so a rebuild after a rescan
 * costs only the videos that actually changed -- which, after a refresh that
 * fetched new comments, is exactly the right set.
 *
 * THIS IS CACHE. It lives in the cache directory, nothing is written inside
 * the archive, and deleting it costs one rebuild.
 */

#ifndef YTDL_SEARCH_INDEX_H
#define YTDL_SEARCH_INDEX_H

#include <glib.h>

#include "archive.h"

G_BEGIN_DECLS

/* Which text a search looks at.
 *
 * METADATA is the substring match over title, uploader, id and channel that
 * the Library already had, and it is the default: it needs no index, it
 * answers instantly, and it is what almost every search is. The other three
 * need the index and say so in the UI when it has not been built. */
typedef enum
{
  YTDL_SEARCH_METADATA = 0,
  YTDL_SEARCH_COMMENTS,
  YTDL_SEARCH_TRANSCRIPT,
  YTDL_SEARCH_EVERYTHING,
  YTDL_N_SEARCH_SCOPES
} YtdlSearchScope;

const char     *ytdl_search_scope_label (YtdlSearchScope scope);
const char     *ytdl_search_scope_id (YtdlSearchScope scope);
YtdlSearchScope ytdl_search_scope_from_id (const char *id);

/* TRUE for every scope that cannot be answered without the index. */
gboolean ytdl_search_scope_needs_index (YtdlSearchScope scope);

typedef struct _YtdlSearchIndex YtdlSearchIndex;

/* Loads from the cache directory. Never NULL: an unreadable or corrupt store
 * yields an empty index, because the cost of being wrong here is one rebuild
 * and the cost of refusing to start is the window. */
YtdlSearchIndex *ytdl_search_index_load (void);
void             ytdl_search_index_free (YtdlSearchIndex *index);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlSearchIndex, ytdl_search_index_free)

/* How many videos the index currently covers, and how many of the archive's
 * are missing from it or stale. Drives the "index N videos" prompt, which has
 * to be honest about what a search can currently see. */
guint ytdl_search_index_size (YtdlSearchIndex *index);
guint ytdl_search_index_outdated (YtdlSearchIndex *index,
                                  const YtdlIndex *archive);

/* Called on the BUILDING thread, once per video. */
typedef void (*YtdlSearchProgress) (gsize done, gsize total, gpointer user_data);

/* Parse every video whose stamp is missing or stale and record its tokens.
 *
 * Runs on whichever thread calls it and is the expensive operation in this
 * file: it parses every changed info.json, which is the same cost the detail
 * page pays per video, paid once for all of them. @cancellable may be NULL.
 *
 * Videos that have gone from the archive are dropped, so the store does not
 * grow forever across rescans. */
void ytdl_search_index_build (YtdlSearchIndex *index, const YtdlIndex *archive,
                              YtdlSearchProgress progress, gpointer user_data,
                              GCancellable *cancellable);

/* Best-effort write to the cache directory. */
void ytdl_search_index_save (YtdlSearchIndex *index);

/* The keys of the videos whose @scope text matches every token of @query.
 *
 * Returns a GHashTable of borrowed-key-to-itself used as a set; look entries
 * up in it with g_hash_table_contains. NULL is never returned; an empty query
 * yields an empty set, because "match everything" is the caller's business to
 * decide and not this function's to guess. */
GHashTable *ytdl_search_index_query (YtdlSearchIndex *index, const char *query,
                                     YtdlSearchScope scope);

/* ---------------------------------------------------------------------- */
/* Tokenizing, exposed because it is the contract                         */
/* ---------------------------------------------------------------------- */

/* Case-folded runs of letters and digits, everything else a separator.
 *
 * Exposed, and pinned by the test suite against the same strings the Swift and
 * C# suites use, because this is the one function whose disagreement between
 * the three apps would be invisible: the same query would quietly return
 * different videos on different platforms. Apostrophes are separators, so
 * "don't" is two tokens in every app -- which is a choice, not an oversight,
 * and it is the choice all three make. */
GPtrArray *ytdl_search_tokenize (const char *text); /* char*, owned */

/* The minimum token length the index keeps. One-character tokens match almost
 * everything and cost the most to store. */
#define YTDL_SEARCH_MIN_TOKEN 2

/* ---------------------------------------------------------------------- */
/* Snippets                                                               */
/* ---------------------------------------------------------------------- */

typedef struct
{
  char *text;  /* one comment, or one run of caption lines */
  char *who;   /* the comment's author, or a timestamp for a cue; may be NULL */
  gboolean from_transcript;
} YtdlSnippet;

void ytdl_snippet_free (gpointer snippet);

/* Read @entry's own comments and captions and return the passages that match.
 *
 * Deliberately NOT served from the index, which holds no text. This re-reads
 * the video's files, which is affordable precisely because it is called for
 * the handful of videos a query matched rather than for all of them. Runs on
 * whichever thread calls it and should not be the main one. */
GPtrArray *ytdl_search_snippets (const YtdlEntry *entry, const char *query,
                                 YtdlSearchScope scope, gsize max);

G_END_DECLS

#endif /* YTDL_SEARCH_INDEX_H */
