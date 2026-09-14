/* Tests for the URL preview.
 *
 * The thing most worth defending here is not that either parser works. It is
 * that the TWO OF THEM AGREE.
 *
 * probe.c reads a probe-contract document when the installed pipeline can
 * produce one, and derives the same answers from raw `yt-dlp -J` when it
 * cannot. Two derivations that disagree would be worse than having no preview
 * at all: one pipeline version would offer MP4 for a video where another did
 * not, with nothing in the UI saying why, and the difference would only ever
 * be visible to someone who upgraded mid-session. So the fixture below is fed
 * through both paths and the derived lists are compared field by field --
 * which is also, incidentally, a test of the pipeline's own PowerShell
 * derivation, because the expected values here were written from
 * docs/probe-contract.md rather than from either implementation.
 *
 * The format list is chosen so that a plausible wrong answer fails. yt-dlp
 * spells VP9 as "vp09" and AV1 as "av01" with a zero; it calls AAC "mp4a";
 * its storyboards are .mhtml entries with no streams at all; and a VP8
 * rendition has a real height and no --codec spelling whatsoever.
 *
 * Nothing here spawns a process or touches the network. The async runner's
 * own path -- pipeline first, fallback only on "too old" -- is logic that
 * needs a pipeline to exercise, and is covered by the pipeline's 085-probe
 * suite from the other side.
 */

#include "url_probe.h"

#include <glib.h>
#include <string.h>

void ytdl_register_url_probe_tests (void);

/* The same nine renditions the pipeline's 085-probe suite uses, in yt-dlp's
 * own shape. Keeping the two fixtures identical is deliberate: when this file
 * and that one both pass, the C and the PowerShell derivations have been
 * shown to agree on the same input rather than each being self-consistent. */
static const char *const RAW_FORMATS =
    "{\"id\":\"dQw4w9WgXcQ\",\"title\":\"Test Video\",\"uploader\":\"Test "
    "Channel\",\"channel\":\"Test Channel\",\"duration\":212.0,"
    "\"upload_date\":\"20250114\",\"view_count\":1234567,"
    "\"thumbnail\":\"https://i.ytimg.com/vi/x/maxres.jpg\","
    "\"subtitles\":{\"en\":[],\"de\":[]},"
    "\"formats\":["
    "{\"format_id\":\"sb0\",\"ext\":\"mhtml\",\"vcodec\":\"none\","
    "\"acodec\":\"none\",\"height\":180},"
    "{\"format_id\":\"137\",\"ext\":\"mp4\",\"vcodec\":\"avc1.640028\","
    "\"acodec\":\"none\",\"height\":1080,\"width\":1920,\"fps\":30,"
    "\"tbr\":4412.5,\"filesize\":112233445},"
    "{\"format_id\":\"248\",\"ext\":\"webm\",\"vcodec\":\"vp09.00.40.08\","
    "\"acodec\":\"none\",\"height\":1080,\"width\":1920,\"fps\":30},"
    "{\"format_id\":\"271\",\"ext\":\"webm\",\"vcodec\":\"vp09.00.50.08\","
    "\"acodec\":\"none\",\"height\":1440,\"width\":2560,"
    "\"filesize_approx\":220000000},"
    "{\"format_id\":\"401\",\"ext\":\"mp4\",\"vcodec\":\"av01.0.12M.08\","
    "\"acodec\":\"none\",\"height\":2160,\"width\":3840,"
    "\"dynamic_range\":\"SDR\"},"
    "{\"format_id\":\"330\",\"ext\":\"webm\",\"vcodec\":\"vp08.00.10.08\","
    "\"acodec\":\"none\",\"height\":480},"
    "{\"format_id\":\"140\",\"ext\":\"m4a\",\"vcodec\":\"none\","
    "\"acodec\":\"mp4a.40.2\"},"
    "{\"format_id\":\"251\",\"ext\":\"webm\",\"vcodec\":\"none\","
    "\"acodec\":\"opus\"},"
    "{\"format_id\":\"18\",\"ext\":\"mp4\",\"vcodec\":\"avc1.42001E\","
    "\"acodec\":\"mp4a.40.2\",\"height\":360}"
    "]}";

/* The same video as the pipeline would have described it: the derived lists
 * are present, and video_family/audio_family are already filled in. */
static const char *const CONTRACT_DOC =
    "{\"probe_version\":1,\"kind\":\"video\","
    "\"url\":\"https://youtu.be/dQw4w9WgXcQ\",\"id\":\"dQw4w9WgXcQ\","
    "\"title\":\"Test Video\",\"uploader\":\"Test Channel\","
    "\"duration\":212.0,\"upload_date\":\"20250114\",\"entry_count\":1,"
    "\"entries\":[],\"entries_truncated\":false,"
    "\"subtitle_langs\":[\"en\",\"de\"],"
    "\"heights\":[2160,1440,1080,480,360],"
    "\"video_codecs\":[\"avc1\",\"vp9\",\"av01\"],"
    "\"audio_codecs\":[\"opus\",\"aac\"],"
    "\"containers\":[\"mkv\",\"mp4\",\"webm\"],"
    "\"has_video\":true,\"has_audio\":true,"
    "\"formats\":["
    "{\"format_id\":\"137\",\"ext\":\"mp4\",\"vcodec\":\"avc1.640028\","
    "\"acodec\":\"none\",\"video_family\":\"avc1\",\"height\":1080,"
    "\"filesize\":112233445},"
    "{\"format_id\":\"271\",\"ext\":\"webm\",\"vcodec\":\"vp09.00.50.08\","
    "\"acodec\":\"none\",\"video_family\":\"vp9\",\"height\":1440},"
    "{\"format_id\":\"401\",\"ext\":\"mp4\",\"vcodec\":\"av01.0.12M.08\","
    "\"acodec\":\"none\",\"video_family\":\"av01\",\"height\":2160},"
    "{\"format_id\":\"251\",\"ext\":\"webm\",\"vcodec\":\"none\","
    "\"acodec\":\"opus\",\"audio_family\":\"opus\"}"
    "],"
    "\"pot\":{\"healthy\":true,\"reason\":\"provider healthy\","
    "\"note\":\"Formats were read with the same player clients a download "
    "will use.\"}}";

static char *
heights_of (const YtdlUrlProbe *p)
{
  GString *s = g_string_new (NULL);
  for (guint i = 0; i < p->heights->len; i++)
    g_string_append_printf (s, "%s%d", i == 0 ? "" : ",",
                            g_array_index (p->heights, int, i));
  return g_string_free (s, FALSE);
}

/* ---------------------------------------------------------------------- */

static void
test_codec_families (void)
{
  /* The four mappings, and the four near-misses that would each produce a
   * visibly wrong dropdown. */
  g_assert_cmpstr (ytdl_url_probe_video_family ("avc1.640028"), ==, "avc1");
  g_assert_cmpstr (ytdl_url_probe_video_family ("h264"), ==, "avc1");
  /* yt-dlp emits the four-character form. A test for "vp9" alone reports the
   * most common codec on YouTube as having no spelling at all. */
  g_assert_cmpstr (ytdl_url_probe_video_family ("vp09.00.50.08"), ==, "vp9");
  g_assert_cmpstr (ytdl_url_probe_video_family ("vp9"), ==, "vp9");
  /* With a zero. "av1" is not a thing yt-dlp ever writes. */
  g_assert_cmpstr (ytdl_url_probe_video_family ("av01.0.12M.08"), ==, "av01");
  g_assert_null (ytdl_url_probe_video_family ("av1"));
  g_assert_null (ytdl_url_probe_video_family ("vp08.00.10.08"));
  g_assert_null (ytdl_url_probe_video_family ("none"));
  g_assert_null (ytdl_url_probe_video_family (NULL));

  g_assert_cmpstr (ytdl_url_probe_audio_family ("mp4a.40.2"), ==, "aac");
  g_assert_cmpstr (ytdl_url_probe_audio_family ("opus"), ==, "opus");
  g_assert_cmpstr (ytdl_url_probe_audio_family ("mp3"), ==, "mp3");
  g_assert_cmpstr (ytdl_url_probe_audio_family ("flac"), ==, "flac");
  g_assert_null (ytdl_url_probe_audio_family ("none"));
}

static void
test_fallback_derivation (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (YtdlUrlProbe) p = ytdl_url_probe_from_ytdlp (NULL, RAW_FORMATS, &error);
  g_assert_no_error (error);
  g_assert_nonnull (p);

  g_assert_cmpstr (p->kind, ==, "video");
  g_assert_cmpstr (p->id, ==, "dQw4w9WgXcQ");
  g_assert_cmpstr (p->title, ==, "Test Video");
  g_assert_true (p->from_fallback);

  /* The storyboard is gone; the eight real renditions are not. */
  g_assert_cmpuint (p->formats->len, ==, 8);

  /* 1080 appears twice in the input and once here; 480 is the VP8 rendition,
   * whose height is real even though this pipeline has no --codec spelling
   * for its codec; 180 was the storyboard's. */
  g_autofree char *h = heights_of (p);
  g_assert_cmpstr (h, ==, "2160,1440,1080,480,360");

  g_autofree char *vc = g_strjoinv (",", p->video_codecs);
  g_assert_cmpstr (vc, ==, "avc1,vp9,av01");
  g_autofree char *ac = g_strjoinv (",", p->audio_codecs);
  g_assert_cmpstr (ac, ==, "opus,aac");
  g_autofree char *ct = g_strjoinv (",", p->containers);
  g_assert_cmpstr (ct, ==, "mkv,mp4,webm");

  g_assert_true (p->has_video);
  g_assert_true (p->has_audio);

  /* An exact size and an estimate must not be collapsed into one number: the
   * UI shows "421 MB" differently from "~421 MB", and merging them presents
   * a guess as a fact. */
  for (guint i = 0; i < p->formats->len; i++)
    {
      const YtdlUrlProbeFormat *f = g_ptr_array_index (p->formats, i);
      if (g_strcmp0 (f->format_id, "137") == 0)
        {
          g_assert_cmpint (f->filesize, ==, 112233445);
          g_assert_cmpint (f->filesize_approx, ==, 0);
        }
      if (g_strcmp0 (f->format_id, "271") == 0)
        {
          g_assert_cmpint (f->filesize, ==, 0);
          g_assert_cmpint (f->filesize_approx, ==, 220000000);
        }
      /* The VP8 rendition is listed, and has no family. Not "any": "any" is
       * a choice the user makes. */
      if (g_strcmp0 (f->format_id, "330") == 0)
        g_assert_null (f->video_family);
    }

  g_autofree char *subs = g_strjoinv (",", p->subtitle_langs);
  g_assert_cmpstr (subs, ==, "en,de");
}

static void
test_contract_document (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (YtdlUrlProbe) p = ytdl_url_probe_parse (CONTRACT_DOC, &error);
  g_assert_no_error (error);
  g_assert_nonnull (p);

  g_assert_cmpint (p->probe_version, ==, YTDL_SUPPORTED_URL_PROBE_VERSION);
  g_assert_false (p->from_fallback);
  g_assert_cmpstr (p->title, ==, "Test Video");
  g_assert_true (p->pot_healthy);
  g_assert_nonnull (p->pot_note);

  /* The derived lists are READ from the document, not recomputed. The
   * pipeline made them under the PO token provider a real download will use,
   * and recomputing here would let this app disagree with the command it is
   * about to run -- note the formats array in the document is deliberately
   * shorter than the heights array it ships with. */
  g_autofree char *h = heights_of (p);
  g_assert_cmpstr (h, ==, "2160,1440,1080,480,360");
  g_assert_cmpuint (p->formats->len, ==, 4);
}

static void
test_both_paths_agree (void)
{
  /* The reason this file exists. */
  g_autoptr (GError) e1 = NULL;
  g_autoptr (GError) e2 = NULL;
  g_autoptr (YtdlUrlProbe) fallback =
      ytdl_url_probe_from_ytdlp (NULL, RAW_FORMATS, &e1);
  g_autoptr (YtdlUrlProbe) contract = ytdl_url_probe_parse (CONTRACT_DOC, &e2);
  g_assert_no_error (e1);
  g_assert_no_error (e2);

  g_autofree char *h1 = heights_of (fallback);
  g_autofree char *h2 = heights_of (contract);
  g_assert_cmpstr (h1, ==, h2);

  g_autofree char *v1 = g_strjoinv (",", fallback->video_codecs);
  g_autofree char *v2 = g_strjoinv (",", contract->video_codecs);
  g_assert_cmpstr (v1, ==, v2);

  g_autofree char *a1 = g_strjoinv (",", fallback->audio_codecs);
  g_autofree char *a2 = g_strjoinv (",", contract->audio_codecs);
  g_assert_cmpstr (a1, ==, a2);

  g_autofree char *c1 = g_strjoinv (",", fallback->containers);
  g_autofree char *c2 = g_strjoinv (",", contract->containers);
  g_assert_cmpstr (c1, ==, c2);
}

static void
test_containers_are_a_real_constraint (void)
{
  /* Opus-only audio. yt-dlp cannot mux Opus into mp4, so offering it
   * produces a re-encode or a failed merge depending on version -- which the
   * user discovers afterwards. This is the assertion that earns the whole
   * derivation its place. */
  static const char *const opus_only =
      "{\"id\":\"x\",\"formats\":["
      "{\"format_id\":\"248\",\"ext\":\"webm\",\"vcodec\":\"vp09.00.40.08\","
      "\"acodec\":\"none\",\"height\":1080},"
      "{\"format_id\":\"251\",\"ext\":\"webm\",\"vcodec\":\"none\","
      "\"acodec\":\"opus\"}]}";

  g_autoptr (GError) error = NULL;
  g_autoptr (YtdlUrlProbe) p = ytdl_url_probe_from_ytdlp (NULL, opus_only, &error);
  g_assert_no_error (error);
  g_autofree char *ct = g_strjoinv (",", p->containers);
  g_assert_cmpstr (ct, ==, "mkv,webm");

  /* And the mirror image: AAC with no Opus gets mp4 and not webm. */
  static const char *const aac_only =
      "{\"id\":\"x\",\"formats\":["
      "{\"format_id\":\"137\",\"ext\":\"mp4\",\"vcodec\":\"avc1.640028\","
      "\"acodec\":\"none\",\"height\":1080},"
      "{\"format_id\":\"140\",\"ext\":\"m4a\",\"vcodec\":\"none\","
      "\"acodec\":\"mp4a.40.2\"}]}";

  g_autoptr (GError) error2 = NULL;
  g_autoptr (YtdlUrlProbe) q = ytdl_url_probe_from_ytdlp (NULL, aac_only, &error2);
  g_assert_no_error (error2);
  g_autofree char *ct2 = g_strjoinv (",", q->containers);
  g_assert_cmpstr (ct2, ==, "mkv,mp4");
}

static void
test_heights_cross_filter (void)
{
  /* 1440p exists only in VP9 in the fixture, and 2160p only in AV1. A
   * Quality list built from the union of heights therefore offers
   * combinations the video does not have -- which is the same class of
   * silent wrong answer the static list had, just with better numbers in
   * it. */
  g_autoptr (GError) error = NULL;
  g_autoptr (YtdlUrlProbe) p = ytdl_url_probe_from_ytdlp (NULL, RAW_FORMATS, &error);
  g_assert_no_error (error);

  g_autoptr (GArray) vp9 = ytdl_url_probe_heights_for_codec (p, "vp9");
  g_assert_cmpuint (vp9->len, ==, 2);
  g_assert_cmpint (g_array_index (vp9, int, 0), ==, 1440);
  g_assert_cmpint (g_array_index (vp9, int, 1), ==, 1080);

  g_autoptr (GArray) av01 = ytdl_url_probe_heights_for_codec (p, "av01");
  g_assert_cmpuint (av01->len, ==, 1);
  g_assert_cmpint (g_array_index (av01, int, 0), ==, 2160);

  /* "any" means every height the video has, VP8's included. */
  g_autoptr (GArray) any = ytdl_url_probe_heights_for_codec (p, "any");
  g_assert_cmpuint (any->len, ==, 5);
  g_autoptr (GArray) null_family = ytdl_url_probe_heights_for_codec (p, NULL);
  g_assert_cmpuint (null_family->len, ==, 5);
}

static void
test_playlist_entries (void)
{
  static const char *const flat =
      "{\"_type\":\"playlist\",\"id\":\"PL1\",\"title\":\"A Playlist\","
      "\"channel\":\"Test Channel\",\"playlist_count\":3,\"entries\":["
      "{\"id\":\"a\",\"title\":\"One\",\"duration\":60,"
      "\"url\":\"https://youtu.be/a\"},"
      "{\"id\":\"b\",\"title\":\"Two\",\"duration\":120,"
      "\"url\":\"https://youtu.be/b\"},"
      "{\"id\":\"c\",\"title\":\"Three\",\"url\":\"https://youtu.be/c\"}]}";

  g_autoptr (GError) error = NULL;
  g_autoptr (YtdlUrlProbe) p = ytdl_url_probe_from_ytdlp (flat, RAW_FORMATS, &error);
  g_assert_no_error (error);

  g_assert_cmpstr (p->kind, ==, "playlist");
  g_assert_cmpint (p->entry_count, ==, 3);
  g_assert_cmpint (p->playlist_count, ==, 3);

  /* 1-based, because that is what --playlist-items counts. An off-by-one
   * here queues the wrong videos and the run succeeds. */
  for (guint i = 0; i < p->entries->len; i++)
    {
      const YtdlUrlProbeEntry *e = g_ptr_array_index (p->entries, i);
      g_assert_cmpint (e->index, ==, (int) i + 1);
    }
  const YtdlUrlProbeEntry *second = g_ptr_array_index (p->entries, 1);
  g_assert_cmpstr (second->title, ==, "Two");
  g_assert_cmpstr (second->id, ==, "b");

  /* The format table came from the first entry and says so. */
  g_assert_cmpstr (p->formats_from_id, ==, "dQw4w9WgXcQ");
  g_assert_cmpuint (p->formats->len, ==, 8);
}

static void
test_items_range_compaction (void)
{
  /* Compacted rather than emitted as a comma list: a 400-video channel
   * selection would otherwise be a command line thousands of characters
   * long, which is unreadable in the command preview and close enough to a
   * real limit on Windows to matter. */
  const int runs[] = { 1, 2, 3, 7, 10, 11, 12 };
  g_autofree char *a = ytdl_url_probe_items_range (runs, G_N_ELEMENTS (runs));
  g_assert_cmpstr (a, ==, "1-3,7,10-12");

  const int one[] = { 5 };
  g_autofree char *b = ytdl_url_probe_items_range (one, 1);
  g_assert_cmpstr (b, ==, "5");

  /* A pair stays a pair. "4-5" is the same length as "4,5" and reads like it
   * might be open-ended. */
  const int pair[] = { 4, 5 };
  g_autofree char *c = ytdl_url_probe_items_range (pair, 2);
  g_assert_cmpstr (c, ==, "4,5");

  /* Unsorted input, and duplicates, both produce the same answer as sorted
   * unique input would -- a saved range parsed back in can overlap itself. */
  const int messy[] = { 12, 3, 1, 2, 11, 10, 3 };
  g_autofree char *d = ytdl_url_probe_items_range (messy, G_N_ELEMENTS (messy));
  g_assert_cmpstr (d, ==, "1-3,10-12");

  g_autofree char *empty = ytdl_url_probe_items_range (NULL, 0);
  g_assert_cmpstr (empty, ==, "");
}

static void
test_items_range_round_trip (void)
{
  g_autoptr (GArray) got = ytdl_url_probe_parse_items_range ("1-3,7,10-12");
  g_assert_cmpuint (got->len, ==, 7);
  g_assert_cmpint (g_array_index (got, int, 0), ==, 1);
  g_assert_cmpint (g_array_index (got, int, 3), ==, 7);
  g_assert_cmpint (g_array_index (got, int, 6), ==, 12);

  g_autofree char *back =
      ytdl_url_probe_items_range ((const int *) got->data, got->len);
  g_assert_cmpstr (back, ==, "1-3,7,10-12");

  /* yt-dlp's open-ended forms are NOT expanded to a guessed bound: a tick
   * list built from a guess would show a selection the pipeline may not
   * agree with. They parse to nothing and the text the user typed stays. */
  g_autoptr (GArray) open_end = ytdl_url_probe_parse_items_range ("5-");
  g_assert_cmpuint (open_end->len, ==, 0);
  g_autoptr (GArray) open_start = ytdl_url_probe_parse_items_range ("-10");
  g_assert_cmpuint (open_start->len, ==, 0);

  g_autoptr (GArray) junk = ytdl_url_probe_parse_items_range ("not a range");
  g_assert_cmpuint (junk->len, ==, 0);
  g_autoptr (GArray) none = ytdl_url_probe_parse_items_range (NULL);
  g_assert_cmpuint (none->len, ==, 0);
}

static void
test_bad_input_is_an_error_not_a_crash (void)
{
  g_autoptr (GError) e1 = NULL;
  g_assert_null (ytdl_url_probe_parse ("", &e1));
  g_assert_nonnull (e1);

  g_autoptr (GError) e2 = NULL;
  g_assert_null (ytdl_url_probe_parse ("not json at all", &e2));
  g_assert_nonnull (e2);

  /* A bare array is valid JSON and not a probe document. */
  g_autoptr (GError) e3 = NULL;
  g_assert_null (ytdl_url_probe_parse ("[1,2,3]", &e3));
  g_assert_nonnull (e3);

  /* An object with nothing in it parses, and every list comes back empty
   * rather than NULL -- the form walks these arrays unconditionally. */
  g_autoptr (GError) e4 = NULL;
  g_autoptr (YtdlUrlProbe) bare = ytdl_url_probe_parse ("{}", &e4);
  g_assert_no_error (e4);
  g_assert_nonnull (bare);
  g_assert_cmpstr (bare->kind, ==, "video");
  g_assert_nonnull (bare->containers);
  /* No formats at all still leaves mkv, which is always muxable. */
  g_assert_cmpstr (bare->containers[0], ==, "mkv");
}

void
ytdl_register_url_probe_tests (void)
{
  g_test_add_func ("/probe/codec-families", test_codec_families);
  g_test_add_func ("/probe/fallback-derivation", test_fallback_derivation);
  g_test_add_func ("/probe/contract-document", test_contract_document);
  g_test_add_func ("/probe/both-paths-agree", test_both_paths_agree);
  g_test_add_func ("/probe/containers-constrained",
                   test_containers_are_a_real_constraint);
  g_test_add_func ("/probe/heights-cross-filter", test_heights_cross_filter);
  g_test_add_func ("/probe/playlist-entries", test_playlist_entries);
  g_test_add_func ("/probe/items-range", test_items_range_compaction);
  g_test_add_func ("/probe/items-range-round-trip", test_items_range_round_trip);
  g_test_add_func ("/probe/bad-input", test_bad_input_is_an_error_not_a_crash);
}
