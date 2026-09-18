#include "library_filter.h"

#include <string.h>

/* ---------------------------------------------------------------------- */
/* Sort keys                                                              */
/* ---------------------------------------------------------------------- */

/* Parallel arrays rather than a struct table, kept adjacent so a key added
 * to one and not the other is visible in a three-line diff. test_library
 * asserts every key has both. */
static const char *const SORT_LABELS[YTDL_N_SORT_KEYS] = {
  "Upload date", "Title", "Channel", "Duration", "Size",
};

static const char *const SORT_IDS[YTDL_N_SORT_KEYS] = {
  "date", "title", "channel", "duration", "size",
};

const char *
ytdl_sort_key_label (YtdlSortKey key)
{
  if (key < 0 || key >= YTDL_N_SORT_KEYS)
    return SORT_LABELS[YTDL_SORT_DATE];
  return SORT_LABELS[key];
}

const char *
ytdl_sort_key_id (YtdlSortKey key)
{
  if (key < 0 || key >= YTDL_N_SORT_KEYS)
    return SORT_IDS[YTDL_SORT_DATE];
  return SORT_IDS[key];
}

YtdlSortKey
ytdl_sort_key_from_id (const char *id)
{
  if (id == NULL)
    return YTDL_SORT_DATE;
  for (int i = 0; i < YTDL_N_SORT_KEYS; i++)
    if (g_strcmp0 (id, SORT_IDS[i]) == 0)
      return (YtdlSortKey) i;
  /* An id this build does not know is a setting written by a newer one.
   * Falling back to the default is the same rule the profile store uses for
   * an option it has never heard of: ignore it, do not refuse to start. */
  return YTDL_SORT_DATE;
}

/* ---------------------------------------------------------------------- */
/* Construction                                                           */
/* ---------------------------------------------------------------------- */

YtdlLibraryFilter *
ytdl_library_filter_new (void)
{
  YtdlLibraryFilter *f = g_new0 (YtdlLibraryFilter, 1);
  f->channels = g_ptr_array_new_with_free_func (g_free);
  f->sort = YTDL_SORT_DATE;
  /* Newest first. The only default that is not a coin toss: an archive is
   * added to at the newest end, so what someone wants to see when the window
   * opens is what arrived last. */
  f->descending = TRUE;
  return f;
}

void
ytdl_library_filter_free (YtdlLibraryFilter *filter)
{
  if (filter == NULL)
    return;
  g_free (filter->needle);
  g_free (filter->date_from);
  g_free (filter->date_to);
  g_clear_pointer (&filter->channels, g_ptr_array_unref);
  g_free (filter);
}

void
ytdl_library_filter_reset (YtdlLibraryFilter *filter)
{
  g_return_if_fail (filter != NULL);
  g_clear_pointer (&filter->needle, g_free);
  g_clear_pointer (&filter->date_from, g_free);
  g_clear_pointer (&filter->date_to, g_free);
  if (filter->channels->len > 0)
    g_ptr_array_remove_range (filter->channels, 0, filter->channels->len);
  filter->flags = YTDL_FACET_NONE;
  /* Cleared, not freed: all three are borrowed from whoever built them --
   * the search control, and the user-data store. */
  filter->key_allow = NULL;
  filter->playlist_keys = NULL;
  /* watched_keys is NOT cleared. It is not a filter: it is the set the
   * UNWATCHED facet reads, and it stays valid whether or not that facet is
   * on. Clearing it here would make "Clear filters" silently turn every
   * video unwatched the next time the facet was ticked. */
  /* sort and descending survive on purpose -- see the header. */
}

guint
ytdl_library_filter_facet_count (const YtdlLibraryFilter *filter)
{
  g_return_val_if_fail (filter != NULL, 0);

  guint n = 0;
  if (filter->channels->len > 0)
    n++;
  /* One date facet, not two: "2024 only" is a single idea the user had, and
   * counting it twice makes the badge read as more narrowing than it is. */
  if (filter->date_from != NULL || filter->date_to != NULL)
    n++;
  if (filter->playlist_keys != NULL)
    n++;
  for (guint bit = 0; bit < YTDL_N_FACET_FLAGS; bit++)
    if (filter->flags & (1u << bit))
      n++;
  return n;
}

gboolean
ytdl_library_filter_is_narrowing (const YtdlLibraryFilter *filter)
{
  g_return_val_if_fail (filter != NULL, FALSE);
  if (filter->needle != NULL && *filter->needle != '\0')
    return TRUE;
  /* A collection-wide search narrows without putting anything in the needle,
   * so the empty state would otherwise say "there is no archive here" when a
   * comment search simply found nothing. */
  if (filter->key_allow != NULL)
    return TRUE;
  return ytdl_library_filter_facet_count (filter) > 0;
}

const char *
ytdl_facet_flag_label (YtdlFacetFlags flag)
{
  switch (flag)
    {
    case YTDL_FACET_AUDIO_ONLY:
      return "Audio only";
    case YTDL_FACET_NO_MEDIA:
      return "No media file";
    case YTDL_FACET_LAYOUT_TOO_NEW:
      return "Newer archive layout";
    case YTDL_FACET_VERIFY_FAILED:
      return "Failed verification";
    case YTDL_FACET_UNWATCHED:
      return "Unwatched";
    default:
      return "";
    }
}

gboolean
ytdl_library_filter_has_channel (const YtdlLibraryFilter *filter,
                                 const char *channel)
{
  g_return_val_if_fail (filter != NULL, FALSE);
  if (channel == NULL)
    return FALSE;
  for (guint i = 0; i < filter->channels->len; i++)
    if (g_strcmp0 (g_ptr_array_index (filter->channels, i), channel) == 0)
      return TRUE;
  return FALSE;
}

void
ytdl_library_filter_set_channel (YtdlLibraryFilter *filter,
                                 const char *channel, gboolean on)
{
  g_return_if_fail (filter != NULL);
  g_return_if_fail (channel != NULL);

  for (guint i = 0; i < filter->channels->len; i++)
    {
      if (g_strcmp0 (g_ptr_array_index (filter->channels, i), channel) != 0)
        continue;
      if (!on)
        g_ptr_array_remove_index (filter->channels, i);
      return; /* already in the set: adding again is a no-op, not a duplicate */
    }
  if (on)
    g_ptr_array_add (filter->channels, g_strdup (channel));
}

/* ---------------------------------------------------------------------- */
/* Matching                                                               */
/* ---------------------------------------------------------------------- */

/* The eight-digit form the layout contract specifies, and the same form the
 * folder-name fallback produces. Anything else is a date this reader does not
 * have, which is not the same as a date outside the range. */
static gboolean
is_yyyymmdd (const char *s)
{
  if (s == NULL || strlen (s) != 8)
    return FALSE;
  for (int i = 0; i < 8; i++)
    if (!g_ascii_isdigit (s[i]))
      return FALSE;
  return TRUE;
}

static gboolean
matches_needle (const YtdlEntry *e, const char *needle_folded)
{
  if (needle_folded == NULL || *needle_folded == '\0')
    return TRUE;

  const char *fields[] = { e->title, e->uploader, e->id, e->channel };
  for (gsize i = 0; i < G_N_ELEMENTS (fields); i++)
    {
      if (fields[i] == NULL)
        continue;
      g_autofree char *folded = g_utf8_casefold (fields[i], -1);
      if (strstr (folded, needle_folded) != NULL)
        return TRUE;
    }
  return FALSE;
}

gboolean
ytdl_library_filter_metadata_matches (const YtdlEntry *entry,
                                      const char *needle)
{
  g_return_val_if_fail (entry != NULL, FALSE);
  if (needle == NULL || *needle == '\0')
    return TRUE;
  g_autofree char *folded = g_utf8_casefold (needle, -1);
  return matches_needle (entry, folded);
}

gboolean
ytdl_library_filter_matches (const YtdlLibraryFilter *filter,
                             const YtdlEntry *entry, YtdlVerifyLookup lookup,
                             gpointer user_data)
{
  g_return_val_if_fail (filter != NULL, FALSE);
  g_return_val_if_fail (entry != NULL, FALSE);

  g_autofree char *folded =
      (filter->needle != NULL && *filter->needle != '\0')
          ? g_utf8_casefold (filter->needle, -1)
          : NULL;
  if (!matches_needle (entry, folded))
    return FALSE;

  /* ANDed with everything else, and checked early because it is one hash
   * lookup and it is the narrowest thing in the filter when it is set at
   * all. */
  if (filter->key_allow != NULL &&
      !g_hash_table_contains (filter->key_allow, entry->key))
    return FALSE;

  /* Empty set means "every channel". The alternative -- empty means none --
   * would make the library go blank the instant someone opened the facet
   * popover and unticked the one channel they had ticked. */
  if (filter->channels->len > 0 &&
      !ytdl_library_filter_has_channel (filter, entry->channel))
    return FALSE;

  if (filter->date_from != NULL || filter->date_to != NULL)
    {
      if (!is_yyyymmdd (entry->upload_date))
        return FALSE;
      if (filter->date_from != NULL &&
          strcmp (entry->upload_date, filter->date_from) < 0)
        return FALSE;
      if (filter->date_to != NULL &&
          strcmp (entry->upload_date, filter->date_to) > 0)
        return FALSE;
    }

  if ((filter->flags & YTDL_FACET_AUDIO_ONLY) &&
      !ytdl_entry_is_audio_only (entry))
    return FALSE;

  if ((filter->flags & YTDL_FACET_NO_MEDIA) &&
      ytdl_entry_media_index (entry) >= 0)
    return FALSE;

  if ((filter->flags & YTDL_FACET_LAYOUT_TOO_NEW) && !entry->layout_too_new)
    return FALSE;

  if ((filter->flags & YTDL_FACET_UNWATCHED) &&
      filter->watched_keys != NULL &&
      g_hash_table_contains (filter->watched_keys, entry->key))
    return FALSE;

  /* Like key_allow: an EMPTY set means "a playlist is selected and it is
   * empty", which must show nothing, while NULL means no playlist filter. */
  if (filter->playlist_keys != NULL &&
      !g_hash_table_contains (filter->playlist_keys, entry->key))
    return FALSE;

  if (filter->flags & YTDL_FACET_VERIFY_FAILED)
    {
      /* UNKNOWN is not a match. A video nobody has verified has not passed
       * and has not failed, and showing it here would turn "these are
       * broken" into "these might be broken", which is a different and much
       * less useful claim. */
      YtdlVerifyState state =
          lookup != NULL ? lookup (entry, user_data) : YTDL_VERIFY_UNKNOWN;
      if (state != YTDL_VERIFY_FAILED)
        return FALSE;
    }

  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Sorting                                                                */
/* ---------------------------------------------------------------------- */

static gboolean
has_text (const char *s)
{
  return s != NULL && *s != '\0';
}

/* Collated, not strcmp'd: "Ärger" belongs next to "Arger" in a list a person
 * reads, and a byte comparison puts it after "Zebra". Callers guarantee both
 * sides are non-empty -- absence is handled before the direction flip, not
 * inside the comparison; see ytdl_library_filter_compare. */
static int
cmp_text (const char *a, const char *b)
{
  gboolean a_empty = !has_text (a);
  gboolean b_empty = !has_text (b);
  if (a_empty || b_empty)
    return a_empty && b_empty ? 0 : (a_empty ? 1 : -1);

  g_autofree char *ka = g_utf8_collate_key_for_filename (a, -1);
  g_autofree char *kb = g_utf8_collate_key_for_filename (b, -1);
  return strcmp (ka, kb);
}

/* The sort field a key reads, as "is it there at all". Kept separate from
 * the comparison because absence must NOT be reversed along with the
 * direction -- that is the whole point; see the note in
 * ytdl_library_filter_compare. */
static gboolean
key_is_present (YtdlSortKey key, const YtdlEntry *e)
{
  switch (key)
    {
    case YTDL_SORT_DATE:
      return has_text (e->upload_date);
    case YTDL_SORT_TITLE:
      return has_text (e->title);
    case YTDL_SORT_CHANNEL:
      return has_text (e->uploader) || has_text (e->channel);
    case YTDL_SORT_DURATION:
      /* No info.json, or one with no duration, reads as 0. That is "not
       * known", not "a zero-second video" -- there is no such thing in an
       * archive. */
      return e->duration > 0;
    case YTDL_SORT_SIZE:
      return ytdl_entry_total_size (e) > 0;
    default:
      return TRUE;
    }
}

static int
cmp_u64 (guint64 a, guint64 b)
{
  return a < b ? -1 : (a > b ? 1 : 0);
}

static int
cmp_double (double a, double b)
{
  return a < b ? -1 : (a > b ? 1 : 0);
}

int
ytdl_library_filter_compare (const YtdlLibraryFilter *filter,
                             const YtdlEntry *a, const YtdlEntry *b)
{
  g_return_val_if_fail (filter != NULL, 0);
  g_return_val_if_fail (a != NULL && b != NULL, 0);

  /* ABSENCE IS DECIDED BEFORE THE DIRECTION FLIP, and this is the single
   * subtlest line in the file. A video with no upload date is missing
   * information, and information that is missing belongs at the BOTTOM of
   * the list in both directions. Folding that into the comparison and then
   * negating the result for a descending sort flips it too, so pressing the
   * sort-direction button fills the first screen with blank cards -- which
   * reads as a rendering bug, not as an ordering choice. Caught by a test
   * rather than by reading this code.
   *
   * Both missing falls through to the tie-breakers below, which are also
   * not reversed. */
  gboolean a_has = key_is_present (filter->sort, a);
  gboolean b_has = key_is_present (filter->sort, b);
  if (a_has != b_has)
    return a_has ? -1 : 1;

  /* Neither side has the key: the tie-breakers below decide, and they are
   * the same ones a genuine tie uses. */
  int r = 0;
  switch (a_has ? filter->sort : YTDL_N_SORT_KEYS)
    {
    case YTDL_SORT_DATE:
      /* The eight-digit string, not the parsed date: it is already
       * lexicographically ordered, and an entry whose date came from the
       * folder-name fallback is in the same form. cmp_text puts a missing
       * date last, which is the same rule as a missing title. */
      r = cmp_text (a->upload_date, b->upload_date);
      break;
    case YTDL_SORT_TITLE:
      r = cmp_text (a->title, b->title);
      break;
    case YTDL_SORT_CHANNEL:
      r = cmp_text (a->uploader != NULL ? a->uploader : a->channel,
                    b->uploader != NULL ? b->uploader : b->channel);
      break;
    case YTDL_SORT_DURATION:
      r = cmp_double (a->duration, b->duration);
      break;
    case YTDL_SORT_SIZE:
      r = cmp_u64 (ytdl_entry_total_size (a), ytdl_entry_total_size (b));
      break;
    default:
      r = 0;
      break;
    }

  if (filter->descending)
    r = -r;

  /* The tie-breakers are NOT reversed with the direction, and that is the
   * point of them: they exist to make the order TOTAL so the grid does not
   * reshuffle equal-keyed videos between rebuilds. Title first because it is
   * what a person would expect to see grouped; then the archive-relative
   * path, which is unique by construction, so the comparison can never
   * return 0 for two different videos. */
  if (r == 0)
    r = cmp_text (a->title, b->title);
  if (r == 0)
    r = g_strcmp0 (a->rel, b->rel);
  return r;
}

typedef struct
{
  const YtdlLibraryFilter *filter;
} SortCtx;

static int
sort_trampoline (gconstpointer pa, gconstpointer pb, gpointer user_data)
{
  const SortCtx *ctx = user_data;
  const YtdlEntry *a = *(const YtdlEntry *const *) pa;
  const YtdlEntry *b = *(const YtdlEntry *const *) pb;
  return ytdl_library_filter_compare (ctx->filter, a, b);
}

GPtrArray *
ytdl_library_filter_apply (const YtdlLibraryFilter *filter,
                           const YtdlIndex *index, YtdlVerifyLookup lookup,
                           gpointer user_data)
{
  g_return_val_if_fail (filter != NULL, NULL);

  /* No free func: every element is borrowed from the index. */
  GPtrArray *out = g_ptr_array_new ();
  if (index == NULL || index->entries == NULL)
    return out;

  for (guint i = 0; i < index->entries->len; i++)
    {
      const YtdlEntry *e = g_ptr_array_index (index->entries, i);
      if (ytdl_library_filter_matches (filter, e, lookup, user_data))
        g_ptr_array_add (out, (gpointer) e);
    }

  SortCtx ctx = { filter };
  g_ptr_array_sort_with_data (out, sort_trampoline, &ctx);
  return out;
}
