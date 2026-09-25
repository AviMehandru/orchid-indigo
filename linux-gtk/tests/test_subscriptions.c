/* The Subscriptions pane's reading of `ytdl --subscriptions --json`, and every
 * word it shows -- as fixtures.
 *
 * THE SAME DOCUMENT AND THE SAME STRINGS are asserted by
 * macos-swiftui/Tests/SubscriptionTests.swift and
 * windows-winui/YtdlWin.Tests/SubscriptionTests.cs. Three apps describing the
 * same subscription three different ways -- "3 h ago" here, "3 hours ago"
 * there, "2 h ago" in the third because it rounded down -- is the drift this
 * file exists to stop, so the expected strings are spelled out in full.
 *
 * The document is the contract in orchid-ochre's docs/subscriptions.md, with
 * every state a row can be in: never checked, fine, nothing new, found errors,
 * failed, paused. `now` is fixed, so every relative time is exact.
 */

#include "pipeline.h"
#include "subscriptions.h"

#include <glib.h>
#include <string.h>

void ytdl_register_subscription_tests (void);

/* now = 1790000000. Each started/next_due is now plus or minus a round number
 * of seconds, written out so the expectations below can be checked by eye. */
static const char *fixture =
    "{\"subscriptions_version\":1,\"now\":1790000000,"
    "\"schedule\":{\"mechanism\":\"systemd\",\"supported\":true,"
    "\"installed\":true,\"active\":true,\"running\":false,"
    "\"next_check\":1790001500,\"linger\":false,"
    "\"detail\":\"systemd user timer ytdl-subscriptions.timer, hourly\"},"
    "\"subscriptions\":["
    /* never checked */
    "{\"id\":\"a1b2c3d4\",\"url\":\"https://www.youtube.com/@Never/videos\","
    "\"name\":null,\"options\":[\"--sync\"],\"data_root\":null,"
    "\"every_hours\":24,\"enabled\":true,\"added\":1789990000,"
    "\"updated\":1789990000,\"last_run\":null,\"next_due\":1790000000,"
    "\"due\":true},"
    /* fine: 3 h ago (now-10800), every 6 h, next in 3 h */
    "{\"id\":\"b2c3d4e5\",\"url\":\"https://www.youtube.com/@Fine/videos\","
    "\"name\":\"Fine Channel\",\"options\":[\"--sync\",\"--quality\",\"1080\","
    "\"--proxy\",\"socks5://***@127.0.0.1:1080\"],"
    "\"data_root\":\"/mnt/archive\",\"every_hours\":6,\"enabled\":true,"
    "\"added\":1789000000,\"updated\":1789000000,"
    "\"last_run\":{\"started\":1789989200,\"finished\":1789989500,"
    "\"result\":\"ok\",\"exit_code\":0,\"touched\":2,\"skipped\":1,"
    "\"errors\":0,\"warnings\":0,\"trigger\":\"schedule\",\"message\":null},"
    "\"next_due\":1790010800,\"due\":false},"
    /* nothing new: a day ago, weekly, next in 6 d */
    "{\"id\":\"c3d4e5f6\",\"url\":\"https://www.youtube.com/@Quiet/videos\","
    "\"name\":\"Quiet\",\"options\":[],\"data_root\":null,\"every_hours\":168,"
    "\"enabled\":true,\"added\":1788000000,\"updated\":1788000000,"
    "\"last_run\":{\"started\":1789913600,\"finished\":1789913660,"
    "\"result\":\"ok\",\"exit_code\":0,\"touched\":0,\"skipped\":1,"
    "\"errors\":0,\"warnings\":0,\"trigger\":\"schedule\",\"message\":null},"
    "\"next_due\":1790518400,\"due\":false},"
    /* errors: 30 s ago, hourly, next in 60 min */
    "{\"id\":\"d4e5f6a7\",\"url\":\"https://www.youtube.com/@Errs/videos\","
    "\"name\":\"Some Errors\",\"options\":[\"--sync\"],\"data_root\":null,"
    "\"every_hours\":1,\"enabled\":true,\"added\":1788000000,"
    "\"updated\":1788000000,"
    "\"last_run\":{\"started\":1789999970,\"finished\":1789999990,"
    "\"result\":\"errors\",\"exit_code\":0,\"touched\":3,\"skipped\":0,"
    "\"errors\":1,\"warnings\":2,\"trigger\":\"manual\",\"message\":null},"
    "\"next_due\":1790003570,\"due\":false},"
    /* failed: 2 h ago, every 12 h, next in 10 h */
    "{\"id\":\"e5f6a7b8\",\"url\":\"https://www.youtube.com/@Broken/videos\","
    "\"name\":\"Broken\",\"options\":[\"--codec\",\"av1\"],\"data_root\":null,"
    "\"every_hours\":12,\"enabled\":true,\"added\":1788000000,"
    "\"updated\":1788000000,"
    "\"last_run\":{\"started\":1789992800,\"finished\":1789992802,"
    "\"result\":\"failed\",\"exit_code\":1,\"touched\":0,\"skipped\":0,"
    "\"errors\":0,\"warnings\":0,\"trigger\":\"schedule\","
    "\"message\":\"Error: --codec must be one of: any, avc1, vp9, av01 (got: "
    "'av1'). Note av01 is spelled with a zero.\"},"
    "\"next_due\":1790036000,\"due\":false},"
    /* paused: last checked 3 d ago, every 2 days, a field this app does not
     * know about, which must be ignored */
    "{\"id\":\"f6a7b8c9\",\"url\":\"https://www.youtube.com/@Paused/videos\","
    "\"name\":\"Paused One\",\"options\":[\"--sync\"],\"data_root\":null,"
    "\"every_hours\":48,\"enabled\":false,\"added\":1788000000,"
    "\"updated\":1788000000,\"a_future_field\":{\"x\":1},"
    "\"last_run\":{\"started\":1789740800,\"finished\":1789740900,"
    "\"result\":\"ok\",\"exit_code\":0,\"touched\":1,\"skipped\":0,"
    "\"errors\":0,\"warnings\":0,\"trigger\":\"schedule\",\"message\":null},"
    "\"next_due\":null,\"due\":false}"
    "]}";

static const struct
{
  const char *id;
  const char *title;
  const char *every;
  const char *status;
} expected[] = {
  { "a1b2c3d4", "https://www.youtube.com/@Never/videos", "Every day",
    "Not checked yet · due now" },
  { "b2c3d4e5", "Fine Channel", "Every 6 hours",
    "Checked 3 h ago · 2 new · next in 3 h" },
  { "c3d4e5f6", "Quiet", "Every 7 days",
    "Checked 24 h ago · nothing new · next in 6 d" },
  { "d4e5f6a7", "Some Errors", "Every hour",
    "Checked just now · 3 new, 1 error · next in 60 min" },
  { "e5f6a7b8", "Broken", "Every 12 hours",
    "Check failed 2 h ago · next in 10 h" },
  { "f6a7b8c9", "Paused One", "Every 2 days",
    "Paused · last checked 3 d ago" },
};

static void
test_parse_fixture (void)
{
  GError *error = NULL;
  g_autoptr (YtdlSubscriptionList) l = ytdl_subscriptions_parse (fixture, &error);
  g_assert_no_error (error);
  g_assert_nonnull (l);
  g_assert_cmpint (l->version, ==, 1);
  g_assert_cmpint (l->now, ==, 1790000000);
  g_assert_cmpuint (l->subscriptions->len, ==, G_N_ELEMENTS (expected));

  g_assert_cmpstr (l->schedule.mechanism, ==, "systemd");
  g_assert_true (l->schedule.supported);
  g_assert_true (l->schedule.installed);
  g_assert_true (l->schedule.active);
  g_assert_false (l->schedule.running);
  g_assert_cmpint (l->schedule.next_check, ==, 1790001500);
  g_assert_cmpint (l->schedule.linger, ==, 0);

  const YtdlSubscription *never = g_ptr_array_index (l->subscriptions, 0);
  g_assert_null (never->name);
  g_assert_null (never->last_run);
  g_assert_null (never->data_root);
  g_assert_true (never->due);

  const YtdlSubscription *fine = g_ptr_array_index (l->subscriptions, 1);
  g_assert_cmpstr (fine->data_root, ==, "/mnt/archive");
  g_assert_cmpuint (g_strv_length (fine->options), ==, 5);
  g_assert_cmpstr (fine->options[4], ==, "socks5://***@127.0.0.1:1080");
  g_assert_nonnull (fine->last_run);
  g_assert_cmpint (fine->last_run->touched, ==, 2);
  g_assert_cmpint (fine->last_run->skipped, ==, 1);
  g_assert_cmpstr (fine->last_run->trigger, ==, "schedule");

  const YtdlSubscription *quiet = g_ptr_array_index (l->subscriptions, 2);
  g_assert_cmpuint (g_strv_length (quiet->options), ==, 0);

  const YtdlSubscription *broken = g_ptr_array_index (l->subscriptions, 4);
  g_assert_cmpstr (broken->last_run->result, ==, "failed");
  g_assert_cmpint (broken->last_run->exit_code, ==, 1);
  g_assert_true (g_str_has_prefix (broken->last_run->message,
                                   "Error: --codec must be one of"));

  const YtdlSubscription *paused = g_ptr_array_index (l->subscriptions, 5);
  g_assert_false (paused->enabled);
  g_assert_cmpint (paused->next_due, ==, 0);
}

static void
test_row_wording (void)
{
  g_autoptr (YtdlSubscriptionList) l = ytdl_subscriptions_parse (fixture, NULL);
  g_assert_nonnull (l);
  for (gsize i = 0; i < G_N_ELEMENTS (expected); i++)
    {
      const YtdlSubscription *s = g_ptr_array_index (l->subscriptions, i);
      g_assert_cmpstr (s->id, ==, expected[i].id);
      g_assert_cmpstr (ytdl_subscription_title (s), ==, expected[i].title);
      g_autofree char *every = ytdl_subscription_every_label (s->every_hours);
      g_assert_cmpstr (every, ==, expected[i].every);
      g_autofree char *status = ytdl_subscription_status_line (s, l->now);
      g_assert_cmpstr (status, ==, expected[i].status);
    }
}

static void
test_schedule_wording (void)
{
  g_autoptr (YtdlSubscriptionList) l = ytdl_subscriptions_parse (fixture, NULL);
  YtdlSchedule s = l->schedule;
  g_autofree char *on = ytdl_schedule_status_line (&s, l->now);
  g_assert_cmpstr (on, ==,
                   "On · checks hourly · next check in 25 min · only while "
                   "you are logged in");

  s.linger = 1;
  s.running = TRUE;
  g_autofree char *running = ytdl_schedule_status_line (&s, l->now);
  g_assert_cmpstr (running, ==,
                   "On · checks hourly · next check in 25 min · checking now");

  s.running = FALSE;
  s.next_check = 0;
  s.linger = -1;
  g_autofree char *no_next = ytdl_schedule_status_line (&s, l->now);
  g_assert_cmpstr (no_next, ==, "On · checks hourly");

  s.active = FALSE;
  g_autofree char *inactive = ytdl_schedule_status_line (&s, l->now);
  g_assert_cmpstr (inactive, ==,
                   "Installed but not running · turn it off and on again");

  s.installed = FALSE;
  g_autofree char *off = ytdl_schedule_status_line (&s, l->now);
  g_assert_cmpstr (off, ==,
                   "Off · subscriptions are checked only when you press "
                   "Check now");

  s.supported = FALSE;
  g_autofree char *unsupported = ytdl_schedule_status_line (&s, l->now);
  g_assert_cmpstr (unsupported, ==,
                   "systemd user timer ytdl-subscriptions.timer, hourly");
}

static void
test_relative_times (void)
{
  const gint64 now = 1790000000;
  static const struct { gint64 delta; const char *ago; const char *in; } cases[] = {
    { 0,      "just now",   "due now" },
    { 89,     "just now",   "in 1 min" },
    { 90,     "2 min ago",  "in 2 min" },
    { 150,    "3 min ago",  "in 3 min" },  /* 2.5 rounds UP, in all three */
    { 5399,   "90 min ago", "in 90 min" },
    { 5400,   "2 h ago",    "in 2 h" },
    { 9000,   "3 h ago",    "in 3 h" },    /* 2.5 h rounds up */
    { 129599, "36 h ago",   "in 36 h" },
    { 129600, "2 d ago",    "in 2 d" },    /* 1.5 d rounds up */
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *ago = ytdl_format_ago (now - cases[i].delta, now);
      g_assert_cmpstr (ago, ==, cases[i].ago);
      g_autofree char *in = ytdl_format_in (now + cases[i].delta, now);
      g_assert_cmpstr (in, ==, cases[i].in);
    }
  /* A clock that went backwards is "just now", not "-3 min ago". */
  g_autofree char *future = ytdl_format_ago (now + 600, now);
  g_assert_cmpstr (future, ==, "just now");
  g_autofree char *past = ytdl_format_in (now - 600, now);
  g_assert_cmpstr (past, ==, "due now");
}

static void
test_every_labels (void)
{
  static const struct { int h; const char *label; } cases[] = {
    { 1, "Every hour" }, { 2, "Every 2 hours" }, { 23, "Every 23 hours" },
    { 24, "Every day" }, { 48, "Every 2 days" }, { 36, "Every 36 hours" },
    { 168, "Every 7 days" }, { 720, "Every 30 days" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *l = ytdl_subscription_every_label (cases[i].h);
      g_assert_cmpstr (l, ==, cases[i].label);
    }
}

static void
test_refuses_what_it_cannot_read (void)
{
  GError *error = NULL;
  g_assert_null (ytdl_subscriptions_parse ("{\"subscriptions_version\":2,"
                                           "\"subscriptions\":[]}",
                                           &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_nonnull (strstr (error->message, "version 2"));
  g_clear_error (&error);

  g_assert_null (ytdl_subscriptions_parse ("[]", &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  g_assert_null (ytdl_subscriptions_parse ("not json", &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  /* An empty list is a list. */
  g_autoptr (YtdlSubscriptionList) empty = ytdl_subscriptions_parse (
      "{\"subscriptions_version\":1,\"now\":1,\"schedule\":{\"supported\":true,"
      "\"detail\":\"not installed\"},\"subscriptions\":[]}",
      &error);
  g_assert_no_error (error);
  g_assert_cmpuint (empty->subscriptions->len, ==, 0);
  g_assert_cmpint (empty->schedule.linger, ==, -1);
}

static char *
joined (GStrv v)
{
  return g_strjoinv (" ", v);
}

static void
test_arguments (void)
{
  g_auto (GStrv) list = ytdl_subscriptions_list_args ();
  g_autofree char *l = joined (list);
  g_assert_cmpstr (l, ==, "--subscriptions --json");

  /* The form, as a subscription: the run's own arguments, connection
   * included, then the subscription flags. */
  g_autoptr (YtdlRunOptions) o = ytdl_run_options_new ();
  o->url = g_strdup ("https://www.youtube.com/@Chan/videos");
  o->sync = TRUE;
  o->quality = g_strdup ("1080");
  o->proxy = g_strdup ("socks5://me:pw@127.0.0.1:1080");
  o->refresh = TRUE; /* never stored: the pipeline refuses it */
  g_auto (GStrv) sub = ytdl_subscribe_args (o, 12, "My Channel");
  g_autofree char *s = joined (sub);
  g_assert_cmpstr (s, ==,
                   "https://www.youtube.com/@Chan/videos --sync --quality 1080 "
                   "--proxy socks5://me:pw@127.0.0.1:1080 --subscribe --every "
                   "12h --name My Channel");

  g_auto (GStrv) daily = ytdl_subscribe_args (o, 48, NULL);
  g_autofree char *d = joined (daily);
  g_assert_true (g_str_has_suffix (d, "--subscribe --every 2d"));

  g_auto (GStrv) pause = ytdl_subscription_edit_args ("b2c3d4e5", 0, 1);
  g_autofree char *p = joined (pause);
  g_assert_cmpstr (p, ==, "--edit-subscription b2c3d4e5 --pause");
  g_auto (GStrv) resume = ytdl_subscription_edit_args ("b2c3d4e5", 168, 0);
  g_autofree char *r = joined (resume);
  g_assert_cmpstr (r, ==,
                   "--edit-subscription b2c3d4e5 --every 7d --resume");
  g_auto (GStrv) every = ytdl_subscription_edit_args ("b2c3d4e5", 6, -1);
  g_autofree char *e = joined (every);
  g_assert_cmpstr (e, ==, "--edit-subscription b2c3d4e5 --every 6h");

  g_auto (GStrv) un = ytdl_unsubscribe_args ("b2c3d4e5");
  g_autofree char *u = joined (un);
  g_assert_cmpstr (u, ==, "--unsubscribe b2c3d4e5");

  g_auto (GStrv) on = ytdl_schedule_args (TRUE);
  g_autofree char *on_s = joined (on);
  g_assert_cmpstr (on_s, ==, "--schedule install");
  g_auto (GStrv) off = ytdl_schedule_args (FALSE);
  g_autofree char *off_s = joined (off);
  g_assert_cmpstr (off_s, ==, "--schedule remove");
}

static void
test_check_now_goes_through_the_queue (void)
{
  g_autoptr (YtdlSubscriptionList) l = ytdl_subscriptions_parse (fixture, NULL);
  const YtdlSubscription *fine = g_ptr_array_index (l->subscriptions, 1);
  g_autoptr (YtdlRunOptions) o = ytdl_subscription_run_options (fine);

  /* What the runner stamps on at enqueue, and what a Run again copies: none of
   * it may reach the command, because the subscription has its own. */
  o->proxy = g_strdup ("http://other:1");
  o->quality = g_strdup ("480");
  g_auto (GStrv) argv = ytdl_run_options_to_args (o);
  g_autofree char *a = joined (argv);
  g_assert_cmpstr (a, ==, "--run-subscriptions b2c3d4e5");
  g_autofree char *preview = ytdl_run_options_command_preview (o);
  g_assert_cmpstr (preview, ==, "ytdl --run-subscriptions b2c3d4e5");

  /* Survives the queue file, and a copy. */
  g_autoptr (JsonBuilder) b = json_builder_new ();
  ytdl_run_options_build_json (b, o);
  g_autoptr (JsonNode) node = json_builder_get_root (b);
  g_autoptr (YtdlRunOptions) back =
      ytdl_run_options_from_json (json_node_get_object (node));
  g_assert_cmpstr (back->subscription_id, ==, "b2c3d4e5");
  g_assert_cmpstr (back->url, ==, "https://www.youtube.com/@Fine/videos");
  g_assert_cmpstr (back->data_root, ==, "/mnt/archive");
  g_autoptr (YtdlRunOptions) copy = ytdl_run_options_copy (back);
  g_assert_cmpstr (copy->subscription_id, ==, "b2c3d4e5");

  /* And a queue file written before subscriptions existed still reads as a
   * plain download. */
  g_autoptr (JsonParser) p = json_parser_new ();
  g_assert_true (json_parser_load_from_data (
      p, "{\"url\":\"https://youtu.be/x\",\"sync\":false}", -1, NULL));
  g_autoptr (YtdlRunOptions) old =
      ytdl_run_options_from_json (json_node_get_object (json_parser_get_root (p)));
  g_assert_null (old->subscription_id);
  g_auto (GStrv) old_argv = ytdl_run_options_to_args (old);
  g_assert_cmpstr (old_argv[0], ==, "https://youtu.be/x");
}

static void
test_failure_messages (void)
{
  YtdlCommandResult r = { 1, NULL, NULL };
  r.err = (char *) "Note: without --sync, every check walks the whole listing.\n"
                   "Error: --codec must be one of: any, avc1, vp9, av01\n";
  g_autofree char *m = ytdl_command_result_message (&r);
  g_assert_cmpstr (m, ==, "Error: --codec must be one of: any, avc1, vp9, av01");

  r.err = (char *) "[subscriptions] Another subscription check is already "
                   "running; not starting a second one.\n";
  r.exit_code = 3;
  g_autofree char *busy = ytdl_command_result_message (&r);
  g_assert_true (g_str_has_prefix (busy, "[subscriptions] Another"));

  r.err = NULL;
  g_autofree char *bare = ytdl_command_result_message (&r);
  g_assert_cmpstr (bare, ==, "ytdl exited with code 3");

  /* The two ways an older pipeline answers. */
  r.exit_code = 1;
  r.err = (char *) "Warning: '--subscriptions' does not look like a YouTube "
                   "URL.\nUnknown option: --json\n";
  g_assert_true (ytdl_command_result_means_too_old (&r));
  r.err = (char *) "Error: /x/scripts/subscriptions.ps1 is missing -- this "
                   "install predates subscriptions.\n";
  g_assert_true (ytdl_command_result_means_too_old (&r));
  r.err = (char *) "Error: no subscription 'x'.\n";
  g_assert_false (ytdl_command_result_means_too_old (&r));
}

void
ytdl_register_subscription_tests (void)
{
  g_test_add_func ("/subscriptions/parse-fixture", test_parse_fixture);
  g_test_add_func ("/subscriptions/row-wording", test_row_wording);
  g_test_add_func ("/subscriptions/schedule-wording", test_schedule_wording);
  g_test_add_func ("/subscriptions/relative-times", test_relative_times);
  g_test_add_func ("/subscriptions/every-labels", test_every_labels);
  g_test_add_func ("/subscriptions/refuses-what-it-cannot-read",
                   test_refuses_what_it_cannot_read);
  g_test_add_func ("/subscriptions/arguments", test_arguments);
  g_test_add_func ("/subscriptions/check-now-goes-through-the-queue",
                   test_check_now_goes_through_the_queue);
  g_test_add_func ("/subscriptions/failure-messages", test_failure_messages);
}
