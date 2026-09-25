/* Subscriptions: the pipeline's stored list of sources and its hourly check,
 * as this app sees them.
 *
 * THIS APP RUNS NO TIMER. That is the design, not a gap. A timer here would
 * only fire while this window is open -- the one time nobody needs one -- and
 * this app, the other two in this repository and the Tauri one would each be
 * running their own, racing each other on the same channels and the same
 * manifests. So the list and the schedule live in the pipeline
 * (`ytdl --subscribe`, `ytdl --schedule install`, docs/subscriptions.md in
 * orchid-ochre) and every frontend manages them the way it manages everything
 * else: by building a `ytdl` command line.
 *
 * What is here:
 *
 *   - a reader for `ytdl --subscriptions --json`, the contract in that doc;
 *   - the wording every row of the Subscriptions pane shows, pinned by one
 *     fixture that tests/test_subscriptions.c, the SwiftUI app's
 *     SubscriptionTests.swift and the WinUI app's SubscriptionTests.cs all
 *     assert word for word;
 *   - the argument lists for the commands the pane sends;
 *   - one asynchronous "run ytdl with these arguments and tell me what it
 *     said", for the commands that are over in a second. A check itself is
 *     NOT run this way: "Check now" goes through the Downloads queue as
 *     `ytdl --run-subscriptions ID` (see YtdlRunOptions.subscription_id), so
 *     it is sequential with every other run, shows its progress, and can be
 *     cancelled like any of them.
 */

#ifndef YTDL_SUBSCRIPTIONS_H
#define YTDL_SUBSCRIPTIONS_H

#include <gio/gio.h>
#include <glib.h>

#include "pipeline.h"

G_BEGIN_DECLS

/* The subscriptions_version this app reads. A document declaring a newer one
 * is refused with a message saying so rather than misread -- the pipeline
 * bumps it only when a field is removed or changes meaning. */
#define YTDL_SUPPORTED_SUBSCRIPTIONS_VERSION 1

/* The intervals the pane offers. The pipeline takes any whole number of hours
 * from 1 to 720; these are the ones worth a menu entry. */
extern const int ytdl_subscription_interval_choices[];
extern const gsize ytdl_subscription_interval_count;

typedef struct
{
  gint64 started;  /* Unix seconds */
  gint64 finished;
  char  *result;   /* ok | errors | failed */
  int    exit_code;
  gint64 touched;
  gint64 skipped;
  gint64 errors;
  gint64 warnings;
  char  *trigger;  /* schedule | manual */
  char  *message;  /* the session's last line when failed; may be NULL */
} YtdlSubscriptionRun;

typedef struct
{
  char     *id;
  char     *url;
  char     *name;      /* may be NULL */
  GStrv     options;   /* proxy passwords already masked by the pipeline */
  char     *data_root; /* may be NULL: the pipeline's default */
  int       every_hours;
  gboolean  enabled;
  gint64    added;
  gint64    updated;
  YtdlSubscriptionRun *last_run; /* NULL until first checked */
  gint64    next_due;  /* 0 when paused */
  gboolean  due;
} YtdlSubscription;

typedef struct
{
  char    *mechanism; /* systemd | launchd | task-scheduler */
  gboolean supported;
  gboolean installed;
  gboolean active;
  gboolean running;
  gint64   next_check; /* 0 when the scheduler does not say */
  int      linger;     /* -1 unknown, 0 off, 1 on */
  char    *detail;
} YtdlSchedule;

typedef struct
{
  int          version;
  gint64       now; /* compute every "3 h ago" against this, not the clock */
  YtdlSchedule schedule;
  GPtrArray   *subscriptions; /* YtdlSubscription* */
} YtdlSubscriptionList;

void ytdl_subscription_list_free (YtdlSubscriptionList *l);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlSubscriptionList, ytdl_subscription_list_free)

/* The whole of `ytdl --subscriptions --json`'s stdout. NULL with @error set
 * when it does not parse, is not the document, or declares a version newer
 * than YTDL_SUPPORTED_SUBSCRIPTIONS_VERSION. Unknown fields are ignored. */
YtdlSubscriptionList *ytdl_subscriptions_parse (const char *json,
                                                GError    **error);

/* ---------------------------------------------------------------------- */
/* Wording -- the shared fixture                                          */
/* ---------------------------------------------------------------------- */

/* The name, or the URL when there is none. */
const char *ytdl_subscription_title (const YtdlSubscription *s);

/* "Every hour", "Every 6 hours", "Every day", "Every 7 days". */
char *ytdl_subscription_every_label (int hours);

/* One line for the row:
 *
 *   Not checked yet · due now
 *   Checked 3 h ago · 2 new · next in 3 h
 *   Checked 24 h ago · nothing new · next in 6 d
 *   Checked just now · 3 new, 1 error · next in 60 min
 *   Check failed 2 h ago · next in 10 h
 *   Paused · last checked 3 d ago
 *
 * Relative to @now, which should be the document's own `now`. */
char *ytdl_subscription_status_line (const YtdlSubscription *s, gint64 now);

/* The automatic-checks row:
 *
 *   On · checks hourly · next check in 25 min · only while you are logged in
 *   Off · subscriptions are checked only when you press Check now
 *   Installed but not running · turn it off and on again
 *   <the pipeline's own detail>                  when not supported here
 *
 * with " · checking now" appended while a check is running. */
char *ytdl_schedule_status_line (const YtdlSchedule *s, gint64 now);

/* Exposed for the tests: "just now", "4 min ago", "3 h ago", "2 d ago", and
 * "due now", "in 25 min", "in 3 h", "in 6 d". Integer rounding, half up, so
 * that C, Swift and C# agree to the minute. */
char *ytdl_format_ago (gint64 then, gint64 now);
char *ytdl_format_in (gint64 when, gint64 now);

/* ---------------------------------------------------------------------- */
/* The commands the pane sends -- everything after ytdl.ps1               */
/* ---------------------------------------------------------------------- */

GStrv ytdl_subscriptions_list_args (void);

/* The Downloads form's options, as a subscription. @opts is the form exactly
 * as a run would send it, connection settings included -- a scheduled check
 * of a members-only playlist needs its cookies as much as the first download
 * did. @name may be NULL. */
GStrv ytdl_subscribe_args (const YtdlRunOptions *opts, int every_hours,
                           const char *name);

/* @every_hours 0 leaves it alone; @pause -1 leaves it, 0 resumes, 1 pauses. */
GStrv ytdl_subscription_edit_args (const char *id, int every_hours,
                                   int pause);
GStrv ytdl_unsubscribe_args (const char *id);

/* install | remove */
GStrv ytdl_schedule_args (gboolean install);

/* A queued run that checks one subscription now. The URL and data root are
 * carried for the history row and the log path only; the argv is
 * `--run-subscriptions ID`, and the subscription's own stored options -- not
 * the form's, not the connection settings -- are what the pipeline uses. */
YtdlRunOptions *ytdl_subscription_run_options (const YtdlSubscription *s);

/* ---------------------------------------------------------------------- */
/* Running a short command                                                */
/* ---------------------------------------------------------------------- */

typedef struct
{
  int   exit_code;
  char *out;
  char *err;
} YtdlCommandResult;

void ytdl_command_result_free (YtdlCommandResult *r);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlCommandResult, ytdl_command_result_free)

/* The sentence to show when @r failed: the "Error: ..." line if the pipeline
 * printed one, else the first thing it printed, else the exit code. */
char *ytdl_command_result_message (const YtdlCommandResult *r);

/* Does this failure mean the installed pipeline predates subscriptions?
 * ytdl.ps1 without them answers "--subscriptions" in the URL position with
 * its missing-URL error; one with ytdl.ps1 but no subscriptions.ps1 says so. */
gboolean ytdl_command_result_means_too_old (const YtdlCommandResult *r);

/* `pwsh -NoProfile -File <installed ytdl.ps1> @args`, off the main thread,
 * stdout and stderr captured apart. Fails (rather than returning a result)
 * only when the pipeline could not be started at all. */
void ytdl_command_run_async (const char *const *args,
                             GCancellable       *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer            user_data);
YtdlCommandResult *ytdl_command_run_finish (GAsyncResult *result,
                                            GError      **error);

G_END_DECLS

#endif /* YTDL_SUBSCRIPTIONS_H */
