/* The two parsers the detail page is built on.
 *
 * Both turn something yt-dlp wrote for a machine into something a person can
 * read, and both fail QUIETLY when they are wrong -- a mis-threaded comment
 * section still renders, and a transcript with the rolling duplication left in
 * still looks like a transcript. Neither would throw. So they are pinned here.
 */

#include "archive.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

void ytdl_register_detail_tests (void);

static JsonArray *
array_of (JsonParser **keep, const char *json)
{
  *keep = json_parser_new ();
  g_assert_true (json_parser_load_from_data (*keep, json, -1, NULL));
  JsonNode *root = json_parser_get_root (*keep);
  g_assert_true (JSON_NODE_HOLDS_ARRAY (root));
  return json_node_get_array (root);
}

static const YtdlComment *
top (GPtrArray *tops, guint i)
{
  g_assert_cmpuint (i, <, tops->len);
  return g_ptr_array_index (tops, i);
}

/* ---------------------------------------------------------------------- */
/* Comment threading                                                      */
/* ---------------------------------------------------------------------- */

static void
test_threads_replies_under_parents (void)
{
  /* The reply comes FIRST, before the parent it belongs to. yt-dlp gives no
   * ordering guarantee, which is the whole reason this is a two-pass job
   * rather than a fold. */
  g_autoptr (JsonParser) keep = NULL;
  JsonArray *raw = array_of (
      &keep,
      "["
      "  {\"id\":\"a.1\",\"parent\":\"a\",\"text\":\"reply\",\"author\":\"R\","
      "   \"timestamp\":200},"
      "  {\"id\":\"a\",\"parent\":\"root\",\"text\":\"top\",\"author\":\"T\"},"
      "  {\"id\":\"a.0\",\"parent\":\"a\",\"text\":\"earlier\",\"author\":\"E\","
      "   \"timestamp\":100}"
      "]");

  g_autoptr (GPtrArray) tops = ytdl_thread_comments (raw);
  g_assert_cmpuint (tops->len, ==, 1);

  const YtdlComment *t = top (tops, 0);
  g_assert_cmpstr (t->text, ==, "top");
  g_assert_cmpuint (t->replies->len, ==, 2);

  /* Replies in time order, not arrival order. */
  const YtdlComment *r0 = g_ptr_array_index (t->replies, 0);
  const YtdlComment *r1 = g_ptr_array_index (t->replies, 1);
  g_assert_cmpstr (r0->text, ==, "earlier");
  g_assert_cmpstr (r1->text, ==, "reply");
}

static void
test_recovers_parent_from_compound_id (void)
{
  /* The parent field carries the compound reply id rather than the bare
   * parent. yt-dlp's reply ids are "<parent>.<reply>", so the parent is
   * recoverable from the prefix. */
  g_autoptr (JsonParser) keep = NULL;
  JsonArray *raw = array_of (
      &keep,
      "["
      "  {\"id\":\"top1\",\"parent\":\"root\",\"text\":\"t\",\"author\":\"A\"},"
      "  {\"id\":\"top1.9\",\"parent\":\"top1.9\",\"text\":\"r\",\"author\":\"B\"}"
      "]");

  g_autoptr (GPtrArray) tops = ytdl_thread_comments (raw);
  g_assert_cmpuint (tops->len, ==, 1);
  g_assert_cmpuint (top (tops, 0)->replies->len, ==, 1);
}

static void
test_orphan_reply_is_promoted_not_dropped (void)
{
  /* The parent is genuinely absent -- a deleted comment, or a fetch that was
   * truncated. Silently losing archived text would be the worse failure, so
   * the reply is shown at top level instead. */
  g_autoptr (JsonParser) keep = NULL;
  JsonArray *raw = array_of (
      &keep,
      "[{\"id\":\"zz.1\",\"parent\":\"gone\",\"text\":\"orphan\","
      "  \"author\":\"O\"}]");

  g_autoptr (GPtrArray) tops = ytdl_thread_comments (raw);
  g_assert_cmpuint (tops->len, ==, 1);
  g_assert_cmpstr (top (tops, 0)->text, ==, "orphan");
}

static void
test_pinned_first_then_likes (void)
{
  g_autoptr (JsonParser) keep = NULL;
  JsonArray *raw = array_of (
      &keep,
      "["
      "  {\"id\":\"a\",\"parent\":\"root\",\"text\":\"few\",\"author\":\"A\","
      "   \"like_count\":5},"
      "  {\"id\":\"b\",\"parent\":\"root\",\"text\":\"many\",\"author\":\"B\","
      "   \"like_count\":900},"
      "  {\"id\":\"c\",\"parent\":\"root\",\"text\":\"pinned\",\"author\":\"C\","
      "   \"like_count\":1,\"is_pinned\":true}"
      "]");

  g_autoptr (GPtrArray) tops = ytdl_thread_comments (raw);
  g_assert_cmpuint (tops->len, ==, 3);
  /* The order YouTube itself shows. A comment section in arbitrary order is a
   * different document from the one people actually read. */
  g_assert_cmpstr (top (tops, 0)->text, ==, "pinned");
  g_assert_cmpstr (top (tops, 1)->text, ==, "many");
  g_assert_cmpstr (top (tops, 2)->text, ==, "few");
}

static void
test_missing_fields_are_tolerated (void)
{
  g_autoptr (JsonParser) keep = NULL;
  JsonArray *raw = array_of (&keep, "[{\"id\":\"a\"},{\"parent\":\"root\"}]");

  g_autoptr (GPtrArray) tops = ytdl_thread_comments (raw);
  g_assert_cmpuint (tops->len, ==, 2);
  /* No author is "(unknown)", not NULL -- every field is optional because
   * the shape varies by extractor and by yt-dlp version. */
  g_assert_cmpstr (top (tops, 0)->author, ==, "(unknown)");
  g_assert_cmpint (top (tops, 0)->like_count, ==, -1);
}

static void
test_empty_comment_list (void)
{
  g_autoptr (GPtrArray) none = ytdl_thread_comments (NULL);
  g_assert_cmpuint (none->len, ==, 0);
}

/* ---------------------------------------------------------------------- */
/* Transcript                                                             */
/* ---------------------------------------------------------------------- */

static GPtrArray *
cues_of (const char *vtt, char **path_out)
{
  g_autofree char *dir = g_dir_make_tmp ("ytdl-vtt-XXXXXX", NULL);
  g_assert_nonnull (dir);
  char *path = g_build_filename (dir, "Subtitles.en.vtt", NULL);
  g_assert_true (g_file_set_contents (path, vtt, -1, NULL));
  GPtrArray *cues = ytdl_parse_subtitle_cues (path);
  if (path_out != NULL)
    *path_out = g_strdup (path);
  g_unlink (path);
  g_rmdir (dir);
  g_free (path);
  return cues;
}

static void
test_basic_vtt (void)
{
  g_autoptr (GPtrArray) cues =
      cues_of ("WEBVTT\n"
               "\n"
               "00:00:01.000 --> 00:00:03.500\n"
               "First line\n"
               "\n"
               "00:00:04.000 --> 00:00:06.000\n"
               "Second line\n",
               NULL);

  g_assert_cmpuint (cues->len, ==, 2);
  const YtdlCue *a = g_ptr_array_index (cues, 0);
  g_assert_cmpstr (a->text, ==, "First line");
  g_assert_cmpfloat (a->start, >, 0.99);
  g_assert_cmpfloat (a->start, <, 1.01);
  g_assert_cmpfloat (a->end, >, 3.49);
  g_assert_cmpfloat (a->end, <, 3.51);
}

static void
test_collapses_rolling_auto_captions (void)
{
  /* YouTube's ASR output is a rolling two-line display: each cue repeats the
   * previous cue's text and adds a few words. Read as-is it is unusable as
   * prose -- this is the single most important thing this parser does. */
  g_autoptr (GPtrArray) cues =
      cues_of ("WEBVTT\n"
               "\n"
               "00:00:01.000 --> 00:00:03.000\n"
               "the quick brown fox\n"
               "\n"
               "00:00:03.000 --> 00:00:05.000\n"
               "the quick brown fox jumps over\n"
               "\n"
               "00:00:05.000 --> 00:00:07.000\n"
               "the quick brown fox jumps over the lazy dog\n",
               NULL);

  g_assert_cmpuint (cues->len, ==, 3);
  g_assert_cmpstr (((YtdlCue *) g_ptr_array_index (cues, 0))->text, ==,
                   "the quick brown fox");
  g_assert_cmpstr (((YtdlCue *) g_ptr_array_index (cues, 1))->text, ==,
                   "jumps over");
  g_assert_cmpstr (((YtdlCue *) g_ptr_array_index (cues, 2))->text, ==,
                   "the lazy dog");
}

static void
test_identical_cue_extends_rather_than_repeats (void)
{
  g_autoptr (GPtrArray) cues = cues_of ("WEBVTT\n"
                                        "\n"
                                        "00:00:01.000 --> 00:00:02.000\n"
                                        "same text here\n"
                                        "\n"
                                        "00:00:02.000 --> 00:00:09.000\n"
                                        "same text here\n",
                                        NULL);

  g_assert_cmpuint (cues->len, ==, 1);
  const YtdlCue *c = g_ptr_array_index (cues, 0);
  g_assert_cmpfloat (c->end, >, 8.99);
}

static void
test_strips_karaoke_tags_and_entities (void)
{
  g_autoptr (GPtrArray) cues =
      cues_of ("WEBVTT\n"
               "\n"
               "00:00:01.000 --> 00:00:03.000 align:start position:0%\n"
               "<c>hello</c><00:00:01.500><c> there</c> &amp; welcome\n",
               NULL);

  g_assert_cmpuint (cues->len, ==, 1);
  /* Tags gone, entity decoded, runs of whitespace collapsed to one space. */
  g_assert_cmpstr (((YtdlCue *) g_ptr_array_index (cues, 0))->text, ==,
                   "hello there & welcome");
}

static void
test_timestamp_forms (void)
{
  /* mm:ss with a comma decimal (SRT style), and a one-digit fraction: ".5"
   * is 500ms, not 5ms, which is what padding on the right is for. */
  g_autoptr (GPtrArray) cues = cues_of ("WEBVTT\n"
                                        "\n"
                                        "01:02,5 --> 01:04,250\n"
                                        "short form\n",
                                        NULL);

  g_assert_cmpuint (cues->len, ==, 1);
  const YtdlCue *c = g_ptr_array_index (cues, 0);
  g_assert_cmpfloat (c->start, >, 62.49);
  g_assert_cmpfloat (c->start, <, 62.51);
  g_assert_cmpfloat (c->end, >, 64.24);
  g_assert_cmpfloat (c->end, <, 64.26);
}

static void
test_blocks_without_timing_are_skipped (void)
{
  /* The WEBVTT header, NOTE blocks and cue numbers all arrive as blocks with
   * no "-->" in them. None of them is a cue. */
  g_autoptr (GPtrArray) cues = cues_of ("WEBVTT\n"
                                        "Kind: captions\n"
                                        "Language: en\n"
                                        "\n"
                                        "NOTE this is a comment\n"
                                        "\n"
                                        "1\n"
                                        "00:00:01.000 --> 00:00:02.000\n"
                                        "only real cue\n",
                                        NULL);

  g_assert_cmpuint (cues->len, ==, 1);
  g_assert_cmpstr (((YtdlCue *) g_ptr_array_index (cues, 0))->text, ==,
                   "only real cue");
}

static void
test_missing_file_is_empty_not_a_crash (void)
{
  g_autoptr (GPtrArray) cues =
      ytdl_parse_subtitle_cues ("/definitely/not/here.vtt");
  g_assert_cmpuint (cues->len, ==, 0);
}

static void
test_detects_auto_generated_track (void)
{
  g_autofree char *dir = g_dir_make_tmp ("ytdl-auto-XXXXXX", NULL);
  g_autofree char *autop = g_build_filename (dir, "auto.vtt", NULL);
  g_autofree char *human = g_build_filename (dir, "human.vtt", NULL);

  /* The filenames cannot tell these apart -- --write-subs and
   * --write-auto-subs both land in Subtitles/ under the same base name. The
   * contents can. */
  g_assert_true (g_file_set_contents (
      autop,
      "WEBVTT\n\n00:00:01.000 --> 00:00:02.000 align:start position:0%\n"
      "<c>word</c>\n",
      -1, NULL));
  g_assert_true (g_file_set_contents (
      human, "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nA written line.\n", -1,
      NULL));

  g_assert_true (ytdl_subtitle_is_auto (autop));
  g_assert_false (ytdl_subtitle_is_auto (human));

  g_unlink (autop);
  g_unlink (human);
  g_rmdir (dir);
}

static void
test_subtitle_extension_predicate (void)
{
  g_assert_true (ytdl_ext_is_subtitle (".vtt"));
  g_assert_true (ytdl_ext_is_subtitle (".srt"));
  g_assert_false (ytdl_ext_is_subtitle (".mkv"));
  g_assert_false (ytdl_ext_is_subtitle (""));
  g_assert_false (ytdl_ext_is_subtitle (NULL));
}

void
ytdl_register_detail_tests (void)
{
  g_test_add_func ("/comments/threads-replies", test_threads_replies_under_parents);
  g_test_add_func ("/comments/compound-id-parent",
                   test_recovers_parent_from_compound_id);
  g_test_add_func ("/comments/orphan-promoted",
                   test_orphan_reply_is_promoted_not_dropped);
  g_test_add_func ("/comments/pinned-then-likes", test_pinned_first_then_likes);
  g_test_add_func ("/comments/missing-fields", test_missing_fields_are_tolerated);
  g_test_add_func ("/comments/empty", test_empty_comment_list);

  g_test_add_func ("/transcript/basic-vtt", test_basic_vtt);
  g_test_add_func ("/transcript/collapses-rolling",
                   test_collapses_rolling_auto_captions);
  g_test_add_func ("/transcript/identical-extends",
                   test_identical_cue_extends_rather_than_repeats);
  g_test_add_func ("/transcript/strips-tags",
                   test_strips_karaoke_tags_and_entities);
  g_test_add_func ("/transcript/timestamp-forms", test_timestamp_forms);
  g_test_add_func ("/transcript/skips-untimed-blocks",
                   test_blocks_without_timing_are_skipped);
  g_test_add_func ("/transcript/missing-file",
                   test_missing_file_is_empty_not_a_crash);
  g_test_add_func ("/transcript/detects-auto", test_detects_auto_generated_track);
  g_test_add_func ("/transcript/ext-predicate", test_subtitle_extension_predicate);
}
