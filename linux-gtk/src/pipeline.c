#include "pipeline.h"

#include "paths.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_LOG_LINES 4000
#define MAX_HISTORY   300

/* How often the main thread drains what the worker produced. yt-dlp redraws
 * its progress line several times a second; at 50ms the bar still looks
 * continuous and the UI thread wakes 20 times a second instead of hundreds. */
#define DRAIN_INTERVAL_MS 50

static gint64
now_secs (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

/* ---------------------------------------------------------------------- */
/* Options                                                                */
/* ---------------------------------------------------------------------- */

YtdlRunOptions *
ytdl_run_options_new (void)
{
  YtdlRunOptions *o = g_new0 (YtdlRunOptions, 1);
  o->ytdlp_args = g_ptr_array_new_with_free_func (g_free);
  return o;
}

void
ytdl_run_options_free (YtdlRunOptions *o)
{
  if (o == NULL)
    return;
  g_free (o->url);
  g_free (o->data_root);
  g_free (o->items);
  g_free (o->after);
  g_free (o->mode);
  g_free (o->quality);
  g_free (o->codec);
  g_free (o->audio_codec);
  g_free (o->container);
  g_clear_pointer (&o->ytdlp_args, g_ptr_array_unref);
  g_free (o);
}

YtdlRunOptions *
ytdl_run_options_copy (const YtdlRunOptions *s)
{
  if (s == NULL)
    return NULL;
  YtdlRunOptions *o = ytdl_run_options_new ();
  o->url = g_strdup (s->url);
  o->data_root = g_strdup (s->data_root);
  o->sync = s->sync;
  o->items = g_strdup (s->items);
  o->after = g_strdup (s->after);
  o->lazy = s->lazy;
  o->workers = s->workers;
  o->no_pot = s->no_pot;
  o->skip_pot_update = s->skip_pot_update;
  o->pot_port = s->pot_port;
  o->mode = g_strdup (s->mode);
  o->quality = g_strdup (s->quality);
  o->codec = g_strdup (s->codec);
  o->audio_codec = g_strdup (s->audio_codec);
  o->container = g_strdup (s->container);
  o->no_comments = s->no_comments;
  o->no_subs = s->no_subs;
  o->no_thumbnail = s->no_thumbnail;
  o->no_metadata = s->no_metadata;
  if (s->ytdlp_args != NULL)
    for (guint i = 0; i < s->ytdlp_args->len; i++)
      g_ptr_array_add (o->ytdlp_args,
                       g_strdup (g_ptr_array_index (s->ytdlp_args, i)));
  return o;
}

char *
ytdl_normalize_url (const char *url)
{
  g_autofree char *u = g_strdup (url != NULL ? url : "");
  g_strstrip (u);

  if (strstr (u, "://") != NULL)
    return g_steal_pointer (&u);

  gboolean looks_like_id = strlen (u) == 11;
  for (const char *p = u; looks_like_id && *p; p++)
    if (!g_ascii_isalnum (*p) && *p != '-' && *p != '_')
      looks_like_id = FALSE;
  if (looks_like_id)
    return g_strdup_printf ("https://www.youtube.com/watch?v=%s", u);

  if (g_str_has_prefix (u, "youtube.com") ||
      g_str_has_prefix (u, "www.youtube.com") ||
      g_str_has_prefix (u, "youtu.be"))
    return g_strdup_printf ("https://%s", u);

  return g_steal_pointer (&u);
}

static gboolean
nonempty (const char *s)
{
  if (s == NULL)
    return FALSE;
  for (const char *p = s; *p; p++)
    if (!g_ascii_isspace (*p))
      return TRUE;
  return FALSE;
}

/* Emitted only when it differs from the pipeline's own default, so a plain
 * download produces exactly the command line it produced before these options
 * existed. That matters for more than tidiness: --quality best, --codec any
 * and --mode full are no-ops the pipeline would accept and ignore, but
 * emitting them would put four extra flags in the preview of every ordinary
 * download and make the common case look complicated. */
static void
push_non_default (GPtrArray *v, const char *flag, const char *value,
                  const char *dflt)
{
  if (!nonempty (value))
    return;
  g_autofree char *t = g_strdup (value);
  g_strstrip (t);
  if (g_strcmp0 (t, dflt) == 0)
    return;
  g_ptr_array_add (v, g_strdup (flag));
  g_ptr_array_add (v, g_steal_pointer (&t));
}

GStrv
ytdl_run_options_to_args (const YtdlRunOptions *o)
{
  g_return_val_if_fail (o != NULL, NULL);
  GPtrArray *v = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (v, ytdl_normalize_url (o->url));

  if (nonempty (o->data_root))
    {
      /* Expand ~ HERE, at the one point a path leaves this process.
       *
       * run_ytdlp.ps1 resolves -DataRoot with [System.IO.Path]::GetFullPath,
       * which has no notion of a home directory: "~/Videos" reaches it as a
       * literal "~" folder under the pipeline process's working directory.
       * Meanwhile this app's own expand_tilde DID expand it, so the same
       * typed path pointed at two different places -- the library indexed one
       * and the downloads went to the other, with nothing reporting an error. */
      g_autofree char *trimmed = g_strdup (o->data_root);
      g_ptr_array_add (v, g_strdup ("--path"));
      g_ptr_array_add (v, ytdl_expand_tilde (g_strstrip (trimmed)));
    }
  if (o->sync)
    g_ptr_array_add (v, g_strdup ("--sync"));
  if (nonempty (o->items))
    {
      g_autofree char *t = g_strdup (o->items);
      g_ptr_array_add (v, g_strdup ("--items"));
      g_ptr_array_add (v, g_strdup (g_strstrip (t)));
    }
  if (nonempty (o->after))
    {
      g_autofree char *t = g_strdup (o->after);
      g_ptr_array_add (v, g_strdup ("--after"));
      g_ptr_array_add (v, g_strdup (g_strstrip (t)));
    }
  if (o->lazy)
    g_ptr_array_add (v, g_strdup ("--lazy"));
  if (o->workers > 1)
    {
      g_ptr_array_add (v, g_strdup ("--workers"));
      g_ptr_array_add (v, g_strdup_printf ("%u", o->workers));
    }
  if (o->no_pot)
    g_ptr_array_add (v, g_strdup ("--no-pot"));
  if (o->skip_pot_update)
    g_ptr_array_add (v, g_strdup ("--skip-pot-update"));
  if (o->pot_port > 0)
    {
      g_ptr_array_add (v, g_strdup ("--pot-port"));
      g_ptr_array_add (v, g_strdup_printf ("%u", o->pot_port));
    }

  push_non_default (v, "--mode", o->mode, "full");
  push_non_default (v, "--quality", o->quality, "best");
  push_non_default (v, "--codec", o->codec, "any");
  push_non_default (v, "--audio-codec", o->audio_codec, "any");
  push_non_default (v, "--container", o->container, "mkv");

  if (o->no_comments)
    g_ptr_array_add (v, g_strdup ("--no-comments"));
  if (o->no_subs)
    g_ptr_array_add (v, g_strdup ("--no-subs"));
  if (o->no_thumbnail)
    g_ptr_array_add (v, g_strdup ("--no-thumbnail"));
  if (o->no_metadata)
    g_ptr_array_add (v, g_strdup ("--no-metadata"));

  /* Repeated rather than joined: ytdl.ps1 takes one value per occurrence, and
   * a real --match-filter expression contains commas and spaces, so no
   * separator would be safe to join on. */
  if (o->ytdlp_args != NULL)
    for (guint i = 0; i < o->ytdlp_args->len; i++)
      {
        const char *raw = g_ptr_array_index (o->ytdlp_args, i);
        if (!nonempty (raw))
          continue;
        g_autofree char *t = g_strdup (raw);
        g_ptr_array_add (v, g_strdup ("--ytdlp-arg"));
        g_ptr_array_add (v, g_strdup (g_strstrip (t)));
      }

  g_ptr_array_add (v, NULL);
  return (GStrv) g_ptr_array_free (v, FALSE);
}

char *
ytdl_run_options_command_preview (const YtdlRunOptions *o)
{
  g_auto (GStrv) args = ytdl_run_options_to_args (o);
  GString *s = g_string_new ("ytdl");
  for (gsize i = 0; args[i] != NULL; i++)
    {
      g_string_append_c (s, ' ');
      if (i == 0 || strchr (args[i], ' ') != NULL)
        g_string_append_printf (s, "\"%s\"", args[i]);
      else
        g_string_append (s, args[i]);
    }
  return g_string_free (s, FALSE);
}

/* ---------------------------------------------------------------------- */
/* Records                                                                */
/* ---------------------------------------------------------------------- */

void
ytdl_progress_clear (YtdlProgress *p)
{
  if (p == NULL)
    return;
  g_clear_pointer (&p->speed, g_free);
  g_clear_pointer (&p->eta, g_free);
  g_clear_pointer (&p->total, g_free);
  g_clear_pointer (&p->stage, g_free);
  g_clear_pointer (&p->video_id, g_free);
  g_clear_pointer (&p->destination, g_free);
  p->percent = -1.0;
}

static void
progress_copy (YtdlProgress *dst, const YtdlProgress *src)
{
  ytdl_progress_clear (dst);
  dst->percent = src->percent;
  dst->speed = g_strdup (src->speed);
  dst->eta = g_strdup (src->eta);
  dst->total = g_strdup (src->total);
  dst->stage = g_strdup (src->stage);
  dst->video_id = g_strdup (src->video_id);
  dst->destination = g_strdup (src->destination);
}

void
ytdl_run_record_free (YtdlRunRecord *r)
{
  if (r == NULL)
    return;
  g_free (r->id);
  g_free (r->command);
  g_free (r->state);
  g_free (r->log_path);
  g_free (r->last_line);
  g_clear_pointer (&r->opts, ytdl_run_options_free);
  g_free (r);
}

static YtdlRunRecord *
record_copy (const YtdlRunRecord *s)
{
  if (s == NULL)
    return NULL;
  YtdlRunRecord *r = g_new0 (YtdlRunRecord, 1);
  r->id = g_strdup (s->id);
  r->command = g_strdup (s->command);
  r->started = s->started;
  r->finished = s->finished;
  r->state = g_strdup (s->state);
  r->exit_code = s->exit_code;
  r->videos_touched = s->videos_touched;
  r->archive_skipped = s->archive_skipped;
  r->errors = s->errors;
  r->warnings = s->warnings;
  r->log_path = g_strdup (s->log_path);
  r->last_line = g_strdup (s->last_line);
  r->opts = ytdl_run_options_copy (s->opts);
  return r;
}

/* ---------------------------------------------------------------------- */
/* Output parsing                                                         */
/* ---------------------------------------------------------------------- */

/* Strip ANSI escape sequences. yt-dlp turns colour off when stdout is not a
 * terminal, but pwsh does not always, and a progress line full of escape
 * bytes renders as garbage in a label. */
char *
ytdl_strip_ansi (const char *s)
{
  g_return_val_if_fail (s != NULL, NULL);
  GString *out = g_string_sized_new (strlen (s));
  for (const char *p = s; *p;)
    {
      if (*p == '\x1b')
        {
          p++;
          if (*p == '[')
            {
              p++;
              while (*p && !g_ascii_isalpha (*p))
                p++;
              if (*p)
                p++;
            }
          continue;
        }
      g_string_append_c (out, *p++);
    }
  return g_string_free (out, FALSE);
}

static void
set_str (char **slot, const char *value)
{
  g_free (*slot);
  *slot = g_strdup (value);
}

/* yt-dlp's own progress line, e.g.
 *   [download]  45.2% of  120.00MiB at   2.00MiB/s ETA 00:30
 * Scanned by token rather than with a regex, to keep the dependency list to
 * what the build actually needs. Returns TRUE when @inout was changed. */
gboolean
ytdl_parse_progress_line (const char *line, YtdlProgress *p)
{
  g_return_val_if_fail (line != NULL && p != NULL, FALSE);

  g_autofree char *trimmed = g_strdup (line);
  g_strstrip (trimmed);

  if (g_str_has_prefix (trimmed, "[download]"))
    {
      const char *rest = trimmed + strlen ("[download]");

      g_autofree char *rtrim = g_strdup (rest);
      g_strstrip (rtrim);
      if (g_str_has_prefix (rtrim, "Destination:"))
        {
          g_autofree char *d = g_strdup (rtrim + strlen ("Destination:"));
          set_str (&p->destination, g_strstrip (d));
          set_str (&p->stage, "downloading");
          return TRUE;
        }

      g_auto (GStrv) toks = g_strsplit_set (rest, " \t", -1);

      /* Runs of spaces make g_strsplit_set emit empty tokens, and the
       * progress line is full of them ("45.2% of  120.00MiB at   2.00MiB/s").
       *
       * Compacting toks IN PLACE looks like the obvious way to drop them and
       * is a double free: overwriting toks[n] with a later pointer leaves the
       * original still sitting further down the array, and g_strfreev walks
       * to the unchanged NULL terminator and frees it twice. A separate array
       * of BORROWED pointers leaves toks exactly as g_strsplit_set built it. */
      g_autoptr (GPtrArray) t = g_ptr_array_new ();
      for (gsize i = 0; toks[i] != NULL; i++)
        if (toks[i][0] != '\0')
          g_ptr_array_add (t, toks[i]);

      gboolean matched = FALSE;
      const gsize n = t->len;
      const char *const *tok = (const char *const *) t->pdata;

      for (gsize i = 0; i < n; i++)
        {
          gsize len = strlen (tok[i]);
          if (len > 1 && tok[i][len - 1] == '%')
            {
              g_autofree char *num = g_strndup (tok[i], len - 1);
              char *end = NULL;
              double v = g_ascii_strtod (num, &end);
              if (end != NULL && *end == '\0')
                {
                  p->percent = v;
                  set_str (&p->stage, "downloading");
                  matched = TRUE;
                }
            }
          if (i + 1 < n)
            {
              if (g_strcmp0 (tok[i], "of") == 0)
                set_str (&p->total, tok[i + 1]);
              else if (g_strcmp0 (tok[i], "at") == 0)
                set_str (&p->speed, tok[i + 1]);
              else if (g_strcmp0 (tok[i], "ETA") == 0)
                set_str (&p->eta, tok[i + 1]);
            }
        }
      if (matched)
        return TRUE;
    }

  /* Stage markers worth surfacing, all emitted by the pipeline itself or by
   * yt-dlp's own extractor chatter. */
  static const struct
  {
    const char *needle;
    const char *stage;
  } stages[] = {
    { "[Merger]", "merging" },
    { "[Metadata]", "embedding metadata" },
    { "[EmbedSubtitle]", "embedding subtitles" },
    { "[ThumbnailsConvertor]", "thumbnail" },
    { "[postprocess]", "post-processing" },
    { "Fetching comments", "fetching comments" },
    { "[info] Writing video subtitles", "subtitles" },
    { "-- Enumerating videos", "enumerating" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (stages); i++)
    if (strstr (line, stages[i].needle) != NULL)
      {
        set_str (&p->stage, stages[i].stage);
        p->percent = -1.0;
        return TRUE;
      }

  /* "[youtube] dQw4w9WgXcQ: Downloading webpage" -- the id of the video the
   * session is currently on, which is the only reliable per-video marker in a
   * multi-video run. */
  if (g_str_has_prefix (trimmed, "[youtube] "))
    {
      const char *rest = trimmed + strlen ("[youtube] ");
      const char *colon = strchr (rest, ':');
      if (colon != NULL)
        {
          g_autofree char *id = g_strndup (rest, (gsize) (colon - rest));
          g_strstrip (id);
          if (strlen (id) == 11 && strchr (id, ' ') == NULL)
            {
              set_str (&p->video_id, id);
              return TRUE;
            }
        }
    }

  return FALSE;
}

/* run_ytdlp.ps1 closes every session with
 *   -- Session summary: N video(s) touched, M already archived (skipped),
 *      E error(s), W warning(s) --
 * which is the only place those counts exist. Parsed here so a history row can
 * show them without re-reading download.log.
 *
 * Each whitespace token has its non-digit edges trimmed before parsing, so
 * "3," and "(skipped)" behave -- the latter yielding nothing. */
gboolean
ytdl_parse_session_summary (const char *line, gint64 *videos, gint64 *skipped,
                            gint64 *errors, gint64 *warnings)
{
  if (line == NULL || strstr (line, "Session summary:") == NULL)
    return FALSE;

  g_auto (GStrv) toks = g_strsplit_set (line, " \t", -1);
  gint64 nums[4];
  gsize found = 0;

  for (gsize i = 0; toks[i] != NULL && found < 4; i++)
    {
      const char *t = toks[i];
      const char *start = t;
      while (*start && !g_ascii_isdigit (*start))
        start++;
      const char *end = t + strlen (t);
      while (end > start && !g_ascii_isdigit (*(end - 1)))
        end--;
      if (end <= start)
        continue;

      g_autofree char *digits = g_strndup (start, (gsize) (end - start));
      gboolean all_digits = TRUE;
      for (const char *p = digits; *p; p++)
        if (!g_ascii_isdigit (*p))
          all_digits = FALSE;
      if (!all_digits)
        continue;

      nums[found++] = g_ascii_strtoll (digits, NULL, 10);
    }

  if (found < 4)
    return FALSE;
  if (videos)
    *videos = nums[0];
  if (skipped)
    *skipped = nums[1];
  if (errors)
    *errors = nums[2];
  if (warnings)
    *warnings = nums[3];
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* The runner                                                             */
/* ---------------------------------------------------------------------- */

typedef struct
{
  char    *text;
  gboolean transient;
} LineEvent;

static void
line_event_free (gpointer p)
{
  LineEvent *e = p;
  if (e == NULL)
    return;
  g_free (e->text);
  g_free (e);
}

struct _YtdlRunner
{
  GObject parent_instance;

  GMutex   lock;
  GCond    wake;
  GPtrArray *queue;   /* YtdlRunRecord* */
  GPtrArray *history; /* YtdlRunRecord* */
  YtdlRunRecord *current;
  YtdlProgress   progress;
  GPtrArray     *log; /* char* */
  gboolean       paused;
  GPid           child_pid; /* 0 when nothing is running */

  gint      cancel_flag;
  gint      stop_flag;
  gint      state_dirty;
  GThread  *worker;

  GAsyncQueue *lines; /* LineEvent*, worker -> main */
  guint        drain_id;
  guint        counter;
};

enum
{
  SIG_STATE_CHANGED,
  SIG_LINE,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (YtdlRunner, ytdl_runner, G_TYPE_OBJECT)

/* ---------------------------------------------------------------------- */
/* Persistence                                                            */
/* ---------------------------------------------------------------------- */

static char *
state_file (const char *name)
{
  g_autofree char *dir = ytdl_state_dir ();
  return g_build_filename (dir, name, NULL);
}

static void
build_options (JsonBuilder *b, const YtdlRunOptions *o)
{
  json_builder_begin_object (b);
#define S(k, v)                                                               \
  do                                                                          \
    {                                                                         \
      if ((v) != NULL)                                                        \
        {                                                                     \
          json_builder_set_member_name (b, k);                                \
          json_builder_add_string_value (b, v);                               \
        }                                                                     \
    }                                                                         \
  while (0)
#define B(k, v)                                                               \
  do                                                                          \
    {                                                                         \
      json_builder_set_member_name (b, k);                                    \
      json_builder_add_boolean_value (b, v);                                  \
    }                                                                         \
  while (0)

  S ("url", o->url);
  S ("data_root", o->data_root);
  B ("sync", o->sync);
  S ("items", o->items);
  S ("after", o->after);
  B ("lazy", o->lazy);
  json_builder_set_member_name (b, "workers");
  json_builder_add_int_value (b, o->workers);
  B ("no_pot", o->no_pot);
  B ("skip_pot_update", o->skip_pot_update);
  json_builder_set_member_name (b, "pot_port");
  json_builder_add_int_value (b, o->pot_port);
  S ("mode", o->mode);
  S ("quality", o->quality);
  S ("codec", o->codec);
  S ("audio_codec", o->audio_codec);
  S ("container", o->container);
  B ("no_comments", o->no_comments);
  B ("no_subs", o->no_subs);
  B ("no_thumbnail", o->no_thumbnail);
  B ("no_metadata", o->no_metadata);
#undef S
#undef B

  json_builder_set_member_name (b, "ytdlp_args");
  json_builder_begin_array (b);
  if (o->ytdlp_args != NULL)
    for (guint i = 0; i < o->ytdlp_args->len; i++)
      json_builder_add_string_value (b, g_ptr_array_index (o->ytdlp_args, i));
  json_builder_end_array (b);

  json_builder_end_object (b);
}

/* Every field is optional on read. A queue or history file written by an older
 * build simply does not set what it did not know about, which is the C
 * equivalent of the Rust side's #[serde(default)] -- and the reason a profile
 * or a queued run survives an upgrade instead of failing the whole file. */
static YtdlRunOptions *
parse_options (JsonObject *obj)
{
  YtdlRunOptions *o = ytdl_run_options_new ();
  if (obj == NULL)
    return o;

#define GS(k, slot)                                                           \
  do                                                                          \
    {                                                                         \
      if (json_object_has_member (obj, k))                                    \
        {                                                                     \
          JsonNode *n = json_object_get_member (obj, k);                      \
          if (JSON_NODE_HOLDS_VALUE (n) &&                                    \
              json_node_get_value_type (n) == G_TYPE_STRING)                  \
            slot = g_strdup (json_node_get_string (n));                       \
        }                                                                     \
    }                                                                         \
  while (0)
#define GB(k, slot)                                                           \
  do                                                                          \
    {                                                                         \
      if (json_object_has_member (obj, k))                                    \
        slot = json_object_get_boolean_member (obj, k);                       \
    }                                                                         \
  while (0)

  GS ("url", o->url);
  GS ("data_root", o->data_root);
  GB ("sync", o->sync);
  GS ("items", o->items);
  GS ("after", o->after);
  GB ("lazy", o->lazy);
  if (json_object_has_member (obj, "workers"))
    o->workers = (guint) json_object_get_int_member (obj, "workers");
  GB ("no_pot", o->no_pot);
  GB ("skip_pot_update", o->skip_pot_update);
  if (json_object_has_member (obj, "pot_port"))
    o->pot_port = (guint) json_object_get_int_member (obj, "pot_port");
  GS ("mode", o->mode);
  GS ("quality", o->quality);
  GS ("codec", o->codec);
  GS ("audio_codec", o->audio_codec);
  GS ("container", o->container);
  GB ("no_comments", o->no_comments);
  GB ("no_subs", o->no_subs);
  GB ("no_thumbnail", o->no_thumbnail);
  GB ("no_metadata", o->no_metadata);
#undef GS
#undef GB

  if (json_object_has_member (obj, "ytdlp_args"))
    {
      JsonNode *n = json_object_get_member (obj, "ytdlp_args");
      if (JSON_NODE_HOLDS_ARRAY (n))
        {
          JsonArray *a = json_node_get_array (n);
          for (guint i = 0; i < json_array_get_length (a); i++)
            g_ptr_array_add (o->ytdlp_args,
                             g_strdup (json_array_get_string_element (a, i)));
        }
    }
  return o;
}

static void
build_record (JsonBuilder *b, const YtdlRunRecord *r)
{
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "id");
  json_builder_add_string_value (b, r->id);
  json_builder_set_member_name (b, "command");
  json_builder_add_string_value (b, r->command);
  json_builder_set_member_name (b, "started");
  json_builder_add_int_value (b, r->started);
  json_builder_set_member_name (b, "finished");
  json_builder_add_int_value (b, r->finished);
  json_builder_set_member_name (b, "state");
  json_builder_add_string_value (b, r->state);
  json_builder_set_member_name (b, "exit_code");
  json_builder_add_int_value (b, r->exit_code);
  json_builder_set_member_name (b, "videos_touched");
  json_builder_add_int_value (b, r->videos_touched);
  json_builder_set_member_name (b, "archive_skipped");
  json_builder_add_int_value (b, r->archive_skipped);
  json_builder_set_member_name (b, "errors");
  json_builder_add_int_value (b, r->errors);
  json_builder_set_member_name (b, "warnings");
  json_builder_add_int_value (b, r->warnings);
  json_builder_set_member_name (b, "log_path");
  json_builder_add_string_value (b, r->log_path != NULL ? r->log_path : "");
  json_builder_set_member_name (b, "last_line");
  json_builder_add_string_value (b, r->last_line != NULL ? r->last_line : "");
  json_builder_set_member_name (b, "opts");
  build_options (b, r->opts);
  json_builder_end_object (b);
}

static YtdlRunRecord *
parse_record (JsonObject *obj)
{
  YtdlRunRecord *r = g_new0 (YtdlRunRecord, 1);
  r->videos_touched = -1;
  r->archive_skipped = -1;
  r->errors = -1;
  r->warnings = -1;

  if (json_object_has_member (obj, "id"))
    r->id = g_strdup (json_object_get_string_member (obj, "id"));
  if (json_object_has_member (obj, "command"))
    r->command = g_strdup (json_object_get_string_member (obj, "command"));
  if (json_object_has_member (obj, "state"))
    r->state = g_strdup (json_object_get_string_member (obj, "state"));
  if (json_object_has_member (obj, "started"))
    r->started = json_object_get_int_member (obj, "started");
  if (json_object_has_member (obj, "finished"))
    r->finished = json_object_get_int_member (obj, "finished");
  if (json_object_has_member (obj, "exit_code"))
    r->exit_code = (int) json_object_get_int_member (obj, "exit_code");
  if (json_object_has_member (obj, "videos_touched"))
    r->videos_touched = json_object_get_int_member (obj, "videos_touched");
  if (json_object_has_member (obj, "archive_skipped"))
    r->archive_skipped = json_object_get_int_member (obj, "archive_skipped");
  if (json_object_has_member (obj, "errors"))
    r->errors = json_object_get_int_member (obj, "errors");
  if (json_object_has_member (obj, "warnings"))
    r->warnings = json_object_get_int_member (obj, "warnings");
  if (json_object_has_member (obj, "log_path"))
    r->log_path = g_strdup (json_object_get_string_member (obj, "log_path"));
  if (json_object_has_member (obj, "last_line"))
    r->last_line = g_strdup (json_object_get_string_member (obj, "last_line"));

  JsonObject *opts = NULL;
  if (json_object_has_member (obj, "opts"))
    {
      JsonNode *n = json_object_get_member (obj, "opts");
      if (JSON_NODE_HOLDS_OBJECT (n))
        opts = json_node_get_object (n);
    }
  r->opts = parse_options (opts);

  if (r->id == NULL)
    r->id = g_strdup ("");
  if (r->state == NULL)
    r->state = g_strdup ("done");
  if (r->command == NULL)
    r->command = g_strdup ("");
  return r;
}

/* Written through a temp file and renamed.
 *
 * The queue and the history are the same category of data as profiles.json:
 * losing a queue somebody built up, or the record of what ran overnight, is
 * losing real work. A truncated write from a crash or a full disk would take
 * all of it, and rename(2) is atomic. */
static void
write_records_atomic (const char *path, GPtrArray *records)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_array (b);
  for (guint i = 0; i < records->len; i++)
    build_record (b, g_ptr_array_index (records, i));
  json_builder_end_array (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_pretty (gen, TRUE);
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  json_generator_set_root (gen, root);

  g_autofree char *text = json_generator_to_data (gen, NULL);
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0755);

  g_autofree char *tmp = g_strconcat (path, ".tmp", NULL);
  if (g_file_set_contents (tmp, text, -1, NULL))
    {
      if (g_rename (tmp, path) != 0)
        g_unlink (tmp);
    }
}

static GPtrArray *
read_records (const char *path)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (
      (GDestroyNotify) ytdl_run_record_free);

  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, path, NULL))
    return out;

  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    return out;

  JsonArray *a = json_node_get_array (root);
  for (guint i = 0; i < json_array_get_length (a); i++)
    {
      JsonNode *n = json_array_get_element (a, i);
      if (JSON_NODE_HOLDS_OBJECT (n))
        g_ptr_array_add (out, parse_record (json_node_get_object (n)));
    }
  return out;
}

/* Caller holds the lock. */
static void
persist_locked (YtdlRunner *self)
{
  g_autofree char *hp = state_file ("history.json");
  g_autofree char *qp = state_file ("queue.json");
  write_records_atomic (hp, self->history);
  write_records_atomic (qp, self->queue);
}

/* ---------------------------------------------------------------------- */
/* Worker -> main thread                                                  */
/* ---------------------------------------------------------------------- */

static void
mark_state_dirty (YtdlRunner *self)
{
  g_atomic_int_set (&self->state_dirty, 1);
}

static void
push_line (YtdlRunner *self, const char *text, gboolean transient)
{
  LineEvent *e = g_new0 (LineEvent, 1);
  e->text = g_strdup (text);
  e->transient = transient;
  g_async_queue_push (self->lines, e);
}

static gboolean
drain (gpointer user_data)
{
  YtdlRunner *self = user_data;

  /* Bounded per tick. A run that produces output faster than the UI can draw
   * must not let this loop starve the frame clock -- the backlog simply moves
   * on the next tick, 50ms later. */
  for (int i = 0; i < 400; i++)
    {
      LineEvent *e = g_async_queue_try_pop (self->lines);
      if (e == NULL)
        break;
      g_signal_emit (self, signals[SIG_LINE], 0, e->text, e->transient);
      line_event_free (e);
    }

  if (g_atomic_int_compare_and_exchange (&self->state_dirty, 1, 0))
    g_signal_emit (self, signals[SIG_STATE_CHANGED], 0);

  return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------------- */
/* Running one item                                                       */
/* ---------------------------------------------------------------------- */

/* Kill the whole tree, not just the child.
 *
 * ytdl.ps1 starts run_ytdlp.ps1 as a CHILD pwsh process, which starts yt-dlp,
 * which starts postprocess.ps1 and ffmpeg. Killing only the process this app
 * spawned would leave a download running with nothing reading its output. The
 * run gets its own process group (see child_setup) and the GROUP is signalled.
 */
static void
kill_tree (GPid pid)
{
  if (pid <= 0)
    return;
  kill (-(pid_t) pid, SIGTERM);
  g_usleep (1500 * 1000);
  kill (-(pid_t) pid, SIGKILL);
}

/* Runs in the forked child between fork and exec, so it must be
 * async-signal-safe. setpgid is. This is what makes kill_tree's negative pid
 * reach every descendant. */
static void
child_setup (gpointer user_data)
{
  setpgid (0, 0);
}

typedef struct
{
  YtdlRunner *runner;
  int         fd;
  const char *stream;
} PumpArgs;

/* Reads a child stream byte-wise and splits on BOTH \n and \r.
 *
 * This is not defensiveness: yt-dlp redraws its progress line with a carriage
 * return and yt-dlp.conf sets no --newline, so a line-oriented reader would
 * either block until the download finished or deliver one enormous line.
 * Splitting on \r as well is what makes the progress bar move. */
static gpointer
pump_thread (gpointer data)
{
  PumpArgs *args = data;
  YtdlRunner *self = args->runner;
  GString *buf = g_string_new (NULL);
  char byte;

  for (;;)
    {
      gssize n = read (args->fd, &byte, 1);
      if (n <= 0)
        {
          if (n < 0 && errno == EINTR)
            continue;
          break;
        }

      if (byte != '\n' && byte != '\r')
        {
          g_string_append_c (buf, byte);
          /* A single line this long is not a line; drop it rather than let a
           * runaway stream grow the buffer without bound. */
          if (buf->len > 64 * 1024)
            g_string_truncate (buf, 0);
          continue;
        }

      gboolean transient = (byte == '\r');
      g_autofree char *text = ytdl_strip_ansi (buf->str);
      g_string_truncate (buf, 0);

      g_autofree char *probe = g_strdup (text);
      if (*g_strstrip (probe) == '\0')
        continue;

      g_mutex_lock (&self->lock);
      if (ytdl_parse_progress_line (text, &self->progress))
        mark_state_dirty (self);

      if (!transient)
        {
          g_ptr_array_add (self->log, g_strdup (text));
          if (self->log->len > MAX_LOG_LINES)
            g_ptr_array_remove_range (self->log, 0,
                                      self->log->len - MAX_LOG_LINES);
        }
      if (self->current != NULL)
        {
          set_str (&self->current->last_line, text);
          gint64 v, s, e, w;
          if (ytdl_parse_session_summary (text, &v, &s, &e, &w))
            {
              self->current->videos_touched = v;
              self->current->archive_skipped = s;
              self->current->errors = e;
              self->current->warnings = w;
            }
          mark_state_dirty (self);
        }
      g_mutex_unlock (&self->lock);

      push_line (self, text, transient);
    }

  g_string_free (buf, TRUE);
  close (args->fd);
  g_free (args);
  return NULL;
}

/* Caller does NOT hold the lock. */
static void
finish_run (YtdlRunner *self, YtdlRunRecord *rec)
{
  g_mutex_lock (&self->lock);
  g_clear_pointer (&self->current, ytdl_run_record_free);
  self->child_pid = 0;
  ytdl_progress_clear (&self->progress);
  g_ptr_array_insert (self->history, 0, rec);
  if (self->history->len > MAX_HISTORY)
    g_ptr_array_remove_range (self->history, MAX_HISTORY,
                              self->history->len - MAX_HISTORY);
  persist_locked (self);
  g_mutex_unlock (&self->lock);
  mark_state_dirty (self);
}

static void
fail_run (YtdlRunner *self, YtdlRunRecord *rec, const char *why)
{
  set_str (&rec->state, "failed");
  rec->finished = now_secs ();
  set_str (&rec->last_line, why);
  push_line (self, why, FALSE);
  finish_run (self, rec);
}

static void
run_one (YtdlRunner *self, YtdlRunRecord *rec)
{
  g_autofree char *pwsh = ytdl_find_pwsh ();
  if (pwsh == NULL)
    {
      fail_run (self, rec,
                "pwsh (PowerShell 7) was not found. Every stage of this "
                "pipeline is a PowerShell script, so nothing can run without "
                "it -- install it and re-run setup.");
      return;
    }

  g_autofree char *scripts = ytdl_scripts_dir ();
  g_autofree char *script = g_build_filename (scripts, "ytdl.ps1", NULL);
  if (!g_file_test (script, G_FILE_TEST_IS_REGULAR))
    {
      g_autofree char *msg = g_strdup_printf (
          "%s does not exist. This app drives the installed pipeline, not a "
          "checkout -- run the installer, or set YTDLP_INSTALL_ROOT to where "
          "it lives.",
          script);
      fail_run (self, rec, msg);
      return;
    }

  /* Where this run will write. --workers > 1 splits into
   * download.worker-<id>.log files instead, which the history row links to by
   * directory rather than by name. */
  g_autofree char *data_root =
      nonempty (rec->opts->data_root)
          ? ytdl_expand_tilde (rec->opts->data_root)
          : ytdl_install_root ();
  set_str (&rec->log_path, NULL);
  rec->log_path =
      g_build_filename (data_root, "Archive Logs", "Logs", "download.log", NULL);

  g_mutex_lock (&self->lock);
  g_ptr_array_set_size (self->log, 0);
  ytdl_progress_clear (&self->progress);
  self->progress.stage = g_strdup ("starting");
  self->current = record_copy (rec);
  g_mutex_unlock (&self->lock);
  g_atomic_int_set (&self->cancel_flag, 0);
  mark_state_dirty (self);

  g_auto (GStrv) opt_args = ytdl_run_options_to_args (rec->opts);
  GPtrArray *argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv, g_strdup (pwsh));
  g_ptr_array_add (argv, g_strdup ("-NoProfile"));
  g_ptr_array_add (argv, g_strdup ("-File"));
  g_ptr_array_add (argv, g_strdup (script));
  for (gsize i = 0; opt_args[i] != NULL; i++)
    g_ptr_array_add (argv, g_strdup (opt_args[i]));
  g_ptr_array_add (argv, NULL);

  GPid pid = 0;
  int out_fd = -1, err_fd = -1;
  GError *error = NULL;
  gboolean spawned = g_spawn_async_with_pipes (
      NULL, (char **) argv->pdata, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
      child_setup, NULL, &pid, NULL, &out_fd, &err_fd, &error);
  /* No G_SPAWN_CHILD_INHERITS_STDIN: without it GLib attaches the child's
   * stdin to /dev/null, which is what a GUI wants. Inheriting would leave
   * yt-dlp able to block forever waiting on a terminal that is not there. */
  g_ptr_array_unref (argv);

  if (!spawned)
    {
      g_autofree char *msg =
          g_strdup_printf ("could not start %s: %s", pwsh, error->message);
      g_clear_error (&error);
      fail_run (self, rec, msg);
      return;
    }

  g_mutex_lock (&self->lock);
  self->child_pid = pid;
  g_mutex_unlock (&self->lock);

  PumpArgs *oa = g_new0 (PumpArgs, 1);
  oa->runner = self;
  oa->fd = out_fd;
  oa->stream = "stdout";
  GThread *ot = g_thread_new ("ytdl-out", pump_thread, oa);

  PumpArgs *ea = g_new0 (PumpArgs, 1);
  ea->runner = self;
  ea->fd = err_fd;
  ea->stream = "stderr";
  GThread *et = g_thread_new ("ytdl-err", pump_thread, ea);

  /* Both pumps must finish before the status is read: they are what fills in
   * last_line and the session summary, and a record written while they are
   * still draining would lose the counts the run just reported. */
  g_thread_join (ot);
  g_thread_join (et);

  int status = 0;
  waitpid ((pid_t) pid, &status, 0);
  g_spawn_close_pid (pid);

  gboolean cancelled = g_atomic_int_compare_and_exchange (&self->cancel_flag, 1, 0);
  int code = WIFEXITED (status) ? WEXITSTATUS (status) : -1;

  g_mutex_lock (&self->lock);
  if (self->current != NULL)
    {
      rec->videos_touched = self->current->videos_touched;
      rec->archive_skipped = self->current->archive_skipped;
      rec->errors = self->current->errors;
      rec->warnings = self->current->warnings;
      set_str (&rec->last_line, self->current->last_line);
    }
  g_mutex_unlock (&self->lock);

  rec->exit_code = code;
  rec->finished = now_secs ();
  set_str (&rec->state,
           cancelled ? "cancelled" : (code == 0 ? "done" : "failed"));
  finish_run (self, rec);
}

static gpointer
worker_thread (gpointer data)
{
  YtdlRunner *self = data;

  for (;;)
    {
      if (g_atomic_int_get (&self->stop_flag))
        return NULL;

      YtdlRunRecord *item = NULL;
      g_mutex_lock (&self->lock);
      while (self->queue->len == 0 || self->paused)
        {
          if (g_atomic_int_get (&self->stop_flag))
            {
              g_mutex_unlock (&self->lock);
              return NULL;
            }
          gint64 until = g_get_monotonic_time () + 500 * G_TIME_SPAN_MILLISECOND;
          g_cond_wait_until (&self->wake, &self->lock, until);
        }
      item = g_ptr_array_steal_index (self->queue, 0);
      persist_locked (self);
      g_mutex_unlock (&self->lock);
      mark_state_dirty (self);

      if (item != NULL)
        {
          set_str (&item->state, "running");
          item->started = now_secs ();
          run_one (self, item);
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Public API                                                             */
/* ---------------------------------------------------------------------- */

char *
ytdl_runner_enqueue (YtdlRunner *self, const YtdlRunOptions *opts,
                     GError **error)
{
  g_return_val_if_fail (YTDL_IS_RUNNER (self), NULL);

  if (opts == NULL || !nonempty (opts->url))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Enter a URL first.");
      return NULL;
    }

  YtdlRunRecord *rec = g_new0 (YtdlRunRecord, 1);
  rec->opts = ytdl_run_options_copy (opts);
  rec->command = ytdl_run_options_command_preview (rec->opts);
  rec->state = g_strdup ("queued");
  rec->videos_touched = -1;
  rec->archive_skipped = -1;
  rec->errors = -1;
  rec->warnings = -1;
  rec->last_line = g_strdup ("");

  g_mutex_lock (&self->lock);
  /* Unique enough without a rand dependency: the pid, a counter and the
   * clock. These only have to be distinct within one history file. */
  rec->id = g_strdup_printf ("%" G_GINT64_FORMAT "-%x%x", now_secs (),
                             (guint) getpid (), self->counter++);
  rec->started = now_secs ();
  g_ptr_array_add (self->queue, rec);
  persist_locked (self);
  g_cond_broadcast (&self->wake);
  g_mutex_unlock (&self->lock);
  mark_state_dirty (self);

  return g_strdup (rec->id);
}

gboolean
ytdl_runner_cancel (YtdlRunner *self)
{
  g_return_val_if_fail (YTDL_IS_RUNNER (self), FALSE);

  g_mutex_lock (&self->lock);
  GPid pid = self->child_pid;
  g_mutex_unlock (&self->lock);

  if (pid <= 0)
    return FALSE;

  g_atomic_int_set (&self->cancel_flag, 1);
  kill_tree (pid);
  mark_state_dirty (self);
  return TRUE;
}

void
ytdl_runner_set_paused (YtdlRunner *self, gboolean paused)
{
  g_return_if_fail (YTDL_IS_RUNNER (self));
  g_mutex_lock (&self->lock);
  self->paused = paused;
  g_cond_broadcast (&self->wake);
  g_mutex_unlock (&self->lock);
  mark_state_dirty (self);
}

gboolean
ytdl_runner_get_paused (YtdlRunner *self)
{
  g_return_val_if_fail (YTDL_IS_RUNNER (self), FALSE);
  g_mutex_lock (&self->lock);
  gboolean p = self->paused;
  g_mutex_unlock (&self->lock);
  return p;
}

void
ytdl_runner_remove_queued (YtdlRunner *self, const char *id)
{
  g_return_if_fail (YTDL_IS_RUNNER (self));
  g_mutex_lock (&self->lock);
  for (guint i = 0; i < self->queue->len; i++)
    {
      YtdlRunRecord *r = g_ptr_array_index (self->queue, i);
      if (g_strcmp0 (r->id, id) == 0)
        {
          g_ptr_array_remove_index (self->queue, i);
          break;
        }
    }
  persist_locked (self);
  g_mutex_unlock (&self->lock);
  mark_state_dirty (self);
}

void
ytdl_runner_clear_history (YtdlRunner *self)
{
  g_return_if_fail (YTDL_IS_RUNNER (self));
  g_mutex_lock (&self->lock);
  g_ptr_array_set_size (self->history, 0);
  persist_locked (self);
  g_mutex_unlock (&self->lock);
  mark_state_dirty (self);
}

static GPtrArray *
copy_records (GPtrArray *src)
{
  GPtrArray *out =
      g_ptr_array_new_with_free_func ((GDestroyNotify) ytdl_run_record_free);
  for (guint i = 0; i < src->len; i++)
    g_ptr_array_add (out, record_copy (g_ptr_array_index (src, i)));
  return out;
}

GPtrArray *
ytdl_runner_queue (YtdlRunner *self)
{
  g_return_val_if_fail (YTDL_IS_RUNNER (self), NULL);
  g_mutex_lock (&self->lock);
  GPtrArray *out = copy_records (self->queue);
  g_mutex_unlock (&self->lock);
  return out;
}

GPtrArray *
ytdl_runner_history (YtdlRunner *self)
{
  g_return_val_if_fail (YTDL_IS_RUNNER (self), NULL);
  g_mutex_lock (&self->lock);
  GPtrArray *out = copy_records (self->history);
  g_mutex_unlock (&self->lock);
  return out;
}

YtdlRunRecord *
ytdl_runner_current (YtdlRunner *self)
{
  g_return_val_if_fail (YTDL_IS_RUNNER (self), NULL);
  g_mutex_lock (&self->lock);
  YtdlRunRecord *out = record_copy (self->current);
  g_mutex_unlock (&self->lock);
  return out;
}

void
ytdl_runner_progress (YtdlRunner *self, YtdlProgress *out)
{
  g_return_if_fail (YTDL_IS_RUNNER (self) && out != NULL);
  g_mutex_lock (&self->lock);
  progress_copy (out, &self->progress);
  g_mutex_unlock (&self->lock);
}

void
ytdl_runner_start (YtdlRunner *self)
{
  g_return_if_fail (YTDL_IS_RUNNER (self));
  if (self->worker != NULL)
    return;
  if (self->drain_id == 0)
    self->drain_id = g_timeout_add (DRAIN_INTERVAL_MS, drain, self);
  self->worker = g_thread_new ("ytdl-runner", worker_thread, self);
}

void
ytdl_runner_stop (YtdlRunner *self)
{
  g_return_if_fail (YTDL_IS_RUNNER (self));
  g_atomic_int_set (&self->stop_flag, 1);
  g_mutex_lock (&self->lock);
  g_cond_broadcast (&self->wake);
  g_mutex_unlock (&self->lock);
  if (self->worker != NULL)
    {
      g_thread_join (self->worker);
      self->worker = NULL;
    }
  if (self->drain_id != 0)
    {
      g_source_remove (self->drain_id);
      self->drain_id = 0;
    }
}

/* ---------------------------------------------------------------------- */

static void
ytdl_runner_finalize (GObject *object)
{
  YtdlRunner *self = YTDL_RUNNER (object);
  ytdl_runner_stop (self);

  g_clear_pointer (&self->queue, g_ptr_array_unref);
  g_clear_pointer (&self->history, g_ptr_array_unref);
  g_clear_pointer (&self->current, ytdl_run_record_free);
  g_clear_pointer (&self->log, g_ptr_array_unref);
  ytdl_progress_clear (&self->progress);
  if (self->lines != NULL)
    g_async_queue_unref (self->lines);
  g_mutex_clear (&self->lock);
  g_cond_clear (&self->wake);

  G_OBJECT_CLASS (ytdl_runner_parent_class)->finalize (object);
}

static void
ytdl_runner_class_init (YtdlRunnerClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = ytdl_runner_finalize;

  signals[SIG_STATE_CHANGED] =
      g_signal_new ("state-changed", G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  signals[SIG_LINE] =
      g_signal_new ("line", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                    NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING,
                    G_TYPE_BOOLEAN);
}

static void
ytdl_runner_init (YtdlRunner *self)
{
  g_mutex_init (&self->lock);
  g_cond_init (&self->wake);
  self->queue =
      g_ptr_array_new_with_free_func ((GDestroyNotify) ytdl_run_record_free);
  self->history =
      g_ptr_array_new_with_free_func ((GDestroyNotify) ytdl_run_record_free);
  self->log = g_ptr_array_new_with_free_func (g_free);
  self->lines = g_async_queue_new_full (line_event_free);
  self->progress.percent = -1.0;
}

YtdlRunner *
ytdl_runner_new (void)
{
  YtdlRunner *self = g_object_new (YTDL_TYPE_RUNNER, NULL);

  /* Restore what the last session left behind. A queue that survives a
   * restart is the difference between "I queued twelve channels overnight"
   * being a plan and being a thing you have to babysit. */
  g_autofree char *hp = state_file ("history.json");
  g_autofree char *qp = state_file ("queue.json");
  g_clear_pointer (&self->history, g_ptr_array_unref);
  g_clear_pointer (&self->queue, g_ptr_array_unref);
  self->history = read_records (hp);
  self->queue = read_records (qp);

  /* Anything recorded as running belongs to a process that died with the last
   * window. Left as "running" it would be a row that never resolves. */
  for (guint i = 0; i < self->history->len; i++)
    {
      YtdlRunRecord *r = g_ptr_array_index (self->history, i);
      if (g_strcmp0 (r->state, "running") == 0)
        {
          set_str (&r->state, "failed");
          if (!nonempty (r->last_line))
            set_str (&r->last_line,
                     "Interrupted -- the window closed while this was running.");
        }
    }
  return self;
}
