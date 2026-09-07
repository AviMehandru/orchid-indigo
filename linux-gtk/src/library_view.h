/* The library grid.
 *
 * GtkGridView over a GListStore rather than a GtkFlowBox full of eagerly
 * built cards: the grid recycles widgets and binds only what is visible, so
 * an archive with several thousand videos costs the same as one with twenty.
 * A FlowBox would build every card up front, which is exactly the shape of
 * archive this tool exists to produce.
 */

#ifndef YTDL_LIBRARY_VIEW_H
#define YTDL_LIBRARY_VIEW_H

#include <gtk/gtk.h>

#include "archive.h"

G_BEGIN_DECLS

#define YTDL_TYPE_LIBRARY_VIEW (ytdl_library_view_get_type ())
G_DECLARE_FINAL_TYPE (YtdlLibraryView, ytdl_library_view, YTDL, LIBRARY_VIEW,
                      GtkBox)

GtkWidget *ytdl_library_view_new (void);

/* The view borrows @index and does not own it; the caller must keep it alive
 * and call this again with NULL before freeing it. Passing NULL clears. */
void ytdl_library_view_set_index (YtdlLibraryView *self, YtdlIndex *index);

/* Case-insensitive substring over title, uploader and video id. */
void ytdl_library_view_set_filter (YtdlLibraryView *self, const char *needle);

/* How many videos are showing after the filter. */
guint ytdl_library_view_get_shown (YtdlLibraryView *self);

/* Signal: "video-activated" (const char *key)
 *
 * Emitted when a card is activated -- double-click or Enter, GTK's convention
 * for "open this". Carries the opaque key rather than the entry, so a handler
 * cannot end up holding a pointer into an index a rescan has since replaced;
 * it looks the key up again against whatever index is current. */

G_END_DECLS

#endif /* YTDL_LIBRARY_VIEW_H */
