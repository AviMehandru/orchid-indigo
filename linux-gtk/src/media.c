#include "media.h"

#include "paths.h"

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <string.h>

static void
stream_free (gpointer data)
{
  YtdlStream *s = data;
  if (s == NULL)
    return;
  g_free (s->kind);
  g_free (s->codec);
  g_free (s->profile);
  g_free (s->language);
  g_free (s->title);
  g_free (s);
}

static void
chapter_free (gpointer data)
{
  YtdlChapter *c = data;
  if (c == NULL)
    return;
  g_free (c->title);
  g_free (c);
}

void
ytdl_probe_free (YtdlProbe *p)
{
  if (p == NULL)
    return;
  g_free (p->error);
  g_free (p->format);
  g_clear_pointer (&p->streams, g_ptr_array_unref);
  g_clear_pointer (&p->chapters, g_ptr_array_unref);
  g_free (p);
}

static YtdlProbe *
probe_new (void)
{
  YtdlProbe *p = g_new0 (YtdlProbe, 1);
  p->streams = g_ptr_array_new_with_free_func (stream_free);
  p->chapters = g_ptr_array_new_with_free_func (chapter_free);
  return p;
}

static YtdlProbe *
probe_failed (const char *fmt, ...) G_GNUC_PRINTF (1, 2);

static YtdlProbe *
probe_failed (const char *fmt, ...)
{
  YtdlProbe *p = probe_new ();
  va_list args;
  va_start (args, fmt);
  p->error = g_strdup_vprintf (fmt, args);
  va_end (args);
  return p;
}

/* ffprobe reports numbers as JSON strings more often than not ("bit_rate":
 * "128000"), and omits a key entirely rather than sending null when it does
 * not know. Both are normal, neither is an error. */
static const char *
str_of (JsonObject *o, const char *key)
{
  if (o == NULL || !json_object_has_member (o, key))
    return NULL;
  JsonNode *n = json_object_get_member (o, key);
  if (!JSON_NODE_HOLDS_VALUE (n))
    return NULL;
  if (json_node_get_value_type (n) != G_TYPE_STRING)
    return NULL;
  const char *s = json_node_get_string (n);
  return (s != NULL && *s != '\0') ? s : NULL;
}

static gint64
int_of (JsonObject *o, const char *key)
{
  if (o == NULL || !json_object_has_member (o, key))
    return 0;
  JsonNode *n = json_object_get_member (o, key);
  if (!JSON_NODE_HOLDS_VALUE (n))
    return 0;
  GType t = json_node_get_value_type (n);
  if (t == G_TYPE_INT64)
    return json_node_get_int (n);
  if (t == G_TYPE_DOUBLE)
    return (gint64) json_node_get_double (n);
  if (t == G_TYPE_STRING)
    return g_ascii_strtoll (json_node_get_string (n), NULL, 10);
  return 0;
}

static double
dbl_of (JsonObject *o, const char *key)
{
  if (o == NULL || !json_object_has_member (o, key))
    return 0.0;
  JsonNode *n = json_object_get_member (o, key);
  if (!JSON_NODE_HOLDS_VALUE (n))
    return 0.0;
  GType t = json_node_get_value_type (n);
  if (t == G_TYPE_DOUBLE)
    return json_node_get_double (n);
  if (t == G_TYPE_INT64)
    return (double) json_node_get_int (n);
  if (t == G_TYPE_STRING)
    return g_ascii_strtod (json_node_get_string (n), NULL);
  return 0.0;
}

/* "30000/1001" -> 29.97. ffprobe gives frame rates as exact rationals, which
 * is the honest representation and useless to display. */
static double
rational (const char *s)
{
  if (s == NULL)
    return 0.0;
  const char *slash = strchr (s, '/');
  if (slash == NULL)
    return g_ascii_strtod (s, NULL);

  g_autofree char *num = g_strndup (s, (gsize) (slash - s));
  double n = g_ascii_strtod (num, NULL);
  double d = g_ascii_strtod (slash + 1, NULL);
  return d != 0.0 ? n / d : 0.0;
}

static void
read_stream (JsonObject *o, GPtrArray *out)
{
  YtdlStream *s = g_new0 (YtdlStream, 1);
  s->index = (int) int_of (o, "index");
  s->kind = g_strdup (str_of (o, "codec_type"));
  s->codec = g_strdup (str_of (o, "codec_name"));
  s->profile = g_strdup (str_of (o, "profile"));
  s->width = (int) int_of (o, "width");
  s->height = (int) int_of (o, "height");
  s->channels = (int) int_of (o, "channels");
  s->sample_rate = (int) int_of (o, "sample_rate");
  s->bit_rate = int_of (o, "bit_rate");

  /* avg_frame_rate is 0/0 for a still image and for many audio streams. */
  s->fps = rational (str_of (o, "avg_frame_rate"));

  JsonObject *tags = NULL;
  if (json_object_has_member (o, "tags"))
    {
      JsonNode *n = json_object_get_member (o, "tags");
      if (JSON_NODE_HOLDS_OBJECT (n))
        tags = json_node_get_object (n);
    }
  if (tags != NULL)
    {
      const char *lang = str_of (tags, "language");
      /* "und" is Matroska's "undetermined", which is not a language and
       * should not be shown as though it were one. */
      if (lang != NULL && g_strcmp0 (lang, "und") != 0)
        s->language = g_strdup (lang);
      s->title = g_strdup (str_of (tags, "title"));
    }

  JsonObject *disp = NULL;
  if (json_object_has_member (o, "disposition"))
    {
      JsonNode *n = json_object_get_member (o, "disposition");
      if (JSON_NODE_HOLDS_OBJECT (n))
        disp = json_node_get_object (n);
    }
  if (disp != NULL)
    {
      s->is_default = int_of (disp, "default") != 0;
      s->attached_pic = int_of (disp, "attached_pic") != 0;
    }

  if (s->kind == NULL)
    s->kind = g_strdup ("data");
  g_ptr_array_add (out, s);
}

static void
read_chapter (JsonObject *o, GPtrArray *out)
{
  YtdlChapter *c = g_new0 (YtdlChapter, 1);
  c->start = dbl_of (o, "start_time");
  c->end = dbl_of (o, "end_time");

  if (json_object_has_member (o, "tags"))
    {
      JsonNode *n = json_object_get_member (o, "tags");
      if (JSON_NODE_HOLDS_OBJECT (n))
        c->title = g_strdup (str_of (json_node_get_object (n), "title"));
    }
  if (c->title == NULL)
    c->title = g_strdup ("(untitled chapter)");
  g_ptr_array_add (out, c);
}

YtdlProbe *
ytdl_media_probe (const char *path)
{
  if (path == NULL || !g_file_test (path, G_FILE_TEST_IS_REGULAR))
    return probe_failed ("There is no media file in this folder to inspect.");

  g_autofree char *ffprobe = ytdl_which ("ffprobe");
  if (ffprobe == NULL)
    return probe_failed (
        "ffprobe is not installed, so the stream details cannot be read. "
        "Everything else on this page still works, and the file itself is "
        "untouched. Install ffmpeg to get this section back.");

  const char *argv[] = {
    ffprobe, "-v", "quiet", "-print_format", "json",
    "-show_format", "-show_streams", "-show_chapters",
    path, NULL,
  };

  char  *out = NULL;
  char  *err = NULL;
  int    status = 0;
  GError *error = NULL;
  gboolean ok = g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_DEFAULT, NULL,
                              NULL, &out, &err, &status, &error);
  g_free (err);

  if (!ok)
    {
      YtdlProbe *p =
          probe_failed ("Could not run ffprobe: %s", error->message);
      g_clear_error (&error);
      g_free (out);
      return p;
    }
  if (!g_spawn_check_wait_status (status, NULL))
    {
      g_free (out);
      return probe_failed (
          "ffprobe could not read this file. It may be truncated -- an "
          "interrupted run leaves a partial media file behind, which is a "
          "state the archive layout says to tolerate rather than treat as "
          "corruption.");
    }

  g_autoptr (JsonParser) parser = json_parser_new ();
  gboolean parsed = json_parser_load_from_data (parser, out, -1, NULL);
  g_free (out);
  if (!parsed)
    return probe_failed ("ffprobe returned something that is not JSON.");

  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    return probe_failed ("ffprobe returned an unexpected shape.");

  JsonObject *obj = json_node_get_object (root);
  YtdlProbe *p = probe_new ();
  p->ok = TRUE;

  if (json_object_has_member (obj, "format"))
    {
      JsonNode *n = json_object_get_member (obj, "format");
      if (JSON_NODE_HOLDS_OBJECT (n))
        {
          JsonObject *f = json_node_get_object (n);
          p->format = g_strdup (str_of (f, "format_long_name"));
          p->duration = dbl_of (f, "duration");
          p->size = (guint64) int_of (f, "size");
          p->bit_rate = int_of (f, "bit_rate");
        }
    }

  if (json_object_has_member (obj, "streams"))
    {
      JsonNode *n = json_object_get_member (obj, "streams");
      if (JSON_NODE_HOLDS_ARRAY (n))
        {
          JsonArray *a = json_node_get_array (n);
          for (guint i = 0; i < json_array_get_length (a); i++)
            {
              JsonNode *e = json_array_get_element (a, i);
              if (JSON_NODE_HOLDS_OBJECT (e))
                read_stream (json_node_get_object (e), p->streams);
            }
        }
    }

  if (json_object_has_member (obj, "chapters"))
    {
      JsonNode *n = json_object_get_member (obj, "chapters");
      if (JSON_NODE_HOLDS_ARRAY (n))
        {
          JsonArray *a = json_node_get_array (n);
          for (guint i = 0; i < json_array_get_length (a); i++)
            {
              JsonNode *e = json_array_get_element (a, i);
              if (JSON_NODE_HOLDS_OBJECT (e))
                read_chapter (json_node_get_object (e), p->chapters);
            }
        }
    }

  if (p->format == NULL)
    p->format = g_strdup ("unknown container");
  return p;
}

char *
ytdl_probe_summary (const YtdlProbe *p)
{
  if (p == NULL || !p->ok)
    return g_strdup ("");

  const YtdlStream *v = NULL;
  const YtdlStream *a = NULL;
  guint subs = 0;
  for (guint i = 0; i < p->streams->len; i++)
    {
      const YtdlStream *s = g_ptr_array_index (p->streams, i);
      if (g_strcmp0 (s->kind, "video") == 0 && !s->attached_pic && v == NULL)
        v = s;
      else if (g_strcmp0 (s->kind, "audio") == 0 && a == NULL)
        a = s;
      else if (g_strcmp0 (s->kind, "subtitle") == 0)
        subs++;
    }

  GString *out = g_string_new (NULL);
  if (v != NULL && v->height > 0)
    g_string_append_printf (out, "%dp", v->height);

  if (v != NULL || a != NULL)
    {
      if (out->len > 0)
        g_string_append (out, " · ");
      g_string_append_printf (out, "%s", v != NULL && v->codec ? v->codec : "—");
      if (a != NULL && a->codec != NULL)
        g_string_append_printf (out, " / %s", a->codec);
    }

  if (p->duration > 0)
    {
      int total = (int) (p->duration + 0.5);
      if (out->len > 0)
        g_string_append (out, " · ");
      if (total >= 3600)
        g_string_append_printf (out, "%d:%02d:%02d", total / 3600,
                                (total % 3600) / 60, total % 60);
      else
        g_string_append_printf (out, "%d:%02d", total / 60, total % 60);
    }

  if (p->size > 0)
    {
      g_autofree char *size = g_format_size (p->size);
      if (out->len > 0)
        g_string_append (out, " · ");
      g_string_append (out, size);
    }

  if (subs > 0)
    g_string_append_printf (out, " · %u subtitle track%s", subs,
                            subs == 1 ? "" : "s");

  return g_string_free (out, FALSE);
}

gboolean
ytdl_media_playback_available (void)
{
  /* GTK falls back to a do-nothing implementation when no media backend is
   * installed, and that fallback plays everything silently and forever
   * without complaining. Asking the type name is the only reliable way to
   * tell the difference between "will play" and "will show a black
   * rectangle". */
  g_autoptr (GtkMediaStream) probe = gtk_media_file_new ();
  const char *type = G_OBJECT_TYPE_NAME (probe);
  return type != NULL && strstr (type, "Nothing") == NULL;
}
