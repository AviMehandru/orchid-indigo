#include "verify_cache.h"

#include "paths.h"

#include <json-glib/json-glib.h>

#define CACHE_VERSION 1

typedef struct
{
  YtdlVerifyState state;
  char           *stamp; /* the manifest's archive_creation_time, or "" */
} Record;

struct _YtdlVerifyCache
{
  char       *path;
  GHashTable *by_key; /* char* key -> Record*, owned */
  gboolean    dirty;
};

static void
record_free (gpointer p)
{
  Record *r = p;
  if (r == NULL)
    return;
  g_free (r->stamp);
  g_free (r);
}

static const char *
state_to_id (YtdlVerifyState s)
{
  switch (s)
    {
    case YTDL_VERIFY_OK:
      return "ok";
    case YTDL_VERIFY_FAILED:
      return "failed";
    default:
      return "unknown";
    }
}

static YtdlVerifyState
state_from_id (const char *id)
{
  if (g_strcmp0 (id, "ok") == 0)
    return YTDL_VERIFY_OK;
  if (g_strcmp0 (id, "failed") == 0)
    return YTDL_VERIFY_FAILED;
  return YTDL_VERIFY_UNKNOWN;
}

YtdlVerifyCache *
ytdl_verify_cache_load (void)
{
  YtdlVerifyCache *cache = g_new0 (YtdlVerifyCache, 1);
  cache->by_key = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                         record_free);

  g_autofree char *dir = ytdl_cache_dir ();
  cache->path = g_build_filename (dir, "verify.json", NULL);

  g_autoptr (JsonParser) parser = json_parser_new ();
  /* Every failure below lands in the same place: an empty cache. A corrupt
   * store is a re-verify, and there is no version of "refuse to open the
   * Library because a cache file is malformed" that is the right call. */
  if (!json_parser_load_from_file (parser, cache->path, NULL))
    return cache;

  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    return cache;

  JsonObject *obj = json_node_get_object (root);
  if (!json_object_has_member (obj, "videos"))
    return cache;

  /* The version is read and ignored for now, deliberately: writing it from
   * the first release is what makes a future format change able to discard
   * old records instead of misreading them. A cache that cannot say what
   * shape it is in has to be thrown away wholesale the first time the shape
   * changes. */
  JsonNode *vnode = json_object_get_member (obj, "videos");
  if (!JSON_NODE_HOLDS_OBJECT (vnode))
    return cache;

  JsonObject *videos = json_node_get_object (vnode);
  g_autoptr (GList) keys = json_object_get_members (videos);
  for (GList *l = keys; l != NULL; l = l->next)
    {
      const char *key = l->data;
      JsonNode *n = json_object_get_member (videos, key);
      if (!JSON_NODE_HOLDS_OBJECT (n))
        continue;
      JsonObject *rec = json_node_get_object (n);

      Record *r = g_new0 (Record, 1);
      r->state = state_from_id (
          json_object_has_member (rec, "state")
              ? json_object_get_string_member (rec, "state")
              : NULL);
      r->stamp = g_strdup (json_object_has_member (rec, "stamp")
                               ? json_object_get_string_member (rec, "stamp")
                               : "");
      if (r->stamp == NULL)
        r->stamp = g_strdup ("");
      g_hash_table_insert (cache->by_key, g_strdup (key), r);
    }

  return cache;
}

void
ytdl_verify_cache_free (YtdlVerifyCache *cache)
{
  if (cache == NULL)
    return;
  g_free (cache->path);
  g_clear_pointer (&cache->by_key, g_hash_table_destroy);
  g_free (cache);
}

YtdlVerifyState
ytdl_verify_cache_get (YtdlVerifyCache *cache, const YtdlEntry *entry)
{
  g_return_val_if_fail (cache != NULL, YTDL_VERIFY_UNKNOWN);
  g_return_val_if_fail (entry != NULL, YTDL_VERIFY_UNKNOWN);

  Record *r = g_hash_table_lookup (cache->by_key, entry->key);
  if (r == NULL)
    return YTDL_VERIFY_UNKNOWN;

  /* The whole point of the file. A record whose stamp no longer matches the
   * manifest was taken before something rewrote this folder -- a refresh,
   * a manual repair, a re-download -- and a verification result from before
   * the files changed is not a weaker answer than none, it is a wrong one.
   * Treated as absent rather than deleted here, because a read should not
   * mutate; the next write over this key replaces it. */
  const char *now = entry->creation_stamp != NULL ? entry->creation_stamp : "";
  if (g_strcmp0 (r->stamp, now) != 0)
    return YTDL_VERIFY_UNKNOWN;

  return r->state;
}

void
ytdl_verify_cache_set (YtdlVerifyCache *cache, const YtdlEntry *entry,
                       YtdlVerifyState state)
{
  g_return_if_fail (cache != NULL);
  g_return_if_fail (entry != NULL);
  g_return_if_fail (entry->key != NULL);

  Record *r = g_new0 (Record, 1);
  r->state = state;
  r->stamp = g_strdup (entry->creation_stamp != NULL ? entry->creation_stamp
                                                     : "");
  g_hash_table_insert (cache->by_key, g_strdup (entry->key), r);
  cache->dirty = TRUE;
}

guint
ytdl_verify_cache_known (YtdlVerifyCache *cache)
{
  g_return_val_if_fail (cache != NULL, 0);

  /* Counts records, not matching records: this is asked with no index to
   * hand, and its job is to let the facet say "of the N I have checked"
   * rather than to be exact about which of them are still current. A record
   * whose folder has since been rewritten drops out of the count the next
   * time anything looks it up. */
  guint n = 0;
  GHashTableIter it;
  gpointer value;
  g_hash_table_iter_init (&it, cache->by_key);
  while (g_hash_table_iter_next (&it, NULL, &value))
    {
      const Record *r = value;
      if (r->state != YTDL_VERIFY_UNKNOWN)
        n++;
    }
  return n;
}

YtdlVerifyState
ytdl_verify_cache_lookup (const YtdlEntry *entry, gpointer user_data)
{
  return ytdl_verify_cache_get (user_data, entry);
}

void
ytdl_verify_cache_save (YtdlVerifyCache *cache)
{
  g_return_if_fail (cache != NULL);
  if (!cache->dirty)
    return;

  g_autofree char *dir = g_path_get_dirname (cache->path);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      g_warning ("could not create %s: the verification cache is not kept",
                 dir);
      return;
    }

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "version");
  json_builder_add_int_value (b, CACHE_VERSION);
  json_builder_set_member_name (b, "videos");
  json_builder_begin_object (b);

  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, cache->by_key);
  while (g_hash_table_iter_next (&it, &key, &value))
    {
      const Record *r = value;
      /* An UNKNOWN record carries no information and would grow the file
       * for every video anyone ever opened. */
      if (r->state == YTDL_VERIFY_UNKNOWN)
        continue;
      json_builder_set_member_name (b, key);
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "state");
      json_builder_add_string_value (b, state_to_id (r->state));
      json_builder_set_member_name (b, "stamp");
      json_builder_add_string_value (b, r->stamp != NULL ? r->stamp : "");
      json_builder_end_object (b);
    }

  json_builder_end_object (b);
  json_builder_end_object (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, json_builder_get_root (b));
  json_generator_set_pretty (gen, TRUE);

  GError *error = NULL;
  if (!json_generator_to_file (gen, cache->path, &error))
    {
      /* Warned and swallowed. This is a cache; the next run re-verifies. */
      g_warning ("could not write %s: %s", cache->path, error->message);
      g_clear_error (&error);
      return;
    }
  cache->dirty = FALSE;
}
