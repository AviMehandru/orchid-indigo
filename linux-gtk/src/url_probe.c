#include "url_probe.h"

#include "paths.h"
#include "pipeline.h"

#include <string.h>

/* ---------------------------------------------------------------------- */
/* Freeing                                                                */
/* ---------------------------------------------------------------------- */

static void
format_free (gpointer data)
{
  YtdlUrlProbeFormat *f = data;
  if (f == NULL)
    return;
  g_free (f->format_id);
  g_free (f->ext);
  g_free (f->vcodec);
  g_free (f->acodec);
  g_free (f->video_family);
  g_free (f->audio_family);
  g_free (f->dynamic_range);
  g_free (f->format_note);
  g_free (f);
}

static void
entry_free (gpointer data)
{
  YtdlUrlProbeEntry *e = data;
  if (e == NULL)
    return;
  g_free (e->id);
  g_free (e->title);
  g_free (e->uploader);
  g_free (e->url);
  g_free (e->thumbnail);
  g_free (e);
}

void
ytdl_url_probe_free (YtdlUrlProbe *p)
{
  if (p == NULL)
    return;
  g_free (p->kind);
  g_free (p->url);
  g_free (p->id);
  g_free (p->title);
  g_free (p->uploader);
  g_free (p->channel);
  g_free (p->channel_url);
  g_free (p->extractor);
  g_free (p->thumbnail);
  g_free (p->description);
  g_free (p->upload_date);
  g_free (p->live_status);
  g_free (p->availability);
  g_free (p->webpage_url);
  g_free (p->formats_from_id);
  g_free (p->formats_from_title);
  g_free (p->pot_reason);
  g_free (p->pot_note);
  g_clear_pointer (&p->entries, g_ptr_array_unref);
  g_clear_pointer (&p->formats, g_ptr_array_unref);
  g_clear_pointer (&p->heights, g_array_unref);
  g_strfreev (p->video_codecs);
  g_strfreev (p->audio_codecs);
  g_strfreev (p->containers);
  g_strfreev (p->subtitle_langs);
  g_free (p);
}

static YtdlUrlProbe *
probe_new (void)
{
  YtdlUrlProbe *p = g_new0 (YtdlUrlProbe, 1);
  p->entries = g_ptr_array_new_with_free_func (entry_free);
  p->formats = g_ptr_array_new_with_free_func (format_free);
  p->heights = g_array_new (FALSE, FALSE, sizeof (int));
  p->kind = g_strdup ("video");
  return p;
}

/* ---------------------------------------------------------------------- */
/* JSON readers                                                           */
/* ---------------------------------------------------------------------- */
/* Same shape as archive.c's: every one tolerates a missing member, a null,
 * and the wrong type. yt-dlp's info dict varies by extractor, by video and by
 * version, and a reader that requires a field to be present will eventually
 * meet a video that does not have it. */

static char *
str_member (JsonObject *obj, const char *name)
{
  if (obj == NULL || !json_object_has_member (obj, name))
    return NULL;
  JsonNode *node = json_object_get_member (obj, name);
  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return NULL;
  if (!JSON_NODE_HOLDS_VALUE (node)
      || json_node_get_value_type (node) != G_TYPE_STRING)
    return NULL;
  const char *s = json_node_get_string (node);
  if (s == NULL || *s == '\0')
    return NULL;
  return g_strdup (s);
}

static gint64
int_member (JsonObject *obj, const char *name)
{
  if (obj == NULL || !json_object_has_member (obj, name))
    return 0;
  JsonNode *node = json_object_get_member (obj, name);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
    return 0;
  GType t = json_node_get_value_type (node);
  if (t == G_TYPE_INT64)
    return json_node_get_int (node);
  if (t == G_TYPE_DOUBLE)
    return (gint64) json_node_get_double (node);
  return 0;
}

static double
dbl_member (JsonObject *obj, const char *name)
{
  if (obj == NULL || !json_object_has_member (obj, name))
    return 0.0;
  JsonNode *node = json_object_get_member (obj, name);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
    return 0.0;
  GType t = json_node_get_value_type (node);
  if (t == G_TYPE_DOUBLE)
    return json_node_get_double (node);
  if (t == G_TYPE_INT64)
    return (double) json_node_get_int (node);
  return 0.0;
}

static gboolean
bool_member (JsonObject *obj, const char *name)
{
  if (obj == NULL || !json_object_has_member (obj, name))
    return FALSE;
  JsonNode *node = json_object_get_member (obj, name);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
    return FALSE;
  if (json_node_get_value_type (node) != G_TYPE_BOOLEAN)
    return FALSE;
  return json_node_get_boolean (node);
}

static JsonArray *
array_member (JsonObject *obj, const char *name)
{
  if (obj == NULL || !json_object_has_member (obj, name))
    return NULL;
  JsonNode *node = json_object_get_member (obj, name);
  if (node == NULL || !JSON_NODE_HOLDS_ARRAY (node))
    return NULL;
  return json_node_get_array (node);
}

static JsonObject *
object_member (JsonObject *obj, const char *name)
{
  if (obj == NULL || !json_object_has_member (obj, name))
    return NULL;
  JsonNode *node = json_object_get_member (obj, name);
  if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
    return NULL;
  return json_node_get_object (node);
}

static GStrv
strv_member (JsonObject *obj, const char *name)
{
  JsonArray *arr = array_member (obj, name);
  GPtrArray *out = g_ptr_array_new ();
  if (arr != NULL)
    for (guint i = 0; i < json_array_get_length (arr); i++)
      {
        JsonNode *n = json_array_get_element (arr, i);
        if (n != NULL && JSON_NODE_HOLDS_VALUE (n)
            && json_node_get_value_type (n) == G_TYPE_STRING)
          g_ptr_array_add (out, g_strdup (json_node_get_string (n)));
      }
  g_ptr_array_add (out, NULL);
  return (GStrv) g_ptr_array_free (out, FALSE);
}

/* The member names of an object, in document order -- which is how yt-dlp
 * reports available subtitle languages: `"subtitles": {"en": [...], ...}`. */
static GStrv
object_keys (JsonObject *obj, const char *name)
{
  JsonObject *inner = object_member (obj, name);
  GPtrArray *out = g_ptr_array_new ();
  if (inner != NULL)
    {
      g_autoptr (GList) keys = json_object_get_members (inner);
      for (GList *l = keys; l != NULL; l = l->next)
        g_ptr_array_add (out, g_strdup ((const char *) l->data));
    }
  g_ptr_array_add (out, NULL);
  return (GStrv) g_ptr_array_free (out, FALSE);
}

static JsonParser *
parse_text (const char *json, GError **error)
{
  JsonParser *parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, json, -1, error))
    {
      g_object_unref (parser);
      return NULL;
    }
  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "the probe did not return a JSON object");
      g_object_unref (parser);
      return NULL;
    }
  return parser;
}

/* ---------------------------------------------------------------------- */
/* Codec families                                                         */
/* ---------------------------------------------------------------------- */

const char *
ytdl_url_probe_video_family (const char *vcodec)
{
  if (vcodec == NULL || *vcodec == '\0' || g_strcmp0 (vcodec, "none") == 0)
    return NULL;
  g_autofree char *c = g_ascii_strdown (vcodec, -1);
  if (g_str_has_prefix (c, "avc1") || g_str_has_prefix (c, "h264"))
    return "avc1";
  /* "vp09" FIRST and then "vp9": yt-dlp emits the four-character form with
   * the zero, and a codebase that tests only for "vp9" reports VP9 as having
   * no --codec spelling at all -- which shows up as a Video codec list that
   * is missing its most common entry, on every video. */
  if (g_str_has_prefix (c, "vp09") || g_str_has_prefix (c, "vp9"))
    return "vp9";
  /* With a ZERO. yt-dlp never emits "av1". */
  if (g_str_has_prefix (c, "av01"))
    return "av01";
  return NULL;
}

const char *
ytdl_url_probe_audio_family (const char *acodec)
{
  if (acodec == NULL || *acodec == '\0' || g_strcmp0 (acodec, "none") == 0)
    return NULL;
  g_autofree char *c = g_ascii_strdown (acodec, -1);
  if (g_str_has_prefix (c, "opus"))
    return "opus";
  /* "mp4a" is what a user calls AAC. */
  if (g_str_has_prefix (c, "mp4a") || g_str_has_prefix (c, "aac"))
    return "aac";
  if (g_str_has_prefix (c, "mp3"))
    return "mp3";
  if (g_str_has_prefix (c, "flac"))
    return "flac";
  return NULL;
}

static gboolean
has_video_stream (const YtdlUrlProbeFormat *f)
{
  return f->vcodec != NULL && g_strcmp0 (f->vcodec, "none") != 0;
}

static gboolean
has_audio_stream (const YtdlUrlProbeFormat *f)
{
  return f->acodec != NULL && g_strcmp0 (f->acodec, "none") != 0;
}

static gboolean
family_offered (const YtdlUrlProbe *p, const char *family, gboolean video)
{
  for (guint i = 0; i < p->formats->len; i++)
    {
      const YtdlUrlProbeFormat *f = g_ptr_array_index (p->formats, i);
      const char *have = video ? f->video_family : f->audio_family;
      if (g_strcmp0 (have, family) == 0)
        return TRUE;
    }
  return FALSE;
}

static int
cmp_int_desc (gconstpointer a, gconstpointer b)
{
  int x = *(const int *) a;
  int y = *(const int *) b;
  return (x < y) - (x > y);
}

/* Everything that is computed FROM the format list rather than read out of
 * it. This is the function that must agree, value for value, with
 * Get-FormatSummary in the pipeline's probe.ps1 -- and with Probe.swift and
 * Probe.cs. When the pipeline's own document is what was parsed, this is not
 * called at all: the derived arrays come from the document, because the
 * pipeline is the authority. It runs only for the fallback path, and the
 * tests hold the two to one fixture. */
static void
derive_lists (YtdlUrlProbe *p)
{
  /* "Does this format carry video" and "which --codec value is it" are two
   * different questions, and conflating them loses resolutions. A VP8
   * rendition has a real height the user can ask for and no --codec spelling
   * at all, so the heights come from the first question and the codec lists
   * from the second. */
  g_array_set_size (p->heights, 0);
  p->has_video = FALSE;
  p->has_audio = FALSE;

  for (guint i = 0; i < p->formats->len; i++)
    {
      const YtdlUrlProbeFormat *f = g_ptr_array_index (p->formats, i);
      if (has_audio_stream (f))
        p->has_audio = TRUE;
      if (!has_video_stream (f))
        continue;
      p->has_video = TRUE;
      if (f->height <= 0)
        continue;

      gboolean seen = FALSE;
      for (guint j = 0; j < p->heights->len; j++)
        if (g_array_index (p->heights, int, j) == f->height)
          {
            seen = TRUE;
            break;
          }
      if (!seen)
        g_array_append_val (p->heights, f->height);
    }
  g_array_sort (p->heights, cmp_int_desc);

  /* Canonical order, not offer order. yt-dlp's format ordering varies with
   * client and with its own sorting changes, and a dropdown whose entries
   * reshuffle between two probes of the same video looks broken. */
  static const char *const v_order[] = { "avc1", "vp9", "av01", NULL };
  static const char *const a_order[] = { "opus", "aac", "mp3", "flac", NULL };

  GPtrArray *vc = g_ptr_array_new ();
  for (gsize i = 0; v_order[i] != NULL; i++)
    if (family_offered (p, v_order[i], TRUE))
      g_ptr_array_add (vc, g_strdup (v_order[i]));
  g_ptr_array_add (vc, NULL);
  g_strfreev (p->video_codecs);
  p->video_codecs = (GStrv) g_ptr_array_free (vc, FALSE);

  GPtrArray *ac = g_ptr_array_new ();
  for (gsize i = 0; a_order[i] != NULL; i++)
    if (family_offered (p, a_order[i], FALSE))
      g_ptr_array_add (ac, g_strdup (a_order[i]));
  g_ptr_array_add (ac, NULL);
  g_strfreev (p->audio_codecs);
  p->audio_codecs = (GStrv) g_ptr_array_free (ac, FALSE);

  /* Which --container values a merge could actually produce.
   *
   * mkv unconditionally: Matroska carries every codec pair YouTube serves,
   * which is why it is the pipeline's default and its archival choice. The
   * other two are real constraints -- yt-dlp cannot mux Opus into mp4 or AAC
   * into webm, and asking it to produces a re-encode or a failed merge
   * depending on version. Offering one that cannot be made is the specific
   * mistake this whole module exists to stop the form making. */
  gboolean mp4_v = g_strv_contains ((const char *const *) p->video_codecs, "avc1")
                   || g_strv_contains ((const char *const *) p->video_codecs, "av01");
  gboolean mp4_a = g_strv_contains ((const char *const *) p->audio_codecs, "aac");
  gboolean webm_v = g_strv_contains ((const char *const *) p->video_codecs, "vp9")
                    || g_strv_contains ((const char *const *) p->video_codecs, "av01");
  gboolean webm_a = g_strv_contains ((const char *const *) p->audio_codecs, "opus");

  GPtrArray *ct = g_ptr_array_new ();
  g_ptr_array_add (ct, g_strdup ("mkv"));
  if (mp4_v && mp4_a)
    g_ptr_array_add (ct, g_strdup ("mp4"));
  if (webm_v && webm_a)
    g_ptr_array_add (ct, g_strdup ("webm"));
  g_ptr_array_add (ct, NULL);
  g_strfreev (p->containers);
  p->containers = (GStrv) g_ptr_array_free (ct, FALSE);
}

/* ---------------------------------------------------------------------- */
/* Reading a format object                                                */
/* ---------------------------------------------------------------------- */

/* @derived is TRUE when the object came from the pipeline's document, which
 * already carries video_family/audio_family; FALSE for a raw yt-dlp format,
 * where they are computed here. */
static YtdlUrlProbeFormat *
read_format (JsonObject *obj, gboolean derived)
{
  YtdlUrlProbeFormat *f = g_new0 (YtdlUrlProbeFormat, 1);
  f->format_id = str_member (obj, "format_id");
  f->ext = str_member (obj, "ext");
  f->vcodec = str_member (obj, "vcodec");
  f->acodec = str_member (obj, "acodec");
  f->height = (int) int_member (obj, "height");
  f->width = (int) int_member (obj, "width");
  f->fps = dbl_member (obj, "fps");
  f->tbr = dbl_member (obj, "tbr");
  f->filesize = int_member (obj, "filesize");
  f->filesize_approx = int_member (obj, "filesize_approx");
  f->dynamic_range = str_member (obj, "dynamic_range");
  f->format_note = str_member (obj, "format_note");

  if (derived)
    {
      f->video_family = str_member (obj, "video_family");
      f->audio_family = str_member (obj, "audio_family");
    }
  else
    {
      f->video_family = g_strdup (ytdl_url_probe_video_family (f->vcodec));
      f->audio_family = g_strdup (ytdl_url_probe_audio_family (f->acodec));
    }
  return f;
}

static void
read_formats (YtdlUrlProbe *p, JsonObject *obj, gboolean derived)
{
  JsonArray *arr = array_member (obj, "formats");
  if (arr == NULL)
    return;
  for (guint i = 0; i < json_array_get_length (arr); i++)
    {
      JsonNode *n = json_array_get_element (arr, i);
      if (n == NULL || !JSON_NODE_HOLDS_OBJECT (n))
        continue;
      JsonObject *fo = json_node_get_object (n);

      /* Storyboards. yt-dlp's .mhtml pseudo-formats carry no streams and are
       * not a rendition of anything; left in, each becomes a phantom row and
       * a phantom height. The pipeline drops them too, so this only ever
       * fires on the fallback path -- but it fires there identically, which
       * is the point. */
      g_autofree char *ext = str_member (fo, "ext");
      if (g_strcmp0 (ext, "mhtml") == 0)
        continue;

      YtdlUrlProbeFormat *f = read_format (fo, derived);
      if (!has_video_stream (f) && !has_audio_stream (f))
        {
          format_free (f);
          continue;
        }
      g_ptr_array_add (p->formats, f);
    }
}

/* ---------------------------------------------------------------------- */
/* Parsing a probe-contract document                                      */
/* ---------------------------------------------------------------------- */

YtdlUrlProbe *
ytdl_url_probe_parse (const char *json, GError **error)
{
  if (json == NULL || *json == '\0')
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "the probe returned nothing");
      return NULL;
    }

  g_autoptr (JsonParser) parser = parse_text (json, error);
  if (parser == NULL)
    return NULL;
  JsonObject *obj = json_node_get_object (json_parser_get_root (parser));

  YtdlUrlProbe *p = probe_new ();
  p->probe_version = (int) int_member (obj, "probe_version");
  g_free (p->kind);
  p->kind = str_member (obj, "kind");
  if (p->kind == NULL)
    p->kind = g_strdup ("video");

  p->url = str_member (obj, "url");
  p->id = str_member (obj, "id");
  p->title = str_member (obj, "title");
  p->uploader = str_member (obj, "uploader");
  p->channel = str_member (obj, "channel");
  p->channel_url = str_member (obj, "channel_url");
  p->extractor = str_member (obj, "extractor");
  p->thumbnail = str_member (obj, "thumbnail");
  p->description = str_member (obj, "description");
  p->upload_date = str_member (obj, "upload_date");
  p->live_status = str_member (obj, "live_status");
  p->availability = str_member (obj, "availability");
  p->webpage_url = str_member (obj, "webpage_url");
  p->formats_from_id = str_member (obj, "formats_from_id");
  p->formats_from_title = str_member (obj, "formats_from_title");

  p->duration = dbl_member (obj, "duration");
  p->view_count = int_member (obj, "view_count");
  p->like_count = int_member (obj, "like_count");
  p->comment_count = int_member (obj, "comment_count");
  p->age_limit = (int) int_member (obj, "age_limit");
  p->entry_count = (int) int_member (obj, "entry_count");
  p->playlist_count = (int) int_member (obj, "playlist_count");
  p->entries_truncated = bool_member (obj, "entries_truncated");

  JsonArray *entries = array_member (obj, "entries");
  if (entries != NULL)
    for (guint i = 0; i < json_array_get_length (entries); i++)
      {
        JsonNode *n = json_array_get_element (entries, i);
        if (n == NULL || !JSON_NODE_HOLDS_OBJECT (n))
          continue;
        JsonObject *eo = json_node_get_object (n);
        YtdlUrlProbeEntry *e = g_new0 (YtdlUrlProbeEntry, 1);
        e->index = (int) int_member (eo, "index");
        /* A document from a pipeline that somehow omitted the index still
         * produces a usable list rather than a list of zeroes that would all
         * write the same --items value. */
        if (e->index <= 0)
          e->index = (int) i + 1;
        e->id = str_member (eo, "id");
        e->title = str_member (eo, "title");
        e->uploader = str_member (eo, "uploader");
        e->url = str_member (eo, "url");
        e->thumbnail = str_member (eo, "thumbnail");
        e->duration = dbl_member (eo, "duration");
        g_ptr_array_add (p->entries, e);
      }

  read_formats (p, obj, TRUE);

  /* The derived lists are READ, not recomputed. The pipeline is the
   * authority on them: it made them under the PO token provider a real
   * download will use, and recomputing here would mean this app could
   * disagree with the command it is about to run. */
  p->video_codecs = strv_member (obj, "video_codecs");
  p->audio_codecs = strv_member (obj, "audio_codecs");
  p->containers = strv_member (obj, "containers");
  p->subtitle_langs = strv_member (obj, "subtitle_langs");
  p->has_video = bool_member (obj, "has_video");
  p->has_audio = bool_member (obj, "has_audio");

  JsonArray *heights = array_member (obj, "heights");
  if (heights != NULL)
    for (guint i = 0; i < json_array_get_length (heights); i++)
      {
        JsonNode *n = json_array_get_element (heights, i);
        if (n == NULL || !JSON_NODE_HOLDS_VALUE (n))
          continue;
        int h = (int) json_node_get_int (n);
        if (h > 0)
          g_array_append_val (p->heights, h);
      }

  JsonObject *pot = object_member (obj, "pot");
  if (pot != NULL)
    {
      p->pot_healthy = bool_member (pot, "healthy");
      p->pot_reason = str_member (pot, "reason");
      p->pot_note = str_member (pot, "note");
    }

  /* A document with no containers at all is one from a pipeline whose
   * derivation failed, or a truncated read. Falling back to the local
   * derivation is better than a Container row with nothing in it. */
  if (p->containers == NULL || p->containers[0] == NULL)
    derive_lists (p);

  return p;
}

/* ---------------------------------------------------------------------- */
/* The fallback: deriving from raw yt-dlp output                          */
/* ---------------------------------------------------------------------- */

YtdlUrlProbe *
ytdl_url_probe_from_ytdlp (const char *flat, const char *full, GError **error)
{
  if (full == NULL || *full == '\0')
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "yt-dlp returned nothing");
      return NULL;
    }

  g_autoptr (JsonParser) full_parser = parse_text (full, error);
  if (full_parser == NULL)
    return NULL;
  JsonObject *fo = json_node_get_object (json_parser_get_root (full_parser));

  YtdlUrlProbe *p = probe_new ();
  p->probe_version = YTDL_SUPPORTED_URL_PROBE_VERSION;
  p->from_fallback = TRUE;

  /* The fallback never brings up the PO token provider -- doing so would mean
   * reimplementing pot-provider.ps1 in C, which is exactly the duplication
   * this app exists to avoid. So it says so, rather than leaving the user to
   * wonder why the format table is thinner than the one they saw yesterday. */
  p->pot_healthy = FALSE;
  p->pot_reason = g_strdup ("this app read yt-dlp directly, without the "
                            "pipeline's PO token provider");
  p->pot_note = g_strdup ("No PO token provider: a real download may see "
                          "formats this list does not show.");

  g_autoptr (JsonParser) flat_parser = NULL;
  JsonObject *lo = NULL;
  if (flat != NULL && *flat != '\0')
    {
      flat_parser = parse_text (flat, NULL);
      if (flat_parser != NULL)
        lo = json_node_get_object (json_parser_get_root (flat_parser));
    }

  g_autofree char *type = lo != NULL ? str_member (lo, "_type") : NULL;
  gboolean is_playlist =
      g_strcmp0 (type, "playlist") == 0 || g_strcmp0 (type, "multi_video") == 0;

  g_free (p->kind);
  p->kind = g_strdup (is_playlist ? "playlist" : "video");

  if (is_playlist)
    {
      p->id = str_member (lo, "id");
      p->title = str_member (lo, "title");
      p->uploader = str_member (lo, "uploader");
      if (p->uploader == NULL)
        p->uploader = str_member (lo, "channel");
      p->channel = str_member (lo, "channel");
      p->extractor = str_member (lo, "extractor_key");
      p->playlist_count = (int) int_member (lo, "playlist_count");

      JsonArray *arr = array_member (lo, "entries");
      if (arr != NULL)
        for (guint i = 0; i < json_array_get_length (arr); i++)
          {
            JsonNode *n = json_array_get_element (arr, i);
            if (n == NULL || !JSON_NODE_HOLDS_OBJECT (n))
              continue;
            JsonObject *eo = json_node_get_object (n);
            YtdlUrlProbeEntry *e = g_new0 (YtdlUrlProbeEntry, 1);
            e->index = (int) p->entries->len + 1;
            e->id = str_member (eo, "id");
            e->title = str_member (eo, "title");
            e->uploader = str_member (eo, "uploader");
            if (e->uploader == NULL)
              e->uploader = str_member (eo, "channel");
            e->url = str_member (eo, "url");
            if (e->url == NULL)
              e->url = str_member (eo, "webpage_url");
            e->duration = dbl_member (eo, "duration");
            g_ptr_array_add (p->entries, e);
          }
      p->entry_count = (int) p->entries->len;
      p->formats_from_id = str_member (fo, "id");
      p->formats_from_title = str_member (fo, "title");
      p->thumbnail = str_member (fo, "thumbnail");
      p->duration = dbl_member (fo, "duration");
    }
  else
    {
      p->id = str_member (fo, "id");
      p->title = str_member (fo, "title");
      p->uploader = str_member (fo, "uploader");
      if (p->uploader == NULL)
        p->uploader = str_member (fo, "channel");
      p->channel = str_member (fo, "channel");
      p->channel_url = str_member (fo, "channel_url");
      p->extractor = str_member (fo, "extractor_key");
      p->duration = dbl_member (fo, "duration");
      p->upload_date = str_member (fo, "upload_date");
      p->view_count = int_member (fo, "view_count");
      p->like_count = int_member (fo, "like_count");
      p->comment_count = int_member (fo, "comment_count");
      p->live_status = str_member (fo, "live_status");
      p->availability = str_member (fo, "availability");
      p->age_limit = (int) int_member (fo, "age_limit");
      p->thumbnail = str_member (fo, "thumbnail");
      p->description = str_member (fo, "description");
      p->webpage_url = str_member (fo, "webpage_url");
      p->subtitle_langs = object_keys (fo, "subtitles");
      p->entry_count = 1;
    }

  read_formats (p, fo, FALSE);
  derive_lists (p);
  return p;
}

/* ---------------------------------------------------------------------- */
/* Using a probe                                                          */
/* ---------------------------------------------------------------------- */

GArray *
ytdl_url_probe_heights_for_codec (const YtdlUrlProbe *p, const char *family)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (int));
  if (p == NULL)
    return out;

  gboolean all = family == NULL || *family == '\0'
                 || g_strcmp0 (family, "any") == 0;

  for (guint i = 0; i < p->formats->len; i++)
    {
      const YtdlUrlProbeFormat *f = g_ptr_array_index (p->formats, i);
      if (!has_video_stream (f) || f->height <= 0)
        continue;
      if (!all && g_strcmp0 (f->video_family, family) != 0)
        continue;

      gboolean seen = FALSE;
      for (guint j = 0; j < out->len; j++)
        if (g_array_index (out, int, j) == f->height)
          {
            seen = TRUE;
            break;
          }
      if (!seen)
        g_array_append_val (out, f->height);
    }
  g_array_sort (out, cmp_int_desc);
  return out;
}

static int
cmp_int_asc (gconstpointer a, gconstpointer b)
{
  int x = *(const int *) a;
  int y = *(const int *) b;
  return (x > y) - (x < y);
}

char *
ytdl_url_probe_items_range (const int *indices, gsize n)
{
  if (indices == NULL || n == 0)
    return g_strdup ("");

  g_autoptr (GArray) sorted = g_array_sized_new (FALSE, FALSE, sizeof (int),
                                                 (guint) n);
  for (gsize i = 0; i < n; i++)
    if (indices[i] > 0)
      g_array_append_val (sorted, indices[i]);
  if (sorted->len == 0)
    return g_strdup ("");
  g_array_sort (sorted, cmp_int_asc);

  GString *s = g_string_new (NULL);
  guint i = 0;
  while (i < sorted->len)
    {
      int start = g_array_index (sorted, int, i);
      int end = start;
      guint j = i + 1;
      /* Duplicates are absorbed rather than rejected: two ticked rows cannot
       * produce the same index, but a range parsed back in from a saved
       * profile can overlap itself, and "1-3,2-4" is a legal thing for a
       * human to have typed. */
      while (j < sorted->len)
        {
          int next = g_array_index (sorted, int, j);
          if (next == end || next == end + 1)
            {
              end = next;
              j++;
            }
          else
            break;
        }
      if (s->len > 0)
        g_string_append_c (s, ',');
      if (start == end)
        g_string_append_printf (s, "%d", start);
      else if (end == start + 1)
        /* "1,2" rather than "1-2": the same length, and a two-element range
         * written as a range reads like it might be open-ended. */
        g_string_append_printf (s, "%d,%d", start, end);
      else
        g_string_append_printf (s, "%d-%d", start, end);
      i = j;
    }
  return g_string_free (s, FALSE);
}

GArray *
ytdl_url_probe_parse_items_range (const char *spec)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (int));
  if (spec == NULL || *spec == '\0')
    return out;

  g_auto (GStrv) parts = g_strsplit (spec, ",", -1);
  for (gsize i = 0; parts[i] != NULL; i++)
    {
      g_strstrip (parts[i]);
      if (*parts[i] == '\0')
        continue;

      const char *dash = strchr (parts[i], '-');
      if (dash == NULL)
        {
          int v = atoi (parts[i]);
          if (v > 0)
            g_array_append_val (out, v);
          continue;
        }

      /* yt-dlp's own open-ended forms -- "5-" and "-10" -- are deliberately
       * NOT expanded to a guessed bound. A tick list built from a guess would
       * show a selection the pipeline may not agree with, and ytdl.ps1 is the
       * validator here, not this. They parse to nothing and the range stays
       * in the text field as typed. */
      if (dash == parts[i] || *(dash + 1) == '\0')
        continue;

      g_autofree char *lo_s = g_strndup (parts[i], (gsize) (dash - parts[i]));
      int lo = atoi (lo_s);
      int hi = atoi (dash + 1);
      if (lo <= 0 || hi < lo)
        continue;
      /* A bound on how much a malformed range can allocate. 100,000 is far
       * past any real playlist and far short of a memory problem. */
      if (hi - lo > 100000)
        continue;
      for (int v = lo; v <= hi; v++)
        g_array_append_val (out, v);
    }
  return out;
}

/* ---------------------------------------------------------------------- */
/* Running one                                                            */
/* ---------------------------------------------------------------------- */

typedef struct
{
  char    *url;
  char    *items;
  gboolean no_pot;
  guint    pot_port;
  GStrv    extra_args;
  GStrv    connection_args; /* ytdl flags: --cookies*, --proxy */
} ProbeRequest;

static void
probe_request_free (gpointer data)
{
  ProbeRequest *r = data;
  if (r == NULL)
    return;
  g_free (r->url);
  g_free (r->items);
  g_strfreev (r->extra_args);
  g_strfreev (r->connection_args);
  g_free (r);
}

/* One synchronous child, stdout and stderr captured separately.
 *
 * Separately, not merged, and that is the whole reason a probe can be told
 * from a failure at all: the contract is one JSON document on stdout and
 * everything conversational on stderr, so merging them would put the
 * pipeline's own "[pot] ..." notes inside the string about to be parsed. */
static gboolean
run_capture (const char *const *argv, GCancellable *cancellable, char **out,
             char **err, int *status, GError **error)
{
  g_autoptr (GSubprocess) proc =
      g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                   | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                         error);
  if (proc == NULL)
    return FALSE;
  if (!g_subprocess_communicate_utf8 (proc, NULL, cancellable, out, err, error))
    {
      /* A cancelled probe leaves a yt-dlp mid-request. Nothing downstream
       * cares -- it has written no file and touched no manifest -- but the
       * child must not outlive the window. */
      g_subprocess_force_exit (proc);
      return FALSE;
    }
  *status = g_subprocess_get_status (proc);
  return TRUE;
}

/* The first non-empty line of stderr, or NULL. yt-dlp's and ytdl.ps1's useful
 * sentence is not always the last thing printed once the pipeline's own notes
 * are in the stream, so this reports the first line that says something. */
static char *
first_line (const char *text)
{
  if (text == NULL)
    return NULL;
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  for (gsize i = 0; lines[i] != NULL; i++)
    {
      g_strstrip (lines[i]);
      if (*lines[i] != '\0')
        return g_strdup (lines[i]);
    }
  return NULL;
}

/* Does this failure mean "the installed pipeline is older than --probe"?
 *
 * Narrow on purpose. Falling back on ANY failure would turn "this video is
 * private" into a second, slower attempt that also fails, and would hide a
 * genuinely broken pipeline behind a path that happens to work. The two
 * signals that actually mean "too old" are ytdl.ps1's own unknown-option
 * error and a missing probe.ps1. */
static gboolean
means_pipeline_too_old (const char *stderr_text)
{
  if (stderr_text == NULL)
    return FALSE;
  return strstr (stderr_text, "Unknown option: --probe") != NULL
         || strstr (stderr_text, "probe.ps1") != NULL;
}

static YtdlUrlProbe *
probe_via_pipeline (ProbeRequest *r, GCancellable *cancellable,
                    gboolean *too_old, GError **error)
{
  *too_old = FALSE;

  g_autofree char *pwsh = ytdl_find_pwsh ();
  g_autofree char *scripts = ytdl_scripts_dir ();
  g_autofree char *script = g_build_filename (scripts, "ytdl.ps1", NULL);
  if (pwsh == NULL || !g_file_test (script, G_FILE_TEST_IS_REGULAR))
    {
      *too_old = TRUE;
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "the installed pipeline was not found");
      return NULL;
    }

  g_autoptr (GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv, g_strdup (pwsh));
  g_ptr_array_add (argv, g_strdup ("-NoProfile"));
  g_ptr_array_add (argv, g_strdup ("-File"));
  g_ptr_array_add (argv, g_strdup (script));
  g_ptr_array_add (argv, g_strdup (r->url));
  g_ptr_array_add (argv, g_strdup ("--probe"));
  if (r->items != NULL && *r->items != '\0')
    {
      g_ptr_array_add (argv, g_strdup ("--items"));
      g_ptr_array_add (argv, g_strdup (r->items));
    }
  if (r->no_pot)
    g_ptr_array_add (argv, g_strdup ("--no-pot"));
  if (r->pot_port > 0)
    {
      g_ptr_array_add (argv, g_strdup ("--pot-port"));
      g_ptr_array_add (argv, g_strdup_printf ("%u", r->pot_port));
    }
  for (gsize i = 0;
       r->connection_args != NULL && r->connection_args[i] != NULL; i++)
    g_ptr_array_add (argv, g_strdup (r->connection_args[i]));
  for (gsize i = 0; r->extra_args != NULL && r->extra_args[i] != NULL; i++)
    {
      g_ptr_array_add (argv, g_strdup ("--ytdlp-arg"));
      g_ptr_array_add (argv, g_strdup (r->extra_args[i]));
    }
  g_ptr_array_add (argv, NULL);

  g_autofree char *out = NULL;
  g_autofree char *err = NULL;
  int status = 0;
  if (!run_capture ((const char *const *) argv->pdata, cancellable, &out, &err,
                    &status, error))
    return NULL;

  if (out != NULL && *g_strstrip (out) != '\0')
    {
      GError *parse_error = NULL;
      YtdlUrlProbe *p = ytdl_url_probe_parse (out, &parse_error);
      if (p != NULL)
        return p;
      g_clear_error (&parse_error);
    }

  *too_old = means_pipeline_too_old (err);
  g_autofree char *why = first_line (err);
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
               why != NULL ? why : "the pipeline's probe returned nothing");
  return NULL;
}

static YtdlUrlProbe *
probe_via_ytdlp (ProbeRequest *r, GCancellable *cancellable, GError **error)
{
  g_autofree char *ytdlp = ytdl_which ("yt-dlp");
  if (ytdlp == NULL)
    {
      g_set_error_literal (
          error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
          "this pipeline is older than `ytdl --probe`, and yt-dlp is not on "
          "PATH either, so there is no way to read the URL. Update the "
          "pipeline to get the preview.");
      return NULL;
    }

  /* The same two bounded calls probe.ps1 makes, for the same reason: `-J`
   * against a channel dumps the full extraction of every video in it. Flat
   * first to find out what this is; then one full extraction for the format
   * table. --ignore-config, because the pipeline's own conf opens with
   * --update and a preview must not self-update yt-dlp. */
  const char *base[] = { "--ignore-config", "--no-progress", "--socket-timeout",
                         "15", "--retries", "2", "--extractor-retries", "2" };

  /* The connection flags are spelled the same in ytdl and in yt-dlp, so they
   * carry straight over -- with one exception. --cookies FILE is dropped
   * here: yt-dlp writes its cookie jar back to that path on exit, and the
   * pipeline's probe is what knows to hand it a private copy instead. A
   * pipeline too old for --probe is too old for --cookies as well, so the
   * only thing this fallback could do with the real path is let yt-dlp
   * rewrite the user's credentials file. The browser source and the proxy
   * have no such side effect. */
  g_autoptr (GPtrArray) conn = g_ptr_array_new_with_free_func (g_free);
  for (gsize i = 0;
       r->connection_args != NULL && r->connection_args[i] != NULL
       && r->connection_args[i + 1] != NULL;
       i += 2)
    {
      if (g_strcmp0 (r->connection_args[i], "--cookies") == 0)
        continue;
      g_ptr_array_add (conn, g_strdup (r->connection_args[i]));
      g_ptr_array_add (conn, g_strdup (r->connection_args[i + 1]));
    }

  g_autofree char *item_spec =
      (r->items != NULL && *r->items != '\0') ? g_strdup (r->items)
                                              : g_strdup ("1:501");

  g_autoptr (GPtrArray) flat_argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (flat_argv, g_strdup (ytdlp));
  g_ptr_array_add (flat_argv, g_strdup ("-J"));
  for (gsize i = 0; i < G_N_ELEMENTS (base); i++)
    g_ptr_array_add (flat_argv, g_strdup (base[i]));
  for (guint i = 0; i < conn->len; i++)
    g_ptr_array_add (flat_argv, g_strdup (g_ptr_array_index (conn, i)));
  for (gsize i = 0; r->extra_args != NULL && r->extra_args[i] != NULL; i++)
    g_ptr_array_add (flat_argv, g_strdup (r->extra_args[i]));
  g_ptr_array_add (flat_argv, g_strdup ("--flat-playlist"));
  g_ptr_array_add (flat_argv, g_strdup ("--playlist-items"));
  g_ptr_array_add (flat_argv, g_strdup (item_spec));
  /* The end-of-options marker, same as every call site in the pipeline:
   * about one YouTube id in thirty starts with "-" or "_". */
  g_ptr_array_add (flat_argv, g_strdup ("--"));
  g_ptr_array_add (flat_argv, g_strdup (r->url));
  g_ptr_array_add (flat_argv, NULL);

  g_autofree char *flat = NULL;
  g_autofree char *flat_err = NULL;
  int status = 0;
  if (!run_capture ((const char *const *) flat_argv->pdata, cancellable, &flat,
                    &flat_err, &status, error))
    return NULL;
  if (flat == NULL || *g_strstrip (flat) == '\0')
    {
      g_autofree char *why = first_line (flat_err);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                   why != NULL ? why : "yt-dlp could not read that URL");
      return NULL;
    }

  /* Which URL the format table should come from: the video itself, or a
   * playlist's first entry. */
  g_autofree char *target = NULL;
  {
    GError *ignored = NULL;
    g_autoptr (JsonParser) fp = parse_text (flat, &ignored);
    g_clear_error (&ignored);
    if (fp != NULL)
      {
        JsonObject *lo = json_node_get_object (json_parser_get_root (fp));
        g_autofree char *type = str_member (lo, "_type");
        if (g_strcmp0 (type, "playlist") == 0
            || g_strcmp0 (type, "multi_video") == 0)
          {
            JsonArray *arr = array_member (lo, "entries");
            if (arr != NULL && json_array_get_length (arr) > 0)
              {
                JsonNode *n = json_array_get_element (arr, 0);
                if (n != NULL && JSON_NODE_HOLDS_OBJECT (n))
                  {
                    JsonObject *eo = json_node_get_object (n);
                    target = str_member (eo, "url");
                    if (target == NULL)
                      target = str_member (eo, "webpage_url");
                  }
              }
          }
      }
  }
  if (target == NULL)
    target = g_strdup (r->url);

  g_autoptr (GPtrArray) full_argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (full_argv, g_strdup (ytdlp));
  g_ptr_array_add (full_argv, g_strdup ("-J"));
  for (gsize i = 0; i < G_N_ELEMENTS (base); i++)
    g_ptr_array_add (full_argv, g_strdup (base[i]));
  for (guint i = 0; i < conn->len; i++)
    g_ptr_array_add (full_argv, g_strdup (g_ptr_array_index (conn, i)));
  for (gsize i = 0; r->extra_args != NULL && r->extra_args[i] != NULL; i++)
    g_ptr_array_add (full_argv, g_strdup (r->extra_args[i]));
  g_ptr_array_add (full_argv, g_strdup ("--no-playlist"));
  g_ptr_array_add (full_argv, g_strdup ("--"));
  g_ptr_array_add (full_argv, g_strdup (target));
  g_ptr_array_add (full_argv, NULL);

  g_autofree char *full = NULL;
  g_autofree char *full_err = NULL;
  if (!run_capture ((const char *const *) full_argv->pdata, cancellable, &full,
                    &full_err, &status, error))
    return NULL;
  if (full == NULL || *g_strstrip (full) == '\0')
    {
      g_autofree char *why = first_line (full_err);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                   why != NULL ? why : "yt-dlp could not read that URL");
      return NULL;
    }

  YtdlUrlProbe *p = ytdl_url_probe_from_ytdlp (flat, full, error);
  if (p != NULL && p->url == NULL)
    p->url = g_strdup (r->url);
  return p;
}

static void
probe_thread (GTask *task, gpointer source, gpointer task_data,
              GCancellable *cancellable)
{
  ProbeRequest *r = task_data;
  GError *error = NULL;
  gboolean too_old = FALSE;

  YtdlUrlProbe *p = probe_via_pipeline (r, cancellable, &too_old, &error);
  if (p != NULL)
    {
      g_task_return_pointer (task, p, (GDestroyNotify) ytdl_url_probe_free);
      return;
    }

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_clear_error (&error);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                               "cancelled");
      return;
    }

  if (!too_old)
    {
      /* A pipeline that HAS --probe and failed has told us something real --
       * the video is private, the URL is wrong, cookies are needed. Trying
       * again without the pipeline would replace that sentence with a worse
       * one from a different program. */
      g_task_return_error (task, error);
      return;
    }
  g_clear_error (&error);

  p = probe_via_ytdlp (r, cancellable, &error);
  if (p != NULL)
    g_task_return_pointer (task, p, (GDestroyNotify) ytdl_url_probe_free);
  else
    g_task_return_error (task, error);
}

void
ytdl_url_probe_run_async (const char *url, const char *items, gboolean no_pot,
                      guint pot_port, const char *const *extra_args,
                      const char *const *connection_args,
                      GCancellable *cancellable, GAsyncReadyCallback callback,
                      gpointer user_data)
{
  ProbeRequest *r = g_new0 (ProbeRequest, 1);
  /* Normalised the same way a run normalises it, so a bare video id typed in
   * the URL field probes exactly as it downloads. */
  r->url = ytdl_normalize_url (url);
  r->items = g_strdup (items);
  r->no_pot = no_pot;
  r->pot_port = pot_port;
  r->extra_args = g_strdupv ((GStrv) extra_args);
  r->connection_args = g_strdupv ((GStrv) connection_args);

  GTask *task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, ytdl_url_probe_run_async);
  g_task_set_task_data (task, r, probe_request_free);
  g_task_run_in_thread (task, probe_thread);
  g_object_unref (task);
}

YtdlUrlProbe *
ytdl_url_probe_run_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}
