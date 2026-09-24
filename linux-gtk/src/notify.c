/* The rules are in notify.h. This is only their arithmetic. */

#include "notify.h"

#include <string.h>

struct _YtdlNoticeTracker
{
  /* Every run id already accounted for. Rebuilt from the history on each
   * update, so it never holds more than the history does: an id that has
   * aged out of the capped history cannot come back, so there is nothing to
   * remember it for. */
  GHashTable *seen;

  /* The queue in progress. Reset when it ends. */
  guint    done;
  guint    failed;
  guint    cancelled;
  gboolean have_counts; /* any run reported a session summary */
  gint64   touched;
  gint64   skipped;
  gint64   errors;
  /* For a one-run queue's summary, and for naming the latest failure. */
  char    *last_done_target;
  char    *last_failure_target;
  char    *last_failure_reason;
};

void
ytdl_notice_free (YtdlNotice *n)
{
  if (n == NULL)
    return;
  g_free (n->title);
  g_free (n->body);
  g_free (n);
}

/* Whitespace trimmed, then cut at @max_chars CHARACTERS -- not bytes, which
 * would split a multi-byte title in half -- with an ellipsis standing in for
 * what was dropped. Newlines inside become spaces: the body's own line breaks
 * are what separate the target from the reason, and a reason that brought
 * its own would scramble that. */
char *
ytdl_notice_clip (const char *text, glong max_chars)
{
  if (text == NULL)
    return g_strdup ("");

  g_autofree char *valid = g_utf8_make_valid (text, -1);
  g_strstrip (valid);
  for (char *p = valid; *p; p++)
    if (*p == '\n' || *p == '\r' || *p == '\t')
      *p = ' ';

  if (g_utf8_strlen (valid, -1) <= max_chars)
    return g_strdup (valid);

  /* One character is given to the ellipsis, so the result is never longer
   * than asked for. Trailing space before it is trimmed so the cut never
   * reads "word …". */
  g_autofree char *head = g_utf8_substring (valid, 0, max_chars - 1);
  g_strchomp (head);
  return g_strconcat (head, "…", NULL);
}

/* What the run was OF, for a person reading a notification: the URL they
 * pasted, which is the thing they would recognise. The full command is the
 * fallback for a record restored from a store that predates saving options. */
static char *
run_target (const YtdlRunRecord *r)
{
  const char *t = NULL;
  if (r->opts != NULL && r->opts->url != NULL && *r->opts->url != '\0')
    t = r->opts->url;
  else if (r->command != NULL && *r->command != '\0')
    t = r->command;
  else
    t = "a run";
  return ytdl_notice_clip (t, YTDL_NOTICE_TARGET_MAX);
}

/* Why it failed. The last line the run printed is almost always the answer
 * -- yt-dlp's ERROR line, or the reason this app gave when it could not start
 * the run at all. Only a run that died silently falls back to its exit
 * code. */
static char *
run_reason (const YtdlRunRecord *r)
{
  if (r->last_line != NULL)
    {
      g_autofree char *clipped =
          ytdl_notice_clip (r->last_line, YTDL_NOTICE_REASON_MAX);
      if (*clipped != '\0')
        return g_steal_pointer (&clipped);
    }
  if (r->exit_code > 0)
    return g_strdup_printf ("ytdl exited with code %d.", r->exit_code);
  return g_strdup ("It printed no error before it stopped.");
}

YtdlNoticeTracker *
ytdl_notice_tracker_new (GPtrArray *history)
{
  YtdlNoticeTracker *t = g_new0 (YtdlNoticeTracker, 1);
  t->seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; history != NULL && i < history->len; i++)
    {
      const YtdlRunRecord *r = g_ptr_array_index (history, i);
      if (r->id != NULL)
        g_hash_table_add (t->seen, g_strdup (r->id));
    }
  return t;
}

static void
reset_queue (YtdlNoticeTracker *t)
{
  t->done = t->failed = t->cancelled = 0;
  t->have_counts = FALSE;
  t->touched = t->skipped = t->errors = 0;
  g_clear_pointer (&t->last_done_target, g_free);
  g_clear_pointer (&t->last_failure_target, g_free);
  g_clear_pointer (&t->last_failure_reason, g_free);
}

void
ytdl_notice_tracker_free (YtdlNoticeTracker *t)
{
  if (t == NULL)
    return;
  reset_queue (t);
  g_hash_table_unref (t->seen);
  g_free (t);
}

/* "3 touched, 12 already archived, 1 error" -- the pipeline's own session
 * summary, in its own words, summed over the queue. NULL when no run in it
 * printed a summary (a run that could not start, or was killed), because
 * "0 touched" would read as "it ran and found nothing". */
static char *
counts_line (const YtdlNoticeTracker *t)
{
  if (!t->have_counts)
    return NULL;
  GString *s = g_string_new (NULL);
  g_string_append_printf (s,
                          "%" G_GINT64_FORMAT " touched, %" G_GINT64_FORMAT
                          " already archived",
                          t->touched, t->skipped);
  if (t->errors > 0)
    g_string_append_printf (s, ", %" G_GINT64_FORMAT " %s", t->errors,
                            t->errors == 1 ? "error" : "errors");
  return g_string_free (s, FALSE);
}

static YtdlNotice *
summary (const YtdlNoticeTracker *t)
{
  guint runs = t->done + t->failed + t->cancelled;
  g_autofree char *counts = counts_line (t);
  YtdlNotice *n = g_new0 (YtdlNotice, 1);
  n->failure = t->failed > 0;

  /* One run: say what it was. The single-download case is the common one,
   * and "Queue finished: 1 done" would be a strange way to put it. */
  if (runs == 1)
    {
      if (t->failed == 1)
        {
          n->title = g_strdup ("Download failed");
          n->body = g_strdup_printf ("%s\n%s", t->last_failure_target,
                                     t->last_failure_reason);
        }
      else
        {
          n->title = g_strdup ("Download finished");
          n->body = counts != NULL
                        ? g_strdup_printf ("%s\n%s", t->last_done_target,
                                           counts)
                        : g_strdup (t->last_done_target);
        }
      return n;
    }

  GString *title = g_string_new ("Queue finished: ");
  const char *sep = "";
  if (t->done > 0)
    {
      g_string_append_printf (title, "%u done", t->done);
      sep = ", ";
    }
  if (t->failed > 0)
    {
      g_string_append_printf (title, "%s%u failed", sep, t->failed);
      sep = ", ";
    }
  if (t->cancelled > 0)
    g_string_append_printf (title, "%s%u cancelled", sep, t->cancelled);
  n->title = g_string_free (title, FALSE);

  GString *body = g_string_new (NULL);
  if (counts != NULL)
    g_string_append (body, counts);
  if (t->failed > 0)
    g_string_append_printf (body, "%sLast failure: %s — %s",
                            body->len > 0 ? "\n" : "",
                            t->last_failure_target, t->last_failure_reason);
  if (body->len == 0)
    g_string_append (body, "Nothing failed.");
  n->body = g_string_free (body, FALSE);
  return n;
}

YtdlNotice *
ytdl_notice_tracker_update (YtdlNoticeTracker *t, GPtrArray *history,
                            guint remaining)
{
  g_return_val_if_fail (t != NULL, NULL);

  gboolean failed_now = FALSE;

  /* Newest first in the history, so walked backwards: when two runs finish
   * inside one drain tick, the later one's failure is the one a notice
   * should name. */
  for (guint i = history != NULL ? history->len : 0; i > 0; i--)
    {
      const YtdlRunRecord *r = g_ptr_array_index (history, i - 1);
      if (r->id == NULL || g_hash_table_contains (t->seen, r->id))
        continue;

      if (g_strcmp0 (r->state, "done") == 0)
        {
          t->done++;
          g_free (t->last_done_target);
          t->last_done_target = run_target (r);
        }
      else if (g_strcmp0 (r->state, "failed") == 0)
        {
          t->failed++;
          failed_now = TRUE;
          g_free (t->last_failure_target);
          g_free (t->last_failure_reason);
          t->last_failure_target = run_target (r);
          t->last_failure_reason = run_reason (r);
        }
      else if (g_strcmp0 (r->state, "cancelled") == 0)
        t->cancelled++;

      /* -1 means the run never printed a summary; see counts_line. */
      if (r->videos_touched >= 0)
        {
          t->have_counts = TRUE;
          t->touched += r->videos_touched;
          t->skipped += MAX (r->archive_skipped, 0);
          t->errors += MAX (r->errors, 0);
        }
    }

  g_hash_table_remove_all (t->seen);
  for (guint i = 0; history != NULL && i < history->len; i++)
    {
      const YtdlRunRecord *r = g_ptr_array_index (history, i);
      if (r->id != NULL)
        g_hash_table_add (t->seen, g_strdup (r->id));
    }

  if (remaining == 0)
    {
      YtdlNotice *n = NULL;
      /* Rule 4: a queue that only ever got cancelled ends silently. */
      if (t->done + t->failed > 0)
        n = summary (t);
      reset_queue (t);
      return n;
    }

  if (failed_now)
    {
      YtdlNotice *n = g_new0 (YtdlNotice, 1);
      n->failure = TRUE;
      n->title = t->failed == 1
                     ? g_strdup ("A download failed")
                     : g_strdup_printf ("%u downloads failed so far",
                                        t->failed);
      n->body = g_strdup_printf ("%s\n%s\nThe queue goes on: %u still to run.",
                                 t->last_failure_target,
                                 t->last_failure_reason, remaining);
      return n;
    }

  return NULL;
}
