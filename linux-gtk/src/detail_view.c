#include "detail_view.h"

#include <adwaita.h>

#include "health.h"
#include "media.h"
#include "paths.h"

#include <string.h>

/* Comment sections run to thousands of entries, and one GtkBox per comment is
 * thousands of widgets built in a click handler. Capped, with the total said
 * out loud -- a page that quietly shows 200 of 4,000 would be worse than one
 * that takes a moment. The transcript has no such cap because it renders into
 * a single text buffer rather than a widget per line. */
#define MAX_COMMENTS_SHOWN 200

typedef struct
{
  /* Copied on the main thread before the worker starts: the index can be
   * replaced by a rescan while this is in flight, and every pointer into the
   * old one would go with it. */
  char *key;
  char *dir;
  char *title;
  char *uploader;
  char *upload_date;
  char *channel;
  char *original_url;
  char *download_mode;
  char *media_path;   /* NULL when the folder has no media, which is ordinary */
  char *subtitle_path;
  gboolean subtitle_is_auto;
  char    *subtitle_name;
  guint64  layout_version;
  gboolean layout_too_new;

  GPtrArray *files; /* char* "rel\tsize" pairs, prepared on the main thread */

  /* Filled by the worker. */
  YtdlProbe *probe;
  GPtrArray *comments;  /* YtdlComment* */
  GPtrArray *cues;      /* YtdlCue* */
  char      *description;
  gint64     view_count;
  gint64     like_count;
  char      *tags;
  gsize      comment_total;

  YtdlDetailView *view;
  guint           generation;
} DetailData;

struct _YtdlDetailView
{
  GtkBox parent_instance;

  GtkWidget *subtitle;
  GtkWidget *summary;
  GtkWidget *badges;

  GtkWidget *video;        /* GtkVideo, or NULL when no media backend */
  GtkWidget *video_holder;
  GtkWidget *no_player_note;

  GtkWidget *spinner;
  GtkWidget *stack;

  GtkWidget *streams_box;
  GtkWidget *meta_box;
  GtkWidget *comments_box;
  GtkWidget *transcript;
  GtkWidget *transcript_note;
  GtkWidget *files_box;
  GtkWidget *verify_result;

  char *media_path;
  char *folder;

  /* Bumped on every show(); a worker whose generation no longer matches has
   * been superseded by a later click and throws its result away. */
  guint generation;
};

G_DEFINE_FINAL_TYPE (YtdlDetailView, ytdl_detail_view, GTK_TYPE_BOX)

/* ---------------------------------------------------------------------- */
/* Small helpers                                                          */
/* ---------------------------------------------------------------------- */

static void
clear_box (GtkWidget *box)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (box)) != NULL)
    gtk_box_remove (GTK_BOX (box), child);
}

static char *
format_clock (double seconds)
{
  if (seconds < 0)
    seconds = 0;
  int total = (int) seconds;
  if (total >= 3600)
    return g_strdup_printf ("%d:%02d:%02d", total / 3600, (total % 3600) / 60,
                            total % 60);
  return g_strdup_printf ("%d:%02d", total / 60, total % 60);
}

static char *
format_date (const char *yyyymmdd)
{
  static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May",
                                        "Jun", "Jul", "Aug", "Sep", "Oct",
                                        "Nov", "Dec" };
  if (yyyymmdd == NULL || strlen (yyyymmdd) != 8)
    return g_strdup (yyyymmdd != NULL ? yyyymmdd : "");
  for (int i = 0; i < 8; i++)
    if (!g_ascii_isdigit (yyyymmdd[i]))
      return g_strdup (yyyymmdd);
  int m = (yyyymmdd[4] - '0') * 10 + (yyyymmdd[5] - '0');
  if (m < 1 || m > 12)
    return g_strdup (yyyymmdd);
  return g_strdup_printf ("%c%c %s %.4s", yyyymmdd[6], yyyymmdd[7],
                          months[m - 1], yyyymmdd);
}

/* Thousands separators, done by hand.
 *
 * printf's "%'" grouping flag does nothing under the C locale, which is what
 * an app launched from a .desktop file usually gets -- so a view count came
 * out as 24913882. Grouping here rather than calling setlocale keeps the
 * change to the one place that wants it, instead of altering number and date
 * formatting for the whole process. */
static char *
format_count (gint64 n)
{
  g_autofree char *digits = g_strdup_printf ("%" G_GINT64_FORMAT, ABS (n));
  gsize len = strlen (digits);

  GString *out = g_string_new (n < 0 ? "-" : "");
  for (gsize i = 0; i < len; i++)
    {
      if (i > 0 && (len - i) % 3 == 0)
        g_string_append_c (out, ',');
      g_string_append_c (out, digits[i]);
    }
  return g_string_free (out, FALSE);
}

static GtkWidget *
kv_row (const char *key, const char *value, gboolean selectable)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *k = gtk_label_new (key);
  gtk_label_set_xalign (GTK_LABEL (k), 0.0f);
  gtk_widget_set_size_request (k, 150, -1);
  gtk_widget_set_valign (k, GTK_ALIGN_START);
  gtk_widget_add_css_class (k, "dim-label");
  gtk_box_append (GTK_BOX (row), k);

  GtkWidget *v = gtk_label_new (value != NULL && *value != '\0' ? value : "—");
  gtk_label_set_xalign (GTK_LABEL (v), 0.0f);
  gtk_label_set_wrap (GTK_LABEL (v), TRUE);
  gtk_label_set_selectable (GTK_LABEL (v), selectable);
  gtk_widget_set_hexpand (v, TRUE);
  gtk_box_append (GTK_BOX (row), v);
  return row;
}

static void
add_badge (GtkWidget *box, const char *text, const char *css)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_widget_add_css_class (l, "ytdl-pill");
  if (css != NULL)
    gtk_widget_add_css_class (l, css);
  gtk_box_append (GTK_BOX (box), l);
}

/* mpv first: it plays every codec combination yt-dlp can produce and reads
 * embedded subtitles and chapters, which is exactly what this pipeline's .mkv
 * output is full of. */
static void
open_externally (const char *path, gboolean prefer_player)
{
  if (path == NULL)
    return;

  if (prefer_player)
    {
      const char *const players[] = { "mpv", "vlc", NULL };
      for (gsize i = 0; players[i] != NULL; i++)
        {
          g_autofree char *exe = ytdl_which (players[i]);
          if (exe == NULL)
            continue;
          const char *argv[] = { exe, path, NULL };
          g_spawn_async (NULL, (char **) argv, NULL, G_SPAWN_DEFAULT, NULL,
                         NULL, NULL, NULL);
          return;
        }
    }

  g_autofree char *xdg = ytdl_which ("xdg-open");
  if (xdg == NULL)
    return;
  const char *argv[] = { xdg, path, NULL };
  g_spawn_async (NULL, (char **) argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL,
                 NULL);
}

/* ---------------------------------------------------------------------- */
/* Loading                                                                */
/* ---------------------------------------------------------------------- */

static void
detail_data_free (DetailData *d)
{
  if (d == NULL)
    return;
  g_free (d->key);
  g_free (d->dir);
  g_free (d->title);
  g_free (d->uploader);
  g_free (d->upload_date);
  g_free (d->channel);
  g_free (d->original_url);
  g_free (d->download_mode);
  g_free (d->media_path);
  g_free (d->subtitle_path);
  g_free (d->subtitle_name);
  g_free (d->description);
  g_free (d->tags);
  g_clear_pointer (&d->files, g_ptr_array_unref);
  g_clear_pointer (&d->probe, ytdl_probe_free);
  g_clear_pointer (&d->comments, g_ptr_array_unref);
  g_clear_pointer (&d->cues, g_ptr_array_unref);
  g_free (d);
}

static void render (YtdlDetailView *self, DetailData *d);

static gboolean
load_finished (gpointer user_data)
{
  DetailData *d = user_data;
  YtdlDetailView *self = d->view;

  /* Superseded by a later click. Dropping the result is the whole point of
   * the generation counter: rendering it would replace the page the user is
   * now looking at with the one they left. */
  if (d->generation != self->generation)
    {
      detail_data_free (d);
      g_object_unref (self);
      return G_SOURCE_REMOVE;
    }

  gtk_spinner_stop (GTK_SPINNER (self->spinner));
  gtk_widget_set_visible (self->spinner, FALSE);
  render (self, d);

  detail_data_free (d);
  g_object_unref (self);
  return G_SOURCE_REMOVE;
}

static gpointer
load_thread (gpointer user_data)
{
  DetailData *d = user_data;

  d->probe = ytdl_media_probe (d->media_path);

  g_autoptr (YtdlInfo) info = NULL;
  {
    /* ytdl_entry_load_info wants an entry; the worker only has the folder, so
     * a stack entry with just the fields it reads stands in. This is the one
     * place a path is assembled outside the index, and it is assembled from
     * the index's own dir, not from anything a caller supplied. */
    YtdlEntry stub = { 0 };
    stub.dir = d->dir;
    info = ytdl_entry_load_info (&stub);
  }

  if (info != NULL)
    {
      d->description = ytdl_info_string (info, "description");
      d->view_count = ytdl_info_int (info, "view_count");
      d->like_count = ytdl_info_int (info, "like_count");
      d->comments = ytdl_info_comments (info);

      if (json_object_has_member (info->root, "tags"))
        {
          JsonNode *n = json_object_get_member (info->root, "tags");
          if (JSON_NODE_HOLDS_ARRAY (n))
            {
              JsonArray *a = json_node_get_array (n);
              GString *s = g_string_new (NULL);
              for (guint i = 0; i < json_array_get_length (a) && i < 40; i++)
                {
                  const char *t = json_array_get_string_element (a, i);
                  if (t == NULL)
                    continue;
                  if (s->len > 0)
                    g_string_append (s, ", ");
                  g_string_append (s, t);
                }
              d->tags = g_string_free (s, FALSE);
            }
        }
    }
  if (d->comments == NULL)
    d->comments = g_ptr_array_new_with_free_func (ytdl_comment_free);

  d->comment_total = 0;
  for (guint i = 0; i < d->comments->len; i++)
    {
      const YtdlComment *c = g_ptr_array_index (d->comments, i);
      d->comment_total += 1 + c->replies->len;
    }

  if (d->subtitle_path != NULL)
    d->cues = ytdl_parse_subtitle_cues (d->subtitle_path);
  else
    d->cues = g_ptr_array_new_with_free_func (ytdl_cue_free);

  g_idle_add (load_finished, d);
  return NULL;
}

/* ---------------------------------------------------------------------- */
/* Rendering                                                              */
/* ---------------------------------------------------------------------- */

static void
render_streams (YtdlDetailView *self, DetailData *d)
{
  clear_box (self->streams_box);
  const YtdlProbe *p = d->probe;

  if (p == NULL || !p->ok)
    {
      GtkWidget *note = gtk_label_new (
          p != NULL && p->error != NULL ? p->error : "No media file.");
      gtk_label_set_xalign (GTK_LABEL (note), 0.0f);
      gtk_label_set_wrap (GTK_LABEL (note), TRUE);
      gtk_widget_add_css_class (note, "dim-label");
      gtk_box_append (GTK_BOX (self->streams_box), note);
      return;
    }

  gtk_box_append (GTK_BOX (self->streams_box),
                  kv_row ("Container", p->format, TRUE));
  if (p->bit_rate > 0)
    {
      g_autofree char *br =
          g_strdup_printf ("%.2f Mb/s overall", p->bit_rate / 1000000.0);
      gtk_box_append (GTK_BOX (self->streams_box), kv_row ("Bitrate", br, FALSE));
    }

  for (guint i = 0; i < p->streams->len; i++)
    {
      const YtdlStream *s = g_ptr_array_index (p->streams, i);
      GString *v = g_string_new (NULL);

      g_string_append (v, s->codec != NULL ? s->codec : "unknown codec");
      if (s->profile != NULL)
        g_string_append_printf (v, " (%s)", s->profile);
      if (s->width > 0 && s->height > 0)
        g_string_append_printf (v, " · %d×%d", s->width, s->height);
      if (s->fps > 0.01)
        g_string_append_printf (v, " · %.3g fps", s->fps);
      if (s->channels > 0)
        g_string_append_printf (v, " · %d ch", s->channels);
      if (s->sample_rate > 0)
        g_string_append_printf (v, " · %d Hz", s->sample_rate);
      if (s->bit_rate > 0)
        g_string_append_printf (v, " · %" G_GINT64_FORMAT " kb/s",
                                s->bit_rate / 1000);
      if (s->language != NULL)
        g_string_append_printf (v, " · %s", s->language);
      if (s->title != NULL)
        g_string_append_printf (v, " · \"%s\"", s->title);
      if (s->is_default)
        g_string_append (v, " · default");
      /* Called out because this is exactly what made the webview build serve
       * every thumbnail with a video MIME type: ffprobe reports an attached
       * cover as a one-frame video stream. */
      if (s->attached_pic)
        g_string_append (v, " · attached cover image, not a video track");

      g_autofree char *label =
          g_strdup_printf ("%s #%d", s->kind != NULL ? s->kind : "stream",
                           s->index);
      gtk_box_append (GTK_BOX (self->streams_box),
                      kv_row (label, v->str, TRUE));
      g_string_free (v, TRUE);
    }

  if (p->chapters->len > 0)
    {
      GtkWidget *head = gtk_label_new (NULL);
      g_autofree char *ct =
          g_strdup_printf ("%u chapters", p->chapters->len);
      gtk_label_set_text (GTK_LABEL (head), ct);
      gtk_label_set_xalign (GTK_LABEL (head), 0.0f);
      gtk_widget_set_margin_top (head, 8);
      gtk_widget_add_css_class (head, "heading");
      gtk_box_append (GTK_BOX (self->streams_box), head);

      for (guint i = 0; i < p->chapters->len; i++)
        {
          const YtdlChapter *c = g_ptr_array_index (p->chapters, i);
          g_autofree char *at = format_clock (c->start);
          gtk_box_append (GTK_BOX (self->streams_box),
                          kv_row (at, c->title, TRUE));
        }
    }
}

static void
render_meta (YtdlDetailView *self, DetailData *d)
{
  clear_box (self->meta_box);

  g_autofree char *date = format_date (d->upload_date);
  gtk_box_append (GTK_BOX (self->meta_box), kv_row ("Channel", d->uploader, TRUE));
  gtk_box_append (GTK_BOX (self->meta_box), kv_row ("Uploaded", date, FALSE));

  if (d->view_count > 0)
    {
      g_autofree char *v = format_count (d->view_count);
      gtk_box_append (GTK_BOX (self->meta_box), kv_row ("Views", v, FALSE));
    }
  if (d->like_count > 0)
    {
      g_autofree char *v = format_count (d->like_count);
      gtk_box_append (GTK_BOX (self->meta_box), kv_row ("Likes", v, FALSE));
    }
  if (d->original_url != NULL)
    gtk_box_append (GTK_BOX (self->meta_box),
                    kv_row ("Source", d->original_url, TRUE));

  g_autofree char *layout =
      d->layout_version == 0
          ? g_strdup ("1 (predates versioning)")
          : g_strdup_printf ("%" G_GUINT64_FORMAT, d->layout_version);
  gtk_box_append (GTK_BOX (self->meta_box), kv_row ("Archive layout", layout, FALSE));
  if (d->download_mode != NULL)
    gtk_box_append (GTK_BOX (self->meta_box),
                    kv_row ("Download mode", d->download_mode, FALSE));
  if (d->tags != NULL && *d->tags != '\0')
    gtk_box_append (GTK_BOX (self->meta_box), kv_row ("Tags", d->tags, TRUE));

  gtk_box_append (GTK_BOX (self->meta_box), kv_row ("Folder", d->dir, TRUE));

  if (d->description != NULL && *d->description != '\0')
    {
      GtkWidget *head = gtk_label_new ("Description");
      gtk_label_set_xalign (GTK_LABEL (head), 0.0f);
      gtk_widget_set_margin_top (head, 10);
      gtk_widget_add_css_class (head, "heading");
      gtk_box_append (GTK_BOX (self->meta_box), head);

      GtkWidget *desc = gtk_label_new (d->description);
      gtk_label_set_xalign (GTK_LABEL (desc), 0.0f);
      gtk_label_set_wrap (GTK_LABEL (desc), TRUE);
      gtk_label_set_selectable (GTK_LABEL (desc), TRUE);
      gtk_box_append (GTK_BOX (self->meta_box), desc);
    }
}

static GtkWidget *
comment_widget (const YtdlComment *c, gboolean is_reply)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  /* A rule down the left rather than an indent alone, so a long thread still
   * reads as one conversation once the replies wrap. */
  if (is_reply)
    {
      gtk_widget_set_margin_start (box, 24);
      gtk_widget_add_css_class (box, "ytdl-reply");
    }
  gtk_widget_set_margin_bottom (box, is_reply ? 4 : 10);

  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *author = gtk_label_new (c->author);
  gtk_label_set_xalign (GTK_LABEL (author), 0.0f);
  gtk_widget_add_css_class (author, "heading");
  gtk_widget_add_css_class (author, "caption");
  gtk_box_append (GTK_BOX (head), author);

  if (c->author_is_uploader)
    add_badge (head, "uploader", "accent");
  if (c->is_pinned)
    add_badge (head, "pinned", "accent");
  if (c->is_favorited)
    add_badge (head, "hearted", "accent");
  if (c->like_count > 0)
    {
      g_autofree char *n = format_count (c->like_count);
      g_autofree char *likes = g_strdup_printf ("%s likes", n);
      add_badge (head, likes, NULL);
    }
  if (c->time_text != NULL)
    add_badge (head, c->time_text, NULL);
  gtk_box_append (GTK_BOX (box), head);

  GtkWidget *text = gtk_label_new (c->text);
  gtk_label_set_xalign (GTK_LABEL (text), 0.0f);
  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_label_set_selectable (GTK_LABEL (text), TRUE);
  gtk_box_append (GTK_BOX (box), text);

  return box;
}

static void
render_comments (YtdlDetailView *self, DetailData *d)
{
  clear_box (self->comments_box);

  if (d->comments->len == 0)
    {
      const char *why =
          g_strcmp0 (d->download_mode, "metadata-only") == 0 ||
                  g_strcmp0 (d->download_mode, "subs-only") == 0
              ? "This video was fetched in a mode that does not capture "
                "comments."
              : "No comments were captured for this video. That is what "
                "--no-comments produces, and also what a video with comments "
                "disabled produces.";
      GtkWidget *note = gtk_label_new (why);
      gtk_label_set_xalign (GTK_LABEL (note), 0.0f);
      gtk_label_set_wrap (GTK_LABEL (note), TRUE);
      gtk_widget_add_css_class (note, "dim-label");
      gtk_box_append (GTK_BOX (self->comments_box), note);
      return;
    }

  guint shown = MIN (d->comments->len, MAX_COMMENTS_SHOWN);
  g_autofree char *count = g_strdup_printf (
      "%" G_GSIZE_FORMAT " comments including replies · %u threads%s",
      d->comment_total, d->comments->len,
      shown < d->comments->len ? " (showing the first 200)" : "");
  GtkWidget *head = gtk_label_new (count);
  gtk_label_set_xalign (GTK_LABEL (head), 0.0f);
  gtk_widget_add_css_class (head, "dim-label");
  gtk_widget_add_css_class (head, "caption");
  gtk_widget_set_margin_bottom (head, 8);
  gtk_box_append (GTK_BOX (self->comments_box), head);

  for (guint i = 0; i < shown; i++)
    {
      const YtdlComment *c = g_ptr_array_index (d->comments, i);
      gtk_box_append (GTK_BOX (self->comments_box), comment_widget (c, FALSE));
      for (guint r = 0; r < c->replies->len; r++)
        gtk_box_append (GTK_BOX (self->comments_box),
                        comment_widget (g_ptr_array_index (c->replies, r), TRUE));
    }
}

static void
render_transcript (YtdlDetailView *self, DetailData *d)
{
  GtkTextBuffer *buf =
      gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->transcript));

  if (d->subtitle_path == NULL)
    {
      gtk_text_buffer_set_text (
          buf,
          "No subtitle track in this folder. --no-subs produces that, and so "
          "does a video with no captions available.",
          -1);
      gtk_label_set_text (GTK_LABEL (self->transcript_note), "");
      return;
    }
  if (d->cues->len == 0)
    {
      gtk_text_buffer_set_text (
          buf, "The subtitle file is present but produced no readable cues.",
          -1);
      return;
    }

  GString *s = g_string_new (NULL);
  for (guint i = 0; i < d->cues->len; i++)
    {
      const YtdlCue *c = g_ptr_array_index (d->cues, i);
      g_autofree char *at = format_clock (c->start);
      g_string_append_printf (s, "[%s]  %s\n", at, c->text);
    }
  gtk_text_buffer_set_text (buf, s->str, (int) s->len);
  g_string_free (s, TRUE);

  g_autofree char *note = g_strdup_printf (
      "%s · %u lines%s", d->subtitle_name, d->cues->len,
      d->subtitle_is_auto ? " · auto-generated, rolling duplication collapsed"
                          : " · uploaded track");
  gtk_label_set_text (GTK_LABEL (self->transcript_note), note);
}

static void
render_files (YtdlDetailView *self, DetailData *d)
{
  clear_box (self->files_box);

  for (guint i = 0; i < d->files->len; i++)
    {
      const char *packed = g_ptr_array_index (d->files, i);
      const char *tab = strchr (packed, '\t');
      if (tab == NULL)
        continue;
      g_autofree char *rel = g_strndup (packed, (gsize) (tab - packed));
      gtk_box_append (GTK_BOX (self->files_box), kv_row (rel, tab + 1, TRUE));
    }
}

static void
render (YtdlDetailView *self, DetailData *d)
{
  g_autofree char *date = format_date (d->upload_date);
  g_autofree char *sub =
      g_strdup_printf ("%s%s%s", d->uploader != NULL ? d->uploader : "",
                       (date != NULL && *date != '\0') ? " · " : "", date);
  gtk_label_set_text (GTK_LABEL (self->subtitle), sub);

  g_autofree char *summary = ytdl_probe_summary (d->probe);
  gtk_label_set_text (GTK_LABEL (self->summary), summary);

  clear_box (self->badges);
  if (d->layout_too_new)
    add_badge (self->badges, "written with a newer archive layout", "warn");
  if (d->media_path == NULL)
    add_badge (self->badges,
               d->download_mode != NULL ? d->download_mode : "no media file",
               NULL);

  render_streams (self, d);
  render_meta (self, d);
  render_comments (self, d);
  render_transcript (self, d);
  render_files (self, d);

  /* The player is loaded last and never auto-played. Opening a page should
   * not start making noise. */
  if (self->video != NULL && d->media_path != NULL)
    {
      gtk_widget_set_visible (self->video_holder, TRUE);
      gtk_widget_set_visible (self->no_player_note, FALSE);
      g_autoptr (GFile) f = g_file_new_for_path (d->media_path);
      gtk_video_set_file (GTK_VIDEO (self->video), f);
    }
  else
    {
      gtk_widget_set_visible (self->video_holder, FALSE);
      gtk_widget_set_visible (self->no_player_note, d->media_path != NULL);
    }
}

/* ---------------------------------------------------------------------- */
/* Actions                                                                */
/* ---------------------------------------------------------------------- */

static void
on_open_player (GtkButton *b, gpointer user_data)
{
  YtdlDetailView *self = user_data;
  open_externally (self->media_path, TRUE);
}

static void
on_open_folder (GtkButton *b, gpointer user_data)
{
  YtdlDetailView *self = user_data;
  open_externally (self->folder, FALSE);
}

typedef struct
{
  YtdlDetailView     *view;
  char               *dir;
  YtdlChecksumResult *result;
} VerifyJob;

static gboolean
verify_finished (gpointer user_data)
{
  VerifyJob *j = user_data;
  YtdlChecksumResult *r = j->result;

  g_autofree char *text = NULL;
  const char *css = "dim-label";
  if (r == NULL || !r->present)
    {
      text = g_strdup ("No checksums.sha256 in this folder.");
    }
  else if (r->failed->len == 0 && r->missing->len == 0)
    {
      text = g_strdup_printf ("All %" G_GSIZE_FORMAT " files verify.",
                              r->checked);
      css = "success";
    }
  else
    {
      /* video_postprocessing.log is EXCLUDED from checksums.sha256 by
       * postprocess.ps1 because it is still being appended to when the hashes
       * are computed -- so it is never one of these, and anything listed here
       * is a real mismatch. */
      text = g_strdup_printf (
          "%" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT " verify · %u failed · "
          "%u missing",
          r->ok, r->checked, r->failed->len, r->missing->len);
      css = "error";
    }

  gtk_label_set_text (GTK_LABEL (j->view->verify_result), text);
  gtk_widget_remove_css_class (j->view->verify_result, "success");
  gtk_widget_remove_css_class (j->view->verify_result, "error");
  gtk_widget_add_css_class (j->view->verify_result, css);

  g_clear_pointer (&j->result, ytdl_checksum_result_free);
  g_object_unref (j->view);
  g_free (j->dir);
  g_free (j);
  return G_SOURCE_REMOVE;
}

/* Hashing every file in a folder is seconds of work on a large video, so it
 * goes on a thread like everything else that touches the disk in bulk. */
static gpointer
verify_thread (gpointer user_data)
{
  VerifyJob *j = user_data;
  j->result = ytdl_health_verify_checksums (j->dir);
  g_idle_add (verify_finished, j);
  return NULL;
}

static void
on_verify (GtkButton *b, gpointer user_data)
{
  YtdlDetailView *self = user_data;
  if (self->folder == NULL)
    return;

  gtk_label_set_text (GTK_LABEL (self->verify_result), "Verifying…");

  VerifyJob *j = g_new0 (VerifyJob, 1);
  j->view = g_object_ref (self);
  j->dir = g_strdup (self->folder);
  GThread *t = g_thread_new ("ytdl-verify", verify_thread, j);
  g_thread_unref (t);
}

/* ---------------------------------------------------------------------- */
/* Public                                                                 */
/* ---------------------------------------------------------------------- */

void
ytdl_detail_view_clear (YtdlDetailView *self)
{
  g_return_if_fail (YTDL_IS_DETAIL_VIEW (self));
  if (self->video != NULL)
    gtk_video_set_file (GTK_VIDEO (self->video), NULL);
  self->generation++;
}

void
ytdl_detail_view_show (YtdlDetailView *self, const YtdlEntry *entry)
{
  g_return_if_fail (YTDL_IS_DETAIL_VIEW (self));
  g_return_if_fail (entry != NULL);

  if (self->video != NULL)
    gtk_video_set_file (GTK_VIDEO (self->video), NULL);

  DetailData *d = g_new0 (DetailData, 1);
  d->view = g_object_ref (self);
  d->generation = ++self->generation;
  d->key = g_strdup (entry->key);
  d->dir = g_strdup (entry->dir);
  d->title = g_strdup (entry->title);
  d->uploader = g_strdup (entry->uploader);
  d->upload_date = g_strdup (entry->upload_date);
  d->channel = g_strdup (entry->channel);
  d->original_url = g_strdup (entry->original_url);
  d->download_mode = g_strdup (entry->download_mode);
  d->layout_version = entry->layout_version;
  d->layout_too_new = entry->layout_too_new;
  d->view_count = -1;
  d->like_count = -1;

  gssize mi = ytdl_entry_media_index (entry);
  if (mi >= 0)
    d->media_path = ytdl_entry_path_for_index (entry, (gsize) mi);

  /* Pick a subtitle track: a human-written one wins over an auto-generated
   * one, since the whole reason the auto/human distinction is read from file
   * CONTENTS is that it matters which you are reading. */
  d->files = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < entry->files->len; i++)
    {
      const YtdlFile *f = g_ptr_array_index (entry->files, i);
      g_autofree char *size = g_format_size (f->size);
      g_ptr_array_add (d->files, g_strdup_printf ("%s\t%s", f->rel, size));

      if (!ytdl_ext_is_subtitle (f->ext))
        continue;
      g_autofree char *path = ytdl_entry_path_for_index (entry, i);
      if (path == NULL)
        continue;
      gboolean is_auto = ytdl_subtitle_is_auto (path);
      if (d->subtitle_path == NULL || (d->subtitle_is_auto && !is_auto))
        {
          g_free (d->subtitle_path);
          g_free (d->subtitle_name);
          d->subtitle_path = g_steal_pointer (&path);
          d->subtitle_name = g_strdup (f->rel);
          d->subtitle_is_auto = is_auto;
        }
    }

  g_free (self->media_path);
  self->media_path = g_strdup (d->media_path);
  g_free (self->folder);
  self->folder = g_strdup (d->dir);

  gtk_label_set_text (GTK_LABEL (self->subtitle),
                      entry->title != NULL ? entry->title : "(untitled)");
  gtk_label_set_text (GTK_LABEL (self->summary), "Reading…");
  gtk_label_set_text (GTK_LABEL (self->verify_result), "");
  gtk_widget_set_visible (self->spinner, TRUE);
  gtk_spinner_start (GTK_SPINNER (self->spinner));

  GThread *t = g_thread_new ("ytdl-detail", load_thread, d);
  g_thread_unref (t);
}

/* ---------------------------------------------------------------------- */

/* Clamped, because these are columns of text. Unclamped, a key/value row on a
 * 1800px window puts the key at the left edge and the value 150px away with a
 * metre of nothing after it, and a paragraph of description runs to a line
 * length nobody can track back from. */
static GtkWidget *
scrolled_section (GtkWidget *child)
{
  gtk_widget_set_margin_start (child, 12);
  gtk_widget_set_margin_end (child, 12);
  gtk_widget_set_margin_top (child, 12);
  gtk_widget_set_margin_bottom (child, 18);

  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 860);
  adw_clamp_set_tightening_threshold (ADW_CLAMP (clamp), 660);
  adw_clamp_set_child (ADW_CLAMP (clamp), child);

  GtkWidget *s = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (s), clamp);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (s), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (s, TRUE);
  return s;
}

static void
ytdl_detail_view_dispose (GObject *object)
{
  YtdlDetailView *self = YTDL_DETAIL_VIEW (object);
  g_clear_pointer (&self->media_path, g_free);
  g_clear_pointer (&self->folder, g_free);
  G_OBJECT_CLASS (ytdl_detail_view_parent_class)->dispose (object);
}

static void
ytdl_detail_view_class_init (YtdlDetailViewClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = ytdl_detail_view_dispose;
}

static void
ytdl_detail_view_init (YtdlDetailView *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 8);

  /* --- header --- */
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start (head, 12);
  gtk_widget_set_margin_end (head, 12);
  gtk_widget_set_margin_top (head, 10);

  /* NO TITLE LABEL HERE. The title is the AdwNavigationPage's, so it is in
   * the header bar where a GNOME app puts it -- and where the back button,
   * the page transition and the window title all agree with it. Repeating it
   * in the content was the arrangement a hand-rolled stack needed. */
  GtkWidget *title_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

  self->subtitle = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->subtitle), 0.0f);
  gtk_label_set_wrap (GTK_LABEL (self->subtitle), TRUE);
  gtk_widget_set_hexpand (self->subtitle, TRUE);
  gtk_widget_add_css_class (self->subtitle, "heading");
  gtk_box_append (GTK_BOX (title_row), self->subtitle);

  self->spinner = gtk_spinner_new ();
  gtk_widget_set_visible (self->spinner, FALSE);
  gtk_widget_set_valign (self->spinner, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (title_row), self->spinner);
  gtk_box_append (GTK_BOX (head), title_row);

  self->summary = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->summary), 0.0f);
  gtk_widget_add_css_class (self->summary, "caption");
  gtk_widget_add_css_class (self->summary, "dim-label");
  gtk_box_append (GTK_BOX (head), self->summary);

  self->badges = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (head), self->badges);
  gtk_box_append (GTK_BOX (self), head);

  /* --- player --- */
  self->video_holder = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start (self->video_holder, 12);
  gtk_widget_set_margin_end (self->video_holder, 12);

  if (ytdl_media_playback_available ())
    {
      self->video = gtk_video_new ();
      gtk_video_set_autoplay (GTK_VIDEO (self->video), FALSE);
      gtk_widget_set_size_request (self->video, -1, 320);
      gtk_box_append (GTK_BOX (self->video_holder), self->video);
    }
  gtk_widget_set_visible (self->video_holder, FALSE);

  self->no_player_note = gtk_label_new (
      "No GStreamer media backend is installed, so the video cannot play in "
      "this window. The file itself is fine — open it in mpv.");
  gtk_label_set_wrap (GTK_LABEL (self->no_player_note), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->no_player_note), 0.0f);
  gtk_widget_set_margin_start (self->no_player_note, 12);
  gtk_widget_set_margin_end (self->no_player_note, 12);
  gtk_widget_add_css_class (self->no_player_note, "dim-label");
  gtk_widget_set_visible (self->no_player_note, FALSE);
  gtk_box_append (GTK_BOX (self), self->no_player_note);

  /* The player and the tabs share the space through a draggable split.
   *
   * Packed directly into the box, GtkVideo asks for the video's natural size
   * -- 854x480 becomes 646 pixels tall once it is stretched to the window
   * width -- and the tabs below it get whatever is left, which was two rows.
   * A GtkPaned constrains it to the split instead, and lets someone who wants
   * a bigger picture drag for one. */
  GtkWidget *split = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_start_child (GTK_PANED (split), self->video_holder);
  gtk_paned_set_resize_start_child (GTK_PANED (split), FALSE);
  gtk_paned_set_shrink_start_child (GTK_PANED (split), TRUE);
  gtk_paned_set_position (GTK_PANED (split), 300);
  gtk_widget_set_vexpand (split, TRUE);
  gtk_box_append (GTK_BOX (self), split);

  /* --- actions --- */
  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start (actions, 12);
  gtk_widget_set_margin_end (actions, 12);

  GtkWidget *play = gtk_button_new_with_label ("Open in mpv");
  gtk_widget_set_tooltip_text (
      play, "mpv plays every codec combination this pipeline produces and "
            "reads the embedded subtitles and chapters.");
  g_signal_connect (play, "clicked", G_CALLBACK (on_open_player), self);
  gtk_box_append (GTK_BOX (actions), play);

  GtkWidget *folder = gtk_button_new_with_label ("Open folder");
  g_signal_connect (folder, "clicked", G_CALLBACK (on_open_folder), self);
  gtk_box_append (GTK_BOX (actions), folder);

  GtkWidget *verify = gtk_button_new_with_label ("Verify checksums");
  gtk_widget_set_tooltip_text (
      verify, "Re-hashes every file in this folder against the "
              "checksums.sha256 postprocess.ps1 wrote.");
  g_signal_connect (verify, "clicked", G_CALLBACK (on_verify), self);
  gtk_box_append (GTK_BOX (actions), verify);

  self->verify_result = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->verify_result), 0.0f);
  gtk_widget_set_hexpand (self->verify_result, TRUE);
  gtk_widget_add_css_class (self->verify_result, "caption");
  gtk_box_append (GTK_BOX (actions), self->verify_result);

  GtkWidget *lower = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top (lower, 8);
  gtk_box_append (GTK_BOX (lower), actions);
  gtk_paned_set_end_child (GTK_PANED (split), lower);

  /* --- sections --- */
  self->streams_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  self->meta_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  self->comments_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  self->files_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

  self->transcript = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (self->transcript), FALSE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (self->transcript),
                               GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (self->transcript), FALSE);

  GtkWidget *tbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  self->transcript_note = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->transcript_note), 0.0f);
  gtk_widget_add_css_class (self->transcript_note, "caption");
  gtk_widget_add_css_class (self->transcript_note, "dim-label");
  gtk_box_append (GTK_BOX (tbox), self->transcript_note);
  gtk_box_append (GTK_BOX (tbox), self->transcript);

  /* AdwViewStack and AdwViewSwitcher, not the GTK pair.
   *
   * The switcher takes an icon per page as well as a label, and it collapses
   * to icons on its own when the window is too narrow for five words -- which
   * is the whole difference between a row of tabs that adapts and a row that
   * clips. */
  self->stack = adw_view_stack_new ();
  adw_view_stack_add_titled_with_icon (
      ADW_VIEW_STACK (self->stack), scrolled_section (self->meta_box), "meta",
      "Details", "dialog-information-symbolic");
  adw_view_stack_add_titled_with_icon (
      ADW_VIEW_STACK (self->stack), scrolled_section (self->streams_box),
      "streams", "Media", "media-playback-start-symbolic");
  adw_view_stack_add_titled_with_icon (
      ADW_VIEW_STACK (self->stack), scrolled_section (self->comments_box),
      "comments", "Comments", "user-available-symbolic");
  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (self->stack),
                                       scrolled_section (tbox), "transcript",
                                       "Transcript",
                                       "utilities-terminal-symbolic");
  adw_view_stack_add_titled_with_icon (
      ADW_VIEW_STACK (self->stack), scrolled_section (self->files_box),
      "files", "Files", "folder-symbolic");
  gtk_widget_set_vexpand (self->stack, TRUE);

  GtkWidget *switcher = adw_view_switcher_new ();
  adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher),
                               ADW_VIEW_STACK (self->stack));
  adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher),
                                ADW_VIEW_SWITCHER_POLICY_WIDE);

  /* halign FILL, and this is not cosmetic. With halign CENTER the switcher
   * gets its NATURAL width and is centred in it, so when five tabs want more
   * room than the pane has it overflows and the last one is sliced through
   * the middle of a glyph -- which is exactly what happened at 470px. Given
   * the real allocation instead, it shrinks its items and ellipsises their
   * labels, which is the behaviour it already had and was being denied.
   *
   * The clamp is what keeps FILL from stretching five tabs across a 1600px
   * window: below 640 it fills, above it centres. */
  gtk_widget_set_halign (switcher, GTK_ALIGN_FILL);

  GtkWidget *switcher_clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (switcher_clamp), 640);
  adw_clamp_set_child (ADW_CLAMP (switcher_clamp), switcher);

  /* AdwViewSwitcher does NOT adapt on its own -- WIDE and NARROW are two
   * fixed layouts, and WIDE ellipsised to "Com…"/"Tran…" long before it had
   * to. The window's own AdwBreakpoint cannot reach this widget: it is three
   * pages down and does not know the window exists.
   *
   * AdwBreakpointBin is the answer -- a breakpoint scoped to a WIDGET,
   * measured against this bin's own allocation. Below 620sp each icon stacks
   * over its label, which fits all five down to about 560px -- the practical
   * floor for this page, set by the player and the metadata rows rather than
   * by the tabs. It needs an explicit minimum size for the same reason the
   * window does: it has to know the smallest allocation it can be asked to
   * lay out. */
  GtkWidget *switcher_bin = adw_breakpoint_bin_new ();
  gtk_widget_set_size_request (switcher_bin, 120, 42);
  adw_breakpoint_bin_set_child (ADW_BREAKPOINT_BIN (switcher_bin),
                                switcher_clamp);

  AdwBreakpoint *narrow = adw_breakpoint_new (
      adw_breakpoint_condition_parse ("max-width: 620sp"));
  adw_breakpoint_add_setters (narrow, G_OBJECT (switcher), "policy",
                              ADW_VIEW_SWITCHER_POLICY_NARROW, NULL);
  adw_breakpoint_bin_add_breakpoint (ADW_BREAKPOINT_BIN (switcher_bin),
                                     narrow);
  gtk_box_append (GTK_BOX (lower), switcher_bin);
  gtk_box_append (GTK_BOX (lower), self->stack);
}

GtkWidget *
ytdl_detail_view_new (void)
{
  return g_object_new (YTDL_TYPE_DETAIL_VIEW, NULL);
}
