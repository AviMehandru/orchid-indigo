/* Which videos the Library shows, and in what order.
 *
 * Split out of library_view.c rather than grown inside it, for two reasons
 * that are both about the other two apps. The rules here -- what "audio only"
 * means, how a date range treats a folder whose upload_date came from the
 * folder name, what an empty channel set means -- are a contract three
 * independent implementations have to agree on, exactly like the probe's
 * derivation is. Keeping them in one file per app, with the same shape and
 * the same fixture values, is what makes that agreement checkable instead of
 * hoped for. And a filter that is a plain struct with a pure predicate can be
 * tested without a window, which is the only kind of test this app's suite can
 * run at all.
 *
 * NOTHING HERE READS THE DISK. The index is already in memory, every field
 * this consults was populated by the scan, and ytdl_entry_total_size sums the
 * sizes the scan recorded. That is what makes it safe to re-run the whole
 * filter on every keystroke.
 *
 * WHAT THE THREE APPS AGREE ON, AND WHAT THEY DO NOT. The rules are the
 * contract: which videos a facet admits, that an empty channel set means all
 * of them, that an unparseable date is excluded by a range rather than kept,
 * that a missing key sorts last in BOTH directions, and that the order is
 * total. The collation of two present titles is NOT: this uses the platform's
 * locale-aware comparison, the Swift app uses the system's, and the C# app
 * uses .NET's. That is deliberate rather than an oversight -- a German user's
 * Library should sort the way the rest of their desktop sorts, and three apps
 * forced to agree byte-for-byte would all have to be wrong somewhere to do
 * it. So the shared fixtures assert the RULES on inputs whose ordering every
 * reasonable collation agrees on, and never assert a specific collation.
 */

#ifndef YTDL_LIBRARY_FILTER_H
#define YTDL_LIBRARY_FILTER_H

#include <glib.h>

#include "archive.h"

G_BEGIN_DECLS

/* The sort keys, in the order they are offered.
 *
 * DATE first because it is what the grid was already implicitly ordered by
 * and what someone opening the app expects. Every key falls back to title,
 * then to the archive-relative path, so the order is TOTAL: two videos with
 * the same duration do not swap places between one rebuild and the next,
 * which is the kind of flicker that reads as a bug. */
typedef enum
{
  YTDL_SORT_DATE = 0,
  YTDL_SORT_TITLE,
  YTDL_SORT_CHANNEL,
  YTDL_SORT_DURATION,
  YTDL_SORT_SIZE,
  YTDL_N_SORT_KEYS
} YtdlSortKey;

/* A short label for the sort dropdown. Never NULL for a valid key. */
const char *ytdl_sort_key_label (YtdlSortKey key);

/* The stable string a sort key is persisted as. Persisting the enum's
 * NUMBER would mean inserting a key in the middle silently changes what a
 * saved setting means. */
const char *ytdl_sort_key_id (YtdlSortKey key);
YtdlSortKey ytdl_sort_key_from_id (const char *id); /* YTDL_SORT_DATE if unknown */

/* The flag facets, as a bitmask. All of them are AND-ed: asking for
 * "audio only" and "no media file" together is asking for something no
 * folder can be, and it correctly shows nothing rather than quietly
 * becoming an OR.
 *
 * VERIFY_FAILED is different in kind from the other three and the difference
 * is worth stating, because it is the one a user can misread. The other
 * three are properties of the manifest and are known for every video the
 * moment it is indexed. Whether a folder still verifies is only knowable by
 * hashing every file in it, which takes seconds per video -- so this facet
 * filters on RECORDED results only, and a video that has never been verified
 * is not shown by it. It never claims an unverified video passes; it says
 * "of the ones I have checked, these failed". The UI has to say so too. */
typedef enum
{
  YTDL_FACET_NONE            = 0,
  YTDL_FACET_AUDIO_ONLY      = 1 << 0,
  YTDL_FACET_NO_MEDIA        = 1 << 1,
  YTDL_FACET_LAYOUT_TOO_NEW  = 1 << 2,
  YTDL_FACET_VERIFY_FAILED   = 1 << 3
} YtdlFacetFlags;

/* What is known about a video's last checksum verification. */
typedef enum
{
  YTDL_VERIFY_UNKNOWN = 0, /* never checked, or checked before the folder changed */
  YTDL_VERIFY_OK,
  YTDL_VERIFY_FAILED
} YtdlVerifyState;

/* Supplied by the caller so this file does not have to know where
 * verification results are kept -- they live in the cache directory, which is
 * verify_cache.h's business, and a filter that reached for it would be a
 * filter that could not be tested without one. */
typedef YtdlVerifyState (*YtdlVerifyLookup) (const YtdlEntry *entry,
                                             gpointer user_data);

typedef struct
{
  /* The substring search that was already here: title, uploader, id,
   * channel, case-folded. NULL or empty matches everything. */
  char *needle;

  /* Selected uploader folder names. EMPTY MEANS ALL, not none -- a facet
   * nobody has touched must not hide the whole library. Owned. */
  GPtrArray *channels;

  /* "YYYYMMDD" bounds, inclusive, either may be NULL. Compared as strings,
   * which is correct for this format and is also why a folder whose
   * upload_date came from the folder-name fallback still sorts and filters
   * sensibly: the fallback produces the same eight digits. An upload_date
   * that is not eight digits is EXCLUDED by any date bound rather than
   * silently kept -- it is a video whose date is unknown, and claiming it
   * falls inside a range would be inventing one. */
  char *date_from;
  char *date_to;

  YtdlFacetFlags flags;

  YtdlSortKey sort;
  gboolean    descending;
} YtdlLibraryFilter;

YtdlLibraryFilter *ytdl_library_filter_new (void);
void               ytdl_library_filter_free (YtdlLibraryFilter *filter);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlLibraryFilter, ytdl_library_filter_free)

/* Drop every facet and the needle. The SORT is deliberately left alone: it is
 * a view preference, not a filter, and "Clear filters" throwing away someone's
 * chosen ordering would be a surprise. */
void ytdl_library_filter_reset (YtdlLibraryFilter *filter);

/* TRUE when anything at all is narrowing the library. Drives the "filters
 * active" indicator, so it must NOT count the sort. */
gboolean ytdl_library_filter_is_narrowing (const YtdlLibraryFilter *filter);

/* How many facets are set, for the badge on the filter button. The needle is
 * not counted -- it has its own visible search bar. */
guint ytdl_library_filter_facet_count (const YtdlLibraryFilter *filter);

void     ytdl_library_filter_set_channel (YtdlLibraryFilter *filter,
                                          const char *channel, gboolean on);
gboolean ytdl_library_filter_has_channel (const YtdlLibraryFilter *filter,
                                          const char *channel);

/* One entry against the filter. @lookup may be NULL, in which case every
 * video reads as YTDL_VERIFY_UNKNOWN and the verify facet matches nothing. */
gboolean ytdl_library_filter_matches (const YtdlLibraryFilter *filter,
                                      const YtdlEntry *entry,
                                      YtdlVerifyLookup lookup,
                                      gpointer user_data);

/* Filter and sort a whole index in one pass.
 *
 * Returns a GPtrArray of BORROWED `const YtdlEntry *` -- the index owns them,
 * and the array must not outlive it. g_ptr_array_unref when done. */
GPtrArray *ytdl_library_filter_apply (const YtdlLibraryFilter *filter,
                                      const YtdlIndex *index,
                                      YtdlVerifyLookup lookup,
                                      gpointer user_data);

/* The comparison a given key implies, exposed for the test suite: sorting is
 * the half of this file where an error is invisible in a screenshot and
 * obvious in an assertion. Returns <0, 0 or >0 with @descending already
 * applied. */
int ytdl_library_filter_compare (const YtdlLibraryFilter *filter,
                                 const YtdlEntry *a, const YtdlEntry *b);

G_END_DECLS

#endif /* YTDL_LIBRARY_FILTER_H */
