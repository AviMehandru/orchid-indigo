/* What the queue announces, and when -- notify.h's rules as fixtures.
 *
 * THE SAME VALUES are asserted by macos-swiftui/Tests/NoticeTests.swift and
 * windows-winui/YtdlWin.Tests/NoticeTests.cs. A change to a title or a rule
 * here that is not made there is three apps announcing the same queue three
 * different ways, so the strings are spelled out in full rather than matched
 * loosely.
 *
 * The last test is not a fixture: it drives the real runner, to pin the one
 * thing the tracker cannot check for itself -- that "nothing left to run" is
 * never reported while a run is about to start.
 */

#include "notify.h"
#include "pipeline.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdarg.h>
#include <string.h>

void ytdl_register_notify_tests (void);

static YtdlRunRecord *
rec (const char *id, const char *state, const char *last_line,
     gint64 touched, gint64 skipped, gint64 errors)
{
  YtdlRunRecord *r = g_new0 (YtdlRunRecord, 1);
  r->id = g_strdup (id);
  r->state = g_strdup (state);
  r->opts = ytdl_run_options_new ();
  r->opts->url = g_strdup_printf ("https://youtu.be/%s", id);
  r->command = g_strdup_printf ("ytdl %s", r->opts->url);
  r->last_line = g_strdup (last_line != NULL ? last_line : "");
  r->videos_touched = touched;
  r->archive_skipped = skipped;
  r->errors = errors;
  r->warnings = 0;
  r->exit_code = g_strcmp0 (state, "failed") == 0 ? 1 : 0;
  return r;
}

/* The runner's history is newest first; so are these. Takes ownership. */
static GPtrArray *
history (YtdlRunRecord *first, ...)
{
  GPtrArray *h =
      g_ptr_array_new_with_free_func ((GDestroyNotify) ytdl_run_record_free);
  va_list ap;
  va_start (ap, first);
  for (YtdlRunRecord *r = first; r != NULL; r = va_arg (ap, YtdlRunRecord *))
    g_ptr_array_add (h, r);
  va_end (ap);
  return h;
}

static void
assert_notice (YtdlNotice *n, const char *title, const char *body,
               gboolean failure)
{
  g_assert_nonnull (n);
  g_assert_cmpstr (n->title, ==, title);
  g_assert_cmpstr (n->body, ==, body);
  g_assert_cmpint (n->failure, ==, failure);
}

static void
test_single_done (void)
{
  g_autoptr (YtdlNoticeTracker) t = ytdl_notice_tracker_new (NULL);
  g_autoptr (GPtrArray) h =
      history (rec ("aaaaaaaaaaa", "done", "", 3, 12, 0), NULL);
  g_autoptr (YtdlNotice) n = ytdl_notice_tracker_update (t, h, 0);
  assert_notice (n, "Download finished",
                 "https://youtu.be/aaaaaaaaaaa\n"
                 "3 touched, 12 already archived",
                 FALSE);
}

static void
test_single_failed (void)
{
  g_autoptr (YtdlNoticeTracker) t = ytdl_notice_tracker_new (NULL);
  g_autoptr (GPtrArray) h = history (
      rec ("bbbbbbbbbbb", "failed",
           "ERROR: [youtube] bbbbbbbbbbb: Private video", -1, -1, -1),
      NULL);
  g_autoptr (YtdlNotice) n = ytdl_notice_tracker_update (t, h, 0);
  assert_notice (n, "Download failed",
                 "https://youtu.be/bbbbbbbbbbb\n"
                 "ERROR: [youtube] bbbbbbbbbbb: Private video",
                 TRUE);
}

/* Three runs, one at a time, the middle one failing: silence, then the
 * failure at once, then one summary that repeats it. */
static void
test_queue_of_three (void)
{
  g_autoptr (YtdlNoticeTracker) t = ytdl_notice_tracker_new (NULL);

  g_autoptr (GPtrArray) h1 = history (rec ("c1", "done", "", 1, 0, 0), NULL);
  g_autoptr (YtdlNotice) n1 = ytdl_notice_tracker_update (t, h1, 2);
  g_assert_null (n1);

  g_autoptr (GPtrArray) h2 =
      history (rec ("c2", "failed", "ERROR: HTTP Error 403: Forbidden", 0, 0, 1),
               rec ("c1", "done", "", 1, 0, 0), NULL);
  g_autoptr (YtdlNotice) n2 = ytdl_notice_tracker_update (t, h2, 1);
  assert_notice (n2, "A download failed",
                 "https://youtu.be/c2\n"
                 "ERROR: HTTP Error 403: Forbidden\n"
                 "The queue goes on: 1 still to run.",
                 TRUE);

  /* Nothing new, still running: nothing to say, and the failure is not
   * announced a second time. */
  g_autoptr (YtdlNotice) again = ytdl_notice_tracker_update (t, h2, 1);
  g_assert_null (again);

  g_autoptr (GPtrArray) h3 =
      history (rec ("c3", "done", "", 2, 5, 0),
               rec ("c2", "failed", "ERROR: HTTP Error 403: Forbidden", 0, 0, 1),
               rec ("c1", "done", "", 1, 0, 0), NULL);
  g_autoptr (YtdlNotice) n3 = ytdl_notice_tracker_update (t, h3, 0);
  assert_notice (n3, "Queue finished: 2 done, 1 failed",
                 "3 touched, 5 already archived, 1 error\n"
                 "Last failure: https://youtu.be/c2 — "
                 "ERROR: HTTP Error 403: Forbidden",
                 TRUE);
}

static void
test_second_failure_counts (void)
{
  g_autoptr (YtdlNoticeTracker) t = ytdl_notice_tracker_new (NULL);
  g_autoptr (GPtrArray) h1 =
      history (rec ("f1", "failed", "ERROR: one", -1, -1, -1), NULL);
  g_autoptr (YtdlNotice) n1 = ytdl_notice_tracker_update (t, h1, 3);
  g_assert_cmpstr (n1->title, ==, "A download failed");

  g_autoptr (GPtrArray) h2 =
      history (rec ("f2", "failed", "ERROR: two", -1, -1, -1),
               rec ("f1", "failed", "ERROR: one", -1, -1, -1), NULL);
  g_autoptr (YtdlNotice) n2 = ytdl_notice_tracker_update (t, h2, 2);
  assert_notice (n2, "2 downloads failed so far",
                 "https://youtu.be/f2\nERROR: two\n"
                 "The queue goes on: 2 still to run.",
                 TRUE);
}

/* A run finishing and the queue going idle inside one drain tick: the
 * summary only, never a failure notice followed by a summary. */
static void
test_coalesced_tick (void)
{
  g_autoptr (YtdlNoticeTracker) t = ytdl_notice_tracker_new (NULL);
  g_autoptr (GPtrArray) h =
      history (rec ("d2", "failed", "ERROR: gone", -1, -1, -1),
               rec ("d1", "done", "", 4, 0, 0), NULL);
  g_autoptr (YtdlNotice) n = ytdl_notice_tracker_update (t, h, 0);
  assert_notice (n, "Queue finished: 1 done, 1 failed",
                 "4 touched, 0 already archived\n"
                 "Last failure: https://youtu.be/d2 — ERROR: gone",
                 TRUE);
}

static void
test_cancelled_only_is_silent (void)
{
  g_autoptr (YtdlNoticeTracker) t = ytdl_notice_tracker_new (NULL);
  g_autoptr (GPtrArray) h1 =
      history (rec ("e1", "cancelled", "", -1, -1, -1), NULL);
  g_autoptr (YtdlNotice) n1 = ytdl_notice_tracker_update (t, h1, 0);
  g_assert_null (n1);

  /* ...and the queue it ended is closed: the next one does not count it. */
  g_autoptr (GPtrArray) h2 =
      history (rec ("e2", "done", "", 1, 0, 0),
               rec ("e1", "cancelled", "", -1, -1, -1), NULL);
  g_autoptr (YtdlNotice) n2 = ytdl_notice_tracker_update (t, h2, 0);
  assert_notice (n2, "Download finished",
                 "https://youtu.be/e2\n1 touched, 0 already archived", FALSE);
}

static void
test_cancelled_is_named_in_a_queue (void)
{
  g_autoptr (YtdlNoticeTracker) t = ytdl_notice_tracker_new (NULL);
  g_autoptr (GPtrArray) h =
      history (rec ("g2", "cancelled", "", -1, -1, -1),
               rec ("g1", "done", "", -1, -1, -1), NULL);
  g_autoptr (YtdlNotice) n = ytdl_notice_tracker_update (t, h, 0);
  /* No run printed a session summary, so there are no counts to give --
   * and "0 touched" would read as "it ran and found nothing". */
  assert_notice (n, "Queue finished: 1 done, 1 cancelled", "Nothing failed.",
                 FALSE);
}

static void
test_restored_history_is_not_announced (void)
{
  g_autoptr (GPtrArray) restored =
      history (rec ("r1", "failed", "Interrupted", -1, -1, -1), NULL);
  g_autoptr (YtdlNoticeTracker) t = ytdl_notice_tracker_new (restored);
  g_autoptr (YtdlNotice) n = ytdl_notice_tracker_update (t, restored, 0);
  g_assert_null (n);
}

static void
test_failure_reasons (void)
{
  g_autoptr (YtdlNoticeTracker) t = ytdl_notice_tracker_new (NULL);

  YtdlRunRecord *silent = rec ("h1", "failed", "", -1, -1, -1);
  silent->exit_code = 2;
  g_autoptr (GPtrArray) h1 = history (silent, NULL);
  g_autoptr (YtdlNotice) n1 = ytdl_notice_tracker_update (t, h1, 0);
  g_assert_cmpstr (n1->body, ==,
                   "https://youtu.be/h1\nytdl exited with code 2.");

  YtdlRunRecord *killed = rec ("h2", "failed", "  \n", -1, -1, -1);
  killed->exit_code = -1;
  YtdlRunRecord *prev = rec ("h1", "failed", "", -1, -1, -1);
  prev->exit_code = 2;
  g_autoptr (GPtrArray) h2 = history (killed, prev, NULL);
  g_autoptr (YtdlNotice) n2 = ytdl_notice_tracker_update (t, h2, 0);
  g_assert_cmpstr (n2->body, ==,
                   "https://youtu.be/h2\n"
                   "It printed no error before it stopped.");
}

/* A record from a store written before options were saved has no URL; the
 * command stands in for it. */
static void
test_target_falls_back_to_command (void)
{
  g_autoptr (YtdlNoticeTracker) t = ytdl_notice_tracker_new (NULL);
  YtdlRunRecord *r = rec ("i1", "done", "", -1, -1, -1);
  g_clear_pointer (&r->opts, ytdl_run_options_free);
  g_autoptr (GPtrArray) h = history (r, NULL);
  g_autoptr (YtdlNotice) n = ytdl_notice_tracker_update (t, h, 0);
  assert_notice (n, "Download finished", "ytdl https://youtu.be/i1", FALSE);
}

static void
test_clip (void)
{
  g_autofree char *x200 = g_strnfill (200, 'x');
  g_autofree char *c1 = ytdl_notice_clip (x200, 160);
  g_assert_cmpint (g_utf8_strlen (c1, -1), ==, 160);
  g_assert_true (g_str_has_suffix (c1, "x…"));

  /* Characters, not bytes: a cut in the middle of a two-byte character would
   * be invalid UTF-8, which a notification daemon may refuse outright. */
  GString *e = g_string_new (NULL);
  for (int i = 0; i < 200; i++)
    g_string_append (e, "é");
  g_autofree char *c2 = ytdl_notice_clip (e->str, 160);
  g_string_free (e, TRUE);
  g_assert_true (g_utf8_validate (c2, -1, NULL));
  g_assert_cmpint (g_utf8_strlen (c2, -1), ==, 160);

  /* A reason's own line breaks would scramble the body's. */
  g_autofree char *c3 = ytdl_notice_clip ("  ERROR: a\nb\tc  ", 160);
  g_assert_cmpstr (c3, ==, "ERROR: a b c");

  /* No "word …": the space before the cut goes. */
  g_autofree char *c4 = ytdl_notice_clip ("abc defgh", 5);
  g_assert_cmpstr (c4, ==, "abc…");

  g_autofree char *c5 = ytdl_notice_clip ("short", 160);
  g_assert_cmpstr (c5, ==, "short");
}

/* ---------------------------------------------------------------------- */
/* The runner's side of it                                                */
/* ---------------------------------------------------------------------- */

typedef struct
{
  guint    polls;
  guint    premature; /* remaining == 0 before every run was in history */
  guint    expected;
  gboolean finished;
} PollState;

/* Polled from the main loop as fast as it will go, racing the worker. */
static gboolean
poll_settled (gpointer user_data)
{
  YtdlRunner *runner = g_object_get_data (G_OBJECT (user_data), "runner");
  PollState *ps = g_object_get_data (G_OBJECT (user_data), "state");
  guint remaining = 99;
  g_autoptr (GPtrArray) h = ytdl_runner_settled (runner, &remaining);
  ps->polls++;
  if (remaining == 0)
    {
      if (h->len < ps->expected)
        ps->premature++;
      else
        ps->finished = TRUE;
    }
  return G_SOURCE_CONTINUE;
}

/* The queue must never read as finished while a run is between leaving the
 * queue and being recorded as current -- that gap is real (the worker takes
 * the item, drops the lock, and only then does run_one set current), and a
 * summary sent from inside it would announce a queue that is still going.
 *
 * Driven with runs that fail at once: the install root points at an empty
 * directory, so each run stops at "ytdl.ps1 does not exist" (or at "pwsh not
 * found", on a machine without it) having gone through exactly the steal /
 * finish path a real run does, and without spawning anything. Forty of them
 * so the worker is busy stealing for the whole of the poll.
 *
 * The gap is narrow, so this is probabilistic in the direction that matters:
 * it cannot fail on correct code, and it catches the bug most runs rather
 * than every one. When it was written, counting `current != NULL` instead of
 * the in-flight flag failed it in 3 of 5 runs, each time on the LAST item --
 * the queue empty, the run taken off it, current not yet set. */
static void
test_runner_never_reads_finished_early (void)
{
  g_autofree char *dir = g_dir_make_tmp ("ytdl-notify-XXXXXX", NULL);
  g_autofree char *old_config = g_strdup (g_getenv ("XDG_CONFIG_HOME"));
  g_autofree char *old_root = g_strdup (g_getenv ("YTDLP_INSTALL_ROOT"));
  g_setenv ("XDG_CONFIG_HOME", dir, TRUE);
  g_setenv ("YTDLP_INSTALL_ROOT", dir, TRUE);

  g_autoptr (YtdlRunner) runner = ytdl_runner_new ();
  guint remaining = 99;
  g_autoptr (GPtrArray) h0 = ytdl_runner_settled (runner, &remaining);
  g_assert_cmpuint (remaining, ==, 0);
  g_assert_cmpuint (h0->len, ==, 0);

  const guint n = 40;
  ytdl_runner_set_paused (runner, TRUE);
  g_autoptr (YtdlRunOptions) o = ytdl_run_options_new ();
  o->url = g_strdup ("https://youtu.be/abcdefghijk");
  for (guint i = 0; i < n; i++)
    g_free (ytdl_runner_enqueue (runner, o, NULL));

  /* Paused with runs waiting is not finished. */
  g_autoptr (GPtrArray) h1 = ytdl_runner_settled (runner, &remaining);
  g_assert_cmpuint (remaining, ==, n);

  PollState ps = { 0 };
  ps.expected = n;
  g_autoptr (GObject) carrier = g_object_new (G_TYPE_OBJECT, NULL);
  g_object_set_data (carrier, "runner", runner);
  g_object_set_data (carrier, "state", &ps);
  guint src = g_idle_add (poll_settled, carrier);

  ytdl_runner_start (runner);
  ytdl_runner_set_paused (runner, FALSE);

  gint64 deadline = g_get_monotonic_time () + 20 * G_TIME_SPAN_SECOND;
  while (!ps.finished && g_get_monotonic_time () < deadline)
    g_main_context_iteration (NULL, FALSE);
  g_source_remove (src);
  ytdl_runner_stop (runner);

  g_assert_true (ps.finished);
  g_assert_cmpuint (ps.premature, ==, 0);
  g_autoptr (GPtrArray) h2 = ytdl_runner_settled (runner, &remaining);
  g_assert_cmpuint (h2->len, ==, n);
  g_assert_cmpuint (remaining, ==, 0);
  for (guint i = 0; i < h2->len; i++)
    g_assert_cmpstr (((YtdlRunRecord *) g_ptr_array_index (h2, i))->state, ==,
                     "failed");

  if (old_config != NULL)
    g_setenv ("XDG_CONFIG_HOME", old_config, TRUE);
  else
    g_unsetenv ("XDG_CONFIG_HOME");
  if (old_root != NULL)
    g_setenv ("YTDLP_INSTALL_ROOT", old_root, TRUE);
  else
    g_unsetenv ("YTDLP_INSTALL_ROOT");

  g_autofree char *state = g_build_filename (dir, "ytdl-gtk", NULL);
  g_autofree char *q = g_build_filename (state, "queue.json", NULL);
  g_autofree char *hist = g_build_filename (state, "history.json", NULL);
  g_unlink (q);
  g_unlink (hist);
  g_rmdir (state);
  g_rmdir (dir);
}

void
ytdl_register_notify_tests (void)
{
  g_test_add_func ("/notify/single-done", test_single_done);
  g_test_add_func ("/notify/single-failed", test_single_failed);
  g_test_add_func ("/notify/queue-of-three", test_queue_of_three);
  g_test_add_func ("/notify/second-failure-counts", test_second_failure_counts);
  g_test_add_func ("/notify/coalesced-tick", test_coalesced_tick);
  g_test_add_func ("/notify/cancelled-only-is-silent",
                   test_cancelled_only_is_silent);
  g_test_add_func ("/notify/cancelled-is-named-in-a-queue",
                   test_cancelled_is_named_in_a_queue);
  g_test_add_func ("/notify/restored-history-not-announced",
                   test_restored_history_is_not_announced);
  g_test_add_func ("/notify/failure-reasons", test_failure_reasons);
  g_test_add_func ("/notify/target-falls-back-to-command",
                   test_target_falls_back_to_command);
  g_test_add_func ("/notify/clip", test_clip);
  g_test_add_func ("/notify/runner-never-reads-finished-early",
                   test_runner_never_reads_finished_early);
}
