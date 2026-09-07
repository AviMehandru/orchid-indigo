/* The conformance test docs/archive-layout.md asks every out-of-repo consumer
 * to keep: build a fixture tree in the documented shape and assert this
 * reader finds it.
 *
 * The point is not coverage. It is that a layout bump in the pipeline becomes
 * a FAILING BUILD here rather than a user reporting an empty library. This
 * repo's tests passing and the pipeline's tests passing says nothing about
 * the two agreeing; only a fixture in the documented shape does.
 *
 * Every case below is a state the contract names as ordinary and that a naive
 * reader gets wrong -- particularly the layout 1 -> 2 changes, where matching
 * on ".mkv" was correct and is now a bug.
 */

#include "archive.h"
#include "paths.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct
{
  char *root;   /* the fake "Complete Archive" */
  char *tmpdir; /* the whole sandbox, removed on teardown */
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

/* One video folder in the documented shape. @manifest_extra is spliced into
 * manifest.json; @info is written verbatim as the info.json, or skipped when
 * NULL to exercise the folder-name fallback. */
static char *
make_video (Fixture *fx, const char *channel, const char *folder,
            const char *manifest_extra, const char *info)
{
  char *vdir = g_build_filename (fx->root, channel, folder, NULL);
  mkdirp (vdir);

  g_autofree char *manifest =
      g_strdup_printf ("{\n"
                       "  \"archive_layout_version\": 2,\n"
                       "  \"video_id\": \"dQw4w9WgXcQ\",\n"
                       "  \"title\": \"from the manifest\",\n"
                       "  \"uploader\": \"%s\"%s%s\n"
                       "}\n",
                       channel, manifest_extra != NULL ? ",\n  " : "",
                       manifest_extra != NULL ? manifest_extra : "");

  g_autofree char *mpath =
      g_build_filename (vdir, "Video metadata", "manifest.json", NULL);
  write_file (mpath, manifest);

  if (info != NULL)
    {
      g_autofree char *ipath = g_build_filename (
          vdir, "Video metadata", "Video.info.json", NULL);
      write_file (ipath, info);
    }

  return vdir;
}

static void
fixture_set_up (Fixture *fx, gconstpointer user_data)
{
  GError *error = NULL;
  fx->tmpdir = g_dir_make_tmp ("ytdl-gtk-test-XXXXXX", &error);
  g_assert_no_error (error);
  fx->root = g_build_filename (fx->tmpdir, "Youtube Videos",
                               "Complete Archive", NULL);
  mkdirp (fx->root);
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
fixture_tear_down (Fixture *fx, gconstpointer user_data)
{
  rm_rf (fx->tmpdir);
  g_free (fx->root);
  g_free (fx->tmpdir);
}

static YtdlIndex *
scan (Fixture *fx)
{
  YtdlIndex *idx = ytdl_index_new ();
  GError *error = NULL;
  g_assert_true (ytdl_index_scan (idx, fx->root, NULL, NULL, &error));
  g_assert_no_error (error);
  return idx;
}

static const YtdlEntry *
only_entry (YtdlIndex *idx)
{
  g_assert_cmpuint (idx->entries->len, ==, 1);
  return g_ptr_array_index (idx->entries, 0);
}

/* ---------------------------------------------------------------------- */

static void
test_discovers_contract_layout (Fixture *fx, gconstpointer unused)
{
  g_autofree char *v = make_video (
      fx, "Rick Astley",
      "Rick Astley - 20091025 - dQw4w9WgXcQ - Never Gonna Give You Up",
      "\"media_file\": \"Final files/Final Video.mkv\"", NULL);
  g_autofree char *media =
      g_build_filename (v, "Final files", "Final Video.mkv", NULL);
  write_file (media, "not really a video");

  YtdlIndex *idx = scan (fx);
  const YtdlEntry *e = only_entry (idx);

  g_assert_cmpstr (e->channel, ==, "Rick Astley");
  g_assert_cmpstr (e->id, ==, "dQw4w9WgXcQ");
  g_assert_cmpstr (e->upload_date, ==, "20091025");
  g_assert_cmpint (ytdl_entry_media_index (e), >=, 0);

  /* The key round-trips: it is the only handle the UI ever holds. */
  g_assert_true (ytdl_index_get (idx, e->key) == e);

  gsize videos, channels;
  ytdl_index_stats (idx, &videos, &channels, NULL);
  g_assert_cmpuint (videos, ==, 1);
  g_assert_cmpuint (channels, ==, 1);

  ytdl_index_free (idx);
}

static void
test_folder_name_fallback (Fixture *fx, gconstpointer unused)
{
  /* No info.json at all -- the documented fallback, and a real state. */
  g_autofree char *v = make_video (
      fx, "Some Channel",
      "Some Channel - 20240101 - abcdefghijk - A Title - With Dashes", NULL,
      NULL);
  (void) v;

  YtdlIndex *idx = scan (fx);
  const YtdlEntry *e = only_entry (idx);

  g_assert_cmpstr (e->id, ==, "abcdefghijk");
  g_assert_cmpstr (e->upload_date, ==, "20240101");
  /* The title keeps its own " - ", which a naive 4-field split would truncate
   * to "A Title". */
  g_assert_cmpstr (e->title, ==, "A Title - With Dashes");

  ytdl_index_free (idx);
}

static void
test_media_found_whatever_container (Fixture *fx, gconstpointer unused)
{
  const char *exts[] = { "mkv", "mp4", "webm" };
  for (gsize i = 0; i < G_N_ELEMENTS (exts); i++)
    {
      g_autofree char *folder = g_strdup_printf (
          "Chan - 2024010%d - abcdefghij%d - T", (int) i + 1, (int) i);
      /* No media_file in the manifest: this is the GLOB path, which is the
       * one that was wrong under layout 1. */
      g_autofree char *v = make_video (fx, "Chan", folder, NULL, NULL);
      g_autofree char *name = g_strdup_printf ("Final Video.%s", exts[i]);
      g_autofree char *media =
          g_build_filename (v, "Final files", name, NULL);
      write_file (media, "x");
    }

  YtdlIndex *idx = scan (fx);
  g_assert_cmpuint (idx->entries->len, ==, 3);
  for (guint i = 0; i < idx->entries->len; i++)
    {
      const YtdlEntry *e = g_ptr_array_index (idx->entries, i);
      g_assert_cmpint (ytdl_entry_media_index (e), >=, 0);
    }
  ytdl_index_free (idx);
}

static void
test_audio_only_download (Fixture *fx, gconstpointer unused)
{
  /* Layout 2's headline change: Final AUDIO, and an extension a video-only
   * reader would never glob for. */
  g_autofree char *v =
      make_video (fx, "Chan", "Chan - 20240101 - abcdefghijk - T",
                  "\"download_mode\": \"audio-only\"", NULL);
  g_autofree char *media =
      g_build_filename (v, "Final files", "Final Audio.opus", NULL);
  write_file (media, "x");

  YtdlIndex *idx = scan (fx);
  const YtdlEntry *e = only_entry (idx);

  gssize mi = ytdl_entry_media_index (e);
  g_assert_cmpint (mi, >=, 0);
  const YtdlFile *f = g_ptr_array_index (e->files, mi);
  g_assert_cmpstr (f->ext, ==, ".opus");
  g_assert_cmpstr (e->download_mode, ==, "audio-only");

  ytdl_index_free (idx);
}

static void
test_no_media_is_valid (Fixture *fx, gconstpointer unused)
{
  /* --mode metadata-only writes a complete folder with no media in it. The
   * entry must EXIST and report -1, not be hidden and not be an error. */
  g_autofree char *v =
      make_video (fx, "Chan", "Chan - 20240101 - abcdefghijk - T",
                  "\"download_mode\": \"metadata-only\", "
                  "\"media_file\": null",
                  NULL);
  (void) v;

  YtdlIndex *idx = scan (fx);
  const YtdlEntry *e = only_entry (idx);

  g_assert_cmpint (ytdl_entry_media_index (e), ==, -1);
  g_assert_cmpstr (e->download_mode, ==, "metadata-only");

  ytdl_index_free (idx);
}

static void
test_skips_pre_merge_streams (Fixture *fx, gconstpointer unused)
{
  /* --keep-video leaves video-only and audio-only files behind. Picking one
   * gives a silent video or a black audio track, so the folder is skipped
   * outright -- and the format-id name is refused even in Final files/. */
  g_autofree char *v =
      make_video (fx, "Chan", "Chan - 20240101 - abcdefghijk - T", NULL, NULL);

  g_autofree char *pre =
      g_build_filename (v, "Pre-merge streams", "Final Video.f137.mp4", NULL);
  write_file (pre, "silent video");

  g_autofree char *stray =
      g_build_filename (v, "Final files", "Final Video.f251.webm", NULL);
  write_file (stray, "audio only");

  YtdlIndex *idx = scan (fx);
  const YtdlEntry *e = only_entry (idx);
  g_assert_cmpint (ytdl_entry_media_index (e), ==, -1);
  ytdl_index_free (idx);

  /* Add the real merged output and it wins. */
  g_autofree char *real =
      g_build_filename (v, "Final files", "Final Video.mkv", NULL);
  write_file (real, "the actual video");

  YtdlIndex *idx2 = scan (fx);
  const YtdlEntry *e2 = only_entry (idx2);
  gssize mi = ytdl_entry_media_index (e2);
  g_assert_cmpint (mi, >=, 0);
  const YtdlFile *f = g_ptr_array_index (e2->files, mi);
  g_assert_cmpstr (f->rel, ==, "Final files/Final Video.mkv");
  ytdl_index_free (idx2);
}

static void
test_video_wins_over_audio (Fixture *fx, gconstpointer unused)
{
  g_autofree char *v =
      make_video (fx, "Chan", "Chan - 20240101 - abcdefghijk - T", NULL, NULL);
  /* Written in an order where a filesystem listing could hand back either
   * first; the choice must not depend on that. */
  g_autofree char *audio =
      g_build_filename (v, "Final files", "Final Audio.m4a", NULL);
  g_autofree char *video =
      g_build_filename (v, "Final files", "Final Video.mp4", NULL);
  write_file (audio, "a");
  write_file (video, "v");

  YtdlIndex *idx = scan (fx);
  const YtdlEntry *e = only_entry (idx);
  gssize mi = ytdl_entry_media_index (e);
  g_assert_cmpint (mi, >=, 0);
  const YtdlFile *f = g_ptr_array_index (e->files, mi);
  g_assert_cmpstr (f->ext, ==, ".mp4");
  ytdl_index_free (idx);
}

static void
test_manifest_media_file_wins (Fixture *fx, gconstpointer unused)
{
  /* The contract says prefer media_file over globbing. Here globbing would
   * find the .mkv; the manifest names the .mp4. */
  g_autofree char *v = make_video (
      fx, "Chan", "Chan - 20240101 - abcdefghijk - T",
      "\"media_file\": \"Final files/Final Video.mp4\"", NULL);
  g_autofree char *a =
      g_build_filename (v, "Final files", "Final Video.mkv", NULL);
  g_autofree char *b =
      g_build_filename (v, "Final files", "Final Video.mp4", NULL);
  write_file (a, "x");
  write_file (b, "y");

  YtdlIndex *idx = scan (fx);
  const YtdlEntry *e = only_entry (idx);
  gssize mi = ytdl_entry_media_index (e);
  const YtdlFile *f = g_ptr_array_index (e->files, mi);
  g_assert_cmpstr (f->rel, ==, "Final files/Final Video.mp4");
  ytdl_index_free (idx);
}

static void
test_missing_layout_version_is_layout_one (Fixture *fx, gconstpointer unused)
{
  /* An absent field means the video predates versioning, which the contract
   * defines as layout 1 -- readable, not an error. */
  g_autofree char *vdir = g_build_filename (
      fx->root, "Chan", "Chan - 20240101 - abcdefghijk - T", NULL);
  g_autofree char *mpath =
      g_build_filename (vdir, "Video metadata", "manifest.json", NULL);
  write_file (mpath, "{ \"video_id\": \"abcdefghijk\" }\n");

  g_autofree char *media =
      g_build_filename (vdir, "Final files", "Final Video.mkv", NULL);
  write_file (media, "x");

  YtdlIndex *idx = scan (fx);
  const YtdlEntry *e = only_entry (idx);
  g_assert_cmpuint (e->layout_version, ==, 0);
  g_assert_false (e->layout_too_new);
  g_assert_cmpint (ytdl_entry_media_index (e), >=, 0);
  ytdl_index_free (idx);
}

static void
test_flags_newer_layout (Fixture *fx, gconstpointer unused)
{
  g_autofree char *vdir = g_build_filename (
      fx->root, "Chan", "Chan - 20240101 - abcdefghijk - T", NULL);
  g_autofree char *mpath =
      g_build_filename (vdir, "Video metadata", "manifest.json", NULL);
  g_autofree char *manifest = g_strdup_printf (
      "{ \"archive_layout_version\": %d }\n", YTDL_SUPPORTED_ARCHIVE_LAYOUT + 1);
  write_file (mpath, manifest);

  YtdlIndex *idx = scan (fx);
  const YtdlEntry *e = only_entry (idx);
  /* Flagged, but still PRESENT. An empty library with no explanation is the
   * outcome the whole contract exists to prevent. */
  g_assert_true (e->layout_too_new);
  ytdl_index_free (idx);
}

static void
test_channel_info_is_not_a_video (Fixture *fx, gconstpointer unused)
{
  g_autofree char *v =
      make_video (fx, "Chan", "Chan - 20240101 - abcdefghijk - T", NULL, NULL);
  (void) v;
  g_autofree char *ci =
      g_build_filename (fx->root, "Chan", "Channel Info", "avatar.png", NULL);
  write_file (ci, "not a video");

  YtdlIndex *idx = scan (fx);
  g_assert_cmpuint (idx->entries->len, ==, 1);
  ytdl_index_free (idx);
}

static void
test_path_for_index_stays_inside (Fixture *fx, gconstpointer unused)
{
  g_autofree char *v =
      make_video (fx, "Chan", "Chan - 20240101 - abcdefghijk - T", NULL, NULL);
  g_autofree char *media =
      g_build_filename (v, "Final files", "Final Video.mkv", NULL);
  write_file (media, "x");

  YtdlIndex *idx = scan (fx);
  const YtdlEntry *e = only_entry (idx);

  gssize mi = ytdl_entry_media_index (e);
  g_autofree char *path = ytdl_entry_path_for_index (e, (gsize) mi);
  g_assert_nonnull (path);
  g_assert_true (g_str_has_prefix (path, e->dir));
  g_assert_true (g_file_test (path, G_FILE_TEST_IS_REGULAR));

  /* Out of range returns NULL rather than reading past the array. */
  g_assert_null (ytdl_entry_path_for_index (e, e->files->len + 99));

  ytdl_index_free (idx);
}

static void
test_scan_reports_a_missing_root (Fixture *fx, gconstpointer unused)
{
  YtdlIndex *idx = ytdl_index_new ();
  GError *error = NULL;
  g_autofree char *missing =
      g_build_filename (fx->tmpdir, "no-such-archive", NULL);

  g_assert_false (ytdl_index_scan (idx, missing, NULL, NULL, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  ytdl_index_free (idx);
}

/* Not a fixture test: pure parsing, and the case that motivated anchoring the
 * parse on the date+id shape rather than splitting into four fields. */
static void
test_folder_name_parsing (void)
{
  char *uploader = NULL, *date = NULL, *id = NULL, *title = NULL;

  g_assert_true (ytdl_parse_folder_name (
      "A - B - 20240101 - abcdefghijk - T - U", &uploader, &date, &id,
      &title));
  g_assert_cmpstr (uploader, ==, "A - B");
  g_assert_cmpstr (date, ==, "20240101");
  g_assert_cmpstr (id, ==, "abcdefghijk");
  g_assert_cmpstr (title, ==, "T - U");
  g_free (uploader);
  g_free (date);
  g_free (id);
  g_free (title);

  /* Nothing resembling a date and an id: the caller falls back to the whole
   * folder name, so this must say so rather than half-fill the outputs. */
  g_assert_false (
      ytdl_parse_folder_name ("just a folder", NULL, NULL, NULL, NULL));
}

/* CLI_VERSION's REQUIRES_ARCHIVE_LAYOUT and archive.h's
 * YTDL_SUPPORTED_ARCHIVE_LAYOUT are two copies of one fact. The pin is what
 * an installer and a human read; the constant is what actually decides,
 * per video, whether a folder is rendered or flagged. Them disagreeing is
 * how an app ends up claiming to read a layout it does not.
 *
 * The path is baked in by meson because CLI_VERSION belongs to the
 * repository, not to this app -- all three apps assert against the same
 * number, which is the point of there being only one. */
static void
test_pin_matches_supported_layout (void)
{
#ifndef YTDL_CLI_VERSION_PATH
  g_test_skip ("built without YTDL_CLI_VERSION_PATH");
#else
  g_autofree char *text = NULL;
  GError *error = NULL;
  if (!g_file_get_contents (YTDL_CLI_VERSION_PATH, &text, NULL, &error))
    {
      g_test_message ("cannot read %s: %s", YTDL_CLI_VERSION_PATH,
                      error->message);
      g_clear_error (&error);
      g_test_fail ();
      return;
    }

  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  gint64 declared = -1;
  for (gsize i = 0; lines[i] != NULL; i++)
    {
      g_autofree char *line = g_strdup (g_strstrip (lines[i]));
      if (!g_str_has_prefix (line, "REQUIRES_ARCHIVE_LAYOUT="))
        continue;
      declared = g_ascii_strtoll (
          line + strlen ("REQUIRES_ARCHIVE_LAYOUT="), NULL, 10);
      break;
    }

  g_assert_cmpint (declared, >, 0);
  g_assert_cmpint (declared, ==, YTDL_SUPPORTED_ARCHIVE_LAYOUT);
#endif
}

/* Defined in test_pipeline.c -- the runner's parsers and the log tail, kept
 * in their own file but run from this one binary. */
void ytdl_register_pipeline_tests (void);

/* Defined in test_detail.c -- comment threading and the transcript parser. */
void ytdl_register_detail_tests (void);

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

#define FIX(path, fn) \
  g_test_add (path, Fixture, NULL, fixture_set_up, fn, fixture_tear_down)

  FIX ("/archive/discovers-contract-layout", test_discovers_contract_layout);
  FIX ("/archive/folder-name-fallback", test_folder_name_fallback);
  FIX ("/archive/media-any-container", test_media_found_whatever_container);
  FIX ("/archive/audio-only", test_audio_only_download);
  FIX ("/archive/no-media-is-valid", test_no_media_is_valid);
  FIX ("/archive/skips-pre-merge-streams", test_skips_pre_merge_streams);
  FIX ("/archive/video-wins-over-audio", test_video_wins_over_audio);
  FIX ("/archive/manifest-media-file-wins", test_manifest_media_file_wins);
  FIX ("/archive/layout-1-readable",
       test_missing_layout_version_is_layout_one);
  FIX ("/archive/flags-newer-layout", test_flags_newer_layout);
  FIX ("/archive/channel-info-skipped", test_channel_info_is_not_a_video);
  FIX ("/archive/path-stays-inside", test_path_for_index_stays_inside);
  FIX ("/archive/missing-root-errors", test_scan_reports_a_missing_root);

#undef FIX

  g_test_add_func ("/archive/folder-name-parsing", test_folder_name_parsing);
  g_test_add_func ("/archive/pin-matches-supported-layout",
                   test_pin_matches_supported_layout);

  ytdl_register_pipeline_tests ();
  ytdl_register_detail_tests ();

  return g_test_run ();
}
