/* Collection-wide comment and transcript search.
 *
 * The tokenizer is the contract, and it is the one function here whose
 * disagreement between the three apps would be INVISIBLE: the same query would
 * quietly return different videos on different platforms, with nothing in any
 * UI saying why. So the strings below are the same ones the Swift and C#
 * suites tokenize, and the expected token lists are written from the rule --
 * "case-folded runs of letters and digits, everything else a separator" --
 * rather than from what any one implementation happens to do.
 *
 * The other half is the matching semantics, which are easy to get subtly
 * wrong in ways that only show up as "search feels broken":
 *
 *   - ALL query tokens must match, in any order, anywhere in the field. A
 *     search that OR-ed them would return most of the archive for two common
 *     words.
 *   - A query token matches a stored token by PREFIX, pinned to a token
 *     boundary. "rail" finds "railway" and must not find "guardrail" --
 *     substring matching without the boundary is the single most likely
 *     implementation, and it makes short queries useless.
 *   - The index is rebuilt only for videos whose stamp changed, which after
 *     `ytdl --refresh` is exactly the videos whose comments are new.
 */

#include "archive.h"
#include "paths.h"
#include "search_index.h"

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

/* A video with a real info.json carrying a comment tree, and a .vtt.
 *
 * The comment shapes are yt-dlp's own: a FLAT array with a `parent` field that
 * is either "root" or the parent's id, deliberately out of order, because that
 * is what the reader has to cope with and an index built on a tidier fixture
 * would not prove it does. */
static void
make_video (Fixture *fx, const char *channel, const char *name,
            const char *title, const char *date, const char *stamp,
            const char *comment_a, const char *comment_b, const char *vtt)
{
  g_autofree char *id = g_strdup_printf ("%-11s", name);
  g_strdelimit (id, " ", '_');

  g_autofree char *folder =
      g_strdup_printf ("%s - %s - %s - %s", channel, date, id, title);
  g_autofree char *vdir = g_build_filename (fx->root, channel, folder, NULL);
  mkdirp (vdir);

  g_autofree char *manifest = g_strdup_printf (
      "{\n"
      "  \"archive_layout_version\": 2,\n"
      "  \"archive_creation_time\": \"%s\",\n"
      "  \"video_id\": \"%s\",\n"
      "  \"title\": \"%s\",\n"
      "  \"uploader\": \"%s\",\n"
      "  \"upload_date\": \"%s\",\n"
      "  \"download_mode\": \"full\",\n"
      "  \"media_file\": \"Final files/Final Video.mkv\"\n"
      "}\n",
      stamp, id, title, channel, date);
  g_autofree char *mpath =
      g_build_filename (vdir, "Video metadata", "manifest.json", NULL);
  write_file (mpath, manifest);

  g_autofree char *info = g_strdup_printf (
      "{\n"
      "  \"id\": \"%s\",\n"
      "  \"title\": \"%s\",\n"
      "  \"description\": \"A description mentioning aqueducts.\",\n"
      "  \"comments\": [\n"
      "    {\"id\": \"c2\", \"parent\": \"c1\", \"text\": \"%s\","
      " \"author\": \"Replier\"},\n"
      "    {\"id\": \"c1\", \"parent\": \"root\", \"text\": \"%s\","
      " \"author\": \"Asker\"}\n"
      "  ]\n"
      "}\n",
      id, title, comment_b, comment_a);
  g_autofree char *ipath =
      g_build_filename (vdir, "Video metadata", "Video.info.json", NULL);
  write_file (ipath, info);

  g_autofree char *media =
      g_build_filename (vdir, "Final files", "Final Video.mkv", NULL);
  write_file (media, "x");

  if (vtt != NULL)
    {
      g_autofree char *spath =
          g_build_filename (vdir, "Subtitles", "Subtitles.en.vtt", NULL);
      write_file (spath, vtt);
    }
}

/* Deliberately NOT auto-generated: no karaoke tags, no cue settings. The
 * rolling-duplication collapse is tested elsewhere and would only obscure
 * what this file is about. */
static const char *const VTT_BRIDGE =
    "WEBVTT\n"
    "\n"
    "00:00:01.000 --> 00:00:04.000\n"
    "this railway bridge swings sideways\n"
    "\n"
    "00:00:04.000 --> 00:00:08.000\n"
    "to let the canal boats through\n";

static const char *const VTT_ICE =
    "WEBVTT\n"
    "\n"
    "00:00:02.000 --> 00:00:06.000\n"
    "ice behaves strangely under pressure\n";

static void
set_up (Fixture *fx, gconstpointer unused)
{
  GError *error = NULL;
  fx->tmpdir = g_dir_make_tmp ("ytdl-gtk-search-XXXXXX", &error);
  g_assert_no_error (error);

  fx->old_cache = g_strdup (g_getenv ("XDG_CACHE_HOME"));
  g_autofree char *cache = g_build_filename (fx->tmpdir, "cache", NULL);
  mkdirp (cache);
  g_setenv ("XDG_CACHE_HOME", cache, TRUE);

  fx->root = g_build_filename (fx->tmpdir, "Complete Archive", NULL);
  mkdirp (fx->root);

  make_video (fx, "Tom Scott", "bridge", "The Moving Bridge", "20240131",
              "2026-01-01T00:00:00Z",
              "How does the guardrail stay on when it swings?",
              "The counterweight does most of the work.", VTT_BRIDGE);

  make_video (fx, "Veritasium", "ice", "Strange Ice", "20230615",
              "2026-01-02T00:00:00Z",
              "Regelation is the word you are looking for.",
              "I was taught this was pressure melting.", VTT_ICE);

  fx->index = ytdl_index_new ();
  g_assert_true (ytdl_index_scan (fx->index, fx->root, NULL, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (fx->index->entries->len, ==, 2);
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

  g_autofree char *cmd = g_strdup_printf ("rm -rf '%s'", fx->tmpdir);
  int ignored = system (cmd);
  (void) ignored;
  g_free (fx->tmpdir);
  g_free (fx->root);
}

static const YtdlEntry *
entry_named (Fixture *fx, const char *name)
{
  for (guint i = 0; i < fx->index->entries->len; i++)
    {
      const YtdlEntry *e = g_ptr_array_index (fx->index->entries, i);
      if (e->id != NULL && g_str_has_prefix (e->id, name))
        return e;
    }
  return NULL;
}

/* ---------------------------------------------------------------------- */
/* The tokenizer                                                          */
/* ---------------------------------------------------------------------- */

static void
check_tokens (const char *input, const char *const *expected)
{
  g_autoptr (GPtrArray) got = ytdl_search_tokenize (input);
  gsize n = 0;
  while (expected[n] != NULL)
    n++;

  g_assert_cmpuint (got->len, ==, n);
  for (gsize i = 0; i < n; i++)
    g_assert_cmpstr (g_ptr_array_index (got, i), ==, expected[i]);
}

static void
test_tokenizer_rule (Fixture *fx, gconstpointer unused)
{
  /* Case-folded runs of letters and digits; everything else separates. */
  check_tokens ("Hello, World!",
                (const char *const[]){ "hello", "world", NULL });

  /* Digits are letters as far as this is concerned: "1080p" and "av01" are
   * words someone will search for. */
  check_tokens ("1080p AV01 x264",
                (const char *const[]){ "1080p", "av01", "x264", NULL });

  /* The apostrophe is a SEPARATOR, so "don't" is two tokens. That is a
   * choice rather than an oversight, and the point is that all three apps
   * make the same one -- a query for "dont" finds neither, and a query for
   * "don" finds both. */
  check_tokens ("don't", (const char *const[]){ "don", "t", NULL });

  /* Punctuation, hyphens and newlines all separate, and runs of them do not
   * produce empty tokens. */
  check_tokens ("well---known  \n stuff.",
                (const char *const[]){ "well", "known", "stuff", NULL });

  /* Non-ASCII letters are letters, and case-folding is not ASCII-only. */
  check_tokens ("Größe ÉCOLE",
                (const char *const[]){ "größe", "école", NULL });

  check_tokens ("", (const char *const[]){ NULL });
  check_tokens ("   ...   ", (const char *const[]){ NULL });
}

static void
test_tokenizer_survives_bad_utf8 (Fixture *fx, gconstpointer unused)
{
  /* Comment text is somebody else's input, written to disk by yt-dlp and read
   * back here. An invalid sequence must not walk this function off the end of
   * the string, which is what g_utf8_next_char does when it is handed one. */
  const char bad[] = { 'o', 'k', ' ', (char) 0xC3, (char) 0x28, 'x', '\0' };
  g_autoptr (GPtrArray) got = ytdl_search_tokenize (bad);
  g_assert_cmpuint (got->len, >=, 1);
  g_assert_cmpstr (g_ptr_array_index (got, 0), ==, "ok");
}

/* ---------------------------------------------------------------------- */
/* Query semantics                                                        */
/* ---------------------------------------------------------------------- */

static void
test_all_tokens_must_match (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlSearchIndex) ix = ytdl_search_index_load ();
  ytdl_search_index_build (ix, fx->index, NULL, NULL, NULL);
  g_assert_cmpuint (ytdl_search_index_size (ix), ==, 2);

  const YtdlEntry *bridge = entry_named (fx, "bridge");
  const YtdlEntry *ice = entry_named (fx, "ice");
  g_assert_nonnull (bridge);
  g_assert_nonnull (ice);

  /* Both words are in the bridge video's comments; only one is in the ice
   * video's. An OR would return both and would return most of an archive for
   * any two common words. */
  {
    g_autoptr (GHashTable) hits =
        ytdl_search_index_query (ix, "counterweight work", YTDL_SEARCH_COMMENTS);
    g_assert_cmpuint (g_hash_table_size (hits), ==, 1);
    g_assert_true (g_hash_table_contains (hits, bridge->key));
  }

  /* Order does not matter. */
  {
    g_autoptr (GHashTable) hits =
        ytdl_search_index_query (ix, "work counterweight", YTDL_SEARCH_COMMENTS);
    g_assert_cmpuint (g_hash_table_size (hits), ==, 1);
  }

  /* A token that appears in neither excludes everything, even alongside one
   * that appears in both. */
  {
    g_autoptr (GHashTable) hits =
        ytdl_search_index_query (ix, "the zeppelin", YTDL_SEARCH_COMMENTS);
    g_assert_cmpuint (g_hash_table_size (hits), ==, 0);
  }
}

static void
test_prefix_matching_respects_token_boundaries (Fixture *fx,
                                                gconstpointer unused)
{
  g_autoptr (YtdlSearchIndex) ix = ytdl_search_index_load ();
  ytdl_search_index_build (ix, fx->index, NULL, NULL, NULL);

  const YtdlEntry *bridge = entry_named (fx, "bridge");

  /* "rail" is a prefix of "railway", which is in the bridge transcript. */
  {
    g_autoptr (GHashTable) hits =
        ytdl_search_index_query (ix, "rail", YTDL_SEARCH_TRANSCRIPT);
    g_assert_cmpuint (g_hash_table_size (hits), ==, 1);
    g_assert_true (g_hash_table_contains (hits, bridge->key));
  }

  /* THE ONE THAT MATTERS. "rail" must NOT match "guardrail", which is in the
   * bridge video's COMMENTS. A plain substring search over the token blob --
   * the most likely implementation -- would match it, and that is what makes
   * short queries useless. Searching the comments for "rail" must therefore
   * find nothing, because "guardrail" is the only rail-ish token there. */
  {
    g_autoptr (GHashTable) hits =
        ytdl_search_index_query (ix, "rail", YTDL_SEARCH_COMMENTS);
    g_assert_cmpuint (g_hash_table_size (hits), ==, 0);
  }

  /* And the whole token still matches itself. */
  {
    g_autoptr (GHashTable) hits =
        ytdl_search_index_query (ix, "guardrail", YTDL_SEARCH_COMMENTS);
    g_assert_cmpuint (g_hash_table_size (hits), ==, 1);
  }
}

static void
test_scopes_are_separate (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlSearchIndex) ix = ytdl_search_index_load ();
  ytdl_search_index_build (ix, fx->index, NULL, NULL, NULL);

  /* "sideways" is in the transcript and not in the comments. */
  g_autoptr (GHashTable) in_transcript =
      ytdl_search_index_query (ix, "sideways", YTDL_SEARCH_TRANSCRIPT);
  g_assert_cmpuint (g_hash_table_size (in_transcript), ==, 1);

  g_autoptr (GHashTable) in_comments =
      ytdl_search_index_query (ix, "sideways", YTDL_SEARCH_COMMENTS);
  g_assert_cmpuint (g_hash_table_size (in_comments), ==, 0);

  g_autoptr (GHashTable) in_either =
      ytdl_search_index_query (ix, "sideways", YTDL_SEARCH_EVERYTHING);
  g_assert_cmpuint (g_hash_table_size (in_either), ==, 1);

  /* The DESCRIPTION is indexed with the comments rather than given a scope of
   * its own -- it is the uploader's own words about the video, which is what
   * someone searching "comments" is reaching for. */
  g_autoptr (GHashTable) description =
      ytdl_search_index_query (ix, "aqueducts", YTDL_SEARCH_COMMENTS);
  g_assert_cmpuint (g_hash_table_size (description), ==, 2);
}

static void
test_empty_query_matches_nothing (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlSearchIndex) ix = ytdl_search_index_load ();
  ytdl_search_index_build (ix, fx->index, NULL, NULL, NULL);

  /* "Match everything" is the caller's decision to make, not this function's
   * to guess. The Library keeps showing the whole archive when the search box
   * is empty because IT decides that, not because the index said so. */
  g_autoptr (GHashTable) hits =
      ytdl_search_index_query (ix, "   ", YTDL_SEARCH_EVERYTHING);
  g_assert_cmpuint (g_hash_table_size (hits), ==, 0);
}

/* ---------------------------------------------------------------------- */
/* Freshness                                                              */
/* ---------------------------------------------------------------------- */

static void
test_only_changed_videos_are_reparsed (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlSearchIndex) ix = ytdl_search_index_load ();

  g_assert_cmpuint (ytdl_search_index_outdated (ix, fx->index), ==, 2);
  ytdl_search_index_build (ix, fx->index, NULL, NULL, NULL);
  g_assert_cmpuint (ytdl_search_index_outdated (ix, fx->index), ==, 0);

  /* Rewrite ONE video the way `ytdl --refresh` would: a new creation stamp
   * and new comments, everything else the same. Only that video is stale. */
  make_video (fx, "Tom Scott", "bridge", "The Moving Bridge", "20240131",
              "2026-05-05T00:00:00Z",
              "Actually the swing is hydraulic these days.",
              "Someone said zeppelin and I cannot unsee it.", VTT_BRIDGE);

  YtdlIndex *after = ytdl_index_new ();
  GError *error = NULL;
  g_assert_true (ytdl_index_scan (after, fx->root, NULL, NULL, &error));
  g_assert_no_error (error);

  g_assert_cmpuint (ytdl_search_index_outdated (ix, after), ==, 1);

  ytdl_search_index_build (ix, after, NULL, NULL, NULL);
  g_assert_cmpuint (ytdl_search_index_outdated (ix, after), ==, 0);

  /* And the new comment text is what is searchable now. */
  g_autoptr (GHashTable) fresh =
      ytdl_search_index_query (ix, "zeppelin", YTDL_SEARCH_COMMENTS);
  g_assert_cmpuint (g_hash_table_size (fresh), ==, 1);

  g_autoptr (GHashTable) gone =
      ytdl_search_index_query (ix, "counterweight", YTDL_SEARCH_COMMENTS);
  g_assert_cmpuint (g_hash_table_size (gone), ==, 0);

  ytdl_index_free (after);
}

static void
test_index_round_trips_and_drops_removed (Fixture *fx, gconstpointer unused)
{
  {
    g_autoptr (YtdlSearchIndex) ix = ytdl_search_index_load ();
    ytdl_search_index_build (ix, fx->index, NULL, NULL, NULL);
    ytdl_search_index_save (ix);
  }

  g_autoptr (YtdlSearchIndex) reloaded = ytdl_search_index_load ();
  g_assert_cmpuint (ytdl_search_index_size (reloaded), ==, 2);
  g_assert_cmpuint (ytdl_search_index_outdated (reloaded, fx->index), ==, 0);

  /* A video that has left the archive leaves the index too, or the store
   * grows forever across rescans of a tree somebody reorganises. */
  g_autofree char *gone =
      g_build_filename (fx->root, "Veritasium", NULL);
  g_autofree char *cmd = g_strdup_printf ("rm -rf '%s'", gone);
  int ignored = system (cmd);
  (void) ignored;

  YtdlIndex *after = ytdl_index_new ();
  GError *error = NULL;
  g_assert_true (ytdl_index_scan (after, fx->root, NULL, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (after->entries->len, ==, 1);

  ytdl_search_index_build (reloaded, after, NULL, NULL, NULL);
  g_assert_cmpuint (ytdl_search_index_size (reloaded), ==, 1);

  ytdl_index_free (after);
}

static void
test_corrupt_index_is_empty_not_fatal (Fixture *fx, gconstpointer unused)
{
  g_autofree char *dir = ytdl_cache_dir ();
  g_autofree char *path = g_build_filename (dir, "search-index.json", NULL);
  write_file (path, "{ not json at all");

  g_autoptr (YtdlSearchIndex) ix = ytdl_search_index_load ();
  g_assert_nonnull (ix);
  g_assert_cmpuint (ytdl_search_index_size (ix), ==, 0);
}

static void
test_version_mismatch_is_discarded (Fixture *fx, gconstpointer unused)
{
  /* A store written by a different version of this format is thrown away
   * rather than misread. Without the check, a later change to what a token is
   * would leave every existing user with an index that silently answers the
   * old way. */
  g_autofree char *dir = ytdl_cache_dir ();
  g_autofree char *path = g_build_filename (dir, "search-index.json", NULL);
  write_file (path,
              "{\"version\": 99, \"videos\": {\"abc\": {\"stamp\": \"x\","
              " \"c\": \" hello \"}}}");

  g_autoptr (YtdlSearchIndex) ix = ytdl_search_index_load ();
  g_assert_cmpuint (ytdl_search_index_size (ix), ==, 0);
}

/* ---------------------------------------------------------------------- */
/* Snippets                                                               */
/* ---------------------------------------------------------------------- */

static void
test_snippets_come_from_the_real_text (Fixture *fx, gconstpointer unused)
{
  const YtdlEntry *bridge = entry_named (fx, "bridge");
  g_assert_nonnull (bridge);

  /* The index holds no text at all, so a snippet is proof that the matched
   * video's own files were re-read. */
  g_autoptr (GPtrArray) comments =
      ytdl_search_snippets (bridge, "counterweight", YTDL_SEARCH_COMMENTS, 5);
  g_assert_cmpuint (comments->len, ==, 1);
  const YtdlSnippet *c = g_ptr_array_index (comments, 0);
  g_assert_nonnull (strstr (c->text, "counterweight"));
  g_assert_cmpstr (c->who, ==, "Replier");
  g_assert_false (c->from_transcript);

  /* A transcript hit spans cues: "railway bridge swings" and "canal boats"
   * are different cues, and a matcher that worked per cue would find the
   * phrase in neither. The passage carries a timestamp instead of an
   * author. */
  g_autoptr (GPtrArray) cues =
      ytdl_search_snippets (bridge, "railway canal", YTDL_SEARCH_TRANSCRIPT, 5);
  g_assert_cmpuint (cues->len, ==, 1);
  const YtdlSnippet *t = g_ptr_array_index (cues, 0);
  g_assert_true (t->from_transcript);
  g_assert_cmpstr (t->who, ==, "0:01");

  /* A query that matches the index but not any single passage returns no
   * snippets rather than a wrong one. */
  g_autoptr (GPtrArray) none =
      ytdl_search_snippets (bridge, "zeppelin", YTDL_SEARCH_EVERYTHING, 5);
  g_assert_cmpuint (none->len, ==, 0);
}

static void
test_scope_ids_round_trip (Fixture *fx, gconstpointer unused)
{
  for (int i = 0; i < YTDL_N_SEARCH_SCOPES; i++)
    {
      const char *id = ytdl_search_scope_id ((YtdlSearchScope) i);
      g_assert_nonnull (id);
      g_assert_cmpint (ytdl_search_scope_from_id (id), ==, i);
      g_assert_nonnull (ytdl_search_scope_label ((YtdlSearchScope) i));
    }
  g_assert_cmpint (ytdl_search_scope_from_id ("lyrics"), ==,
                   YTDL_SEARCH_METADATA);

  /* Only the metadata scope can be answered without the index, and the UI
   * keys its "not built yet" message off exactly this. */
  g_assert_false (ytdl_search_scope_needs_index (YTDL_SEARCH_METADATA));
  g_assert_true (ytdl_search_scope_needs_index (YTDL_SEARCH_COMMENTS));
  g_assert_true (ytdl_search_scope_needs_index (YTDL_SEARCH_TRANSCRIPT));
  g_assert_true (ytdl_search_scope_needs_index (YTDL_SEARCH_EVERYTHING));
}

void ytdl_register_search_tests (void);

void
ytdl_register_search_tests (void)
{
#define FIX(path, fn) g_test_add (path, Fixture, NULL, set_up, fn, tear_down)

  FIX ("/search/tokenizer-rule", test_tokenizer_rule);
  FIX ("/search/tokenizer-bad-utf8", test_tokenizer_survives_bad_utf8);
  FIX ("/search/all-tokens-must-match", test_all_tokens_must_match);
  FIX ("/search/prefix-respects-boundaries",
       test_prefix_matching_respects_token_boundaries);
  FIX ("/search/scopes-are-separate", test_scopes_are_separate);
  FIX ("/search/empty-query-matches-nothing", test_empty_query_matches_nothing);
  FIX ("/search/only-changed-reparsed", test_only_changed_videos_are_reparsed);
  FIX ("/search/round-trips-and-drops", test_index_round_trips_and_drops_removed);
  FIX ("/search/corrupt-is-empty", test_corrupt_index_is_empty_not_fatal);
  FIX ("/search/version-mismatch-discarded", test_version_mismatch_is_discarded);
  FIX ("/search/snippets-from-real-text", test_snippets_come_from_the_real_text);
  FIX ("/search/scope-ids-round-trip", test_scope_ids_round_trip);

#undef FIX
}
