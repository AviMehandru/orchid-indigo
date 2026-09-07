/* Tests for the download runner's pure parts.
 *
 * Three things are covered here, and they are the three that would break
 * silently rather than loudly:
 *
 *   1. Argument building. A flag emitted when it should not be, or a default
 *      leaking into the command line, changes what ends up in the archive.
 *   2. Progress parsing. yt-dlp's progress line is not an API; a change in it
 *      makes the bar stop moving with no error anywhere.
 *   3. The session summary. Those four counts exist ONLY in that one line of
 *      run_ytdlp.ps1 output. Nothing else in the system reports them.
 *
 * The runner's spawn/pump/cancel path is exercised separately, end to end,
 * against a fake ytdl.ps1 -- see tests/fake-pipeline. Testing it here would
 * mean asserting on a real download.
 */

#include "health.h"
#include "pipeline.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

void ytdl_register_pipeline_tests (void);

/* Join args with '|' so a test failure prints the whole command line rather
 * than "expected 4, got 6". */
static char *
args_of (const YtdlRunOptions *o)
{
  g_auto (GStrv) v = ytdl_run_options_to_args (o);
  return g_strjoinv ("|", v);
}

static void
test_normalize_url (void)
{
  g_autofree char *bare = ytdl_normalize_url ("dQw4w9WgXcQ");
  g_assert_cmpstr (bare, ==, "https://www.youtube.com/watch?v=dQw4w9WgXcQ");

  /* About one YouTube id in thirty starts with "-" or "_", and a bare one
   * would be bound as a parameter by the shell before ytdl.ps1 ever saw it.
   * Normalising is what removes that whole class. */
  g_autofree char *dashy = ytdl_normalize_url ("-dQw4w9WgXc");
  g_assert_cmpstr (dashy, ==, "https://www.youtube.com/watch?v=-dQw4w9WgXc");

  g_autofree char *full =
      ytdl_normalize_url ("https://www.youtube.com/watch?v=abc");
  g_assert_cmpstr (full, ==, "https://www.youtube.com/watch?v=abc");

  g_autofree char *schemeless = ytdl_normalize_url ("youtu.be/abcdefghijk");
  g_assert_cmpstr (schemeless, ==, "https://youtu.be/abcdefghijk");

  /* Not a validator. A channel name is passed through untouched, because
   * yt-dlp's extractor is the real authority on what is downloadable. */
  g_autofree char *odd = ytdl_normalize_url ("  not a url  ");
  g_assert_cmpstr (odd, ==, "not a url");
}

static void
test_defaults_emit_nothing (void)
{
  g_autoptr (YtdlRunOptions) o = ytdl_run_options_new ();
  o->url = g_strdup ("https://example.com/v");
  /* Exactly what the UI hands over when nothing has been touched. */
  o->mode = g_strdup ("full");
  o->quality = g_strdup ("best");
  o->codec = g_strdup ("any");
  o->audio_codec = g_strdup ("any");
  o->container = g_strdup ("mkv");
  o->workers = 1;

  g_autofree char *joined = args_of (o);
  /* A plain download must produce the command line it produced before any of
   * these options existed. --mode full and friends are no-ops the pipeline
   * would accept and ignore, but emitting them would put four extra flags in
   * the preview of every ordinary download. */
  g_assert_cmpstr (joined, ==, "https://example.com/v");
}

static void
test_non_defaults_emit (void)
{
  g_autoptr (YtdlRunOptions) o = ytdl_run_options_new ();
  o->url = g_strdup ("https://example.com/v");
  o->mode = g_strdup ("audio-only");
  o->quality = g_strdup ("1080");
  o->codec = g_strdup ("vp9");
  o->audio_codec = g_strdup ("opus");
  o->container = g_strdup ("webm");
  o->sync = TRUE;
  o->lazy = TRUE;
  o->no_comments = TRUE;
  o->workers = 4;

  g_autofree char *joined = args_of (o);
  g_assert_cmpstr (joined, ==,
                   "https://example.com/v|--sync|--lazy|--workers|4|"
                   "--mode|audio-only|--quality|1080|--codec|vp9|"
                   "--audio-codec|opus|--container|webm|--no-comments");
}

static void
test_workers_only_above_one (void)
{
  g_autoptr (YtdlRunOptions) o = ytdl_run_options_new ();
  o->url = g_strdup ("u");

  o->workers = 1;
  g_autofree char *one = args_of (o);
  g_assert_null (strstr (one, "--workers"));

  o->workers = 2;
  g_autofree char *two = args_of (o);
  g_assert_nonnull (strstr (two, "--workers|2"));
}

static void
test_passthrough_args_repeat (void)
{
  g_autoptr (YtdlRunOptions) o = ytdl_run_options_new ();
  o->url = g_strdup ("u");
  g_ptr_array_add (o->ytdlp_args, g_strdup ("--match-filter"));
  /* Contains a comma AND a space -- which is exactly why these are repeated
   * rather than joined on any separator. */
  g_ptr_array_add (o->ytdlp_args, g_strdup ("duration > 60 & view_count > 10"));
  g_ptr_array_add (o->ytdlp_args, g_strdup ("   "));

  g_autofree char *joined = args_of (o);
  g_assert_cmpstr (joined, ==,
                   "u|--ytdlp-arg|--match-filter|"
                   "--ytdlp-arg|duration > 60 & view_count > 10");
}

static void
test_data_root_tilde_is_expanded (void)
{
  g_autoptr (YtdlRunOptions) o = ytdl_run_options_new ();
  o->url = g_strdup ("u");
  o->data_root = g_strdup ("~/Videos");

  g_auto (GStrv) v = ytdl_run_options_to_args (o);
  g_assert_cmpstr (v[1], ==, "--path");

  /* The bug this pins: run_ytdlp.ps1 resolves -DataRoot with GetFullPath,
   * which has no notion of a home directory, so an unexpanded "~/Videos"
   * reached it as a literal "~" folder under the pipeline's working
   * directory -- while this app's own expansion pointed somewhere else. The
   * library indexed one folder and the downloads went to another, with
   * nothing reporting an error. */
  g_assert_true (g_path_is_absolute (v[2]));
  g_assert_null (strchr (v[2], '~'));
  g_assert_true (g_str_has_suffix (v[2], "/Videos"));
}

static void
test_command_preview_quotes (void)
{
  g_autoptr (YtdlRunOptions) o = ytdl_run_options_new ();
  o->url = g_strdup ("https://example.com/v");
  g_ptr_array_add (o->ytdlp_args, g_strdup ("a b"));

  g_autofree char *cmd = ytdl_run_options_command_preview (o);
  /* The URL is always quoted; anything with a space is quoted; nothing else
   * is. What is shown must be pasteable into a terminal as-is. */
  g_assert_cmpstr (cmd, ==,
                   "ytdl \"https://example.com/v\" --ytdlp-arg \"a b\"");
}

static void
test_strip_ansi (void)
{
  g_autofree char *plain =
      ytdl_strip_ansi ("\x1b[0;32m[download]\x1b[0m  12.0%");
  g_assert_cmpstr (plain, ==, "[download]  12.0%");

  g_autofree char *untouched = ytdl_strip_ansi ("no escapes here");
  g_assert_cmpstr (untouched, ==, "no escapes here");
}

static void
test_progress_line (void)
{
  YtdlProgress p = { 0 };
  p.percent = -1.0;

  g_assert_true (ytdl_parse_progress_line (
      "[download]  45.2% of  120.00MiB at   2.00MiB/s ETA 00:30", &p));
  g_assert_cmpfloat (p.percent, >, 45.0);
  g_assert_cmpfloat (p.percent, <, 45.5);
  g_assert_cmpstr (p.total, ==, "120.00MiB");
  g_assert_cmpstr (p.speed, ==, "2.00MiB/s");
  g_assert_cmpstr (p.eta, ==, "00:30");
  g_assert_cmpstr (p.stage, ==, "downloading");

  g_assert_true (ytdl_parse_progress_line (
      "[download] Destination: Final Video.f137.mp4", &p));
  g_assert_cmpstr (p.destination, ==, "Final Video.f137.mp4");

  /* A stage marker clears the percentage: 45% of the download is not 45% of
   * the merge, and leaving the old number up reads as a stalled bar. */
  g_assert_true (ytdl_parse_progress_line ("[Merger] Merging formats", &p));
  g_assert_cmpstr (p.stage, ==, "merging");
  g_assert_cmpfloat (p.percent, <, 0.0);

  g_assert_true (ytdl_parse_progress_line (
      "[youtube] dQw4w9WgXcQ: Downloading webpage", &p));
  g_assert_cmpstr (p.video_id, ==, "dQw4w9WgXcQ");

  /* Ordinary chatter changes nothing. */
  g_assert_false (ytdl_parse_progress_line ("some unrelated output", &p));

  ytdl_progress_clear (&p);
}

static void
test_session_summary (void)
{
  gint64 v = -1, s = -1, e = -1, w = -1;
  g_assert_true (ytdl_parse_session_summary (
      "-- Session summary: 12 video(s) touched, 3 already archived (skipped), "
      "1 error(s), 2 warning(s) --",
      &v, &s, &e, &w));
  g_assert_cmpint (v, ==, 12);
  g_assert_cmpint (s, ==, 3);
  g_assert_cmpint (e, ==, 1);
  g_assert_cmpint (w, ==, 2);

  /* Zeroes are a real answer and must parse, not be mistaken for absence. */
  g_assert_true (ytdl_parse_session_summary (
      "-- Session summary: 0 video(s) touched, 0 already archived (skipped), "
      "0 error(s), 0 warning(s) --",
      &v, &s, &e, &w));
  g_assert_cmpint (v, ==, 0);

  g_assert_false (
      ytdl_parse_session_summary ("[download] 100% of 1.00MiB", &v, &s, &e, &w));
  g_assert_false (ytdl_parse_session_summary ("Session summary: incomplete 1 2",
                                              &v, &s, &e, &w));
}

/* ---------------------------------------------------------------------- */

static void
test_log_tail (void)
{
  g_autofree char *dir = g_dir_make_tmp ("ytdl-tail-XXXXXX", NULL);
  g_assert_nonnull (dir);
  g_autofree char *path = g_build_filename (dir, "download.log", NULL);

  GString *body = g_string_new (NULL);
  for (int i = 1; i <= 500; i++)
    g_string_append_printf (body, "line %d\n", i);
  g_assert_true (g_file_set_contents (path, body->str, -1, NULL));
  g_string_free (body, TRUE);

  g_autofree char *tail = ytdl_health_log_tail (path, 3);
  g_assert_cmpstr (tail, ==, "line 498\nline 499\nline 500");

  /* Asking for more lines than exist yields the file, not a crash, and no
   * leading blank from the trailing newline. */
  g_autofree char *all = ytdl_health_log_tail (path, 10000);
  g_assert_true (g_str_has_prefix (all, "line 1\n"));
  g_assert_true (g_str_has_suffix (all, "line 500"));

  g_autofree char *missing =
      g_build_filename (dir, "does-not-exist.log", NULL);
  g_autofree char *none = ytdl_health_log_tail (missing, 10);
  g_assert_cmpstr (none, ==, "");

  g_unlink (path);
  g_rmdir (dir);
}

void
ytdl_register_pipeline_tests (void)
{
  g_test_add_func ("/pipeline/normalize-url", test_normalize_url);
  g_test_add_func ("/pipeline/defaults-emit-nothing", test_defaults_emit_nothing);
  g_test_add_func ("/pipeline/non-defaults-emit", test_non_defaults_emit);
  g_test_add_func ("/pipeline/workers-only-above-one",
                   test_workers_only_above_one);
  g_test_add_func ("/pipeline/passthrough-repeats", test_passthrough_args_repeat);
  g_test_add_func ("/pipeline/data-root-tilde", test_data_root_tilde_is_expanded);
  g_test_add_func ("/pipeline/command-preview", test_command_preview_quotes);
  g_test_add_func ("/pipeline/strip-ansi", test_strip_ansi);
  g_test_add_func ("/pipeline/progress-line", test_progress_line);
  g_test_add_func ("/pipeline/session-summary", test_session_summary);
  g_test_add_func ("/health/log-tail", test_log_tail);
}
