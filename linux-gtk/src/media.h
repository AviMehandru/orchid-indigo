/* What is actually inside the media file.
 *
 * The archive's whole point is that the ORIGINAL file is kept, so the useful
 * question on a video's page is "what did I actually get" -- 1080p VP9 with
 * Opus and three subtitle tracks, or a 360p fallback nobody noticed at the
 * time. That answer is in the container, not in info.json: info.json records
 * what yt-dlp was asked for and what it believed it fetched, while the file
 * records what is on disk now.
 *
 * WHY ffprobe AND NOT AN EBML PARSER. Hand-rolling Matroska would fit this
 * project's dependency principle and would literally "parse mkv" -- but the
 * --container option also produces .mp4 and .webm, and --mode audio-only
 * produces .m4a and .opus. A Matroska parser answers for one of those. ffprobe
 * answers for all of them, is already a documented dependency of the pipeline,
 * and is already probed for on the Health pane. The cost is honest and bounded:
 * no ffprobe, no stream details, and the page says so rather than going blank.
 *
 * READ-ONLY, like everything else here. This spawns ffprobe. It never spawns
 * ffmpeg: nothing is re-encoded, remuxed or written. The webview build needed
 * an .mkv -> .webm remux because a browser engine cannot play Matroska; a GTK
 * player plays the original file directly, so that entire apparatus is gone.
 */

#ifndef YTDL_MEDIA_H
#define YTDL_MEDIA_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct
{
  char *kind;  /* video | audio | subtitle | attachment | data */
  char *codec; /* short name, e.g. "vp9", "opus", "webvtt" */
  char *profile;
  char *language; /* from the track's language tag; NULL when untagged */
  char *title;    /* the track's own title, when it has one */
  int   index;

  /* Video only; 0 when not applicable. */
  int    width;
  int    height;
  double fps;

  /* Audio only; 0 when not applicable. */
  int channels;
  int sample_rate;

  gint64   bit_rate; /* bits/sec, 0 when the container does not say */
  gboolean is_default;
  /* An attached cover image. ffprobe reports one as a one-frame video stream,
   * which is exactly how the webview build ended up serving thumbnails with a
   * video MIME type -- so it is flagged rather than counted as video. */
  gboolean attached_pic;
} YtdlStream;

typedef struct
{
  char  *title;
  double start;
  double end;
} YtdlChapter;

typedef struct
{
  gboolean   ok;      /* FALSE when ffprobe is missing or the file is unreadable */
  char      *error;   /* why, when !ok -- shown to the user verbatim */
  char      *format;  /* container long name */
  double     duration;
  guint64    size;
  gint64     bit_rate;
  GPtrArray *streams;  /* YtdlStream* */
  GPtrArray *chapters; /* YtdlChapter* */
} YtdlProbe;

/* Runs ffprobe and parses its JSON. BLOCKS -- call it off the UI thread.
 * Never returns NULL: a failure comes back as a probe with ok = FALSE and a
 * message, because "ffprobe is not installed" is something the page should
 * say rather than something it should hide. */
YtdlProbe *ytdl_media_probe (const char *path);
void       ytdl_probe_free (YtdlProbe *p);

/* A one-line summary for a header: "1080p · VP9 / Opus · 9:47 · 412 MB". */
char *ytdl_probe_summary (const YtdlProbe *p);

/* Whether GTK can be expected to play this file in-window.
 *
 * GtkVideo goes through GStreamer, so this is a question about installed
 * plugins rather than about the file. Used to decide between showing the
 * player and showing "open it in mpv instead" -- an empty black rectangle is
 * a worse answer than a sentence explaining what is missing. */
gboolean ytdl_media_playback_available (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlProbe, ytdl_probe_free)

G_END_DECLS

#endif /* YTDL_MEDIA_H */
