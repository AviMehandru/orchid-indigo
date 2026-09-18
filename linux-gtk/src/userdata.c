#include "userdata.h"

#include "paths.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#define USER_DATA_VERSION 1

typedef struct
{
  gboolean watched;
  double   position; /* seconds; 0 means no resume point */
  char    *updated;  /* ISO 8601, for a human reading the file */
} WatchState;

struct _YtdlUserData
{
  char       *path;
  GHashTable *watch;     /* char* key -> WatchState*, owned */
  /* Just the keys whose WatchState says watched, as a set. Maintained beside
   * `watch` rather than derived on demand because the Library's "unwatched"
   * facet needs it on every keystroke, and because the filter is deliberately
   * a plain membership test against a borrowed table. Only three places can
   * change a watched flag, and all three go through set_watched_flag. */
  GHashTable *watched;   /* char* key, owned */
  GPtrArray  *playlists; /* YtdlPlaylist*, owned */
  gboolean    dirty;
  /* Set when a file existed and could not be parsed. Blocks saving, so a
   * corrupt store is left for its owner rather than overwritten. */
  gboolean    unreadable;
};

static void
watch_free (gpointer p)
{
  WatchState *w = p;
  if (w == NULL)
    return;
  g_free (w->updated);
  g_free (w);
}

static void
playlist_free (gpointer p)
{
  YtdlPlaylist *pl = p;
  if (pl == NULL)
    return;
  g_free (pl->id);
  g_free (pl->name);
  g_clear_pointer (&pl->keys, g_ptr_array_unref);
  g_free (pl);
}

/* The ONE place a watched flag changes, so that the set beside it cannot
 * drift. Returns TRUE when something actually moved. */
static gboolean
set_watched_flag (YtdlUserData *ud, const char *key, WatchState *w,
                  gboolean watched)
{
  if (w->watched == watched)
    return FALSE;

  w->watched = watched;
  if (watched)
    g_hash_table_add (ud->watched, g_strdup (key));
  else
    g_hash_table_remove (ud->watched, key);
  return TRUE;
}

static char *
now_iso (void)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  return g_date_time_format_iso8601 (now);
}

/* ---------------------------------------------------------------------- */
/* Load and save                                                          */
/* ---------------------------------------------------------------------- */

YtdlUserData *
ytdl_user_data_load (void)
{
  YtdlUserData *ud = g_new0 (YtdlUserData, 1);
  ud->watch =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, watch_free);
  ud->watched = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  ud->playlists = g_ptr_array_new_with_free_func (playlist_free);

  g_autofree char *dir = ytdl_state_dir ();
  ud->path = g_build_filename (dir, "userdata.json", NULL);

  /* A file that is not there is the ordinary first-launch state and is NOT
   * "unreadable": there is nothing to protect, and the first save should
   * create it. Only a file that exists and will not parse gets the flag. */
  if (!g_file_test (ud->path, G_FILE_TEST_EXISTS))
    return ud;

  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, ud->path, NULL))
    {
      ud->unreadable = TRUE;
      g_warning ("could not parse %s; watch state and playlists are read-only "
                 "this session",
                 ud->path);
      return ud;
    }

  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      ud->unreadable = TRUE;
      return ud;
    }

  JsonObject *obj = json_node_get_object (root);

  /* A store written by a newer format is READ, not discarded, which is the
   * opposite of what the caches do. A cache can be thrown away and rebuilt;
   * this cannot. So unknown fields are ignored and what is understood is
   * kept, and the version is written so a future change can decide for
   * itself what to do about an older one. */
  if (json_object_has_member (obj, "watch"))
    {
      JsonNode *wnode = json_object_get_member (obj, "watch");
      if (JSON_NODE_HOLDS_OBJECT (wnode))
        {
          JsonObject *watch = json_node_get_object (wnode);
          g_autoptr (GList) keys = json_object_get_members (watch);
          for (GList *l = keys; l != NULL; l = l->next)
            {
              const char *key = l->data;
              JsonNode *n = json_object_get_member (watch, key);
              if (!JSON_NODE_HOLDS_OBJECT (n))
                continue;
              JsonObject *rec = json_node_get_object (n);

              WatchState *w = g_new0 (WatchState, 1);
              if (json_object_has_member (rec, "watched"))
                w->watched = json_object_get_boolean_member (rec, "watched");
              if (json_object_has_member (rec, "position"))
                w->position = json_object_get_double_member (rec, "position");
              if (w->position < 0)
                w->position = 0;
              if (json_object_has_member (rec, "updated"))
                w->updated =
                    g_strdup (json_object_get_string_member (rec, "updated"));
              g_hash_table_insert (ud->watch, g_strdup (key), w);
              if (w->watched)
                g_hash_table_add (ud->watched, g_strdup (key));
            }
        }
    }

  if (json_object_has_member (obj, "playlists"))
    {
      JsonNode *pnode = json_object_get_member (obj, "playlists");
      if (JSON_NODE_HOLDS_ARRAY (pnode))
        {
          JsonArray *arr = json_node_get_array (pnode);
          for (guint i = 0; i < json_array_get_length (arr); i++)
            {
              JsonNode *n = json_array_get_element (arr, i);
              if (!JSON_NODE_HOLDS_OBJECT (n))
                continue;
              JsonObject *rec = json_node_get_object (n);

              if (!json_object_has_member (rec, "id") ||
                  !json_object_has_member (rec, "name"))
                continue;

              YtdlPlaylist *pl = g_new0 (YtdlPlaylist, 1);
              pl->id = g_strdup (json_object_get_string_member (rec, "id"));
              pl->name = g_strdup (json_object_get_string_member (rec, "name"));
              pl->keys = g_ptr_array_new_with_free_func (g_free);

              if (json_object_has_member (rec, "keys"))
                {
                  JsonNode *kn = json_object_get_member (rec, "keys");
                  if (JSON_NODE_HOLDS_ARRAY (kn))
                    {
                      JsonArray *ka = json_node_get_array (kn);
                      for (guint j = 0; j < json_array_get_length (ka); j++)
                        {
                          const char *k = json_array_get_string_element (ka, j);
                          if (k != NULL && *k != '\0')
                            g_ptr_array_add (pl->keys, g_strdup (k));
                        }
                    }
                }

              /* A playlist with no id or no name cannot be addressed or
               * shown, so it is dropped rather than kept as a ghost. */
              if (pl->id == NULL || pl->name == NULL)
                playlist_free (pl);
              else
                g_ptr_array_add (ud->playlists, pl);
            }
        }
    }

  return ud;
}

void
ytdl_user_data_free (YtdlUserData *ud)
{
  if (ud == NULL)
    return;
  g_free (ud->path);
  g_clear_pointer (&ud->watch, g_hash_table_destroy);
  g_clear_pointer (&ud->watched, g_hash_table_destroy);
  g_clear_pointer (&ud->playlists, g_ptr_array_unref);
  g_free (ud);
}

gboolean
ytdl_user_data_is_read_only (YtdlUserData *ud)
{
  g_return_val_if_fail (ud != NULL, TRUE);
  return ud->unreadable;
}

void
ytdl_user_data_save (YtdlUserData *ud)
{
  g_return_if_fail (ud != NULL);
  if (!ud->dirty)
    return;

  /* The whole point of the flag. Writing here would replace a file somebody
   * may still be able to salvage with one built from the empty store that
   * failing to read it produced. */
  if (ud->unreadable)
    {
      g_warning ("not overwriting the unparseable %s", ud->path);
      return;
    }

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "version");
  json_builder_add_int_value (b, USER_DATA_VERSION);

  json_builder_set_member_name (b, "watch");
  json_builder_begin_object (b);
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, ud->watch);
  while (g_hash_table_iter_next (&it, &key, &value))
    {
      const WatchState *w = value;
      /* A record that says nothing is not written. Opening a video and
       * closing it immediately must not add a line to this file forever. */
      if (!w->watched && w->position <= 0)
        continue;
      json_builder_set_member_name (b, key);
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "watched");
      json_builder_add_boolean_value (b, w->watched);
      json_builder_set_member_name (b, "position");
      json_builder_add_double_value (b, w->position);
      json_builder_set_member_name (b, "updated");
      json_builder_add_string_value (b, w->updated != NULL ? w->updated : "");
      json_builder_end_object (b);
    }
  json_builder_end_object (b);

  json_builder_set_member_name (b, "playlists");
  json_builder_begin_array (b);
  for (guint i = 0; i < ud->playlists->len; i++)
    {
      const YtdlPlaylist *pl = g_ptr_array_index (ud->playlists, i);
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "id");
      json_builder_add_string_value (b, pl->id);
      json_builder_set_member_name (b, "name");
      json_builder_add_string_value (b, pl->name);
      json_builder_set_member_name (b, "keys");
      json_builder_begin_array (b);
      for (guint j = 0; j < pl->keys->len; j++)
        json_builder_add_string_value (b, g_ptr_array_index (pl->keys, j));
      json_builder_end_array (b);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_end_object (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, json_builder_get_root (b));
  /* Indented, unlike the search index: this one is small, it is user data,
   * and "delete this folder and start over" is a complete answer partly
   * because the things in it can be read. */
  json_generator_set_pretty (gen, TRUE);

  g_autofree char *text = json_generator_to_data (gen, NULL);
  g_autofree char *dir = g_path_get_dirname (ud->path);
  g_mkdir_with_parents (dir, 0700);

  /* Through a temp file and renamed, like everything else this app owns that
   * is not disposable. A half-written userdata.json after a crash would lose
   * every playlist rather than the last change. */
  g_autofree char *tmp = g_strconcat (ud->path, ".tmp", NULL);
  if (g_file_set_contents (tmp, text, -1, NULL))
    {
      if (g_rename (tmp, ud->path) != 0)
        g_unlink (tmp);
      else
        ud->dirty = FALSE;
    }
}

/* ---------------------------------------------------------------------- */
/* Watch state                                                            */
/* ---------------------------------------------------------------------- */

static WatchState *
watch_for (YtdlUserData *ud, const char *key, gboolean create)
{
  WatchState *w = g_hash_table_lookup (ud->watch, key);
  if (w != NULL || !create)
    return w;
  w = g_new0 (WatchState, 1);
  g_hash_table_insert (ud->watch, g_strdup (key), w);
  return w;
}

gboolean
ytdl_user_data_is_watched (YtdlUserData *ud, const char *key)
{
  g_return_val_if_fail (ud != NULL, FALSE);
  if (key == NULL)
    return FALSE;
  const WatchState *w = g_hash_table_lookup (ud->watch, key);
  return w != NULL && w->watched;
}

void
ytdl_user_data_set_watched (YtdlUserData *ud, const char *key,
                            gboolean watched)
{
  g_return_if_fail (ud != NULL);
  g_return_if_fail (key != NULL);

  WatchState *w = watch_for (ud, key, TRUE);
  if (!set_watched_flag (ud, key, w, watched))
    return;

  /* Marking something watched by hand clears a resume point: the two would
   * otherwise disagree, and the flag is the more deliberate statement of the
   * pair. Marking it UNWATCHED leaves the position alone -- "I want to see
   * this again" and "start it over" are different wishes. */
  if (watched)
    w->position = 0;

  g_free (w->updated);
  w->updated = now_iso ();
  ud->dirty = TRUE;
}

double
ytdl_user_data_position (YtdlUserData *ud, const char *key)
{
  g_return_val_if_fail (ud != NULL, 0);
  if (key == NULL)
    return 0;
  const WatchState *w = g_hash_table_lookup (ud->watch, key);
  return w != NULL ? w->position : 0;
}

void
ytdl_user_data_set_position (YtdlUserData *ud, const char *key, double seconds,
                             double duration)
{
  g_return_if_fail (ud != NULL);
  g_return_if_fail (key != NULL);

  if (seconds < 0)
    seconds = 0;

  gboolean finished =
      duration > 0 && seconds >= duration * YTDL_WATCHED_FRACTION;

  WatchState *w = watch_for (ud, key, TRUE);

  if (finished)
    {
      /* Watched, and no resume point: re-opening something you finished
       * should start it again rather than drop you back at the end card. */
      set_watched_flag (ud, key, w, TRUE);
      w->position = 0;
    }
  else if (seconds < YTDL_RESUME_MIN_SECONDS)
    {
      /* Opening a video and closing it again must not litter the library
       * with eight-second resume offers. The watched flag is untouched: a
       * video you have already seen does not become unseen because you
       * glanced at the first ten seconds of it. */
      w->position = 0;
    }
  else
    {
      w->position = seconds;
    }

  g_free (w->updated);
  w->updated = now_iso ();
  ud->dirty = TRUE;
}

guint
ytdl_user_data_watched_count (YtdlUserData *ud)
{
  g_return_val_if_fail (ud != NULL, 0);

  /* The set's size, not a walk of `watch`: the two must agree, and reading
   * the answer off the set is what makes a drift between them show up as a
   * wrong count in the tests rather than as a facet that quietly disagrees
   * with the label above it. */
  return g_hash_table_size (ud->watched);
}

GHashTable *
ytdl_user_data_watched_keys (YtdlUserData *ud)
{
  g_return_val_if_fail (ud != NULL, NULL);
  return ud->watched;
}

/* ---------------------------------------------------------------------- */
/* Playlists                                                              */
/* ---------------------------------------------------------------------- */

GPtrArray *
ytdl_user_data_playlists (YtdlUserData *ud)
{
  g_return_val_if_fail (ud != NULL, NULL);
  return ud->playlists;
}

YtdlPlaylist *
ytdl_user_data_playlist (YtdlUserData *ud, const char *id)
{
  g_return_val_if_fail (ud != NULL, NULL);
  if (id == NULL)
    return NULL;
  for (guint i = 0; i < ud->playlists->len; i++)
    {
      YtdlPlaylist *pl = g_ptr_array_index (ud->playlists, i);
      if (g_strcmp0 (pl->id, id) == 0)
        return pl;
    }
  return NULL;
}

YtdlPlaylist *
ytdl_user_data_playlist_create (YtdlUserData *ud, const char *name)
{
  g_return_val_if_fail (ud != NULL, NULL);
  if (name == NULL)
    return NULL;

  g_autofree char *trimmed = g_strdup (name);
  g_strstrip (trimmed);
  if (*trimmed == '\0')
    return NULL;

  YtdlPlaylist *pl = g_new0 (YtdlPlaylist, 1);
  /* A UUID rather than a slug of the name, so renaming a playlist does not
   * change its identity and two playlists called the same thing are still
   * two playlists. */
  pl->id = g_uuid_string_random ();
  pl->name = g_steal_pointer (&trimmed);
  pl->keys = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (ud->playlists, pl);
  ud->dirty = TRUE;
  return pl;
}

gboolean
ytdl_user_data_playlist_rename (YtdlUserData *ud, const char *id,
                                const char *name)
{
  YtdlPlaylist *pl = ytdl_user_data_playlist (ud, id);
  if (pl == NULL || name == NULL)
    return FALSE;

  g_autofree char *trimmed = g_strdup (name);
  g_strstrip (trimmed);
  if (*trimmed == '\0')
    return FALSE;

  g_free (pl->name);
  pl->name = g_steal_pointer (&trimmed);
  ud->dirty = TRUE;
  return TRUE;
}

gboolean
ytdl_user_data_playlist_delete (YtdlUserData *ud, const char *id)
{
  g_return_val_if_fail (ud != NULL, FALSE);
  for (guint i = 0; i < ud->playlists->len; i++)
    {
      const YtdlPlaylist *pl = g_ptr_array_index (ud->playlists, i);
      if (g_strcmp0 (pl->id, id) != 0)
        continue;
      g_ptr_array_remove_index (ud->playlists, i);
      ud->dirty = TRUE;
      return TRUE;
    }
  return FALSE;
}

gboolean
ytdl_user_data_playlist_contains (YtdlUserData *ud, const char *id,
                                  const char *key)
{
  const YtdlPlaylist *pl = ytdl_user_data_playlist (ud, id);
  if (pl == NULL || key == NULL)
    return FALSE;
  for (guint i = 0; i < pl->keys->len; i++)
    if (g_strcmp0 (g_ptr_array_index (pl->keys, i), key) == 0)
      return TRUE;
  return FALSE;
}

gboolean
ytdl_user_data_playlist_add (YtdlUserData *ud, const char *id, const char *key)
{
  YtdlPlaylist *pl = ytdl_user_data_playlist (ud, id);
  if (pl == NULL || key == NULL || *key == '\0')
    return FALSE;

  /* Already there is a no-op rather than a duplicate: a playlist is a set the
   * user ordered, not a bag, and "add" pressed twice from two places should
   * not produce two rows that both delete at once. */
  if (ytdl_user_data_playlist_contains (ud, id, key))
    return TRUE;

  g_ptr_array_add (pl->keys, g_strdup (key));
  ud->dirty = TRUE;
  return TRUE;
}

gboolean
ytdl_user_data_playlist_remove (YtdlUserData *ud, const char *id,
                                const char *key)
{
  YtdlPlaylist *pl = ytdl_user_data_playlist (ud, id);
  if (pl == NULL || key == NULL)
    return FALSE;

  for (guint i = 0; i < pl->keys->len; i++)
    {
      if (g_strcmp0 (g_ptr_array_index (pl->keys, i), key) != 0)
        continue;
      g_ptr_array_remove_index (pl->keys, i);
      ud->dirty = TRUE;
      return TRUE;
    }
  return FALSE;
}
