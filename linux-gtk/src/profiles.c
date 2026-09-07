#include "profiles.h"

#include "paths.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

static gint64
now_secs (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

static char *
store_path (void)
{
  g_autofree char *dir = ytdl_state_dir ();
  return g_build_filename (dir, "profiles.json", NULL);
}

static void
profile_free (gpointer data)
{
  YtdlProfile *p = data;
  if (p == NULL)
    return;
  g_free (p->name);
  g_clear_pointer (&p->opts, ytdl_run_options_free);
  g_free (p);
}

void
ytdl_profile_store_free (YtdlProfileStore *store)
{
  if (store == NULL)
    return;
  g_free (store->active);
  g_clear_pointer (&store->profiles, g_ptr_array_unref);
  g_free (store);
}

static gssize
position_of (const YtdlProfileStore *store, const char *name)
{
  if (store == NULL || name == NULL)
    return -1;
  for (guint i = 0; i < store->profiles->len; i++)
    {
      const YtdlProfile *p = g_ptr_array_index (store->profiles, i);
      if (g_ascii_strcasecmp (p->name, name) == 0)
        return (gssize) i;
    }
  return -1;
}

const YtdlProfile *
ytdl_profiles_get (const YtdlProfileStore *store, const char *name)
{
  gssize i = position_of (store, name);
  return i < 0 ? NULL : g_ptr_array_index (store->profiles, i);
}

/* ---------------------------------------------------------------------- */
/* Persistence                                                            */
/* ---------------------------------------------------------------------- */

YtdlProfileStore *
ytdl_profiles_load (void)
{
  YtdlProfileStore *store = g_new0 (YtdlProfileStore, 1);
  store->profiles = g_ptr_array_new_with_free_func (profile_free);

  g_autofree char *path = store_path ();
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, path, NULL))
    return store;

  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    return store;

  JsonObject *obj = json_node_get_object (root);

  if (json_object_has_member (obj, "active"))
    {
      JsonNode *n = json_object_get_member (obj, "active");
      if (JSON_NODE_HOLDS_VALUE (n) &&
          json_node_get_value_type (n) == G_TYPE_STRING)
        {
          const char *a = json_node_get_string (n);
          if (a != NULL && *a != '\0')
            store->active = g_strdup (a);
        }
    }

  if (!json_object_has_member (obj, "profiles"))
    return store;
  JsonNode *pn = json_object_get_member (obj, "profiles");
  if (!JSON_NODE_HOLDS_ARRAY (pn))
    return store;

  JsonArray *arr = json_node_get_array (pn);
  for (guint i = 0; i < json_array_get_length (arr); i++)
    {
      JsonNode *e = json_array_get_element (arr, i);
      if (!JSON_NODE_HOLDS_OBJECT (e))
        continue;
      JsonObject *po = json_node_get_object (e);

      if (!json_object_has_member (po, "name"))
        continue;
      const char *name = json_object_get_string_member (po, "name");
      if (name == NULL || *name == '\0')
        continue;

      JsonObject *opts = NULL;
      if (json_object_has_member (po, "opts"))
        {
          JsonNode *on = json_object_get_member (po, "opts");
          if (JSON_NODE_HOLDS_OBJECT (on))
            opts = json_node_get_object (on);
        }

      YtdlProfile *p = g_new0 (YtdlProfile, 1);
      p->name = g_strdup (name);
      p->opts = ytdl_run_options_from_json (opts);
      p->saved = json_object_has_member (po, "saved")
                     ? json_object_get_int_member (po, "saved")
                     : 0;
      /* A stored URL is dropped on read as well as on write. A profiles.json
       * hand-edited to carry one must not be able to hijack a download. */
      g_clear_pointer (&p->opts->url, g_free);
      g_ptr_array_add (store->profiles, p);
    }

  /* An active name that no longer matches anything is cleared rather than
   * left pointing at nothing. */
  if (store->active != NULL && position_of (store, store->active) < 0)
    g_clear_pointer (&store->active, g_free);

  return store;
}

static gboolean
save_store (const YtdlProfileStore *store, GError **error)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);

  json_builder_set_member_name (b, "active");
  if (store->active != NULL)
    json_builder_add_string_value (b, store->active);
  else
    json_builder_add_null_value (b);

  json_builder_set_member_name (b, "profiles");
  json_builder_begin_array (b);
  for (guint i = 0; i < store->profiles->len; i++)
    {
      const YtdlProfile *p = g_ptr_array_index (store->profiles, i);
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "name");
      json_builder_add_string_value (b, p->name);
      json_builder_set_member_name (b, "saved");
      json_builder_add_int_value (b, p->saved);
      json_builder_set_member_name (b, "opts");
      ytdl_run_options_build_json (b, p->opts);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_end_object (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_pretty (gen, TRUE);
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  json_generator_set_root (gen, root);
  g_autofree char *text = json_generator_to_data (gen, NULL);

  g_autofree char *path = store_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0755);

  /* Temp file plus rename. A truncated write from a crash or a full disk
   * would take every profile at once, and rename(2) is atomic. */
  g_autofree char *tmp = g_strconcat (path, ".tmp", NULL);
  if (!g_file_set_contents (tmp, text, -1, error))
    return FALSE;
  if (g_rename (tmp, path) != 0)
    {
      g_unlink (tmp);
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                   "Could not replace %s.", path);
      return FALSE;
    }
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Operations                                                             */
/* ---------------------------------------------------------------------- */

static char *
clean_name (const char *name, GError **error)
{
  g_autofree char *n = g_strdup (name != NULL ? name : "");
  g_strstrip (n);

  if (*n == '\0')
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "A profile needs a name.");
      return NULL;
    }
  if (g_utf8_strlen (n, -1) > YTDL_PROFILE_MAX_NAME)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Profile names are limited to %d characters.",
                   YTDL_PROFILE_MAX_NAME);
      return NULL;
    }
  return g_steal_pointer (&n);
}

gboolean
ytdl_profiles_save (YtdlProfileStore *store, const char *name,
                    const YtdlRunOptions *opts, GError **error)
{
  g_return_val_if_fail (store != NULL, FALSE);

  g_autofree char *clean = clean_name (name, error);
  if (clean == NULL)
    return FALSE;

  YtdlRunOptions *copy = ytdl_run_options_copy (opts);
  g_clear_pointer (&copy->url, g_free);

  gssize i = position_of (store, clean);
  if (i >= 0)
    {
      YtdlProfile *p = g_ptr_array_index (store->profiles, i);
      ytdl_run_options_free (p->opts);
      p->opts = copy;
      p->saved = now_secs ();
      /* Keep the name as newly typed, so re-saving "Archival" over "archival"
       * fixes the capitalisation rather than ignoring it. */
      g_free (p->name);
      p->name = g_strdup (clean);
    }
  else
    {
      if (store->profiles->len >= YTDL_PROFILE_MAX)
        {
          ytdl_run_options_free (copy);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "That would be more than %d profiles. Delete one first.",
                       YTDL_PROFILE_MAX);
          return FALSE;
        }
      YtdlProfile *p = g_new0 (YtdlProfile, 1);
      p->name = g_strdup (clean);
      p->opts = copy;
      p->saved = now_secs ();
      /* Appended rather than sorted: a dropdown that reshuffles itself under
       * the pointer is worse than one in an arbitrary but stable order. */
      g_ptr_array_add (store->profiles, p);
    }

  g_free (store->active);
  store->active = g_strdup (clean);
  return save_store (store, error);
}

gboolean
ytdl_profiles_delete (YtdlProfileStore *store, const char *name,
                      GError **error)
{
  g_return_val_if_fail (store != NULL, FALSE);

  gssize i = position_of (store, name);
  if (i < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "There is no profile called \"%s\".",
                   name != NULL ? name : "");
      return FALSE;
    }

  const YtdlProfile *removed = g_ptr_array_index (store->profiles, i);
  gboolean was_active =
      store->active != NULL &&
      g_ascii_strcasecmp (store->active, removed->name) == 0;

  g_ptr_array_remove_index (store->profiles, (guint) i);
  if (was_active)
    g_clear_pointer (&store->active, g_free);

  return save_store (store, error);
}

gboolean
ytdl_profiles_rename (YtdlProfileStore *store, const char *from,
                      const char *to, GError **error)
{
  g_return_val_if_fail (store != NULL, FALSE);

  g_autofree char *clean = clean_name (to, error);
  if (clean == NULL)
    return FALSE;

  gssize i = position_of (store, from);
  if (i < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "There is no profile called \"%s\".",
                   from != NULL ? from : "");
      return FALSE;
    }

  /* Renaming onto an existing name collides -- unless it is this same profile
   * being re-capitalised, which is a rename people actually do. */
  gssize j = position_of (store, clean);
  if (j >= 0 && j != i)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                   "There is already a profile called \"%s\".", clean);
      return FALSE;
    }

  YtdlProfile *p = g_ptr_array_index (store->profiles, i);
  gboolean was_active = store->active != NULL &&
                        g_ascii_strcasecmp (store->active, p->name) == 0;

  g_free (p->name);
  p->name = g_strdup (clean);
  if (was_active)
    {
      g_free (store->active);
      store->active = g_strdup (clean);
    }
  return save_store (store, error);
}

gboolean
ytdl_profiles_activate (YtdlProfileStore *store, const char *name,
                        GError **error)
{
  g_return_val_if_fail (store != NULL, FALSE);

  if (name == NULL)
    {
      g_clear_pointer (&store->active, g_free);
      return save_store (store, error);
    }

  const YtdlProfile *p = ytdl_profiles_get (store, name);
  if (p == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "There is no profile called \"%s\".", name);
      return FALSE;
    }

  g_free (store->active);
  store->active = g_strdup (p->name);
  return save_store (store, error);
}
