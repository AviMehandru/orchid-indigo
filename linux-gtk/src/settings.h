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
} YtdlSettings;

YtdlSettings *ytdl_settings_load (void);
void          ytdl_settings_save (const YtdlSettings *s);
void          ytdl_settings_free (YtdlSettings *s);

/* The data root as a real path: the configured value with ~ expanded, or the
 * pipeline's own default. Never NULL. */
char *ytdl_settings_resolved_data_root (const YtdlSettings *s);

G_END_DECLS

#endif /* YTDL_SETTINGS_H */
