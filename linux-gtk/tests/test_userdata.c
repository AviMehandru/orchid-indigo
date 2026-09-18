/* Watch state, resume positions and playlists.
 *
 * This is the first thing in the app that is USER DATA rather than a cache,
 * and nearly every case below is about that distinction:
 *
 *   - A corrupt store is NOT silently replaced. The caches are rebuilt from
 *     the archive and losing one costs a rescan; losing this loses the fact
 *     that you watched something, which nothing can reconstruct. So a file
 *     that will not parse leaves the session read-only and stays on disk.
 *   - A record is NOT stamped with the manifest's archive_creation_time, which
 *     is exactly what the verification cache does. "I have seen this" is a
 *     fact about the person, and `ytdl --refresh` fetching newer comments must
 *     not un-watch anything.
 *   - Finishing a video clears its resume point, and glancing at the first ten
 *     seconds does not create one. Both are the difference between a feature
 *     people use and one they turn off.
 *
 * The fixture values are shared with the Swift and C# suites.
 */

#include "library_filter.h"
#include "paths.h"
#include "userdata.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct
{
  char *tmpdir;
  char *old_state;
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

static void
set_up (Fixture *fx, gconstpointer unused)
{
  GError *error = NULL;
  fx->tmpdir = g_dir_make_tmp ("ytdl-gtk-userdata-XXXXXX", &error);
  g_assert_no_error (error);

  /* userdata.json lives in the STATE dir, which is XDG_CONFIG_HOME. Pointed
   * at the sandbox so running the suite cannot clobber the playlists of
   * whoever runs it. */
  fx->old_state = g_strdup (g_getenv ("XDG_CONFIG_HOME"));
  g_autofree char *state = g_build_filename (fx->tmpdir, "config", NULL);
  mkdirp (state);
  g_setenv ("XDG_CONFIG_HOME", state, TRUE);
}

static void
tear_down (Fixture *fx, gconstpointer unused)
{
  if (fx->old_state != NULL)
    g_setenv ("XDG_CONFIG_HOME", fx->old_state, TRUE);
  else
    g_unsetenv ("XDG_CONFIG_HOME");
  g_free (fx->old_state);

  g_autofree char *cmd = g_strdup_printf ("rm -rf '%s'", fx->tmpdir);
  int ignored = system (cmd);
  (void) ignored;
  g_free (fx->tmpdir);
}

static char *
store_path (void)
{
  g_autofree char *dir = ytdl_state_dir ();
  return g_build_filename (dir, "userdata.json", NULL);
}

/* ---------------------------------------------------------------------- */
/* Watch state                                                            */
/* ---------------------------------------------------------------------- */

static void
test_watched_round_trips (Fixture *fx, gconstpointer unused)
{
  {
    g_autoptr (YtdlUserData) ud = ytdl_user_data_load ();
    g_assert_false (ytdl_user_data_is_watched (ud, "aaa"));
    ytdl_user_data_set_watched (ud, "aaa", TRUE);
    g_assert_true (ytdl_user_data_is_watched (ud, "aaa"));
    g_assert_cmpuint (ytdl_user_data_watched_count (ud), ==, 1);
    ytdl_user_data_save (ud);
  }

  g_autoptr (YtdlUserData) reloaded = ytdl_user_data_load ();
  g_assert_true (ytdl_user_data_is_watched (reloaded, "aaa"));
  g_assert_false (ytdl_user_data_is_watched (reloaded, "bbb"));
}

static void
test_finishing_clears_the_resume_point (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlUserData) ud = ytdl_user_data_load ();

  /* Half way through a ten-minute video: a resume point, not watched. */
  ytdl_user_data_set_position (ud, "aaa", 300.0, 600.0);
  g_assert_cmpfloat (ytdl_user_data_position (ud, "aaa"), ==, 300.0);
  g_assert_false (ytdl_user_data_is_watched (ud, "aaa"));

  /* Past the 90% mark. Watched, and the resume point GOES -- re-opening
   * something you finished should start it again rather than drop you back
   * at the end card. */
  ytdl_user_data_set_position (ud, "aaa", 560.0, 600.0);
  g_assert_true (ytdl_user_data_is_watched (ud, "aaa"));
  g_assert_cmpfloat (ytdl_user_data_position (ud, "aaa"), ==, 0.0);
}

static void
test_a_glance_does_not_become_a_resume_point (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlUserData) ud = ytdl_user_data_load ();

  /* Opening a video and closing it again must not litter the library with
   * eight-second resume offers. */
  ytdl_user_data_set_position (ud, "aaa", 8.0, 600.0);
  g_assert_cmpfloat (ytdl_user_data_position (ud, "aaa"), ==, 0.0);

  /* And a video already seen does not become unseen because somebody
   * glanced at the first ten seconds of it. */
  ytdl_user_data_set_watched (ud, "bbb", TRUE);
  ytdl_user_data_set_position (ud, "bbb", 5.0, 600.0);
  g_assert_true (ytdl_user_data_is_watched (ud, "bbb"));
}

static void
test_unknown_duration_stores_the_position (Fixture *fx, gconstpointer unused)
{
  /* A folder with no info.json has no duration, which the layout contract
   * names as ordinary. The position is still worth keeping; nothing is
   * inferred about being finished. */
  g_autoptr (YtdlUserData) ud = ytdl_user_data_load ();
  ytdl_user_data_set_position (ud, "aaa", 120.0, 0.0);
  g_assert_cmpfloat (ytdl_user_data_position (ud, "aaa"), ==, 120.0);
  g_assert_false (ytdl_user_data_is_watched (ud, "aaa"));
}

static void
test_marking_watched_by_hand_clears_the_position (Fixture *fx,
                                                  gconstpointer unused)
{
  g_autoptr (YtdlUserData) ud = ytdl_user_data_load ();

  ytdl_user_data_set_position (ud, "aaa", 300.0, 600.0);
  ytdl_user_data_set_watched (ud, "aaa", TRUE);
  g_assert_cmpfloat (ytdl_user_data_position (ud, "aaa"), ==, 0.0);

  /* But marking it UNWATCHED leaves a position alone: "I want to see this
   * again" and "start it over" are different wishes, and only one of them
   * was expressed. */
  ytdl_user_data_set_position (ud, "bbb", 300.0, 600.0);
  ytdl_user_data_set_watched (ud, "bbb", TRUE);
  ytdl_user_data_set_position (ud, "bbb", 250.0, 600.0);
  ytdl_user_data_set_watched (ud, "bbb", FALSE);
  g_assert_cmpfloat (ytdl_user_data_position (ud, "bbb"), ==, 250.0);
}

static void
test_empty_records_are_not_written (Fixture *fx, gconstpointer unused)
{
  {
    g_autoptr (YtdlUserData) ud = ytdl_user_data_load ();
    /* Touched, and then left saying nothing at all. */
    ytdl_user_data_set_position (ud, "aaa", 3.0, 600.0);
    ytdl_user_data_save (ud);
  }

  /* The file must not accumulate a line for every video anybody ever
   * opened. */
  g_autofree char *path = store_path ();
  g_autofree char *text = NULL;
  if (g_file_get_contents (path, &text, NULL, NULL))
    g_assert_null (strstr (text, "aaa"));
}

/* ---------------------------------------------------------------------- */
/* The corrupt-store rule                                                 */
/* ---------------------------------------------------------------------- */

static void
test_corrupt_store_is_read_only_and_preserved (Fixture *fx,
                                               gconstpointer unused)
{
  g_autofree char *path = store_path ();
  write_file (path, "{ this is not json");

  /* BOTH warnings are asserted rather than silenced. Losing watch state is
   * the one failure in this app that nothing can reconstruct, so the loud
   * complaint is a feature: a store that stopped parsing and said nothing
   * would look exactly like a store that was never written. g_test framework
   * makes a g_warning fatal, which is why these have to be declared -- and
   * declaring them is what pins the behaviour. */
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*could not parse*");

  g_autoptr (YtdlUserData) ud = ytdl_user_data_load ();
  g_assert_nonnull (ud);
  g_assert_true (ytdl_user_data_is_read_only (ud));

  /* The app still runs -- refusing to start over a malformed file would lose
   * the whole application rather than one file. */
  ytdl_user_data_set_watched (ud, "aaa", TRUE);

  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*not overwriting*");
  ytdl_user_data_save (ud);
  g_test_assert_expected_messages ();

  /* THE POINT: the unparseable file is still there, byte for byte, for its
   * owner to look at. A cache would have been replaced; this is not a
   * cache. */
  g_autofree char *after = NULL;
  g_assert_true (g_file_get_contents (path, &after, NULL, NULL));
  g_assert_cmpstr (after, ==, "{ this is not json");
}

static void
test_missing_store_is_not_read_only (Fixture *fx, gconstpointer unused)
{
  /* A file that is not there is the ordinary first-launch state. It must NOT
   * be treated as corrupt, or a fresh install could never save anything. */
  g_autoptr (YtdlUserData) ud = ytdl_user_data_load ();
  g_assert_false (ytdl_user_data_is_read_only (ud));

  ytdl_user_data_set_watched (ud, "aaa", TRUE);
  ytdl_user_data_save (ud);

  g_autoptr (YtdlUserData) reloaded = ytdl_user_data_load ();
  g_assert_true (ytdl_user_data_is_watched (reloaded, "aaa"));
}

/* ---------------------------------------------------------------------- */
/* Playlists                                                              */
/* ---------------------------------------------------------------------- */

static void
test_playlist_crud (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlUserData) ud = ytdl_user_data_load ();

  YtdlPlaylist *pl = ytdl_user_data_playlist_create (ud, "  Watch later  ");
  g_assert_nonnull (pl);
  /* Trimmed, because a trailing space is invisible and would make two
   * playlists look identical. */
  g_assert_cmpstr (pl->name, ==, "Watch later");
  g_assert_nonnull (pl->id);

  /* A blank name is refused: an unnamed playlist is unfindable. */
  g_assert_null (ytdl_user_data_playlist_create (ud, "   "));
  g_assert_null (ytdl_user_data_playlist_create (ud, NULL));

  /* Duplicate names ARE allowed. They are the user's to make, ids are what
   * identify a playlist, and refusing the second would be this app deciding
   * something it has no business deciding. */
  YtdlPlaylist *twin = ytdl_user_data_playlist_create (ud, "Watch later");
  g_assert_nonnull (twin);
  g_assert_cmpstr (twin->id, !=, pl->id);
  g_assert_cmpuint (ytdl_user_data_playlists (ud)->len, ==, 2);

  g_autofree char *id = g_strdup (pl->id);

  g_assert_true (ytdl_user_data_playlist_add (ud, id, "aaa"));
  g_assert_true (ytdl_user_data_playlist_add (ud, id, "bbb"));
  /* Adding twice is a no-op, not a duplicate: a playlist is a set the user
   * ordered, not a bag. */
  g_assert_true (ytdl_user_data_playlist_add (ud, id, "aaa"));
  g_assert_cmpuint (pl->keys->len, ==, 2);
  g_assert_true (ytdl_user_data_playlist_contains (ud, id, "aaa"));

  /* Renaming does NOT change the id, which is why one exists. */
  g_assert_true (ytdl_user_data_playlist_rename (ud, id, "Tonight"));
  g_assert_cmpstr (ytdl_user_data_playlist (ud, id)->name, ==, "Tonight");
  g_assert_false (ytdl_user_data_playlist_rename (ud, id, "  "));

  g_assert_true (ytdl_user_data_playlist_remove (ud, id, "aaa"));
  g_assert_false (ytdl_user_data_playlist_contains (ud, id, "aaa"));
  g_assert_false (ytdl_user_data_playlist_remove (ud, id, "aaa"));

  g_assert_true (ytdl_user_data_playlist_delete (ud, id));
  g_assert_null (ytdl_user_data_playlist (ud, id));
  g_assert_cmpuint (ytdl_user_data_playlists (ud)->len, ==, 1);
}

static void
test_playlists_round_trip (Fixture *fx, gconstpointer unused)
{
  g_autofree char *id = NULL;
  {
    g_autoptr (YtdlUserData) ud = ytdl_user_data_load ();
    YtdlPlaylist *pl = ytdl_user_data_playlist_create (ud, "Tonight");
    id = g_strdup (pl->id);
    ytdl_user_data_playlist_add (ud, id, "aaa");
    ytdl_user_data_playlist_add (ud, id, "bbb");
    ytdl_user_data_save (ud);
  }

  g_autoptr (YtdlUserData) reloaded = ytdl_user_data_load ();
  YtdlPlaylist *pl = ytdl_user_data_playlist (reloaded, id);
  g_assert_nonnull (pl);
  g_assert_cmpstr (pl->name, ==, "Tonight");
  g_assert_cmpuint (pl->keys->len, ==, 2);
  /* Order is the user's and is preserved. */
  g_assert_cmpstr (g_ptr_array_index (pl->keys, 0), ==, "aaa");
  g_assert_cmpstr (g_ptr_array_index (pl->keys, 1), ==, "bbb");
}

/* ---------------------------------------------------------------------- */
/* The facets these feed                                                  */
/* ---------------------------------------------------------------------- */

static void
test_unwatched_facet (Fixture *fx, gconstpointer unused)
{
  /* The facet reads a borrowed SET rather than calling back into the store,
   * so this can be asserted without an archive at all. NULL reads as
   * "nothing is watched", which is exactly right for a fresh install: every
   * video is unwatched. */
  g_autoptr (YtdlLibraryFilter) f = ytdl_library_filter_new ();
  f->flags = YTDL_FACET_UNWATCHED;

  YtdlEntry a = { 0 };
  a.key = (char *) "aaa";
  a.title = (char *) "A";
  a.rel = (char *) "A";

  g_assert_true (ytdl_library_filter_matches (f, &a, NULL, NULL));

  g_autoptr (GHashTable) watched =
      g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_add (watched, (gpointer) "aaa");
  f->watched_keys = watched;

  g_assert_false (ytdl_library_filter_matches (f, &a, NULL, NULL));

  /* And with the facet off, being watched is irrelevant again. */
  f->flags = YTDL_FACET_NONE;
  g_assert_true (ytdl_library_filter_matches (f, &a, NULL, NULL));
}

/* The set the facet reads is maintained beside the watch records rather than
 * derived from them each time, which is a duplicated fact and therefore a
 * thing that can drift. Every path that can set or clear a watched flag is
 * driven here and the set is checked against the store after each one. */
static void
test_watched_set_tracks_every_path (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlUserData) ud = ytdl_user_data_load ();
  GHashTable *set = ytdl_user_data_watched_keys (ud);
  g_assert_nonnull (set);
  g_assert_cmpuint (g_hash_table_size (set), ==, 0);

  /* By hand. */
  ytdl_user_data_set_watched (ud, "aaa", TRUE);
  g_assert_true (g_hash_table_contains (set, "aaa"));
  g_assert_cmpuint (ytdl_user_data_watched_count (ud), ==, 1);

  /* By finishing: crossing YTDL_WATCHED_FRACTION sets the flag from inside
   * set_position, which is the path a hand-written set would forget. */
  ytdl_user_data_set_position (ud, "bbb", 95.0, 100.0);
  g_assert_true (g_hash_table_contains (set, "bbb"));
  g_assert_cmpuint (ytdl_user_data_watched_count (ud), ==, 2);

  /* A resume point is NOT being watched. */
  ytdl_user_data_set_position (ud, "ccc", 40.0, 100.0);
  g_assert_false (g_hash_table_contains (set, "ccc"));
  g_assert_cmpuint (ytdl_user_data_watched_count (ud), ==, 2);

  /* And back off again. */
  ytdl_user_data_set_watched (ud, "aaa", FALSE);
  g_assert_false (g_hash_table_contains (set, "aaa"));
  g_assert_cmpuint (ytdl_user_data_watched_count (ud), ==, 1);

  /* The table is LIVE, not a snapshot: the same pointer reflects all of the
   * above. That is what lets the Library hold it across a whole session. */
  g_assert_true (set == ytdl_user_data_watched_keys (ud));

  /* And it survives a round trip through the file. */
  ytdl_user_data_save (ud);
  g_autoptr (YtdlUserData) again = ytdl_user_data_load ();
  GHashTable *reloaded = ytdl_user_data_watched_keys (again);
  g_assert_cmpuint (g_hash_table_size (reloaded), ==, 1);
  g_assert_true (g_hash_table_contains (reloaded, "bbb"));
}

static void
test_playlist_facet_and_count (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlLibraryFilter) f = ytdl_library_filter_new ();

  YtdlEntry a = { 0 };
  a.key = (char *) "aaa";
  a.title = (char *) "A";
  a.rel = (char *) "A";

  /* NULL means no playlist filter. */
  g_assert_true (ytdl_library_filter_matches (f, &a, NULL, NULL));
  g_assert_cmpuint (ytdl_library_filter_facet_count (f), ==, 0);

  /* An EMPTY set means "a playlist is selected and it is empty", which shows
   * nothing -- the same asymmetry key_allow has, and the opposite of what
   * an empty channel set means. */
  g_autoptr (GHashTable) empty = g_hash_table_new (g_str_hash, g_str_equal);
  f->playlist_keys = empty;
  g_assert_false (ytdl_library_filter_matches (f, &a, NULL, NULL));
  g_assert_cmpuint (ytdl_library_filter_facet_count (f), ==, 1);

  g_hash_table_add (empty, (gpointer) "aaa");
  g_assert_true (ytdl_library_filter_matches (f, &a, NULL, NULL));

  /* Reset drops the playlist but NOT the watched set: the latter is not a
   * filter, it is the data the UNWATCHED facet reads, and clearing it would
   * make "Clear filters" silently turn every video unwatched. */
  g_autoptr (GHashTable) watched = g_hash_table_new (g_str_hash, g_str_equal);
  f->watched_keys = watched;
  ytdl_library_filter_reset (f);
  g_assert_null (f->playlist_keys);
  g_assert_true (f->watched_keys == watched);
}

static void
test_every_flag_has_a_label (Fixture *fx, gconstpointer unused)
{
  /* The UI iterates the flags rather than listing them again, so a flag
   * added to the enum and not named here is a blank checkbox. */
  for (guint bit = 0; bit < YTDL_N_FACET_FLAGS; bit++)
    {
      const char *label = ytdl_facet_flag_label ((YtdlFacetFlags) (1u << bit));
      g_assert_nonnull (label);
      g_assert_cmpstr (label, !=, "");
    }
}

void ytdl_register_userdata_tests (void);

void
ytdl_register_userdata_tests (void)
{
#define FIX(path, fn) g_test_add (path, Fixture, NULL, set_up, fn, tear_down)

  FIX ("/userdata/watched-round-trips", test_watched_round_trips);
  FIX ("/userdata/finishing-clears-resume",
       test_finishing_clears_the_resume_point);
  FIX ("/userdata/glance-is-not-a-resume-point",
       test_a_glance_does_not_become_a_resume_point);
  FIX ("/userdata/unknown-duration", test_unknown_duration_stores_the_position);
  FIX ("/userdata/watched-by-hand-clears-position",
       test_marking_watched_by_hand_clears_the_position);
  FIX ("/userdata/empty-records-not-written", test_empty_records_are_not_written);

  FIX ("/userdata/corrupt-is-read-only",
       test_corrupt_store_is_read_only_and_preserved);
  FIX ("/userdata/missing-is-not-read-only", test_missing_store_is_not_read_only);

  FIX ("/userdata/playlist-crud", test_playlist_crud);
  FIX ("/userdata/playlists-round-trip", test_playlists_round_trip);

  FIX ("/userdata/watched-set-tracks-every-path",
       test_watched_set_tracks_every_path);
  FIX ("/userdata/unwatched-facet", test_unwatched_facet);
  FIX ("/userdata/playlist-facet", test_playlist_facet_and_count);
  FIX ("/userdata/every-flag-has-a-label", test_every_flag_has_a_label);

#undef FIX
}
