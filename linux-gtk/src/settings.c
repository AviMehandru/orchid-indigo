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
  if (json_object_has_member (obj, "default_workers"))
    s->default_workers = (guint) json_object_get_int_member (obj, "default_workers");
  if (s->default_workers < 1)
    s->default_workers = 1;
  return s;
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
  json_builder_end_object (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_pretty (gen, TRUE);
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  json_generator_set_root (gen, root);

  g_autofree char *text = json_generator_to_data (gen, NULL);
  g_autofree char *path = settings_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0755);

  g_autofree char *tmp = g_strconcat (path, ".tmp", NULL);
  if (g_file_set_contents (tmp, text, -1, NULL))
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
