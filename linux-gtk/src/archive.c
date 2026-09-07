#include "archive.h"
#include "paths.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* Extension sets. Lowercased, with the leading dot, so a comparison is one
 * strcmp against the value stored on YtdlFile. */
static const char *const VIDEO_EXTS[] = { ".mkv", ".mp4",  ".webm", ".m4v",
                                          ".mov", ".avi",  ".flv",  ".ts",
                                          NULL };
static const char *const AUDIO_EXTS[] = { ".m4a", ".opus", ".mp3", ".flac",
                                          ".ogg", ".wav",  ".aac", NULL };
static const char *const IMAGE_EXTS[] = { ".png",  ".jpg", ".jpeg",
                                          ".webp", ".gif", ".avif", NULL };
/* --write-subs and --write-auto-subs both land in Subtitles/; the container
 * formats yt-dlp can be asked for are these. */
static const char *const SUB_EXTS[] = { ".vtt", ".srt", ".ass",
                                        ".ssa", ".sub", ".lrc", NULL };

static gboolean
ext_in (const char *const *set, const char *ext)
{
  if (ext == NULL)
    return FALSE;
  for (gsize i = 0; set[i] != NULL; i++)
    if (g_strcmp0 (set[i], ext) == 0)
      return TRUE;
  return FALSE;
}

/* ---------------------------------------------------------------------- */
/* YtdlFile                                                               */
/* ---------------------------------------------------------------------- */

static void
ytdl_file_free (gpointer data)
{
  YtdlFile *f = data;
  if (f == NULL)
    return;
  g_free (f->rel);
  g_free (f->ext);
  g_free (f->folder);
  g_free (f);
}

/* Lowercased extension including the dot, or "" when there is none. Uses the
 * LAST dot of the basename only, so "Final Video.f137.mp4" gives ".mp4". */
static char *
ext_of (const char *rel)
{
  const char *slash = strrchr (rel, '/');
  const char *base = slash != NULL ? slash + 1 : rel;
  const char *dot = strrchr (base, '.');
  if (dot == NULL || dot == base)
    return g_strdup ("");
  return g_ascii_strdown (dot, -1);
}

/* ---------------------------------------------------------------------- */
/* YtdlEntry                                                              */
/* ---------------------------------------------------------------------- */

static void
ytdl_entry_free (gpointer data)
{
  YtdlEntry *e = data;
  if (e == NULL)
    return;
  g_free (e->key);
  g_free (e->dir);
  g_free (e->rel);
  g_free (e->channel);
  g_free (e->id);
  g_free (e->title);
  g_free (e->uploader);
  g_free (e->upload_date);
  g_free (e->channel_url);
  g_free (e->original_url);
  g_free (e->download_mode);
  g_free (e->media_file);
  g_clear_pointer (&e->files, g_ptr_array_unref);
  g_free (e);
}

/* ---------------------------------------------------------------------- */
/* Folder-name fallback                                                   */
/* ---------------------------------------------------------------------- */

static gboolean
is_all_digits (const char *s, gsize len)
{
  if (strlen (s) != len)
    return FALSE;
  for (gsize i = 0; i < len; i++)
    if (!g_ascii_isdigit (s[i]))
      return FALSE;
  return TRUE;
}

static gboolean
looks_like_video_id (const char *s)
{
  if (strlen (s) != 11)
    return FALSE;
  for (const char *p = s; *p; p++)
    if (!g_ascii_isalnum (*p) && *p != '-' && *p != '_')
      return FALSE;
  return TRUE;
}

/* "<uploader> - <YYYYMMDD> - <id> - <title>".
 *
 * Not a left-to-right split with a field limit, because BOTH the uploader and
 * the title routinely contain " - " themselves. The date and the id are the
 * only two fields with a checkable shape, so the parse anchors on finding an
 * 8-digit run immediately followed by an 11-character id and works outwards
 * from there. Everything left of the date is the uploader; everything right
 * of the id is the title, rejoined with its separators intact. */
gboolean
ytdl_parse_folder_name (const char *name, char **uploader, char **upload_date,
                        char **id, char **title)
{
  g_return_val_if_fail (name != NULL, FALSE);

  g_auto (GStrv) parts = g_strsplit (name, " - ", -1);
  gsize n = g_strv_length (parts);
  if (n < 3)
    return FALSE;

  for (gsize i = 1; i + 1 < n; i++)
    {
      if (!is_all_digits (parts[i], 8))
        continue;
      if (!looks_like_video_id (parts[i + 1]))
        continue;

      if (uploader != NULL)
        {
          g_autofree char **head = g_memdup2 (parts, sizeof (char *) * i);
          GString *s = g_string_new (NULL);
          for (gsize k = 0; k < i; k++)
            {
              if (k > 0)
                g_string_append (s, " - ");
              g_string_append (s, head[k]);
            }
          *uploader = g_string_free (s, FALSE);
        }
      if (upload_date != NULL)
        *upload_date = g_strdup (parts[i]);
      if (id != NULL)
        *id = g_strdup (parts[i + 1]);
      if (title != NULL)
        {
          GString *s = g_string_new (NULL);
          for (gsize k = i + 2; k < n; k++)
            {
              if (k > i + 2)
                g_string_append (s, " - ");
              g_string_append (s, parts[k]);
            }
          *title = g_string_free (s, FALSE);
        }
      return TRUE;
    }

  return FALSE;
}

/* ---------------------------------------------------------------------- */
/* JSON helpers -- every one tolerates a missing member, a null, and a       */
/* member of the wrong type, because info.json is yt-dlp's output and the    */
/* shape varies by extractor and by version.                                 */
/* ---------------------------------------------------------------------- */

static JsonObject *
object_member (JsonObject *obj, const char *name)
{
  if (obj == NULL || !json_object_has_member (obj, name))
    return NULL;
  JsonNode *node = json_object_get_member (obj, name);
  return JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
}

static char *
string_member (JsonObject *obj, const char *name)
{
  if (obj == NULL || !json_object_has_member (obj, name))
    return NULL;
  JsonNode *node = json_object_get_member (obj, name);
  if (!JSON_NODE_HOLDS_VALUE (node))
    return NULL;
  if (json_node_get_value_type (node) != G_TYPE_STRING)
    return NULL;
  const char *s = json_node_get_string (node);
  return (s != NULL && *s != '\0') ? g_strdup (s) : NULL;
}

static gint64
int_member (JsonObject *obj, const char *name, gint64 fallback)
{
  if (obj == NULL || !json_object_has_member (obj, name))
    return fallback;
  JsonNode *node = json_object_get_member (obj, name);
  if (!JSON_NODE_HOLDS_VALUE (node))
    return fallback;
  GType t = json_node_get_value_type (node);
  if (t == G_TYPE_INT64)
    return json_node_get_int (node);
  if (t == G_TYPE_DOUBLE)
    return (gint64) json_node_get_double (node);
  return fallback;
}

static double
double_member (JsonObject *obj, const char *name, double fallback)
{
  if (obj == NULL || !json_object_has_member (obj, name))
    return fallback;
  JsonNode *node = json_object_get_member (obj, name);
  if (!JSON_NODE_HOLDS_VALUE (node))
    return fallback;
  GType t = json_node_get_value_type (node);
  if (t == G_TYPE_DOUBLE)
    return json_node_get_double (node);
  if (t == G_TYPE_INT64)
    return (double) json_node_get_int (node);
  return fallback;
}

/* Load a JSON file into a parser the caller owns, or NULL. A malformed
 * info.json is an ordinary state the contract tells consumers to tolerate,
 * so this reports nothing and lets the folder-name fallback take over. */
static JsonParser *
load_json (const char *path)
{
  if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
    return NULL;
  JsonParser *parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, path, NULL))
    {
      g_object_unref (parser);
      return NULL;
    }
  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_object_unref (parser);
      return NULL;
    }
  return parser;
}

/* ---------------------------------------------------------------------- */
/* Listing a video folder                                                 */
/* ---------------------------------------------------------------------- */

static void
list_files (const char *base, const char *dir, const char *prefix,
            const char *folder, GPtrArray *out, int depth)
{
  /* The contract fixes the shape at <video folder>/<subfolder>/<file>. Three
   * levels is slack for a subfolder someone nests one deeper; unbounded
   * recursion into a user-chosen directory is not something to offer. */
  if (depth > 3)
    return;

  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  if (d == NULL)
    return;

  const char *name;
  while ((name = g_dir_read_name (d)) != NULL)
    {
      g_autofree char *full = g_build_filename (dir, name, NULL);
      g_autofree char *rel =
          (prefix != NULL && *prefix != '\0')
              ? g_strdup_printf ("%s/%s", prefix, name)
              : g_strdup (name);

      if (g_file_test (full, G_FILE_TEST_IS_DIR))
        {
          /* At depth 0 the child IS the top-level subfolder, and every file
           * beneath it is attributed to that folder -- which is what makes
           * "skip Pre-merge streams/" a single comparison later. */
          const char *child_folder = (depth == 0) ? name : folder;
          list_files (base, full, rel, child_folder, out, depth + 1);
          continue;
        }

      if (!g_file_test (full, G_FILE_TEST_IS_REGULAR))
        continue;

      GStatBuf st;
      guint64 size = (g_stat (full, &st) == 0) ? (guint64) st.st_size : 0;

      YtdlFile *f = g_new0 (YtdlFile, 1);
      f->rel = g_steal_pointer (&rel);
      f->ext = ext_of (f->rel);
      f->folder = g_strdup (folder != NULL ? folder : "");
      f->size = size;
      g_ptr_array_add (out, f);
    }
}

static int
file_cmp (gconstpointer a, gconstpointer b)
{
  const YtdlFile *fa = *(YtdlFile *const *) a;
  const YtdlFile *fb = *(YtdlFile *const *) b;
  return g_strcmp0 (fa->rel, fb->rel);
}

/* ---------------------------------------------------------------------- */
/* Media selection                                                        */
/* ---------------------------------------------------------------------- */

/* "Final Video.f137.mp4" -- a --keep-video leftover that yt-dlp names with a
 * format-id segment. Excluded even when it sits in Final files/, because
 * picking one gives a silent video or a black audio track. */
static gboolean
has_format_id_segment (const char *rel)
{
  const char *slash = strrchr (rel, '/');
  const char *base = slash != NULL ? slash + 1 : rel;

  const char *last_dot = strrchr (base, '.');
  if (last_dot == NULL)
    return FALSE;

  /* Look at the segment before the extension. */
  g_autofree char *stem = g_strndup (base, (gsize) (last_dot - base));
  const char *prev_dot = strrchr (stem, '.');
  if (prev_dot == NULL)
    return FALSE;

  if (prev_dot[1] != 'f')
    return FALSE;
  for (const char *p = prev_dot + 2; *p; p++)
    if (!g_ascii_isdigit (*p))
      return FALSE;
  return prev_dot[2] != '\0';
}

static gboolean
basename_starts_with (const char *rel, const char *want)
{
  const char *slash = strrchr (rel, '/');
  const char *base = slash != NULL ? slash + 1 : rel;
  return g_str_has_prefix (base, want);
}

gssize
ytdl_entry_media_index (const YtdlEntry *entry)
{
  g_return_val_if_fail (entry != NULL, -1);
  if (entry->files == NULL)
    return -1;

  /* 1. The manifest says so outright. Preferred over globbing at all, per the
   *    contract -- it is the only answer that stays right when --container or
   *    --audio-codec changes the extension. */
  if (entry->media_file != NULL)
    {
      g_autofree char *want =
          g_strdelimit (g_strdup (entry->media_file), "\\", '/');
      for (guint i = 0; i < entry->files->len; i++)
        {
          const YtdlFile *f = g_ptr_array_index (entry->files, i);
          if (g_strcmp0 (f->rel, want) == 0)
            return (gssize) i;
        }
      /* Named but absent: fall through and glob. A manifest naming a file
       * somebody has since deleted should degrade to "no media", not to a
       * broken path. */
    }

  /* 2. Glob by BASE NAME, never by extension. Matching on .mkv was correct
   *    under layout 1 and is a bug under layout 2. Video wins over audio when
   *    a folder somehow holds both. */
  gssize audio_hit = -1;
  for (guint i = 0; i < entry->files->len; i++)
    {
      const YtdlFile *f = g_ptr_array_index (entry->files, i);

      if (g_strcmp0 (f->folder, "Pre-merge streams") == 0)
        continue;
      if (has_format_id_segment (f->rel))
        continue;
      if (!basename_starts_with (f->rel, "Final Video.") &&
          !basename_starts_with (f->rel, "Final Audio."))
        continue;

      if (ext_in (VIDEO_EXTS, f->ext))
        return (gssize) i;
      if (audio_hit < 0 && ext_in (AUDIO_EXTS, f->ext))
        audio_hit = (gssize) i;
    }
  if (audio_hit >= 0)
    return audio_hit;

  /* 3. None. An ORDINARY state: --mode metadata-only, comments-only and
   *    subs-only each write a complete folder with no media in it, and so
   *    does an interrupted run. Not corrupt, and not to be hidden. */
  return -1;
}

gssize
ytdl_entry_thumbnail_index (const YtdlEntry *entry)
{
  g_return_val_if_fail (entry != NULL, -1);
  if (entry->files == NULL)
    return -1;

  gssize best = -1;
  guint64 best_size = 0;
  for (guint i = 0; i < entry->files->len; i++)
    {
      const YtdlFile *f = g_ptr_array_index (entry->files, i);
      if (!ext_in (IMAGE_EXTS, f->ext))
        continue;
      /* Images/ is where postprocess.ps1 puts them; anything elsewhere is a
       * fallback so a hand-reorganised folder still shows something. */
      gboolean preferred = g_strcmp0 (f->folder, "Images") == 0;
      guint64 weight = f->size + (preferred ? G_GUINT64_CONSTANT (1) << 40 : 0);
      if (best < 0 || weight > best_size)
        {
          best = (gssize) i;
          best_size = weight;
        }
    }
  return best;
}

char *
ytdl_entry_path_for_index (const YtdlEntry *entry, gsize idx)
{
  g_return_val_if_fail (entry != NULL, NULL);
  if (entry->files == NULL || idx >= entry->files->len)
    return NULL;

  const YtdlFile *f = g_ptr_array_index (entry->files, idx);
  g_autofree char *joined = g_build_filename (entry->dir, f->rel, NULL);

  /* Re-check containment even though the relative path came from our own
   * listing. It costs one string compare and it is what makes "the caller
   * never supplies a path" an enforced property rather than a convention. */
  g_autofree char *cwd = g_get_current_dir ();
  g_autofree char *canon = g_canonicalize_filename (joined, cwd);
  g_autofree char *base = g_canonicalize_filename (entry->dir, cwd);
  g_autofree char *base_slash = g_strconcat (base, "/", NULL);

  if (!g_str_has_prefix (canon, base_slash))
    return NULL;

  return g_steal_pointer (&canon);
}

/* ---------------------------------------------------------------------- */
/* Building one entry                                                     */
/* ---------------------------------------------------------------------- */

static void
apply_manifest (YtdlEntry *entry, const char *meta_dir)
{
  g_autofree char *path = g_build_filename (meta_dir, "manifest.json", NULL);
  g_autoptr (JsonParser) parser = load_json (path);
  if (parser == NULL)
    return;

  JsonObject *obj = json_node_get_object (json_parser_get_root (parser));

  /* Absent means the video predates versioning, which the contract defines as
   * layout 1. Not an error, and not something to warn about. */
  entry->layout_version =
      (guint64) int_member (obj, "archive_layout_version", 0);
  entry->layout_too_new =
      entry->layout_version > YTDL_SUPPORTED_ARCHIVE_LAYOUT;

  entry->media_file = string_member (obj, "media_file");
  entry->download_mode = string_member (obj, "download_mode");

  if (entry->id == NULL)
    entry->id = string_member (obj, "video_id");
  if (entry->title == NULL)
    entry->title = string_member (obj, "title");
  if (entry->uploader == NULL)
    entry->uploader = string_member (obj, "uploader");
  if (entry->upload_date == NULL)
    entry->upload_date = string_member (obj, "upload_date");
  if (entry->original_url == NULL)
    entry->original_url = string_member (obj, "original_url");
  if (entry->channel_url == NULL)
    entry->channel_url = string_member (obj, "channel_url");

  /* run_settings may legitimately be null -- a video written by a standalone
   * postprocess.ps1 invocation has none. */
  JsonObject *run = object_member (obj, "run_settings");
  if (run != NULL && entry->download_mode == NULL)
    entry->download_mode = string_member (run, "mode");
}

/* The richer fields live in yt-dlp's own info.json. Named "<something>.info.json"
 * rather than a fixed name, so the directory is scanned for the suffix. */
static void
apply_info_json (YtdlEntry *entry, const char *meta_dir)
{
  g_autoptr (GDir) d = g_dir_open (meta_dir, 0, NULL);
  if (d == NULL)
    return;

  g_autofree char *found = NULL;
  const char *name;
  while ((name = g_dir_read_name (d)) != NULL)
    {
      if (g_str_has_suffix (name, ".info.json"))
        {
          found = g_build_filename (meta_dir, name, NULL);
          break;
        }
    }
  if (found == NULL)
    return;

  g_autoptr (JsonParser) parser = load_json (found);
  if (parser == NULL)
    return;

  JsonObject *obj = json_node_get_object (json_parser_get_root (parser));

  entry->duration = double_member (obj, "duration", entry->duration);
  entry->view_count = int_member (obj, "view_count", entry->view_count);
  entry->timestamp = int_member (obj, "timestamp", entry->timestamp);

  char *s;
  if ((s = string_member (obj, "title")) != NULL)
    {
      g_free (entry->title);
      entry->title = s;
    }
  if ((s = string_member (obj, "id")) != NULL)
    {
      g_free (entry->id);
      entry->id = s;
    }
  if ((s = string_member (obj, "upload_date")) != NULL)
    {
      g_free (entry->upload_date);
      entry->upload_date = s;
    }
  /* "uploader" is the display name; "channel" is the fallback some extractors
   * fill instead. */
  if ((s = string_member (obj, "uploader")) != NULL)
    {
      g_free (entry->uploader);
      entry->uploader = s;
    }
  else if ((s = string_member (obj, "channel")) != NULL)
    {
      g_free (entry->uploader);
      entry->uploader = s;
    }
  if ((s = string_member (obj, "channel_url")) != NULL)
    {
      g_free (entry->channel_url);
      entry->channel_url = s;
    }
  if ((s = string_member (obj, "webpage_url")) != NULL)
    {
      g_free (entry->original_url);
      entry->original_url = s;
    }
}

static YtdlEntry *
build_entry (const char *root, const char *channel, const char *folder_name)
{
  YtdlEntry *e = g_new0 (YtdlEntry, 1);
  e->rel = g_strdup_printf ("%s/%s", channel, folder_name);
  e->dir = g_build_filename (root, channel, folder_name, NULL);
  e->channel = g_strdup (channel);
  e->key = ytdl_key_for (e->rel);
  e->files = g_ptr_array_new_with_free_func (ytdl_file_free);
  e->duration = -1.0;
  e->view_count = -1;
  e->timestamp = -1;

  /* The folder name FIRST, so that manifest and info.json are corrections to
   * a value that already exists rather than the only source. A missing or
   * unparseable info.json is a documented, ordinary state, and the folder
   * name is the documented fallback -- so the fallback is simply always
   * applied, and the good sources overwrite it. */
  ytdl_parse_folder_name (folder_name, &e->uploader, &e->upload_date, &e->id,
                          &e->title);
  if (e->title == NULL || *e->title == '\0')
    {
      g_free (e->title);
      e->title = g_strdup (folder_name);
    }
  if (e->uploader == NULL)
    e->uploader = g_strdup (channel);

  g_autofree char *meta_dir = g_build_filename (e->dir, "Video metadata", NULL);
  apply_manifest (e, meta_dir);
  apply_info_json (e, meta_dir);

  list_files (e->dir, e->dir, "", NULL, e->files, 0);
  g_ptr_array_sort (e->files, file_cmp);

  return e;
}

/* ---------------------------------------------------------------------- */
/* The index                                                              */
/* ---------------------------------------------------------------------- */

YtdlIndex *
ytdl_index_new (void)
{
  YtdlIndex *idx = g_new0 (YtdlIndex, 1);
  idx->entries = g_ptr_array_new_with_free_func (ytdl_entry_free);
  idx->by_key = g_hash_table_new (g_str_hash, g_str_equal);
  idx->channels = g_ptr_array_new_with_free_func (g_free);
  return idx;
}

void
ytdl_index_free (YtdlIndex *index)
{
  if (index == NULL)
    return;
  g_free (index->root);
  g_clear_pointer (&index->by_key, g_hash_table_destroy);
  g_clear_pointer (&index->entries, g_ptr_array_unref);
  g_clear_pointer (&index->channels, g_ptr_array_unref);
  g_free (index);
}

static int
str_cmp (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 (*(const char *const *) a, *(const char *const *) b);
}

/* Channel folders, then video folders inside each. "Channel Info" is a
 * channel-level asset directory, not a video, and is skipped by name -- it is
 * also the marker looks_like_channel_dir keys on. */
static GPtrArray *
discover (const char *root, GPtrArray *channels_out)
{
  GPtrArray *found = g_ptr_array_new_with_free_func (g_free);

  g_autoptr (GDir) rd = g_dir_open (root, 0, NULL);
  if (rd == NULL)
    return found;

  g_autoptr (GPtrArray) channels = g_ptr_array_new_with_free_func (g_free);
  const char *cname;
  while ((cname = g_dir_read_name (rd)) != NULL)
    {
      g_autofree char *cdir = g_build_filename (root, cname, NULL);
      if (!g_file_test (cdir, G_FILE_TEST_IS_DIR))
        continue;
      g_ptr_array_add (channels, g_strdup (cname));
    }
  g_ptr_array_sort (channels, str_cmp);

  for (guint i = 0; i < channels->len; i++)
    {
      const char *chan = g_ptr_array_index (channels, i);
      g_autofree char *cdir = g_build_filename (root, chan, NULL);

      g_autoptr (GDir) cd = g_dir_open (cdir, 0, NULL);
      if (cd == NULL)
        continue;

      g_autoptr (GPtrArray) vids = g_ptr_array_new_with_free_func (g_free);
      const char *vname;
      while ((vname = g_dir_read_name (cd)) != NULL)
        {
          if (g_strcmp0 (vname, "Channel Info") == 0)
            continue;
          g_autofree char *vdir = g_build_filename (cdir, vname, NULL);
          if (!g_file_test (vdir, G_FILE_TEST_IS_DIR))
            continue;
          g_ptr_array_add (vids, g_strdup (vname));
        }
      g_ptr_array_sort (vids, str_cmp);

      if (vids->len > 0 && channels_out != NULL)
        g_ptr_array_add (channels_out, g_strdup (chan));

      for (guint k = 0; k < vids->len; k++)
        g_ptr_array_add (found,
                         g_strdup_printf ("%s/%s", chan,
                                          (const char *) g_ptr_array_index (
                                              vids, k)));
    }

  return found;
}

gboolean
ytdl_index_scan (YtdlIndex *index, const char *root, YtdlScanProgress progress,
                 gpointer user_data, GError **error)
{
  g_return_val_if_fail (index != NULL, FALSE);
  g_return_val_if_fail (root != NULL, FALSE);

  if (!g_file_test (root, G_FILE_TEST_IS_DIR))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                   "The archive root %s is not a directory. Point Settings at "
                   "the same path you would pass to `ytdl --path`.",
                   root);
      return FALSE;
    }

  g_free (index->root);
  index->root = g_strdup (root);
  g_ptr_array_set_size (index->entries, 0);
  g_ptr_array_set_size (index->channels, 0);
  g_hash_table_remove_all (index->by_key);

  g_autoptr (GPtrArray) rels = discover (root, index->channels);

  for (guint i = 0; i < rels->len; i++)
    {
      const char *rel = g_ptr_array_index (rels, i);
      const char *slash = strchr (rel, '/');
      if (slash == NULL)
        continue;

      g_autofree char *chan = g_strndup (rel, (gsize) (slash - rel));
      YtdlEntry *e = build_entry (root, chan, slash + 1);

      /* Last writer wins on a key collision, which two identical relative
       * paths cannot produce -- so this is only reachable via a SHA-256
       * truncation collision, and inserting anyway keeps the table and the
       * array consistent. */
      g_hash_table_insert (index->by_key, e->key, e);
      g_ptr_array_add (index->entries, e);

      if (progress != NULL)
        progress (i + 1, rels->len, e->title, user_data);
    }

  return TRUE;
}

const YtdlEntry *
ytdl_index_get (const YtdlIndex *index, const char *key)
{
  g_return_val_if_fail (index != NULL, NULL);
  if (key == NULL)
    return NULL;
  return g_hash_table_lookup (index->by_key, key);
}

void
ytdl_index_stats (const YtdlIndex *index, gsize *videos, gsize *channels,
                  guint64 *bytes)
{
  g_return_if_fail (index != NULL);

  if (videos != NULL)
    *videos = index->entries->len;
  if (channels != NULL)
    *channels = index->channels->len;

  if (bytes != NULL)
    {
      guint64 total = 0;
      for (guint i = 0; i < index->entries->len; i++)
        {
          const YtdlEntry *e = g_ptr_array_index (index->entries, i);
          for (guint k = 0; k < e->files->len; k++)
            {
              const YtdlFile *f = g_ptr_array_index (e->files, k);
              total += f->size;
            }
        }
      *bytes = total;
    }
}

/* ---------------------------------------------------------------------- */
/* info.json                                                              */
/* ---------------------------------------------------------------------- */

YtdlInfo *
ytdl_entry_load_info (const YtdlEntry *entry)
{
  g_return_val_if_fail (entry != NULL, NULL);

  g_autofree char *meta_dir =
      g_build_filename (entry->dir, "Video metadata", NULL);
  g_autoptr (GDir) d = g_dir_open (meta_dir, 0, NULL);
  if (d == NULL)
    return NULL;

  g_autofree char *found = NULL;
  const char *name;
  while ((name = g_dir_read_name (d)) != NULL)
    if (g_str_has_suffix (name, ".info.json"))
      {
        found = g_build_filename (meta_dir, name, NULL);
        break;
      }
  if (found == NULL)
    return NULL;

  JsonParser *parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, found, NULL))
    {
      g_object_unref (parser);
      return NULL;
    }
  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_object_unref (parser);
      return NULL;
    }

  YtdlInfo *info = g_new0 (YtdlInfo, 1);
  info->parser = parser;
  info->root = json_node_get_object (root);
  return info;
}

void
ytdl_info_free (YtdlInfo *info)
{
  if (info == NULL)
    return;
  g_clear_object (&info->parser);
  g_free (info);
}

char *
ytdl_info_string (const YtdlInfo *info, const char *key)
{
  if (info == NULL)
    return NULL;
  return string_member (info->root, key);
}

gint64
ytdl_info_int (const YtdlInfo *info, const char *key)
{
  if (info == NULL)
    return 0;
  return int_member (info->root, key, 0);
}

/* ---------------------------------------------------------------------- */
/* Comments                                                               */
/* ---------------------------------------------------------------------- */

gboolean
ytdl_ext_is_subtitle (const char *ext)
{
  return ext_in (SUB_EXTS, ext);
}

void
ytdl_comment_free (gpointer data)
{
  YtdlComment *c = data;
  if (c == NULL)
    return;
  g_free (c->id);
  g_free (c->text);
  g_free (c->author);
  g_free (c->author_id);
  g_free (c->time_text);
  g_clear_pointer (&c->replies, g_ptr_array_unref);
  g_free (c);
}

static gboolean
bool_member (JsonObject *obj, const char *key)
{
  if (obj == NULL || !json_object_has_member (obj, key))
    return FALSE;
  JsonNode *n = json_object_get_member (obj, key);
  if (!JSON_NODE_HOLDS_VALUE (n))
    return FALSE;
  if (json_node_get_value_type (n) != G_TYPE_BOOLEAN)
    return FALSE;
  return json_node_get_boolean (n);
}

static YtdlComment *
comment_from (JsonObject *o)
{
  YtdlComment *c = g_new0 (YtdlComment, 1);
  c->id = string_member (o, "id");
  c->text = string_member (o, "text");
  c->author = string_member (o, "author");
  c->author_id = string_member (o, "author_id");
  c->time_text = string_member (o, "_time_text");
  c->timestamp = int_member (o, "timestamp", -1);
  c->like_count = int_member (o, "like_count", -1);
  c->is_favorited = bool_member (o, "is_favorited");
  c->author_is_uploader = bool_member (o, "author_is_uploader");
  c->is_pinned = bool_member (o, "is_pinned");
  c->replies = g_ptr_array_new_with_free_func (ytdl_comment_free);

  if (c->id == NULL)
    c->id = g_strdup ("");
  if (c->text == NULL)
    c->text = g_strdup ("");
  if (c->author == NULL)
    c->author = g_strdup ("(unknown)");
  return c;
}

static int
reply_cmp (gconstpointer a, gconstpointer b)
{
  const YtdlComment *x = *(YtdlComment *const *) a;
  const YtdlComment *y = *(YtdlComment *const *) b;
  gint64 xa = x->timestamp < 0 ? 0 : x->timestamp;
  gint64 ya = y->timestamp < 0 ? 0 : y->timestamp;
  return (xa > ya) - (xa < ya);
}

/* Pinned first, then most-liked. The same order YouTube itself shows, which
 * matters because a transcript of a comment section in arbitrary order is a
 * different document from the one people actually read. */
static int
top_cmp (gconstpointer a, gconstpointer b)
{
  const YtdlComment *x = *(YtdlComment *const *) a;
  const YtdlComment *y = *(YtdlComment *const *) b;
  if (x->is_pinned != y->is_pinned)
    return y->is_pinned - x->is_pinned;
  gint64 xl = x->like_count < 0 ? 0 : x->like_count;
  gint64 yl = y->like_count < 0 ? 0 : y->like_count;
  return (yl > xl) - (yl < xl);
}

GPtrArray *
ytdl_thread_comments (JsonArray *raw)
{
  GPtrArray *tops = g_ptr_array_new_with_free_func (ytdl_comment_free);
  if (raw == NULL)
    return tops;

  /* id -> index into tops. Borrowed keys: each points at its comment's own
   * id, which outlives the table. */
  g_autoptr (GHashTable) index = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GPtrArray) orphan_parents = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) orphan_comments = g_ptr_array_new (); /* moved out */

  for (guint i = 0; i < json_array_get_length (raw); i++)
    {
      JsonNode *n = json_array_get_element (raw, i);
      if (!JSON_NODE_HOLDS_OBJECT (n))
        continue;
      JsonObject *o = json_node_get_object (n);

      g_autofree char *parent = string_member (o, "parent");
      YtdlComment *c = comment_from (o);

      if (parent == NULL || *parent == '\0' || g_strcmp0 (parent, "root") == 0)
        {
          g_hash_table_insert (index, c->id, GUINT_TO_POINTER (tops->len));
          g_ptr_array_add (tops, c);
        }
      else
        {
          g_ptr_array_add (orphan_parents, g_steal_pointer (&parent));
          g_ptr_array_add (orphan_comments, c);
        }
    }

  for (guint i = 0; i < orphan_comments->len; i++)
    {
      const char *parent = g_ptr_array_index (orphan_parents, i);
      YtdlComment *c = g_ptr_array_index (orphan_comments, i);

      gpointer slot = NULL;
      gboolean found = g_hash_table_lookup_extended (index, parent, NULL, &slot);
      if (!found)
        {
          /* yt-dlp's reply ids are "<parent>.<reply>", so the parent id is
           * recoverable even when the parent field itself is unhelpful. */
          const char *dot = strchr (parent, '.');
          if (dot != NULL)
            {
              g_autofree char *head = g_strndup (parent, (gsize) (dot - parent));
              found = g_hash_table_lookup_extended (index, head, NULL, &slot);
            }
        }

      if (found)
        {
          YtdlComment *top = g_ptr_array_index (tops, GPOINTER_TO_UINT (slot));
          g_ptr_array_add (top->replies, c);
        }
      else
        {
          /* A reply whose parent is genuinely absent -- a deleted comment, or
           * a truncated fetch -- is shown at top level rather than dropped.
           * Silently losing archived text would be the worse failure. */
          g_ptr_array_add (tops, c);
        }
    }

  for (guint i = 0; i < tops->len; i++)
    {
      YtdlComment *t = g_ptr_array_index (tops, i);
      g_ptr_array_sort (t->replies, reply_cmp);
    }
  g_ptr_array_sort (tops, top_cmp);
  return tops;
}

GPtrArray *
ytdl_info_comments (const YtdlInfo *info)
{
  if (info == NULL || !json_object_has_member (info->root, "comments"))
    return g_ptr_array_new_with_free_func (ytdl_comment_free);

  JsonNode *n = json_object_get_member (info->root, "comments");
  if (!JSON_NODE_HOLDS_ARRAY (n))
    return g_ptr_array_new_with_free_func (ytdl_comment_free);

  return ytdl_thread_comments (json_node_get_array (n));
}

/* ---------------------------------------------------------------------- */
/* Transcript                                                             */
/* ---------------------------------------------------------------------- */

void
ytdl_cue_free (gpointer data)
{
  YtdlCue *c = data;
  if (c == NULL)
    return;
  g_free (c->text);
  g_free (c);
}

/* [hh:]mm:ss[.,]mmm -- leading run of digits, colons and a decimal mark. */
static gboolean
parse_ts (const char *text, double *out)
{
  if (text == NULL)
    return FALSE;
  while (*text == ' ' || *text == '\t')
    text++;

  gsize end = 0;
  while (text[end] != '\0' &&
         (g_ascii_isdigit (text[end]) || text[end] == ':' ||
          text[end] == '.' || text[end] == ','))
    end++;
  if (end == 0)
    return FALSE;

  g_autofree char *t = g_strndup (text, end);
  char *mark = strpbrk (t, ".,");
  g_autofree char *frac = mark != NULL ? g_strdup (mark + 1) : g_strdup ("");
  if (mark != NULL)
    *mark = '\0';

  g_auto (GStrv) parts = g_strsplit (t, ":", -1);
  gsize n = g_strv_length (parts);
  double h = 0, m = 0, s = 0;
  if (n == 3)
    {
      h = g_ascii_strtod (parts[0], NULL);
      m = g_ascii_strtod (parts[1], NULL);
      s = g_ascii_strtod (parts[2], NULL);
    }
  else if (n == 2)
    {
      m = g_ascii_strtod (parts[0], NULL);
      s = g_ascii_strtod (parts[1], NULL);
    }
  else
    {
      return FALSE;
    }

  /* ".5" is 500ms, not 5ms -- pad on the RIGHT to three digits. */
  double ms = 0;
  if (*frac != '\0')
    {
      char padded[4] = { '0', '0', '0', '\0' };
      for (gsize i = 0; i < 3 && frac[i] != '\0'; i++)
        padded[i] = frac[i];
      ms = g_ascii_strtod (padded, NULL);
    }

  *out = h * 3600.0 + m * 60.0 + s + ms / 1000.0;
  return TRUE;
}

/* Removes <c>, </c>, <v Name>, and the per-word <00:00:01.234> karaoke
 * timestamps YouTube's ASR emits. A hand-rolled scanner rather than a regex
 * dependency: the grammar is "everything between < and >". */
static char *
strip_inline_tags (const char *s)
{
  GString *out = g_string_sized_new (strlen (s));
  gsize depth = 0;
  for (const char *p = s; *p; p++)
    {
      if (*p == '<')
        depth++;
      else if (*p == '>')
        {
          if (depth > 0)
            depth--;
        }
      else if (depth == 0)
        g_string_append_c (out, *p);
    }
  return g_string_free (out, FALSE);
}

static char *
unescape_entities (const char *s)
{
  static const char *const from[] = { "&amp;", "&lt;",  "&gt;",
                                      "&quot;", "&#39;", "&nbsp;", NULL };
  static const char *const to[] = { "&", "<", ">", "\"", "'", " ", NULL };

  char *cur = g_strdup (s);
  for (gsize i = 0; from[i] != NULL; i++)
    {
      g_auto (GStrv) split = g_strsplit (cur, from[i], -1);
      char *next = g_strjoinv (to[i], split);
      g_free (cur);
      cur = next;
    }
  return cur;
}

static char *
collapse_ws (const char *s)
{
  g_auto (GStrv) parts = g_strsplit_set (s, " \t\n\r", -1);
  GString *out = g_string_new (NULL);
  for (gsize i = 0; parts[i] != NULL; i++)
    {
      if (parts[i][0] == '\0')
        continue;
      if (out->len > 0)
        g_string_append_c (out, ' ');
      g_string_append (out, parts[i]);
    }
  return g_string_free (out, FALSE);
}

GPtrArray *
ytdl_parse_subtitle_cues (const char *path)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (ytdl_cue_free);

  char *raw = NULL;
  if (!g_file_get_contents (path, &raw, NULL, NULL))
    return out;

  /* Normalise line endings first: a .vtt written on Windows would otherwise
   * never match the blank-line block separator. */
  g_autofree char *normalised = NULL;
  {
    g_auto (GStrv) a = g_strsplit (raw, "\r\n", -1);
    g_autofree char *j = g_strjoinv ("\n", a);
    g_auto (GStrv) b = g_strsplit (j, "\r", -1);
    normalised = g_strjoinv ("\n", b);
  }
  g_free (raw);

  g_autoptr (GPtrArray) cues = g_ptr_array_new_with_free_func (ytdl_cue_free);
  g_auto (GStrv) blocks = g_strsplit (normalised, "\n\n", -1);

  for (gsize b = 0; blocks[b] != NULL; b++)
    {
      g_auto (GStrv) all = g_strsplit (blocks[b], "\n", -1);

      /* Borrowed pointers to the non-blank lines. Compacting `all` in place
       * would leave duplicates in its tail for g_strfreev to free twice. */
      g_autoptr (GPtrArray) lines = g_ptr_array_new ();
      for (gsize i = 0; all[i] != NULL; i++)
        {
          g_autofree char *probe = g_strdup (all[i]);
          if (*g_strstrip (probe) != '\0')
            g_ptr_array_add (lines, all[i]);
        }
      if (lines->len == 0)
        continue;

      gssize time_idx = -1;
      for (guint i = 0; i < lines->len; i++)
        if (strstr (g_ptr_array_index (lines, i), "-->") != NULL)
          {
            time_idx = (gssize) i;
            break;
          }
      if (time_idx < 0)
        continue;

      const char *timeline = g_ptr_array_index (lines, time_idx);
      const char *arrow = strstr (timeline, "-->");
      g_autofree char *left = g_strndup (timeline, (gsize) (arrow - timeline));

      double start = 0, end = 0;
      if (!parse_ts (left, &start))
        continue;
      if (!parse_ts (arrow + 3, &end))
        end = start + 3.0;

      GString *body = g_string_new (NULL);
      for (guint i = (guint) time_idx + 1; i < lines->len; i++)
        {
          if (body->len > 0)
            g_string_append_c (body, ' ');
          g_string_append (body, g_ptr_array_index (lines, i));
        }

      g_autofree char *stripped = strip_inline_tags (body->str);
      g_string_free (body, TRUE);
      g_autofree char *unescaped = unescape_entities (stripped);
      char *text = collapse_ws (unescaped);

      if (*text == '\0')
        {
          g_free (text);
          continue;
        }

      YtdlCue *c = g_new0 (YtdlCue, 1);
      c->start = start;
      c->end = end;
      c->text = text;
      g_ptr_array_add (cues, c);
    }

  /* The rolling-display collapse.
   *
   * The comparison is against the previous cue's FULL text, not against what
   * was last emitted. That distinction is the whole algorithm:
   *
   *   cue 1  "the quick brown fox"
   *   cue 2  "the quick brown fox jumps over"
   *   cue 3  "the quick brown fox jumps over the lazy dog"
   *
   * Emitting the tail of cue 2 puts "jumps over" in the output. Comparing cue
   * 3 against THAT finds no common prefix, so cue 3 is emitted whole and the
   * duplication the collapse exists to remove comes straight back on the third
   * line. Keeping last_full separate from the output is what makes the third
   * line "the lazy dog".
   */
  const char *last_full = NULL;
  for (guint i = 0; i < cues->len; i++)
    {
      YtdlCue *cue = g_ptr_array_index (cues, i);
      YtdlCue *emitted = out->len > 0 ? g_ptr_array_index (out, out->len - 1) : NULL;

      if (last_full != NULL && emitted != NULL)
        {
          if (g_strcmp0 (cue->text, last_full) == 0)
            {
              emitted->end = MAX (emitted->end, cue->end);
              continue;
            }
          /* The 12-character floor keeps a genuinely repeated short line
           * ("Yeah." then "Yeah. Right.") from being chopped into fragments. */
          if (g_str_has_prefix (cue->text, last_full) &&
              g_utf8_strlen (last_full, -1) > 12)
            {
              g_autofree char *tail = g_strdup (cue->text + strlen (last_full));
              g_strstrip (tail);
              if (*tail == '\0')
                {
                  emitted->end = MAX (emitted->end, cue->end);
                }
              else
                {
                  YtdlCue *c = g_new0 (YtdlCue, 1);
                  c->start = cue->start;
                  c->end = cue->end;
                  c->text = g_strdup (tail);
                  g_ptr_array_add (out, c);
                }
              last_full = cue->text;
              continue;
            }
        }

      YtdlCue *c = g_new0 (YtdlCue, 1);
      c->start = cue->start;
      c->end = cue->end;
      c->text = g_strdup (cue->text);
      g_ptr_array_add (out, c);
      last_full = cue->text;
    }

  return out;
}

gboolean
ytdl_subtitle_is_auto (const char *path)
{
  char *raw = NULL;
  gsize len = 0;
  if (!g_file_get_contents (path, &raw, &len, NULL))
    return FALSE;

  /* Only the head matters, and these files can be large. */
  g_autofree char *head = g_strndup (raw, MIN (len, 8000));
  g_free (raw);

  return strstr (head, "<c.") != NULL || strstr (head, "<c>") != NULL ||
         strstr (head, "align:start position:") != NULL;
}
