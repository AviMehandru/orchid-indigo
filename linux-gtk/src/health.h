/* Health, config, and integrity.
 *
 * Everything here answers a question you would otherwise answer by running a
 * command and reading a file: is pwsh installed, is yt-dlp current, which
 * CONFIG_VERSION is installed, do this video's checksums still verify.
 *
 * DELIBERATELY READ-ONLY. Nothing here installs, updates or repairs anything.
 * `yt-dlp -U` is run by run_ytdlp.ps1's own once-per-24h dependency check, and
 * a second updater racing it from a GUI is exactly the kind of shared-state
 * collision the pipeline spent a release removing.
 */

#ifndef YTDL_HEALTH_H
#define YTDL_HEALTH_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct
{
  const char *name;
  const char *importance; /* required | recommended | optional */
  const char *note;
  gboolean    found;
  char       *path;
  char       *version; /* NULL when the probe timed out or printed nothing */
} YtdlDependency;

/* The probe result, cached.
 *
 * Seven `--version` calls cost seconds -- pwsh and yt-dlp are the better part
 * of one each on their own -- and the Health pane is opened far more often
 * than a toolchain changes. @force is the Refresh button: it skips the cache,
 * which is what someone who has just installed a missing dependency expects
 * that button to do.
 *
 * BLOCKS for up to the probe timeout. Call it off the UI thread. */
GPtrArray *ytdl_health_dependencies (gboolean force); /* YtdlDependency*, owned */

typedef struct
{
  char    *name;
  char    *path;
  gboolean present;
  guint64  size;
  gint64   modified; /* unix seconds, 0 when absent */
} YtdlInstalledFile;

/* What is INSTALLED, never what is in a checkout.
 *
 * The repo holds the sources; the installer copies them to their runtime
 * locations. Editing a file in a clone has no effect on a live install until
 * it is copied over, which is exactly the confusion this list settles. */
GPtrArray *ytdl_health_installed_files (void); /* YtdlInstalledFile*, owned */

typedef struct
{
  char    *path;
  gboolean present;
  char    *config_version; /* from "# CONFIG_VERSION:" */
  char    *body;
  gsize    option_count;
} YtdlConfigInfo;

/* CONFIG_VERSION is recorded in manifest.json and download.log by both
 * scripts, so the number shown here is the one those files will carry. */
YtdlConfigInfo *ytdl_health_config_info (void);
void            ytdl_config_info_free (YtdlConfigInfo *info);

typedef struct
{
  char      *data_root;
  gsize      videos;
  gsize      channels;
  guint64    total_bytes;
  gssize     global_manifest_entries; /* -1 when unreadable */
  gssize     archive_txt_ids;         /* -1 when unreadable */
  char      *log_dir;
  gsize      history_snapshots;
} YtdlArchiveStats;

YtdlArchiveStats *ytdl_health_archive_stats (const char *data_root,
                                             gsize videos, gsize channels,
                                             guint64 total_bytes);
void ytdl_archive_stats_free (YtdlArchiveStats *s);

typedef struct
{
  gboolean   present;
  gsize      checked;
  gsize      ok;
  GPtrArray *failed;  /* char* */
  GPtrArray *missing; /* char* */
} YtdlChecksumResult;

/* Verify a video folder against its own checksums.sha256.
 *
 * Standard sha256sum format ("<hash>  <relative/path>"), written by
 * postprocess.ps1 over every file in the folder EXCEPT
 * Logs/video_postprocessing.log -- excluded because it is still being appended
 * to when the hashes are computed, and a manifest that always reports one
 * failure teaches you to ignore its failures. Its absence is therefore not a
 * failure here either. */
YtdlChecksumResult *ytdl_health_verify_checksums (const char *video_dir);
void                ytdl_checksum_result_free (YtdlChecksumResult *r);

/* The last @lines lines of a file, WITHOUT reading the file.
 *
 * download.log is appended to by every run and never rotated by the pipeline,
 * so on a machine that has been archiving for a while it is the largest thing
 * this pane touches. Reading it whole to keep the last 300 lines made the
 * pane's cost grow with the age of the install, for a panel whose content is
 * fixed-size. This seeks to the last megabyte and works backwards. */
char *ytdl_health_log_tail (const char *path, gsize lines);

void ytdl_dependency_free (YtdlDependency *d);
void ytdl_installed_file_free (YtdlInstalledFile *f);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlDependency, ytdl_dependency_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlInstalledFile, ytdl_installed_file_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlConfigInfo, ytdl_config_info_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlArchiveStats, ytdl_archive_stats_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlChecksumResult, ytdl_checksum_result_free)

G_END_DECLS

#endif /* YTDL_HEALTH_H */
