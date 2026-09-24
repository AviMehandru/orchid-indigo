#include "downloads_view.h"

#include "paths.h"
#include "profiles.h"
#include "url_probe.h"

#include <adwaita.h>
#include <string.h>

/* How many playlist entries get a tick row before the list is summarised
 * instead. A GtkListBox builds every child eagerly, so a 500-entry channel is
 * 500 widgets constructed on the main thread the moment the expander opens --
 * which is a visible stall, for a list nobody scrolls to the bottom of. Past
 * this the range field stays the way to say what you want. */
#define MAX_ENTRY_ROWS 200

#define MAX_LOG_LINES_SHOWN 4000

struct _YtdlDownloadsView
{
  GtkBox parent_instance;

  YtdlRunner   *runner;   /* borrowed */
  YtdlSettings *settings; /* borrowed */

  GtkWidget *url;
  GtkWidget *dest;
  GtkWidget *items;
  GtkWidget *preview;

  /* The URL preview.
   *
   * The form used to know nothing about the URL in it until the run failed:
   * the Quality list was a fixed ladder, asking for 1440p AV1 was a request
   * that silently resolved to something else, and a typo'd URL was discovered
   * by a failed row in the history list. Everything below exists to answer
   * the question before Start rather than after. */
  YtdlUrlProbe *probe;         /* NULL until one has come back */
  GCancellable *probe_cancel;  /* non-NULL only while one is in flight */
  GtkWidget    *probe_button;
  GtkWidget    *probe_group;
  GtkWidget    *probe_status;
  GtkWidget    *probe_spinner;
  GtkWidget    *probe_meta;
  GtkWidget    *probe_thumb;
  GtkWidget    *probe_note;
  GtkWidget    *probe_formats;
  GtkWidget    *probe_entries;
  /* AdwExpanderRow can add and remove rows and cannot list them, so what was
   * put into each of the two expanders is remembered here to be taken out
   * again on the next probe. */
  GPtrArray    *format_rows;  /* GtkWidget* */
  GPtrArray    *entry_rows;   /* GtkWidget* */
  GPtrArray    *entry_checks; /* GtkWidget*, one per row above */
  /* Set while the form is being rewritten from a probe, so the rebuild of one
   * combo's model does not reenter through its own notify::selected and
   * rebuild the others underneath itself. */
  gboolean      applying_probe;

  GtkWidget *mode, *quality, *codec, *audio_codec, *container;
  GtkWidget *workers;
  GtkWidget *sync_cb, *lazy_cb, *no_pot_cb;
  GtkWidget *no_comments_cb, *no_subs_cb, *no_thumbnail_cb, *no_metadata_cb;
  GtkWidget *extra_args;

  /* The options that used to be reachable only by typing yt-dlp's spelling
   * into the Advanced box. Per run, and saved in a profile like the rest of
   * the form. */
  GtkWidget *fps;
  GtkWidget *sub_langs;
  GtkWidget *chapters_cb; /* ON means embed -- the inverse of --no-chapters */
  GtkWidget *sponsor_mode;
  GtkWidget *sponsor_cats;

  /* The Connection group. NOT per run and NOT in a profile: these are
   * settings, written to settings.json as they change and handed to the
   * runner, which stamps them onto every run the app starts. See settings.h
   * for why. */
  GtkWidget *cookies_source;
  GtkWidget *cookies_browser;
  GtkWidget *cookies_profile;
  GtkWidget *cookies_file;
  GtkWidget *proxy;
  GtkWidget *limit_rate;
  GtkWidget *downloader;
  gboolean   loading_connection; /* suppresses the save while filling */

  /* One sentence under the Queue heading. See update_queue_note. */
  GtkWidget *queue_note;

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
  GtkWidget *clear_history;

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

/* Replace a combo's contents with a new set.
 *
 * The id array a combo carries starts out as one of the static tables in the
 * constructor and is REPLACED here by an owned copy when a probe rebuilds the
 * row from what the video actually offers. g_object_set_data_full frees the
 * previous owned array when the next one lands; the original static table has
 * no destroy notify, so it is never passed to g_free. Both are read through
 * the same const pointer in combo_value, which is why nothing there has to
 * know which kind it is holding.
 *
 * @keep is the value to reselect if the new set still contains it. When it
 * does not -- 1440p on a video that turned out to be 1080p at best -- the
 * first entry is selected instead and the caller says so in the note row,
 * rather than leaving a selection that reads as honoured and is not. */
static void
combo_rebuild (GtkWidget *w, const char *const *ids, const char *const *labels,
               const char *keep)
{
  g_autoptr (GtkStringList) model = gtk_string_list_new (labels);
  adw_combo_row_set_model (ADW_COMBO_ROW (w), G_LIST_MODEL (model));
  g_object_set_data_full (G_OBJECT (w), "ytdl-ids", g_strdupv ((GStrv) ids),
                          (GDestroyNotify) g_strfreev);

  guint selected = 0;
  for (gsize i = 0; ids[i] != NULL; i++)
    if (g_strcmp0 (ids[i], keep) == 0)
      {
        selected = (guint) i;
        break;
      }
  adw_combo_row_set_selected (ADW_COMBO_ROW (w), selected);
}

/* Put back the full static list this row was built with. Called when the
 * preview is cleared, because a dropdown describing the previous URL is worse
 * than one describing no URL at all. */
static void
combo_restore_default (GtkWidget *w)
{
  const char *const *ids = g_object_get_data (G_OBJECT (w), "ytdl-ids-default");
  const char *const *labels =
      g_object_get_data (G_OBJECT (w), "ytdl-labels-default");
  if (ids == NULL || labels == NULL)
    return;
  g_autofree char *keep = g_strdup (combo_value (w));
  combo_rebuild (w, ids, labels, keep);
}

static YtdlRunOptions *
collect (YtdlDownloadsView *self)
{
  YtdlRunOptions *o = ytdl_run_options_new ();

  o->url = g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->url)));
  o->data_root = g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->dest)));
  o->items = g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->items)));

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

  o->fps = (guint) g_ascii_strtoull (combo_value (self->fps), NULL, 10);
  {
    g_autofree char *langs =
        g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->sub_langs))));
    if (*langs != '\0')
      o->sub_langs = g_steal_pointer (&langs);
  }
  o->no_chapters =
      !adw_switch_row_get_active (ADW_SWITCH_ROW (self->chapters_cb));
  {
    const char *how = combo_value (self->sponsor_mode);
    g_autofree char *cats = g_strstrip (
        g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->sponsor_cats))));
    /* An empty category list with a mode chosen is sent as "sponsor" rather
     * than dropped: the mode row says SponsorBlock is on, and a run that
     * quietly did nothing about it would contradict the form. "sponsor" is
     * the category the field is pre-filled with, so this is only ever the
     * user clearing the field and forgetting to type another. */
    if (*cats == '\0')
      {
        g_free (cats);
        cats = g_strdup ("sponsor");
      }
    if (g_strcmp0 (how, "mark") == 0)
      o->sponsorblock_mark = g_steal_pointer (&cats);
    else if (g_strcmp0 (how, "remove") == 0)
      o->sponsorblock_remove = g_steal_pointer (&cats);
  }

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

  /* Whatever the form is showing greyed out stays out of the command line.
   * See the header comment on this function for why it is not validation. */
  ytdl_run_options_drop_inapplicable (o);
  return o;
}

/* Grey out what cannot apply, mirroring ytdl_run_options_drop_inapplicable
 * rule for rule -- the one decides what the user SEES, the other what is
 * SENT, and the two must never disagree. Insensitive rather than hidden, so
 * the value is still visible and comes back when the mode does. */
static void
update_sensitivity (YtdlDownloadsView *self)
{
  static const char *const no_media[] = { "metadata-only", "comments-only",
                                          "subs-only", NULL };
  /* Every control this touches is built after the Format group whose
   * notify::selected brings us here; a notification during construction must
   * not reach a widget that does not exist yet. */
  if (self->fps == NULL || self->sponsor_mode == NULL
      || self->sponsor_cats == NULL || self->sub_langs == NULL)
    return;

  gboolean media = !g_strv_contains (no_media, combo_value (self->mode));
  gboolean marking = g_strcmp0 (combo_value (self->sponsor_mode), "mark") == 0;
  gboolean sponsoring = g_strcmp0 (combo_value (self->sponsor_mode), "off") != 0;

  gtk_widget_set_sensitive (self->fps, media);
  gtk_widget_set_sensitive (self->sponsor_mode, media);
  gtk_widget_set_sensitive (self->sponsor_cats, media && sponsoring);
  gtk_widget_set_sensitive (self->chapters_cb, media && !marking);
  gtk_widget_set_sensitive (
      self->sub_langs,
      !adw_switch_row_get_active (ADW_SWITCH_ROW (self->no_subs_cb)));
}

static void
refresh_preview (YtdlDownloadsView *self)
{
  g_autoptr (YtdlRunOptions) o = collect (self);
  {
    /* The runner stamps the Connection settings onto every run at enqueue,
     * so the preview has to show them too or it would not be the command
     * that runs. The proxy's password is masked by the preview itself. */
    g_autoptr (YtdlRunOptions) conn = ytdl_settings_connection (self->settings);
    ytdl_run_options_set_connection (o, conn);
  }

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
  update_sensitivity (user_data);
  refresh_preview (user_data);
}

static void rebuild_quality_for_codec (YtdlDownloadsView *self);

/* GtkDropDown reports selection through a property notification rather than a
 * "changed" signal, so it needs the three-argument shape. */
static void
on_notify_changed (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;

  /* Changing the codec changes which heights are reachable: 1440p is commonly
   * published only in VP9, so a Quality list built from the union of every
   * height offers a combination this video does not have -- the same silent
   * wrong answer the static ladder gave, with better-looking numbers in it. */
  if (!self->applying_probe && self->probe != NULL
      && obj == G_OBJECT (self->codec))
    rebuild_quality_for_codec (self);

  update_sensitivity (self);
  refresh_preview (self);
}

/* ---------------------------------------------------------------------- */
/* The URL preview                                                        */
/* ---------------------------------------------------------------------- */

static char *
human_duration (double seconds)
{
  if (seconds <= 0.0)
    return g_strdup ("");
  int total = (int) (seconds + 0.5);
  int h = total / 3600;
  int m = (total % 3600) / 60;
  int s = total % 60;
  if (h > 0)
    return g_strdup_printf ("%d:%02d:%02d", h, m, s);
  return g_strdup_printf ("%d:%02d", m, s);
}

/* yt-dlp's upload_date is YYYYMMDD with no separators, which reads as a serial
 * number rather than a date. Reformatted by slicing rather than by parsing
 * into a GDateTime: there is no timezone to get right here, and a date that
 * fails to parse should come out blank rather than as 1970. */
static char *
human_date (const char *yyyymmdd)
{
  if (yyyymmdd == NULL || strlen (yyyymmdd) != 8)
    return g_strdup ("");
  for (int i = 0; i < 8; i++)
    if (!g_ascii_isdigit (yyyymmdd[i]))
      return g_strdup ("");
  return g_strdup_printf ("%.4s-%.2s-%.2s", yyyymmdd, yyyymmdd + 4,
                          yyyymmdd + 6);
}

static char *
human_count (gint64 n)
{
  if (n <= 0)
    return g_strdup ("");
  if (n >= 1000000)
    return g_strdup_printf ("%.1fM", (double) n / 1000000.0);
  if (n >= 1000)
    return g_strdup_printf ("%.1fK", (double) n / 1000.0);
  return g_strdup_printf ("%" G_GINT64_FORMAT, n);
}

static char *
human_size (gint64 bytes)
{
  if (bytes <= 0)
    return g_strdup ("");
  return g_format_size ((guint64) bytes);
}

/* AdwExpanderRow has add_row and remove, and no way to enumerate or clear what
 * it holds -- so the rows added to one have to be remembered to be taken back
 * out. Both preview expanders are rebuilt from scratch on every probe, and
 * without this the second probe of a session shows the first one's formats
 * underneath its own. */
static void
drop_expander_rows (GtkWidget *expander, GPtrArray *rows)
{
  if (rows == NULL)
    return;
  for (guint i = 0; i < rows->len; i++)
    adw_expander_row_remove (ADW_EXPANDER_ROW (expander),
                             g_ptr_array_index (rows, i));
  g_ptr_array_set_size (rows, 0);
}

static void
clear_entry_rows (YtdlDownloadsView *self)
{
  drop_expander_rows (self->probe_entries, self->entry_rows);
  if (self->entry_checks != NULL)
    g_ptr_array_set_size (self->entry_checks, 0);
}

/* Everything the preview put on screen goes away, and every dropdown goes
 * back to its full static list.
 *
 * Called when the URL changes, which is the important case: a Quality row
 * still showing the heights of the PREVIOUS video, against a URL that has
 * been replaced, is worse than one showing the generic ladder -- it looks
 * like knowledge and is not. */
static void
clear_probe (YtdlDownloadsView *self)
{
  if (self->probe_cancel != NULL)
    {
      g_cancellable_cancel (self->probe_cancel);
      g_clear_object (&self->probe_cancel);
    }
  g_clear_pointer (&self->probe, ytdl_url_probe_free);

  clear_entry_rows (self);
  gtk_widget_set_visible (self->probe_group, FALSE);
  gtk_widget_set_visible (self->probe_spinner, FALSE);
  gtk_widget_set_sensitive (self->probe_button, TRUE);
  gtk_picture_set_paintable (GTK_PICTURE (self->probe_thumb), NULL);

  self->applying_probe = TRUE;
  combo_restore_default (self->quality);
  combo_restore_default (self->codec);
  combo_restore_default (self->audio_codec);
  combo_restore_default (self->container);
  self->applying_probe = FALSE;
  refresh_preview (self);
}

static void
on_url_changed (GtkEditable *editable, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  if (self->probe != NULL || self->probe_cancel != NULL)
    clear_probe (self);
  refresh_preview (self);
}

/* The Quality row, filtered to the heights the currently-selected codec
 * actually offers. "Best" is always first and is not one of the probed
 * heights: it is a pipeline concept, always available, and means "no cap". */
static void
rebuild_quality_for_codec (YtdlDownloadsView *self)
{
  if (self->probe == NULL)
    return;

  g_autofree char *codec = g_strdup (combo_value (self->codec));
  g_autoptr (GArray) heights =
      ytdl_url_probe_heights_for_codec (self->probe, codec);

  g_autoptr (GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (ids, g_strdup ("best"));
  g_ptr_array_add (labels, g_strdup ("Best"));
  for (guint i = 0; i < heights->len; i++)
    {
      int h = g_array_index (heights, int, i);
      g_ptr_array_add (ids, g_strdup_printf ("%d", h));
      g_ptr_array_add (labels, g_strdup_printf ("%dp", h));
    }
  g_ptr_array_add (ids, NULL);
  g_ptr_array_add (labels, NULL);

  g_autofree char *keep = g_strdup (combo_value (self->quality));
  gboolean was_applying = self->applying_probe;
  self->applying_probe = TRUE;
  combo_rebuild (self->quality, (const char *const *) ids->pdata,
                 (const char *const *) labels->pdata, keep);
  self->applying_probe = was_applying;
}

static const char *
video_codec_label (const char *family)
{
  if (g_strcmp0 (family, "avc1") == 0)
    return "AVC1 / H.264";
  if (g_strcmp0 (family, "vp9") == 0)
    return "VP9";
  if (g_strcmp0 (family, "av01") == 0)
    return "AV1";
  return family;
}

static const char *
audio_codec_label (const char *family)
{
  if (g_strcmp0 (family, "opus") == 0)
    return "Opus";
  if (g_strcmp0 (family, "aac") == 0)
    return "AAC";
  if (g_strcmp0 (family, "mp3") == 0)
    return "MP3";
  if (g_strcmp0 (family, "flac") == 0)
    return "FLAC";
  return family;
}

static const char *
container_label (const char *id)
{
  if (g_strcmp0 (id, "mkv") == 0)
    return "MKV";
  if (g_strcmp0 (id, "mp4") == 0)
    return "MP4";
  if (g_strcmp0 (id, "webm") == 0)
    return "WebM";
  return id;
}

/* Rebuild a codec-ish row from a list of families, with "Any" pinned first.
 * Returns TRUE when the value that was selected before survived. */
static gboolean
rebuild_family_combo (YtdlDownloadsView *self, GtkWidget *row, GStrv families,
                      const char *(*label_of) (const char *),
                      gboolean include_any)
{
  g_autoptr (GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
  if (include_any)
    {
      g_ptr_array_add (ids, g_strdup ("any"));
      g_ptr_array_add (labels, g_strdup ("Any"));
    }
  for (gsize i = 0; families != NULL && families[i] != NULL; i++)
    {
      g_ptr_array_add (ids, g_strdup (families[i]));
      g_ptr_array_add (labels, g_strdup (label_of (families[i])));
    }
  /* A probe that came back with nothing usable must not leave an EMPTY row:
   * an AdwComboRow with no model shows a blank control that cannot be opened,
   * which reads as a broken widget rather than as "this video has none of
   * these". The pipeline's own always-available value goes in instead. */
  if (ids->len == 0)
    {
      g_ptr_array_add (ids, g_strdup (include_any ? "any" : "mkv"));
      g_ptr_array_add (labels, g_strdup (include_any ? "Any" : "MKV"));
    }
  g_ptr_array_add (ids, NULL);
  g_ptr_array_add (labels, NULL);

  g_autofree char *keep = g_strdup (combo_value (row));
  gboolean survived =
      g_strv_contains ((const char *const *) ids->pdata, keep);
  combo_rebuild (row, (const char *const *) ids->pdata,
                 (const char *const *) labels->pdata, keep);
  return survived;
}

static void
fill_format_rows (YtdlDownloadsView *self)
{
  const YtdlUrlProbe *p = self->probe;
  drop_expander_rows (self->probe_formats, self->format_rows);

  gsize shown = 0;
  for (guint i = 0; i < p->formats->len; i++)
    {
      const YtdlUrlProbeFormat *f = g_ptr_array_index (p->formats, i);
      GtkWidget *row = adw_action_row_new ();

      g_autofree char *title = NULL;
      if (f->height > 0)
        title = g_strdup_printf ("%dp%s%s", f->height,
                                 f->fps >= 50.0 ? " " : "",
                                 f->fps >= 50.0 ? "high frame rate" : "");
      else
        title = g_strdup ("Audio");
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);

      GString *sub = g_string_new (NULL);
      g_string_append_printf (sub, "%s", f->format_id != NULL ? f->format_id : "?");
      if (f->vcodec != NULL && g_strcmp0 (f->vcodec, "none") != 0)
        g_string_append_printf (sub, " · %s", f->vcodec);
      if (f->acodec != NULL && g_strcmp0 (f->acodec, "none") != 0)
        g_string_append_printf (sub, " · %s", f->acodec);
      if (f->ext != NULL)
        g_string_append_printf (sub, " · %s", f->ext);
      /* An exact size and an estimate are shown differently on purpose: the
       * tilde is the difference between a fact and yt-dlp's tbr*duration
       * guess, and presenting the guess as a fact is how a 4 GB download
       * surprises someone. */
      if (f->filesize > 0)
        {
          g_autofree char *sz = human_size (f->filesize);
          g_string_append_printf (sub, " · %s", sz);
        }
      else if (f->filesize_approx > 0)
        {
          g_autofree char *sz = human_size (f->filesize_approx);
          g_string_append_printf (sub, " · ~%s", sz);
        }
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub->str);
      g_string_free (sub, TRUE);

      adw_expander_row_add_row (ADW_EXPANDER_ROW (self->probe_formats), row);
      g_ptr_array_add (self->format_rows, row);
      shown++;
    }

  g_autofree char *sub = g_strdup_printf (
      "%" G_GSIZE_FORMAT " rendition%s this video actually has", shown,
      shown == 1 ? "" : "s");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->probe_formats), sub);
  gtk_widget_set_visible (self->probe_formats, shown > 0);
}

static void write_items_from_checks (YtdlDownloadsView *self);

static void
on_entry_toggled (GtkCheckButton *check, gpointer user_data)
{
  write_items_from_checks (user_data);
}

/* Turn the ticked rows into a --items value.
 *
 * The empty string is a meaningful answer and not a failure to produce one:
 * ytdl with no --items takes the whole listing, which is what "everything is
 * ticked" means. Writing out "1-200" instead would silently CAP a 4,000-video
 * channel at the 200 entries this window happened to enumerate -- the run
 * would succeed and quietly archive a twentieth of what was asked for. So a
 * full selection only collapses to "" when the list is known to be complete. */
static void
write_items_from_checks (YtdlDownloadsView *self)
{
  if (self->entry_checks == NULL || self->entry_checks->len == 0)
    return;

  g_autoptr (GArray) picked = g_array_new (FALSE, FALSE, sizeof (int));
  gboolean all = TRUE;
  for (guint i = 0; i < self->entry_checks->len; i++)
    {
      GtkWidget *c = g_ptr_array_index (self->entry_checks, i);
      int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (c), "ytdl-index"));
      if (gtk_check_button_get_active (GTK_CHECK_BUTTON (c)))
        g_array_append_val (picked, idx);
      else
        all = FALSE;
    }

  gboolean complete =
      all && self->probe != NULL && !self->probe->entries_truncated
      && self->entry_checks->len == self->probe->entries->len;

  g_autofree char *spec =
      complete ? g_strdup ("")
               : ytdl_url_probe_items_range ((const int *) picked->data,
                                             picked->len);

  self->applying_probe = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (self->items), spec);
  self->applying_probe = FALSE;
  refresh_preview (self);
}

/* The reverse: a range typed (or restored from a profile) re-ticks the rows,
 * so the two halves of the same statement cannot disagree on screen. */
static void
sync_checks_from_items (YtdlDownloadsView *self)
{
  if (self->entry_checks == NULL || self->entry_checks->len == 0)
    return;

  const char *spec = gtk_editable_get_text (GTK_EDITABLE (self->items));
  gboolean empty = spec == NULL || *spec == '\0';
  g_autoptr (GArray) want = ytdl_url_probe_parse_items_range (spec);

  self->applying_probe = TRUE;
  for (guint i = 0; i < self->entry_checks->len; i++)
    {
      GtkWidget *c = g_ptr_array_index (self->entry_checks, i);
      int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (c), "ytdl-index"));
      gboolean on = empty;
      for (guint j = 0; !on && j < want->len; j++)
        on = g_array_index (want, int, j) == idx;
      gtk_check_button_set_active (GTK_CHECK_BUTTON (c), on);
    }
  self->applying_probe = FALSE;
}

static void
on_items_changed (GtkEditable *editable, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  /* applying_probe is set while write_items_from_checks is the one editing,
   * which is what keeps ticking a box from immediately re-deriving the boxes
   * from the text it just wrote. */
  if (!self->applying_probe)
    sync_checks_from_items (self);
  refresh_preview (self);
}

static void
on_select_all (GtkButton *btn, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  gboolean on = g_object_get_data (G_OBJECT (btn), "ytdl-select-all") != NULL;
  self->applying_probe = TRUE;
  for (guint i = 0; i < self->entry_checks->len; i++)
    gtk_check_button_set_active (
        GTK_CHECK_BUTTON (g_ptr_array_index (self->entry_checks, i)), on);
  self->applying_probe = FALSE;
  write_items_from_checks (self);
}

static void
fill_entry_rows (YtdlDownloadsView *self)
{
  const YtdlUrlProbe *p = self->probe;
  clear_entry_rows (self);

  if (p == NULL || p->entries->len == 0)
    {
      gtk_widget_set_visible (self->probe_entries, FALSE);
      return;
    }

  guint shown = MIN (p->entries->len, (guint) MAX_ENTRY_ROWS);
  for (guint i = 0; i < shown; i++)
    {
      const YtdlUrlProbeEntry *e = g_ptr_array_index (p->entries, i);

      GtkWidget *row = adw_action_row_new ();
      g_autofree char *title =
          g_strdup_printf ("%d. %s", e->index,
                           e->title != NULL ? e->title : "(untitled)");
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);

      GString *sub = g_string_new (NULL);
      if (e->duration > 0.0)
        {
          g_autofree char *d = human_duration (e->duration);
          g_string_append (sub, d);
        }
      if (e->id != NULL)
        g_string_append_printf (sub, "%s%s", sub->len > 0 ? " · " : "", e->id);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub->str);
      g_string_free (sub, TRUE);

      GtkWidget *check = gtk_check_button_new ();
      gtk_check_button_set_active (GTK_CHECK_BUTTON (check), TRUE);
      gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
      /* The PLAYLIST position, carried on the widget rather than recomputed
       * from the row's place in the list: the list is filtered and truncated,
       * and a position derived from the visible order queues the wrong
       * videos -- a bug whose first symptom is a successful download of
       * something nobody asked for. */
      g_object_set_data (G_OBJECT (check), "ytdl-index",
                         GINT_TO_POINTER (e->index));
      g_signal_connect (check, "toggled", G_CALLBACK (on_entry_toggled), self);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row), check);
      adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), check);

      adw_expander_row_add_row (ADW_EXPANDER_ROW (self->probe_entries), row);
      g_ptr_array_add (self->entry_rows, row);
      g_ptr_array_add (self->entry_checks, check);
    }

  GString *sub = g_string_new (NULL);
  g_string_append_printf (sub, "%u item%s", p->entry_count,
                          p->entry_count == 1 ? "" : "s");
  if (p->playlist_count > 0 && p->playlist_count > p->entry_count)
    g_string_append_printf (sub, " of %d", p->playlist_count);
  if (shown < p->entries->len)
    g_string_append_printf (sub, " · first %u shown", shown);
  if (p->entries_truncated)
    /* Said out loud, because a silently short list of a 4,000-upload channel
     * reads as a complete one. */
    g_string_append (sub, " · the listing was cut short; use the range field "
                          "to reach the rest");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->probe_entries), sub->str);
  g_string_free (sub, TRUE);

  gtk_widget_set_visible (self->probe_entries, TRUE);
  sync_checks_from_items (self);
}

static void
on_thumbnail_loaded (GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr (YtdlDownloadsView) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
      g_file_load_bytes_finish (G_FILE (source), res, NULL, &error);
  if (bytes == NULL)
    return;

  g_autoptr (GdkTexture) tex = gdk_texture_new_from_bytes (bytes, &error);
  if (tex == NULL)
    return;
  gtk_picture_set_paintable (GTK_PICTURE (self->probe_thumb),
                             GDK_PAINTABLE (tex));
  gtk_widget_set_visible (self->probe_thumb, TRUE);
}

/* Fetched through GIO rather than by adding an HTTP library.
 *
 * That is a real trade: g_file_load_bytes on an https:// URI needs a GIO
 * backend for it, which a minimal container or a system without gvfs does not
 * have. The alternative was a fourth dependency in a project whose whole
 * argument is three system libraries and no bundled runtime. So the thumbnail
 * is the one part of the preview allowed to be absent: it fails silently, the
 * title, duration, uploader and format table are all still there, and nothing
 * else in the window changes. Degrading honestly, rather than pretending. */
static void
load_thumbnail (YtdlDownloadsView *self, const char *uri)
{
  gtk_picture_set_paintable (GTK_PICTURE (self->probe_thumb), NULL);
  gtk_widget_set_visible (self->probe_thumb, FALSE);
  if (uri == NULL || !g_str_has_prefix (uri, "http"))
    return;

  g_autoptr (GFile) f = g_file_new_for_uri (uri);
  g_file_load_bytes_async (f, self->probe_cancel, on_thumbnail_loaded,
                           g_object_ref (self));
}

/* Everything the probe knows, written into the form and onto the screen. */
static void
apply_probe (YtdlDownloadsView *self)
{
  const YtdlUrlProbe *p = self->probe;
  g_return_if_fail (p != NULL);

  self->applying_probe = TRUE;

  gboolean codec_kept = rebuild_family_combo (self, self->codec,
                                              p->video_codecs,
                                              video_codec_label, TRUE);
  gboolean audio_kept = rebuild_family_combo (self, self->audio_codec,
                                              p->audio_codecs,
                                              audio_codec_label, TRUE);
  gboolean container_kept = rebuild_family_combo (self, self->container,
                                                  p->containers,
                                                  container_label, FALSE);
  g_autofree char *wanted_height = g_strdup (combo_value (self->quality));
  rebuild_quality_for_codec (self);
  gboolean quality_kept =
      g_strcmp0 (combo_value (self->quality), wanted_height) == 0;

  self->applying_probe = FALSE;

  /* --- The metadata row --- */
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->probe_meta),
                                 p->title != NULL ? p->title : "(no title)");

  GString *meta = g_string_new (NULL);
  if (p->uploader != NULL)
    g_string_append (meta, p->uploader);
  if (p->duration > 0.0)
    {
      g_autofree char *d = human_duration (p->duration);
      g_string_append_printf (meta, "%s%s", meta->len > 0 ? " · " : "", d);
    }
  if (p->view_count > 0)
    {
      g_autofree char *v = human_count (p->view_count);
      g_string_append_printf (meta, "%s%s views", meta->len > 0 ? " · " : "",
                              v);
    }
  if (p->upload_date != NULL)
    {
      g_autofree char *d = human_date (p->upload_date);
      if (*d != '\0')
        g_string_append_printf (meta, "%s%s", meta->len > 0 ? " · " : "", d);
    }
  if (g_strcmp0 (p->kind, "playlist") == 0)
    g_string_append_printf (meta, "%s%d item%s", meta->len > 0 ? " · " : "",
                            p->entry_count, p->entry_count == 1 ? "" : "s");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->probe_meta), meta->str);
  g_string_free (meta, TRUE);

  load_thumbnail (self, p->thumbnail);

  /* --- The note row: everything the user should not have to infer --- */
  GString *note = g_string_new (NULL);
  if (!quality_kept || !codec_kept || !audio_kept || !container_kept)
    g_string_append (
        note,
        "Some of what was selected is not offered for this URL, so those rows "
        "moved to what is. ");
  if (p->from_fallback)
    g_string_append (note,
                     "Read with yt-dlp directly: the installed pipeline "
                     "predates `ytdl --probe`, so the PO token provider was "
                     "not used and the list may be short. ");
  else if (!p->pot_healthy && p->pot_note != NULL)
    g_string_append_printf (note, "%s ", p->pot_note);
  if (p->formats_from_id != NULL)
    /* Named rather than presented as the playlist's own, because a channel
     * can serve 4K AV1 for a recent upload and 360p AVC for one from 2011. */
    g_string_append_printf (note, "Formats shown are for \"%s\". ",
                            p->formats_from_title != NULL
                                ? p->formats_from_title
                                : p->formats_from_id);
  if (p->age_limit > 0)
    g_string_append_printf (note, "Age restricted (%d+). ", p->age_limit);
  if (p->live_status != NULL && g_strcmp0 (p->live_status, "is_live") == 0)
    g_string_append (note, "This is live right now. ");

  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->probe_note), note->str);
  gtk_widget_set_visible (self->probe_note, note->len > 0);
  g_string_free (note, TRUE);

  fill_format_rows (self);
  fill_entry_rows (self);

  gtk_widget_set_visible (self->probe_group, TRUE);
  refresh_preview (self);
}

static void
set_probe_status (YtdlDownloadsView *self, const char *text, gboolean bad)
{
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->probe_status),
                                 text != NULL ? text : "");
  gtk_widget_remove_css_class (self->probe_status, "error");
  if (bad)
    gtk_widget_add_css_class (self->probe_status, "error");
  gtk_widget_set_visible (self->probe_status, text != NULL && *text != '\0');
}

static void
on_probe_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr (YtdlDownloadsView) self = user_data;
  g_autoptr (GError) error = NULL;

  YtdlUrlProbe *p = ytdl_url_probe_run_finish (res, &error);

  gtk_widget_set_visible (self->probe_spinner, FALSE);
  gtk_widget_set_sensitive (self->probe_button, TRUE);
  g_clear_object (&self->probe_cancel);

  if (p == NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          set_probe_status (self, "", FALSE);
          return;
        }
      /* The message is yt-dlp's or ytdl.ps1's own sentence -- "Video
       * unavailable", "Sign in to confirm your age" -- not one invented
       * here, because theirs says what to do about it. */
      set_probe_status (self, error->message, TRUE);
      gtk_widget_set_visible (self->probe_group, TRUE);
      gtk_widget_set_visible (self->probe_meta, FALSE);
      return;
    }

  g_clear_pointer (&self->probe, ytdl_url_probe_free);
  self->probe = p;

  if (p->probe_version > YTDL_SUPPORTED_URL_PROBE_VERSION)
    /* Said, not guessed around. The contract's rule is that fields are added
     * and never redefined, so a newer document is still readable -- but the
     * one thing this app must not do is present a partial reading of it as a
     * complete one. */
    set_probe_status (self,
                      "This pipeline's probe is newer than this app knows "
                      "about; some of what it reported is not shown.", FALSE);
  else
    set_probe_status (self, "", FALSE);

  gtk_widget_set_visible (self->probe_meta, TRUE);
  apply_probe (self);
}

static void
on_probe_clicked (GtkButton *btn, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;

  /* A second click while one is running cancels it rather than starting a
   * race between two answers for the same field. */
  if (self->probe_cancel != NULL)
    {
      g_cancellable_cancel (self->probe_cancel);
      g_clear_object (&self->probe_cancel);
      gtk_widget_set_visible (self->probe_spinner, FALSE);
      gtk_widget_set_sensitive (self->probe_button, TRUE);
      set_probe_status (self, "", FALSE);
      return;
    }

  const char *url = gtk_editable_get_text (GTK_EDITABLE (self->url));
  if (url == NULL || *url == '\0')
    {
      set_probe_status (self, "Paste a URL first.", TRUE);
      gtk_widget_set_visible (self->probe_group, TRUE);
      gtk_widget_set_visible (self->probe_meta, FALSE);
      return;
    }

  g_auto (GStrv) extra = NULL;
  {
    /* The same one-argument-per-line reading collect() does, so a URL that
     * needs --cookies-from-browser is probed with it too -- a preview that
     * fails where the download would succeed is worse than no preview. */
    g_autoptr (YtdlRunOptions) o = collect (self);
    GPtrArray *v = g_ptr_array_new ();
    for (guint i = 0; o->ytdlp_args != NULL && i < o->ytdlp_args->len; i++)
      g_ptr_array_add (v, g_strdup (g_ptr_array_index (o->ytdlp_args, i)));
    g_ptr_array_add (v, NULL);
    extra = (GStrv) g_ptr_array_free (v, FALSE);
  }

  self->probe_cancel = g_cancellable_new ();
  gtk_widget_set_visible (self->probe_group, TRUE);
  gtk_widget_set_visible (self->probe_meta, FALSE);
  gtk_widget_set_visible (self->probe_spinner, TRUE);
  gtk_widget_set_sensitive (self->probe_button, TRUE);
  set_probe_status (self, "Reading the URL…", FALSE);

  /* Cookies and proxy only: `ytdl --probe` refuses the speed limit and the
   * downloader, which govern moving media bytes and a probe moves none. */
  g_autoptr (YtdlRunOptions) conn = ytdl_settings_connection (self->settings);
  g_auto (GStrv) conn_args = ytdl_run_options_connection_args (conn, TRUE);

  ytdl_url_probe_run_async (
      url, gtk_editable_get_text (GTK_EDITABLE (self->items)),
      adw_switch_row_get_active (ADW_SWITCH_ROW (self->no_pot_cb)), 0,
      (const char *const *) extra, (const char *const *) conn_args,
      self->probe_cancel, on_probe_done, g_object_ref (self));
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

/* Defined with the rest of the actions, several hundred lines below, and
 * needed here: the history rows are built up with the queue rows, and the
 * actions that operate on them are grouped together further down. */
static void show_error (YtdlDownloadsView *self, const char *message);

/* Queue the same run again.
 *
 * YtdlRunRecord already carries the whole YtdlRunOptions it was built from, so
 * this is a copy and an enqueue -- no re-derivation from the command string,
 * which would mean parsing back a line this app formatted for a human and
 * getting --ytdlp-arg values wrong the first time one contained a space.
 *
 * The URL is copied with everything else, which is the point: a run that
 * failed at video 40 of 80 is re-run as the same session, and --sync (if it
 * was set) will walk it forward from wherever the archive now is. */
static void
on_run_again (GtkButton *btn, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  const YtdlRunOptions *opts = g_object_get_data (G_OBJECT (btn), "run-opts");
  if (opts == NULL)
    return;

  GError *error = NULL;
  g_autofree char *id = ytdl_runner_enqueue (self->runner, opts, &error);
  if (id == NULL)
    {
      show_error (self, error->message);
      g_clear_error (&error);
    }
}

static GtkWidget *
make_history_row (YtdlDownloadsView *self, const YtdlRunRecord *r)
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

  /* Offered on every finished run, not only on a failed one. "It failed,
   * run it again" is the obvious case, but "that worked, do the next
   * channel the same way" is the commoner one, and a record that carries
   * its whole option set can serve both. Withheld only when there is no
   * option set to re-run, which is a record restored from a store written
   * before those were saved. */
  if (r->opts != NULL)
    {
      GtkWidget *again =
          gtk_button_new_from_icon_name ("view-refresh-symbolic");
      gtk_widget_set_tooltip_text (again, "Queue this run again");
      gtk_widget_set_valign (again, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (again, "flat");
      /* A COPY, owned by the button. The records this row was built from are
       * a snapshot that is freed as soon as the refresh returns, and a
       * pointer into one would be dangling by the time anybody clicked. */
      g_object_set_data_full (G_OBJECT (again), "run-opts",
                              ytdl_run_options_copy (r->opts),
                              (GDestroyNotify) ytdl_run_options_free);
      g_signal_connect (again, "clicked", G_CALLBACK (on_run_again), self);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), again);
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

  clear_list (self->queue_box);
  for (guint i = 0; i < queue->len; i++)
    gtk_list_box_append (GTK_LIST_BOX (self->queue_box),
                         make_queue_row (self, g_ptr_array_index (queue, i)));

  clear_list (self->history_box);
  for (guint i = 0; i < history->len; i++)
    gtk_list_box_append (GTK_LIST_BOX (self->history_box),
                         make_history_row (self,
                                           g_ptr_array_index (history, i)));

  g_autofree char *qt =
      g_strdup_printf ("Queue (%u)", queue->len);
  gtk_label_set_text (GTK_LABEL (self->queue_title), qt);

  /* The queue is sequential by design (see pipeline.h), and it is correct --
   * but thirty runs from a bulk re-fetch executing one at a time look like a
   * stuck app next to a competitor running eight at once, unless something
   * on screen says the wait is deliberate and where the real parallelism
   * lives. Shown only while something is actually waiting, which is the only
   * time the question arises. */
  if (queue->len > 0)
    {
      g_autofree char *note = g_strdup_printf (
          "%u waiting. Runs go one at a time on purpose: two ytdl runs at "
          "once would race on the archive's shared manifests. To download "
          "several videos of one playlist or channel at the same time, raise "
          "Workers before adding it.",
          queue->len);
      gtk_label_set_text (GTK_LABEL (self->queue_note), note);
    }
  gtk_widget_set_visible (self->queue_note, queue->len > 0);
  g_autofree char *ht = g_strdup_printf ("History (%u)", history->len);
  gtk_label_set_text (GTK_LABEL (self->history_title), ht);

  /* Hidden rather than insensitive when there is nothing to clear: the list
   * already says "no runs yet this session", and a greyed-out button beside
   * that sentence is a second, weaker way of saying it. */
  gtk_widget_set_visible (self->clear_history, history->len > 0);

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

  {
    g_autofree char *fps = g_strdup_printf ("%u", o->fps);
    combo_set_value (self->fps, o->fps > 0 ? fps : "0");
  }
  gtk_editable_set_text (GTK_EDITABLE (self->sub_langs),
                         o->sub_langs != NULL ? o->sub_langs : "");
  adw_switch_row_set_active (ADW_SWITCH_ROW (self->chapters_cb),
                             !o->no_chapters);
  /* A profile written before these existed has neither list, which reads as
   * SponsorBlock off -- the behaviour that profile always had. The category
   * field keeps whatever it held, so turning the mode back on does not make
   * somebody retype a list. */
  if (o->sponsorblock_remove != NULL && *o->sponsorblock_remove != '\0')
    {
      combo_set_value (self->sponsor_mode, "remove");
      gtk_editable_set_text (GTK_EDITABLE (self->sponsor_cats),
                             o->sponsorblock_remove);
    }
  else if (o->sponsorblock_mark != NULL && *o->sponsorblock_mark != '\0')
    {
      combo_set_value (self->sponsor_mode, "mark");
      gtk_editable_set_text (GTK_EDITABLE (self->sponsor_cats),
                             o->sponsorblock_mark);
    }
  else
    combo_set_value (self->sponsor_mode, "off");

  /* The destination is part of the profile, but an empty one must not wipe a
   * destination the user has set for this session. */
  if (o->data_root != NULL && *o->data_root != '\0')
    gtk_editable_set_text (GTK_EDITABLE (self->dest), o->data_root);

  /* --items, on the other hand, IS applied even when empty: an empty range
   * means "all of them", which is a real setting rather than an unset one,
   * and leaving the previous URL's selection in place would silently narrow
   * the next run. */
  gtk_editable_set_text (GTK_EDITABLE (self->items),
                         o->items != NULL ? o->items : "");

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
  update_sensitivity (self);
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

/* No confirmation, deliberately. History is a record of what this session
 * did, not an artefact -- the archive itself is untouched, and every run in
 * the list has already written its own line to download.log. The runner
 * emits state-changed, which is what empties the list and hides this button
 * again. */
static void
on_clear_history (GtkButton *btn, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  ytdl_runner_clear_history (self->runner);
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
/* Connection settings                                                    */
/* ---------------------------------------------------------------------- */

static void
update_connection_visibility (YtdlDownloadsView *self)
{
  const char *src = combo_value (self->cookies_source);
  gboolean browser = g_strcmp0 (src, "browser") == 0;
  gtk_widget_set_visible (self->cookies_browser, browser);
  gtk_widget_set_visible (self->cookies_profile, browser);
  gtk_widget_set_visible (self->cookies_file, g_strcmp0 (src, "file") == 0);
}

static void
replace_text (char **slot, const char *value)
{
  g_free (*slot);
  *slot = (value != NULL && *value != '\0') ? g_strdup (value) : NULL;
}

/* Every change is written straight to settings.json and handed straight to the
 * runner. Written per change rather than on exit because nothing else in this
 * app saves settings on the way out, and a proxy that was typed, used for a
 * run, and then forgotten on the next launch would be a setting that does not
 * behave like one. The file is a few hundred bytes. */
static void
on_connection_changed (YtdlDownloadsView *self)
{
  if (self->loading_connection)
    return;
  YtdlSettings *st = self->settings;

  const char *src = combo_value (self->cookies_source);
  replace_text (&st->cookies_source, g_strcmp0 (src, "none") == 0 ? NULL : src);
  replace_text (&st->cookies_browser, combo_value (self->cookies_browser));
  replace_text (&st->cookies_profile,
                gtk_editable_get_text (GTK_EDITABLE (self->cookies_profile)));
  replace_text (&st->cookies_file,
                gtk_editable_get_text (GTK_EDITABLE (self->cookies_file)));
  replace_text (&st->proxy, gtk_editable_get_text (GTK_EDITABLE (self->proxy)));
  replace_text (&st->limit_rate,
                gtk_editable_get_text (GTK_EDITABLE (self->limit_rate)));
  const char *dl = combo_value (self->downloader);
  replace_text (&st->downloader, g_strcmp0 (dl, "native") == 0 ? NULL : dl);

  ytdl_settings_save (st);
  g_autoptr (YtdlRunOptions) conn = ytdl_settings_connection (st);
  ytdl_runner_set_connection (self->runner, conn);

  update_connection_visibility (self);
  refresh_preview (self);
}

static void
on_connection_notify (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  on_connection_changed (user_data);
}

static void
on_connection_edited (GtkEditable *editable, gpointer user_data)
{
  on_connection_changed (user_data);
}

static void
load_connection (YtdlDownloadsView *self)
{
  const YtdlSettings *st = self->settings;
  self->loading_connection = TRUE;
  combo_set_value (self->cookies_source,
                   st->cookies_source != NULL ? st->cookies_source : "none");
  if (st->cookies_browser != NULL)
    combo_set_value (self->cookies_browser, st->cookies_browser);
  gtk_editable_set_text (GTK_EDITABLE (self->cookies_profile),
                         st->cookies_profile != NULL ? st->cookies_profile : "");
  gtk_editable_set_text (GTK_EDITABLE (self->cookies_file),
                         st->cookies_file != NULL ? st->cookies_file : "");
  gtk_editable_set_text (GTK_EDITABLE (self->proxy),
                         st->proxy != NULL ? st->proxy : "");
  gtk_editable_set_text (GTK_EDITABLE (self->limit_rate),
                         st->limit_rate != NULL ? st->limit_rate : "");
  combo_set_value (self->downloader,
                   st->downloader != NULL ? st->downloader : "native");
  self->loading_connection = FALSE;
  update_connection_visibility (self);
}

static void
on_cookie_file_chosen (GObject *source, GAsyncResult *res, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  g_autoptr (GFile) file =
      gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), res, NULL);
  if (file == NULL)
    return; /* dismissed */
  g_autofree char *path = g_file_get_path (file);
  if (path != NULL)
    gtk_editable_set_text (GTK_EDITABLE (self->cookies_file), path);
}

static void
on_cookie_browse (GtkButton *btn, gpointer user_data)
{
  YtdlDownloadsView *self = user_data;
  GtkFileDialog *dlg = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dlg, "Choose a cookies.txt file");
  GtkWidget *root = GTK_WIDGET (gtk_widget_get_root (GTK_WIDGET (self)));
  gtk_file_dialog_open (dlg, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                        NULL, on_cookie_file_chosen, self);
  g_object_unref (dlg);
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

/* @trailing, when given, sits at the right-hand end of the heading row --
 * which is why the heading is a horizontal box rather than a bare label. The
 * label still hexpands, so a section with no trailing widget lays out exactly
 * as it did before. */
static GtkWidget *
section (const char *title, GtkWidget *trailing, GtkWidget **title_label_out)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);

  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *l = gtk_label_new (title);
  gtk_label_set_xalign (GTK_LABEL (l), 0.0f);
  gtk_widget_set_hexpand (l, TRUE);
  gtk_widget_add_css_class (l, "heading");
  gtk_box_append (GTK_BOX (head), l);
  if (trailing != NULL)
    gtk_box_append (GTK_BOX (head), trailing);
  gtk_box_append (GTK_BOX (box), head);

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

  /* Plain arrays of BORROWED widget pointers: the expander owns every row in
   * them, so a free func here would unparent widgets GTK is still holding. */
  self->format_rows = g_ptr_array_new ();
  self->entry_rows = g_ptr_array_new ();
  self->entry_checks = g_ptr_array_new ();
}

static void
ytdl_downloads_view_dispose (GObject *object)
{
  YtdlDownloadsView *self = YTDL_DOWNLOADS_VIEW (object);

  /* A probe in flight holds a reference to this view and will finish on the
   * main loop after dispose. Cancelling first is what stops its callback
   * touching widgets that are on their way out. */
  if (self->probe_cancel != NULL)
    g_cancellable_cancel (self->probe_cancel);
  g_clear_object (&self->probe_cancel);
  g_clear_pointer (&self->probe, ytdl_url_probe_free);
  g_clear_pointer (&self->format_rows, g_ptr_array_unref);
  g_clear_pointer (&self->entry_rows, g_ptr_array_unref);
  g_clear_pointer (&self->entry_checks, g_ptr_array_unref);

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

  /* Icon buttons, not labelled ones. Three labelled buttons in a group header
   * measure about 285px and will not shrink, so at a 360px window they left
   * roughly fifty pixels for the group title and it collapsed to an ellipsis.
   * Icons with tooltips are the GNOME treatment for group-header actions
   * anyway, and they take the squeeze out at every width.
   *
   * No destructive-action class on Delete: a scarlet button in a group header
   * reads as the primary action of the page, and this one deletes a preset,
   * not data. */
  GtkWidget *pbtns = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (pbtns, "linked");

  GtkWidget *psave = gtk_button_new_from_icon_name ("document-save-symbolic");
  gtk_widget_set_tooltip_text (
      psave, "Save these options as a profile — every option except the URL.");
  g_signal_connect (psave, "clicked", G_CALLBACK (on_profile_save), self);
  gtk_box_append (GTK_BOX (pbtns), psave);

  GtkWidget *pren = gtk_button_new_from_icon_name ("document-edit-symbolic");
  gtk_widget_set_tooltip_text (pren, "Rename the selected profile");
  g_signal_connect (pren, "clicked", G_CALLBACK (on_profile_rename), self);
  gtk_box_append (GTK_BOX (pbtns), pren);

  GtkWidget *pdel = gtk_button_new_from_icon_name ("user-trash-symbolic");
  gtk_widget_set_tooltip_text (pdel, "Delete the selected profile");
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
  /* on_url_changed rather than on_form_changed: editing the URL invalidates
   * any preview on screen, and a Quality row still listing the previous
   * video's heights is worse than one listing the generic ladder, because it
   * looks like knowledge. */
  g_signal_connect (self->url, "changed", G_CALLBACK (on_url_changed), self);

  self->probe_button = gtk_button_new_from_icon_name ("edit-find-symbolic");
  gtk_widget_set_tooltip_text (self->probe_button,
                               "Read this URL without downloading it");
  gtk_widget_set_valign (self->probe_button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (self->probe_button, "flat");
  g_signal_connect (self->probe_button, "clicked",
                    G_CALLBACK (on_probe_clicked), self);
  adw_entry_row_add_suffix (ADW_ENTRY_ROW (self->url), self->probe_button);
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

  /* --items, which had no control at all before the preview existed -- it was
   * reachable only by typing yt-dlp's range syntax into the Advanced box as a
   * passthrough. It stays a text field rather than becoming purely a set of
   * tick boxes, for two reasons: the open-ended forms ("200-") cannot be
   * expressed by ticking a list that was truncated, and a range is what a
   * profile stores. Ticking writes here; typing here re-ticks. */
  self->items = adw_entry_row_new ();
  adw_preferences_row_set_title (
      ADW_PREFERENCES_ROW (self->items),
      "Playlist items — empty means all of them (e.g. 1-20, 5,8,10-15)");
  g_signal_connect (self->items, "changed", G_CALLBACK (on_items_changed),
                    self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (dgroup), self->items);
  gtk_box_append (GTK_BOX (form), dgroup);

  /* --- What the URL actually is ------------------------------------- */
  self->probe_group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (self->probe_group),
                                   "Preview");
  adw_preferences_group_set_description (
      ADW_PREFERENCES_GROUP (self->probe_group),
      "Read from the URL without downloading anything. The Quality, codec and "
      "container rows below are rebuilt from what this video actually has.");

  self->probe_spinner = gtk_spinner_new ();
  gtk_spinner_start (GTK_SPINNER (self->probe_spinner));
  gtk_widget_set_valign (self->probe_spinner, GTK_ALIGN_CENTER);
  adw_preferences_group_set_header_suffix (
      ADW_PREFERENCES_GROUP (self->probe_group), self->probe_spinner);

  self->probe_status = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->probe_status), "");
  adw_action_row_set_title_lines (ADW_ACTION_ROW (self->probe_status), 0);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->probe_group),
                             self->probe_status);

  self->probe_meta = adw_action_row_new ();
  adw_action_row_set_title_lines (ADW_ACTION_ROW (self->probe_meta), 0);
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (self->probe_meta), 0);
  self->probe_thumb = gtk_picture_new ();
  /* A fixed box rather than a natural size: thumbnails come back at anything
   * from 120x90 to 1280x720, and a row that changes height depending on which
   * one arrived makes the whole group jump. */
  gtk_widget_set_size_request (self->probe_thumb, 160, 90);
  gtk_picture_set_content_fit (GTK_PICTURE (self->probe_thumb),
                               GTK_CONTENT_FIT_COVER);
  gtk_widget_set_valign (self->probe_thumb, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (self->probe_thumb, "card");
  adw_action_row_add_prefix (ADW_ACTION_ROW (self->probe_meta),
                             self->probe_thumb);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->probe_group),
                             self->probe_meta);

  self->probe_note = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->probe_note),
                                 "Worth knowing");
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (self->probe_note), 0);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->probe_group),
                             self->probe_note);

  self->probe_formats = adw_expander_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->probe_formats),
                                 "Formats");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->probe_group),
                             self->probe_formats);

  self->probe_entries = adw_expander_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->probe_entries),
                                 "Playlist items");
  {
    GtkWidget *btns = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign (btns, GTK_ALIGN_CENTER);

    GtkWidget *all = gtk_button_new_with_label ("All");
    gtk_widget_add_css_class (all, "flat");
    g_object_set_data (G_OBJECT (all), "ytdl-select-all", GINT_TO_POINTER (1));
    g_signal_connect (all, "clicked", G_CALLBACK (on_select_all), self);

    GtkWidget *none = gtk_button_new_with_label ("None");
    gtk_widget_add_css_class (none, "flat");
    g_signal_connect (none, "clicked", G_CALLBACK (on_select_all), self);

    gtk_box_append (GTK_BOX (btns), all);
    gtk_box_append (GTK_BOX (btns), none);
    adw_expander_row_add_suffix (ADW_EXPANDER_ROW (self->probe_entries), btns);
  }
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->probe_group),
                             self->probe_entries);

  /* Hidden until there is something to say. An empty Preview group sitting
   * above the Format group would read as a section that failed to load. */
  gtk_widget_set_visible (self->probe_group, FALSE);
  gtk_box_append (GTK_BOX (form), self->probe_group);

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

  /* Keep the static tables reachable after a probe has replaced the live
   * ones, so clearing the preview can put the full lists back. Stored WITHOUT
   * a destroy notify, deliberately: these point at the static arrays above,
   * and registering g_strfreev against them would hand string literals to
   * free() the first time a probe replaced them. */
#define KEEP_DEFAULTS(row, ids_, labels_)                                     \
  do                                                                          \
    {                                                                         \
      g_object_set_data (G_OBJECT (row), "ytdl-ids-default", (gpointer) ids_); \
      g_object_set_data (G_OBJECT (row), "ytdl-labels-default",               \
                         (gpointer) labels_);                                 \
    }                                                                         \
  while (0)

  KEEP_DEFAULTS (self->quality, q_ids, q_labels);
  KEEP_DEFAULTS (self->codec, c_ids, c_labels);
  KEEP_DEFAULTS (self->audio_codec, a_ids, a_labels);
  KEEP_DEFAULTS (self->container, k_ids, k_labels);
#undef KEEP_DEFAULTS

  GtkWidget *fgroup = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (fgroup), "Format");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (fgroup), self->mode);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (fgroup), self->quality);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (fgroup), self->codec);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (fgroup),
                             self->audio_codec);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (fgroup), self->container);

  /* Frame rate as a CEILING, the same shape as Quality: ytdl --fps is a
   * predicate with a fallback, never a filter that fails a download. Three
   * values, because a ceiling of 60 already admits 50 and one of 30 already
   * admits 25 and 24 -- there is nothing a finer list could express. */
  static const char *const fps_ids[] = { "0", "60", "30", NULL };
  static const char *const fps_labels[] = { "Any", "≤ 60 fps", "≤ 30 fps",
                                            NULL };
  self->fps = make_combo (
      fps_ids, fps_labels, "Frame rate",
      "A ceiling, like Quality. On a 60 fps upload, ≤ 30 can mean 480p.",
      "0");
  g_signal_connect (self->fps, "notify::selected",
                    G_CALLBACK (on_notify_changed), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (fgroup), self->fps);

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
  /* Subtitle languages mean nothing when subtitles are skipped. */
  g_signal_connect (self->no_subs_cb, "notify::active",
                    G_CALLBACK (on_notify_changed), self);

  /* --- Subtitles, chapters, SponsorBlock ----------------------------- */
  GtkWidget *xgroup = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (xgroup),
                                   "Subtitles, chapters and SponsorBlock");

  /* A free-text list rather than a picker, because yt-dlp's own syntax is
   * the thing worth exposing -- regexes ("en.*"), exclusions ("-live_chat")
   * and "all" -- and a picker would need a language list this app cannot
   * know until a probe has run. The Preview group says which languages a
   * probed video actually has. */
  self->sub_langs = adw_entry_row_new ();
  adw_preferences_row_set_title (
      ADW_PREFERENCES_ROW (self->sub_langs),
      "Subtitle languages — empty means English (en.*); e.g. en.*,de,-live_chat");
  g_signal_connect (self->sub_langs, "changed", G_CALLBACK (on_form_changed),
                    self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (xgroup), self->sub_langs);

  self->chapters_cb = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->chapters_cb),
                                 "Embed chapters");
  adw_action_row_set_subtitle (
      ADW_ACTION_ROW (self->chapters_cb),
      "Chapter markers inside the media file. They are kept in the info.json "
      "either way.");
  adw_switch_row_set_active (ADW_SWITCH_ROW (self->chapters_cb), TRUE);
  g_signal_connect (self->chapters_cb, "notify::active",
                    G_CALLBACK (on_notify_changed), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (xgroup), self->chapters_cb);

  static const char *const sb_ids[] = { "off", "mark", "remove", NULL };
  static const char *const sb_labels[] = { "Off", "Mark as chapters",
                                           "Cut out of the file", NULL };
  self->sponsor_mode = make_combo (
      sb_ids, sb_labels, "SponsorBlock",
      "Cutting changes the archived file: it is no longer the one YouTube "
      "served. The uncut streams stay in Pre-merge streams, and the manifest "
      "records the cut.",
      "off");
  g_signal_connect (self->sponsor_mode, "notify::selected",
                    G_CALLBACK (on_notify_changed), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (xgroup),
                             self->sponsor_mode);

  self->sponsor_cats = adw_entry_row_new ();
  adw_preferences_row_set_title (
      ADW_PREFERENCES_ROW (self->sponsor_cats),
      "SponsorBlock categories — e.g. sponsor,selfpromo,intro or all");
  gtk_editable_set_text (GTK_EDITABLE (self->sponsor_cats), "sponsor");
  g_signal_connect (self->sponsor_cats, "changed",
                    G_CALLBACK (on_form_changed), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (xgroup),
                             self->sponsor_cats);
  gtk_box_append (GTK_BOX (form), xgroup);

  /* --- Connection ---------------------------------------------------- */
  GtkWidget *cgroup = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (cgroup),
                                   "Connection");
  adw_preferences_group_set_description (
      ADW_PREFERENCES_GROUP (cgroup),
      "Saved as you change it, and used by every run this app starts — "
      "downloads, previews and re-fetches alike. Not part of a profile.");

  static const char *const cs_ids[] = { "none", "browser", "file", NULL };
  static const char *const cs_labels[] = { "None", "From a browser",
                                           "From a cookies.txt file", NULL };
  self->cookies_source = make_combo (
      cs_ids, cs_labels, "Cookies",
      "For members-only, age-restricted and private videos, and YouTube "
      "Premium's higher bitrate.",
      "none");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (cgroup),
                             self->cookies_source);

  /* No Safari: yt-dlp reads it on macOS only, and this is the Linux app. */
  static const char *const br_ids[] = { "firefox", "chrome", "chromium",
                                        "brave",   "edge",   "opera",
                                        "vivaldi", "whale",  NULL };
  static const char *const br_labels[] = { "Firefox", "Chrome", "Chromium",
                                           "Brave",   "Edge",   "Opera",
                                           "Vivaldi", "Whale",  NULL };
  self->cookies_browser =
      make_combo (br_ids, br_labels, "Browser",
                  "Read while the download runs. Close the browser first if "
                  "it keeps its cookie database locked.",
                  "firefox");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (cgroup),
                             self->cookies_browser);

  self->cookies_profile = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->cookies_profile),
                                 "Browser profile — empty means the default");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (cgroup),
                             self->cookies_profile);

  self->cookies_file = adw_entry_row_new ();
  adw_preferences_row_set_title (
      ADW_PREFERENCES_ROW (self->cookies_file),
      "cookies.txt — read, never written; each run gets a private copy");
  {
    GtkWidget *pick = gtk_button_new_from_icon_name ("document-open-symbolic");
    gtk_widget_set_tooltip_text (pick, "Choose a cookies.txt file");
    gtk_widget_set_valign (pick, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class (pick, "flat");
    g_signal_connect (pick, "clicked", G_CALLBACK (on_cookie_browse), self);
    adw_entry_row_add_suffix (ADW_ENTRY_ROW (self->cookies_file), pick);
  }
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (cgroup),
                             self->cookies_file);

  self->proxy = adw_entry_row_new ();
  adw_preferences_row_set_title (
      ADW_PREFERENCES_ROW (self->proxy),
      "Proxy — e.g. socks5h://127.0.0.1:1080; a password is masked in logs");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (cgroup), self->proxy);

  self->limit_rate = adw_entry_row_new ();
  adw_preferences_row_set_title (
      ADW_PREFERENCES_ROW (self->limit_rate),
      "Speed limit — bytes per second, e.g. 2M; per worker; empty means none");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (cgroup), self->limit_rate);

  static const char *const dl_ids[] = { "native", "aria2c", NULL };
  static const char *const dl_labels[] = { "Built in", "aria2c", NULL };
  self->downloader = make_combo (
      dl_ids, dl_labels, "Downloader",
      "aria2c must be installed, and reports no progress here.",
      "native");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (cgroup), self->downloader);
  gtk_box_append (GTK_BOX (form), cgroup);

  /* Filled BEFORE the change handlers are connected, so loading the saved
   * values does not immediately write them back. */
  load_connection (self);
  g_signal_connect (self->cookies_source, "notify::selected",
                    G_CALLBACK (on_connection_notify), self);
  g_signal_connect (self->cookies_browser, "notify::selected",
                    G_CALLBACK (on_connection_notify), self);
  g_signal_connect (self->downloader, "notify::selected",
                    G_CALLBACK (on_connection_notify), self);
  GtkWidget *const conn_entries[] = { self->cookies_profile, self->cookies_file,
                                      self->proxy, self->limit_rate };
  for (gsize i = 0; i < G_N_ELEMENTS (conn_entries); i++)
    g_signal_connect (conn_entries[i], "changed",
                      G_CALLBACK (on_connection_edited), self);

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

  GtkWidget *qsec = section ("Queue (0)", NULL, &self->queue_title);
  self->queue_note = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->queue_note), 0.0f);
  gtk_label_set_wrap (GTK_LABEL (self->queue_note), TRUE);
  gtk_widget_add_css_class (self->queue_note, "caption");
  gtk_widget_add_css_class (self->queue_note, "dim-label");
  gtk_widget_set_visible (self->queue_note, FALSE);
  gtk_box_append (GTK_BOX (qsec), self->queue_note);
  GtkWidget *qscroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (qscroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (qscroll),
                                 self->queue_box);
  gtk_widget_set_vexpand (qscroll, TRUE);
  gtk_widget_set_size_request (qscroll, -1, 110);
  gtk_box_append (GTK_BOX (qsec), qscroll);

  self->clear_history = gtk_button_new_with_label ("Clear");
  gtk_widget_add_css_class (self->clear_history, "flat");
  gtk_widget_set_valign (self->clear_history, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->clear_history, FALSE);
  gtk_widget_set_tooltip_text (
      self->clear_history,
      "Forgets this session's run records. The archive, download.log and "
      "archive.txt are not touched.");
  g_signal_connect (self->clear_history, "clicked",
                    G_CALLBACK (on_clear_history), self);

  GtkWidget *hsec =
      section ("History (0)", self->clear_history, &self->history_title);
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

  update_sensitivity (self);
  refresh_preview (self);
  on_state_changed (runner, self);

  return GTK_WIDGET (self);
}
