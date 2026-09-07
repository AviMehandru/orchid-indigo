/* The small amount of CSS libadwaita does not already provide.
 *
 * MOST OF THE OLD STYLESHEET IS GONE, and that is the point of the port.
 * Cards, boxed lists, status pages, header bars, the light/dark palette and
 * the whole spacing scale are libadwaita's job now; anything written here
 * would be a second, worse copy of them that drifts every release.
 *
 * What is left is the handful of things libadwaita has no widget for: a
 * duration badge that sits on top of a video frame, a small state pill, a
 * placeholder tint behind a thumbnail that has not decoded, and the override
 * that stops GtkGridView painting its selection straight over a card.
 *
 * EVERY COLOUR HERE IS ONE OF LIBADWAITA'S NAMED COLOURS. @accent_color and
 * friends already track the system light/dark preference and, since GNOME 47,
 * the user's chosen accent -- so a pill written against them is the right
 * colour on someone else's desktop without this file knowing anything about
 * which theme is loaded.
 */

#ifndef YTDL_STYLE_H
#define YTDL_STYLE_H

#include <adwaita.h>

G_BEGIN_DECLS

/* Installs the stylesheet on the default display. Call once, after adw_init()
 * has run and before any window is presented. */
void ytdl_style_load (void);

G_END_DECLS

#endif /* YTDL_STYLE_H */
