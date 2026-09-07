/* Reading the archive that postprocess.ps1 writes.
 *
 * This is a THIRD independent implementation of the layout contract in
 * docs/archive-layout.md -- archive-viewer.py reads it in Python, the Tauri
 * app reads it in Rust, and this reads it in C. That duplication is the price
 * of three standalone apps with no shared engine, and the contract document
 * says plainly how it is paid: "Out-of-repo consumers are expected to keep
 * their own conformance test that builds a fixture tree in this shape and
 * asserts their reader finds it." tests/test_archive.c is this app's.
 *
 * THE ARCHIVE IS READ-ONLY. Nothing in this file creates, moves or modifies
 * anything under Youtube Videos/. postprocess.ps1 writes a checksums.sha256
 * covering every file in a video folder, so a derived file dropped in there
 * makes that manifest stop verifying. Derived state goes to the cache dir.
 *
 * Layout 2 rules that a layout-1 reader gets WRONG, implemented here:
 *   - the media file may be Final Video.<any ext> or Final Audio.<any ext>;
 *     match on base name, or better, read media_file from the manifest
 *   - a folder with NO media file at all is valid, not corrupt
 *   - Pre-merge streams/ must be skipped when choosing "the" video, or you
 *     pick a silent video or a black audio track
 */

#ifndef YTDL_ARCHIVE_H
#define YTDL_ARCHIVE_H

#include <glib.h>

G_BEGIN_DECLS

/* The highest layout version this reader understands. A video written with a
 * higher one is shown on a best-effort basis and flagged, never hidden: an
 * empty library with no explanation is the outcome the contract exists to
 * prevent. */
#define YTDL_SUPPORTED_ARCHIVE_LAYOUT 2

typedef struct
{
  char   *rel;    /* folder-relative, always '/'-separated */
  char   *ext;    /* lowercased, WITH the leading dot; "" if none */
  char   *folder; /* the top-level subfolder, e.g. "Final files"; "" at root */
  guint64 size;
} YtdlFile;

typedef struct
{
  char *key;     /* opaque; see ytdl_key_for */
  char *dir;     /* absolute path to the video folder */
  char *rel;     /* path relative to the archive root */
  char *channel; /* the uploader folder's name */

  /* 0 means manifest.json carried no archive_layout_version, which the
   * contract defines as layout 1 -- not an error and not worth a warning. */
  guint64  layout_version;
  gboolean layout_too_new;

  char  *id;
  char  *title;
  char  *uploader;
  char  *upload_date;
  char  *channel_url;
  char  *original_url;
  char  *download_mode; /* which --mode wrote this folder; may be NULL */
  char  *media_file;    /* manifest's media_file, folder-relative; may be NULL */
  gint64 timestamp;
  gint64 view_count;
  double duration;

  GPtrArray *files; /* YtdlFile*, owned */
} YtdlEntry;

typedef struct
{
  char       *root;
  GPtrArray  *entries;  /* YtdlEntry*, owned */
  GHashTable *by_key;   /* key -> YtdlEntry*, borrowed */
  GPtrArray  *channels; /* char*, owned; uploader folder names, sorted */
} YtdlIndex;

/* Called once per video folder as a scan proceeds. Runs on whichever thread
 * called ytdl_index_scan. */
typedef void (*YtdlScanProgress) (gsize done, gsize total, const char *name,
                                  gpointer user_data);

YtdlIndex *ytdl_index_new (void);
void       ytdl_index_free (YtdlIndex *index);

/* Walk <root>/<Uploader>/<video folder>/ and build the index.
 *
 * Returns FALSE and sets @error only when the root itself cannot be read. An
 * individual unreadable or malformed video folder is not an error: it is
 * indexed from its folder name, because that is the documented fallback and a
 * real state that real runs produce. */
gboolean ytdl_index_scan (YtdlIndex *index, const char *root,
                          YtdlScanProgress progress, gpointer user_data,
                          GError **error);

const YtdlEntry *ytdl_index_get (const YtdlIndex *index, const char *key);

void ytdl_index_stats (const YtdlIndex *index, gsize *videos, gsize *channels,
                       guint64 *bytes);

/* The index into entry->files of the media file, or -1 when the folder has
 * none -- which is ORDINARY. --mode metadata-only, comments-only and
 * subs-only all write a complete folder with no media in it, and so does an
 * interrupted run; the contract says not to try to tell them apart by
 * guessing. download_mode says which. */
gssize ytdl_entry_media_index (const YtdlEntry *entry);

/* The index into entry->files of the best thumbnail, or -1. */
gssize ytdl_entry_thumbnail_index (const YtdlEntry *entry);

/* Resolve a file index to an absolute path, re-checking that the result is
 * inside entry->dir. Returns NULL for an out-of-range index or a path that
 * escapes the folder. This is the ONLY way a path is produced: no caller
 * supplies one. */
char *ytdl_entry_path_for_index (const YtdlEntry *entry, gsize idx);

/* Parse "<uploader> - <YYYYMMDD> - <id> - <title>". Any out parameter may be
 * NULL. Returns FALSE if the name does not carry a date and an id, in which
 * case nothing is written. Exposed because it is the documented fallback for
 * a missing info.json and the conformance test pins it directly. */
gboolean ytdl_parse_folder_name (const char *name, char **uploader,
                                 char **upload_date, char **id, char **title);

G_END_DECLS

#endif /* YTDL_ARCHIVE_H */
