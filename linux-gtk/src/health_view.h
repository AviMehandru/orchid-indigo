/* The Health pane: what is installed, what version, and does the archive
 * still verify.
 *
 * Read-only, like everything behind it. The pipeline updates yt-dlp on its own
 * 24h throttle and a second updater racing it from a window is exactly the
 * shared-state collision the pipeline spent a release removing.
 */

#ifndef YTDL_HEALTH_VIEW_H
#define YTDL_HEALTH_VIEW_H

#include <gtk/gtk.h>

#include "archive.h"
#include "settings.h"

G_BEGIN_DECLS

#define YTDL_TYPE_HEALTH_VIEW (ytdl_health_view_get_type ())
G_DECLARE_FINAL_TYPE (YtdlHealthView, ytdl_health_view, YTDL, HEALTH_VIEW,
                      GtkBox)

GtkWidget *ytdl_health_view_new (YtdlSettings *settings);

/* Borrowed, and may be NULL. Pass NULL before freeing an index. */
void ytdl_health_view_set_index (YtdlHealthView *self, YtdlIndex *index);

/* Re-read everything cheap and re-probe the dependencies. @force skips the
 * five-minute probe cache. */
void ytdl_health_view_refresh (YtdlHealthView *self, gboolean force);

G_END_DECLS

#endif /* YTDL_HEALTH_VIEW_H */
