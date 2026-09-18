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
 * AdwViewSwitcher own the three top-level pages; AdwToolbarView owns the
 * header, the search bar and the status line and the way they behave as the
 * content scrolls under them. An AdwBreakpoint moves the switcher to the
 * bottom on a narrow window. None of that is code here.
 */

#include <adwaita.h>

#include "archive.h"
#include "detail_view.h"
#include "downloads_view.h"
#include "health_view.h"
#include "library_filter.h"
#include "library_view.h"
#include "paths.h"
#include "pipeline.h"
#include "profiles.h"
#include "search_index.h"
#include "userdata.h"
#include "settings.h"
#include "style.h"
#include "verify_cache.h"

typedef struct
{
  GtkWidget *window;
  GtkWidget *library;
  GtkWidget *health;
  GtkWidget *detail;

  GtkWidget *header;      /* AdwHeaderBar of the main page */
  GtkWidget *switcher_bar;/* AdwViewSwitcherBar, revealed when narrow */
  GtkWidget *nav;         /* AdwNavigationView: main <-> detail */
  GtkWidget *detail_page; /* AdwNavigationPage wrapping the detail view */
  GtkWidget *stack;       /* AdwViewStack: library / downloads / health */
  GtkWidget *toasts;      /* AdwToastOverlay */

  YtdlSettings *settings;
  YtdlRunner   *runner;
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

  /* --- the three pages -------------------------------------------- */
  app->stack = adw_view_stack_new ();

  app->library = ytdl_library_view_new ();
  g_signal_connect (app->library, "video-activated",
                    G_CALLBACK (on_video_activated), app);

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
