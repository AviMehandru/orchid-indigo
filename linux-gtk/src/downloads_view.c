#include "downloads_view.h"

#include "paths.h"
#include "profiles.h"

#include <adwaita.h>
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
  /* TRUE while the log still holds its placeholder rather than real output. */
  gboolean       log_placeholder;

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

/* AdwComboRow, not GtkDropDown in a hand-labelled box.
 *
 * The row IS the label plus the control plus the row's share of a boxed
 * list -- the title, the subtitle that explains the option, the height, the
 * separator and the rounded ends of the group all come from libadwaita. A
 * GtkDropDown under a caption label is the same widget with three of those
 * four things done worse.
 *
 * A combo row selects by INDEX while the pipeline takes strings, so the id
 * array rides along on the widget and the selected index indexes into it. */
static const char *
combo_value (GtkWidget *w)
{
  const char *const *ids = g_object_get_data (G_OBJECT (w), "ytdl-ids");
  guint i = adw_combo_row_get_selected (ADW_COMBO_ROW (w));
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
        adw_combo_row_set_selected (ADW_COMBO_ROW (w), (guint) i);
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

  o->workers = (guint) adw_spin_row_get_value (ADW_SPIN_ROW (self->workers));
  o->sync = adw_switch_row_get_active (ADW_SWITCH_ROW (self->sync_cb));
  o->lazy = adw_switch_row_get_active (ADW_SWITCH_ROW (self->lazy_cb));
  o->no_pot = adw_switch_row_get_active (ADW_SWITCH_ROW (self->no_pot_cb));
  o->no_comments =
      adw_switch_row_get_active (ADW_SWITCH_ROW (self->no_comments_cb));
  o->no_subs = adw_switch_row_get_active (ADW_SWITCH_ROW (self->no_subs_cb));
  o->no_thumbnail =
      adw_switch_row_get_active (ADW_SWITCH_ROW (self->no_thumbnail_cb));
  o->no_metadata =
      adw_switch_row_get_active (ADW_SWITCH_ROW (self->no_metadata_cb));

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

  /* Escaped before it becomes markup: a command preview contains quotes and
   * can contain an ampersand from a query string, and Pango drops the WHOLE
   * label on a parse error rather than the offending character. */
  g_autofree char *cmd = ytdl_run_options_command_preview (o);
  g_autofree char *escaped = g_markup_escape_text (cmd, -1);
  g_autofree char *markup = g_strdup_printf ("<tt>%s</tt>", escaped);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->preview), markup);
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
  /* The placeholder is not log content: it is dropped whole the moment there
   * is real output, before any of the append logic looks at the buffer. */
  if (self->log_placeholder)
    {
      gtk_text_buffer_set_text (self->log_buf, "", -1);
      self->log_placeholder = FALSE;
    }

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

/* Removes the ROWS, and deliberately not gtk_list_box_remove_all().
 *
 * remove_all() walks every child of the list box, and the placeholder is one
 * of them; gtk_list_box_remove() special-cases the placeholder and clears it
 * rather than unparenting a row. So the first refresh -- which happens in the
 * constructor, before anything has ever been queued -- threw the empty state
 * away, and every empty list after that was a blank rectangle. Removing by
 * index only ever touches rows. */
static void
clear_list (GtkWidget *list)
{
  GtkListBoxRow *row;
  while ((row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (list), 0)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (list), GTK_WIDGET (row));
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
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                 r->opts != NULL && r->opts->url != NULL
                                     ? r->opts->url
                                     : "");
  /* The URL is data, not markup: a title with an ampersand in it is a common
   * enough query string, and Pango would refuse the whole label. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);

  GtkWidget *rm = gtk_button_new_from_icon_name ("list-remove-symbolic");
  gtk_widget_set_tooltip_text (rm, "Remove from the queue");
  gtk_widget_set_valign (rm, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (rm, "flat");
  g_object_set_data_full (G_OBJECT (rm), "run-id", g_strdup (r->id), g_free);
  g_signal_connect (rm, "clicked", G_CALLBACK (on_remove_queued), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), rm);

  return row;
}

static GtkWidget *
make_history_row (const YtdlRunRecord *r)
{
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                 r->command != NULL ? r->command : "");
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_action_row_set_title_lines (ADW_ACTION_ROW (row), 1);

  /* The four counts exist only in the pipeline's own session summary line,
   * which is why they are parsed out of it as the run goes. A run that was
   * cancelled or died early never printed one, and shows nothing rather than
   * four zeroes that would read as "it ran and found nothing". */
  g_autofree char *when = format_when (r->started);
  g_autofree char *counts =
      r->videos_touched >= 0
          ? g_strdup_printf (" · %" G_GINT64_FORMAT " touched · %"
                             G_GINT64_FORMAT " skipped · %" G_GINT64_FORMAT
                             " errors · %" G_GINT64_FORMAT " warnings",
                             r->videos_touched, r->archive_skipped, r->errors,
                             r->warnings)
          : g_strdup ("");
  g_autofree char *last =
      (r->last_line != NULL && *r->last_line != '\0')
          ? g_strdup_printf ("\n%s", r->last_line)
          : g_strdup ("");
  g_autofree char *subtitle = g_strdup_printf ("%s%s%s", when, counts, last);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 2);

  GtkWidget *state = gtk_label_new (r->state);
  gtk_widget_add_css_class (state, "ytdl-pill");
  gtk_widget_set_valign (state, GTK_ALIGN_CENTER);
  if (g_strcmp0 (r->state, "done") == 0)
    gtk_widget_add_css_class (state, "ok");
  else if (g_strcmp0 (r->state, "failed") == 0)
    gtk_widget_add_css_class (state, "err");
  else
    gtk_widget_add_css_class (state, "warn");
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), state);

  return row;
}

static void
on_state_changed (YtdlRunner *runner, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;

  g_autoptr (GPtrArray) queue = ytdl_runner_queue (self->runner);
  g_autoptr (GPtrArray) history = ytdl_runner_history (self->runner);
  YtdlRunRecord *current = ytdl_runner_current (self->runner);

  clear_list (self->queue_box);
  for (guint i = 0; i < queue->len; i++)
    gtk_list_box_append (GTK_LIST_BOX (self->queue_box),
                         make_queue_row (self, g_ptr_array_index (queue, i)));

  clear_list (self->history_box);
  for (guint i = 0; i < history->len; i++)
    gtk_list_box_append (GTK_LIST_BOX (self->history_box),
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

  adw_spin_row_set_value (ADW_SPIN_ROW (self->workers),
                          o->workers > 0 ? o->workers : 1);
  adw_switch_row_set_active (ADW_SWITCH_ROW (self->sync_cb), o->sync);
  adw_switch_row_set_active (ADW_SWITCH_ROW (self->lazy_cb), o->lazy);
  adw_switch_row_set_active (ADW_SWITCH_ROW (self->no_pot_cb), o->no_pot);
  adw_switch_row_set_active (ADW_SWITCH_ROW (self->no_comments_cb),
                             o->no_comments);
  adw_switch_row_set_active (ADW_SWITCH_ROW (self->no_subs_cb), o->no_subs);
  adw_switch_row_set_active (ADW_SWITCH_ROW (self->no_thumbnail_cb),
                             o->no_thumbnail);
  adw_switch_row_set_active (ADW_SWITCH_ROW (self->no_metadata_cb),
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
  /* Hidden rather than empty: an empty label still claims a line, which put a
   * gap between the Profile group and the next one that looked like a missing
   * widget. */
  gtk_widget_set_visible (self->profile_status, text != NULL && *text != '\0');
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

  adw_combo_row_set_selected (ADW_COMBO_ROW (self->profile_drop), selected);
  self->applying = FALSE;
}

static const char *
selected_profile_name (YtdlDownloadsView *self)
{
  guint i = adw_combo_row_get_selected (ADW_COMBO_ROW (self->profile_drop));
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

/* Applies whatever the name dialog collected. Shared by the OK button and by
 * Enter in the entry, which are the same intent and must not be two
 * implementations of it. */
static void
commit_name (YtdlDownloadsView *self, const char *typed)
{
  GError *error = NULL;
  gboolean ok;
  g_autofree char *msg = NULL;

  if (self->renaming)
    {
      const char *from = selected_profile_name (self);
      ok = from != NULL &&
           ytdl_profiles_rename (self->store, from, typed, &error);
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

  refresh_profiles (self);
  set_profile_status (self, msg, FALSE);
}

static void
on_name_response (AdwAlertDialog *dlg, const char *response, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;

  /* Enter already committed and closed us; closing emits a response of its
   * own, and running the save twice would report "a profile by that name
   * already exists" for the profile just created. */
  if (g_object_get_data (G_OBJECT (dlg), "ytdl-committed") != NULL)
    return;
  if (g_strcmp0 (response, "confirm") != 0)
    return;

  GtkWidget *entry = g_object_get_data (G_OBJECT (dlg), "ytdl-entry");
  commit_name (self, gtk_editable_get_text (GTK_EDITABLE (entry)));
}

static void
on_name_activated (GtkWidget *entry, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  GtkWidget *dlg = g_object_get_data (G_OBJECT (entry), "ytdl-dialog");

  commit_name (self, gtk_editable_get_text (GTK_EDITABLE (entry)));
  g_object_set_data (G_OBJECT (dlg), "ytdl-committed", GINT_TO_POINTER (1));
  adw_dialog_close (ADW_DIALOG (dlg));
}

/* An AdwAlertDialog rather than the inline row this used to be. The dialog is
 * what the 1.5 floor in meson.build buys: it is presented INTO the window
 * rather than as a second toplevel, so it is a sheet on a narrow window and a
 * centred dialog on a wide one, and Escape, the close button and the focus
 * trap are all libadwaita's rather than three more handlers here. */
static void
show_name_dialog (YtdlDownloadsView *self, gboolean renaming,
                  const char *initial)
{
  self->renaming = renaming;

  AdwDialog *dlg = adw_alert_dialog_new (
      renaming ? "Rename profile" : "Save these options as a profile", NULL);
  adw_alert_dialog_set_body (
      ADW_ALERT_DIALOG (dlg),
      renaming ? "The options stay as they are; only the name changes."
               : "Every option except the URL is saved under this name.");

  GtkWidget *entry = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (entry), "Name");
  gtk_editable_set_text (GTK_EDITABLE (entry), initial != NULL ? initial : "");
  g_object_set_data (G_OBJECT (entry), "ytdl-dialog", dlg);
  g_signal_connect (entry, "entry-activated", G_CALLBACK (on_name_activated),
                    self);

  GtkWidget *group = adw_preferences_group_new ();
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), entry);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dlg), group);

  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg), "cancel", "_Cancel",
                                  "confirm",
                                  renaming ? "_Rename" : "_Save", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg), "confirm",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "confirm");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "cancel");

  g_object_set_data (G_OBJECT (dlg), "ytdl-entry", entry);
  g_signal_connect (dlg, "response", G_CALLBACK (on_name_response), self);

  adw_dialog_present (dlg, GTK_WIDGET (self));
  gtk_widget_grab_focus (entry);
}

static void
on_profile_save (GtkButton *b, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  /* Pre-filled with the current profile's name, so Save over an existing one
   * is Enter rather than retyping. */
  show_name_dialog (self, FALSE, selected_profile_name (self));
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
  show_name_dialog (self, TRUE, name);
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

/* ---------------------------------------------------------------------- */
/* Actions                                                                */
/* ---------------------------------------------------------------------- */

static void
show_error (YtdlDownloadsView *self, const char *message)
{
  AdwDialog *dlg = adw_alert_dialog_new ("Cannot do that", message);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dlg), "ok", "_OK");
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "ok");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "ok");
  adw_dialog_present (dlg, GTK_WIDGET (self));
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

/* Every option is a row in a boxed list, so the builder makes a row rather
 * than a control plus a caption label. The subtitle is where the explanation
 * goes that used to live in a tooltip nobody hovers over. */
static GtkWidget *
make_combo (const char *const *ids, const char *const *labels,
            const char *title, const char *subtitle, const char *active)
{
  GtkWidget *row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle != NULL)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);

  g_autoptr (GtkStringList) model = gtk_string_list_new (labels);
  adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (model));

  /* The ids arrays are static, so storing the pointer rather than copying is
   * safe for the life of the widget. */
  g_object_set_data (G_OBJECT (row), "ytdl-ids", (gpointer) ids);

  guint selected = 0;
  for (gsize i = 0; ids[i] != NULL; i++)
    if (g_strcmp0 (ids[i], active) == 0)
      {
        selected = (guint) i;
        break;
      }
  adw_combo_row_set_selected (ADW_COMBO_ROW (row), selected);
  return row;
}

/* A boxed list with something to say when it is empty. GtkListBox's own
 * placeholder is used rather than a row appended when the list is empty,
 * because a placeholder is not a row: it cannot be selected, activated or
 * counted, which a fake "nothing here" row can be and eventually is. */
static GtkWidget *
make_list (const char *empty_text)
{
  GtkWidget *list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (list, "boxed-list");
  gtk_widget_set_valign (list, GTK_ALIGN_START);

  GtkWidget *l = gtk_label_new (empty_text);
  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  gtk_label_set_justify (GTK_LABEL (l), GTK_JUSTIFY_CENTER);
  gtk_widget_set_margin_top (l, 18);
  gtk_widget_set_margin_bottom (l, 18);
  gtk_widget_set_margin_start (l, 12);
  gtk_widget_set_margin_end (l, 12);
  gtk_widget_add_css_class (l, "dim-label");
  gtk_list_box_set_placeholder (GTK_LIST_BOX (list), l);

  return list;
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
  /* No margins and no spacing: AdwClamp owns the horizontal measure and the
   * preference groups own the vertical rhythm. Margins set here would be a
   * second opinion about both. */
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);
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

  /* ------------------------------------------------------------------
   * The form: five boxed lists in a clamp, not a wall of controls.
   *
   * AdwPreferencesGroup is what gives each block a title, a description, the
   * rounded frame, the separators and the spacing between blocks -- and it is
   * why every option can now carry a one-line explanation as a subtitle
   * instead of a tooltip nobody hovers over.
   * ------------------------------------------------------------------ */
  GtkWidget *form = gtk_box_new (GTK_ORIENTATION_VERTICAL, 24);
  gtk_widget_set_margin_start (form, 12);
  gtk_widget_set_margin_end (form, 12);
  gtk_widget_set_margin_top (form, 18);
  gtk_widget_set_margin_bottom (form, 24);

  /* --- Profiles ------------------------------------------------------ */
  GtkWidget *pgroup = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (pgroup), "Profile");

  GtkWidget *pbtns = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *psave = gtk_button_new_with_label ("Save as…");
  g_signal_connect (psave, "clicked", G_CALLBACK (on_profile_save), self);
  gtk_box_append (GTK_BOX (pbtns), psave);
  GtkWidget *pren = gtk_button_new_with_label ("Rename…");
  g_signal_connect (pren, "clicked", G_CALLBACK (on_profile_rename), self);
  gtk_box_append (GTK_BOX (pbtns), pren);
  /* No destructive-action class: a scarlet button in a group header reads as
   * the primary action of the page, and this one deletes a preset -- not
   * data. The confirmation it deserves is the one the label already gives. */
  GtkWidget *pdel = gtk_button_new_with_label ("Delete");
  g_signal_connect (pdel, "clicked", G_CALLBACK (on_profile_delete), self);
  gtk_box_append (GTK_BOX (pbtns), pdel);
  adw_preferences_group_set_header_suffix (ADW_PREFERENCES_GROUP (pgroup),
                                           pbtns);

  const char *const initial[] = { "(no profile)", NULL };
  self->profile_list = gtk_string_list_new (initial);
  self->profile_drop = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->profile_drop),
                                 "Active profile");
  adw_action_row_set_subtitle (
      ADW_ACTION_ROW (self->profile_drop),
      "Applies its options and leaves the URL alone — a preset that replaced "
      "what you were about to download would be the one thing a preset must "
      "never do.");
  adw_combo_row_set_model (ADW_COMBO_ROW (self->profile_drop),
                           G_LIST_MODEL (self->profile_list));
  g_signal_connect (self->profile_drop, "notify::selected",
                    G_CALLBACK (on_profile_selected), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (pgroup),
                             self->profile_drop);
  gtk_box_append (GTK_BOX (form), pgroup);

  self->profile_status = gtk_label_new ("");
  gtk_widget_set_visible (self->profile_status, FALSE);
  gtk_label_set_xalign (GTK_LABEL (self->profile_status), 0.0f);
  gtk_label_set_wrap (GTK_LABEL (self->profile_status), TRUE);
  gtk_widget_add_css_class (self->profile_status, "caption");
  gtk_widget_add_css_class (self->profile_status, "dim-label");
  gtk_box_append (GTK_BOX (form), self->profile_status);

  /* --- URL + destination -------------------------------------------- */
  GtkWidget *dgroup = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (dgroup), "Download");

  self->start = gtk_button_new ();
  {
    GtkWidget *content = adw_button_content_new ();
    adw_button_content_set_icon_name (ADW_BUTTON_CONTENT (content),
                                      "list-add-symbolic");
    adw_button_content_set_label (ADW_BUTTON_CONTENT (content),
                                  "Add to queue");
    gtk_button_set_child (GTK_BUTTON (self->start), content);
  }
  gtk_widget_add_css_class (self->start, "suggested-action");
  g_signal_connect (self->start, "clicked", G_CALLBACK (on_start), self);
  adw_preferences_group_set_header_suffix (ADW_PREFERENCES_GROUP (dgroup),
                                           self->start);

  self->url = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->url),
                                 "Video, playlist or channel URL");
  g_signal_connect (self->url, "changed", G_CALLBACK (on_form_changed), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (dgroup), self->url);

  self->dest = adw_entry_row_new ();
  adw_preferences_row_set_title (
      ADW_PREFERENCES_ROW (self->dest),
      "Destination — empty means the pipeline's own default");
  if (settings->data_root != NULL)
    gtk_editable_set_text (GTK_EDITABLE (self->dest), settings->data_root);
  g_signal_connect (self->dest, "changed", G_CALLBACK (on_dest_changed), self);

  GtkWidget *browse = gtk_button_new_from_icon_name ("folder-open-symbolic");
  gtk_widget_set_tooltip_text (browse, "Choose a destination folder");
  gtk_widget_set_valign (browse, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (browse, "flat");
  g_signal_connect (browse, "clicked", G_CALLBACK (on_browse), self);
  adw_entry_row_add_suffix (ADW_ENTRY_ROW (self->dest), browse);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (dgroup), self->dest);
  gtk_box_append (GTK_BOX (form), dgroup);

  /* --- Format -------------------------------------------------------- */
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

  self->mode = make_combo (mode_ids, mode_labels, "Mode",
                           "Which parts of each video to fetch.", "full");
  self->quality = make_combo (q_ids, q_labels, "Quality",
                              "A ceiling, not a demand — a video that was "
                              "never published at this height comes down at "
                              "the best it has.",
                              "best");
  self->codec = make_combo (c_ids, c_labels, "Video codec", NULL, "any");
  self->audio_codec = make_combo (a_ids, a_labels, "Audio codec", NULL, "any");
  self->container = make_combo (k_ids, k_labels, "Container", NULL, "mkv");

  GtkWidget *fgroup = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (fgroup), "Format");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (fgroup), self->mode);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (fgroup), self->quality);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (fgroup), self->codec);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (fgroup),
                             self->audio_codec);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (fgroup), self->container);

  self->workers = adw_spin_row_new_with_range (1, 16, 1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->workers),
                                 "Workers");
  adw_action_row_set_subtitle (
      ADW_ACTION_ROW (self->workers),
      "The pipeline's own parallelism. The queue here is always sequential, "
      "because independent ytdl invocations race on the shared manifests — "
      "--workers is the supported way to run several at once.");
  adw_spin_row_set_value (ADW_SPIN_ROW (self->workers),
                          settings->default_workers);
  g_signal_connect (self->workers, "notify::value",
                    G_CALLBACK (on_notify_changed), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (fgroup), self->workers);
  gtk_box_append (GTK_BOX (form), fgroup);

  GtkWidget *const combos[] = { self->mode,        self->quality,
                                self->codec,       self->audio_codec,
                                self->container };
  for (gsize i = 0; i < G_N_ELEMENTS (combos); i++)
    g_signal_connect (combos[i], "notify::selected",
                      G_CALLBACK (on_notify_changed), self);

  /* --- Passes -------------------------------------------------------- */
  GtkWidget *sgroup = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (sgroup), "Passes");
  adw_preferences_group_set_description (
      ADW_PREFERENCES_GROUP (sgroup),
      "Everything is on by default. Turning a pass off makes a run faster and "
      "the archive less complete.");

  /* AdwSwitchRow, not a check button with a label beside it: a switch says
   * "this is a setting that stays" where a check box says "this applies to
   * the thing I am about to press OK on". These stay. */
#define SWITCH(field, title, sub)                                             \
  do                                                                          \
    {                                                                         \
      self->field = adw_switch_row_new ();                                    \
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->field),       \
                                     title);                                  \
      adw_action_row_set_subtitle (ADW_ACTION_ROW (self->field), sub);        \
      g_signal_connect (self->field, "notify::active",                        \
                        G_CALLBACK (on_notify_changed), self);                \
      adw_preferences_group_add (ADW_PREFERENCES_GROUP (sgroup),              \
                                 self->field);                                \
    }                                                                         \
  while (0)

  SWITCH (sync_cb, "Sync",
          "Walk the whole channel, stopping at the first video already "
          "archived.");
  SWITCH (lazy_cb, "Lazy", "Skip the up-front enumeration.");
  SWITCH (no_pot_cb, "Skip PO token",
          "Do not run the PO token provider for this run.");
  SWITCH (no_comments_cb, "Skip comments", "Do not run the comments pass.");
  SWITCH (no_subs_cb, "Skip subtitles", "Do not capture subtitles.");
  SWITCH (no_thumbnail_cb, "Skip thumbnail", "Do not capture the thumbnail.");
  SWITCH (no_metadata_cb, "Skip metadata", "Do not run the metadata pass.");
#undef SWITCH
  gtk_box_append (GTK_BOX (form), sgroup);

  /* --- Passthrough and the command ----------------------------------- */
  GtkWidget *agroup = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (agroup), "Advanced");

  self->extra_args = adw_entry_row_new ();
  adw_preferences_row_set_title (
      ADW_PREFERENCES_ROW (self->extra_args),
      "Extra yt-dlp arguments, one per line");
  g_signal_connect (self->extra_args, "changed", G_CALLBACK (on_form_changed),
                    self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (agroup), self->extra_args);

  self->preview = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->preview),
                                 "This is the command that will run");
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (self->preview), 0);
  adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (self->preview),
                                          TRUE);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (agroup), self->preview);
  gtk_box_append (GTK_BOX (form), agroup);

  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 780);
  adw_clamp_set_tightening_threshold (ADW_CLAMP (clamp), 600);
  adw_clamp_set_child (ADW_CLAMP (clamp), form);

  GtkWidget *form_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (form_scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (form_scroll), clamp);

  /* ------------------------------------------------------------------
   * The run area: what is happening now, below the form that describes
   * what will happen next.
   * ------------------------------------------------------------------ */
  GtkWidget *run = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (run, 12);
  gtk_widget_set_margin_end (run, 12);
  gtk_widget_set_margin_top (run, 8);
  gtk_widget_set_margin_bottom (run, 12);

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
  gtk_label_set_ellipsize (GTK_LABEL (self->stage), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (self->stage, TRUE);
  gtk_widget_add_css_class (self->stage, "dim-label");
  gtk_box_append (GTK_BOX (controls), self->stage);
  gtk_box_append (GTK_BOX (run), controls);

  self->progress = gtk_progress_bar_new ();
  gtk_box_append (GTK_BOX (run), self->progress);

  /* --- Log ----------------------------------------------------------- */
  self->log = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (self->log), FALSE);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (self->log), TRUE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (self->log), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (self->log), 10);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (self->log), 10);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (self->log), 8);
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (self->log), 8);
  gtk_widget_add_css_class (self->log, "ytdl-log");
  self->log_buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->log));
  gtk_text_buffer_set_text (
      self->log_buf,
      "Output from ytdl.ps1 appears here once a run starts.", -1);
  self->log_placeholder = TRUE;

  GtkWidget *log_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (log_scroll), self->log);
  gtk_widget_add_css_class (log_scroll, "card");
  gtk_widget_set_vexpand (log_scroll, TRUE);
  gtk_widget_set_size_request (log_scroll, -1, 120);

  /* --- Queue and history --------------------------------------------- */
  self->queue_box = make_list ("Nothing queued.\nPaste a URL above and press "
                               "Add to queue.");
  self->history_box = make_list ("No runs yet this session.");

  GtkWidget *qsec = section ("Queue (0)", &self->queue_title);
  GtkWidget *qscroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (qscroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (qscroll),
                                 self->queue_box);
  gtk_widget_set_vexpand (qscroll, TRUE);
  gtk_widget_set_size_request (qscroll, -1, 110);
  gtk_box_append (GTK_BOX (qsec), qscroll);

  GtkWidget *hsec = section ("History (0)", &self->history_title);
  GtkWidget *hscroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (hscroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (hscroll),
                                 self->history_box);
  gtk_widget_set_vexpand (hscroll, TRUE);
  gtk_widget_set_size_request (hscroll, -1, 110);
  gtk_box_append (GTK_BOX (hsec), hscroll);

  GtkWidget *lists = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_start_child (GTK_PANED (lists), qsec);
  gtk_paned_set_end_child (GTK_PANED (lists), hsec);
  gtk_paned_set_position (GTK_PANED (lists), 360);

  GtkWidget *run_split = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_start_child (GTK_PANED (run_split), log_scroll);
  gtk_paned_set_end_child (GTK_PANED (run_split), lists);
  gtk_paned_set_position (GTK_PANED (run_split), 150);
  gtk_widget_set_vexpand (run_split, TRUE);
  gtk_box_append (GTK_BOX (run), run_split);

  /* The split is draggable because the two halves are wanted in different
   * proportions at different times: all form while setting a run up, all log
   * while watching one. A fixed ratio would be wrong in both states. */
  GtkWidget *split = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_start_child (GTK_PANED (split), form_scroll);
  gtk_paned_set_end_child (GTK_PANED (split), run);
  gtk_paned_set_position (GTK_PANED (split), 470);
  gtk_paned_set_shrink_start_child (GTK_PANED (split), FALSE);
  gtk_paned_set_shrink_end_child (GTK_PANED (split), FALSE);
  gtk_widget_set_vexpand (split, TRUE);
  gtk_box_append (GTK_BOX (self), split);

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
