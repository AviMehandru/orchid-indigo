#include "downloads_view.h"

#include "paths.h"
#include "profiles.h"

#include <string.h>

#define MAX_LOG_LINES_SHOWN 4000

struct _YtdlDownloadsView
{
  GtkBox parent_instance;

  YtdlRunner   *runner;   /* borrowed */
  YtdlSettings *settings; /* borrowed */

  GtkWidget *url;
  GtkWidget *dest;
  GtkWidget *preview;

  GtkWidget *mode, *quality, *codec, *audio_codec, *container;
  GtkWidget *workers;
  GtkWidget *sync_cb, *lazy_cb, *no_pot_cb;
  GtkWidget *no_comments_cb, *no_subs_cb, *no_thumbnail_cb, *no_metadata_cb;
  GtkWidget *extra_args;

  GtkWidget *start, *cancel, *pause;
  GtkWidget *progress;
  GtkWidget *stage;

  GtkWidget     *log;
  GtkTextBuffer *log_buf;
  gboolean       transient_active;

  GtkWidget *queue_box;
  GtkWidget *history_box;
  GtkWidget *queue_title;
  GtkWidget *history_title;

  /* Profiles */
  YtdlProfileStore *store;
  GtkWidget        *profile_drop;
  GtkStringList    *profile_list;
  GtkWidget        *name_row;
  GtkWidget        *name_entry;
  GtkWidget        *profile_status;
  gboolean          renaming;  /* the name row serves Save and Rename both */
  gboolean          applying;  /* suppresses the selection handler */
};

G_DEFINE_FINAL_TYPE (YtdlDownloadsView, ytdl_downloads_view, GTK_TYPE_BOX)

/* ---------------------------------------------------------------------- */
/* Reading the form                                                       */
/* ---------------------------------------------------------------------- */

/* GtkDropDown, not GtkComboBoxText.
 *
 * GTK deprecated the combo box in 4.10 and this build is -Werror, so the
 * choice is made for us -- but it is also the right one: DropDown is the
 * widget the rest of a modern GTK4 desktop uses.
 *
 * A DropDown selects by INDEX while the pipeline takes strings, so the id
 * array rides along on the widget and the selected index indexes into it. */
static const char *
combo_value (GtkWidget *w)
{
  const char *const *ids = g_object_get_data (G_OBJECT (w), "ytdl-ids");
  guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (w));
  if (ids == NULL || i == GTK_INVALID_LIST_POSITION)
    return "";
  return ids[i];
}

static void
combo_set_value (GtkWidget *w, const char *id)
{
  const char *const *ids = g_object_get_data (G_OBJECT (w), "ytdl-ids");
  if (ids == NULL || id == NULL)
    return;
  for (gsize i = 0; ids[i] != NULL; i++)
    if (g_strcmp0 (ids[i], id) == 0)
      {
        gtk_drop_down_set_selected (GTK_DROP_DOWN (w), (guint) i);
        return;
      }
}

static YtdlRunOptions *
collect (YtdlDownloadsView *self)
{
  YtdlRunOptions *o = ytdl_run_options_new ();

  o->url = g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->url)));
  o->data_root = g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->dest)));

  o->mode = g_strdup (combo_value (self->mode));
  o->quality = g_strdup (combo_value (self->quality));
  o->codec = g_strdup (combo_value (self->codec));
  o->audio_codec = g_strdup (combo_value (self->audio_codec));
  o->container = g_strdup (combo_value (self->container));

  o->workers =
      (guint) gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (self->workers));
  o->sync = gtk_check_button_get_active (GTK_CHECK_BUTTON (self->sync_cb));
  o->lazy = gtk_check_button_get_active (GTK_CHECK_BUTTON (self->lazy_cb));
  o->no_pot = gtk_check_button_get_active (GTK_CHECK_BUTTON (self->no_pot_cb));
  o->no_comments =
      gtk_check_button_get_active (GTK_CHECK_BUTTON (self->no_comments_cb));
  o->no_subs = gtk_check_button_get_active (GTK_CHECK_BUTTON (self->no_subs_cb));
  o->no_thumbnail =
      gtk_check_button_get_active (GTK_CHECK_BUTTON (self->no_thumbnail_cb));
  o->no_metadata =
      gtk_check_button_get_active (GTK_CHECK_BUTTON (self->no_metadata_cb));

  /* One --ytdlp-arg per line, because a real --match-filter expression
   * contains commas and spaces and there is no separator that would be safe
   * to split a single-line field on. */
  const char *extra = gtk_editable_get_text (GTK_EDITABLE (self->extra_args));
  if (extra != NULL && *extra != '\0')
    {
      g_auto (GStrv) parts = g_strsplit (extra, "\n", -1);
      for (gsize i = 0; parts[i] != NULL; i++)
        {
          g_strstrip (parts[i]);
          if (*parts[i] != '\0')
            g_ptr_array_add (o->ytdlp_args, g_strdup (parts[i]));
        }
    }
  return o;
}

static void
refresh_preview (YtdlDownloadsView *self)
{
  g_autoptr (YtdlRunOptions) o = collect (self);

  /* With no URL typed, the preview would read ytdl "" -- which looks like a
   * bug rather than an empty field, and is what it showed right after a queue
   * add cleared the box. A placeholder keeps the rest of the command visible,
   * so the options you have set are still readable while you paste a URL. */
  if (o->url == NULL || *g_strstrip (o->url) == '\0')
    {
      g_free (o->url);
      o->url = g_strdup ("<URL>");
    }

  g_autofree char *cmd = ytdl_run_options_command_preview (o);
  gtk_label_set_text (GTK_LABEL (self->preview), cmd);
}

static void
on_form_changed (GtkWidget *w, gpointer user_data)
{
  refresh_preview (user_data);
}

/* GtkDropDown reports selection through a property notification rather than a
 * "changed" signal, so it needs the three-argument shape. */
static void
on_notify_changed (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  refresh_preview (user_data);
}

/* ---------------------------------------------------------------------- */
/* Log                                                                    */
/* ---------------------------------------------------------------------- */

static void
append_log (YtdlDownloadsView *self, const char *text, gboolean transient)
{
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (self->log_buf, &end);

  /* A transient line REPLACES the previous transient line. yt-dlp redraws its
   * progress with \r and the conf sets no --newline, so without this one
   * download produces thousands of near-identical rows. A permanent line
   * always appends, which leaves the last progress reading visible instead of
   * swallowing it. */
  if (transient && self->transient_active)
    {
      GtkTextIter line_start = end;
      gtk_text_iter_set_line_offset (&line_start, 0);
      gtk_text_buffer_delete (self->log_buf, &line_start, &end);
      gtk_text_buffer_get_end_iter (self->log_buf, &end);
    }
  else if (gtk_text_buffer_get_char_count (self->log_buf) > 0)
    {
      gtk_text_buffer_insert (self->log_buf, &end, "\n", 1);
    }

  gtk_text_buffer_insert (self->log_buf, &end, text, -1);
  self->transient_active = transient;

  /* Bounded, for the same reason the runner bounds its own copy: a --sync of
   * a large channel emits far more than anyone will scroll back through. */
  int lines = gtk_text_buffer_get_line_count (self->log_buf);
  if (lines > MAX_LOG_LINES_SHOWN)
    {
      GtkTextIter start, cut;
      gtk_text_buffer_get_start_iter (self->log_buf, &start);
      gtk_text_buffer_get_iter_at_line (self->log_buf, &cut,
                                        lines - MAX_LOG_LINES_SHOWN);
      gtk_text_buffer_delete (self->log_buf, &start, &cut);
    }

  gtk_text_buffer_get_end_iter (self->log_buf, &end);
  GtkTextMark *mark =
      gtk_text_buffer_create_mark (self->log_buf, NULL, &end, FALSE);
  gtk_text_view_scroll_mark_onscreen (GTK_TEXT_VIEW (self->log), mark);
  gtk_text_buffer_delete_mark (self->log_buf, mark);
}

static void
on_line (YtdlRunner *runner, const char *text, gboolean transient,
         gpointer user_data)
{
  append_log (user_data, text, transient);
}

/* ---------------------------------------------------------------------- */
/* Queue and history rows                                                 */
/* ---------------------------------------------------------------------- */

static void
clear_box (GtkWidget *box)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (box)) != NULL)
    gtk_box_remove (GTK_BOX (box), child);
}

static char *
format_when (gint64 unix_secs)
{
  if (unix_secs <= 0)
    return g_strdup ("");
  g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (unix_secs);
  return dt != NULL ? g_date_time_format (dt, "%b %-d, %H:%M") : g_strdup ("");
}

static void
on_remove_queued (GtkButton *btn, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  const char *id = g_object_get_data (G_OBJECT (btn), "run-id");
  ytdl_runner_remove_queued (self->runner, id);
}

static GtkWidget *
make_queue_row (YtdlDownloadsView *self, const YtdlRunRecord *r)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class (row, "card");
  gtk_widget_set_margin_top (row, 3);
  gtk_widget_set_margin_bottom (row, 3);

  GtkWidget *label = gtk_label_new (r->opts != NULL ? r->opts->url : "");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (row), label);

  GtkWidget *rm = gtk_button_new_from_icon_name ("list-remove-symbolic");
  gtk_widget_set_tooltip_text (rm, "Remove from the queue");
  gtk_widget_add_css_class (rm, "flat");
  g_object_set_data_full (G_OBJECT (rm), "run-id", g_strdup (r->id), g_free);
  g_signal_connect (rm, "clicked", G_CALLBACK (on_remove_queued), self);
  gtk_box_append (GTK_BOX (row), rm);

  return row;
}

static GtkWidget *
make_history_row (const YtdlRunRecord *r)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class (row, "card");
  gtk_widget_set_margin_top (row, 3);
  gtk_widget_set_margin_bottom (row, 3);

  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *state = gtk_label_new (r->state);
  gtk_widget_add_css_class (state, "caption");
  if (g_strcmp0 (r->state, "done") == 0)
    gtk_widget_add_css_class (state, "success");
  else if (g_strcmp0 (r->state, "failed") == 0)
    gtk_widget_add_css_class (state, "error");
  else
    gtk_widget_add_css_class (state, "dim-label");
  gtk_box_append (GTK_BOX (top), state);

  g_autofree char *when = format_when (r->started);
  GtkWidget *time_label = gtk_label_new (when);
  gtk_widget_add_css_class (time_label, "caption");
  gtk_widget_add_css_class (time_label, "dim-label");
  gtk_box_append (GTK_BOX (top), time_label);

  /* The four counts exist only in the pipeline's own session summary line,
   * which is why they are parsed out of it as the run goes. A run that was
   * cancelled or died early never printed one, and shows nothing rather than
   * four zeroes that would read as "it ran and found nothing". */
  if (r->videos_touched >= 0)
    {
      g_autofree char *counts = g_strdup_printf (
          "%" G_GINT64_FORMAT " touched · %" G_GINT64_FORMAT " skipped · "
          "%" G_GINT64_FORMAT " errors · %" G_GINT64_FORMAT " warnings",
          r->videos_touched, r->archive_skipped, r->errors, r->warnings);
      GtkWidget *c = gtk_label_new (counts);
      gtk_widget_add_css_class (c, "caption");
      gtk_widget_add_css_class (c, "dim-label");
      gtk_box_append (GTK_BOX (top), c);
    }
  gtk_box_append (GTK_BOX (row), top);

  GtkWidget *cmd = gtk_label_new (r->command);
  gtk_label_set_xalign (GTK_LABEL (cmd), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (cmd), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_add_css_class (cmd, "monospace");
  gtk_widget_add_css_class (cmd, "caption");
  gtk_box_append (GTK_BOX (row), cmd);

  if (r->last_line != NULL && *r->last_line != '\0')
    {
      GtkWidget *last = gtk_label_new (r->last_line);
      gtk_label_set_xalign (GTK_LABEL (last), 0.0f);
      gtk_label_set_ellipsize (GTK_LABEL (last), PANGO_ELLIPSIZE_END);
      gtk_widget_add_css_class (last, "caption");
      gtk_widget_add_css_class (last, "dim-label");
      gtk_box_append (GTK_BOX (row), last);
    }

  return row;
}

static void
on_state_changed (YtdlRunner *runner, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;

  g_autoptr (GPtrArray) queue = ytdl_runner_queue (self->runner);
  g_autoptr (GPtrArray) history = ytdl_runner_history (self->runner);
  YtdlRunRecord *current = ytdl_runner_current (self->runner);

  clear_box (self->queue_box);
  for (guint i = 0; i < queue->len; i++)
    gtk_box_append (GTK_BOX (self->queue_box),
                    make_queue_row (self, g_ptr_array_index (queue, i)));

  clear_box (self->history_box);
  for (guint i = 0; i < history->len; i++)
    gtk_box_append (GTK_BOX (self->history_box),
                    make_history_row (g_ptr_array_index (history, i)));

  g_autofree char *qt =
      g_strdup_printf ("Queue (%u)", queue->len);
  gtk_label_set_text (GTK_LABEL (self->queue_title), qt);
  g_autofree char *ht = g_strdup_printf ("History (%u)", history->len);
  gtk_label_set_text (GTK_LABEL (self->history_title), ht);

  YtdlProgress p = { 0 };
  p.percent = -1.0;
  ytdl_runner_progress (self->runner, &p);

  gboolean running = current != NULL;
  gtk_widget_set_sensitive (self->cancel, running);

  if (p.percent >= 0.0)
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (self->progress),
                                   CLAMP (p.percent / 100.0, 0.0, 1.0));
  else if (running)
    gtk_progress_bar_pulse (GTK_PROGRESS_BAR (self->progress));
  else
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (self->progress), 0.0);

  GString *stage = g_string_new (NULL);
  if (running)
    {
      g_string_append (stage, p.stage != NULL ? p.stage : "running");
      if (p.video_id != NULL)
        g_string_append_printf (stage, " · %s", p.video_id);
      if (p.speed != NULL)
        g_string_append_printf (stage, " · %s", p.speed);
      if (p.eta != NULL)
        g_string_append_printf (stage, " · ETA %s", p.eta);
      if (p.total != NULL)
        g_string_append_printf (stage, " · of %s", p.total);
    }
  else
    {
      g_string_append (stage, "Idle");
    }
  gtk_label_set_text (GTK_LABEL (self->stage), stage->str);
  g_string_free (stage, TRUE);

  ytdl_progress_clear (&p);
  g_clear_pointer (&current, ytdl_run_record_free);
}

/* ---------------------------------------------------------------------- */
/* Profiles                                                               */
/* ---------------------------------------------------------------------- */

/* The URL is deliberately NOT touched. A profile that replaced what you were
 * about to download would be the one thing a preset must never do -- which is
 * also why the store drops the URL on the way in. */
static void
apply_profile_options (YtdlDownloadsView *self, const YtdlRunOptions *o)
{
  self->applying = TRUE;

  combo_set_value (self->mode, o->mode != NULL ? o->mode : "full");
  combo_set_value (self->quality, o->quality != NULL ? o->quality : "best");
  combo_set_value (self->codec, o->codec != NULL ? o->codec : "any");
  combo_set_value (self->audio_codec,
                   o->audio_codec != NULL ? o->audio_codec : "any");
  combo_set_value (self->container,
                   o->container != NULL ? o->container : "mkv");

  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->workers),
                             o->workers > 0 ? o->workers : 1);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->sync_cb), o->sync);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->lazy_cb), o->lazy);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->no_pot_cb), o->no_pot);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->no_comments_cb),
                               o->no_comments);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->no_subs_cb), o->no_subs);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->no_thumbnail_cb),
                               o->no_thumbnail);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->no_metadata_cb),
                               o->no_metadata);

  /* The destination is part of the profile, but an empty one must not wipe a
   * destination the user has set for this session. */
  if (o->data_root != NULL && *o->data_root != '\0')
    gtk_editable_set_text (GTK_EDITABLE (self->dest), o->data_root);

  GString *extra = g_string_new (NULL);
  if (o->ytdlp_args != NULL)
    for (guint i = 0; i < o->ytdlp_args->len; i++)
      {
        if (extra->len > 0)
          g_string_append_c (extra, '\n');
        g_string_append (extra, g_ptr_array_index (o->ytdlp_args, i));
      }
  gtk_editable_set_text (GTK_EDITABLE (self->extra_args), extra->str);
  g_string_free (extra, TRUE);

  self->applying = FALSE;
  refresh_preview (self);
}

static void
set_profile_status (YtdlDownloadsView *self, const char *text, gboolean bad)
{
  gtk_label_set_text (GTK_LABEL (self->profile_status), text != NULL ? text : "");
  gtk_widget_remove_css_class (self->profile_status, "error");
  if (bad)
    gtk_widget_add_css_class (self->profile_status, "error");
}

/* Index 0 is always "(no profile)" -- a real state, not a placeholder. It is
 * what the window is in before anything has been saved. */
static void
refresh_profiles (YtdlDownloadsView *self)
{
  self->applying = TRUE;

  guint had = g_list_model_get_n_items (G_LIST_MODEL (self->profile_list));
  const char *none[] = { "(no profile)", NULL };
  gtk_string_list_splice (self->profile_list, 0, had, none);

  guint selected = 0;
  for (guint i = 0; i < self->store->profiles->len; i++)
    {
      const YtdlProfile *p = g_ptr_array_index (self->store->profiles, i);
      gtk_string_list_append (self->profile_list, p->name);
      if (self->store->active != NULL &&
          g_ascii_strcasecmp (self->store->active, p->name) == 0)
        selected = i + 1;
    }

  gtk_drop_down_set_selected (GTK_DROP_DOWN (self->profile_drop), selected);
  self->applying = FALSE;
}

static const char *
selected_profile_name (YtdlDownloadsView *self)
{
  guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (self->profile_drop));
  if (i == GTK_INVALID_LIST_POSITION || i == 0)
    return NULL;
  if (i - 1 >= self->store->profiles->len)
    return NULL;
  const YtdlProfile *p = g_ptr_array_index (self->store->profiles, i - 1);
  return p->name;
}

static void
on_profile_selected (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  if (self->applying)
    return;

  const char *name = selected_profile_name (self);
  GError *error = NULL;

  if (name == NULL)
    {
      /* Clearing the selection does NOT reset the form. The options stay
       * exactly as they are; you have simply stopped calling them a profile. */
      ytdl_profiles_activate (self->store, NULL, &error);
      set_profile_status (self, "", FALSE);
    }
  else
    {
      const YtdlProfile *p = ytdl_profiles_get (self->store, name);
      if (p != NULL)
        apply_profile_options (self, p->opts);
      ytdl_profiles_activate (self->store, name, &error);
      set_profile_status (self, "", FALSE);
    }
  if (error != NULL)
    {
      set_profile_status (self, error->message, TRUE);
      g_clear_error (&error);
    }
}

static void
show_name_row (YtdlDownloadsView *self, gboolean renaming, const char *initial)
{
  self->renaming = renaming;
  gtk_editable_set_text (GTK_EDITABLE (self->name_entry),
                         initial != NULL ? initial : "");
  gtk_widget_set_visible (self->name_row, TRUE);
  gtk_widget_grab_focus (self->name_entry);
}

static void
on_profile_save (GtkButton *b, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  /* Pre-filled with the current profile's name, so Save over an existing one
   * is Enter rather than retyping. */
  show_name_row (self, FALSE, selected_profile_name (self));
}

static void
on_profile_rename (GtkButton *b, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  const char *name = selected_profile_name (self);
  if (name == NULL)
    {
      set_profile_status (self, "Select a profile to rename.", TRUE);
      return;
    }
  show_name_row (self, TRUE, name);
}

static void
on_profile_delete (GtkButton *b, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  const char *name = selected_profile_name (self);
  if (name == NULL)
    {
      set_profile_status (self, "Select a profile to delete.", TRUE);
      return;
    }

  g_autofree char *msg = g_strdup_printf ("Deleted \"%s\".", name);
  GError *error = NULL;
  if (!ytdl_profiles_delete (self->store, name, &error))
    {
      set_profile_status (self, error->message, TRUE);
      g_clear_error (&error);
      return;
    }
  refresh_profiles (self);
  set_profile_status (self, msg, FALSE);
}

static void
on_name_cancel (GtkButton *b, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  gtk_widget_set_visible (self->name_row, FALSE);
}

static void
on_name_confirm (GtkWidget *w, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  const char *typed = gtk_editable_get_text (GTK_EDITABLE (self->name_entry));

  GError *error = NULL;
  gboolean ok;
  g_autofree char *msg = NULL;

  if (self->renaming)
    {
      const char *from = selected_profile_name (self);
      ok = from != NULL && ytdl_profiles_rename (self->store, from, typed, &error);
      if (ok)
        msg = g_strdup_printf ("Renamed to \"%s\".", typed);
    }
  else
    {
      g_autoptr (YtdlRunOptions) o = collect (self);
      ok = ytdl_profiles_save (self->store, typed, o, &error);
      if (ok)
        msg = g_strdup_printf ("Saved \"%s\".", typed);
    }

  if (!ok)
    {
      set_profile_status (self,
                          error != NULL ? error->message : "Could not save.",
                          TRUE);
      g_clear_error (&error);
      return;
    }

  gtk_widget_set_visible (self->name_row, FALSE);
  refresh_profiles (self);
  set_profile_status (self, msg, FALSE);
}

/* ---------------------------------------------------------------------- */
/* Actions                                                                */
/* ---------------------------------------------------------------------- */

static void
show_error (YtdlDownloadsView *self, const char *message)
{
  GtkWidget *root = GTK_WIDGET (gtk_widget_get_root (GTK_WIDGET (self)));
  GtkAlertDialog *dlg = gtk_alert_dialog_new ("%s", message);
  gtk_alert_dialog_show (dlg, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL);
  g_object_unref (dlg);
}

static void
on_start (GtkButton *btn, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  g_autoptr (YtdlRunOptions) o = collect (self);

  GError *error = NULL;
  g_autofree char *id = ytdl_runner_enqueue (self->runner, o, &error);
  if (id == NULL)
    {
      show_error (self, error->message);
      g_clear_error (&error);
      return;
    }
  /* The URL is cleared; the options are not. Queueing five videos with the
   * same settings is the common case, and re-picking them each time would be
   * the wrong kind of tidy. */
  gtk_editable_set_text (GTK_EDITABLE (self->url), "");
  refresh_preview (self);
}

static void
on_cancel (GtkButton *btn, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  if (!ytdl_runner_cancel (self->runner))
    show_error (self, "Nothing is running.");
}

static void
on_pause_toggled (GtkToggleButton *btn, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  gboolean paused = gtk_toggle_button_get_active (btn);
  ytdl_runner_set_paused (self->runner, paused);
  gtk_button_set_label (GTK_BUTTON (btn), paused ? "Resume queue" : "Pause queue");
}

static void
on_folder_chosen (GObject *source, GAsyncResult *res, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  g_autoptr (GFile) folder =
      gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), res, NULL);
  if (folder == NULL)
    return; /* dismissed -- not an error */

  g_autofree char *path = g_file_get_path (folder);
  if (path == NULL)
    return;

  gtk_editable_set_text (GTK_EDITABLE (self->dest), path);
  g_free (self->settings->data_root);
  self->settings->data_root = g_strdup (path);
  ytdl_settings_save (self->settings);
  refresh_preview (self);
}

/* The platform's own folder chooser.
 *
 * This is one of the things going native actually buys: the webview build had
 * to route a folder pick through a Rust plugin because a path-typing box with
 * a nicer border was the only alternative it had. Here it is the file chooser
 * the rest of the desktop uses, with its bookmarks and recent places. */
static void
on_browse (GtkButton *btn, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  GtkFileDialog *dlg = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dlg, "Choose a destination folder");

  const char *current = gtk_editable_get_text (GTK_EDITABLE (self->dest));
  if (current != NULL && *current != '\0')
    {
      g_autofree char *expanded = ytdl_expand_tilde (current);
      if (g_file_test (expanded, G_FILE_TEST_IS_DIR))
        {
          g_autoptr (GFile) f = g_file_new_for_path (expanded);
          gtk_file_dialog_set_initial_folder (dlg, f);
        }
    }

  GtkWidget *root = GTK_WIDGET (gtk_widget_get_root (GTK_WIDGET (self)));
  gtk_file_dialog_select_folder (dlg,
                                 GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                                 NULL, on_folder_chosen, self);
  g_object_unref (dlg);
}

static void
on_dest_changed (GtkEditable *editable, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  g_free (self->settings->data_root);
  self->settings->data_root = g_strdup (gtk_editable_get_text (editable));
  refresh_preview (self);
}

/* ---------------------------------------------------------------------- */
/* Construction                                                           */
/* ---------------------------------------------------------------------- */

static GtkWidget *
labelled (const char *text, GtkWidget *child)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (l), 0.0f);
  gtk_widget_add_css_class (l, "caption");
  gtk_widget_add_css_class (l, "dim-label");
  gtk_box_append (GTK_BOX (box), l);
  gtk_box_append (GTK_BOX (box), child);
  return box;
}

static GtkWidget *
make_combo (const char *const *ids, const char *const *labels, const char *active)
{
  GtkStringList *model = gtk_string_list_new (labels);
  GtkWidget *d = gtk_drop_down_new (G_LIST_MODEL (model), NULL);

  /* The ids arrays are static, so storing the pointer rather than copying is
   * safe for the life of the widget. */
  g_object_set_data (G_OBJECT (d), "ytdl-ids", (gpointer) ids);

  guint selected = 0;
  for (gsize i = 0; ids[i] != NULL; i++)
    if (g_strcmp0 (ids[i], active) == 0)
      {
        selected = (guint) i;
        break;
      }
  gtk_drop_down_set_selected (GTK_DROP_DOWN (d), selected);
  return d;
}

static GtkWidget *
section (const char *title, GtkWidget **title_label_out)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *l = gtk_label_new (title);
  gtk_label_set_xalign (GTK_LABEL (l), 0.0f);
  gtk_widget_add_css_class (l, "heading");
  gtk_box_append (GTK_BOX (box), l);
  if (title_label_out != NULL)
    *title_label_out = l;
  return box;
}

static void
ytdl_downloads_view_init (YtdlDownloadsView *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 8);
  gtk_widget_set_margin_start (GTK_WIDGET (self), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (self), 12);
  gtk_widget_set_margin_top (GTK_WIDGET (self), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self), 12);
}

static void
ytdl_downloads_view_dispose (GObject *object)
{
  YtdlDownloadsView *self = YTDL_DOWNLOADS_VIEW (object);
  g_clear_pointer (&self->store, ytdl_profile_store_free);
  G_OBJECT_CLASS (ytdl_downloads_view_parent_class)->dispose (object);
}

static void
ytdl_downloads_view_class_init (YtdlDownloadsViewClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = ytdl_downloads_view_dispose;
}

GtkWidget *
ytdl_downloads_view_new (YtdlRunner *runner, YtdlSettings *settings)
{
  YtdlDownloadsView *self = g_object_new (YTDL_TYPE_DOWNLOADS_VIEW, NULL);
  self->runner = runner;
  self->settings = settings;

  /* --- Profiles ------------------------------------------------------ */
  GtkWidget *prow = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

  const char *const initial[] = { "(no profile)", NULL };
  self->profile_list = gtk_string_list_new (initial);
  self->profile_drop =
      gtk_drop_down_new (G_LIST_MODEL (self->profile_list), NULL);
  gtk_widget_set_tooltip_text (
      self->profile_drop,
      "A saved set of options. Selecting one applies its options and leaves "
      "the URL alone — a preset that replaced what you were about to download "
      "would be the one thing a preset must never do.");
  g_signal_connect (self->profile_drop, "notify::selected",
                    G_CALLBACK (on_profile_selected), self);
  gtk_box_append (GTK_BOX (prow), labelled ("Profile", self->profile_drop));

  GtkWidget *pbtns = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_valign (pbtns, GTK_ALIGN_END);

  GtkWidget *psave = gtk_button_new_with_label ("Save as…");
  gtk_widget_set_tooltip_text (
      psave, "Saves every option except the URL under a name.");
  g_signal_connect (psave, "clicked", G_CALLBACK (on_profile_save), self);
  gtk_box_append (GTK_BOX (pbtns), psave);

  GtkWidget *pren = gtk_button_new_with_label ("Rename…");
  g_signal_connect (pren, "clicked", G_CALLBACK (on_profile_rename), self);
  gtk_box_append (GTK_BOX (pbtns), pren);

  GtkWidget *pdel = gtk_button_new_with_label ("Delete");
  g_signal_connect (pdel, "clicked", G_CALLBACK (on_profile_delete), self);
  gtk_box_append (GTK_BOX (pbtns), pdel);
  gtk_box_append (GTK_BOX (prow), pbtns);

  self->profile_status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->profile_status), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (self->profile_status),
                           PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (self->profile_status, TRUE);
  gtk_widget_set_valign (self->profile_status, GTK_ALIGN_END);
  gtk_widget_add_css_class (self->profile_status, "caption");
  gtk_widget_add_css_class (self->profile_status, "dim-label");
  gtk_box_append (GTK_BOX (prow), self->profile_status);
  gtk_box_append (GTK_BOX (self), prow);

  /* An inline name row rather than a modal dialog: naming a profile is a
   * two-second interaction and a dialog for it is heavier than the thing it
   * asks for. Enter confirms, Escape-equivalent is the Cancel button. */
  self->name_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  self->name_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (self->name_entry),
                                  "Profile name");
  gtk_widget_set_hexpand (self->name_entry, TRUE);
  g_signal_connect (self->name_entry, "activate", G_CALLBACK (on_name_confirm),
                    self);
  gtk_box_append (GTK_BOX (self->name_row), self->name_entry);

  GtkWidget *nok = gtk_button_new_with_label ("OK");
  gtk_widget_add_css_class (nok, "suggested-action");
  g_signal_connect (nok, "clicked", G_CALLBACK (on_name_confirm), self);
  gtk_box_append (GTK_BOX (self->name_row), nok);

  GtkWidget *ncancel = gtk_button_new_with_label ("Cancel");
  g_signal_connect (ncancel, "clicked", G_CALLBACK (on_name_cancel), self);
  gtk_box_append (GTK_BOX (self->name_row), ncancel);

  gtk_widget_set_visible (self->name_row, FALSE);
  gtk_box_append (GTK_BOX (self), self->name_row);

  /* --- URL + destination ------------------------------------------- */
  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

  self->url = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (self->url),
                                  "Video, playlist or channel URL — or a bare "
                                  "11-character video id");
  gtk_widget_set_hexpand (self->url, TRUE);
  g_signal_connect (self->url, "changed", G_CALLBACK (on_form_changed), self);
  gtk_box_append (GTK_BOX (top), labelled ("URL", self->url));

  self->start = gtk_button_new_with_label ("Add to queue");
  gtk_widget_add_css_class (self->start, "suggested-action");
  gtk_widget_set_valign (self->start, GTK_ALIGN_END);
  g_signal_connect (self->start, "clicked", G_CALLBACK (on_start), self);
  gtk_box_append (GTK_BOX (top), self->start);
  gtk_box_append (GTK_BOX (self), top);

  GtkWidget *destrow = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  self->dest = gtk_entry_new ();
  gtk_entry_set_placeholder_text (
      GTK_ENTRY (self->dest),
      "Leave empty for the pipeline's own default (the install root)");
  if (settings->data_root != NULL)
    gtk_editable_set_text (GTK_EDITABLE (self->dest), settings->data_root);
  gtk_widget_set_hexpand (self->dest, TRUE);
  g_signal_connect (self->dest, "changed", G_CALLBACK (on_dest_changed), self);
  gtk_box_append (GTK_BOX (destrow), labelled ("Destination", self->dest));

  GtkWidget *browse = gtk_button_new_with_label ("Choose…");
  gtk_widget_set_valign (browse, GTK_ALIGN_END);
  g_signal_connect (browse, "clicked", G_CALLBACK (on_browse), self);
  gtk_box_append (GTK_BOX (destrow), browse);
  gtk_box_append (GTK_BOX (self), destrow);

  /* --- Options ------------------------------------------------------ */
  static const char *const mode_ids[] = { "full", "video-only", "audio-only",
                                          "metadata-only", "comments-only",
                                          "subs-only", NULL };
  static const char *const mode_labels[] = {
    "Everything", "Video only", "Audio only", "Metadata only", "Comments only",
    "Subtitles only", NULL
  };
  static const char *const q_ids[] = { "best", "2160", "1440", "1080",
                                       "720",  "480",  "360",  NULL };
  static const char *const q_labels[] = { "Best", "2160p", "1440p", "1080p",
                                          "720p", "480p",  "360p",  NULL };
  static const char *const c_ids[] = { "any", "avc1", "vp9", "av01", NULL };
  static const char *const c_labels[] = { "Any", "AVC1 / H.264", "VP9", "AV1",
                                          NULL };
  static const char *const a_ids[] = { "any", "opus", "aac", "mp3", "flac", NULL };
  static const char *const a_labels[] = { "Any",  "Opus", "AAC",
                                          "MP3",  "FLAC", NULL };
  static const char *const k_ids[] = { "mkv", "mp4", "webm", NULL };
  static const char *const k_labels[] = { "MKV", "MP4", "WebM", NULL };

  self->mode = make_combo (mode_ids, mode_labels, "full");
  self->quality = make_combo (q_ids, q_labels, "best");
  self->codec = make_combo (c_ids, c_labels, "any");
  self->audio_codec = make_combo (a_ids, a_labels, "any");
  self->container = make_combo (k_ids, k_labels, "mkv");

  GtkWidget *opts = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (opts), labelled ("Mode", self->mode));
  gtk_box_append (GTK_BOX (opts), labelled ("Quality", self->quality));
  gtk_box_append (GTK_BOX (opts), labelled ("Video codec", self->codec));
  gtk_box_append (GTK_BOX (opts), labelled ("Audio codec", self->audio_codec));
  gtk_box_append (GTK_BOX (opts), labelled ("Container", self->container));

  self->workers = gtk_spin_button_new_with_range (1, 16, 1);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->workers),
                             settings->default_workers);
  gtk_widget_set_tooltip_text (
      self->workers,
      "The pipeline's own parallelism. The queue here is always sequential, "
      "because independent ytdl invocations race on the shared manifests — "
      "--workers is the supported way to run several at once.");
  gtk_box_append (GTK_BOX (opts), labelled ("Workers", self->workers));

  GtkWidget *const drops[] = { self->mode,        self->quality, self->codec,
                               self->audio_codec, self->container };
  for (gsize i = 0; i < G_N_ELEMENTS (drops); i++)
    g_signal_connect (drops[i], "notify::selected",
                      G_CALLBACK (on_notify_changed), self);
  g_signal_connect (self->workers, "value-changed", G_CALLBACK (on_form_changed),
                    self);
  gtk_box_append (GTK_BOX (self), opts);

  GtkWidget *checks = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
#define CHECK(field, label, tip)                                              \
  do                                                                          \
    {                                                                         \
      self->field = gtk_check_button_new_with_label (label);                  \
      gtk_widget_set_tooltip_text (self->field, tip);                         \
      g_signal_connect (self->field, "toggled", G_CALLBACK (on_form_changed), \
                        self);                                                \
      gtk_box_append (GTK_BOX (checks), self->field);                         \
    }                                                                         \
  while (0)

  CHECK (sync_cb, "Sync", "Walk the whole channel, stopping at the first "
                          "video already archived.");
  CHECK (lazy_cb, "Lazy", "Skip the up-front enumeration.");
  CHECK (no_pot_cb, "No PO token", "Skip the PO token provider for this run.");
  CHECK (no_comments_cb, "No comments", "Skip the comments pass.");
  CHECK (no_subs_cb, "No subtitles", "Skip subtitle capture.");
  CHECK (no_thumbnail_cb, "No thumbnail", "Skip thumbnail capture.");
  CHECK (no_metadata_cb, "No metadata", "Skip the metadata pass.");
#undef CHECK
  gtk_box_append (GTK_BOX (self), checks);

  self->extra_args = gtk_entry_new ();
  gtk_entry_set_placeholder_text (
      GTK_ENTRY (self->extra_args),
      "Extra yt-dlp arguments, one per line — passed through as --ytdlp-arg");
  g_signal_connect (self->extra_args, "changed", G_CALLBACK (on_form_changed),
                    self);
  gtk_box_append (GTK_BOX (self),
                  labelled ("Passthrough", self->extra_args));

  /* --- Command preview ---------------------------------------------- */
  self->preview = gtk_label_new ("ytdl");
  gtk_label_set_xalign (GTK_LABEL (self->preview), 0.0f);
  gtk_label_set_selectable (GTK_LABEL (self->preview), TRUE);
  gtk_label_set_wrap (GTK_LABEL (self->preview), TRUE);
  gtk_widget_add_css_class (self->preview, "monospace");
  gtk_widget_add_css_class (self->preview, "ytdl-preview");
  gtk_box_append (GTK_BOX (self),
                  labelled ("This is the command that will run", self->preview));

  /* --- Controls and progress ---------------------------------------- */
  GtkWidget *controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  self->cancel = gtk_button_new_with_label ("Cancel run");
  gtk_widget_add_css_class (self->cancel, "destructive-action");
  gtk_widget_set_sensitive (self->cancel, FALSE);
  gtk_widget_set_tooltip_text (
      self->cancel,
      "Kills the whole process tree — ytdl.ps1, the child pwsh, yt-dlp, "
      "postprocess.ps1 and ffmpeg. Killing only the top process would leave a "
      "download running with nothing reading its output.");
  g_signal_connect (self->cancel, "clicked", G_CALLBACK (on_cancel), self);
  gtk_box_append (GTK_BOX (controls), self->cancel);

  self->pause = gtk_toggle_button_new_with_label ("Pause queue");
  g_signal_connect (self->pause, "toggled", G_CALLBACK (on_pause_toggled), self);
  gtk_box_append (GTK_BOX (controls), self->pause);

  self->stage = gtk_label_new ("Idle");
  gtk_label_set_xalign (GTK_LABEL (self->stage), 0.0f);
  gtk_widget_set_hexpand (self->stage, TRUE);
  gtk_widget_add_css_class (self->stage, "dim-label");
  gtk_box_append (GTK_BOX (controls), self->stage);
  gtk_box_append (GTK_BOX (self), controls);

  self->progress = gtk_progress_bar_new ();
  gtk_box_append (GTK_BOX (self), self->progress);

  /* --- Log, queue, history ------------------------------------------ */
  self->log = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (self->log), FALSE);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (self->log), TRUE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (self->log), GTK_WRAP_WORD_CHAR);
  self->log_buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->log));

  GtkWidget *log_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (log_scroll), self->log);
  gtk_widget_set_vexpand (log_scroll, TRUE);
  gtk_widget_set_size_request (log_scroll, -1, 120);

  self->queue_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  self->history_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *qsec = section ("Queue (0)", &self->queue_title);
  GtkWidget *qscroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (qscroll), self->queue_box);
  gtk_widget_set_vexpand (qscroll, TRUE);
  gtk_widget_set_size_request (qscroll, -1, 110);
  gtk_box_append (GTK_BOX (qsec), qscroll);

  GtkWidget *hsec = section ("History (0)", &self->history_title);
  GtkWidget *hscroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (hscroll),
                                 self->history_box);
  gtk_widget_set_vexpand (hscroll, TRUE);
  gtk_widget_set_size_request (hscroll, -1, 110);
  gtk_box_append (GTK_BOX (hsec), hscroll);

  GtkWidget *lists = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_start_child (GTK_PANED (lists), qsec);
  gtk_paned_set_end_child (GTK_PANED (lists), hsec);
  gtk_paned_set_position (GTK_PANED (lists), 340);

  GtkWidget *bottom = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_start_child (GTK_PANED (bottom), log_scroll);
  gtk_paned_set_end_child (GTK_PANED (bottom), lists);
  gtk_paned_set_position (GTK_PANED (bottom), 150);
  gtk_widget_set_vexpand (bottom, TRUE);
  gtk_box_append (GTK_BOX (self), bottom);

  g_signal_connect (runner, "line", G_CALLBACK (on_line), self);
  g_signal_connect (runner, "state-changed", G_CALLBACK (on_state_changed),
                    self);

  /* Loaded after every widget exists, because restoring the active profile
   * writes into all of them. */
  self->store = ytdl_profiles_load ();
  refresh_profiles (self);
  {
    const char *active = selected_profile_name (self);
    if (active != NULL)
      {
        const YtdlProfile *p = ytdl_profiles_get (self->store, active);
        if (p != NULL)
          apply_profile_options (self, p->opts);
      }
  }

  refresh_preview (self);
  on_state_changed (runner, self);

  return GTK_WIDGET (self);
}
