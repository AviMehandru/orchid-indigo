/* The Downloads pane: build a `ytdl` command line, run it, watch it.
 *
 * The command preview above the Start button is not decoration. A GUI that
 * hides the command it runs makes the CLI harder to learn rather than easier,
 * and every problem report about this app is easier to answer when the user
 * can paste the exact line the window would have run.
 */

#ifndef YTDL_DOWNLOADS_VIEW_H
#define YTDL_DOWNLOADS_VIEW_H

#include <gtk/gtk.h>

#include "pipeline.h"
#include "settings.h"

G_BEGIN_DECLS

#define YTDL_TYPE_DOWNLOADS_VIEW (ytdl_downloads_view_get_type ())
G_DECLARE_FINAL_TYPE (YtdlDownloadsView, ytdl_downloads_view, YTDL,
                      DOWNLOADS_VIEW, GtkBox)

/* Borrows both. The runner must outlive the view; @settings is written back
 * through ytdl_settings_save whenever the destination changes. */
GtkWidget *ytdl_downloads_view_new (YtdlRunner *runner, YtdlSettings *settings);

G_END_DECLS

#endif /* YTDL_DOWNLOADS_VIEW_H */
