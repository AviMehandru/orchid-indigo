#include "health.h"

#include "paths.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* How long a `--version` call gets before it is killed.
 *
 * Not defensiveness: `yt-dlp --version` on a machine whose network is being
 * filtered, and `pwsh --version` with a slow profile on a network drive, can
 * both sit for a long time, and a Health page that hangs on one of them is
 * indistinguishable from a Health page that is broken. A probe that overruns
 * reports the tool as found with no version rather than blocking the pane. */
#define PROBE_TIMEOUT_US (8 * G_USEC_PER_SEC)
#define DEP_TTL_SECS     300

void
ytdl_dependency_free (YtdlDependency *d)
{
  if (d == NULL)
    return;
  g_free (d->path);
  g_free (d->version);
  g_free (d);
}

void
ytdl_installed_file_free (YtdlInstalledFile *f)
{
  if (f == NULL)
    return;
  g_free (f->name);
  g_free (f->path);
  g_free (f);
}

static gint64
now_secs (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

static char *
first_line (const char *s)
{
  if (s == NULL)
    return NULL;
  const char *nl = strchr (s, '\n');
  g_autofree char *line =
      nl != NULL ? g_strndup (s, (gsize) (nl - s)) : g_strdup (s);
  g_strstrip (line);
  return line[0] != '\0' ? g_steal_pointer (&line) : NULL;
}

/* Spawn, poll, and kill on overrun.
 *
 * try-wait rather than a blocking wait: GLib has no timeout on child exit, and
 * every one of these tools writes well under a pipe buffer's worth of output
 * for --version, so nothing can deadlock on an unread pipe while this polls. */
static char *
version_of (const char *exe, const char *const *args)
{
  GPtrArray *argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv, g_strdup (exe));
  for (gsize i = 0; args[i] != NULL; i++)
    g_ptr_array_add (argv, g_strdup (args[i]));
  g_ptr_array_add (argv, NULL);

  GPid pid = 0;
  int out_fd = -1, err_fd = -1;
  gboolean ok = g_spawn_async_with_pipes (
      NULL, (char **) argv->pdata, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
      &pid, NULL, &out_fd, &err_fd, NULL);
  g_ptr_array_unref (argv);
  if (!ok)
    return NULL;

  gint64 deadline = g_get_monotonic_time () + PROBE_TIMEOUT_US;
  gboolean timed_out = FALSE;
  int status = 0;
  for (;;)
    {
      pid_t r = waitpid ((pid_t) pid, &status, WNOHANG);
      if (r == (pid_t) pid)
        break;
      if (r < 0)
        {
          timed_out = TRUE;
          break;
        }
      if (g_get_monotonic_time () >= deadline)
        {
          kill ((pid_t) pid, SIGKILL);
          waitpid ((pid_t) pid, &status, 0);
          timed_out = TRUE;
          break;
        }
      g_usleep (20 * 1000);
    }
  g_spawn_close_pid (pid);

  char *result = NULL;
  if (!timed_out)
    {
      GString *out = g_string_new (NULL);
      GString *err = g_string_new (NULL);
      char buf[4096];
      gssize n;
      while ((n = read (out_fd, buf, sizeof buf)) > 0)
        g_string_append_len (out, buf, n);
      while ((n = read (err_fd, buf, sizeof buf)) > 0)
        g_string_append_len (err, buf, n);
      /* ffmpeg and several others print their banner to stderr. */
      result = first_line (out->len > 0 ? out->str : err->str);
      g_string_free (out, TRUE);
      g_string_free (err, TRUE);
    }

  close (out_fd);
  close (err_fd);
  return result;
}

typedef struct
{
  const char        *name;
  const char        *importance;
  const char *const *args;
  const char        *note;
} DepSpec;

static const char *const ARGS_VERSION[] = { "--version", NULL };
static const char *const ARGS_DASH_VERSION[] = { "-version", NULL };

static const DepSpec DEP_SPECS[] = {
  { "pwsh", "required", ARGS_VERSION,
    "Every stage of the pipeline is a PowerShell 7 script. Without it nothing "
    "downloads." },
  { "yt-dlp", "required", ARGS_VERSION,
    "Does all the actual extraction. run_ytdlp.ps1 runs `yt-dlp -U` on a 24h "
    "throttle." },
  { "ffmpeg", "required", ARGS_DASH_VERSION,
    "Merging, embedding, thumbnails and playback remuxes." },
  { "ffprobe", "recommended", ARGS_DASH_VERSION,
    "Without it, playback falls back to guessing that an .mkv is "
    "WebM-compatible." },
  { "deno", "recommended", ARGS_VERSION,
    "YouTube's JS challenge needs a JS runtime. Its absence usually shows up "
    "as mid-download HTTP 403s rather than an obvious error." },
  { "node", "optional", ARGS_VERSION,
    "Runtime for the PO token provider server." },
  { "python3", "optional", ARGS_VERSION,
    "Runs archive-viewer.py and installs the PO token plugin." },
};

static gpointer
probe_one (gpointer data)
{
  const DepSpec *spec = data;

  char *found = ytdl_which (spec->name);
  if (found == NULL)
    {
      if (g_strcmp0 (spec->name, "pwsh") == 0)
        found = ytdl_find_pwsh ();
      else if (g_strcmp0 (spec->name, "python3") == 0)
        found = ytdl_which ("python");
    }

  YtdlDependency *d = g_new0 (YtdlDependency, 1);
  d->name = spec->name;
  d->importance = spec->importance;
  d->note = spec->note;
  d->found = found != NULL;
  d->version = found != NULL ? version_of (found, spec->args) : NULL;
  d->path = found;
  return d;
}

static GMutex  dep_cache_lock;
static GPtrArray *dep_cache;   /* YtdlDependency*, owned */
static gint64     dep_cache_at;

static YtdlDependency *
dep_copy (const YtdlDependency *s)
{
  YtdlDependency *d = g_new0 (YtdlDependency, 1);
  d->name = s->name;
  d->importance = s->importance;
  d->note = s->note;
  d->found = s->found;
  d->path = g_strdup (s->path);
  d->version = g_strdup (s->version);
  return d;
}

static GPtrArray *
dep_list_copy (GPtrArray *src)
{
  GPtrArray *out =
      g_ptr_array_new_with_free_func ((GDestroyNotify) ytdl_dependency_free);
  for (guint i = 0; i < src->len; i++)
    g_ptr_array_add (out, dep_copy (g_ptr_array_index (src, i)));
  return out;
}

GPtrArray *
ytdl_health_dependencies (gboolean force)
{
  g_mutex_lock (&dep_cache_lock);
  if (!force && dep_cache != NULL &&
      now_secs () - dep_cache_at < DEP_TTL_SECS)
    {
      GPtrArray *cached = dep_list_copy (dep_cache);
      g_mutex_unlock (&dep_cache_lock);
      return cached;
    }
  g_mutex_unlock (&dep_cache_lock);

  /* Probe every tool AT ONCE. These are seven independent subprocess spawns
   * with nothing shared between them, so running them one after another simply
   * added up their latencies -- that loop was the whole cost of the Health
   * page's first paint. */
  GThread *threads[G_N_ELEMENTS (DEP_SPECS)];
  for (gsize i = 0; i < G_N_ELEMENTS (DEP_SPECS); i++)
    threads[i] = g_thread_new ("ytdl-probe", probe_one, (gpointer) &DEP_SPECS[i]);

  /* Joined in spawn order, so the table does not reshuffle itself depending
   * on which tool answered first. */
  GPtrArray *out =
      g_ptr_array_new_with_free_func ((GDestroyNotify) ytdl_dependency_free);
  for (gsize i = 0; i < G_N_ELEMENTS (DEP_SPECS); i++)
    g_ptr_array_add (out, g_thread_join (threads[i]));

  g_mutex_lock (&dep_cache_lock);
  g_clear_pointer (&dep_cache, g_ptr_array_unref);
  dep_cache = dep_list_copy (out);
  dep_cache_at = now_secs ();
  g_mutex_unlock (&dep_cache_lock);

  return out;
}

/* ---------------------------------------------------------------------- */

static YtdlInstalledFile *
stat_file (char *path, const char *name)
{
  YtdlInstalledFile *f = g_new0 (YtdlInstalledFile, 1);
  f->name = g_strdup (name);

  GStatBuf st;
  if (g_stat (path, &st) == 0)
    {
      f->present = TRUE;
      f->size = (guint64) st.st_size;
      f->modified = (gint64) st.st_mtime;
    }
  f->path = path; /* takes ownership */
  return f;
}

GPtrArray *
ytdl_health_installed_files (void)
{
  g_autofree char *s = ytdl_scripts_dir ();
  g_autofree char *c = ytdl_configs_dir ();

  GPtrArray *out = g_ptr_array_new_with_free_func (
      (GDestroyNotify) ytdl_installed_file_free);

  static const char *const scripts[] = {
    "run_ytdlp.ps1", "postprocess.ps1", "ytdl.ps1",
    "pot-provider.ps1", "archive-viewer.py",
  };
  for (gsize i = 0; i < G_N_ELEMENTS (scripts); i++)
    g_ptr_array_add (out, stat_file (g_build_filename (s, scripts[i], NULL),
                                     scripts[i]));
  g_ptr_array_add (
      out, stat_file (g_build_filename (c, "yt-dlp.conf", NULL), "yt-dlp.conf"));
  return out;
}

void
ytdl_config_info_free (YtdlConfigInfo *i)
{
  if (i == NULL)
    return;
  g_free (i->path);
  g_free (i->config_version);
  g_free (i->body);
  g_free (i);
}

YtdlConfigInfo *
ytdl_health_config_info (void)
{
  g_autofree char *dir = ytdl_configs_dir ();
  YtdlConfigInfo *info = g_new0 (YtdlConfigInfo, 1);
  info->path = g_build_filename (dir, "yt-dlp.conf", NULL);
  info->present = g_file_test (info->path, G_FILE_TEST_IS_REGULAR);

  char *body = NULL;
  if (!g_file_get_contents (info->path, &body, NULL, NULL))
    body = g_strdup ("");
  info->body = body;

  g_auto (GStrv) lines = g_strsplit (info->body, "\n", -1);
  for (gsize i = 0; lines[i] != NULL; i++)
    {
      g_autofree char *t = g_strdup (lines[i]);
      g_strstrip (t);
      if (g_str_has_prefix (t, "# CONFIG_VERSION:"))
        {
          g_autofree char *v = g_strdup (t + strlen ("# CONFIG_VERSION:"));
          info->config_version = g_strdup (g_strstrip (v));
        }
      /* Count only real options: a line whose first non-space is "--". */
      const char *p = lines[i];
      while (*p == ' ' || *p == '\t')
        p++;
      if (p[0] == '-' && p[1] == '-')
        info->option_count++;
    }
  return info;
}

void
ytdl_archive_stats_free (YtdlArchiveStats *s)
{
  if (s == NULL)
    return;
  g_free (s->data_root);
  g_free (s->log_dir);
  g_free (s);
}

YtdlArchiveStats *
ytdl_health_archive_stats (const char *data_root, gsize videos, gsize channels,
                           guint64 total_bytes)
{
  YtdlArchiveStats *s = g_new0 (YtdlArchiveStats, 1);
  s->data_root = g_strdup (data_root);
  s->videos = videos;
  s->channels = channels;
  s->total_bytes = total_bytes;
  s->global_manifest_entries = -1;
  s->archive_txt_ids = -1;
  s->log_dir = g_build_filename (data_root, "Archive Logs", "Logs", NULL);

  g_autofree char *global = g_build_filename (data_root, "Youtube Videos",
                                              "global_manifest.json", NULL);
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (json_parser_load_from_file (parser, global, NULL))
    {
      JsonNode *root = json_parser_get_root (parser);
      if (root != NULL && JSON_NODE_HOLDS_ARRAY (root))
        s->global_manifest_entries =
            (gssize) json_array_get_length (json_node_get_array (root));
      else if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
        /* A single-video archive serialises as one object, not a one-element
         * array -- ConvertTo-Json unrolls it. Counting that as zero would be
         * wrong in exactly the case a new user sees. */
        s->global_manifest_entries = 1;
    }

  g_autofree char *archive_txt =
      g_build_filename (s->log_dir, "archive.txt", NULL);
  char *txt = NULL;
  if (g_file_get_contents (archive_txt, &txt, NULL, NULL))
    {
      g_auto (GStrv) lines = g_strsplit (txt, "\n", -1);
      gssize n = 0;
      for (gsize i = 0; lines[i] != NULL; i++)
        {
          g_autofree char *t = g_strdup (lines[i]);
          if (*g_strstrip (t) != '\0')
            n++;
        }
      s->archive_txt_ids = n;
      g_free (txt);
    }

  g_autofree char *history =
      g_build_filename (data_root, "Archive Logs", "Archive History", NULL);
  g_autoptr (GDir) d = g_dir_open (history, 0, NULL);
  if (d != NULL)
    while (g_dir_read_name (d) != NULL)
      s->history_snapshots++;

  return s;
}

/* ---------------------------------------------------------------------- */

void
ytdl_checksum_result_free (YtdlChecksumResult *r)
{
  if (r == NULL)
    return;
  g_clear_pointer (&r->failed, g_ptr_array_unref);
  g_clear_pointer (&r->missing, g_ptr_array_unref);
  g_free (r);
}

static char *
sha256_file (const char *path)
{
  g_autoptr (GChecksum) sum = g_checksum_new (G_CHECKSUM_SHA256);
  FILE *fh = g_fopen (path, "rb");
  if (fh == NULL)
    return NULL;

  guchar *buf = g_malloc (1024 * 1024);
  size_t n;
  while ((n = fread (buf, 1, 1024 * 1024, fh)) > 0)
    g_checksum_update (sum, buf, (gssize) n);
  gboolean bad = ferror (fh) != 0;
  g_free (buf);
  fclose (fh);
  if (bad)
    return NULL;

  return g_strdup (g_checksum_get_string (sum));
}

YtdlChecksumResult *
ytdl_health_verify_checksums (const char *video_dir)
{
  YtdlChecksumResult *r = g_new0 (YtdlChecksumResult, 1);
  r->failed = g_ptr_array_new_with_free_func (g_free);
  r->missing = g_ptr_array_new_with_free_func (g_free);

  g_autofree char *file = g_build_filename (video_dir, "Video metadata",
                                            "checksums.sha256", NULL);
  char *body = NULL;
  if (!g_file_get_contents (file, &body, NULL, NULL))
    return r; /* present stays FALSE */
  r->present = TRUE;

  g_auto (GStrv) lines = g_strsplit (body, "\n", -1);
  g_free (body);

  for (gsize i = 0; lines[i] != NULL; i++)
    {
      g_autofree char *line = g_strdup (lines[i]);
      g_strstrip (line);
      if (*line == '\0')
        continue;

      /* sha256sum format: hash, two spaces, then the path. Splitting on the
       * double space rather than on whitespace matters -- a filename with a
       * space in it is normal here. */
      const char *sep = strstr (line, "  ");
      if (sep == NULL)
        continue;

      g_autofree char *hash = g_strndup (line, (gsize) (sep - line));
      const char *rel = sep + 2;
      r->checked++;

      g_autofree char *path = g_build_filename (video_dir, rel, NULL);
      if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
        {
          g_ptr_array_add (r->missing, g_strdup (rel));
          continue;
        }

      g_autofree char *actual = sha256_file (path);
      if (actual != NULL && g_ascii_strcasecmp (actual, hash) == 0)
        r->ok++;
      else
        g_ptr_array_add (r->failed, g_strdup (rel));
    }
  return r;
}

char *
ytdl_health_log_tail (const char *path, gsize lines)
{
  const goffset MAX_BYTES = 1024 * 1024;

  g_autoptr (GFile) f = g_file_new_for_path (path);
  g_autoptr (GFileInfo) info = g_file_query_info (
      f, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (info == NULL)
    return g_strdup ("");

  goffset size = g_file_info_get_size (info);
  goffset start = size > MAX_BYTES ? size - MAX_BYTES : 0;

  g_autoptr (GFileInputStream) in = g_file_read (f, NULL, NULL);
  if (in == NULL)
    return g_strdup ("");
  if (start > 0 &&
      !g_seekable_seek (G_SEEKABLE (in), start, G_SEEK_SET, NULL, NULL))
    return g_strdup ("");

  GString *buf = g_string_new (NULL);
  char chunk[64 * 1024];
  gssize n;
  while ((n = g_input_stream_read (G_INPUT_STREAM (in), chunk, sizeof chunk,
                                   NULL, NULL)) > 0)
    g_string_append_len (buf, chunk, n);

  /* The first line of the window is dropped when the window did not start at
   * the beginning of the file, because it is almost certainly half a line. */
  const char *text = buf->str;
  if (start > 0)
    {
      const char *nl = strchr (text, '\n');
      text = nl != NULL ? nl + 1 : "";
    }

  g_auto (GStrv) all = g_strsplit (text, "\n", -1);
  gsize total = g_strv_length (all);
  /* g_strsplit on a trailing newline leaves an empty last element; dropping
   * it keeps "last 20 lines" from silently meaning 19 plus a blank. */
  if (total > 0 && all[total - 1][0] == '\0')
    total--;
  gsize from = total > lines ? total - lines : 0;

  GString *out = g_string_new (NULL);
  for (gsize i = from; i < total; i++)
    {
      if (out->len > 0)
        g_string_append_c (out, '\n');
      g_string_append (out, all[i]);
    }
  g_string_free (buf, TRUE);
  return g_string_free (out, FALSE);
}
