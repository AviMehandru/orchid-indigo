/* The precedence between the three ways an archive root can be named.
 *
 * This file exists because of a real bug rather than for symmetry: for the
 * whole life of this app settings.json carried an `archive_root` that
 * settings.c faithfully loaded and saved and that NOTHING read. main.c went
 * straight from --archive-root to autodetection, so the stored value was
 * write-only -- and the Health pane had no way to write it in the first
 * place. The macOS and Windows apps both consulted their stored value first,
 * which made this the one place the three readers disagreed about something
 * the user can see.
 *
 * A test that only asserted "the flag wins" would have passed throughout. The
 * case that catches it is the middle one: no flag, a stored root, and an
 * autodetectable archive somewhere else. That is the assertion this file is
 * really for.
 *
 * Everything here points HOME and YTDLP_INSTALL_ROOT at a sandbox, so running
 * the suite can neither read nor be confused by the archive of whoever runs
 * it.
 */

#include "paths.h"

#include <glib.h>
#include <glib/gstdio.h>

typedef struct
{
  char *tmpdir;      /* the whole sandbox */
  char *home;        /* $HOME within it */
  char *autodetected;/* an archive under $HOME/yt-dlp, findable by autodetect */
  char *elsewhere;   /* an archive nothing autodetects */
  char *old_home;
  char *old_install;
} Fixture;

static void
mkdirp (const char *path)
{
  g_assert_cmpint (g_mkdir_with_parents (path, 0755), ==, 0);
}

/* A data root is accepted by ytdl_resolve_archive_root when it holds
 * "Youtube Videos/Complete Archive". Returns the resolved Complete Archive
 * path the chooser is expected to hand back for @data_root. */
static char *
make_archive (const char *data_root)
{
  char *complete =
      g_build_filename (data_root, "Youtube Videos", "Complete Archive", NULL);
  mkdirp (complete);
  return complete;
}

static void
set_up (Fixture *fx, gconstpointer unused)
{
  GError *error = NULL;
  fx->tmpdir = g_dir_make_tmp ("ytdl-gtk-paths-XXXXXX", &error);
  g_assert_no_error (error);

  const char *old = g_getenv ("HOME");
  fx->old_home = g_strdup (old != NULL ? old : "");
  old = g_getenv ("YTDLP_INSTALL_ROOT");
  fx->old_install = g_strdup (old != NULL ? old : "");

  fx->home = g_build_filename (fx->tmpdir, "home", NULL);
  mkdirp (fx->home);
  g_setenv ("HOME", fx->home, TRUE);

  /* Unset rather than pointed somewhere: with it set, autodetection would
   * find the install root first and these tests would never exercise the
   * ~/yt-dlp candidate the real app usually hits. */
  g_unsetenv ("YTDLP_INSTALL_ROOT");

  g_autofree char *home_ytdlp = g_build_filename (fx->home, "yt-dlp", NULL);
  fx->autodetected = make_archive (home_ytdlp);

  g_autofree char *nas = g_build_filename (fx->tmpdir, "nas", NULL);
  fx->elsewhere = make_archive (nas);
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
          if (g_file_test (child, G_FILE_TEST_IS_DIR) &&
              !g_file_test (child, G_FILE_TEST_IS_SYMLINK))
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
  if (fx->old_home[0] != '\0')
    g_setenv ("HOME", fx->old_home, TRUE);
  else
    g_unsetenv ("HOME");

  if (fx->old_install[0] != '\0')
    g_setenv ("YTDLP_INSTALL_ROOT", fx->old_install, TRUE);
  else
    g_unsetenv ("YTDLP_INSTALL_ROOT");

  rm_rf (fx->tmpdir);
  g_free (fx->tmpdir);
  g_free (fx->home);
  g_free (fx->autodetected);
  g_free (fx->elsewhere);
  g_free (fx->old_home);
  g_free (fx->old_install);
}

/* Nothing named: the usual locations, which is what every existing install
 * relies on and what must not change. */
static void
test_nothing_named_autodetects (Fixture *fx, gconstpointer unused)
{
  g_autofree char *got = ytdl_choose_archive_root (NULL, NULL);
  g_assert_cmpstr (got, ==, fx->autodetected);
}

/* An empty string is not a choice. It is what settings.json holds on a
 * machine that has never used the picker, and treating it as a path would
 * resolve the current directory. */
static void
test_empty_strings_are_not_choices (Fixture *fx, gconstpointer unused)
{
  g_autofree char *got = ytdl_choose_archive_root ("", "");
  g_assert_cmpstr (got, ==, fx->autodetected);
}

/* THE REGRESSION. A stored root beats autodetection -- this is what was
 * missing, and the reason the field was dead weight. */
static void
test_configured_beats_autodetection (Fixture *fx, gconstpointer unused)
{
  g_autofree char *nas = g_build_filename (fx->tmpdir, "nas", NULL);
  g_autofree char *got = ytdl_choose_archive_root (NULL, nas);
  g_assert_cmpstr (got, ==, fx->elsewhere);
}

/* The flag beats the stored root, so --archive-root still means "this tree,
 * this launch" on a machine that has one configured. */
static void
test_cli_beats_configured (Fixture *fx, gconstpointer unused)
{
  g_autofree char *nas = g_build_filename (fx->tmpdir, "nas", NULL);
  g_autofree char *home_ytdlp = g_build_filename (fx->home, "yt-dlp", NULL);
  g_autofree char *got = ytdl_choose_archive_root (home_ytdlp, nas);
  g_assert_cmpstr (got, ==, fx->autodetected);
}

/* A stored root that no longer resolves is an unplugged disk far more often
 * than a decision, so it falls back -- the same answer the macOS and Windows
 * apps give. */
static void
test_unresolvable_configured_falls_back (Fixture *fx, gconstpointer unused)
{
  g_autofree char *gone = g_build_filename (fx->tmpdir, "unmounted", NULL);
  g_autofree char *got = ytdl_choose_archive_root (NULL, gone);
  g_assert_cmpstr (got, ==, fx->autodetected);
}

/* The flag does NOT fall back, and that asymmetry is the point: someone who
 * typed a path is naming a specific tree, and scanning a different one
 * because they mistyped it is worse than an empty window that says so. */
static void
test_unresolvable_cli_does_not_fall_back (Fixture *fx, gconstpointer unused)
{
  g_autofree char *gone = g_build_filename (fx->tmpdir, "typo", NULL);
  g_autofree char *got = ytdl_choose_archive_root (gone, NULL);
  g_assert_null (got);
}

/* Nothing anywhere is a real state -- a machine with the app but no archive
 * yet -- and must be NULL rather than a path that does not exist. */
static void
test_no_archive_anywhere_is_null (Fixture *fx, gconstpointer unused)
{
  g_autofree char *empty = g_build_filename (fx->tmpdir, "bare", NULL);
  mkdirp (empty);
  g_setenv ("HOME", empty, TRUE);

  g_autofree char *got = ytdl_choose_archive_root (NULL, NULL);
  g_assert_null (got);
}

void ytdl_register_paths_tests (void);

void
ytdl_register_paths_tests (void)
{
#define FIX(path, fn) g_test_add (path, Fixture, NULL, set_up, fn, tear_down)

  FIX ("/paths/nothing-named-autodetects", test_nothing_named_autodetects);
  FIX ("/paths/empty-is-not-a-choice", test_empty_strings_are_not_choices);
  FIX ("/paths/configured-beats-autodetection",
       test_configured_beats_autodetection);
  FIX ("/paths/cli-beats-configured", test_cli_beats_configured);
  FIX ("/paths/configured-falls-back", test_unresolvable_configured_falls_back);
  FIX ("/paths/cli-does-not-fall-back",
       test_unresolvable_cli_does_not_fall_back);
  FIX ("/paths/none-anywhere-is-null", test_no_archive_anywhere_is_null);

#undef FIX
}
