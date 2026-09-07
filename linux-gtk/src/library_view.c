#include "library_view.h"

#include "paths.h"

#include <string.h>

/* ---------------------------------------------------------------------- */
/* YtdlVideoObject -- a GObject box around a borrowed YtdlEntry            */
/* ---------------------------------------------------------------------- */
/*
 * GListStore holds GObjects, and YtdlEntry is a plain struct owned by the
 * index. Rather than deep-copying every entry into a GObject, this wraps a
 * BORROWED pointer: the index outlives the store by construction, because
 * set_index() rebuilds the store and clears it before any index is freed.
 */

#define YTDL_TYPE_VIDEO_OBJECT (ytdl_video_object_get_type ())
G_DECLARE_FINAL_TYPE (YtdlVideoObject, ytdl_video_object, YTDL, VIDEO_OBJECT,
                      GObject)

struct _YtdlVideoObject
{
  GObject parent_instance;
  const YtdlEntry *entry; /* borrowed from the index */
};

G_DEFINE_FINAL_TYPE (YtdlVideoObject, ytdl_video_object, G_TYPE_OBJECT)

static void
ytdl_video_object_class_init (YtdlVideoObjectClass *klass)
{
}

static void
ytdl_video_object_init (YtdlVideoObject *self)
{
}

static YtdlVideoObject *
ytdl_video_object_new (const YtdlEntry *entry)
{
  YtdlVideoObject *obj = g_object_new (YTDL_TYPE_VIDEO_OBJECT, NULL);
  obj->entry = entry;
  return obj;
}

/* ---------------------------------------------------------------------- */
/* The view                                                               */
/* ---------------------------------------------------------------------- */

struct _YtdlLibraryView
{
  GtkBox parent_instance;

  GtkWidget  *grid;
  GtkWidget  *scroller;
  GtkWidget  *empty;   /* shown instead of the grid when nothing matches */
  GtkWidget  *stack;
  GListStore *store;

  YtdlIndex *index; /* borrowed */
  char      *needle;

  /* path -> GdkTexture. Thumbnails are decoded once and reused as the grid
   * recycles rows past them; without this a scroll re-decodes a JPEG per
   * visible card per frame budget. */
  GHashTable *thumbs;
};

G_DEFINE_FINAL_TYPE (YtdlLibraryView, ytdl_library_view, GTK_TYPE_BOX)

enum
{
  SIG_VIDEO_ACTIVATED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

#define THUMB_W 240
#define THUMB_H 135

static GdkTexture *
thumbnail_for (YtdlLibraryView *self, const YtdlEntry *entry)
{
  gssize ti = ytdl_entry_thumbnail_index (entry);
  if (ti < 0)
    return NULL;

  g_autofree char *path = ytdl_entry_path_for_index (entry, (gsize) ti);
  if (path == NULL)
    return NULL;

  GdkTexture *cached = g_hash_table_lookup (self->thumbs, path);
  if (cached != NULL)
    return cached;

  /* Scaled at DECODE time, not after: a 1920x1080 thumbnail decoded whole and
   * then scaled costs about eight megabytes per card, and an archive has
   * thousands of cards. */
  g_autoptr (GdkPixbuf) pix = gdk_pixbuf_new_from_file_at_scale (
      path, THUMB_W, THUMB_H, TRUE, NULL);
  if (pix == NULL)
    return NULL;

  GdkTexture *tex = gdk_texture_new_for_pixbuf (pix);
  g_hash_table_insert (self->thumbs, g_steal_pointer (&path), tex);
  return tex;
}

static char *
format_duration (double seconds)
{
  if (seconds <= 0)
    return g_strdup ("");

  int total = (int) (seconds + 0.5);
  int h = total / 3600;
  int m = (total % 3600) / 60;
  int s = total % 60;

  if (h > 0)
    return g_strdup_printf ("%d:%02d:%02d", h, m, s);
  return g_strdup_printf ("%d:%02d", m, s);
}

/* "20240131" -> "31 Jan 2024". Left as-is if it is not the documented shape,
 * because the folder-name fallback can produce anything. */
static char *
format_upload_date (const char *yyyymmdd)
{
  static const char *const months[] = { "Jan", "Feb", "Mar", "Apr",
                                        "May", "Jun", "Jul", "Aug",
                                        "Sep", "Oct", "Nov", "Dec" };
  if (yyyymmdd == NULL || strlen (yyyymmdd) != 8)
    return g_strdup (yyyymmdd != NULL ? yyyymmdd : "");

  for (int i = 0; i < 8; i++)
    if (!g_ascii_isdigit (yyyymmdd[i]))
      return g_strdup (yyyymmdd);

  int month = (yyyymmdd[4] - '0') * 10 + (yyyymmdd[5] - '0');
  if (month < 1 || month > 12)
    return g_strdup (yyyymmdd);

  return g_strdup_printf ("%c%c %s %.4s", yyyymmdd[6], yyyymmdd[7],
                          months[month - 1], yyyymmdd);
}

static void
on_setup_item (GtkSignalListItemFactory *factory, GtkListItem *item,
               gpointer user_data)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_size_request (card, THUMB_W, -1);
  gtk_widget_add_css_class (card, "card");
  gtk_widget_set_margin_start (card, 6);
  gtk_widget_set_margin_end (card, 6);
  gtk_widget_set_margin_top (card, 6);
  gtk_widget_set_margin_bottom (card, 6);

  /* A frame that is always THUMB_W x THUMB_H whether or not an image loads,
   * so the grid does not reflow as thumbnails decode. */
  GtkWidget *thumb = gtk_picture_new ();
  gtk_widget_set_size_request (thumb, THUMB_W, THUMB_H);
  gtk_picture_set_content_fit (GTK_PICTURE (thumb), GTK_CONTENT_FIT_COVER);
  gtk_widget_add_css_class (thumb, "ytdl-thumb");
  gtk_box_append (GTK_BOX (card), thumb);

  GtkWidget *title = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (title), 0.0f);
  gtk_label_set_wrap (GTK_LABEL (title), TRUE);
  gtk_label_set_lines (GTK_LABEL (title), 2);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (title, "heading");
  gtk_box_append (GTK_BOX (card), title);

  GtkWidget *sub = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (sub), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (sub), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (sub, "dim-label");
  gtk_widget_add_css_class (sub, "caption");
  gtk_box_append (GTK_BOX (card), sub);

  GtkWidget *badges = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (card), badges);

  g_object_set_data (G_OBJECT (card), "thumb", thumb);
  g_object_set_data (G_OBJECT (card), "title", title);
  g_object_set_data (G_OBJECT (card), "sub", sub);
  g_object_set_data (G_OBJECT (card), "badges", badges);

  gtk_list_item_set_child (item, card);
}

static void
add_badge (GtkWidget *box, const char *text, const char *css)
{
  GtkWidget *label = gtk_label_new (text);
  gtk_widget_add_css_class (label, "caption");
  if (css != NULL)
    gtk_widget_add_css_class (label, css);
  gtk_box_append (GTK_BOX (box), label);
}

static void
on_bind_item (GtkSignalListItemFactory *factory, GtkListItem *item,
              gpointer user_data)
{
  YtdlLibraryView *self = user_data;
  YtdlVideoObject *obj = gtk_list_item_get_item (item);
  GtkWidget *card = gtk_list_item_get_child (item);
  if (obj == NULL || card == NULL)
    return;

  const YtdlEntry *e = obj->entry;

  GtkWidget *thumb = g_object_get_data (G_OBJECT (card), "thumb");
  GtkWidget *title = g_object_get_data (G_OBJECT (card), "title");
  GtkWidget *sub = g_object_get_data (G_OBJECT (card), "sub");
  GtkWidget *badges = g_object_get_data (G_OBJECT (card), "badges");

  gtk_picture_set_paintable (GTK_PICTURE (thumb),
                             GDK_PAINTABLE (thumbnail_for (self, e)));

  gtk_label_set_text (GTK_LABEL (title), e->title != NULL ? e->title : "");

  g_autofree char *date = format_upload_date (e->upload_date);
  g_autofree char *dur = format_duration (e->duration);
  g_autofree char *subtitle = g_strdup_printf (
      "%s%s%s%s%s", e->uploader != NULL ? e->uploader : "",
      (date != NULL && *date != '\0') ? " · " : "", date,
      (dur != NULL && *dur != '\0') ? " · " : "", dur);
  gtk_label_set_text (GTK_LABEL (sub), subtitle);

  /* Rebuild the badge row: the card is recycled, so last video's badges are
   * still on it. */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (badges)) != NULL)
    gtk_box_remove (GTK_BOX (badges), child);

  if (ytdl_entry_media_index (e) < 0)
    {
      /* Not an error. --mode metadata-only, comments-only and subs-only all
       * write a complete folder with no media, and so does an interrupted
       * run. Saying which is the manifest's job, not a guess from here. */
      const char *why = e->download_mode != NULL ? e->download_mode
                                                 : "no media file";
      add_badge (badges, why, "dim-label");
    }
  if (e->layout_too_new)
    add_badge (badges, "newer archive layout", "warning");
}

/* ---------------------------------------------------------------------- */

static gboolean
matches (const YtdlEntry *e, const char *needle_folded)
{
  if (needle_folded == NULL || *needle_folded == '\0')
    return TRUE;

  const char *fields[] = { e->title, e->uploader, e->id, e->channel };
  for (gsize i = 0; i < G_N_ELEMENTS (fields); i++)
    {
      if (fields[i] == NULL)
        continue;
      g_autofree char *folded = g_utf8_casefold (fields[i], -1);
      if (strstr (folded, needle_folded) != NULL)
        return TRUE;
    }
  return FALSE;
}

static void
rebuild (YtdlLibraryView *self)
{
  g_list_store_remove_all (self->store);

  g_autofree char *folded =
      (self->needle != NULL && *self->needle != '\0')
          ? g_utf8_casefold (self->needle, -1)
          : NULL;

  guint shown = 0;
  if (self->index != NULL)
    {
      for (guint i = 0; i < self->index->entries->len; i++)
        {
          const YtdlEntry *e = g_ptr_array_index (self->index->entries, i);
          if (!matches (e, folded))
            continue;
          g_autoptr (YtdlVideoObject) obj = ytdl_video_object_new (e);
          g_list_store_append (self->store, obj);
          shown++;
        }
    }

  gtk_stack_set_visible_child_name (GTK_STACK (self->stack),
                                    shown > 0 ? "grid" : "empty");
}

void
ytdl_library_view_set_index (YtdlLibraryView *self, YtdlIndex *index)
{
  g_return_if_fail (YTDL_IS_LIBRARY_VIEW (self));

  /* Every wrapper borrows a pointer into the OLD index, so the store must be
   * emptied before the new one is adopted -- not after. */
  g_list_store_remove_all (self->store);
  g_hash_table_remove_all (self->thumbs);

  self->index = index;
  rebuild (self);
}

void
ytdl_library_view_set_filter (YtdlLibraryView *self, const char *needle)
{
  g_return_if_fail (YTDL_IS_LIBRARY_VIEW (self));
  g_free (self->needle);
  self->needle = g_strdup (needle);
  rebuild (self);
}

guint
ytdl_library_view_get_shown (YtdlLibraryView *self)
{
  g_return_val_if_fail (YTDL_IS_LIBRARY_VIEW (self), 0);
  return g_list_model_get_n_items (G_LIST_MODEL (self->store));
}

static void
ytdl_library_view_dispose (GObject *object)
{
  YtdlLibraryView *self = YTDL_LIBRARY_VIEW (object);
  g_clear_pointer (&self->needle, g_free);
  g_clear_pointer (&self->thumbs, g_hash_table_destroy);
  g_clear_object (&self->store);
  self->index = NULL;
  G_OBJECT_CLASS (ytdl_library_view_parent_class)->dispose (object);
}

static void
ytdl_library_view_class_init (YtdlLibraryViewClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = ytdl_library_view_dispose;

  signals[SIG_VIDEO_ACTIVATED] =
      g_signal_new ("video-activated", G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
                    G_TYPE_STRING);
}

static void
on_grid_activate (GtkGridView *grid, guint position, gpointer user_data)
{
  YtdlLibraryView *self = user_data;
  g_autoptr (YtdlVideoObject) obj =
      g_list_model_get_item (G_LIST_MODEL (self->store), position);
  if (obj == NULL || obj->entry == NULL)
    return;
  g_signal_emit (self, signals[SIG_VIDEO_ACTIVATED], 0, obj->entry->key);
}

static void
ytdl_library_view_init (YtdlLibraryView *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);

  self->store = g_list_store_new (YTDL_TYPE_VIDEO_OBJECT);
  self->thumbs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                        g_object_unref);

  GtkSelectionModel *selection = GTK_SELECTION_MODEL (
      gtk_single_selection_new (G_LIST_MODEL (g_object_ref (self->store))));

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (on_setup_item), self);
  g_signal_connect (factory, "bind", G_CALLBACK (on_bind_item), self);

  self->grid = gtk_grid_view_new (selection, factory);
  gtk_grid_view_set_max_columns (GTK_GRID_VIEW (self->grid), 8);
  gtk_grid_view_set_min_columns (GTK_GRID_VIEW (self->grid), 1);
  gtk_grid_view_set_single_click_activate (GTK_GRID_VIEW (self->grid), FALSE);
  g_signal_connect (self->grid, "activate", G_CALLBACK (on_grid_activate), self);

  self->scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->scroller),
                                 self->grid);
  gtk_widget_set_vexpand (self->scroller, TRUE);

  self->empty = gtk_label_new ("No videos to show yet.\n\n"
                               "Point Settings at the same path you would "
                               "pass to `ytdl --path`, then press Rescan.");
  gtk_label_set_justify (GTK_LABEL (self->empty), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (self->empty, "dim-label");
  gtk_widget_set_vexpand (self->empty, TRUE);
  gtk_widget_set_valign (self->empty, GTK_ALIGN_CENTER);

  self->stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (self->stack), self->scroller, "grid");
  gtk_stack_add_named (GTK_STACK (self->stack), self->empty, "empty");
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "empty");
  gtk_widget_set_vexpand (self->stack, TRUE);

  gtk_box_append (GTK_BOX (self), self->stack);
}

GtkWidget *
ytdl_library_view_new (void)
{
  return g_object_new (YTDL_TYPE_LIBRARY_VIEW, NULL);
}
