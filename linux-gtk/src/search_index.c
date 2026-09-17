#include "search_index.h"

#include "paths.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include <string.h>

#define INDEX_VERSION 1

/* One caption cue is a few words, so a two-word query almost never falls
 * inside one. Consecutive cues are glued into passages of about this many
 * bytes before matching, which is roughly a sentence or two -- long enough for
 * a phrase to land in one, short enough to read as a snippet. */
#define TRANSCRIPT_PASSAGE 240

typedef struct
{
  char *stamp;      /* the manifest's archive_creation_time, or a derived one */
  char *comments;   /* " tok tok tok ", sorted and unique; may be NULL */
  char *transcript; /* same */
} Record;

struct _YtdlSearchIndex
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
  g_free (r->comments);
  g_free (r->transcript);
  g_free (r);
}

/* ---------------------------------------------------------------------- */
/* Scopes                                                                 */
/* ---------------------------------------------------------------------- */

static const char *const SCOPE_LABELS[YTDL_N_SEARCH_SCOPES] = {
  "Title and channel", "Comments", "Transcript", "Everything",
};

static const char *const SCOPE_IDS[YTDL_N_SEARCH_SCOPES] = {
  "metadata", "comments", "transcript", "everything",
};

const char *
ytdl_search_scope_label (YtdlSearchScope scope)
{
  if (scope < 0 || scope >= YTDL_N_SEARCH_SCOPES)
    return SCOPE_LABELS[YTDL_SEARCH_METADATA];
  return SCOPE_LABELS[scope];
}

const char *
ytdl_search_scope_id (YtdlSearchScope scope)
{
  if (scope < 0 || scope >= YTDL_N_SEARCH_SCOPES)
    return SCOPE_IDS[YTDL_SEARCH_METADATA];
  return SCOPE_IDS[scope];
}

YtdlSearchScope
ytdl_search_scope_from_id (const char *id)
{
  if (id == NULL)
    return YTDL_SEARCH_METADATA;
  for (int i = 0; i < YTDL_N_SEARCH_SCOPES; i++)
    if (g_strcmp0 (id, SCOPE_IDS[i]) == 0)
      return (YtdlSearchScope) i;
  return YTDL_SEARCH_METADATA;
}

gboolean
ytdl_search_scope_needs_index (YtdlSearchScope scope)
{
  return scope != YTDL_SEARCH_METADATA;
}

/* ---------------------------------------------------------------------- */
/* Tokenizing                                                             */
/* ---------------------------------------------------------------------- */

GPtrArray *
ytdl_search_tokenize (const char *text)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  if (text == NULL)
    return out;

  /* Validated before walking. g_utf8_next_char on invalid UTF-8 walks off the
   * end of the string, and this is fed comment text straight out of a file
   * yt-dlp wrote from somebody else's input -- which is exactly where a
   * malformed sequence comes from. */
  const char *end = NULL;
  if (!g_utf8_validate (text, -1, &end))
    {
      g_autofree char *clean = g_strndup (text, (gsize) (end - text));
      GPtrArray *partial = ytdl_search_tokenize (clean);
      g_ptr_array_unref (out);
      return partial;
    }

  GString *tok = g_string_new (NULL);
  for (const char *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (g_unichar_isalnum (c))
        {
          g_string_append_unichar (tok, g_unichar_tolower (c));
          continue;
        }
      if (tok->len > 0)
        {
          g_ptr_array_add (out, g_strdup (tok->str));
          g_string_truncate (tok, 0);
        }
    }
  if (tok->len > 0)
    g_ptr_array_add (out, g_strdup (tok->str));
  g_string_free (tok, TRUE);

  return out;
}

static int
cmp_str (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 (*(const char *const *) a, *(const char *const *) b);
}

/* " tok1 tok2 tok3 " -- sorted, unique, and padded at both ends.
 *
 * The padding is what makes a query match on a token BOUNDARY with a plain
 * substring search: looking for " rail" finds the token "railway" and does not
 * find "guardrail". Storing it padded rather than padding at query time means
 * the concatenation happens once per video per build instead of once per video
 * per keystroke. */
static char *
token_blob (GPtrArray *tokens)
{
  g_ptr_array_sort (tokens, cmp_str);

  GString *s = g_string_new (" ");
  const char *previous = NULL;
  for (guint i = 0; i < tokens->len; i++)
    {
      const char *t = g_ptr_array_index (tokens, i);
      if (g_utf8_strlen (t, -1) < YTDL_SEARCH_MIN_TOKEN)
        continue;
      if (g_strcmp0 (t, previous) == 0)
        continue;
      g_string_append (s, t);
      g_string_append_c (s, ' ');
      previous = t;
    }

  /* A single space means "parsed, found nothing" -- which is a real answer
   * and must be told apart from "not parsed yet". Kept rather than nulled. */
  return g_string_free (s, FALSE);
}

/* ---------------------------------------------------------------------- */
/* Load and save                                                          */
/* ---------------------------------------------------------------------- */

YtdlSearchIndex *
ytdl_search_index_load (void)
{
  YtdlSearchIndex *index = g_new0 (YtdlSearchIndex, 1);
  index->by_key =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, record_free);

  g_autofree char *dir = ytdl_cache_dir ();
  index->path = g_build_filename (dir, "search-index.json", NULL);

  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, index->path, NULL))
    return index;

  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    return index;

  JsonObject *obj = json_node_get_object (root);

  /* A store written by a different version of this format is discarded rather
   * than misread. That is the whole reason the version is written from the
   * first release: without it, a later change to what a token is would leave
   * every existing user with an index that silently answers the old way. */
  if (!json_object_has_member (obj, "version") ||
      json_object_get_int_member (obj, "version") != INDEX_VERSION)
    return index;

  if (!json_object_has_member (obj, "videos"))
    return index;
  JsonNode *vnode = json_object_get_member (obj, "videos");
  if (!JSON_NODE_HOLDS_OBJECT (vnode))
    return index;

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
      r->stamp = g_strdup (json_object_has_member (rec, "stamp")
                               ? json_object_get_string_member (rec, "stamp")
                               : "");
      if (r->stamp == NULL)
        r->stamp = g_strdup ("");
      if (json_object_has_member (rec, "c"))
        r->comments = g_strdup (json_object_get_string_member (rec, "c"));
      if (json_object_has_member (rec, "t"))
        r->transcript = g_strdup (json_object_get_string_member (rec, "t"));
      g_hash_table_insert (index->by_key, g_strdup (key), r);
    }

  return index;
}

void
ytdl_search_index_free (YtdlSearchIndex *index)
{
  if (index == NULL)
    return;
  g_free (index->path);
  g_clear_pointer (&index->by_key, g_hash_table_destroy);
  g_free (index);
}

void
ytdl_search_index_save (YtdlSearchIndex *index)
{
  g_return_if_fail (index != NULL);
  if (!index->dirty)
    return;

  g_autofree char *dir = g_path_get_dirname (index->path);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      g_warning ("could not create %s: the search index is not kept", dir);
      return;
    }

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "version");
  json_builder_add_int_value (b, INDEX_VERSION);
  json_builder_set_member_name (b, "videos");
  json_builder_begin_object (b);

  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, index->by_key);
  while (g_hash_table_iter_next (&it, &key, &value))
    {
      const Record *r = value;
      json_builder_set_member_name (b, key);
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "stamp");
      json_builder_add_string_value (b, r->stamp != NULL ? r->stamp : "");
      if (r->comments != NULL)
        {
          json_builder_set_member_name (b, "c");
          json_builder_add_string_value (b, r->comments);
        }
      if (r->transcript != NULL)
        {
          json_builder_set_member_name (b, "t");
          json_builder_add_string_value (b, r->transcript);
        }
      json_builder_end_object (b);
    }

  json_builder_end_object (b);
  json_builder_end_object (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, json_builder_get_root (b));
  /* NOT pretty-printed, unlike every other file this app writes. This one is
   * machine-read only and is the largest thing in the cache directory;
   * indenting it would add a byte per token for nobody's benefit. */
  json_generator_set_pretty (gen, FALSE);

  GError *error = NULL;
  if (!json_generator_to_file (gen, index->path, &error))
    {
      g_warning ("could not write %s: %s", index->path, error->message);
      g_clear_error (&error);
      return;
    }
  index->dirty = FALSE;
}

/* ---------------------------------------------------------------------- */
/* Building                                                               */
/* ---------------------------------------------------------------------- */

/* What identifies "this folder, as it is now".
 *
 * The manifest's archive_creation_time when there is one, which
 * postprocess.ps1 rewrites on every pass including `ytdl --refresh` -- so a
 * video whose comments were re-fetched re-indexes and one that was merely
 * rescanned does not. Falling back to the info.json's size and mtime covers a
 * folder with no manifest, which the layout contract names as an ordinary
 * state. */
static char *
stamp_for (const YtdlEntry *entry)
{
  if (entry->creation_stamp != NULL && *entry->creation_stamp != '\0')
    return g_strdup (entry->creation_stamp);

  for (guint i = 0; i < entry->files->len; i++)
    {
      const YtdlFile *f = g_ptr_array_index (entry->files, i);
      if (!g_str_has_suffix (f->rel, ".info.json"))
        continue;
      g_autofree char *path = ytdl_entry_path_for_index (entry, i);
      if (path == NULL)
        continue;
      GStatBuf st;
      if (g_stat (path, &st) != 0)
        continue;
      return g_strdup_printf ("%" G_GINT64_FORMAT ":%" G_GUINT64_FORMAT,
                              (gint64) st.st_mtime, (guint64) st.st_size);
    }

  /* Nothing to key on. Such a folder is re-parsed on every build, which costs
   * nothing because there is nothing in it to parse. */
  return g_strdup ("");
}

/* The human-written track if there is one, exactly as the detail page picks
 * it: the auto/human distinction is read from file CONTENTS because it matters
 * which you are reading, and an index built over the auto track when a real
 * one exists would answer differently from the page. */
static char *
best_subtitle_path (const YtdlEntry *entry)
{
  char *best = NULL;
  gboolean best_is_auto = TRUE;

  for (guint i = 0; i < entry->files->len; i++)
    {
      const YtdlFile *f = g_ptr_array_index (entry->files, i);
      if (!ytdl_ext_is_subtitle (f->ext))
        continue;
      g_autofree char *path = ytdl_entry_path_for_index (entry, i);
      if (path == NULL)
        continue;
      gboolean is_auto = ytdl_subtitle_is_auto (path);
      if (best == NULL || (best_is_auto && !is_auto))
        {
          g_free (best);
          best = g_steal_pointer (&path);
          best_is_auto = is_auto;
        }
    }
  return best;
}

static void
flatten_comments (GPtrArray *comments, GPtrArray *out_text)
{
  for (guint i = 0; i < comments->len; i++)
    {
      const YtdlComment *c = g_ptr_array_index (comments, i);
      if (c->text != NULL)
        g_ptr_array_add (out_text, (gpointer) c->text);
      if (c->author != NULL)
        g_ptr_array_add (out_text, (gpointer) c->author);
      if (c->replies != NULL)
        flatten_comments (c->replies, out_text);
    }
}

static void
index_one (YtdlSearchIndex *index, const YtdlEntry *entry)
{
  Record *r = g_new0 (Record, 1);
  r->stamp = stamp_for (entry);

  /* --- comments --- */
  {
    YtdlEntry stub = { 0 };
    stub.dir = entry->dir;
    g_autoptr (YtdlInfo) info = ytdl_entry_load_info (&stub);

    g_autoptr (GPtrArray) tokens = g_ptr_array_new_with_free_func (g_free);
    if (info != NULL)
      {
        g_autoptr (GPtrArray) comments = ytdl_info_comments (info);
        /* Borrowed pointers into the comment tree; freed with it. */
        g_autoptr (GPtrArray) texts = g_ptr_array_new ();
        if (comments != NULL)
          flatten_comments (comments, texts);

        /* The description is indexed with the comments rather than given a
         * scope of its own. It is the uploader's own words about the video,
         * which is what someone searching "comments" is reaching for when
         * they half-remember something that was said about it -- and a fifth
         * scope for one field would be a menu nobody reads. */
        g_autofree char *description = ytdl_info_string (info, "description");
        if (description != NULL)
          g_ptr_array_add (texts, description);

        for (guint i = 0; i < texts->len; i++)
          {
            g_autoptr (GPtrArray) t =
                ytdl_search_tokenize (g_ptr_array_index (texts, i));
            for (guint j = 0; j < t->len; j++)
              g_ptr_array_add (tokens, g_strdup (g_ptr_array_index (t, j)));
          }
      }
    r->comments = token_blob (tokens);
  }

  /* --- transcript --- */
  {
    g_autofree char *sub = best_subtitle_path (entry);
    g_autoptr (GPtrArray) tokens = g_ptr_array_new_with_free_func (g_free);
    if (sub != NULL)
      {
        g_autoptr (GPtrArray) cues = ytdl_parse_subtitle_cues (sub);
        for (guint i = 0; cues != NULL && i < cues->len; i++)
          {
            const YtdlCue *cue = g_ptr_array_index (cues, i);
            g_autoptr (GPtrArray) t = ytdl_search_tokenize (cue->text);
            for (guint j = 0; j < t->len; j++)
              g_ptr_array_add (tokens, g_strdup (g_ptr_array_index (t, j)));
          }
      }
    r->transcript = token_blob (tokens);
  }

  g_hash_table_insert (index->by_key, g_strdup (entry->key), r);
  index->dirty = TRUE;
}

static gboolean
is_current (YtdlSearchIndex *index, const YtdlEntry *entry)
{
  Record *r = g_hash_table_lookup (index->by_key, entry->key);
  if (r == NULL)
    return FALSE;
  g_autofree char *now = stamp_for (entry);
  return g_strcmp0 (r->stamp, now) == 0;
}

guint
ytdl_search_index_size (YtdlSearchIndex *index)
{
  g_return_val_if_fail (index != NULL, 0);
  return g_hash_table_size (index->by_key);
}

guint
ytdl_search_index_outdated (YtdlSearchIndex *index, const YtdlIndex *archive)
{
  g_return_val_if_fail (index != NULL, 0);
  if (archive == NULL || archive->entries == NULL)
    return 0;

  guint n = 0;
  for (guint i = 0; i < archive->entries->len; i++)
    if (!is_current (index, g_ptr_array_index (archive->entries, i)))
      n++;
  return n;
}

void
ytdl_search_index_build (YtdlSearchIndex *index, const YtdlIndex *archive,
                         YtdlSearchProgress progress, gpointer user_data,
                         GCancellable *cancellable)
{
  g_return_if_fail (index != NULL);
  if (archive == NULL || archive->entries == NULL)
    return;

  gsize total = archive->entries->len;
  for (guint i = 0; i < archive->entries->len; i++)
    {
      if (g_cancellable_is_cancelled (cancellable))
        return;

      const YtdlEntry *e = g_ptr_array_index (archive->entries, i);
      if (!is_current (index, e))
        index_one (index, e);

      if (progress != NULL)
        progress ((gsize) i + 1, total, user_data);
    }

  /* Drop videos the archive no longer has, or the store grows forever across
   * rescans of a tree somebody reorganises. Done after the build rather than
   * during it, because the set of live keys is only complete at the end. */
  g_autoptr (GHashTable) live =
      g_hash_table_new (g_str_hash, g_str_equal);
  for (guint i = 0; i < archive->entries->len; i++)
    {
      const YtdlEntry *e = g_ptr_array_index (archive->entries, i);
      g_hash_table_add (live, e->key);
    }

  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init (&it, index->by_key);
  while (g_hash_table_iter_next (&it, &key, NULL))
    {
      if (!g_hash_table_contains (live, key))
        {
          g_hash_table_iter_remove (&it);
          index->dirty = TRUE;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Querying                                                               */
/* ---------------------------------------------------------------------- */

/* Every query token must appear as the PREFIX of some token in @blob.
 *
 * The leading space on the needle is what pins it to a token boundary: " rail"
 * matches the stored " railway " and does not match " guardrail ". Prefix
 * rather than whole-token because this runs as the user types, and a search
 * that returns nothing until the last letter of a word is one people stop
 * using before they finish typing. */
static gboolean
blob_matches (const char *blob, GPtrArray *needles)
{
  if (blob == NULL)
    return FALSE;
  for (guint i = 0; i < needles->len; i++)
    if (strstr (blob, g_ptr_array_index (needles, i)) == NULL)
      return FALSE;
  return TRUE;
}

/* The query's tokens, each already prefixed with the boundary space. */
static GPtrArray *
query_needles (const char *query)
{
  g_autoptr (GPtrArray) tokens = ytdl_search_tokenize (query);
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < tokens->len; i++)
    {
      const char *t = g_ptr_array_index (tokens, i);
      /* A one-character token is kept HERE even though the index does not
       * store one-character tokens: as a prefix it is a perfectly good filter
       * ("a" matches " apple "), and dropping it would make a query narrower
       * as the user typed its first letter and then wider again. */
      g_ptr_array_add (out, g_strconcat (" ", t, NULL));
    }
  return g_steal_pointer (&out);
}

GHashTable *
ytdl_search_index_query (YtdlSearchIndex *index, const char *query,
                         YtdlSearchScope scope)
{
  GHashTable *hits = g_hash_table_new (g_str_hash, g_str_equal);
  g_return_val_if_fail (index != NULL, hits);

  g_autoptr (GPtrArray) needles = query_needles (query);
  if (needles->len == 0)
    return hits;

  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, index->by_key);
  while (g_hash_table_iter_next (&it, &key, &value))
    {
      const Record *r = value;
      gboolean hit = FALSE;

      if (scope == YTDL_SEARCH_COMMENTS || scope == YTDL_SEARCH_EVERYTHING)
        hit = blob_matches (r->comments, needles);
      if (!hit &&
          (scope == YTDL_SEARCH_TRANSCRIPT || scope == YTDL_SEARCH_EVERYTHING))
        hit = blob_matches (r->transcript, needles);

      if (hit)
        g_hash_table_add (hits, key);
    }

  return hits;
}

/* ---------------------------------------------------------------------- */
/* Snippets                                                               */
/* ---------------------------------------------------------------------- */

void
ytdl_snippet_free (gpointer snippet)
{
  YtdlSnippet *s = snippet;
  if (s == NULL)
    return;
  g_free (s->text);
  g_free (s->who);
  g_free (s);
}

/* The same rule blob_matches applies, against text rather than against a
 * stored blob -- so a passage shown as a hit is a passage that would have
 * matched had it been indexed alone. */
static gboolean
text_matches (const char *text, GPtrArray *needles)
{
  g_autoptr (GPtrArray) tokens = ytdl_search_tokenize (text);
  if (tokens->len == 0)
    return FALSE;

  g_autoptr (GString) blob = g_string_new (" ");
  for (guint i = 0; i < tokens->len; i++)
    {
      g_string_append (blob, g_ptr_array_index (tokens, i));
      g_string_append_c (blob, ' ');
    }
  return blob_matches (blob->str, needles);
}

static void
collect_comment_snippets (GPtrArray *comments, GPtrArray *needles,
                          GPtrArray *out, gsize max)
{
  for (guint i = 0; i < comments->len && out->len < max; i++)
    {
      const YtdlComment *c = g_ptr_array_index (comments, i);
      if (c->text != NULL && text_matches (c->text, needles))
        {
          YtdlSnippet *s = g_new0 (YtdlSnippet, 1);
          s->text = g_strdup (c->text);
          s->who = g_strdup (c->author);
          s->from_transcript = FALSE;
          g_ptr_array_add (out, s);
        }
      if (c->replies != NULL)
        collect_comment_snippets (c->replies, needles, out, max);
    }
}

static char *
clock_for (double seconds)
{
  if (seconds < 0)
    return g_strdup ("");
  int total = (int) seconds;
  int h = total / 3600;
  int m = (total % 3600) / 60;
  int s = total % 60;
  if (h > 0)
    return g_strdup_printf ("%d:%02d:%02d", h, m, s);
  return g_strdup_printf ("%d:%02d", m, s);
}

GPtrArray *
ytdl_search_snippets (const YtdlEntry *entry, const char *query,
                      YtdlSearchScope scope, gsize max)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (ytdl_snippet_free);
  g_return_val_if_fail (entry != NULL, out);

  g_autoptr (GPtrArray) needles = query_needles (query);
  if (needles->len == 0 || max == 0)
    return out;

  if (scope == YTDL_SEARCH_COMMENTS || scope == YTDL_SEARCH_EVERYTHING)
    {
      YtdlEntry stub = { 0 };
      stub.dir = entry->dir;
      g_autoptr (YtdlInfo) info = ytdl_entry_load_info (&stub);
      if (info != NULL)
        {
          g_autoptr (GPtrArray) comments = ytdl_info_comments (info);
          if (comments != NULL)
            collect_comment_snippets (comments, needles, out, max);
        }
    }

  if (out->len < max &&
      (scope == YTDL_SEARCH_TRANSCRIPT || scope == YTDL_SEARCH_EVERYTHING))
    {
      g_autofree char *sub = best_subtitle_path (entry);
      g_autoptr (GPtrArray) cues =
          sub != NULL ? ytdl_parse_subtitle_cues (sub) : NULL;

      /* Cues are glued into passages before matching: one cue is a few words,
       * so a two-word query almost never falls inside one, and matching per
       * cue would report "no transcript hits" for a video whose transcript
       * plainly contains the phrase. */
      GString *passage = g_string_new (NULL);
      double start = -1;
      for (guint i = 0; cues != NULL && i < cues->len && out->len < max; i++)
        {
          const YtdlCue *cue = g_ptr_array_index (cues, i);
          if (cue->text == NULL || *cue->text == '\0')
            continue;
          if (start < 0)
            start = cue->start;
          if (passage->len > 0)
            g_string_append_c (passage, ' ');
          g_string_append (passage, cue->text);

          gboolean last = (i + 1 == cues->len);
          if (passage->len < TRANSCRIPT_PASSAGE && !last)
            continue;

          if (text_matches (passage->str, needles))
            {
              YtdlSnippet *s = g_new0 (YtdlSnippet, 1);
              s->text = g_strdup (passage->str);
              s->who = clock_for (start);
              s->from_transcript = TRUE;
              g_ptr_array_add (out, s);
            }
          g_string_truncate (passage, 0);
          start = -1;
        }
      g_string_free (passage, TRUE);
    }

  return out;
}
