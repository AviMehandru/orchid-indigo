#include "style.h"

static const char *STYLESHEET =
    /* --- the duration badge -------------------------------------------
     *
     * The one place a fixed colour is right. It sits ON the thumbnail, so
     * its background is a video frame rather than the theme's, and a frame
     * can be any colour at all -- deriving it from @view_bg_color would make
     * it invisible over roughly half of them.
     */
    ".ytdl-duration {"
    "  background: alpha(black, 0.72);"
    "  color: white;"
    "  border-radius: 6px;"
    "  padding: 1px 6px;"
    "  margin: 6px;"
    "  font-size: 0.82em;"
    "  font-weight: bold;"
    "}"

    /* --- state pills ---------------------------------------------------
     *
     * libadwaita has no label-sized status widget: AdwActionRow subtitles are
     * prose and AdwBanner is a full-width bar. A pill is what fits inside a
     * row's suffix slot, and on a table of seven rows the SHAPE is what you
     * scan -- "missing" in red text beside "required" in grey text reads as
     * one run-on phrase.
     *
     * The colours are libadwaita's semantic ones, so they follow the system
     * accent and invert with the theme on their own.
     */
    ".ytdl-pill {"
    "  border-radius: 999px;"
    "  padding: 1px 9px;"
    "  font-size: 0.82em;"
    "  font-weight: bold;"
    "  background: alpha(currentColor, 0.10);"
    "}"
    ".ytdl-pill.ok     { background: alpha(@success_color, 0.15); color: @success_color; }"
    ".ytdl-pill.warn   { background: alpha(@warning_color, 0.18); color: @warning_color; }"
    ".ytdl-pill.err    { background: alpha(@error_color, 0.15);   color: @error_color; }"
    ".ytdl-pill.accent { background: alpha(@accent_color, 0.15);  color: @accent_color; }"

    /* --- thumbnail placeholder ----------------------------------------- */
    /* Holds the card's shape while a JPEG decodes, so the grid does not
     * reflow underneath the pointer. */
    ".ytdl-thumb {"
    "  background: alpha(currentColor, 0.10);"
    "  border-radius: 8px;"
    "}"

    /* --- comment replies ----------------------------------------------- */
    /* A rule down the left rather than an indent alone, so a long thread
     * still reads as one conversation once the text wraps. */
    ".ytdl-reply {"
    "  border-left: 2px solid alpha(currentColor, 0.13);"
    "  padding-left: 12px;"
    "}"

    /* --- run log --------------------------------------------------------
     *
     * A GtkTextView paints @view_bg_color, which inside a .card is the same
     * colour as the card -- the log would have no edge at all. Transparent
     * text plus the card underneath is what gives it one.
     */
    ".ytdl-log, .ytdl-log text { background: transparent; }"

    /* --- grid selection -------------------------------------------------
     *
     * GtkGridView paints its selection on the ITEM, which is the box wrapping
     * each card, so the default fills a hard @accent_bg_color rectangle
     * behind the card, hides its border and flips the labels to
     * @accent_fg_color. The item is made transparent and the selection is
     * expressed on the card instead. The foreground has to be reset too:
     * removing the background alone left white-on-pale-blue text, because
     * :selected also swaps the label colour for the one meant to sit on a
     * saturated fill.
     */
    "gridview > child { padding: 0; background: none; border-radius: 12px; }"
    "gridview > child:selected { background: none; color: inherit; }"
    "gridview > child:selected label { color: inherit; }"
    /* An inset box-shadow, not an outline: the card sets
     * GTK_OVERFLOW_HIDDEN so the thumbnail can reach its rounded corners,
     * and an outline drawn outside the border box is clipped away by exactly
     * that. A shadow is painted inside it and survives. */
    "gridview > child:selected .card {"
    "  background: alpha(@accent_color, 0.13);"
    "  box-shadow: inset 0 0 0 2px alpha(@accent_color, 0.55);"
    "}";

void
ytdl_style_load (void)
{
  GdkDisplay *display = gdk_display_get_default ();
  if (display == NULL)
    return;

  g_autoptr (GtkCssProvider) provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (provider, STYLESHEET);
  gtk_style_context_add_provider_for_display (
      display, GTK_STYLE_PROVIDER (provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}
