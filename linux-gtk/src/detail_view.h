/* One video's page.
 *
 * Everything shown here was already on disk and unread: the description and
 * metadata from info.json, the comment tree yt-dlp captured, the subtitle
 * track as prose, the file inventory, and what is actually inside the media
 * container. The Library tells you what you have; this tells you what it is.
 *
 * THE PLAYER IS THE ORIGINAL FILE. The webview build could not do this -- a
 * browser engine cannot play Matroska, so it remuxed .mkv to WebM into a cache
 * directory and served that. GStreamer plays the original, so the entire
 * remux/transcode apparatus is gone and what you watch is the bytes that were
 * archived. Nothing on this page writes anything, anywhere.
 */

#ifndef YTDL_DETAIL_VIEW_H
#define YTDL_DETAIL_VIEW_H

#include <gtk/gtk.h>

#include "archive.h"

G_BEGIN_DECLS

#define YTDL_TYPE_DETAIL_VIEW (ytdl_detail_view_get_type ())
G_DECLARE_FINAL_TYPE (YtdlDetailView, ytdl_detail_view, YTDL, DETAIL_VIEW,
                      GtkBox)

GtkWidget *ytdl_detail_view_new (void);

/* Draw @entry. Returns immediately: the probe, the info.json parse and the
 * transcript all happen on a worker thread, because an info.json with a large
 * comment tree takes long enough to stall a click. */
void ytdl_detail_view_show (YtdlDetailView *self, const YtdlEntry *entry);

/* Stop playback and drop the loaded media. Called when the page is navigated
 * away from -- a GtkVideo left with a file keeps a GStreamer pipeline alive,
 * and audio continuing after you press Back is the kind of thing people
 * remember about an application. */
void ytdl_detail_view_clear (YtdlDetailView *self);

G_END_DECLS

#endif /* YTDL_DETAIL_VIEW_H */
