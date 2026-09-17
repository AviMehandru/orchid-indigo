/* Library sorting, faceting, and the verification cache.
 *
 * These are the rules three independent apps now have to agree on, so the
 * fixture VALUES here are the ones the Swift and C# suites assert against
 * too -- the same mitigation the probe's derivation uses, and the only thing
 * that makes three blind implementations of one contract survivable.
 *
 * Most of what is asserted below is not "does the filter filter". It is the
 * handful of decisions that are invisible in a screenshot and wrong in a way
 * nobody reports:
 *
 *   - an empty channel set means EVERY channel, not none, or the grid goes
 *     blank the first time someone opens the popover and unticks the one
 *     thing they had ticked;
 *   - a video whose upload_date is not eight digits is EXCLUDED by a date
 *     bound rather than kept, because it has no date and claiming it falls
 *     inside a range invents one;
 *   - the sort is TOTAL, so equal-keyed videos do not swap places between
 *     rebuilds;
 *   - a missing title or date sorts last in BOTH directions, so flipping the
 *     order does not fill the first screen with blanks;
 *   - the "failed verification" facet never shows a video nobody has checked;
 *   - and a cached verification result does not survive the folder being
 *     rewritten underneath it, which is exactly what `ytdl --refresh` does.
 */

#include "archive.h"
#include "library_filter.h"
#include "paths.h"
#include "verify_cache.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct
{
  char      *tmpdir;
  char      *root;
  YtdlIndex *index;
  char      *old_cache;
} Fixture;

static void
mkdirp (const char *path)
{
  g_assert_cmpint (g_mkdir_with_parents (path, 0755), ==, 0);
}

static void
write_file (const char *path, const char *contents)
{
  g_autofree char *dir = g_path_get_dirname (path);
  mkdirp (dir);
  GError *error = NULL;
  g_file_set_contents (path, contents, -1, &error);
  g_assert_no_error (error);
}

/* One video in the documented shape, with just enough manifest to exercise
 * the facets. @media_rel may be NULL for a media-less folder; @bytes is the
 * size of the media file, which is most of what the folder weighs.
 *
 * The FOLDER NAME is built in the documented
 * "<uploader> - <YYYYMMDD> - <id> - <title>" form rather than being a bare
 * slug, and that is not decoration. The reader's folder-name fallback runs
 * before the manifest is applied and wins for every field it can fill, so a
 * fixture with slug folder names would be testing the sort against the slugs
 * and not against the titles and dates it appears to set. An empty @date
 * produces a name that deliberately does NOT parse, which is how a video
 * with no usable upload_date is made. */
static void
make_video (Fixture *fx, const char *channel, const char *name,
            const char *title, const char *date, const char *mode,
            const char *media_rel, gsize bytes, int layout,
            const char *stamp, int refreshes)
{
  /* A REAL-SHAPED video id: exactly 11 base64url characters. The folder-name
   * parse anchors on "8 digits followed by an 11-character id", so a fixture
   * with a short slug for an id does not parse -- the reader then falls back
   * to using the whole folder name as the title and fills the rest from the
   * manifest, and the suite ends up asserting a sort over folder names while
   * appearing to assert one over titles. Padded rather than random so the
   * assertions below can still say "alpha". */
  g_autofree char *id = g_strdup_printf ("%-11s", name);
  g_strdelimit (id, " ", '_');

  g_autofree char *folder =
      (date != NULL && *date != '\0')
          ? g_strdup_printf ("%s - %s - %s - %s", channel, date, id, title)
          : g_strdup_printf ("Undated %s", title);

  g_autofree char *vdir = g_build_filename (fx->root, channel, folder, NULL);
  mkdirp (vdir);

  g_autoptr (GString) history = g_string_new ("");
  if (refreshes > 0)
    {
      g_string_append (history, ",\n  \"refresh_history\": [");
      for (int i = 0; i < refreshes; i++)
        g_string_append_printf (history, "%s{\"time\": \"2026-0%d-01\", "
                                         "\"mode\": \"comments-only\"}",
                                i > 0 ? ", " : "", i + 1);
      g_string_append (history, "]");
    }

  g_autofree char *media_field =
      media_rel != NULL ? g_strdup_printf ("\"%s\"", media_rel)
                        : g_strdup ("null");

  g_autofree char *manifest = g_strdup_printf (
      "{\n"
      "  \"archive_layout_version\": %d,\n"
      "  \"archive_creation_time\": \"%s\",\n"
      "  \"video_id\": \"%s\",\n"
      "  \"title\": \"%s\",\n"
      "  \"uploader\": \"%s\",\n"
      "  \"upload_date\": \"%s\",\n"
      "  \"download_mode\": \"%s\",\n"
      "  \"media_file\": %s%s\n"
      "}\n",
      layout, stamp, id, title, channel, date, mode, media_field,
      history->str);

  g_autofree char *mpath =
      g_build_filename (vdir, "Video metadata", "manifest.json", NULL);
  write_file (mpath, manifest);

  if (media_rel != NULL)
    {
      g_autofree char *mrel = g_strdelimit (g_strdup (media_rel), "/",
                                            G_DIR_SEPARATOR);
      g_autofree char *mfile = g_build_filename (vdir, mrel, NULL);
      g_autofree char *blob = g_malloc0 (bytes + 1);
      memset (blob, 'x', bytes);
      write_file (mfile, blob);
    }
}

/* The shared fixture set. Deliberately small, deliberately awkward:
 *
 *   alpha   a full video, 2024-01-31, 600 bytes, never refreshed
 *   bravo   a full video, 2023-06-15, 200 bytes, same channel as alpha
 *   charlie audio-only, 2025-11-02, 100 bytes, a different channel
 *   delta   media-less (comments-only), 2022-03-08
 *   echo    a full video written with layout 3 -- too new for this reader
 *   foxtrot a full video with NO usable upload_date, to pin the exclusion
 *
 * Titles are deliberately not in date order, so a sort that silently fell
 * back to insertion order would still look plausible and would fail here.
 */
static void
set_up (Fixture *fx, gconstpointer unused)
{
  GError *error = NULL;
  fx->tmpdir = g_dir_make_tmp ("ytdl-gtk-library-XXXXXX", &error);
  g_assert_no_error (error);

  /* The verification cache writes into XDG_CACHE_HOME. Pointed at the
   * sandbox so running the suite can neither read nor clobber the cache of
   * whoever runs it. */
  fx->old_cache = g_strdup (g_getenv ("XDG_CACHE_HOME"));
  g_autofree char *cache = g_build_filename (fx->tmpdir, "cache", NULL);
  mkdirp (cache);
  g_setenv ("XDG_CACHE_HOME", cache, TRUE);

  fx->root = g_build_filename (fx->tmpdir, "Complete Archive", NULL);
  mkdirp (fx->root);

  make_video (fx, "Alpha Channel", "alpha", "Zulu title", "20240131", "full",
              "Final files/Final Video.mkv", 600, 2, "2026-01-01T00:00:00Z", 0);
  make_video (fx, "Alpha Channel", "bravo", "Yankee title", "20230615", "full",
              "Final files/Final Video.mkv", 200, 2, "2026-01-02T00:00:00Z", 0);
  make_video (fx, "Bravo Channel", "charlie", "Xray title", "20251102",
              "audio-only", "Final files/Final Audio.opus", 100, 2,
              "2026-01-03T00:00:00Z", 2);
  make_video (fx, "Bravo Channel", "delta", "Whiskey title", "20220308",
              "comments-only", NULL, 0, 2, "2026-01-04T00:00:00Z", 0);
  make_video (fx, "Bravo Channel", "echo", "Victor title", "20240601", "full",
              "Final files/Final Video.mkv", 50, 3, "2026-01-05T00:00:00Z", 0);
  make_video (fx, "Charlie Channel", "foxtrot", "Uniform title", "", "full",
              "Final files/Final Video.mkv", 10, 2, "2026-01-06T00:00:00Z", 0);

  fx->index = ytdl_index_new ();
  g_assert_true (ytdl_index_scan (fx->index, fx->root, NULL, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (fx->index->entries->len, ==, 6);
}

static void
tear_down (Fixture *fx, gconstpointer unused)
{
  g_clear_pointer (&fx->index, ytdl_index_free);

  if (fx->old_cache != NULL)
    g_setenv ("XDG_CACHE_HOME", fx->old_cache, TRUE);
  else
    g_unsetenv ("XDG_CACHE_HOME");
  g_free (fx->old_cache);

  /* rm -rf, the long way: the sandbox holds nested folders and g_rmdir only
   * removes empty ones. */
  g_autofree char *cmd =
      g_strdup_printf ("rm -rf '%s'", fx->tmpdir);
  int ignored = system (cmd);
  (void) ignored;
  g_free (fx->tmpdir);
  g_free (fx->root);
}

/* The short name a video was made under, recovered from its padded 11-char
 * id -- the shortest readable way to assert an ordering. Returns a static
 * buffer, which is fine for a g_assert_cmpstr argument and nothing else. */
static const char *
short_name (const YtdlEntry *e)
{
  static char buf[16];
  if (e->id == NULL)
    return "(no id)";
  g_strlcpy (buf, e->id, sizeof buf);
  for (char *p = buf + strlen (buf); p > buf && p[-1] == '_'; p--)
    p[-1] = '\0';
  return buf;
}

static const char *
name_at (GPtrArray *got, guint i)
{
  g_assert_cmpuint (i, <, got->len);
  return short_name (g_ptr_array_index (got, i));
}

static gboolean
contains (GPtrArray *got, const char *name)
{
  for (guint i = 0; i < got->len; i++)
    if (g_strcmp0 (short_name (g_ptr_array_index (got, i)), name) == 0)
      return TRUE;
  return FALSE;
}

/* ---------------------------------------------------------------------- */
/* Sorting                                                                */
/* ---------------------------------------------------------------------- */

static void
test_default_is_newest_first (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlLibraryFilter) f = ytdl_library_filter_new ();
  g_assert_cmpint (f->sort, ==, YTDL_SORT_DATE);
  g_assert_true (f->descending);

  g_autoptr (GPtrArray) got =
      ytdl_library_filter_apply (f, fx->index, NULL, NULL);
  g_assert_cmpuint (got->len, ==, 6);

  g_assert_cmpstr (name_at (got, 0), ==, "charlie"); /* 2025-11-02 */
  g_assert_cmpstr (name_at (got, 1), ==, "echo");    /* 2024-06-01 */
  g_assert_cmpstr (name_at (got, 2), ==, "alpha");   /* 2024-01-31 */
  g_assert_cmpstr (name_at (got, 3), ==, "bravo");   /* 2023-06-15 */
  g_assert_cmpstr (name_at (got, 4), ==, "delta");   /* 2022-03-08 */

  /* No date at all, and therefore LAST -- not first, which is where a plain
   * reversed string compare would put an empty string. */
  g_assert_cmpstr (name_at (got, 5), ==, "foxtrot");
}

static void
test_missing_field_sorts_last_both_ways (Fixture *fx, gconstpointer unused)
{
  /* The half of the rule above that is easy to get wrong. If "missing sorts
   * last" were implemented by letting the empty string compare naturally,
   * flipping the direction would move every dateless video to the TOP --
   * which means the first screen after a flip is blanks. */
  g_autoptr (YtdlLibraryFilter) f = ytdl_library_filter_new ();
  f->descending = FALSE;

  g_autoptr (GPtrArray) got =
      ytdl_library_filter_apply (f, fx->index, NULL, NULL);
  g_assert_cmpstr (name_at (got, 0), ==, "delta"); /* oldest real date */
  g_assert_cmpstr (name_at (got, 5), ==, "foxtrot");
}

static void
test_every_key_sorts (Fixture *fx, gconstpointer unused)
{
  struct
  {
    YtdlSortKey key;
    const char *first_descending;
  } cases[] = {
    { YTDL_SORT_TITLE, "alpha" },   /* "Zulu title" */
    { YTDL_SORT_CHANNEL, "foxtrot" }, /* "Charlie Channel" */
    { YTDL_SORT_SIZE, "alpha" },    /* 600 bytes of media */
  };

  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr (YtdlLibraryFilter) f = ytdl_library_filter_new ();
      f->sort = cases[i].key;
      f->descending = TRUE;
      g_autoptr (GPtrArray) got =
          ytdl_library_filter_apply (f, fx->index, NULL, NULL);
      g_assert_cmpstr (name_at (got, 0), ==, cases[i].first_descending);
    }
}

static void
test_sort_is_total (Fixture *fx, gconstpointer unused)
{
  /* Nothing in the fixture has a duration -- there are no info.json files --
   * so every entry ties on the primary key. The comparison must still be a
   * strict order, or the grid reshuffles equal videos between rebuilds and
   * reads as a rendering bug. */
  g_autoptr (YtdlLibraryFilter) f = ytdl_library_filter_new ();
  f->sort = YTDL_SORT_DURATION;

  g_autoptr (GPtrArray) a = ytdl_library_filter_apply (f, fx->index, NULL, NULL);
  g_autoptr (GPtrArray) b = ytdl_library_filter_apply (f, fx->index, NULL, NULL);
  g_assert_cmpuint (a->len, ==, b->len);
  for (guint i = 0; i < a->len; i++)
    g_assert_cmpstr (name_at (a, i), ==, name_at (b, i));

  /* And no pair compares equal, which is the property that guarantees it. */
  for (guint i = 0; i + 1 < a->len; i++)
    {
      const YtdlEntry *x = g_ptr_array_index (a, i);
      const YtdlEntry *y = g_ptr_array_index (a, i + 1);
      g_assert_cmpint (ytdl_library_filter_compare (f, x, y), !=, 0);
    }
}

static void
test_sort_ids_round_trip (Fixture *fx, gconstpointer unused)
{
  /* The setting is persisted by ID, never by the enum's number: inserting a
   * key in the middle would otherwise silently change what every saved
   * setting means. */
  for (int i = 0; i < YTDL_N_SORT_KEYS; i++)
    {
      const char *id = ytdl_sort_key_id ((YtdlSortKey) i);
      g_assert_nonnull (id);
      g_assert_cmpint (ytdl_sort_key_from_id (id), ==, i);
      g_assert_nonnull (ytdl_sort_key_label ((YtdlSortKey) i));
    }
  /* A key written by a newer build falls back rather than refusing. */
  g_assert_cmpint (ytdl_sort_key_from_id ("popularity"), ==, YTDL_SORT_DATE);
  g_assert_cmpint (ytdl_sort_key_from_id (NULL), ==, YTDL_SORT_DATE);
}

/* ---------------------------------------------------------------------- */
/* Facets                                                                 */
/* ---------------------------------------------------------------------- */

static void
test_empty_channel_set_means_all (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlLibraryFilter) f = ytdl_library_filter_new ();
  g_assert_cmpuint (f->channels->len, ==, 0);

  g_autoptr (GPtrArray) got =
      ytdl_library_filter_apply (f, fx->index, NULL, NULL);
  g_assert_cmpuint (got->len, ==, 6);
  g_assert_false (ytdl_library_filter_is_narrowing (f));
}

static void
test_channel_facet_is_a_union (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlLibraryFilter) f = ytdl_library_filter_new ();
  ytdl_library_filter_set_channel (f, "Alpha Channel", TRUE);

  g_autoptr (GPtrArray) one =
      ytdl_library_filter_apply (f, fx->index, NULL, NULL);
  g_assert_cmpuint (one->len, ==, 2);

  /* Two channels is MORE videos, not fewer: within one facet the selections
   * are an OR. Between facets they are an AND. Getting that backwards makes
   * every multi-select facet show nothing. */
  ytdl_library_filter_set_channel (f, "Bravo Channel", TRUE);
  g_autoptr (GPtrArray) two =
      ytdl_library_filter_apply (f, fx->index, NULL, NULL);
  g_assert_cmpuint (two->len, ==, 5);

  /* Ticking twice must not double the entry, or unticking once would leave
   * it selected. */
  ytdl_library_filter_set_channel (f, "Bravo Channel", TRUE);
  g_assert_cmpuint (f->channels->len, ==, 2);
  ytdl_library_filter_set_channel (f, "Bravo Channel", FALSE);
  g_assert_false (ytdl_library_filter_has_channel (f, "Bravo Channel"));
}

static void
test_date_range_excludes_undated (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlLibraryFilter) f = ytdl_library_filter_new ();
  f->date_from = g_strdup ("20230101");
  f->date_to = g_strdup ("20241231");

  g_autoptr (GPtrArray) got =
      ytdl_library_filter_apply (f, fx->index, NULL, NULL);
  g_assert_cmpuint (got->len, ==, 3); /* alpha, bravo, echo */
  g_assert_true (contains (got, "alpha"));
  g_assert_true (contains (got, "bravo"));
  g_assert_true (contains (got, "echo"));

  /* The one that matters. foxtrot has no usable date, so it is not inside
   * this range -- it is a video whose date is unknown, and keeping it would
   * be inventing one. */
  g_assert_false (contains (got, "foxtrot"));

  /* Bounds are inclusive at both ends. */
  g_free (f->date_from);
  g_free (f->date_to);
  f->date_from = g_strdup ("20240131");
  f->date_to = g_strdup ("20240131");
  g_autoptr (GPtrArray) exact =
      ytdl_library_filter_apply (f, fx->index, NULL, NULL);
  g_assert_cmpuint (exact->len, ==, 1);
  g_assert_cmpstr (name_at (exact, 0), ==, "alpha");
}

static void
test_flag_facets (Fixture *fx, gconstpointer unused)
{
  struct
  {
    YtdlFacetFlags flag;
    const char    *expect;
  } cases[] = {
    { YTDL_FACET_AUDIO_ONLY, "charlie" },
    { YTDL_FACET_NO_MEDIA, "delta" },
    { YTDL_FACET_LAYOUT_TOO_NEW, "echo" },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr (YtdlLibraryFilter) f = ytdl_library_filter_new ();
      f->flags = cases[i].flag;
      g_autoptr (GPtrArray) got =
          ytdl_library_filter_apply (f, fx->index, NULL, NULL);
      g_assert_cmpuint (got->len, ==, 1);
      g_assert_cmpstr (name_at (got, 0), ==, cases[i].expect);
    }
}

static void
test_audio_only_is_not_media_less (Fixture *fx, gconstpointer unused)
{
  /* Two different states that a reader keying off "no Final Video.mkv" would
   * collapse into one. charlie has media; delta does not. */
  g_autoptr (YtdlLibraryFilter) f = ytdl_library_filter_new ();
  f->flags = YTDL_FACET_AUDIO_ONLY | YTDL_FACET_NO_MEDIA;

  g_autoptr (GPtrArray) got =
      ytdl_library_filter_apply (f, fx->index, NULL, NULL);
  g_assert_cmpuint (got->len, ==, 0);
}

static void
test_facet_count_and_reset (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlLibraryFilter) f = ytdl_library_filter_new ();
  f->sort = YTDL_SORT_SIZE;
  f->descending = FALSE;

  ytdl_library_filter_set_channel (f, "Alpha Channel", TRUE);
  f->date_from = g_strdup ("20230101");
  f->date_to = g_strdup ("20241231");
  f->flags = YTDL_FACET_AUDIO_ONLY;
  f->needle = g_strdup ("zulu");

  /* A date RANGE is one facet, not two: it is a single idea the user had,
   * and counting it twice makes the badge overstate the narrowing. */
  g_assert_cmpuint (ytdl_library_filter_facet_count (f), ==, 3);
  g_assert_true (ytdl_library_filter_is_narrowing (f));

  ytdl_library_filter_reset (f);
  g_assert_cmpuint (ytdl_library_filter_facet_count (f), ==, 0);
  g_assert_false (ytdl_library_filter_is_narrowing (f));

  /* The sort survives a reset on purpose: it is a view preference, not a
   * filter, and throwing it away would be a surprise. */
  g_assert_cmpint (f->sort, ==, YTDL_SORT_SIZE);
  g_assert_false (f->descending);
}

static void
test_needle_still_matches_four_fields (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlLibraryFilter) f = ytdl_library_filter_new ();
  f->needle = g_strdup ("BRAVO CHANNEL");

  /* Case-folded, and matching the uploader as well as the title -- the
   * behaviour that was already there and must not regress. */
  g_autoptr (GPtrArray) got =
      ytdl_library_filter_apply (f, fx->index, NULL, NULL);
  g_assert_cmpuint (got->len, ==, 3);
}

/* ---------------------------------------------------------------------- */
/* Verification facet and cache                                           */
/* ---------------------------------------------------------------------- */

static void
test_verify_facet_ignores_unchecked (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlVerifyCache) cache = ytdl_verify_cache_load ();
  g_assert_cmpuint (ytdl_verify_cache_known (cache), ==, 0);

  g_autoptr (YtdlLibraryFilter) f = ytdl_library_filter_new ();
  f->flags = YTDL_FACET_VERIFY_FAILED;

  /* Nothing has been verified, so the facet shows nothing. It must NOT show
   * everything: "these failed" and "these might have failed" are different
   * claims and only one of them is true here. */
  g_autoptr (GPtrArray) none = ytdl_library_filter_apply (
      f, fx->index, ytdl_verify_cache_lookup, cache);
  g_assert_cmpuint (none->len, ==, 0);

  const YtdlEntry *alpha = NULL, *bravo = NULL;
  for (guint i = 0; i < fx->index->entries->len; i++)
    {
      const YtdlEntry *e = g_ptr_array_index (fx->index->entries, i);
      if (g_strcmp0 (short_name (e), "alpha") == 0)
        alpha = e;
      if (g_strcmp0 (short_name (e), "bravo") == 0)
        bravo = e;
    }
  g_assert_nonnull (alpha);
  g_assert_nonnull (bravo);

  ytdl_verify_cache_set (cache, alpha, YTDL_VERIFY_FAILED);
  ytdl_verify_cache_set (cache, bravo, YTDL_VERIFY_OK);

  g_autoptr (GPtrArray) failed = ytdl_library_filter_apply (
      f, fx->index, ytdl_verify_cache_lookup, cache);
  g_assert_cmpuint (failed->len, ==, 1);
  g_assert_cmpstr (name_at (failed, 0), ==, "alpha");
  g_assert_cmpuint (ytdl_verify_cache_known (cache), ==, 2);
}

static void
test_verify_result_does_not_survive_a_refresh (Fixture *fx, gconstpointer unused)
{
  /* The reason every record carries the manifest's archive_creation_time.
   * `ytdl --refresh` rewrites a folder's sidecars, its hashes and -- when it
   * re-embeds the info.json -- the media file itself, all without changing
   * download_mode. A cached "verifies" from before that is not a stale
   * opinion, it is a wrong one. */
  g_autoptr (YtdlVerifyCache) cache = ytdl_verify_cache_load ();

  const YtdlEntry *alpha = NULL;
  for (guint i = 0; i < fx->index->entries->len; i++)
    {
      const YtdlEntry *e = g_ptr_array_index (fx->index->entries, i);
      if (g_strcmp0 (short_name (e), "alpha") == 0)
        alpha = e;
    }
  g_assert_nonnull (alpha);
  g_assert_cmpstr (alpha->creation_stamp, ==, "2026-01-01T00:00:00Z");

  ytdl_verify_cache_set (cache, alpha, YTDL_VERIFY_OK);
  g_assert_cmpint (ytdl_verify_cache_get (cache, alpha), ==, YTDL_VERIFY_OK);

  /* Rewrite the folder the way a refresh would: a new creation stamp and a
   * refresh_history, with download_mode and media_file preserved. */
  make_video (fx, "Alpha Channel", "alpha", "Zulu title", "20240131", "full",
              "Final files/Final Video.mkv", 600, 2, "2026-05-05T00:00:00Z", 1);

  YtdlIndex *after = ytdl_index_new ();
  GError *error = NULL;
  g_assert_true (ytdl_index_scan (after, fx->root, NULL, NULL, &error));
  g_assert_no_error (error);

  const YtdlEntry *again = NULL;
  for (guint i = 0; i < after->entries->len; i++)
    {
      const YtdlEntry *e = g_ptr_array_index (after->entries, i);
      if (g_strcmp0 (short_name (e), "alpha") == 0)
        again = e;
    }
  g_assert_nonnull (again);
  g_assert_cmpuint (again->refresh_count, ==, 1);
  g_assert_cmpstr (again->download_mode, ==, "full");

  /* Same key, same video, different folder -- so the old result is gone. */
  g_assert_cmpstr (again->key, ==, alpha->key);
  g_assert_cmpint (ytdl_verify_cache_get (cache, again), ==,
                   YTDL_VERIFY_UNKNOWN);

  ytdl_index_free (after);
}

static void
test_verify_cache_round_trips (Fixture *fx, gconstpointer unused)
{
  const YtdlEntry *alpha = g_ptr_array_index (fx->index->entries, 0);

  {
    g_autoptr (YtdlVerifyCache) cache = ytdl_verify_cache_load ();
    ytdl_verify_cache_set (cache, alpha, YTDL_VERIFY_FAILED);
    ytdl_verify_cache_save (cache);
  }

  g_autoptr (YtdlVerifyCache) reloaded = ytdl_verify_cache_load ();
  g_assert_cmpint (ytdl_verify_cache_get (reloaded, alpha), ==,
                   YTDL_VERIFY_FAILED);
}

static void
test_corrupt_cache_is_empty_not_fatal (Fixture *fx, gconstpointer unused)
{
  g_autofree char *dir = ytdl_cache_dir ();
  g_autofree char *path = g_build_filename (dir, "verify.json", NULL);
  write_file (path, "{ this is not json");

  /* A cache is worth one re-verify. There is no version of "refuse to open
   * the Library because a cache file is malformed" that is the right call. */
  g_autoptr (YtdlVerifyCache) cache = ytdl_verify_cache_load ();
  g_assert_nonnull (cache);
  g_assert_cmpuint (ytdl_verify_cache_known (cache), ==, 0);
}

/* ---------------------------------------------------------------------- */

static void
test_total_size_counts_the_folder (Fixture *fx, gconstpointer unused)
{
  /* The whole folder, not just the media file: that is what it costs on the
   * disk it is sitting on, which is the question someone sorting by size is
   * asking. So it is strictly greater than the media alone -- the manifest
   * is in there too. */
  for (guint i = 0; i < fx->index->entries->len; i++)
    {
      const YtdlEntry *e = g_ptr_array_index (fx->index->entries, i);
      if (g_strcmp0 (short_name (e), "alpha") != 0)
        continue;
      g_assert_cmpuint (ytdl_entry_total_size (e), >, 600);
      return;
    }
  g_assert_not_reached ();
}

void ytdl_register_library_tests (void);

void
ytdl_register_library_tests (void)
{
#define FIX(path, fn) g_test_add (path, Fixture, NULL, set_up, fn, tear_down)

  FIX ("/library/default-newest-first", test_default_is_newest_first);
  FIX ("/library/missing-sorts-last-both-ways",
       test_missing_field_sorts_last_both_ways);
  FIX ("/library/every-key-sorts", test_every_key_sorts);
  FIX ("/library/sort-is-total", test_sort_is_total);
  FIX ("/library/sort-ids-round-trip", test_sort_ids_round_trip);

  FIX ("/library/empty-channel-set-means-all", test_empty_channel_set_means_all);
  FIX ("/library/channel-facet-is-a-union", test_channel_facet_is_a_union);
  FIX ("/library/date-range-excludes-undated", test_date_range_excludes_undated);
  FIX ("/library/flag-facets", test_flag_facets);
  FIX ("/library/audio-only-is-not-media-less", test_audio_only_is_not_media_less);
  FIX ("/library/facet-count-and-reset", test_facet_count_and_reset);
  FIX ("/library/needle-matches-four-fields",
       test_needle_still_matches_four_fields);

  FIX ("/library/verify-facet-ignores-unchecked",
       test_verify_facet_ignores_unchecked);
  FIX ("/library/verify-does-not-survive-refresh",
       test_verify_result_does_not_survive_a_refresh);
  FIX ("/library/verify-cache-round-trips", test_verify_cache_round_trips);
  FIX ("/library/corrupt-cache-is-empty", test_corrupt_cache_is_empty_not_fatal);

  FIX ("/library/total-size-counts-folder", test_total_size_counts_the_folder);

#undef FIX
}
