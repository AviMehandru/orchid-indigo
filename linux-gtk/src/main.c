/* ytdl-gtk -- a native GNOME front end for the yt-dlp archival pipeline.
 *
 * WHAT THIS IS NOT: a reimplementation of the pipeline. Downloads are started
 * by handing a command line to the installed ytdl.ps1, exactly as a terminal
 * would. Nothing here knows what run_ytdlp.ps1 or postprocess.ps1 do with it.
 *
 * WHAT THIS DOES OWN: reading the archive. That is a third independent
 * implementation of docs/archive-layout.md, and the price of three standalone
 * apps sharing no engine. tests/test_archive.c is what keeps it honest.
 *
 * No Rust, no webview, no bundled runtime -- GTK4, libadwaita, GLib and
 * json-glib, all of which a GNOME desktop already has.
 *
 * THE SHELL IS LIBADWAITA'S, NOT HAND-BUILT. AdwNavigationView owns
 * library-to-detail, which is where the back button, the gesture, the
 * animation and the Escape key all come from; AdwViewStack plus
 * AdwViewSwitcher own the four top-level pages; AdwToolbarView owns the
 * header, the search bar and the status line and the way they behave as the
 * content scrolls under them. An AdwBreakpoint moves the switcher to the
 * bottom on a narrow window. None of that is code here.
 */

#include <adwaita.h>

#include "archive.h"
#include "detail_view.h"
#include "downloads_view.h"
#include "health.h"
#include "health_view.h"
#include "library_filter.h"
#include "library_view.h"
#include "notify.h"
#include "paths.h"
#include "pipeline.h"
#include "profiles.h"
#include "search_index.h"
#include "userdata.h"
#include "settings.h"
#include "style.h"
#include "subscriptions_view.h"
#include "verify_cache.h"

typedef struct
{
  GtkWidget *window;
  GtkWidget *library;
  GtkWidget *health;
  GtkWidget *subscriptions;
  GtkWidget *detail;

  GtkWidget *header;      /* AdwHeaderBar of the main page */
  GtkWidget *switcher_bar;/* AdwViewSwitcherBar, revealed when narrow */
  GtkWidget *nav;         /* AdwNavigationView: main <-> detail */
  GtkWidget *detail_page; /* AdwNavigationPage wrapping the detail view */
  GtkWidget *stack;       /* AdwViewStack: library / downloads /
                           subscriptions / health */
  GtkWidget *toasts;      /* AdwToastOverlay */

  YtdlSettings *settings;
  YtdlRunner   *runner;
  /* What the queue has done that has not been announced yet. Owned. Fed on
   * every runner state change whether or not anything will be sent, so a
   * summary sent later still covers the whole queue. See notify.h. */
  YtdlNoticeTracker *notices;
  GtkApplication    *gtkapp; /* borrowed; for sending notifications */
  GtkWidget *search_bar;
  GtkWidget *search;
  GtkWidget *search_button;
  GtkWidget *rescan;
  GtkWidget *status;
  GtkWidget *spinner;

  /* Sort and facets. The filter itself lives in the library view; these are
   * only the controls that drive it. */
  YtdlVerifyCache *verify; /* owned; the library view and detail page borrow */
  GtkWidget *filter_button;
  GtkWidget *filter_badge;
  GtkWidget *sort_drop;
  GtkWidget *sort_dir;
  GtkWidget *channel_box;   /* the check buttons, rebuilt on every scan */
  GtkWidget *channel_frame; /* hidden entirely when there is one channel */
  GtkWidget *date_from;
  GtkWidget *date_to;
  GtkWidget *flag_audio;
  GtkWidget *flag_no_media;
  GtkWidget *flag_layout;
  GtkWidget *flag_verify;
  GtkWidget *flag_unwatched;
  GtkWidget *verify_note;
  GtkWidget *watched_note;

  /* Watch state, resume points and playlists: the one store here whose
   * contents came from the person rather than from the pipeline. Owned; the
   * library filter borrows its watched set and the detail page borrows the
   * whole thing. */
  YtdlUserData *userdata;
  GtkWidget    *playlist_frame;
  GtkWidget    *playlist_drop;
  GtkWidget    *playlist_menu;   /* on the detail page's header bar */
  /* The selected playlist's keys, handed to the filter. Owned and rebuilt on
   * every change rather than borrowed from the playlist, because the filter
   * wants a set and a playlist is an ordered array. */
  GHashTable   *playlist_keys;
  /* Ids, parallel to the dropdown's rows; index 0 is the "All videos" row and
   * is NULL. Owned. */
  GPtrArray    *playlist_ids;
  /* The key of the video the detail page is showing, so the playlist menu on
   * its header bar knows what it is adding. */
  char         *detail_key;

  /* Multi-select and the bulk bar.
   *
   * A MODE rather than "ctrl-click always multi-selects": the primary gesture
   * on a card is "open this", and a grid where a stray click adds to a hidden
   * selection does the wrong thing quietly. */
  GtkWidget *select_toggle;
  GtkWidget *bulk_bar;
  GtkWidget *bulk_count;
  GtkWidget *bulk_select_all;
  GtkWidget *bulk_watched;
  GtkWidget *bulk_unwatched;
  GtkWidget *bulk_playlist;
  GtkWidget *bulk_refetch;
  GtkWidget *bulk_verify;
  GtkWidget *bulk_copy;

  /* A bulk verify is the one bulk action that takes real time -- seconds per
   * video, because it hashes every file in the folder. Written by the worker,
   * read by a main-thread idle, atomic for the same reason the scan counters
   * are. */
  gboolean bulk_verifying;
  gint     bulk_verify_done;
  gint     bulk_verify_total;

  /* Collection-wide comment and transcript search. */
  YtdlSearchIndex *search_index; /* owned */
  GtkWidget       *scope_drop;
  GtkWidget       *index_banner;
  GHashTable      *search_hits; /* owned; borrowed by the filter */
  GCancellable    *index_cancel;
  gboolean         indexing;
  gint             index_done;
  gint             index_total;
  guint            index_tick;
  /* Set while the popover is being repopulated from the index, so the
   * "toggled" handlers do not each kick off a rebuild against a
   * half-rebuilt set of controls. */
  gboolean   populating;

  YtdlIndex *index; /* owned; the library view borrows from it */
  char      *archive_root;

  /* Written by the scan thread, read by a main-thread timeout. Atomic rather
   * than mutexed because they are two counters and a tick that reads a stale
   * value simply redraws 100ms later. */
  gint     scan_done;
  gint     scan_total;
  guint    tick_id;
  gboolean scanning;
} App;

/* Both defined below, beside the rest of the facet code, and both needed by
 * on_scan_finished, which has to sit up here with the other scan machinery. */
static void populate_facets (App *app);
static void apply_filter (App *app);
static void run_search (App *app);
static void update_search_banner (App *app);
static void populate_playlists (App *app);
static void rebuild_playlist_menu (App *app);
/* The bulk bar's own refresh, needed by the playlist handlers above it: a new
 * playlist has to appear in the bar's Add-to menu at the moment it is made. */
static void update_bulk_bar (App *app);

/* A scan produces a whole new index; it is swapped in on the main thread so
 * the view is never looking at a half-built one. */
typedef struct
{
  App       *app;
  YtdlIndex *index;
  char      *error;
} ScanResult;

static void
set_status (App *app, const char *text)
{
  gtk_label_set_text (GTK_LABEL (app->status), text);
}

/* Errors go to a toast as well as the status line. The status line is a
 * quiet, permanent readout that people stop seeing; a scan that found no
 * archive at all needs to interrupt once. */
static void
toast (App *app, const char *text)
{
  AdwToast *t = adw_toast_new (text);
  adw_toast_set_timeout (t, 6);
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (app->toasts), t);
}

static void
update_counts (App *app)
{
  if (app->index == NULL)
    {
      set_status (app, "No archive loaded.");
      return;
    }

  gsize videos = 0, channels = 0;
  guint64 bytes = 0;
  ytdl_index_stats (app->index, &videos, &channels, &bytes);

  g_autofree char *size = g_format_size (bytes);
  guint shown = ytdl_library_view_get_shown (YTDL_LIBRARY_VIEW (app->library));

  if (shown != videos)
    {
      g_autofree char *msg = g_strdup_printf (
          "%u of %" G_GSIZE_FORMAT " videos · %" G_GSIZE_FORMAT
          " channels · %s",
          shown, videos, channels, size);
      set_status (app, msg);
    }
  else
    {
      g_autofree char *msg = g_strdup_printf (
          "%" G_GSIZE_FORMAT " videos · %" G_GSIZE_FORMAT " channels · %s",
          videos, channels, size);
      set_status (app, msg);
    }
}

/* ---------------------------------------------------------------------- */
/* Scanning                                                               */
/* ---------------------------------------------------------------------- */

/* Runs on the SCAN THREAD. Touches nothing but two atomics -- calling into
 * GTK from here would be the classic way to make this app crash under load. */
static void
on_scan_progress (gsize done, gsize total, const char *name,
                  gpointer user_data)
{
  App *app = user_data;
  g_atomic_int_set (&app->scan_done, (gint) done);
  g_atomic_int_set (&app->scan_total, (gint) total);
}

static gboolean
on_tick (gpointer user_data)
{
  App *app = user_data;
  if (!app->scanning)
    {
      app->tick_id = 0;
      return G_SOURCE_REMOVE;
    }

  gint done = g_atomic_int_get (&app->scan_done);
  gint total = g_atomic_int_get (&app->scan_total);
  g_autofree char *msg =
      total > 0 ? g_strdup_printf ("Scanning… %d of %d", done, total)
                : g_strdup ("Scanning…");
  set_status (app, msg);
  return G_SOURCE_CONTINUE;
}

/* Main thread: adopt the finished index. */
static gboolean
on_scan_finished (gpointer user_data)
{
  ScanResult *res = user_data;
  App *app = res->app;

  app->scanning = FALSE;
  gtk_widget_set_sensitive (app->rescan, TRUE);
  gtk_spinner_stop (GTK_SPINNER (app->spinner));
  gtk_widget_set_visible (app->spinner, FALSE);

  if (res->error != NULL)
    {
      /* Named in full rather than reduced to "scan failed". An empty library
       * with no explanation is the exact outcome the layout contract exists
       * to prevent, and the same rule applies to not finding one at all. */
      set_status (app, res->error);
      toast (app, res->error);
      ytdl_library_view_set_index (YTDL_LIBRARY_VIEW (app->library), NULL);
      if (app->health != NULL)
        ytdl_health_view_set_index (YTDL_HEALTH_VIEW (app->health), NULL);
      g_clear_pointer (&res->index, ytdl_index_free);
    }
  else
    {
      /* Clear the view's borrowed pointers BEFORE freeing the old index. */
      ytdl_library_view_set_index (YTDL_LIBRARY_VIEW (app->library), NULL);
      /* The detail page borrows nothing from the index once loaded -- it
       * copied what it needed -- but it may still be showing a video that no
       * longer exists, so a rescan returns to the grid. Popping also stops
       * playback, through the "popped" handler. */
      adw_navigation_view_pop_to_tag (ADW_NAVIGATION_VIEW (app->nav), "main");
      g_clear_pointer (&app->index, ytdl_index_free);
      app->index = g_steal_pointer (&res->index);
      ytdl_library_view_set_index (YTDL_LIBRARY_VIEW (app->library),
                                   app->index);
      if (app->health != NULL)
        ytdl_health_view_set_index (YTDL_HEALTH_VIEW (app->health), app->index);
      /* The channel facet is the ARCHIVE's channel list, so it is rebuilt
       * from each new index. A channel that has gone away also goes out of
       * the selection, which populate_facets does by construction: it only
       * ever creates checks for channels the index still has. */
      populate_facets (app);
      apply_filter (app);
      update_counts (app);
    }

  g_free (res->error);
  g_free (res);
  return G_SOURCE_REMOVE;
}

static gpointer
scan_thread (gpointer user_data)
{
  App *app = user_data;

  ScanResult *res = g_new0 (ScanResult, 1);
  res->app = app;
  res->index = ytdl_index_new ();

  GError *error = NULL;
  if (!ytdl_index_scan (res->index, app->archive_root, on_scan_progress, app,
                        &error))
    {
      res->error = g_strdup (error->message);
      g_clear_error (&error);
    }

  g_idle_add (on_scan_finished, res);
  return NULL;
}

static void
start_scan (App *app)
{
  if (app->scanning)
    return;

  if (app->archive_root == NULL)
    {
      const char *msg =
          "Could not find an archive. Looked for 'Youtube Videos/Complete "
          "Archive' under the usual locations. Choose a folder on the Health "
          "pane, pass --archive-root, or set YTDLP_INSTALL_ROOT.";
      set_status (app, msg);
      toast (app, "No archive found. Choose a folder on the Health pane.");
      return;
    }

  app->scanning = TRUE;
  g_atomic_int_set (&app->scan_done, 0);
  g_atomic_int_set (&app->scan_total, 0);
  gtk_widget_set_sensitive (app->rescan, FALSE);
  gtk_widget_set_visible (app->spinner, TRUE);
  gtk_spinner_start (GTK_SPINNER (app->spinner));
  set_status (app, "Scanning…");

  if (app->tick_id == 0)
    app->tick_id = g_timeout_add (100, on_tick, app);

  GThread *t = g_thread_new ("ytdl-scan", scan_thread, app);
  g_thread_unref (t);
}

/* The Health pane picked a folder.
 *
 * A pick that does NOT hold an archive is refused and nothing is stored --
 * the opposite of the destination folder on the Downloads pane, which is a
 * place to write and so cannot be wrong yet. This one names an existing tree,
 * so "there is no Complete Archive under here" is knowable now, and storing
 * it would turn one mistyped pick into a setting that quietly loses to
 * autodetection on every later launch with nothing on screen explaining why.
 *
 * A scan already running is left alone rather than cancelled: it is about to
 * swap in an index built for the OLD root, and on_scan_finished has no way to
 * tell that it is stale. The rescan button is the recovery, and the status
 * line already says a scan is in flight, so this is a visible wait rather
 * than a silent one. */
static void
on_archive_root_chosen (YtdlHealthView *view, const char *path, gpointer data)
{
  App *app = data;

  g_autofree char *resolved = ytdl_resolve_archive_root (path);
  if (resolved == NULL)
    {
      toast (app, "No 'Complete Archive' under that folder.");
      return;
    }

  g_free (app->settings->archive_root);
  app->settings->archive_root = g_strdup (path);
  ytdl_settings_save (app->settings);

  g_free (app->archive_root);
  app->archive_root = g_steal_pointer (&resolved);

  ytdl_health_view_set_archive_root (YTDL_HEALTH_VIEW (app->health),
                                     app->archive_root);

  if (app->scanning)
    {
      toast (app, "Archive root saved. Rescan when this scan finishes.");
      return;
    }

  start_scan (app);
}

/* ---------------------------------------------------------------------- */
/* Navigation                                                             */
/* ---------------------------------------------------------------------- */

/* Fires for every way out of the detail page: the back button, the Escape
 * key, the back mouse button, the edge-swipe gesture, and pop_to_tag above.
 * Hanging the teardown on the signal rather than on the back button is the
 * whole reason the navigation view is worth using -- a GtkVideo left holding
 * a file keeps its GStreamer pipeline alive, and audio continuing after Back
 * is the kind of thing people remember about an application. */
static void
on_popped (AdwNavigationView *nav, AdwNavigationPage *page, gpointer user_data)
{
  App *app = user_data;
  ytdl_detail_view_clear (YTDL_DETAIL_VIEW (app->detail));
}

/* The grid hands over the opaque KEY, not the entry, so the lookup happens
 * against whatever index is current -- a rescan that finished between the
 * click and this handler cannot leave a dangling pointer. */
static void
on_video_activated (GtkWidget *view, const char *key, gpointer user_data)
{
  App *app = user_data;
  if (app->index == NULL)
    return;

  const YtdlEntry *entry = ytdl_index_get (app->index, key);
  if (entry == NULL)
    return;

  ytdl_detail_view_show (YTDL_DETAIL_VIEW (app->detail), entry);

  /* The playlist menu lives on the detail page's header bar rather than
   * inside the detail view, because membership is the application's fact --
   * the same store the Library filters on -- and threading it through the
   * view would be a second owner of it. */
  g_free (app->detail_key);
  app->detail_key = g_strdup (entry->key);
  rebuild_playlist_menu (app);

  adw_navigation_page_set_title (
      ADW_NAVIGATION_PAGE (app->detail_page),
      entry->title != NULL && *entry->title != '\0' ? entry->title : "Video");
  adw_navigation_view_push_by_tag (ADW_NAVIGATION_VIEW (app->nav), "detail");
}

/* Search filters the library and nothing else, so the button that reveals it
 * is only offered on the library page. Leaving it enabled everywhere would
 * put a search entry over the Downloads form that silently does nothing. */
static void
on_page_changed (GObject *stack, GParamSpec *pspec, gpointer user_data)
{
  App *app = user_data;
  const char *name =
      adw_view_stack_get_visible_child_name (ADW_VIEW_STACK (stack));
  gboolean on_library = g_strcmp0 (name, "library") == 0;

  gtk_widget_set_visible (app->search_button, on_library);
  gtk_widget_set_visible (app->filter_button, on_library);
  if (!on_library)
    gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (app->search_bar), FALSE);

  /* Read the list every time the pane is shown: the schedule runs whether or
   * not this app is open, and this is the only way to see what it did. */
  if (g_strcmp0 (name, "subscriptions") == 0 && app->subscriptions != NULL)
    ytdl_subscriptions_view_refresh (
        YTDL_SUBSCRIPTIONS_VIEW (app->subscriptions));
}

static void
on_subscriptions_message (GtkWidget *view, const char *text, gpointer user_data)
{
  toast (user_data, text);
}

static void
on_subscribed (GtkWidget *view, const char *text, gpointer user_data)
{
  App *app = user_data;
  toast (app, text);
  ytdl_subscriptions_view_refresh (YTDL_SUBSCRIPTIONS_VIEW (app->subscriptions));
}

static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  App *app = user_data;
  run_search (app);
}

static void
on_rescan_clicked (GtkButton *button, gpointer user_data)
{
  start_scan (user_data);
}

/* A re-fetch button on a video's page.
 *
 * This is where the detail page's "what the user asked for" becomes a queued
 * run. The options are built here rather than there because building them
 * needs pipeline.h, and the detail page deliberately does not include it.
 *
 * Only the URL, the mode and --refresh are set. Nothing is carried over from
 * whatever is typed on the Downloads form: a refresh is a re-fetch of one
 * component into a folder that already exists, and inheriting a destination
 * or a quality cap from an unrelated form would be a way to write it somewhere
 * else entirely. The data root is the exception and is deliberately NOT set
 * either -- the pipeline's own default resolves to the same archive this
 * video was read from, and a stored override that pointed elsewhere is
 * exactly the "library and downloads pointing at different folders" bug the
 * Health pane exists to catch. */
static void
on_refetch_requested (GtkWidget *view, const char *url, const char *mode,
                      gpointer user_data)
{
  App *app = user_data;
  if (url == NULL || *url == '\0' || mode == NULL)
    return;

  g_autoptr (YtdlRunOptions) o = ytdl_run_options_new ();
  o->url = g_strdup (url);
  o->mode = g_strdup (mode);
  o->refresh = TRUE;

  GError *error = NULL;
  g_autofree char *id = ytdl_runner_enqueue (app->runner, o, &error);
  if (id == NULL)
    {
      toast (app, error != NULL ? error->message : "Could not queue the run.");
      g_clear_error (&error);
      return;
    }

  /* Back to the grid and over to Downloads, because a run that has been
   * queued and cannot be seen is indistinguishable from a button that did
   * nothing. The queue is strictly sequential, so this may sit behind
   * something already running -- which is exactly why it has to be visible. */
  adw_navigation_view_pop_to_tag (ADW_NAVIGATION_VIEW (app->nav), "main");
  adw_view_stack_set_visible_child_name (ADW_VIEW_STACK (app->stack),
                                         "downloads");

  g_autofree char *msg =
      g_strdup_printf ("Queued a %s refresh.", mode);
  toast (app, msg);
}

/* ---------------------------------------------------------------------- */
/* Sort and facets                                                        */
/* ---------------------------------------------------------------------- */

/* Everything a facet control does ends here: re-apply the filter, redraw the
 * counts, and update the badge on the filter button. Routed through one
 * function rather than each handler doing its own three things, because the
 * badge being right is the only signal that a facet is still narrowing the
 * library after the popover has been closed and forgotten. */
static void
apply_filter (App *app)
{
  if (app->populating)
    return;

  ytdl_library_view_refilter (YTDL_LIBRARY_VIEW (app->library));
  update_counts (app);

  YtdlLibraryFilter *f =
      ytdl_library_view_get_filter (YTDL_LIBRARY_VIEW (app->library));
  guint n = ytdl_library_filter_facet_count (f);
  if (n > 0)
    {
      g_autofree char *text = g_strdup_printf ("%u", n);
      gtk_label_set_text (GTK_LABEL (app->filter_badge), text);
      gtk_widget_set_visible (app->filter_badge, TRUE);
    }
  else
    {
      gtk_widget_set_visible (app->filter_badge, FALSE);
    }
}

static void
on_sort_changed (GObject *drop, GParamSpec *pspec, gpointer user_data)
{
  App *app = user_data;
  /* Guarded here as well as in apply_filter, because this handler also
   * WRITES settings.json -- and gtk_drop_down_set_selected during a
   * repopulate fires it. Persisting a value that was just read back from
   * the same file is harmless; doing it once per facet rebuild is noise in
   * a file the user can open. */
  if (app->populating)
    return;

  YtdlLibraryFilter *f =
      ytdl_library_view_get_filter (YTDL_LIBRARY_VIEW (app->library));

  guint selected = gtk_drop_down_get_selected (GTK_DROP_DOWN (drop));
  if (selected == GTK_INVALID_LIST_POSITION)
    return;
  f->sort = (YtdlSortKey) selected;

  /* Persisted by ID, never by the enum's number -- inserting a key in the
   * middle would otherwise silently change what every saved setting means. */
  g_free (app->settings->sort_key);
  app->settings->sort_key = g_strdup (ytdl_sort_key_id (f->sort));
  ytdl_settings_save (app->settings);

  apply_filter (app);
}

static void
on_sort_direction (GtkButton *button, gpointer user_data)
{
  App *app = user_data;
  YtdlLibraryFilter *f =
      ytdl_library_view_get_filter (YTDL_LIBRARY_VIEW (app->library));

  f->descending = !f->descending;
  gtk_button_set_icon_name (button, f->descending ? "go-down-symbolic"
                                                  : "go-up-symbolic");
  gtk_widget_set_tooltip_text (GTK_WIDGET (button),
                               f->descending ? "Descending" : "Ascending");

  app->settings->sort_descending = f->descending;
  ytdl_settings_save (app->settings);

  apply_filter (app);
}

static void
on_channel_toggled (GtkCheckButton *check, gpointer user_data)
{
  App *app = user_data;
  YtdlLibraryFilter *f =
      ytdl_library_view_get_filter (YTDL_LIBRARY_VIEW (app->library));
  const char *channel = g_object_get_data (G_OBJECT (check), "channel");
  if (channel == NULL)
    return;

  ytdl_library_filter_set_channel (f, channel,
                                   gtk_check_button_get_active (check));
  apply_filter (app);
}

/* Accepts what people actually type. "2024-01-31" and "31/01/2024" are both
 * reasonable things to enter into a box labelled with a date, and the filter
 * wants the archive's own YYYYMMDD. Anything that is not eight digits after
 * the separators come out is treated as "no bound" rather than as an error:
 * this fires on every keystroke, so a half-typed date must narrow nothing
 * rather than flash a validation message four times per second. */
static char *
normalize_date (const char *text)
{
  if (text == NULL)
    return NULL;

  g_autoptr (GString) digits = g_string_new (NULL);
  for (const char *p = text; *p != '\0'; p++)
    if (g_ascii_isdigit (*p))
      g_string_append_c (digits, *p);

  if (digits->len != 8)
    return NULL;
  return g_strdup (digits->str);
}

static void
on_date_changed (GtkEditable *entry, gpointer user_data)
{
  App *app = user_data;
  YtdlLibraryFilter *f =
      ytdl_library_view_get_filter (YTDL_LIBRARY_VIEW (app->library));

  g_free (f->date_from);
  g_free (f->date_to);
  f->date_from = normalize_date (gtk_editable_get_text (
      GTK_EDITABLE (app->date_from)));
  f->date_to = normalize_date (gtk_editable_get_text (
      GTK_EDITABLE (app->date_to)));

  /* A bound that was typed but does not parse gets the entry marked rather
   * than silently ignored -- otherwise "2024" in the From box looks like it
   * is filtering and is not. */
  const char *raw_from = gtk_editable_get_text (GTK_EDITABLE (app->date_from));
  const char *raw_to = gtk_editable_get_text (GTK_EDITABLE (app->date_to));
  if (*raw_from != '\0' && f->date_from == NULL)
    gtk_widget_add_css_class (app->date_from, "error");
  else
    gtk_widget_remove_css_class (app->date_from, "error");
  if (*raw_to != '\0' && f->date_to == NULL)
    gtk_widget_add_css_class (app->date_to, "error");
  else
    gtk_widget_remove_css_class (app->date_to, "error");

  apply_filter (app);
}

static void
on_flag_toggled (GtkCheckButton *check, gpointer user_data)
{
  App *app = user_data;
  YtdlLibraryFilter *f =
      ytdl_library_view_get_filter (YTDL_LIBRARY_VIEW (app->library));

  YtdlFacetFlags bit = (YtdlFacetFlags) GPOINTER_TO_UINT (
      g_object_get_data (G_OBJECT (check), "flag"));
  if (gtk_check_button_get_active (check))
    f->flags |= bit;
  else
    f->flags &= ~bit;

  apply_filter (app);
}

/* The detail page marked something watched, or a resume point crossed the
 * threshold and marked it for itself.
 *
 * The Library has to hear about it while the user is still there: with the
 * "unwatched" facet on, finishing a video means it should leave the grid, and
 * a grid that only caught up on the next rescan would look broken. The filter
 * already holds the store's live set, so there is nothing to re-hand -- only
 * a refilter and the note that counts what is marked. */
static void
on_watch_state_changed (GtkWidget *detail, const char *key, gpointer user_data)
{
  App *app = user_data;
  apply_filter (app);
  populate_facets (app);
}

/* ---------------------------------------------------------------------- */
/* Playlists                                                              */
/* ---------------------------------------------------------------------- */

/* Turn the selected playlist into the set the filter wants.
 *
 * Rebuilt rather than held as a pointer into the playlist, because a playlist
 * is an ordered GPtrArray and the filter does membership tests on every
 * keystroke. Copying a few hundred string pointers once per selection change
 * is the cheaper side of that trade by a wide margin. */
static void
apply_playlist_selection (App *app)
{
  YtdlLibraryFilter *f =
      ytdl_library_view_get_filter (YTDL_LIBRARY_VIEW (app->library));

  g_clear_pointer (&app->playlist_keys, g_hash_table_unref);
  f->playlist_keys = NULL;

  guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (app->playlist_drop));
  if (sel == GTK_INVALID_LIST_POSITION || sel == 0
      || app->playlist_ids == NULL || sel >= app->playlist_ids->len)
    return;

  const char *id = g_ptr_array_index (app->playlist_ids, sel);
  YtdlPlaylist *pl = ytdl_user_data_playlist (app->userdata, id);
  if (pl == NULL)
    return;

  /* An EMPTY playlist still produces an empty set rather than NULL. "This
   * playlist has nothing in it" must show nothing, not everything -- NULL
   * here means "no playlist filter", and the two are opposite answers. */
  app->playlist_keys = g_hash_table_new (g_str_hash, g_str_equal);
  for (guint i = 0; i < pl->keys->len; i++)
    g_hash_table_add (app->playlist_keys, g_ptr_array_index (pl->keys, i));

  f->playlist_keys = app->playlist_keys;
}

static void
on_playlist_changed (GObject *drop, GParamSpec *pspec, gpointer user_data)
{
  App *app = user_data;
  if (app->populating)
    return;
  apply_playlist_selection (app);
  apply_filter (app);
}

/* The dropdown, from the store. Index 0 is always "All videos". */
static void
populate_playlists (App *app)
{
  if (app->playlist_drop == NULL)
    return;

  gboolean was_populating = app->populating;
  app->populating = TRUE;

  /* Remember the selection by ID, not by row: creating or deleting a
   * playlist renumbers the rows, and restoring a row index would silently
   * move the user to a different playlist. */
  g_autofree char *selected = NULL;
  guint prev = gtk_drop_down_get_selected (GTK_DROP_DOWN (app->playlist_drop));
  if (app->playlist_ids != NULL && prev != GTK_INVALID_LIST_POSITION
      && prev > 0 && prev < app->playlist_ids->len)
    selected = g_strdup (g_ptr_array_index (app->playlist_ids, prev));

  GPtrArray *playlists = ytdl_user_data_playlists (app->userdata);

  g_clear_pointer (&app->playlist_ids, g_ptr_array_unref);
  app->playlist_ids = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (app->playlist_ids, NULL); /* the "All videos" row */

  GtkStringList *names = gtk_string_list_new (NULL);
  gtk_string_list_append (names, "All videos");

  guint restore = 0;
  for (guint i = 0; i < playlists->len; i++)
    {
      const YtdlPlaylist *pl = g_ptr_array_index (playlists, i);
      gtk_string_list_append (names, pl->name);
      g_ptr_array_add (app->playlist_ids, g_strdup (pl->id));
      if (selected != NULL && g_strcmp0 (selected, pl->id) == 0)
        restore = i + 1;
    }

  gtk_drop_down_set_model (GTK_DROP_DOWN (app->playlist_drop),
                           G_LIST_MODEL (names));
  g_object_unref (names);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (app->playlist_drop), restore);

  /* No playlists is not a choice, exactly like one channel. The section
   * appears the moment there is one to pick. */
  gtk_widget_set_visible (app->playlist_frame, playlists->len > 0);

  app->populating = was_populating;
  apply_playlist_selection (app);
}

static void
on_playlist_toggle_membership (GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
  App *app = user_data;
  const char *id = g_variant_get_string (param, NULL);
  if (app->detail_key == NULL || app->userdata == NULL)
    return;

  if (ytdl_user_data_playlist_contains (app->userdata, id, app->detail_key))
    ytdl_user_data_playlist_remove (app->userdata, id, app->detail_key);
  else
    ytdl_user_data_playlist_add (app->userdata, id, app->detail_key);

  ytdl_user_data_save (app->userdata);
  rebuild_playlist_menu (app);
  /* The Library may be filtered to the very playlist just changed. */
  apply_playlist_selection (app);
  apply_filter (app);
}

static void
on_new_playlist_response (AdwAlertDialog *dialog, const char *response,
                          gpointer user_data)
{
  App *app = user_data;
  if (g_strcmp0 (response, "create") != 0)
    return;

  GtkWidget *entry = g_object_get_data (G_OBJECT (dialog), "entry");
  const char *name = gtk_editable_get_text (GTK_EDITABLE (entry));

  YtdlPlaylist *pl = ytdl_user_data_playlist_create (app->userdata, name);
  if (pl == NULL)
    return; /* blank name; userdata.c refuses it and so does this */

  if (app->detail_key != NULL)
    ytdl_user_data_playlist_add (app->userdata, pl->id, app->detail_key);

  ytdl_user_data_save (app->userdata);
  populate_playlists (app);
  rebuild_playlist_menu (app);
  update_bulk_bar (app);
  apply_filter (app);
}

static void
on_new_playlist (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  App *app = user_data;

  AdwAlertDialog *dialog = ADW_ALERT_DIALOG (
      adw_alert_dialog_new ("New playlist", NULL));
  adw_alert_dialog_add_responses (dialog, "cancel", "Cancel", "create",
                                  "Create", NULL);
  adw_alert_dialog_set_response_appearance (dialog, "create",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (dialog, "create");
  adw_alert_dialog_set_close_response (dialog, "cancel");

  GtkWidget *entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (entry), "Name");
  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
  adw_alert_dialog_set_extra_child (dialog, entry);
  g_object_set_data (G_OBJECT (dialog), "entry", entry);

  g_signal_connect (dialog, "response",
                    G_CALLBACK (on_new_playlist_response), app);
  adw_dialog_present (ADW_DIALOG (dialog), app->window);
}

/* The menu on the detail page's header bar: one check item per playlist plus
 * "New playlist…".
 *
 * Rebuilt rather than kept, because the item for each playlist has to show
 * whether THIS video is in it, and that changes on every page. */
static void
rebuild_playlist_menu (App *app)
{
  if (app->playlist_menu == NULL)
    return;

  gboolean usable = app->userdata != NULL && app->detail_key != NULL
                    && !ytdl_user_data_is_read_only (app->userdata);
  gtk_widget_set_sensitive (app->playlist_menu, usable);

  GMenu *menu = g_menu_new ();
  GPtrArray *playlists = ytdl_user_data_playlists (app->userdata);

  GMenu *section = g_menu_new ();
  for (guint i = 0; i < playlists->len; i++)
    {
      const YtdlPlaylist *pl = g_ptr_array_index (playlists, i);
      gboolean in = app->detail_key != NULL
                    && ytdl_user_data_playlist_contains (app->userdata, pl->id,
                                                         app->detail_key);
      /* A tick in the label rather than a stateful action: the state of a
       * GAction is per-action, and these items all share one action with the
       * playlist id as its target. Making each one stateful would mean an
       * action per playlist, created and destroyed as playlists come and go. */
      g_autofree char *label =
          g_strdup_printf ("%s%s", in ? "✓ " : "", pl->name);
      g_autoptr (GMenuItem) item = g_menu_item_new (label, NULL);
      g_menu_item_set_action_and_target_value (
          item, "win.playlist-toggle", g_variant_new_string (pl->id));
      g_menu_append_item (section, item);
    }
  if (playlists->len > 0)
    g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
  g_object_unref (section);

  g_menu_append (menu, "New playlist…", "win.playlist-new");

  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (app->playlist_menu),
                                  G_MENU_MODEL (menu));
  g_object_unref (menu);
}

/* ---------------------------------------------------------------------- */
/* Multi-select and bulk actions                                          */
/* ---------------------------------------------------------------------- */

/* Borrowed keys, owned by the index. g_ptr_array_unref the array only. */
static GPtrArray *
bulk_keys (App *app)
{
  return ytdl_library_view_selected_keys (YTDL_LIBRARY_VIEW (app->library));
}

static void rebuild_bulk_playlist_menu (App *app);

/* The count, and what is possible with it.
 *
 * Every bulk button is insensitive on an empty selection rather than hidden,
 * because a bar whose contents appear and disappear as you tick boxes is a bar
 * that moves under the pointer. */
static void
update_bulk_bar (App *app)
{
  if (app->bulk_bar == NULL)
    return;

  gboolean on = ytdl_library_view_get_selection_mode (
      YTDL_LIBRARY_VIEW (app->library));
  gtk_action_bar_set_revealed (GTK_ACTION_BAR (app->bulk_bar), on);

  g_autoptr (GPtrArray) keys = bulk_keys (app);
  guint n = keys->len;

  g_autofree char *label =
      n == 0 ? g_strdup ("Nothing selected")
             : g_strdup_printf ("%u selected", n);
  gtk_label_set_text (GTK_LABEL (app->bulk_count), label);

  gboolean any = n > 0;
  gboolean writable =
      app->userdata != NULL && !ytdl_user_data_is_read_only (app->userdata);

  gtk_widget_set_sensitive (app->bulk_watched, any && writable);
  gtk_widget_set_sensitive (app->bulk_unwatched, any && writable);
  gtk_widget_set_sensitive (app->bulk_playlist, any && writable);
  gtk_widget_set_sensitive (app->bulk_copy, any);
  /* A second bulk verify while one is running would interleave two sets of
   * counters into one progress line. */
  gtk_widget_set_sensitive (app->bulk_verify, any && !app->bulk_verifying);
  gtk_widget_set_sensitive (app->bulk_refetch, any);

  rebuild_bulk_playlist_menu (app);
}

static void
on_library_selection_changed (GtkWidget *view, gpointer user_data)
{
  update_bulk_bar (user_data);
}

static void
on_select_toggled (GtkToggleButton *b, gpointer user_data)
{
  App *app = user_data;
  ytdl_library_view_set_selection_mode (YTDL_LIBRARY_VIEW (app->library),
                                        gtk_toggle_button_get_active (b));
  update_bulk_bar (app);
}

static void
on_bulk_select_all (GtkButton *b, gpointer user_data)
{
  App *app = user_data;
  /* Everything the FILTER is showing, not the whole archive: "select all"
   * inside a filtered view meaning the unfiltered set is how somebody marks
   * four thousand videos watched by accident. */
  ytdl_library_view_select_all (YTDL_LIBRARY_VIEW (app->library), TRUE);
  update_bulk_bar (app);
}

static void
bulk_set_watched (App *app, gboolean watched)
{
  g_autoptr (GPtrArray) keys = bulk_keys (app);
  if (keys->len == 0 || app->userdata == NULL)
    return;

  for (guint i = 0; i < keys->len; i++)
    ytdl_user_data_set_watched (app->userdata, g_ptr_array_index (keys, i),
                                watched);

  /* ONE save for the whole batch. Saving per video would rewrite the store a
   * few hundred times for one button press. */
  ytdl_user_data_save (app->userdata);

  g_autofree char *note = g_strdup_printf (
      "Marked %u video%s %s.", keys->len, keys->len == 1 ? "" : "s",
      watched ? "watched" : "unwatched");
  toast (app, note);

  apply_filter (app);
  populate_facets (app);
  update_bulk_bar (app);
}

static void
on_bulk_watched (GtkButton *b, gpointer user_data)
{
  bulk_set_watched (user_data, TRUE);
}

static void
on_bulk_unwatched (GtkButton *b, gpointer user_data)
{
  bulk_set_watched (user_data, FALSE);
}

static void
on_bulk_copy_urls (GtkButton *b, gpointer user_data)
{
  App *app = user_data;
  g_autoptr (GPtrArray) keys = bulk_keys (app);
  if (keys->len == 0 || app->index == NULL)
    return;

  g_autoptr (GString) out = g_string_new (NULL);
  guint have = 0;
  for (guint i = 0; i < keys->len; i++)
    {
      const YtdlEntry *e = ytdl_index_get (app->index,
                                           g_ptr_array_index (keys, i));
      /* A folder with no original_url contributes nothing rather than a blank
       * line: a list with holes in it is worse than a shorter list, because
       * the holes are invisible once it is pasted somewhere. */
      if (e == NULL || e->original_url == NULL || *e->original_url == '\0')
        continue;
      g_string_append (out, e->original_url);
      g_string_append_c (out, '\n');
      have++;
    }

  if (have == 0)
    {
      toast (app, "None of the selected videos recorded a source URL.");
      return;
    }

  gdk_clipboard_set_text (gtk_widget_get_clipboard (app->window), out->str);

  g_autofree char *note =
      have == keys->len
          ? g_strdup_printf ("Copied %u URL%s.", have, have == 1 ? "" : "s")
          : g_strdup_printf ("Copied %u of %u URLs — the rest recorded none.",
                             have, keys->len);
  toast (app, note);
}

/* --- bulk verify ------------------------------------------------------ */

/* One folder's answer, carried back to the main thread.
 *
 * The KEY as well as the state, because the result has to end up in the same
 * verification cache the detail page writes -- a bulk verify whose findings
 * the "failed verification" facet could not see would be a summary you read
 * once and then had no way to act on. The cache is written in
 * bulk_verify_finished: it is not thread-safe, and the entry it stamps each
 * record against has to be looked up in an index this worker must not touch. */
typedef struct
{
  char           *key;
  YtdlVerifyState state;
} BulkVerifyResult;

static void
bulk_verify_result_free (gpointer p)
{
  BulkVerifyResult *r = p;
  if (r == NULL)
    return;
  g_free (r->key);
  g_free (r);
}

typedef struct
{
  App       *app;
  /* COPIES of the key and directory, because a rescan can replace the index
   * while this runs. */
  GPtrArray *keys; /* char*, owned */
  GPtrArray *dirs; /* char*, owned */
  GPtrArray *out;  /* BulkVerifyResult*, owned; handed to the main thread */
} BulkVerifyJob;

static gboolean
bulk_verify_tick (gpointer user_data)
{
  App *app = user_data;
  if (!app->bulk_verifying)
    return G_SOURCE_REMOVE;

  gint done = g_atomic_int_get (&app->bulk_verify_done);
  gint total = g_atomic_int_get (&app->bulk_verify_total);
  g_autofree char *note =
      g_strdup_printf ("Verifying %d of %d…", done, total);
  gtk_label_set_text (GTK_LABEL (app->status), note);
  return G_SOURCE_CONTINUE;
}

static gboolean
bulk_verify_finished (gpointer user_data)
{
  BulkVerifyJob *job = user_data;
  App *app = job->app;
  app->bulk_verifying = FALSE;

  guint bad = 0;
  guint checked = 0;
  guint unchecked = 0;

  for (guint i = 0; i < job->out->len; i++)
    {
      const BulkVerifyResult *r = g_ptr_array_index (job->out, i);
      if (r->state == YTDL_VERIFY_UNKNOWN)
        {
          unchecked++;
          continue;
        }

      checked++;
      if (r->state == YTDL_VERIFY_FAILED)
        bad++;

      /* Recorded here rather than in the worker: the cache stamps every record
       * with the folder's archive_creation_time, which means looking the entry
       * up in the index -- and the index belongs to this thread. */
      const YtdlEntry *e =
          app->index != NULL ? ytdl_index_get (app->index, r->key) : NULL;
      if (e != NULL && app->verify != NULL)
        ytdl_verify_cache_set (app->verify, e, r->state);
    }

  if (app->verify != NULL)
    ytdl_verify_cache_save (app->verify);

  /* THE SUMMARY HAS TO SEPARATE "passed" FROM "had nothing to check".
   *
   * A folder with no checksums.sha256 is not a failure -- the layout contract
   * says a consumer must tolerate one -- but it is not a pass either, and
   * folding it into "all 6 verified" would be this app claiming it checked six
   * folders it never opened a single hash in. That is the same mistake the
   * verify facet's own note exists to avoid. */
  g_autoptr (GString) note = g_string_new (NULL);
  if (checked == 0)
    g_string_append_printf (note,
                            "Nothing to verify: %u folder%s no "
                            "checksums.sha256.",
                            unchecked,
                            unchecked == 1 ? " has" : "s have");
  else if (bad == 0)
    g_string_append_printf (note, "All %u verified.", checked);
  else
    g_string_append_printf (note, "%u of %u failed verification.", bad,
                            checked);

  if (checked > 0 && unchecked > 0)
    g_string_append_printf (note, " %u had no checksums.sha256.", unchecked);

  toast (app, note->str);
  gtk_label_set_text (GTK_LABEL (app->status), note->str);

  g_ptr_array_unref (job->keys);
  g_ptr_array_unref (job->dirs);
  g_ptr_array_unref (job->out);
  g_free (job);

  /* The verify facet can see more than it could a moment ago. */
  populate_facets (app);
  apply_filter (app);
  update_bulk_bar (app);
  return G_SOURCE_REMOVE;
}

static gpointer
bulk_verify_thread (gpointer data)
{
  BulkVerifyJob *job = data;

  for (guint i = 0; i < job->dirs->len; i++)
    {
      const char *dir = g_ptr_array_index (job->dirs, i);
      YtdlChecksumResult *r = ytdl_health_verify_checksums (dir);

      BulkVerifyResult *out = g_new0 (BulkVerifyResult, 1);
      out->key = g_strdup (g_ptr_array_index (job->keys, i));

      if (r == NULL || !r->present)
        {
          /* UNKNOWN, not OK. A folder with no checksums.sha256 has not passed
           * and has not failed; recording it as a pass would put a green
           * answer in the cache for a folder nothing hashed. */
          out->state = YTDL_VERIFY_UNKNOWN;
        }
      else if (r->failed->len > 0 || r->missing->len > 0)
        {
          out->state = YTDL_VERIFY_FAILED;
        }
      else
        {
          out->state = YTDL_VERIFY_OK;
        }

      g_ptr_array_add (job->out, out);
      ytdl_checksum_result_free (r);
      g_atomic_int_inc (&job->app->bulk_verify_done);
    }

  g_idle_add (bulk_verify_finished, job);
  return NULL;
}

static void
on_bulk_verify (GtkButton *b, gpointer user_data)
{
  App *app = user_data;
  if (app->bulk_verifying || app->index == NULL)
    return;

  g_autoptr (GPtrArray) selected = bulk_keys (app);
  if (selected->len == 0)
    return;

  /* The keys and DIRECTORIES are copied out here, on the main thread, while
   * the index is known to be alive. A worker holding borrowed pointers would
   * be holding them into an index a rescan may have replaced. */
  BulkVerifyJob *job = g_new0 (BulkVerifyJob, 1);
  job->app = app;
  job->keys = g_ptr_array_new_with_free_func (g_free);
  job->dirs = g_ptr_array_new_with_free_func (g_free);
  job->out = g_ptr_array_new_with_free_func (bulk_verify_result_free);

  for (guint i = 0; i < selected->len; i++)
    {
      const YtdlEntry *e =
          ytdl_index_get (app->index, g_ptr_array_index (selected, i));
      if (e == NULL || e->dir == NULL || e->key == NULL)
        continue;
      g_ptr_array_add (job->keys, g_strdup (e->key));
      g_ptr_array_add (job->dirs, g_strdup (e->dir));
    }

  if (job->dirs->len == 0)
    {
      g_ptr_array_unref (job->keys);
      g_ptr_array_unref (job->dirs);
      g_ptr_array_unref (job->out);
      g_free (job);
      return;
    }

  app->bulk_verifying = TRUE;
  g_atomic_int_set (&app->bulk_verify_done, 0);
  g_atomic_int_set (&app->bulk_verify_total, (gint) job->dirs->len);
  update_bulk_bar (app);

  g_timeout_add (200, bulk_verify_tick, app);
  GThread *t = g_thread_new ("ytdl-bulk-verify", bulk_verify_thread, job);
  g_thread_unref (t);
}

/* --- bulk re-fetch and bulk playlist ---------------------------------- */

static void
on_bulk_refetch (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  App *app = user_data;
  const char *mode = g_variant_get_string (param, NULL);

  g_autoptr (GPtrArray) keys = bulk_keys (app);
  if (keys->len == 0 || app->index == NULL)
    return;

  /* ONE RUN PER VIDEO, not one run with many URLs. `ytdl --refresh` refreshes
   * the video it is given; a session with several URLs would be a --sync-like
   * shape the refusal list rejects, and one that failed halfway would leave no
   * way to tell which videos were reached. Separate queue entries also mean a
   * single failure is one red row rather than the whole batch. */
  guint queued = 0;
  for (guint i = 0; i < keys->len; i++)
    {
      const YtdlEntry *e =
          ytdl_index_get (app->index, g_ptr_array_index (keys, i));
      if (e == NULL || e->original_url == NULL || *e->original_url == '\0')
        continue;

      g_autoptr (YtdlRunOptions) o = ytdl_run_options_new ();
      o->url = g_strdup (e->original_url);
      o->mode = g_strdup (mode);
      o->refresh = TRUE;

      /* A per-video failure to QUEUE -- a full queue, an unwritable state
       * directory -- stops the batch rather than silently dropping the rest of
       * it, and says which video it stopped on. Carrying on would report a
       * count that was never true. */
      GError *error = NULL;
      g_autofree char *id = ytdl_runner_enqueue (app->runner, o, &error);
      if (id == NULL)
        {
          g_autofree char *why = g_strdup_printf (
              "Queued %u before failing: %s", queued,
              error != NULL ? error->message : "could not queue the run");
          toast (app, why);
          g_clear_error (&error);
          return;
        }
      queued++;
    }

  if (queued == 0)
    {
      toast (app, "None of the selected videos recorded a source URL.");
      return;
    }

  g_autofree char *note = g_strdup_printf (
      "Queued %u %s refresh%s.", queued, mode, queued == 1 ? "" : "es");
  toast (app, note);
  adw_view_stack_set_visible_child_name (ADW_VIEW_STACK (app->stack),
                                         "downloads");
}

static void
on_bulk_playlist_add (GSimpleAction *action, GVariant *param,
                      gpointer user_data)
{
  App *app = user_data;
  const char *id = g_variant_get_string (param, NULL);

  g_autoptr (GPtrArray) keys = bulk_keys (app);
  if (keys->len == 0 || app->userdata == NULL)
    return;

  guint added = 0;
  for (guint i = 0; i < keys->len; i++)
    {
      /* playlist_add returns FALSE for a key already in the list, which is a
       * no-op rather than an error -- so the count is "newly added", which is
       * the number worth reporting. */
      if (ytdl_user_data_playlist_add (app->userdata, id,
                                       g_ptr_array_index (keys, i)))
        added++;
    }

  ytdl_user_data_save (app->userdata);

  const YtdlPlaylist *pl = ytdl_user_data_playlist (app->userdata, id);
  g_autofree char *note = g_strdup_printf (
      "Added %u video%s to %s.", added, added == 1 ? "" : "s",
      pl != NULL ? pl->name : "the playlist");
  toast (app, note);

  apply_playlist_selection (app);
  apply_filter (app);
}

/* The bulk bar's playlist menu. One item per playlist plus New playlist… --
 * and deliberately ADD-ONLY, with no tick marks: a mixed selection where some
 * videos are in a playlist and some are not has no honest checkbox state, and
 * a control that flipped each one independently would remove half of them. */
static void
rebuild_bulk_playlist_menu (App *app)
{
  if (app->bulk_playlist == NULL || app->userdata == NULL)
    return;

  GMenu *menu = g_menu_new ();
  GPtrArray *playlists = ytdl_user_data_playlists (app->userdata);

  for (guint i = 0; i < playlists->len; i++)
    {
      const YtdlPlaylist *pl = g_ptr_array_index (playlists, i);
      g_autoptr (GMenuItem) item = g_menu_item_new (pl->name, NULL);
      g_menu_item_set_action_and_target_value (item, "win.bulk-playlist-add",
                                               g_variant_new_string (pl->id));
      g_menu_append_item (menu, item);
    }

  if (playlists->len == 0)
    {
      g_autoptr (GMenuItem) empty =
          g_menu_item_new ("No playlists yet", NULL);
      g_menu_append_item (menu, empty);
    }

  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (app->bulk_playlist),
                                  G_MENU_MODEL (menu));
  g_object_unref (menu);
}

/* ---------------------------------------------------------------------- */

static void
on_clear_filters (GtkButton *button, gpointer user_data)
{
  App *app = user_data;
  YtdlLibraryFilter *f =
      ytdl_library_view_get_filter (YTDL_LIBRARY_VIEW (app->library));

  /* The needle is part of the filter and so is cleared with it, which means
   * the search ENTRY has to be cleared too or the bar would keep showing a
   * term that is no longer being applied. */
  ytdl_library_filter_reset (f);
  gtk_editable_set_text (GTK_EDITABLE (app->search), "");

  /* reset() drops the playlist restriction, so the dropdown has to follow it
   * back to "All videos" or the popover would claim a playlist is still
   * selected while the grid showed the whole archive. */
  app->populating = TRUE;
  gtk_drop_down_set_selected (GTK_DROP_DOWN (app->playlist_drop), 0);
  app->populating = FALSE;
  g_clear_pointer (&app->playlist_keys, g_hash_table_unref);

  populate_facets (app);
  /* run_search rather than apply_filter: clearing the box has to drop the
   * collection-wide hit set too, and that lives on the other side of the
   * search control rather than inside the filter. */
  run_search (app);
}

/* Rebuild the controls from the current index and the current filter.
 *
 * Called after every scan, because the channel list is the archive's own, and
 * after Clear, because a reset filter has to be visible in the controls. The
 * `populating` guard is what stops each set_active below from kicking off its
 * own rebuild against a half-rebuilt popover. */
static void
populate_facets (App *app)
{
  YtdlLibraryFilter *f =
      ytdl_library_view_get_filter (YTDL_LIBRARY_VIEW (app->library));

  app->populating = TRUE;

  gtk_drop_down_set_selected (GTK_DROP_DOWN (app->sort_drop), (guint) f->sort);
  gtk_button_set_icon_name (GTK_BUTTON (app->sort_dir),
                            f->descending ? "go-down-symbolic"
                                          : "go-up-symbolic");

  gtk_editable_set_text (GTK_EDITABLE (app->date_from),
                         f->date_from != NULL ? f->date_from : "");
  gtk_editable_set_text (GTK_EDITABLE (app->date_to),
                         f->date_to != NULL ? f->date_to : "");

  gtk_check_button_set_active (GTK_CHECK_BUTTON (app->flag_audio),
                               (f->flags & YTDL_FACET_AUDIO_ONLY) != 0);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (app->flag_no_media),
                               (f->flags & YTDL_FACET_NO_MEDIA) != 0);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (app->flag_layout),
                               (f->flags & YTDL_FACET_LAYOUT_TOO_NEW) != 0);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (app->flag_verify),
                               (f->flags & YTDL_FACET_VERIFY_FAILED) != 0);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (app->flag_unwatched),
                               (f->flags & YTDL_FACET_UNWATCHED) != 0);

  /* The verification facet has to say what it is a subset of. It can only
   * see videos somebody has actually verified, and a facet that silently
   * means "of the four I have checked" while looking like it means "of your
   * whole archive" is a facet that will be believed. */
  guint known = app->verify != NULL ? ytdl_verify_cache_known (app->verify) : 0;
  if (known == 0)
    {
      gtk_label_set_text (
          GTK_LABEL (app->verify_note),
          "Nothing has been verified yet. Use Verify on a video's page.");
      gtk_widget_set_sensitive (app->flag_verify, FALSE);
    }
  else
    {
      g_autofree char *note = g_strdup_printf (
          "Of the %u video%s verified so far.", known, known == 1 ? "" : "s");
      gtk_label_set_text (GTK_LABEL (app->verify_note), note);
      gtk_widget_set_sensitive (app->flag_verify, TRUE);
    }

  /* The unwatched facet's note is the mirror image of the verification one,
   * and honest for the opposite reason: this facet DOES see the whole
   * archive -- a video nobody has marked is unwatched, which is the correct
   * answer rather than an unknown one. What it has to say is how much is
   * already marked, because "unwatched" on a fresh install means "all of
   * them" and a facet that appears to do nothing reads as broken. */
  guint watched = app->userdata != NULL
                      ? ytdl_user_data_watched_count (app->userdata)
                      : 0;
  if (app->userdata != NULL && ytdl_user_data_is_read_only (app->userdata))
    gtk_label_set_text (
        GTK_LABEL (app->watched_note),
        "userdata.json could not be read; watch state is read-only this "
        "session.");
  else if (watched == 0)
    gtk_label_set_text (GTK_LABEL (app->watched_note),
                        "Nothing is marked watched yet, so this shows "
                        "everything.");
  else
    {
      g_autofree char *note = g_strdup_printf (
          "%u video%s marked watched.", watched, watched == 1 ? " is" : "s are");
      gtk_label_set_text (GTK_LABEL (app->watched_note), note);
    }

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (app->channel_box)) != NULL)
    gtk_box_remove (GTK_BOX (app->channel_box), child);

  guint channels = app->index != NULL ? app->index->channels->len : 0;
  for (guint i = 0; i < channels; i++)
    {
      const char *name = g_ptr_array_index (app->index->channels, i);
      GtkWidget *check = gtk_check_button_new_with_label (name);
      gtk_check_button_set_active (GTK_CHECK_BUTTON (check),
                                   ytdl_library_filter_has_channel (f, name));
      g_object_set_data_full (G_OBJECT (check), "channel", g_strdup (name),
                              g_free);
      g_signal_connect (check, "toggled", G_CALLBACK (on_channel_toggled),
                        app);
      gtk_box_append (GTK_BOX (app->channel_box), check);
    }

  /* One channel is not a choice. Offering a facet whose only effect is to
   * hide everything or nothing is worse than not offering it. */
  gtk_widget_set_visible (app->channel_frame, channels > 1);

  app->populating = FALSE;
}

/* ---------------------------------------------------------------------- */
/* Collection-wide comment and transcript search                          */
/* ---------------------------------------------------------------------- */

static YtdlSearchScope
current_scope (App *app)
{
  guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (app->scope_drop));
  if (sel == GTK_INVALID_LIST_POSITION)
    return YTDL_SEARCH_METADATA;
  return (YtdlSearchScope) sel;
}

/* The banner is the only place the app can be honest about what a search can
 * currently SEE. A comment search against an index that covers none of the
 * archive returns nothing, and "no results" is a lie about the archive rather
 * than a fact about it. */
static void
update_search_banner (App *app)
{
  if (app->index_banner == NULL)
    return;

  if (!ytdl_search_scope_needs_index (current_scope (app)))
    {
      adw_banner_set_revealed (ADW_BANNER (app->index_banner), FALSE);
      return;
    }

  if (app->indexing)
    {
      gint done = g_atomic_int_get (&app->index_done);
      gint total = g_atomic_int_get (&app->index_total);
      g_autofree char *msg =
          total > 0
              ? g_strdup_printf ("Reading comments and captions… %d of %d",
                                 done, total)
              : g_strdup ("Reading comments and captions…");
      adw_banner_set_title (ADW_BANNER (app->index_banner), msg);
      adw_banner_set_button_label (ADW_BANNER (app->index_banner), "Stop");
      adw_banner_set_revealed (ADW_BANNER (app->index_banner), TRUE);
      return;
    }

  guint stale = ytdl_search_index_outdated (app->search_index, app->index);
  if (stale == 0)
    {
      adw_banner_set_revealed (ADW_BANNER (app->index_banner), FALSE);
      return;
    }

  guint have = ytdl_search_index_size (app->search_index);
  g_autofree char *msg =
      have == 0
          ? g_strdup_printf ("Searching comments and captions needs an index. "
                             "%u video%s to read.",
                             stale, stale == 1 ? "" : "s")
          : g_strdup_printf ("%u video%s changed since the index was built.",
                             stale, stale == 1 ? "" : "s");
  adw_banner_set_title (ADW_BANNER (app->index_banner), msg);
  adw_banner_set_button_label (ADW_BANNER (app->index_banner), "Build index");
  adw_banner_set_revealed (ADW_BANNER (app->index_banner), TRUE);
}

/* Recompute which videos the search box admits, for the current scope.
 *
 * The three shapes are deliberately different, and the difference is the whole
 * reason this is not one code path:
 *
 *   METADATA    the substring match the Library always had. Needs no index,
 *               answers instantly, and is what almost every search is.
 *   COMMENTS
 *   TRANSCRIPT  the index answers alone. The needle is cleared, because
 *               leaving it set would AND the metadata match on top and a
 *               search for a word said in a video would return only the
 *               videos with that word in the TITLE as well.
 *   EVERYTHING  the union of both. A union cannot be expressed as a needle
 *               plus a key set -- those AND -- so the metadata matches are
 *               folded into the key set here.
 */
static void
run_search (App *app)
{
  YtdlLibraryFilter *f =
      ytdl_library_view_get_filter (YTDL_LIBRARY_VIEW (app->library));
  const char *text = gtk_editable_get_text (GTK_EDITABLE (app->search));
  YtdlSearchScope scope = current_scope (app);

  g_clear_pointer (&app->search_hits, g_hash_table_unref);
  f->key_allow = NULL;

  if (scope == YTDL_SEARCH_METADATA || text == NULL || *text == '\0')
    {
      g_free (f->needle);
      f->needle = g_strdup (text);
      apply_filter (app);
      update_search_banner (app);
      return;
    }

  g_free (f->needle);
  f->needle = NULL;

  /* The query's own hash table owns nothing; its keys point into the search
   * index's own storage, which outlives every rebuild of the grid. */
  app->search_hits = ytdl_search_index_query (app->search_index, text, scope);

  if (scope == YTDL_SEARCH_EVERYTHING && app->index != NULL)
    {
      for (guint i = 0; i < app->index->entries->len; i++)
        {
          const YtdlEntry *e = g_ptr_array_index (app->index->entries, i);
          if (ytdl_library_filter_metadata_matches (e, text))
            g_hash_table_add (app->search_hits, e->key);
        }
    }

  f->key_allow = app->search_hits;
  apply_filter (app);
  update_search_banner (app);
}

static void
on_scope_changed (GObject *drop, GParamSpec *pspec, gpointer user_data)
{
  App *app = user_data;
  if (app->populating)
    return;
  run_search (app);
}

typedef struct
{
  App *app;
} IndexJob;

/* Runs on the INDEX THREAD. Two atomics and nothing else, for the same reason
 * the scan's progress callback touches nothing but two atomics. */
static void
on_index_progress (gsize done, gsize total, gpointer user_data)
{
  App *app = user_data;
  g_atomic_int_set (&app->index_done, (gint) done);
  g_atomic_int_set (&app->index_total, (gint) total);
}

static gboolean
on_index_tick (gpointer user_data)
{
  App *app = user_data;
  if (!app->indexing)
    {
      app->index_tick = 0;
      return G_SOURCE_REMOVE;
    }
  update_search_banner (app);
  return G_SOURCE_CONTINUE;
}

static gboolean
on_index_finished (gpointer user_data)
{
  IndexJob *job = user_data;
  App *app = job->app;

  app->indexing = FALSE;
  g_clear_object (&app->index_cancel);
  ytdl_search_index_save (app->search_index);

  /* Re-run the search rather than just redrawing: the whole point of having
   * built the index is that the query the user already typed can now be
   * answered. */
  run_search (app);

  g_free (job);
  return G_SOURCE_REMOVE;
}

static gpointer
index_thread (gpointer user_data)
{
  IndexJob *job = user_data;
  ytdl_search_index_build (job->app->search_index, job->app->index,
                           on_index_progress, job->app,
                           job->app->index_cancel);
  g_idle_add (on_index_finished, job);
  return NULL;
}

/* The banner's button is Build while idle and Stop while building, so one
 * handler covers both -- which also means there is no state in which the
 * banner offers a button that does nothing. */
static void
on_index_banner_clicked (AdwBanner *banner, gpointer user_data)
{
  App *app = user_data;

  if (app->indexing)
    {
      g_cancellable_cancel (app->index_cancel);
      return;
    }
  if (app->index == NULL || app->scanning)
    return;

  app->indexing = TRUE;
  g_atomic_int_set (&app->index_done, 0);
  g_atomic_int_set (&app->index_total, 0);
  g_clear_object (&app->index_cancel);
  app->index_cancel = g_cancellable_new ();

  if (app->index_tick == 0)
    app->index_tick = g_timeout_add (120, on_index_tick, app);
  update_search_banner (app);

  IndexJob *job = g_new0 (IndexJob, 1);
  job->app = app;
  GThread *t = g_thread_new ("ytdl-search-index", index_thread, job);
  g_thread_unref (t);
}

static GtkWidget *
facet_heading (const char *text)
{
  GtkWidget *label = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
  gtk_widget_add_css_class (label, "heading");
  gtk_widget_set_margin_top (label, 6);
  return label;
}

static GtkWidget *
flag_check (App *app, const char *label, YtdlFacetFlags flag)
{
  GtkWidget *check = gtk_check_button_new_with_label (label);
  g_object_set_data (G_OBJECT (check), "flag", GUINT_TO_POINTER (flag));
  g_signal_connect (check, "toggled", G_CALLBACK (on_flag_toggled), app);
  return check;
}

static GtkWidget *
build_filter_popover (App *app)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_widget_set_size_request (box, 300, -1);

  /* --- Sort ------------------------------------------------------- */
  gtk_box_append (GTK_BOX (box), facet_heading ("Sort by"));

  /* The labels come from library_filter.c rather than being written here,
   * so the dropdown's ORDER is the enum's order by construction. A list
   * typed out again in this file would be a second place for a new key to
   * have to be added, and the failure would be a dropdown that silently
   * selects the wrong sort. */
  const char *labels[YTDL_N_SORT_KEYS + 1];
  for (int i = 0; i < YTDL_N_SORT_KEYS; i++)
    labels[i] = ytdl_sort_key_label ((YtdlSortKey) i);
  labels[YTDL_N_SORT_KEYS] = NULL;

  app->sort_drop = gtk_drop_down_new_from_strings (labels);
  gtk_widget_set_hexpand (app->sort_drop, TRUE);
  g_signal_connect (app->sort_drop, "notify::selected",
                    G_CALLBACK (on_sort_changed), app);

  app->sort_dir = gtk_button_new_from_icon_name ("view-sort-descending-symbolic");
  gtk_widget_set_tooltip_text (app->sort_dir, "Descending");
  g_signal_connect (app->sort_dir, "clicked", G_CALLBACK (on_sort_direction),
                    app);

  GtkWidget *sort_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (sort_row), app->sort_drop);
  gtk_box_append (GTK_BOX (sort_row), app->sort_dir);
  gtk_box_append (GTK_BOX (box), sort_row);

  /* --- Channels --------------------------------------------------- */
  app->channel_frame = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (app->channel_frame), facet_heading ("Channels"));

  app->channel_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

  /* Scrolled and height-capped: an archive of two hundred channels would
   * otherwise produce a popover taller than the screen, which GTK will
   * happily try to draw. */
  GtkWidget *channel_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (channel_scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height (
      GTK_SCROLLED_WINDOW (channel_scroll), 220);
  gtk_scrolled_window_set_propagate_natural_height (
      GTK_SCROLLED_WINDOW (channel_scroll), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (channel_scroll),
                                 app->channel_box);
  gtk_box_append (GTK_BOX (app->channel_frame), channel_scroll);
  gtk_box_append (GTK_BOX (box), app->channel_frame);

  /* --- Playlists -------------------------------------------------- */
  app->playlist_frame = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (app->playlist_frame), facet_heading ("Playlist"));

  app->playlist_drop = gtk_drop_down_new (NULL, NULL);
  gtk_widget_set_hexpand (app->playlist_drop, TRUE);
  g_signal_connect (app->playlist_drop, "notify::selected",
                    G_CALLBACK (on_playlist_changed), app);
  gtk_box_append (GTK_BOX (app->playlist_frame), app->playlist_drop);

  GtkWidget *playlist_hint =
      gtk_label_new ("Add videos to a playlist from a video's page.");
  gtk_label_set_xalign (GTK_LABEL (playlist_hint), 0.0f);
  gtk_label_set_wrap (GTK_LABEL (playlist_hint), TRUE);
  gtk_widget_add_css_class (playlist_hint, "dim-label");
  gtk_widget_add_css_class (playlist_hint, "caption");
  gtk_box_append (GTK_BOX (app->playlist_frame), playlist_hint);

  /* Hidden until there is one; populate_playlists decides. */
  gtk_widget_set_visible (app->playlist_frame, FALSE);
  gtk_box_append (GTK_BOX (box), app->playlist_frame);

  /* --- Dates ------------------------------------------------------ */
  gtk_box_append (GTK_BOX (box), facet_heading ("Uploaded between"));

  app->date_from = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (app->date_from), "From");
  gtk_entry_set_max_length (GTK_ENTRY (app->date_from), 10);
  gtk_widget_set_hexpand (app->date_from, TRUE);
  g_signal_connect (app->date_from, "changed", G_CALLBACK (on_date_changed),
                    app);

  app->date_to = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (app->date_to), "To");
  gtk_entry_set_max_length (GTK_ENTRY (app->date_to), 10);
  gtk_widget_set_hexpand (app->date_to, TRUE);
  g_signal_connect (app->date_to, "changed", G_CALLBACK (on_date_changed),
                    app);

  GtkWidget *dates = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (dates), app->date_from);
  gtk_box_append (GTK_BOX (dates), app->date_to);
  gtk_box_append (GTK_BOX (box), dates);

  GtkWidget *date_hint = gtk_label_new ("YYYYMMDD, or any form of it.");
  gtk_label_set_xalign (GTK_LABEL (date_hint), 0.0f);
  gtk_widget_add_css_class (date_hint, "dim-label");
  gtk_widget_add_css_class (date_hint, "caption");
  gtk_box_append (GTK_BOX (box), date_hint);

  /* --- Flags ------------------------------------------------------ */
  gtk_box_append (GTK_BOX (box), facet_heading ("Show only"));

  app->flag_audio = flag_check (app, "Audio only", YTDL_FACET_AUDIO_ONLY);
  app->flag_no_media = flag_check (app, "No media file", YTDL_FACET_NO_MEDIA);
  app->flag_layout =
      flag_check (app, "Newer archive layout", YTDL_FACET_LAYOUT_TOO_NEW);
  app->flag_verify =
      flag_check (app, "Failed verification", YTDL_FACET_VERIFY_FAILED);
  app->flag_unwatched =
      flag_check (app, ytdl_facet_flag_label (YTDL_FACET_UNWATCHED),
                  YTDL_FACET_UNWATCHED);

  gtk_box_append (GTK_BOX (box), app->flag_audio);
  gtk_box_append (GTK_BOX (box), app->flag_no_media);
  gtk_box_append (GTK_BOX (box), app->flag_layout);
  gtk_box_append (GTK_BOX (box), app->flag_unwatched);

  app->watched_note = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (app->watched_note), 0.0f);
  gtk_label_set_wrap (GTK_LABEL (app->watched_note), TRUE);
  gtk_widget_add_css_class (app->watched_note, "dim-label");
  gtk_widget_add_css_class (app->watched_note, "caption");
  gtk_widget_set_margin_start (app->watched_note, 28);
  gtk_box_append (GTK_BOX (box), app->watched_note);

  gtk_box_append (GTK_BOX (box), app->flag_verify);

  app->verify_note = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (app->verify_note), 0.0f);
  gtk_label_set_wrap (GTK_LABEL (app->verify_note), TRUE);
  gtk_widget_add_css_class (app->verify_note, "dim-label");
  gtk_widget_add_css_class (app->verify_note, "caption");
  gtk_widget_set_margin_start (app->verify_note, 28);
  gtk_box_append (GTK_BOX (box), app->verify_note);

  /* --- Clear ------------------------------------------------------ */
  GtkWidget *clear = gtk_button_new_with_label ("Clear filters");
  gtk_widget_set_margin_top (clear, 6);
  g_signal_connect (clear, "clicked", G_CALLBACK (on_clear_filters), app);
  gtk_box_append (GTK_BOX (box), clear);

  GtkWidget *popover = gtk_popover_new ();
  gtk_popover_set_child (GTK_POPOVER (popover), box);
  return popover;
}

/* ---------------------------------------------------------------------- */
/* Construction                                                           */
/* ---------------------------------------------------------------------- */

static GtkWidget *
build_main_page (App *app)
{
  GtkWidget *header = adw_header_bar_new ();
  app->header = header;

  app->rescan = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_widget_set_tooltip_text (app->rescan, "Rescan the archive");
  g_signal_connect (app->rescan, "clicked", G_CALLBACK (on_rescan_clicked),
                    app);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), app->rescan);

  app->spinner = gtk_spinner_new ();
  gtk_widget_set_visible (app->spinner, FALSE);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), app->spinner);

  app->search_button = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (app->search_button),
                            "system-search-symbolic");
  gtk_widget_set_tooltip_text (app->search_button,
                               "Search the library (Ctrl+F)");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), app->search_button);

  /* --- sort and facets --------------------------------------------- */
  /* ON ICON NAMES, because this cost a round of rework and the lesson is
   * not obvious. On Ubuntu 24.04 -- adwaita-icon-theme 46, GTK 4.14, which
   * is this app's stated version floor -- a number of perfectly ordinary
   * symbolic names DO NOT RENDER. They are present as files, and
   * gtk_icon_theme_has_icon returns TRUE for them, and the widget still
   * draws the broken-image glyph. view-filter-symbolic and funnel-symbolic
   * are absent outright; preferences-other-symbolic, view-sort-ascending-
   * symbolic and view-sort-descending-symbolic are present, claimed, and
   * broken.
   *
   * So the rule for this file is: an icon name goes in only after it has
   * been seen to DRAW, not after has_icon agreed it exists. view-list and
   * go-up/go-down were picked that way.
   *
   * Note applications-utilities-symbolic, on the Health page below, is one
   * of the broken ones and predates this change. Left alone here rather
   * than fixed in passing, because it is not what this patch is about. */
  /* How many facets are active has to be visible from the header, because a
   * facet that is still narrowing the library after the popover has been
   * closed and forgotten is the one state this UI can get wrong in a way
   * that reads as lost videos.
   *
   * The count sits BESIDE the icon, inside the button, rather than as a
   * badge overlaid on its corner. The overlay was tried first and looked
   * right in the code: a GtkOverlay sizes itself to its child, so the
   * "badge" was drawn inside the button's own 34 pixels, directly on top of
   * the icon -- a digit superimposed on a glyph, which reads as a rendering
   * fault rather than as a count. Seen in a screenshot, not reasoned about.
   * A button that grows by one character when a filter is on is also the
   * more honest affordance: the button visibly changes. */
  app->filter_button = gtk_menu_button_new ();
  gtk_widget_set_tooltip_text (app->filter_button, "Sort and filter");

  app->filter_badge = gtk_label_new (NULL);
  gtk_widget_set_visible (app->filter_badge, FALSE);
  gtk_widget_add_css_class (app->filter_badge, "numeric");

  GtkWidget *filter_content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_append (GTK_BOX (filter_content),
                  gtk_image_new_from_icon_name ("view-list-symbolic"));
  gtk_box_append (GTK_BOX (filter_content), app->filter_badge);
  gtk_menu_button_set_child (GTK_MENU_BUTTON (app->filter_button),
                             filter_content);

  gtk_menu_button_set_popover (GTK_MENU_BUTTON (app->filter_button),
                               build_filter_popover (app));
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), app->filter_button);

  /* The selection-mode switch. In the header rather than in the bulk bar,
   * because the bar is what it reveals: a control that dismissed the thing it
   * lives inside would have nowhere to be when the bar was hidden. */
  app->select_toggle = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (app->select_toggle),
                            "object-select-symbolic");
  gtk_widget_set_tooltip_text (app->select_toggle, "Select videos");
  g_signal_connect (app->select_toggle, "toggled",
                    G_CALLBACK (on_select_toggled), app);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), app->select_toggle);

  /* --- the four pages --------------------------------------------- */
  app->stack = adw_view_stack_new ();

  app->library = ytdl_library_view_new ();
  g_signal_connect (app->library, "video-activated",
                    G_CALLBACK (on_video_activated), app);
  g_signal_connect (app->library, "selection-changed",
                    G_CALLBACK (on_library_selection_changed), app);

  /* The saved ordering, before the first scan so the first grid ever drawn
   * is already in the order this user chose. The FACETS deliberately do not
   * persist -- see settings.h: an app that reopens showing a fifth of the
   * archive with no visible reason looks like it lost your videos. */
  {
    YtdlLibraryFilter *f =
        ytdl_library_view_get_filter (YTDL_LIBRARY_VIEW (app->library));
    f->sort = ytdl_sort_key_from_id (app->settings->sort_key);
    f->descending = app->settings->sort_descending;
  }
  ytdl_library_view_set_verify_cache (YTDL_LIBRARY_VIEW (app->library),
                                      app->verify);

  /* The watched set is handed over ONCE and is live: the store mutates the
   * same table, so marking a video watched on its page is visible to the
   * next refilter without anything having to re-hand it. */
  {
    YtdlLibraryFilter *f =
        ytdl_library_view_get_filter (YTDL_LIBRARY_VIEW (app->library));
    f->watched_keys = ytdl_user_data_watched_keys (app->userdata);
  }
  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (app->stack),
                                       app->library, "library", "Library",
                                       "view-grid-symbolic");

  GtkWidget *downloads = ytdl_downloads_view_new (app->runner, app->settings);
  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (app->stack), downloads,
                                       "downloads", "Downloads",
                                       "folder-download-symbolic");

  /* The pipeline's subscriptions and its hourly check. This app runs no timer
   * -- see subscriptions.h -- so the pane is a view onto `ytdl` and nothing
   * more, and it is built before the Downloads view's "subscribed" signal can
   * be connected to it. */
  app->subscriptions = ytdl_subscriptions_view_new (app->runner);
  g_signal_connect (app->subscriptions, "message",
                    G_CALLBACK (on_subscriptions_message), app);
  g_signal_connect (downloads, "subscribed", G_CALLBACK (on_subscribed), app);
  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (app->stack),
                                       app->subscriptions, "subscriptions",
                                       "Subscriptions",
                                       "application-rss+xml-symbolic");

  app->health = ytdl_health_view_new (app->settings);
  ytdl_health_view_set_archive_root (YTDL_HEALTH_VIEW (app->health),
                                     app->archive_root);
  g_signal_connect (app->health, "archive-root-chosen",
                    G_CALLBACK (on_archive_root_chosen), app);
  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (app->stack),
                                       app->health, "health", "Health",
                                       "applications-utilities-symbolic");

  g_signal_connect (app->stack, "notify::visible-child",
                    G_CALLBACK (on_page_changed), app);

  GtkWidget *switcher = adw_view_switcher_new ();
  adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher),
                               ADW_VIEW_STACK (app->stack));
  adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher),
                                ADW_VIEW_SWITCHER_POLICY_WIDE);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), switcher);

  /* --- search ------------------------------------------------------ */
  app->search = gtk_search_entry_new ();
  gtk_widget_set_hexpand (app->search, TRUE);
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (app->search),
                                         "Search title, channel or id");
  g_signal_connect (app->search, "search-changed",
                    G_CALLBACK (on_search_changed), app);

  /* WHERE a search looks, beside the box rather than buried in the filter
   * popover. The scope changes what the same typed words MEAN, so it belongs
   * where the words are -- and it is the only affordance that tells anyone the
   * comments and captions are searchable at all, which was the entire gap. */
  const char *scopes[YTDL_N_SEARCH_SCOPES + 1];
  for (int i = 0; i < YTDL_N_SEARCH_SCOPES; i++)
    scopes[i] = ytdl_search_scope_label ((YtdlSearchScope) i);
  scopes[YTDL_N_SEARCH_SCOPES] = NULL;

  app->scope_drop = gtk_drop_down_new_from_strings (scopes);
  gtk_widget_set_tooltip_text (app->scope_drop, "Where to search");
  g_signal_connect (app->scope_drop, "notify::selected",
                    G_CALLBACK (on_scope_changed), app);

  GtkWidget *search_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (search_row), app->search);
  gtk_box_append (GTK_BOX (search_row), app->scope_drop);

  app->search_bar = gtk_search_bar_new ();
  gtk_search_bar_set_child (GTK_SEARCH_BAR (app->search_bar), search_row);
  gtk_search_bar_connect_entry (GTK_SEARCH_BAR (app->search_bar),
                                GTK_EDITABLE (app->search));
  /* Two-way, so Escape inside the bar un-toggles the button as well. */
  g_object_bind_property (app->search_button, "active", app->search_bar,
                          "search-mode-enabled",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  /* --- status line ------------------------------------------------- */
  app->status = gtk_label_new ("Starting…");
  gtk_label_set_xalign (GTK_LABEL (app->status), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (app->status), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (app->status, "dim-label");
  gtk_widget_add_css_class (app->status, "caption");

  GtkWidget *status_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (status_bar, "toolbar");
  gtk_box_append (GTK_BOX (status_bar), app->status);

  /* --- bulk bar ---------------------------------------------------- */
  /*
   * A GtkActionBar rather than a box: it has the revealer, the start/centre/end
   * packing and the toolbar styling already, and set_revealed animates rather
   * than making the window jump by a row.
   *
   * Every button stays VISIBLE and goes insensitive on an empty selection. A
   * bar whose contents appear and disappear as you tick boxes is a bar that
   * moves under the pointer.
   */
  app->bulk_bar = gtk_action_bar_new ();
  gtk_action_bar_set_revealed (GTK_ACTION_BAR (app->bulk_bar), FALSE);

  app->bulk_count = gtk_label_new ("Nothing selected");
  gtk_widget_add_css_class (app->bulk_count, "dim-label");
  gtk_widget_add_css_class (app->bulk_count, "caption");
  gtk_action_bar_pack_start (GTK_ACTION_BAR (app->bulk_bar), app->bulk_count);

  app->bulk_select_all = gtk_button_new_with_label ("Select all");
  gtk_widget_set_tooltip_text (
      app->bulk_select_all,
      "Everything the current filter is showing — not the whole archive.");
  g_signal_connect (app->bulk_select_all, "clicked",
                    G_CALLBACK (on_bulk_select_all), app);
  gtk_action_bar_pack_start (GTK_ACTION_BAR (app->bulk_bar),
                             app->bulk_select_all);

  app->bulk_watched = gtk_button_new_with_label ("Mark watched");
  g_signal_connect (app->bulk_watched, "clicked",
                    G_CALLBACK (on_bulk_watched), app);
  gtk_action_bar_pack_start (GTK_ACTION_BAR (app->bulk_bar),
                             app->bulk_watched);

  app->bulk_unwatched = gtk_button_new_with_label ("Mark unwatched");
  g_signal_connect (app->bulk_unwatched, "clicked",
                    G_CALLBACK (on_bulk_unwatched), app);
  gtk_action_bar_pack_start (GTK_ACTION_BAR (app->bulk_bar),
                             app->bulk_unwatched);

  app->bulk_playlist = gtk_menu_button_new ();
  gtk_menu_button_set_label (GTK_MENU_BUTTON (app->bulk_playlist),
                             "Add to playlist");
  gtk_action_bar_pack_start (GTK_ACTION_BAR (app->bulk_bar),
                             app->bulk_playlist);

  /* The re-fetch menu is FIXED rather than built from the pipeline's mode
   * list: only the three no-media modes are refreshable, which is ytdl.ps1's
   * own rule, and offering "full" here would queue a batch the pipeline
   * refuses one run at a time. */
  {
    GMenu *refetch = g_menu_new ();
    static const struct { const char *label; const char *mode; } modes[] = {
      { "Re-fetch comments", "comments-only" },
      { "Re-fetch subtitles", "subs-only" },
      { "Re-fetch metadata", "metadata-only" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (modes); i++)
      {
        g_autoptr (GMenuItem) item = g_menu_item_new (modes[i].label, NULL);
        g_menu_item_set_action_and_target_value (
            item, "win.bulk-refetch", g_variant_new_string (modes[i].mode));
        g_menu_append_item (refetch, item);
      }

    app->bulk_refetch = gtk_menu_button_new ();
    gtk_menu_button_set_label (GTK_MENU_BUTTON (app->bulk_refetch), "Re-fetch");
    gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (app->bulk_refetch),
                                    G_MENU_MODEL (refetch));
    g_object_unref (refetch);
  }
  gtk_action_bar_pack_end (GTK_ACTION_BAR (app->bulk_bar), app->bulk_refetch);

  app->bulk_verify = gtk_button_new_with_label ("Verify");
  gtk_widget_set_tooltip_text (
      app->bulk_verify,
      "Re-hashes every file in each selected folder. Seconds per video.");
  g_signal_connect (app->bulk_verify, "clicked", G_CALLBACK (on_bulk_verify),
                    app);
  gtk_action_bar_pack_end (GTK_ACTION_BAR (app->bulk_bar), app->bulk_verify);

  app->bulk_copy = gtk_button_new_with_label ("Copy URLs");
  g_signal_connect (app->bulk_copy, "clicked", G_CALLBACK (on_bulk_copy_urls),
                    app);
  gtk_action_bar_pack_end (GTK_ACTION_BAR (app->bulk_bar), app->bulk_copy);

  app->switcher_bar = adw_view_switcher_bar_new ();
  adw_view_switcher_bar_set_stack (ADW_VIEW_SWITCHER_BAR (app->switcher_bar),
                                   ADW_VIEW_STACK (app->stack));

  /* --- assembly ---------------------------------------------------- */
  /* AdwBanner, not a toast and not a label wedged into the status line. A
   * toast disappears, and the fact it carries -- that a comment search
   * currently cannot see most of the archive -- stays true until somebody
   * acts on it. A banner is the widget for exactly that: persistent, one
   * action, and it goes away by itself when the condition does. */
  app->index_banner = adw_banner_new ("");
  adw_banner_set_revealed (ADW_BANNER (app->index_banner), FALSE);
  g_signal_connect (app->index_banner, "button-clicked",
                    G_CALLBACK (on_index_banner_clicked), app);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), app->search_bar);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), app->index_banner);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), app->stack);
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar), app->bulk_bar);
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar), status_bar);
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar),
                                   app->switcher_bar);

  AdwNavigationPage *page =
      adw_navigation_page_new (toolbar, "yt-dlp Archive");
  adw_navigation_page_set_tag (page, "main");
  return GTK_WIDGET (page);
}

static GtkWidget *
build_detail_page (App *app)
{
  app->detail = ytdl_detail_view_new ();
  g_signal_connect (app->detail, "refetch-requested",
                    G_CALLBACK (on_refetch_requested), app);

  GtkWidget *header = adw_header_bar_new ();

  /* view-list-symbolic, not a playlist icon: adwaita-icon-theme 46 -- what
   * Ubuntu 24.04 ships, this app's floor -- has no playlist glyph, and a name
   * it does not have draws the broken-image square rather than failing at
   * build time. Every icon name in this app has been seen to DRAW. */
  app->playlist_menu = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (app->playlist_menu),
                                 "view-list-symbolic");
  gtk_widget_set_tooltip_text (app->playlist_menu, "Playlists");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), app->playlist_menu);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), app->detail);

  AdwNavigationPage *page = adw_navigation_page_new (toolbar, "Video");
  adw_navigation_page_set_tag (page, "detail");
  return GTK_WIDGET (page);
}

/* ---------------------------------------------------------------------- */
/* Notifications                                                          */
/* ---------------------------------------------------------------------- */

/* Connected here rather than in the Downloads view, because a queue that
 * finishes while the Library is showing is exactly the case this is for, and
 * the view only exists to be looked at.
 *
 * The tracker is updated FIRST and unconditionally. The two checks after it
 * -- the setting, and whether the window is the one with focus -- decide only
 * whether this particular notice is sent, never what the tracker knows. */
static void
on_runner_settled (YtdlRunner *runner, gpointer user_data)
{
  App *app = user_data;

  guint remaining = 0;
  g_autoptr (GPtrArray) history = ytdl_runner_settled (runner, &remaining);
  g_autoptr (YtdlNotice) notice =
      ytdl_notice_tracker_update (app->notices, history, remaining);
  if (notice == NULL || !app->settings->notify)
    return;

  /* Looked up, not app->window: this can run while the application is
   * shutting down, after the window has gone, and a stored pointer would be
   * dangling by then. NULL means there is no window to be away from. */
  GtkWindow *win = gtk_application_get_active_window (app->gtkapp);
  if (win == NULL || gtk_window_is_active (win))
    return;

  g_autoptr (GNotification) n = g_notification_new (notice->title);
  g_notification_set_body (n, notice->body);
  /* HIGH, not URGENT: urgent is for things that must break through Do Not
   * Disturb, and a failed download is not a fire alarm. */
  g_notification_set_priority (n, notice->failure
                                      ? G_NOTIFICATION_PRIORITY_HIGH
                                      : G_NOTIFICATION_PRIORITY_NORMAL);
  g_notification_set_default_action (n, "app.show-downloads");
  /* One id for everything: a later notice REPLACES an earlier one rather
   * than stacking under it (notify.h, rule 3). */
  g_application_send_notification (G_APPLICATION (app->gtkapp),
                                   YTDL_NOTICE_ID, n);
}

/* What clicking a notification does: bring the window forward on the page
 * that explains it. On the APPLICATION, because that is the only action map
 * a notification can reach -- the desktop activates it over D-Bus by the
 * app's own id. */
static void
on_show_downloads (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  App *app = user_data;
  if (app->window == NULL)
    return;
  adw_navigation_view_pop_to_tag (ADW_NAVIGATION_VIEW (app->nav), "main");
  adw_view_stack_set_visible_child_name (ADW_VIEW_STACK (app->stack),
                                         "downloads");
  gtk_window_present (GTK_WINDOW (app->window));
}

static void
on_activate (GtkApplication *gtkapp, gpointer user_data)
{
  App *app = user_data;

  ytdl_style_load ();

  app->window = adw_application_window_new (gtkapp);
  gtk_window_set_title (GTK_WINDOW (app->window), "yt-dlp Archive");
  gtk_window_set_default_size (GTK_WINDOW (app->window), 1180, 880);
  /* AdwBreakpoint refuses to work without one, and says so on stderr: it has
   * to know the smallest size the window can take before it can decide which
   * conditions are reachable. 360x300 is the phone-sized floor the breakpoint
   * below is written for. */
  gtk_widget_set_size_request (app->window, 360, 300);

  GtkWidget *main_page = build_main_page (app);
  app->detail_page = build_detail_page (app);

  app->nav = adw_navigation_view_new ();
  adw_navigation_view_add (ADW_NAVIGATION_VIEW (app->nav),
                           ADW_NAVIGATION_PAGE (main_page));
  adw_navigation_view_add (ADW_NAVIGATION_VIEW (app->nav),
                           ADW_NAVIGATION_PAGE (app->detail_page));
  g_signal_connect (app->nav, "popped", G_CALLBACK (on_popped), app);

  app->toasts = adw_toast_overlay_new ();
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (app->toasts), app->nav);
  adw_application_window_set_content (
      ADW_APPLICATION_WINDOW (app->window), app->toasts);

  /* Ctrl+F and plain typing both open the search bar, which is what every
   * other GNOME application does. Capture is on the window, so it works
   * wherever the focus happens to be. */
  gtk_search_bar_set_key_capture_widget (GTK_SEARCH_BAR (app->search_bar),
                                         app->window);

  /* Below 600sp three labelled switcher buttons no longer fit in a header
   * bar. The switcher moves to a bar at the bottom and the header falls back
   * to its own AdwWindowTitle -- which is what setting title-widget to its
   * default (NULL) means. Two setters, and every pane reflows: the grid drops
   * to two columns, the preference groups keep their measure, and nothing
   * else in this file knows the window can be narrow. */
  AdwBreakpoint *bp = adw_breakpoint_new (
      adw_breakpoint_condition_parse ("max-width: 600sp"));
  adw_breakpoint_add_setters (bp,
                              G_OBJECT (app->switcher_bar), "reveal", TRUE,
                              G_OBJECT (app->header), "title-widget", NULL,
                              NULL);
  adw_application_window_add_breakpoint (
      ADW_APPLICATION_WINDOW (app->window), bp);

  /* The playlist menu's two actions. On the WINDOW rather than the
   * application, because both of them act on the video the detail page is
   * currently showing -- a per-window fact. */
  {
    static const GActionEntry playlist_actions[] = {
      { "playlist-toggle", on_playlist_toggle_membership, "s", NULL, NULL,
        { 0 } },
      { "playlist-new", on_new_playlist, NULL, NULL, NULL, { 0 } },
      { "bulk-playlist-add", on_bulk_playlist_add, "s", NULL, NULL, { 0 } },
      { "bulk-refetch", on_bulk_refetch, "s", NULL, NULL, { 0 } },
    };
    g_action_map_add_action_entries (G_ACTION_MAP (app->window),
                                     playlist_actions,
                                     G_N_ELEMENTS (playlist_actions), app);
  }

  ytdl_detail_view_set_user_data (YTDL_DETAIL_VIEW (app->detail),
                                  app->userdata);
  g_signal_connect (app->detail, "watch-state-changed",
                    G_CALLBACK (on_watch_state_changed), app);

  on_page_changed (G_OBJECT (app->stack), NULL, app);
  /* Before the first scan, so the popover is never briefly a set of empty
   * controls with a channel list that has not been built yet. */
  populate_facets (app);
  populate_playlists (app);
  rebuild_playlist_menu (app);
  gtk_window_present (GTK_WINDOW (app->window));

  app->gtkapp = gtkapp;
  {
    static const GActionEntry app_actions[] = {
      { "show-downloads", on_show_downloads, NULL, NULL, NULL, { 0 } },
    };
    g_action_map_add_action_entries (G_ACTION_MAP (gtkapp), app_actions,
                                     G_N_ELEMENTS (app_actions), app);
  }
  g_signal_connect (app->runner, "state-changed",
                    G_CALLBACK (on_runner_settled), app);

  /* The worker starts only once the window it will emit into is real, which
   * is why ytdl_runner_new does not start it. A restored queue would
   * otherwise begin producing events with nothing connected to receive them. */
  ytdl_runner_start (app->runner);
  ytdl_health_view_refresh (YTDL_HEALTH_VIEW (app->health), FALSE);
  start_scan (app);
}

int
main (int argc, char **argv)
{
  g_autofree char *archive_root = NULL;

  /* --archive-root is how the app is pointed at a tree that autodetection
   * would not find: a fixture, a mounted NAS share, a second archive. */
  const GOptionEntry entries[] = {
    { "archive-root", 'r', 0, G_OPTION_ARG_FILENAME, &archive_root,
      "Path to the archive (a data root, or Complete Archive itself)",
      "PATH" },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
  };

  g_autoptr (GOptionContext) ctx =
      g_option_context_new ("- browse a yt-dlp archive");
  g_option_context_add_main_entries (ctx, entries, NULL);
  g_option_context_set_ignore_unknown_options (ctx, TRUE);
  g_option_context_set_help_enabled (ctx, TRUE);

  GError *error = NULL;
  if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
      g_printerr ("%s\n", error->message);
      g_clear_error (&error);
      return 1;
    }

  App app = { 0 };
  app.settings = ytdl_settings_load ();

  /* Before any window exists, because the Downloads pane loads the profile
   * store while it is being built and a fresh install has to have its default
   * on disk by then. Does nothing on every launch after the first. */
  ytdl_profiles_seed_default ();

  app.runner = ytdl_runner_new ();
  /* Seeded with the history the runner just restored, so last session's runs
   * are never announced as if they had just finished (notify.h, rule 5).
   * Before the worker starts, so nothing can finish in between. */
  {
    g_autoptr (GPtrArray) restored = ytdl_runner_history (app.runner);
    app.notices = ytdl_notice_tracker_new (restored);
  }
  /* Before anything can enqueue, so the very first run -- including a re-fetch
   * started from a video's page before the Downloads pane is ever opened --
   * goes out with the saved cookies and proxy. The Downloads pane updates it
   * whenever the Connection settings change. */
  {
    g_autoptr (YtdlRunOptions) conn = ytdl_settings_connection (app.settings);
    ytdl_runner_set_connection (app.runner, conn);
  }

  /* Loaded before any window exists, because build_main_page hands it
   * straight to the library view. Never fails: a missing or corrupt store
   * yields an empty cache, which costs one re-verify. */
  app.verify = ytdl_verify_cache_load ();
  /* Loaded, never built, at startup. Reading every info.json in an archive is
   * the most expensive thing this app can do, and doing it unasked on every
   * launch would make opening the window cost what opening every video costs
   * -- which is the exact rule the index scan already follows. The banner
   * offers it when a scope needs it. */
  app.search_index = ytdl_search_index_load ();
  /* USER DATA, not a cache, and loaded from the state directory for that
   * reason. Never fails: a file that will not parse yields an empty store
   * that refuses to save over it, which the UI says out loud. */
  app.userdata = ytdl_user_data_load ();

  /* --archive-root, then the root chosen on the Health pane, then the usual
   * locations. The middle one used to be missing: settings.json carried an
   * archive_root that nothing read and nothing could write, so a machine
   * whose archive autodetection could not find had no answer but a flag on
   * every launch. ytdl_choose_archive_root owns the precedence. */
  app.archive_root =
      ytdl_choose_archive_root (archive_root, app.settings->archive_root);

  if (archive_root != NULL && app.archive_root == NULL)
    g_printerr ("No 'Complete Archive' found under %s -- starting empty.\n",
                archive_root);

  /* AdwApplication rather than GtkApplication: it is what calls adw_init(),
   * loads libadwaita's stylesheet and connects the app to the system
   * light/dark and accent-colour settings. An AdwApplicationWindow inside a
   * plain GtkApplication is undefined behaviour, not a shortcut. */
  g_autoptr (AdwApplication) adwapp =
      adw_application_new ("io.github.avimehandru.YtdlGtk",
                           G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (adwapp, "activate", G_CALLBACK (on_activate), &app);

  /* argv is deliberately not forwarded: GApplication would try to parse
   * --archive-root itself and refuse it. */
  int status = g_application_run (G_APPLICATION (adwapp), 0, NULL);

  /* Stop the worker BEFORE the index goes: a run finishing during teardown
   * would otherwise touch state that has already been freed. */
  ytdl_runner_stop (app.runner);
  g_clear_object (&app.runner);
  g_clear_pointer (&app.notices, ytdl_notice_tracker_free);
  /* Written once, on the way out, rather than after every verify: this is a
   * cache, the only cost of losing the last few results is re-hashing those
   * folders, and a JSON rewrite per verification would be the more
   * expensive mistake. */
  ytdl_verify_cache_save (app.verify);
  g_clear_pointer (&app.verify, ytdl_verify_cache_free);
  /* Cancelled, not joined: the build thread holds only the index and a
   * cancellable, and a cancelled build simply stops having re-parsed fewer
   * videos than it meant to -- which the next launch's stale count picks up.
   * Saving here keeps whatever it did finish. */
  if (app.index_cancel != NULL)
    g_cancellable_cancel (app.index_cancel);
  g_clear_object (&app.index_cancel);
  ytdl_search_index_save (app.search_index);
  g_clear_pointer (&app.search_index, ytdl_search_index_free);
  g_clear_pointer (&app.search_hits, g_hash_table_unref);
  /* Saved here as well as at every change: the tick that records a resume
   * point is five seconds wide, and closing the window is exactly the moment
   * someone expects the last few seconds to have been kept. */
  ytdl_user_data_save (app.userdata);
  g_clear_pointer (&app.userdata, ytdl_user_data_free);
  g_clear_pointer (&app.playlist_keys, g_hash_table_unref);
  g_clear_pointer (&app.playlist_ids, g_ptr_array_unref);
  g_clear_pointer (&app.detail_key, g_free);
  g_clear_pointer (&app.settings, ytdl_settings_free);
  g_clear_pointer (&app.index, ytdl_index_free);
  g_free (app.archive_root);
  return status;
}
