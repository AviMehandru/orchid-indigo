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
#include "library_filter.h"
#include "verify_cache.h"

G_BEGIN_DECLS

#define YTDL_TYPE_LIBRARY_VIEW (ytdl_library_view_get_type ())
G_DECLARE_FINAL_TYPE (YtdlLibraryView, ytdl_library_view, YTDL, LIBRARY_VIEW,
                      GtkBox)

GtkWidget *ytdl_library_view_new (void);

/* The view borrows @index and does not own it; the caller must keep it alive
 * and call this again with NULL before freeing it. Passing NULL clears. */
void ytdl_library_view_set_index (YtdlLibraryView *self, YtdlIndex *index);

/* The live filter, owned by the view.
 *
 * Handed out for mutation rather than wrapped in a setter per field, because
 * the facet popover in main.c has a dozen controls and a setter each would be
 * a dozen functions that do nothing but assign. The caller mutates and then
 * calls ytdl_library_view_refilter. Never NULL. */
YtdlLibraryFilter *ytdl_library_view_get_filter (YtdlLibraryView *self);

/* Re-apply the filter and redraw. Cheap: nothing here touches the disk. */
void ytdl_library_view_refilter (YtdlLibraryView *self);

/* The verification cache the "failed verification" facet reads. Borrowed --
 * the application owns it, because the detail page writes to the same one. */
void ytdl_library_view_set_verify_cache (YtdlLibraryView *self,
                                         YtdlVerifyCache *cache);

/* Case-insensitive substring over title, uploader and video id. A shortcut
 * for setting the filter's needle and refiltering, kept because the search
 * entry is the one control that changes on every keystroke. */
void ytdl_library_view_set_search (YtdlLibraryView *self, const char *needle);

/* How many videos are showing after the filter. */
guint ytdl_library_view_get_shown (YtdlLibraryView *self);

/* Selection mode: the grid stops opening videos and starts collecting them.
 *
 * A MODE rather than "ctrl-click always multi-selects", because the primary
 * gesture on a card is "open this" and a grid where a stray click adds to a
 * hidden selection is a grid that does the wrong thing quietly. The toggle is
 * what makes the bulk bar's appearance the user's own doing.
 *
 * Leaving selection mode clears the selection. Carrying it out of the mode
 * would mean a selection nothing on screen is showing. */
void     ytdl_library_view_set_selection_mode (YtdlLibraryView *self,
                                               gboolean on);
gboolean ytdl_library_view_get_selection_mode (YtdlLibraryView *self);

/* The selected videos' keys, in grid order.
 *
 * BORROWED strings owned by the index; g_ptr_array_unref the array and do not
 * free the elements. Empty outside selection mode. */
GPtrArray *ytdl_library_view_selected_keys (YtdlLibraryView *self);

/* Select or deselect everything the filter is currently showing -- not the
 * whole archive. "Select all" inside a filtered view meaning the unfiltered
 * set is how someone marks four thousand videos watched by accident. */
void ytdl_library_view_select_all (YtdlLibraryView *self, gboolean all);

/* Signal: "video-activated" (const char *key)
 *
 * Emitted when a card is activated -- double-click or Enter, GTK's convention
 * for "open this". Carries the opaque key rather than the entry, so a handler
 * cannot end up holding a pointer into an index a rescan has since replaced;
 * it looks the key up again against whatever index is current.
 *
 * Signal: "selection-changed" ()
 *
 * The bulk bar's count and sensitivity key off this. Carries nothing: the
 * handler asks for the keys, because a signal that carried the list would
 * build one on every click whether anything read it or not. */

G_END_DECLS

#endif /* YTDL_LIBRARY_VIEW_H */
