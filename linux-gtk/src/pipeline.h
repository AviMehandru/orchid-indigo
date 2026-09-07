/* The download engine: what actually starts a run.
 *
 * This does NOT reimplement any part of the pipeline. It builds a `ytdl`
 * command line and hands it to ytdl.ps1 -- the single argument parser, on
 * every platform -- exactly as a terminal would. Every option below maps
 * one-to-one onto a flag ytdl.ps1 already accepts, and nothing here knows what
 * run_ytdlp.ps1 or postprocess.ps1 do with them.
 *
 * Why ytdl.ps1 and not run_ytdlp.ps1 directly: because then there would be two
 * argument surfaces to keep in agreement, which is the exact problem the
 * pipeline collapsed into one file. A flag added to ytdl.ps1 becomes available
 * here by adding a widget, and a flag it rejects fails the same way it fails
 * in a terminal.
 *
 * ONE RUN AT A TIME, deliberately. The queue is strictly sequential because
 * independent `ytdl` invocations race on shared state (global_manifest.json,
 * channel_manifest.json, the Channel Info refresh throttle, download.log).
 * --workers N is the supported way to get real parallelism -- it enumerates
 * every video up front so no two workers are assigned the same one, and
 * postprocess.ps1 has matching file locking. So "run several at once" is the
 * workers spinner, not a wider queue, and the queue never starts a second
 * process.
 */

#ifndef YTDL_PIPELINE_H
#define YTDL_PIPELINE_H

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* ---------------------------------------------------------------------- */
/* Options                                                                */
/* ---------------------------------------------------------------------- */

/* Every field maps to exactly one ytdl.ps1 flag.
 *
 * Validation is deliberately NOT duplicated here. ytdl.ps1 checks these at the
 * point they reach it and exits non-zero naming the option, which surfaces in
 * the run log like any other pipeline error. A second copy of the
 * accepted-value lists in C would be a second thing to keep in step across two
 * repositories, for nothing the user can see.
 *
 * NULL and 0 mean "not set" throughout, which is what keeps a plain download
 * producing exactly the command line it produced before these options existed.
 */
typedef struct
{
  char *url;
  char *data_root;

  gboolean sync;
  char    *items;
  char    *after;
  gboolean lazy;
  guint    workers;  /* 0 = unset; only emitted when > 1 */
  gboolean no_pot;
  gboolean skip_pot_update;
  guint    pot_port; /* 0 = unset */

  /* Content options -- the only ones that change what ends up in the archive
   * rather than how the session is scheduled. The GUI deliberately does not
   * build a yt-dlp format selector or decide what audio-only means; all of
   * that lives in run_ytdlp.ps1, on the far side of the CLI_VERSION pin. */
  char    *mode;        /* full | video-only | audio-only | metadata-only |
                           comments-only | subs-only */
  char    *quality;     /* a height in pixels, or "best" */
  char    *codec;       /* any | avc1 | vp9 | av01 */
  char    *audio_codec; /* any | opus | aac | mp3 | flac */
  char    *container;   /* mkv | mp4 | webm */
  gboolean no_comments;
  gboolean no_subs;
  gboolean no_thumbnail;
  gboolean no_metadata;

  GPtrArray *ytdlp_args; /* char*, each emitted as its own --ytdlp-arg */
} YtdlRunOptions;

YtdlRunOptions *ytdl_run_options_new (void);
YtdlRunOptions *ytdl_run_options_copy (const YtdlRunOptions *src);
void            ytdl_run_options_free (YtdlRunOptions *opts);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlRunOptions, ytdl_run_options_free)

/* Everything after the script path. The URL is always first and always a full
 * URL: ytdl.ps1 accepts a bare 11-character id, but the shell would try to
 * bind a leading-hyphen id as a parameter before the script ever saw it, and
 * about one YouTube id in thirty starts with "-" or "_". */
GStrv ytdl_run_options_to_args (const YtdlRunOptions *opts);

/* What the equivalent terminal command would be. Shown above the Start button,
 * because a GUI that hides the command it runs makes the CLI harder to learn
 * rather than easier. */
char *ytdl_run_options_command_preview (const YtdlRunOptions *opts);

/* Serialise and restore a whole option set.
 *
 * Shared with profiles.c, which stores exactly a YtdlRunOptions. Every field
 * is optional on read, so a profile or a queued run written before an option
 * existed still loads and simply does not set it. */
void            ytdl_run_options_build_json (JsonBuilder *b,
                                             const YtdlRunOptions *opts);
YtdlRunOptions *ytdl_run_options_from_json (JsonObject *obj);

/* A bare video id becomes a watch URL; anything already URL-shaped is left
 * exactly as typed. Deliberately not a validator -- ytdl.ps1 has its own
 * "that does not look like a URL" warning and yt-dlp's extractor is the real
 * authority on what is downloadable. */
char *ytdl_normalize_url (const char *url);

/* ---------------------------------------------------------------------- */
/* Progress and records                                                   */
/* ---------------------------------------------------------------------- */

typedef struct
{
  double percent; /* < 0 when unknown */
  char  *speed;
  char  *eta;
  char  *total;
  char  *stage;
  char  *video_id;
  char  *destination;
} YtdlProgress;

typedef struct
{
  char *id;
  char *command;
  gint64 started;
  gint64 finished; /* 0 while running */
  /* queued | running | done | failed | cancelled */
  char  *state;
  int    exit_code;
  gint64 videos_touched; /* -1 = not reported */
  gint64 archive_skipped;
  gint64 errors;
  gint64 warnings;
  char  *log_path;
  char  *last_line;
  YtdlRunOptions *opts;
} YtdlRunRecord;

void ytdl_run_record_free (YtdlRunRecord *rec);

/* ---------------------------------------------------------------------- */
/* The runner                                                             */
/* ---------------------------------------------------------------------- */

#define YTDL_TYPE_RUNNER (ytdl_runner_get_type ())
G_DECLARE_FINAL_TYPE (YtdlRunner, ytdl_runner, YTDL, RUNNER, GObject)

/* Signals, both emitted on the MAIN thread:
 *
 *   "state-changed"  ()                      queue/current/history/paused moved
 *   "line"           (const char *text, gboolean transient)
 *
 * The worker never emits directly. Lines go onto an async queue and a single
 * main-thread timeout drains them, because yt-dlp redraws its progress line
 * several times a second and one g_idle_add per line would put the UI thread
 * in a callback storm for the whole of a long download.
 *
 * `transient` marks a carriage-return redraw, which REPLACES the previous
 * transient line instead of appending. Without that distinction one download
 * produces thousands of near-identical log rows.
 */

YtdlRunner *ytdl_runner_new (void);

/* Start the worker thread. Separate from _new so a frontend can construct,
 * draw its window, and only then begin accepting runs -- rather than having a
 * restored queue start emitting into a window that does not exist yet. */
void ytdl_runner_start (YtdlRunner *self);

/* Ask the worker to finish and join it. Safe to call more than once. */
void ytdl_runner_stop (YtdlRunner *self);

/* Returns the new run id, or NULL with @error set. */
char *ytdl_runner_enqueue (YtdlRunner *self, const YtdlRunOptions *opts,
                           GError **error);

/* TRUE if something was actually running. */
gboolean ytdl_runner_cancel (YtdlRunner *self);

void ytdl_runner_set_paused (YtdlRunner *self, gboolean paused);
gboolean ytdl_runner_get_paused (YtdlRunner *self);
void ytdl_runner_remove_queued (YtdlRunner *self, const char *id);
void ytdl_runner_clear_history (YtdlRunner *self);

/* Snapshot accessors. All take the state lock, all return newly allocated
 * data, and all are safe to call from the main thread while a run is live. */
GPtrArray *ytdl_runner_queue (YtdlRunner *self);   /* YtdlRunRecord*, owned */
GPtrArray *ytdl_runner_history (YtdlRunner *self); /* YtdlRunRecord*, owned */
YtdlRunRecord *ytdl_runner_current (YtdlRunner *self); /* NULL when idle */
void ytdl_runner_progress (YtdlRunner *self, YtdlProgress *out);
void ytdl_progress_clear (YtdlProgress *p);

/* Exposed for the test suite: these are the two parsers that turn the
 * pipeline's stdout into everything the UI shows, and they are where a change
 * in yt-dlp's output would first bite. */
char    *ytdl_strip_ansi (const char *s);
gboolean ytdl_parse_progress_line (const char *line, YtdlProgress *inout);
gboolean ytdl_parse_session_summary (const char *line, gint64 *videos,
                                     gint64 *skipped, gint64 *errors,
                                     gint64 *warnings);

G_END_DECLS

#endif /* YTDL_PIPELINE_H */
