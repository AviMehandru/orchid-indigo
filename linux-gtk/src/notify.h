/* What the queue tells you when you are not looking at the window.
 *
 * This file decides WHAT to say and WHEN. It does not send anything: main.c
 * turns a YtdlNotice into a GNotification, and only while the window is in
 * the background. Kept apart from the sending for the same reason the library
 * filter is kept apart from the grid -- so the rules can be tested without a
 * desktop session, and read side by side with the Swift and C# copies, which
 * are asserted against the same fixture values (tests/test_notify.c).
 *
 * THE RULES.
 *
 * A "queue" here is everything the runner does between two moments when it
 * has nothing left to run. Thirty re-fetches from one bulk press are one
 * queue; so is a single Add to queue.
 *
 *  1. When a queue ends, ONE summary. Not one notification per run: a bulk
 *     re-fetch would otherwise put thirty toasts on screen, which teaches
 *     people to switch notifications off, after which the 3am failure this
 *     exists for goes unannounced again.
 *  2. When a run FAILS and more are still to run, say so at once rather than
 *     at the end, because a failure is what someone may want to act on and
 *     the end may be hours away. A success mid-queue says nothing.
 *  3. Both use ONE notification slot (YTDL_NOTICE_ID), so the end-of-queue
 *     summary replaces a mid-queue failure notice instead of stacking under
 *     it -- the summary repeats the failure, so nothing is lost.
 *  4. A queue in which every run was cancelled says nothing. Cancelling is
 *     something done at the window, by somebody who is looking at it.
 *  5. Runs restored from the last session's history are never announced.
 *     The tracker is seeded with them and only reports ids it has not seen.
 *
 * Whether the window is focused, and whether the setting is on, are the
 * caller's to check -- deliberately AFTER calling update(), so the tracker's
 * counts stay right while the window is focused and a summary sent later
 * still covers the whole queue.
 */

#ifndef YTDL_NOTIFY_H
#define YTDL_NOTIFY_H

#include <glib.h>

#include "pipeline.h"

G_BEGIN_DECLS

/* The one notification id this app uses. See rule 3. */
#define YTDL_NOTICE_ID "queue"

/* Longest failure reason and run target carried into a notice, in
 * characters. A yt-dlp error line can be several hundred characters of URL
 * and traceback; a notification shows two or three lines, and a body the
 * desktop truncates mid-word reads worse than one cut here with an ellipsis. */
#define YTDL_NOTICE_REASON_MAX 160
#define YTDL_NOTICE_TARGET_MAX 100

typedef struct
{
  char    *title;
  char    *body;
  /* Something failed. Shown at a higher priority where the platform has
   * one; it never decides WHETHER to notify. */
  gboolean failure;
} YtdlNotice;

void ytdl_notice_free (YtdlNotice *n);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlNotice, ytdl_notice_free)

typedef struct _YtdlNoticeTracker YtdlNoticeTracker;

/* @history is what the runner restored at startup (YtdlRunRecord*, may be
 * NULL). Every id in it is treated as already announced -- rule 5. */
YtdlNoticeTracker *ytdl_notice_tracker_new (GPtrArray *history);
void               ytdl_notice_tracker_free (YtdlNoticeTracker *t);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlNoticeTracker, ytdl_notice_tracker_free)

/* Feed it every state change, with what ytdl_runner_settled returned: the
 * history (newest first) and how many runs are still to run. Returns what to
 * announce now, or NULL for nothing.
 *
 * Coalescing is expected: the runner emits state-changed from a 50ms drain,
 * so one call may carry several newly finished runs, or a run finishing and
 * the queue going idle at once. Both are handled -- the second produces only
 * the summary. */
YtdlNotice *ytdl_notice_tracker_update (YtdlNoticeTracker *t,
                                        GPtrArray *history, guint remaining);

/* Exposed for the test suite. */
char *ytdl_notice_clip (const char *text, glong max_chars);

G_END_DECLS

#endif /* YTDL_NOTIFY_H */
