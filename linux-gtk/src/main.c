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
#include "library_view.h"
#include "paths.h"
#include "pipeline.h"
#include "settings.h"
#include "style.h"

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
          "Archive' under the usual locations. Pass --archive-root, or set "
          "YTDLP_INSTALL_ROOT.";
      set_status (app, msg);
      toast (app, "No archive found. Pass --archive-root or set "
                  "YTDLP_INSTALL_ROOT.");
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
  if (!on_library)
    gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (app->search_bar), FALSE);
}

static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  App *app = user_data;
  ytdl_library_view_set_filter (YTDL_LIBRARY_VIEW (app->library),
                                gtk_editable_get_text (GTK_EDITABLE (entry)));
  update_counts (app);
}

static void
on_rescan_clicked (GtkButton *button, gpointer user_data)
{
  start_scan (user_data);
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

  /* --- the three pages -------------------------------------------- */
  app->stack = adw_view_stack_new ();

  app->library = ytdl_library_view_new ();
  g_signal_connect (app->library, "video-activated",
                    G_CALLBACK (on_video_activated), app);
  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (app->stack),
                                       app->library, "library", "Library",
                                       "view-grid-symbolic");

  GtkWidget *downloads = ytdl_downloads_view_new (app->runner, app->settings);
  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (app->stack), downloads,
                                       "downloads", "Downloads",
                                       "folder-download-symbolic");

  app->health = ytdl_health_view_new (app->settings);
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

  app->search_bar = gtk_search_bar_new ();
  gtk_search_bar_set_child (GTK_SEARCH_BAR (app->search_bar), app->search);
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
  GtkWidget *toolbar = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), app->search_bar);
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

  GtkWidget *toolbar = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar),
                                adw_header_bar_new ());
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

  on_page_changed (G_OBJECT (app->stack), NULL, app);
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
  app.runner = ytdl_runner_new ();
  app.archive_root = archive_root != NULL
                         ? ytdl_resolve_archive_root (archive_root)
                         : ytdl_autodetect_archive_root ();

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
  g_clear_pointer (&app.settings, ytdl_settings_free);
  g_clear_pointer (&app.index, ytdl_index_free);
  g_free (app.archive_root);
  return status;
}
