/* Finding out what a URL is before downloading it.
 *
 * The Downloads form used to know nothing about the URL in it until the run
 * failed. The Quality list was a fixed ladder from 2160p down, the codec and
 * container lists were fixed too, and asking for 1440p AV1 was a request that
 * silently resolved to something else -- discovered, at the earliest, in the
 * Library. This module is the answer to that.
 *
 * WHERE THE ANSWER COMES FROM, and why there are two ways.
 *
 * The pipeline owns a `ytdl <url> --probe` mode that prints one JSON document
 * described by docs/probe-contract.md in orchid-ochre. That is the preferred
 * path and it is the one that gets used, because the pipeline's copy of the
 * derivation is the authority: it maps yt-dlp's codec spellings onto the
 * --codec vocabulary and works out which --container values a merge could
 * actually produce, under the same PO token provider a real download uses.
 *
 * The fallback exists because this app is pinned to a pipeline ref
 * (CLI_VERSION) that a user's machine may predate. An installed ytdl.ps1
 * without --probe answers with a usage error, and the preview would simply
 * never work -- against a pipeline that downloads perfectly well. So when the
 * pipeline probe fails in the specific way that means "this pipeline is older
 * than the feature", this module runs `yt-dlp -J` itself and does the
 * derivation here.
 *
 * THE SECOND COPY IS THE COST AND IT IS PAID ON PURPOSE. Two derivations that
 * disagree would be worse than no preview: one pipeline version would offer
 * mp4 where another did not, for the same video, with nothing saying why. So
 * ytdl_url_probe_parse and ytdl_url_probe_from_ytdlp are held to the same fixture in
 * tests/test_probe.c and asserted to produce identical heights, codec lists
 * and container lists. The same obligation exists in the SwiftUI and WinUI
 * apps, in their own languages, for the same reason the archive layout has a
 * conformance suite in each.
 *
 * Everything returned is newly allocated; free a YtdlUrlProbe with
 * ytdl_url_probe_free().
 */

#ifndef YTDL_URL_PROBE_H
#define YTDL_URL_PROBE_H

#include <gio/gio.h>
#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* The contract version this app knows how to read.
 *
 * Compared per document rather than enforced at startup, the same way the
 * archive layout version is compared per video: a probe document declaring a
 * newer version is reported as such rather than misread, and one declaring an
 * older version is still read, because the rule on the pipeline side is that
 * fields are added and not redefined. */
#define YTDL_SUPPORTED_URL_PROBE_VERSION 1

/* One rendition. Not one row of the UI -- the UI derives its rows from the
 * arrays below, and shows this list whole only in the details expander. */
typedef struct
{
  char *format_id;
  char *ext;
  char *vcodec; /* yt-dlp's own spelling, verbatim */
  char *acodec;
  /* This pipeline's vocabulary, or NULL when the codec has no --codec
   * spelling at all. NULL is not "any": "any" is a choice the user makes,
   * and reporting it for a VP8 rendition would be a lie about what was
   * asked for. */
  char  *video_family;
  char  *audio_family;
  int    height; /* 0 when unknown */
  int    width;
  double fps;
  double tbr;
  gint64 filesize;        /* 0 when unknown. Exact. */
  gint64 filesize_approx; /* 0 when unknown. An estimate, kept separate. */
  char  *dynamic_range;
  char  *format_note;
} YtdlUrlProbeFormat;

/* One playlist entry. */
typedef struct
{
  /* The 1-BASED POSITION IN THE PLAYLIST, which is what --playlist-items
   * counts and therefore the only number that may be written back into
   * --items. Carried explicitly rather than taken from array position,
   * because the list is sortable in the UI and a renumbered selection queues
   * the wrong videos -- a bug whose first symptom is the wrong download
   * finishing successfully. */
  int    index;
  char  *id;
  char  *title;
  char  *uploader;
  char  *url;
  char  *thumbnail;
  double duration; /* seconds, 0 when unknown */
} YtdlUrlProbeEntry;

typedef struct
{
  int   probe_version;
  char *kind; /* "video" or "playlist" */
  char *url;
  char *id;
  char *title;
  char *uploader;
  char *channel;
  char *channel_url;
  char *extractor;
  char *thumbnail;
  char *description;
  char *upload_date; /* YYYYMMDD */
  char *live_status;
  char *availability;
  char *webpage_url;

  double duration;
  gint64 view_count;
  gint64 like_count;
  gint64 comment_count;
  int    age_limit;

  /* Playlists */
  int        entry_count;
  int        playlist_count; /* what the extractor says the WHOLE list holds */
  gboolean   entries_truncated;
  GPtrArray *entries; /* YtdlUrlProbeEntry* */
  char      *formats_from_id;
  char      *formats_from_title;

  /* Formats and the lists derived from them */
  GPtrArray *formats; /* YtdlUrlProbeFormat* */
  GArray    *heights; /* int, descending, distinct */
  GStrv      video_codecs;
  GStrv      audio_codecs;
  GStrv      containers;
  GStrv      subtitle_langs;
  gboolean   has_video;
  gboolean   has_audio;

  gboolean pot_healthy;
  char    *pot_reason;
  char    *pot_note;

  /* TRUE when this came from the app's own yt-dlp call rather than from
   * `ytdl --probe`. Surfaced in the UI, because the two can legitimately
   * differ: the fallback does not bring up the PO token provider, so its
   * format table can be the thinner one. */
  gboolean from_fallback;
} YtdlUrlProbe;

void ytdl_url_probe_free (YtdlUrlProbe *p);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (YtdlUrlProbe, ytdl_url_probe_free)

/* ---------------------------------------------------------------------- */
/* Parsing                                                                */
/* ---------------------------------------------------------------------- */

/* Read a probe-contract document. @json is the whole of the pipeline's
 * stdout, which may have leading or trailing whitespace and nothing else.
 * NULL with @error set when it does not parse or is not an object. */
YtdlUrlProbe *ytdl_url_probe_parse (const char *json, GError **error);

/* The fallback. @flat is `yt-dlp -J --flat-playlist` output and may be NULL
 * for a single video; @full is `yt-dlp -J` output for the video whose formats
 * should be reported. Does here, in C, exactly what probe.ps1 does in
 * PowerShell -- which is why the two are held to one fixture in the tests. */
YtdlUrlProbe *ytdl_url_probe_from_ytdlp (const char *flat, const char *full,
                                  GError **error);

/* Exposed for the tests, and used by both of the above: the codec-family
 * mapping is the single most confusable part of this contract -- yt-dlp emits
 * "vp09" and never "vp9", "av01" with a zero and never "av1", and "mp4a" for
 * what a user calls AAC. Returns a static string or NULL. */
const char *ytdl_url_probe_video_family (const char *vcodec);
const char *ytdl_url_probe_audio_family (const char *acodec);

/* ---------------------------------------------------------------------- */
/* Using a probe                                                          */
/* ---------------------------------------------------------------------- */

/* The heights this video offers for @family ("any" or NULL means all of
 * them), descending. This is the cross-filter the Quality row needs: 1440p
 * commonly exists only in VP9, so a Quality list built from the union of
 * heights offers a combination the video does not have. Caller frees with
 * g_array_unref(). */
GArray *ytdl_url_probe_heights_for_codec (const YtdlUrlProbe *p, const char *family);

/* Compact a set of 1-based playlist positions into --items syntax:
 * {1,2,3,7,10,11,12} becomes "1-3,7,10-12". @indices need not be sorted.
 *
 * Compacted rather than emitted as a comma list because a channel selection
 * of 400 videos would otherwise produce a command line thousands of
 * characters long -- which is both unreadable in the command preview and,
 * on the Windows app, close enough to a real command-line limit to matter.
 * Returns "" for an empty set. */
char *ytdl_url_probe_items_range (const int *indices, gsize n);

/* The inverse: parse an --items range string back into the positions it
 * names, so a profile or a re-opened form can tick the right boxes. Anything
 * unparseable is skipped rather than refused -- ytdl.ps1 is the validator,
 * and a range this cannot read is still one the pipeline may accept.
 * Caller frees with g_array_unref(). */
GArray *ytdl_url_probe_parse_items_range (const char *spec);

/* ---------------------------------------------------------------------- */
/* Running one                                                            */
/* ---------------------------------------------------------------------- */

/* Asynchronous because a probe is two network round trips and the form must
 * stay usable -- including its Cancel, which is what @cancellable is for.
 *
 * @url is used as typed (ytdl_normalize_url is applied internally, so a bare
 * video id works here exactly as it does in the URL field). @items may be
 * NULL. @no_pot and @pot_port mirror the form's own controls, because both
 * change which formats yt-dlp can see. @extra_args is the Advanced box, one
 * argument per element, so a URL that needs --cookies-from-browser can be
 * probed at all.
 *
 * Finish with ytdl_url_probe_run_finish(). The callback runs on the thread that
 * started the operation, which for this app is always the main one. */
void ytdl_url_probe_run_async (const char *url, const char *items, gboolean no_pot,
                           guint pot_port, const char *const *extra_args,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data);

YtdlUrlProbe *ytdl_url_probe_run_finish (GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* YTDL_URL_PROBE_H */
