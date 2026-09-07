#include "health_view.h"

#include "health.h"
#include "paths.h"

#include <string.h>

struct _YtdlHealthView
{
  GtkBox parent_instance;

  YtdlSettings *settings; /* borrowed */
  YtdlIndex    *index;    /* borrowed, may be NULL */

  GtkWidget *deps_box;
  GtkWidget *deps_spinner;
  GtkWidget *files_box;
  GtkWidget *config_box;
  GtkWidget *stats_box;
  GtkWidget *log_view;
  GtkWidget *log_choice;
  GtkWidget *refresh;

  gboolean probing;
};

G_DEFINE_FINAL_TYPE (YtdlHealthView, ytdl_health_view, GTK_TYPE_BOX)

/* ---------------------------------------------------------------------- */
/* Small builders                                                         */
/* ---------------------------------------------------------------------- */

static void
clear_box (GtkWidget *box)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (box)) != NULL)
    gtk_box_remove (GTK_BOX (box), child);
}

static GtkWidget *
kv_row (const char *key, const char *value, const char *value_class)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);

  GtkWidget *k = gtk_label_new (key);
  gtk_label_set_xalign (GTK_LABEL (k), 0.0f);
  gtk_widget_set_size_request (k, 180, -1);
  gtk_widget_add_css_class (k, "dim-label");
  gtk_box_append (GTK_BOX (row), k);

  GtkWidget *v = gtk_label_new (value != NULL ? value : "—");
  gtk_label_set_xalign (GTK_LABEL (v), 0.0f);
  gtk_label_set_selectable (GTK_LABEL (v), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (v), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand (v, TRUE);
  if (value_class != NULL)
    gtk_widget_add_css_class (v, value_class);
  gtk_box_append (GTK_BOX (row), v);

  return row;
}

static GtkWidget *
section (const char *title, GtkWidget *content, GtkWidget *trailing)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_bottom (box, 10);

  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *l = gtk_label_new (title);
  gtk_label_set_xalign (GTK_LABEL (l), 0.0f);
  gtk_widget_set_hexpand (l, TRUE);
  gtk_widget_add_css_class (l, "heading");
  gtk_box_append (GTK_BOX (head), l);
  if (trailing != NULL)
    gtk_box_append (GTK_BOX (head), trailing);
  gtk_box_append (GTK_BOX (box), head);
  gtk_box_append (GTK_BOX (box), content);

  return box;
}

/* ---------------------------------------------------------------------- */
/* Dependencies -- the slow half, on a worker thread                      */
/* ---------------------------------------------------------------------- */

typedef struct
{
  YtdlHealthView *view;
  gboolean        force;
  GPtrArray      *result;
} ProbeJob;

static GtkWidget *
dep_row (const YtdlDependency *d)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class (row, "card");
  gtk_widget_set_margin_bottom (row, 4);

  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

  GtkWidget *name = gtk_label_new (d->name);
  gtk_label_set_xalign (GTK_LABEL (name), 0.0f);
  gtk_widget_set_size_request (name, 90, -1);
  gtk_widget_add_css_class (name, "heading");
  gtk_box_append (GTK_BOX (top), name);

  const char *mark = d->found ? "found" : "missing";
  GtkWidget *state = gtk_label_new (mark);
  gtk_widget_add_css_class (state, "caption");
  if (d->found)
    gtk_widget_add_css_class (state, "success");
  else if (g_strcmp0 (d->importance, "required") == 0)
    gtk_widget_add_css_class (state, "error");
  else
    gtk_widget_add_css_class (state, "warning");
  gtk_box_append (GTK_BOX (top), state);

  GtkWidget *imp = gtk_label_new (d->importance);
  gtk_widget_add_css_class (imp, "caption");
  gtk_widget_add_css_class (imp, "dim-label");
  gtk_box_append (GTK_BOX (top), imp);

  /* Found with no version means the probe hit its 8-second timeout, which is
   * a different thing from missing and is worth saying rather than showing a
   * blank. */
  const char *ver =
      d->version != NULL ? d->version
                         : (d->found ? "version probe timed out" : NULL);
  if (ver != NULL)
    {
      GtkWidget *v = gtk_label_new (ver);
      gtk_label_set_ellipsize (GTK_LABEL (v), PANGO_ELLIPSIZE_END);
      gtk_label_set_selectable (GTK_LABEL (v), TRUE);
      gtk_widget_set_hexpand (v, TRUE);
      gtk_widget_add_css_class (v, "caption");
      gtk_widget_add_css_class (v, "monospace");
      gtk_box_append (GTK_BOX (top), v);
    }
  gtk_box_append (GTK_BOX (row), top);

  /* The note only earns its space when something is wrong. On a healthy
   * machine seven paragraphs of explanation is just noise to scroll past. */
  if (!d->found)
    {
      GtkWidget *note = gtk_label_new (d->note);
      gtk_label_set_xalign (GTK_LABEL (note), 0.0f);
      gtk_label_set_wrap (GTK_LABEL (note), TRUE);
      gtk_widget_add_css_class (note, "caption");
      gtk_widget_add_css_class (note, "dim-label");
      gtk_box_append (GTK_BOX (row), note);
    }
  else if (d->path != NULL)
    {
      GtkWidget *p = gtk_label_new (d->path);
      gtk_label_set_xalign (GTK_LABEL (p), 0.0f);
      gtk_label_set_ellipsize (GTK_LABEL (p), PANGO_ELLIPSIZE_MIDDLE);
      gtk_label_set_selectable (GTK_LABEL (p), TRUE);
      gtk_widget_add_css_class (p, "caption");
      gtk_widget_add_css_class (p, "dim-label");
      gtk_box_append (GTK_BOX (row), p);
    }

  return row;
}

static gboolean
probe_finished (gpointer data)
{
  ProbeJob *job = data;
  YtdlHealthView *self = job->view;

  clear_box (self->deps_box);
  for (guint i = 0; i < job->result->len; i++)
    gtk_box_append (GTK_BOX (self->deps_box),
                    dep_row (g_ptr_array_index (job->result, i)));

  gtk_spinner_stop (GTK_SPINNER (self->deps_spinner));
  gtk_widget_set_visible (self->deps_spinner, FALSE);
  gtk_widget_set_sensitive (self->refresh, TRUE);
  self->probing = FALSE;

  g_ptr_array_unref (job->result);
  g_object_unref (self);
  g_free (job);
  return G_SOURCE_REMOVE;
}

/* Seven subprocess spawns, each with an 8-second ceiling. Off the UI thread,
 * for the same reason the archive scan is: the pane must draw before it knows
 * the answers, or it looks broken rather than busy. */
static gpointer
probe_thread (gpointer data)
{
  ProbeJob *job = data;
  job->result = ytdl_health_dependencies (job->force);
  g_idle_add (probe_finished, job);
  return NULL;
}

/* ---------------------------------------------------------------------- */
/* The cheap half -- straight file reads                                  */
/* ---------------------------------------------------------------------- */

static char *
format_when (gint64 unix_secs)
{
  if (unix_secs <= 0)
    return g_strdup ("—");
  g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (unix_secs);
  return dt != NULL ? g_date_time_format (dt, "%Y-%m-%d %H:%M") : g_strdup ("—");
}

static void
fill_installed (YtdlHealthView *self)
{
  clear_box (self->files_box);
  g_autoptr (GPtrArray) files = ytdl_health_installed_files ();

  for (guint i = 0; i < files->len; i++)
    {
      const YtdlInstalledFile *f = g_ptr_array_index (files, i);
      g_autofree char *when = format_when (f->modified);
      g_autofree char *size = g_format_size (f->size);
      g_autofree char *value =
          f->present ? g_strdup_printf ("%s · %s · %s", size, when, f->path)
                     : g_strdup_printf ("not installed — %s", f->path);
      gtk_box_append (GTK_BOX (self->files_box),
                      kv_row (f->name, value,
                              f->present ? "caption" : "error"));
    }
}

static void
fill_config (YtdlHealthView *self)
{
  clear_box (self->config_box);
  g_autoptr (YtdlConfigInfo) info = ytdl_health_config_info ();

  gtk_box_append (GTK_BOX (self->config_box),
                  kv_row ("CONFIG_VERSION",
                          info->config_version != NULL ? info->config_version
                                                       : "not found",
                          "monospace"));
  g_autofree char *count = g_strdup_printf ("%" G_GSIZE_FORMAT, info->option_count);
  gtk_box_append (GTK_BOX (self->config_box), kv_row ("Options set", count, NULL));
  gtk_box_append (GTK_BOX (self->config_box),
                  kv_row ("Path", info->path, "caption"));

  if (!info->present)
    gtk_box_append (
        GTK_BOX (self->config_box),
        kv_row ("", "yt-dlp.conf is not installed. Downloads will run with "
                    "yt-dlp's own defaults rather than this pipeline's.",
                "error"));
}

static void
fill_stats (YtdlHealthView *self)
{
  clear_box (self->stats_box);

  gsize videos = 0, channels = 0;
  guint64 bytes = 0;
  if (self->index != NULL)
    ytdl_index_stats (self->index, &videos, &channels, &bytes);

  g_autofree char *data_root = ytdl_settings_resolved_data_root (self->settings);
  g_autoptr (YtdlArchiveStats) s =
      ytdl_health_archive_stats (data_root, videos, channels, bytes);

  g_autofree char *size = g_format_size (s->total_bytes);
  g_autofree char *counts =
      g_strdup_printf ("%" G_GSIZE_FORMAT " videos · %" G_GSIZE_FORMAT
                       " channels · %s",
                       s->videos, s->channels, size);
  gtk_box_append (GTK_BOX (self->stats_box), kv_row ("Indexed", counts, NULL));
  gtk_box_append (GTK_BOX (self->stats_box),
                  kv_row ("Data root", s->data_root, "caption"));

  g_autofree char *gm =
      s->global_manifest_entries >= 0
          ? g_strdup_printf ("%" G_GSSIZE_FORMAT, s->global_manifest_entries)
          : g_strdup ("not readable");
  gtk_box_append (GTK_BOX (self->stats_box),
                  kv_row ("global_manifest.json", gm, NULL));

  g_autofree char *ids =
      s->archive_txt_ids >= 0
          ? g_strdup_printf ("%" G_GSSIZE_FORMAT, s->archive_txt_ids)
          : g_strdup ("not readable");
  gtk_box_append (GTK_BOX (self->stats_box), kv_row ("archive.txt ids", ids, NULL));

  g_autofree char *snaps =
      g_strdup_printf ("%" G_GSIZE_FORMAT, s->history_snapshots);
  gtk_box_append (GTK_BOX (self->stats_box),
                  kv_row ("Archive History snapshots", snaps, NULL));

  /* The index counts what this app walked; global_manifest.json is what the
   * pipeline wrote. They disagreeing is the single most useful signal on this
   * pane -- it means one of them is looking at a different folder. */
  if (s->global_manifest_entries >= 0 &&
      (gsize) s->global_manifest_entries != s->videos)
    gtk_box_append (
        GTK_BOX (self->stats_box),
        kv_row ("",
                "The indexed count and global_manifest.json disagree. Usually "
                "that means the Library is pointed at a different folder from "
                "the one downloads are going to.",
                "warning"));
}

static void
fill_log (YtdlHealthView *self)
{
  g_autofree char *data_root = ytdl_settings_resolved_data_root (self->settings);
  guint which = gtk_drop_down_get_selected (GTK_DROP_DOWN (self->log_choice));
  const char *name = (which == 1) ? "archive.txt" : "download.log";

  g_autofree char *path =
      g_build_filename (data_root, "Archive Logs", "Logs", name, NULL);
  g_autofree char *tail = ytdl_health_log_tail (path, 300);

  GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->log_view));
  if (tail == NULL || *tail == '\0')
    {
      g_autofree char *msg =
          g_strdup_printf ("Nothing to show. %s does not exist yet.", path);
      gtk_text_buffer_set_text (buf, msg, -1);
    }
  else
    {
      gtk_text_buffer_set_text (buf, tail, -1);
    }
}

/* ---------------------------------------------------------------------- */

void
ytdl_health_view_refresh (YtdlHealthView *self, gboolean force)
{
  g_return_if_fail (YTDL_IS_HEALTH_VIEW (self));

  fill_installed (self);
  fill_config (self);
  fill_stats (self);
  fill_log (self);

  if (self->probing)
    return;
  self->probing = TRUE;
  gtk_widget_set_sensitive (self->refresh, FALSE);
  gtk_widget_set_visible (self->deps_spinner, TRUE);
  gtk_spinner_start (GTK_SPINNER (self->deps_spinner));

  ProbeJob *job = g_new0 (ProbeJob, 1);
  job->view = g_object_ref (self);
  job->force = force;
  GThread *t = g_thread_new ("ytdl-health", probe_thread, job);
  g_thread_unref (t);
}

void
ytdl_health_view_set_index (YtdlHealthView *self, YtdlIndex *index)
{
  g_return_if_fail (YTDL_IS_HEALTH_VIEW (self));
  self->index = index;
  fill_stats (self);
}

static void
on_refresh (GtkButton *btn, gpointer user_data)
{
  ytdl_health_view_refresh (user_data, TRUE);
}

static void
on_log_choice (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  fill_log (user_data);
}

static void
ytdl_health_view_init (YtdlHealthView *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);
}

static void
ytdl_health_view_class_init (YtdlHealthViewClass *klass)
{
}

GtkWidget *
ytdl_health_view_new (YtdlSettings *settings)
{
  YtdlHealthView *self = g_object_new (YTDL_TYPE_HEALTH_VIEW, NULL);
  self->settings = settings;

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start (content, 12);
  gtk_widget_set_margin_end (content, 12);
  gtk_widget_set_margin_top (content, 12);
  gtk_widget_set_margin_bottom (content, 12);

  self->deps_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  self->deps_spinner = gtk_spinner_new ();
  gtk_widget_set_visible (self->deps_spinner, FALSE);

  self->refresh = gtk_button_new_with_label ("Re-probe");
  gtk_widget_set_tooltip_text (
      self->refresh,
      "Skips the five-minute cache and runs every --version again. What you "
      "want right after installing something that was missing.");
  g_signal_connect (self->refresh, "clicked", G_CALLBACK (on_refresh), self);

  GtkWidget *dep_trailing = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (dep_trailing), self->deps_spinner);
  gtk_box_append (GTK_BOX (dep_trailing), self->refresh);
  gtk_box_append (GTK_BOX (content),
                  section ("Dependencies", self->deps_box, dep_trailing));

  self->files_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_box_append (GTK_BOX (content),
                  section ("Installed pipeline files", self->files_box, NULL));

  self->config_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_box_append (GTK_BOX (content),
                  section ("yt-dlp.conf", self->config_box, NULL));

  self->stats_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_box_append (GTK_BOX (content), section ("Archive", self->stats_box, NULL));

  static const char *const log_labels[] = { "download.log", "archive.txt",
                                            NULL };
  self->log_choice =
      gtk_drop_down_new (G_LIST_MODEL (gtk_string_list_new (log_labels)), NULL);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (self->log_choice), 0);
  g_signal_connect (self->log_choice, "notify::selected",
                    G_CALLBACK (on_log_choice), self);

  self->log_view = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (self->log_view), FALSE);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (self->log_view), TRUE);
  GtkWidget *log_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (log_scroll),
                                 self->log_view);
  gtk_widget_set_size_request (log_scroll, -1, 220);
  gtk_box_append (GTK_BOX (content),
                  section ("Logs", log_scroll, self->log_choice));

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), content);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_box_append (GTK_BOX (self), scroller);

  return GTK_WIDGET (self);
}
