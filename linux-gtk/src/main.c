/* ytdl-gtk -- a native GTK4 front end for the yt-dlp archival pipeline.
 *
 * WHAT THIS IS NOT: a reimplementation of the pipeline. Downloads are started
 * by handing a command line to the installed ytdl.ps1, exactly as a terminal
 * would. Nothing here knows what run_ytdlp.ps1 or postprocess.ps1 do with it.
 *
 * WHAT THIS DOES OWN: reading the archive. That is a third independent
 * implementation of docs/archive-layout.md, and the price of three standalone
 * apps sharing no engine. tests/test_archive.c is what keeps it honest.
 *
 * No Rust, no webview, no bundled runtime -- GTK4, GLib and json-glib, all of
 * which a GNOME desktop already has.
 */

#include <gtk/gtk.h>

#include "archive.h"
#include "library_view.h"
#include "paths.h"

typedef struct
{
  GtkWidget *window;
  GtkWidget *library;
  GtkWidget *search;
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
      ytdl_library_view_set_index (YTDL_LIBRARY_VIEW (app->library), NULL);
      g_clear_pointer (&res->index, ytdl_index_free);
    }
  else
    {
      /* Clear the view's borrowed pointers BEFORE freeing the old index. */
      ytdl_library_view_set_index (YTDL_LIBRARY_VIEW (app->library), NULL);
      g_clear_pointer (&app->index, ytdl_index_free);
      app->index = g_steal_pointer (&res->index);
      ytdl_library_view_set_index (YTDL_LIBRARY_VIEW (app->library),
                                   app->index);
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
      set_status (app,
                  "Could not find an archive. Looked for 'Youtube Videos/"
                  "Complete Archive' under the usual locations. Pass "
                  "--archive-root, or set YTDLP_INSTALL_ROOT.");
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
/* UI                                                                     */
/* ---------------------------------------------------------------------- */

static void
on_rescan_clicked (GtkButton *button, gpointer user_data)
{
  start_scan (user_data);
}

static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  App *app = user_data;
  ytdl_library_view_set_filter (YTDL_LIBRARY_VIEW (app->library),
                                gtk_editable_get_text (GTK_EDITABLE (entry)));
  update_counts (app);
}

static GtkWidget *
make_placeholder (const char *title, const char *body)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (box, TRUE);

  GtkWidget *h = gtk_label_new (title);
  gtk_widget_add_css_class (h, "title-2");
  gtk_box_append (GTK_BOX (box), h);

  GtkWidget *p = gtk_label_new (body);
  gtk_label_set_justify (GTK_LABEL (p), GTK_JUSTIFY_CENTER);
  gtk_label_set_wrap (GTK_LABEL (p), TRUE);
  gtk_widget_set_size_request (p, 420, -1);
  gtk_widget_add_css_class (p, "dim-label");
  gtk_box_append (GTK_BOX (box), p);

  return box;
}

static void
load_css (void)
{
  static const char *css =
      ".ytdl-thumb {"
      "  background: alpha(currentColor, 0.08);"
      "  border-radius: 8px;"
      "}"
      ".card {"
      "  border-radius: 10px;"
      "  padding: 6px;"
      "}";

  g_autoptr (GtkCssProvider) provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (provider, css);
  gtk_style_context_add_provider_for_display (
      gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void
on_activate (GtkApplication *gtkapp, gpointer user_data)
{
  App *app = user_data;

  load_css ();

  app->window = gtk_application_window_new (gtkapp);
  gtk_window_set_title (GTK_WINDOW (app->window), "yt-dlp Archive");
  gtk_window_set_default_size (GTK_WINDOW (app->window), 1100, 720);

  GtkWidget *header = gtk_header_bar_new ();
  gtk_window_set_titlebar (GTK_WINDOW (app->window), header);

  app->rescan = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_widget_set_tooltip_text (app->rescan, "Rescan the archive");
  g_signal_connect (app->rescan, "clicked", G_CALLBACK (on_rescan_clicked),
                    app);
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), app->rescan);

  app->spinner = gtk_spinner_new ();
  gtk_widget_set_visible (app->spinner, FALSE);
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), app->spinner);

  app->search = gtk_search_entry_new ();
  gtk_widget_set_size_request (app->search, 260, -1);
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (app->search),
                                         "Search title, channel or id");
  g_signal_connect (app->search, "search-changed",
                    G_CALLBACK (on_search_changed), app);
  gtk_header_bar_pack_end (GTK_HEADER_BAR (header), app->search);

  app->library = ytdl_library_view_new ();

  GtkWidget *stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (stack),
                                 GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_titled (GTK_STACK (stack), app->library, "library", "Library");

  /* Named as not built rather than left out, so the window says what it is
   * rather than looking finished and doing nothing. */
  gtk_stack_add_titled (
      GTK_STACK (stack),
      make_placeholder ("Downloads",
                        "Not built yet. This pane will drive the installed "
                        "ytdl.ps1 the same way a terminal does, with the "
                        "queue, live progress and run history."),
      "downloads", "Downloads");
  gtk_stack_add_titled (
      GTK_STACK (stack),
      make_placeholder ("Health",
                        "Not built yet. This pane will report the detected "
                        "dependencies, the config version and the archive "
                        "checksums."),
      "health", "Health");

  GtkWidget *switcher = gtk_stack_switcher_new ();
  gtk_stack_switcher_set_stack (GTK_STACK_SWITCHER (switcher),
                                GTK_STACK (stack));
  gtk_header_bar_set_title_widget (GTK_HEADER_BAR (header), switcher);

  app->status = gtk_label_new ("Starting…");
  gtk_label_set_xalign (GTK_LABEL (app->status), 0.0f);
  gtk_label_set_wrap (GTK_LABEL (app->status), TRUE);
  gtk_widget_add_css_class (app->status, "dim-label");
  gtk_widget_add_css_class (app->status, "caption");
  gtk_widget_set_margin_start (app->status, 12);
  gtk_widget_set_margin_end (app->status, 12);
  gtk_widget_set_margin_top (app->status, 6);
  gtk_widget_set_margin_bottom (app->status, 6);

  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (root), stack);
  gtk_box_append (GTK_BOX (root),
                  gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (root), app->status);
  gtk_widget_set_vexpand (stack, TRUE);

  gtk_window_set_child (GTK_WINDOW (app->window), root);
  gtk_window_present (GTK_WINDOW (app->window));

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
  app.archive_root = archive_root != NULL
                         ? ytdl_resolve_archive_root (archive_root)
                         : ytdl_autodetect_archive_root ();

  if (archive_root != NULL && app.archive_root == NULL)
    g_printerr ("No 'Complete Archive' found under %s -- starting empty.\n",
                archive_root);

  g_autoptr (GtkApplication) gtkapp =
      gtk_application_new ("io.github.avimehandru.YtdlGtk",
                           G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (gtkapp, "activate", G_CALLBACK (on_activate), &app);

  /* argv is deliberately not forwarded: GtkApplication would try to parse
   * --archive-root itself and refuse it. */
  int status = g_application_run (G_APPLICATION (gtkapp), 0, NULL);

  g_clear_pointer (&app.index, ytdl_index_free);
  g_free (app.archive_root);
  return status;
}
