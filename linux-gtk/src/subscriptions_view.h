/* The Subscriptions pane: the pipeline's list of sources and its hourly check,
 * shown and changed through `ytdl`.
 *
 * Nothing on this pane is stored by this app, and nothing here runs on a
 * timer. Every row is read from `ytdl --subscriptions --json` each time the
 * pane is shown, and every button is a `ytdl` command -- so a subscription
 * added from a terminal, from the SwiftUI or WinUI app or from the Tauri one
 * shows up here, and one changed here is changed for all of them. See
 * subscriptions.h for why the timer is not in this app.
 *
 * Subscribing happens on the Downloads pane, not here: its form already holds
 * every option a download can take and shows the command line, and a second
 * form here would be a second place for the two to disagree.
 */

#ifndef YTDL_SUBSCRIPTIONS_VIEW_H
#define YTDL_SUBSCRIPTIONS_VIEW_H

#include <gtk/gtk.h>

#include "pipeline.h"

G_BEGIN_DECLS

#define YTDL_TYPE_SUBSCRIPTIONS_VIEW (ytdl_subscriptions_view_get_type ())
G_DECLARE_FINAL_TYPE (YtdlSubscriptionsView, ytdl_subscriptions_view, YTDL,
                      SUBSCRIPTIONS_VIEW, GtkBox)

/* Signals:
 *
 *   "message" (const char *text)  something happened worth a toast -- a check
 *                                 was queued, a subscription removed, a
 *                                 command refused. main.c owns the overlay.
 *
 * Borrows @runner, which must outlive the view: "Check now" queues a run on
 * it like any other. */
GtkWidget *ytdl_subscriptions_view_new (YtdlRunner *runner);

/* Read the list again. Cheap enough to call every time the pane is shown --
 * one pwsh start -- and it is the only way to see what the schedule did since
 * the last look. A refresh already in flight is not started twice. */
void ytdl_subscriptions_view_refresh (YtdlSubscriptionsView *self);

G_END_DECLS

#endif /* YTDL_SUBSCRIPTIONS_VIEW_H */
