#include "settings.h"

#include "paths.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

static char *
settings_path (void)
{
  g_autofree char *dir = ytdl_state_dir ();
  return g_build_filename (dir, "settings.json", NULL);
}

void
ytdl_settings_free (YtdlSettings *s)
{
  if (s == NULL)
    return;
  g_free (s->data_root);
  g_free (s->archive_root);
  g_free (s->sort_key);
  g_free (s->cookies_source);
  g_free (s->cookies_browser);
  g_free (s->cookies_profile);
  g_free (s->cookies_file);
  g_free (s->proxy);
  g_free (s->limit_rate);
  g_free (s->downloader);
  g_free (s);
}

static char *
opt_string (JsonObject *obj, const char *key)
{
  if (obj == NULL || !json_object_has_member (obj, key))
    return NULL;
  JsonNode *n = json_object_get_member (obj, key);
  if (!JSON_NODE_HOLDS_VALUE (n) ||
      json_node_get_value_type (n) != G_TYPE_STRING)
    return NULL;
  const char *v = json_node_get_string (n);
  return (v != NULL && *v != '\0') ? g_strdup (v) : NULL;
}

YtdlSettings *
ytdl_settings_load (void)
{
  YtdlSettings *s = g_new0 (YtdlSettings, 1);
  s->default_workers = 1;
  /* Set HERE, beside default_workers, and not only in the parse below --
   * every "no settings file yet" path returns early from this function, so a
   * default that lives further down applies to upgrades and not to fresh
   * installs. This one is newest-first, and g_new0 had quietly made a first
   * launch oldest-first. Found by running the app and reading the grid, not
   * by reading this file. */
  s->sort_descending = TRUE;
  /* Same reason: a default set only in the parse would leave a fresh install
   * -- the one with no settings.json -- with notifications off. */
  s->notify = TRUE;

  g_autofree char *path = settings_path ();
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, path, NULL))
    return s;

  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    return s;

  JsonObject *obj = json_node_get_object (root);
  s->data_root = opt_string (obj, "data_root");
  s->archive_root = opt_string (obj, "archive_root");
  s->sort_key = opt_string (obj, "library_sort");
  /* Absent reads as TRUE rather than FALSE, which is what a plain
   * get_boolean_member on a missing key would give: newest first is the
   * default, and an upgrade from a settings.json written before this
   * existed must not silently flip every Library to oldest-first. */
  s->sort_descending =
      json_object_has_member (obj, "library_sort_descending")
          ? json_object_get_boolean_member (obj, "library_sort_descending")
          : TRUE;
  if (json_object_has_member (obj, "default_workers"))
    s->default_workers = (guint) json_object_get_int_member (obj, "default_workers");
  if (s->default_workers < 1)
    s->default_workers = 1;

  s->cookies_source = opt_string (obj, "cookies_source");
  s->cookies_browser = opt_string (obj, "cookies_browser");
  s->cookies_profile = opt_string (obj, "cookies_profile");
  s->cookies_file = opt_string (obj, "cookies_file");
  s->proxy = opt_string (obj, "proxy");
  s->limit_rate = opt_string (obj, "limit_rate");
  s->downloader = opt_string (obj, "downloader");
  /* Absent reads as TRUE, for the upgrade case: a settings.json written
   * before this key existed must not switch notifications off. */
  s->notify = json_object_has_member (obj, "notify")
                  ? json_object_get_boolean_member (obj, "notify")
                  : TRUE;
  return s;
}

static gboolean
has_text (const char *s)
{
  if (s == NULL)
    return FALSE;
  for (; *s; s++)
    if (!g_ascii_isspace (*s))
      return TRUE;
  return FALSE;
}

YtdlRunOptions *
ytdl_settings_connection (const YtdlSettings *s)
{
  YtdlRunOptions *o = ytdl_run_options_new ();
  if (s == NULL)
    return o;

  if (g_strcmp0 (s->cookies_source, "browser") == 0
      && has_text (s->cookies_browser))
    {
      g_autofree char *browser = g_strstrip (g_strdup (s->cookies_browser));
      g_autofree char *profile =
          has_text (s->cookies_profile)
              ? g_strstrip (g_strdup (s->cookies_profile))
              : NULL;
      o->cookies_from_browser =
          profile != NULL ? g_strdup_printf ("%s:%s", browser, profile)
                          : g_strdup (browser);
    }
  else if (g_strcmp0 (s->cookies_source, "file") == 0
           && has_text (s->cookies_file))
    {
      /* Expanded here for the same reason --path is expanded in pipeline.c:
       * ytdl.ps1 has no notion of "~", and would look for a folder literally
       * named that under its own working directory. */
      g_autofree char *t = g_strstrip (g_strdup (s->cookies_file));
      o->cookies_file = ytdl_expand_tilde (t);
    }

  if (has_text (s->proxy))
    o->proxy = g_strstrip (g_strdup (s->proxy));
  if (has_text (s->limit_rate))
    o->limit_rate = g_strstrip (g_strdup (s->limit_rate));
  if (has_text (s->downloader) && g_strcmp0 (s->downloader, "native") != 0)
    o->downloader = g_strdup (s->downloader);
  return o;
}

void
ytdl_settings_save (const YtdlSettings *s)
{
  g_return_if_fail (s != NULL);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "data_root");
  json_builder_add_string_value (b, s->data_root != NULL ? s->data_root : "");
  json_builder_set_member_name (b, "archive_root");
  json_builder_add_string_value (b,
                                 s->archive_root != NULL ? s->archive_root : "");
  json_builder_set_member_name (b, "default_workers");
  json_builder_add_int_value (b, s->default_workers);
  json_builder_set_member_name (b, "library_sort");
  json_builder_add_string_value (b, s->sort_key != NULL ? s->sort_key : "");
  json_builder_set_member_name (b, "library_sort_descending");
  json_builder_add_boolean_value (b, s->sort_descending);
#define STR(k, v)                                                             \
  do                                                                          \
    {                                                                         \
      json_builder_set_member_name (b, k);                                    \
      json_builder_add_string_value (b, (v) != NULL ? (v) : "");              \
    }                                                                         \
  while (0)
  STR ("cookies_source", s->cookies_source);
  STR ("cookies_browser", s->cookies_browser);
  STR ("cookies_profile", s->cookies_profile);
  STR ("cookies_file", s->cookies_file);
  STR ("proxy", s->proxy);
  STR ("limit_rate", s->limit_rate);
  STR ("downloader", s->downloader);
#undef STR
  json_builder_set_member_name (b, "notify");
  json_builder_add_boolean_value (b, s->notify);
  json_builder_end_object (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_pretty (gen, TRUE);
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  json_generator_set_root (gen, root);

  g_autofree char *text = json_generator_to_data (gen, NULL);
  g_autofree char *path = settings_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0755);

  /* 0600, because since the Connection settings this file can hold a proxy
   * password. g_file_set_contents_full applies the mode to the temp file it
   * writes, so there is no moment at which the password sits in a file
   * anybody else can read. */
  g_autofree char *tmp = g_strconcat (path, ".tmp", NULL);
  if (g_file_set_contents_full (tmp, text, -1, G_FILE_SET_CONTENTS_CONSISTENT,
                                0600, NULL))
    {
      if (g_rename (tmp, path) != 0)
        g_unlink (tmp);
    }
}

char *
ytdl_settings_resolved_data_root (const YtdlSettings *s)
{
  if (s != NULL && s->data_root != NULL && *s->data_root != '\0')
    return ytdl_expand_tilde (s->data_root);
  return ytdl_install_root ();
}
