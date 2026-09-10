/* Option profiles.
 *
 * These are the app's only user-authored data. An index rebuilds from one
 * rescan and a queue is retypeable; a set of profiles built up over months is
 * not, which is why the store writes through a temp file and a rename and why
 * the round-trip is pinned here.
 *
 * The rules that are easy to get subtly wrong, and are all asserted below:
 * names are case-insensitive but keep their newest capitalisation, deleting
 * the active profile clears the selection rather than leaving it pointing at
 * nothing, and THE URL IS NEVER STORED.
 */

#include "profiles.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

void ytdl_register_profile_tests (void);

typedef struct
{
  char *dir;
  char *old_xdg;
} Fixture;

static void
set_up (Fixture *fx, gconstpointer unused)
{
  /* The store writes to $XDG_CONFIG_HOME/ytdl-gtk. Pointing that at a temp
   * directory keeps the tests off the real profiles of whoever runs them. */
  fx->dir = g_dir_make_tmp ("ytdl-profiles-XXXXXX", NULL);
  g_assert_nonnull (fx->dir);
  const char *old = g_getenv ("XDG_CONFIG_HOME");
  fx->old_xdg = old != NULL ? g_strdup (old) : NULL;
  g_setenv ("XDG_CONFIG_HOME", fx->dir, TRUE);
}

static void
rm_rf (const char *path)
{
  g_autoptr (GDir) d = g_dir_open (path, 0, NULL);
  if (d != NULL)
    {
      const char *name;
      while ((name = g_dir_read_name (d)) != NULL)
        {
          g_autofree char *child = g_build_filename (path, name, NULL);
          if (g_file_test (child, G_FILE_TEST_IS_DIR))
            rm_rf (child);
          else
            g_unlink (child);
        }
    }
  g_rmdir (path);
}

static void
tear_down (Fixture *fx, gconstpointer unused)
{
  if (fx->old_xdg != NULL)
    g_setenv ("XDG_CONFIG_HOME", fx->old_xdg, TRUE);
  else
    g_unsetenv ("XDG_CONFIG_HOME");
  rm_rf (fx->dir);
  g_free (fx->dir);
  g_free (fx->old_xdg);
}

static YtdlRunOptions *
sample_options (void)
{
  YtdlRunOptions *o = ytdl_run_options_new ();
  o->url = g_strdup ("https://example.com/should-not-be-stored");
  o->mode = g_strdup ("audio-only");
  o->quality = g_strdup ("1080");
  o->container = g_strdup ("webm");
  o->workers = 4;
  o->sync = TRUE;
  o->no_comments = TRUE;
  g_ptr_array_add (o->ytdlp_args, g_strdup ("--match-filter"));
  g_ptr_array_add (o->ytdlp_args, g_strdup ("duration > 60"));
  return o;
}

/* ---------------------------------------------------------------------- */

static void
test_save_then_reload_round_trips (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlRunOptions) o = sample_options ();
  GError *error = NULL;

  {
    g_autoptr (YtdlProfileStore) store = ytdl_profiles_load ();
    g_assert_cmpuint (store->profiles->len, ==, 0);
    g_assert_true (ytdl_profiles_save (store, "Archival", o, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (store->active, ==, "Archival");
  }

  /* A fresh load: this is the bit that proves it reached the disk. */
  g_autoptr (YtdlProfileStore) again = ytdl_profiles_load ();
  g_assert_cmpuint (again->profiles->len, ==, 1);
  g_assert_cmpstr (again->active, ==, "Archival");

  const YtdlProfile *p = ytdl_profiles_get (again, "Archival");
  g_assert_nonnull (p);
  g_assert_cmpstr (p->opts->mode, ==, "audio-only");
  g_assert_cmpstr (p->opts->quality, ==, "1080");
  g_assert_cmpstr (p->opts->container, ==, "webm");
  g_assert_cmpuint (p->opts->workers, ==, 4);
  g_assert_true (p->opts->sync);
  g_assert_true (p->opts->no_comments);
  g_assert_cmpuint (p->opts->ytdlp_args->len, ==, 2);
  g_assert_cmpstr (g_ptr_array_index (p->opts->ytdlp_args, 1), ==,
                   "duration > 60");
  g_assert_cmpint (p->saved, >, 0);
}

static void
test_url_is_never_stored (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlRunOptions) o = sample_options ();
  GError *error = NULL;

  g_autoptr (YtdlProfileStore) store = ytdl_profiles_load ();
  g_assert_true (ytdl_profiles_save (store, "P", o, &error));
  g_assert_no_error (error);

  /* A preset that replaced what you were about to download would be the one
   * thing a preset must never do. */
  const YtdlProfile *p = ytdl_profiles_get (store, "P");
  g_assert_null (p->opts->url);

  g_autoptr (YtdlProfileStore) reloaded = ytdl_profiles_load ();
  g_assert_null (ytdl_profiles_get (reloaded, "P")->opts->url);

  /* The caller's own options are untouched -- saving a profile must not clear
   * the URL box the user is still working in. */
  g_assert_cmpstr (o->url, ==, "https://example.com/should-not-be-stored");
}

static void
test_names_are_case_insensitive_but_keep_new_capitalisation (
    Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlRunOptions) o = sample_options ();
  GError *error = NULL;
  g_autoptr (YtdlProfileStore) store = ytdl_profiles_load ();

  g_assert_true (ytdl_profiles_save (store, "archival", o, &error));
  g_assert_true (ytdl_profiles_save (store, "Archival", o, &error));
  g_assert_no_error (error);

  /* One profile, not two indistinguishable rows in a dropdown. */
  g_assert_cmpuint (store->profiles->len, ==, 1);
  const YtdlProfile *p = g_ptr_array_index (store->profiles, 0);
  /* Re-saving fixes the capitalisation rather than ignoring it. */
  g_assert_cmpstr (p->name, ==, "Archival");
  g_assert_nonnull (ytdl_profiles_get (store, "ARCHIVAL"));
}

static void
test_delete_clears_active_when_it_was_active (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlRunOptions) o = sample_options ();
  GError *error = NULL;
  g_autoptr (YtdlProfileStore) store = ytdl_profiles_load ();

  g_assert_true (ytdl_profiles_save (store, "A", o, &error));
  g_assert_true (ytdl_profiles_save (store, "B", o, &error));
  g_assert_cmpstr (store->active, ==, "B");

  g_assert_true (ytdl_profiles_delete (store, "B", &error));
  g_assert_no_error (error);
  /* Left pointing at a name that no longer exists, the dropdown would show a
   * selection that cannot be applied. */
  g_assert_null (store->active);
  g_assert_cmpuint (store->profiles->len, ==, 1);

  /* Deleting the other one does not touch the (already cleared) selection. */
  g_assert_true (ytdl_profiles_delete (store, "a", &error));
  g_assert_cmpuint (store->profiles->len, ==, 0);
}

static void
test_rename (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlRunOptions) o = sample_options ();
  GError *error = NULL;
  g_autoptr (YtdlProfileStore) store = ytdl_profiles_load ();

  g_assert_true (ytdl_profiles_save (store, "Old", o, &error));
  g_assert_true (ytdl_profiles_rename (store, "Old", "New", &error));
  g_assert_no_error (error);
  g_assert_nonnull (ytdl_profiles_get (store, "New"));
  g_assert_null (ytdl_profiles_get (store, "Old"));
  /* The renamed profile was active, so the selection follows it. */
  g_assert_cmpstr (store->active, ==, "New");

  /* Re-capitalising itself is a rename people actually do, and must not
   * collide with itself. */
  g_assert_true (ytdl_profiles_rename (store, "New", "NEW", &error));
  g_assert_no_error (error);
  g_assert_cmpstr (store->active, ==, "NEW");

  g_assert_true (ytdl_profiles_save (store, "Other", o, &error));
  g_assert_false (ytdl_profiles_rename (store, "Other", "NEW", &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
}

static void
test_activate (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlRunOptions) o = sample_options ();
  GError *error = NULL;
  g_autoptr (YtdlProfileStore) store = ytdl_profiles_load ();
  g_assert_true (ytdl_profiles_save (store, "A", o, &error));

  g_assert_true (ytdl_profiles_activate (store, NULL, &error));
  g_assert_null (store->active);

  /* Case-insensitive lookup, but the STORED capitalisation is what is kept. */
  g_assert_true (ytdl_profiles_activate (store, "a", &error));
  g_assert_cmpstr (store->active, ==, "A");

  /* Only reachable from a stale window, so it is an error rather than a
   * silent no-op. */
  g_assert_false (ytdl_profiles_activate (store, "nope", &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
}

static void
test_bad_names_are_refused (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlRunOptions) o = sample_options ();
  GError *error = NULL;
  g_autoptr (YtdlProfileStore) store = ytdl_profiles_load ();

  g_assert_false (ytdl_profiles_save (store, "   ", o, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  g_autofree char *too_long = g_strnfill (YTDL_PROFILE_MAX_NAME + 1, 'x');
  g_assert_false (ytdl_profiles_save (store, too_long, o, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  /* Trimmed, not rejected: a trailing space is a typo, not a different name. */
  g_assert_true (ytdl_profiles_save (store, "  Padded  ", o, &error));
  g_assert_no_error (error);
  g_assert_nonnull (ytdl_profiles_get (store, "Padded"));
}

static void
test_stale_active_name_is_cleared_on_load (Fixture *fx, gconstpointer unused)
{
  /* Hand-written profiles.json naming an active profile that is not in the
   * list. Left as-is, the dropdown would select nothing and report a name. */
  g_autofree char *dir = g_build_filename (fx->dir, "ytdl-gtk", NULL);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  g_autofree char *path = g_build_filename (dir, "profiles.json", NULL);
  g_assert_true (g_file_set_contents (
      path, "{ \"active\": \"ghost\", \"profiles\": [] }", -1, NULL));

  g_autoptr (YtdlProfileStore) store = ytdl_profiles_load ();
  g_assert_null (store->active);
}

static void
test_hand_written_url_is_dropped_on_load (Fixture *fx, gconstpointer unused)
{
  /* The URL is dropped on the way IN as well as on the way out, so a
   * profiles.json edited by hand cannot hijack a download. */
  g_autofree char *dir = g_build_filename (fx->dir, "ytdl-gtk", NULL);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  g_autofree char *path = g_build_filename (dir, "profiles.json", NULL);
  g_assert_true (g_file_set_contents (
      path,
      "{ \"active\": null, \"profiles\": [ { \"name\": \"H\", \"saved\": 1, "
      "  \"opts\": { \"url\": \"https://evil.example/x\", "
      "              \"mode\": \"audio-only\" } } ] }",
      -1, NULL));

  g_autoptr (YtdlProfileStore) store = ytdl_profiles_load ();
  const YtdlProfile *p = ytdl_profiles_get (store, "H");
  g_assert_nonnull (p);
  g_assert_null (p->opts->url);
  g_assert_cmpstr (p->opts->mode, ==, "audio-only");
}

static void
test_missing_file_is_an_empty_store (Fixture *fx, gconstpointer unused)
{
  g_autoptr (YtdlProfileStore) store = ytdl_profiles_load ();
  g_assert_cmpuint (store->profiles->len, ==, 0);
  g_assert_null (store->active);
  g_assert_null (ytdl_profiles_get (store, "anything"));
}

/* ---------------------------------------------------------------------- */
/* The default profile a fresh install starts with                        */
/* ---------------------------------------------------------------------- */

static void
test_first_run_installs_the_default_profile (Fixture *fx, gconstpointer unused)
{
  g_assert_true (ytdl_profiles_seed_default ());

  g_autoptr (YtdlProfileStore) store = ytdl_profiles_load ();
  g_assert_cmpuint (store->profiles->len, ==, 1);

  const YtdlProfile *p = g_ptr_array_index (store->profiles, 0);
  g_assert_cmpstr (p->name, ==, YTDL_PROFILE_DEFAULT_NAME);
  g_assert_cmpint (p->saved, >, 0);

  /* Nothing is SELECTED. The seeded profile is somewhere to go back to, not a
   * preset silently applied to a form the user has not touched yet. */
  g_assert_null (store->active);
}

static void
test_the_default_profile_sets_nothing (Fixture *fx, gconstpointer unused)
{
  /* It carries the app's own defaults, so applying it produces exactly the
   * command line a fresh form produces. A shipped profile that picked a
   * quality or a container would be this window deciding pipeline policy,
   * which is run_ytdlp.ps1's job on the far side of the CLI_VERSION pin. */
  g_assert_true (ytdl_profiles_seed_default ());

  g_autoptr (YtdlProfileStore) store = ytdl_profiles_load ();
  const YtdlProfile *p =
      ytdl_profiles_get (store, YTDL_PROFILE_DEFAULT_NAME);
  g_assert_nonnull (p);

  g_autoptr (YtdlRunOptions) from_profile = ytdl_run_options_copy (p->opts);
  from_profile->url = g_strdup ("https://example.com/watch?v=aaaaaaaaaaa");
  g_autoptr (YtdlRunOptions) fresh = ytdl_run_options_new ();
  fresh->url = g_strdup ("https://example.com/watch?v=aaaaaaaaaaa");

  g_autofree char *seeded = ytdl_run_options_command_preview (from_profile);
  g_autofree char *plain = ytdl_run_options_command_preview (fresh);
  g_assert_cmpstr (seeded, ==, plain);
}

static void
test_the_default_is_seeded_once_and_stays_deleted (Fixture *fx,
                                                   gconstpointer unused)
{
  GError *error = NULL;
  g_assert_true (ytdl_profiles_seed_default ());

  {
    g_autoptr (YtdlProfileStore) store = ytdl_profiles_load ();
    g_assert_true (
        ytdl_profiles_delete (store, YTDL_PROFILE_DEFAULT_NAME, &error));
    g_assert_no_error (error);
  }

  /* profiles.json still exists -- now holding an empty list -- so this is no
   * longer a fresh install. A default that came back at every launch would be
   * a profile the user cannot get rid of. */
  g_assert_false (ytdl_profiles_seed_default ());

  g_autoptr (YtdlProfileStore) again = ytdl_profiles_load ();
  g_assert_cmpuint (again->profiles->len, ==, 0);
}

static void
test_seeding_never_touches_an_existing_store (Fixture *fx,
                                              gconstpointer unused)
{
  g_autoptr (YtdlRunOptions) o = sample_options ();
  GError *error = NULL;

  {
    g_autoptr (YtdlProfileStore) store = ytdl_profiles_load ();
    g_assert_true (ytdl_profiles_save (store, "Mine", o, &error));
    g_assert_no_error (error);
  }

  g_assert_false (ytdl_profiles_seed_default ());

  g_autoptr (YtdlProfileStore) again = ytdl_profiles_load ();
  g_assert_cmpuint (again->profiles->len, ==, 1);
  g_assert_nonnull (ytdl_profiles_get (again, "Mine"));
  g_assert_null (ytdl_profiles_get (again, YTDL_PROFILE_DEFAULT_NAME));
}

static void
test_a_corrupt_store_is_not_replaced_by_the_default (Fixture *fx,
                                                     gconstpointer unused)
{
  /* This is why the check is "is there a file" rather than "did it parse". An
   * unreadable profiles.json is still somebody's profiles -- half-written by a
   * crash, mangled by an editor mid-save -- and overwriting it with a default
   * is the one recovery nobody can undo. */
  g_autofree char *dir = g_build_filename (fx->dir, "ytdl-gtk", NULL);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  g_autofree char *path = g_build_filename (dir, "profiles.json", NULL);
  g_assert_true (g_file_set_contents (path, "{ not json", -1, NULL));

  g_assert_false (ytdl_profiles_seed_default ());

  g_autofree char *after = NULL;
  g_assert_true (g_file_get_contents (path, &after, NULL, NULL));
  g_assert_cmpstr (after, ==, "{ not json");
}

void
ytdl_register_profile_tests (void)
{
#define FIX(path, fn) g_test_add (path, Fixture, NULL, set_up, fn, tear_down)
  FIX ("/profiles/round-trip", test_save_then_reload_round_trips);
  FIX ("/profiles/url-never-stored", test_url_is_never_stored);
  FIX ("/profiles/case-insensitive",
       test_names_are_case_insensitive_but_keep_new_capitalisation);
  FIX ("/profiles/delete-clears-active",
       test_delete_clears_active_when_it_was_active);
  FIX ("/profiles/rename", test_rename);
  FIX ("/profiles/activate", test_activate);
  FIX ("/profiles/bad-names", test_bad_names_are_refused);
  FIX ("/profiles/stale-active-cleared",
       test_stale_active_name_is_cleared_on_load);
  FIX ("/profiles/hand-written-url-dropped",
       test_hand_written_url_is_dropped_on_load);
  FIX ("/profiles/missing-file", test_missing_file_is_an_empty_store);
  FIX ("/profiles/default-seeded-on-first-run",
       test_first_run_installs_the_default_profile);
  FIX ("/profiles/default-sets-nothing", test_the_default_profile_sets_nothing);
  FIX ("/profiles/default-stays-deleted",
       test_the_default_is_seeded_once_and_stays_deleted);
  FIX ("/profiles/seed-leaves-existing-store-alone",
       test_seeding_never_touches_an_existing_store);
  FIX ("/profiles/seed-leaves-corrupt-store-alone",
       test_a_corrupt_store_is_not_replaced_by_the_default);
#undef FIX
}
