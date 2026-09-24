/* The persisted preferences.
 *
 * Kept in $XDG_CONFIG_HOME/ytdl-gtk/settings.json -- the STATE dir, not the
 * cache dir. Losing this loses a destination somebody typed once and expects
 * to still be there, which is not the same category as losing an index that
 * one rescan rebuilds.
 *
 * Written through a temp file and renamed. The Rust app deliberately did not
 * bother for its five settings, on the grounds that they are retypeable in a
 * minute. That reasoning was about effort, not risk, and the atomic write is
 * four lines here -- so this one does it, and the queue and history in
 * pipeline.c use the same helper.
 */

#ifndef YTDL_SETTINGS_H
#define YTDL_SETTINGS_H

#include <glib.h>

#include "pipeline.h"

G_BEGIN_DECLS

typedef struct
{
  /* The `ytdl --path` equivalent. NULL or empty means "wherever the pipeline
   * puts it by default", which is the install root -- a real choice, not a
   * missing value. */
  char *data_root;
  /* An explicitly chosen archive root, if autodetection guessed wrong. */
  char *archive_root;
  guint default_workers;

  /* How the Library is ordered. Persisted as the sort key's stable ID
   * string, never as its enum number: inserting a key in the middle would
   * otherwise silently change what every saved setting means. NULL means the
   * default, which library_filter.c decides -- not this file.
   *
   * The FACETS are deliberately not persisted. A sort is a standing
   * preference for how you like to read a list; a facet is a question you
   * asked once, and an app that reopens showing a fifth of the archive with
   * no visible reason is an app that looks like it lost your videos. */
  char    *sort_key;
  gboolean sort_descending;

  /* How YouTube is reached: the Connection group on the Downloads page.
   *
   * Settings, not form fields and not profile fields. A cookie source or a
   * proxy is a fact about you and your network, not about the video you are
   * about to download -- so it is kept once, here, and the runner stamps it
   * onto every run the app starts (ytdl_runner_set_connection). A profile that
   * carried it would mean switching presets could silently sign you out.
   *
   * The cookie SOURCE is stored separately from the two values it chooses
   * between, so flipping the source to "none" and back does not lose a
   * browser profile or a file path somebody typed once. */
  char *cookies_source;  /* NULL / "browser" / "file" */
  char *cookies_browser; /* "firefox", "chrome", ... */
  char *cookies_profile; /* optional; appended as :PROFILE */
  char *cookies_file;
  char *proxy;
  char *limit_rate;
  char *downloader;      /* NULL = native */

  /* Whether a finished queue or a failed run is announced as a desktop
   * notification while the window is in the background. On by default: the
   * point is to hear about the run that failed at 3am, and a setting that has
   * to be found first is one most people would never turn on. See notify.h
   * for what is announced and when. */
  gboolean notify;
} YtdlSettings;

YtdlSettings *ytdl_settings_load (void);
void          ytdl_settings_save (const YtdlSettings *s);
void          ytdl_settings_free (YtdlSettings *s);

/* The connection settings as a YtdlRunOptions with only its five connection
 * fields set -- the shape ytdl_runner_set_connection and the URL preview take.
 * The cookie source decides which of browser/file is emitted; a source whose
 * value is empty emits nothing rather than a flag with no argument. */
YtdlRunOptions *ytdl_settings_connection (const YtdlSettings *s);

/* The data root as a real path: the configured value with ~ expanded, or the
 * pipeline's own default. Never NULL. */
char *ytdl_settings_resolved_data_root (const YtdlSettings *s);

G_END_DECLS

#endif /* YTDL_SETTINGS_H */
