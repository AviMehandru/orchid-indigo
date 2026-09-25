#include "subscriptions.h"

#include "paths.h"

#include <json-glib/json-glib.h>
#include <stdarg.h>
#include <string.h>

const int ytdl_subscription_interval_choices[] = { 1, 6, 12, 24, 72, 168 };
const gsize ytdl_subscription_interval_count =
    G_N_ELEMENTS (ytdl_subscription_interval_choices);

/* ---------------------------------------------------------------------- */
/* Reading the document                                                   */
/* ---------------------------------------------------------------------- */

static void
run_free (YtdlSubscriptionRun *r)
{
  if (r == NULL)
    return;
  g_free (r->result);
  g_free (r->trigger);
  g_free (r->message);
  g_free (r);
}

static void
subscription_free (gpointer data)
{
  YtdlSubscription *s = data;
  if (s == NULL)
    return;
  g_free (s->id);
  g_free (s->url);
  g_free (s->name);
  g_strfreev (s->options);
  g_free (s->data_root);
  run_free (s->last_run);
  g_free (s);
}

void
ytdl_subscription_list_free (YtdlSubscriptionList *l)
{
  if (l == NULL)
    return;
  g_free (l->schedule.mechanism);
  g_free (l->schedule.detail);
  g_clear_pointer (&l->subscriptions, g_ptr_array_unref);
  g_free (l);
}

/* Every reader below treats a missing member, a null and a member of the
 * wrong type the same way: as absent. The pipeline writes every field on
 * every object, so absence means a newer or older pipeline than this app
 * knows, and the right response to that is a default rather than a refusal
 * of the whole list. */
static JsonNode *
member (JsonObject *o, const char *name)
{
  if (o == NULL || !json_object_has_member (o, name))
    return NULL;
  JsonNode *n = json_object_get_member (o, name);
  return JSON_NODE_HOLDS_NULL (n) ? NULL : n;
}

static char *
str_of (JsonObject *o, const char *name)
{
  JsonNode *n = member (o, name);
  if (n == NULL || !JSON_NODE_HOLDS_VALUE (n)
      || json_node_get_value_type (n) != G_TYPE_STRING)
    return NULL;
  return g_strdup (json_node_get_string (n));
}

static gint64
int_of (JsonObject *o, const char *name, gint64 dflt)
{
  JsonNode *n = member (o, name);
  if (n == NULL || !JSON_NODE_HOLDS_VALUE (n))
    return dflt;
  GType t = json_node_get_value_type (n);
  if (t == G_TYPE_INT64 || t == G_TYPE_INT)
    return json_node_get_int (n);
  if (t == G_TYPE_DOUBLE)
    return (gint64) json_node_get_double (n);
  return dflt;
}

static gboolean
bool_of (JsonObject *o, const char *name, gboolean dflt)
{
  JsonNode *n = member (o, name);
  if (n == NULL || !JSON_NODE_HOLDS_VALUE (n)
      || json_node_get_value_type (n) != G_TYPE_BOOLEAN)
    return dflt;
  return json_node_get_boolean (n);
}

static JsonObject *
obj_of (JsonObject *o, const char *name)
{
  JsonNode *n = member (o, name);
  return (n != NULL && JSON_NODE_HOLDS_OBJECT (n)) ? json_node_get_object (n)
                                                    : NULL;
}

static GStrv
strv_of (JsonObject *o, const char *name)
{
  GPtrArray *v = g_ptr_array_new ();
  JsonNode *n = member (o, name);
  if (n != NULL && JSON_NODE_HOLDS_ARRAY (n))
    {
      JsonArray *a = json_node_get_array (n);
      for (guint i = 0; i < json_array_get_length (a); i++)
        {
          JsonNode *e = json_array_get_element (a, i);
          if (JSON_NODE_HOLDS_VALUE (e)
              && json_node_get_value_type (e) == G_TYPE_STRING)
            g_ptr_array_add (v, g_strdup (json_node_get_string (e)));
        }
    }
  g_ptr_array_add (v, NULL);
  return (GStrv) g_ptr_array_free (v, FALSE);
}

static YtdlSubscription *
read_subscription (JsonObject *o)
{
  YtdlSubscription *s = g_new0 (YtdlSubscription, 1);
  s->id = str_of (o, "id");
  s->url = str_of (o, "url");
  s->name = str_of (o, "name");
  s->options = strv_of (o, "options");
  s->data_root = str_of (o, "data_root");
  s->every_hours = (int) int_of (o, "every_hours", 24);
  s->enabled = bool_of (o, "enabled", TRUE);
  s->added = int_of (o, "added", 0);
  s->updated = int_of (o, "updated", 0);
  s->next_due = int_of (o, "next_due", 0);
  s->due = bool_of (o, "due", FALSE);

  JsonObject *lr = obj_of (o, "last_run");
  if (lr != NULL)
    {
      YtdlSubscriptionRun *r = g_new0 (YtdlSubscriptionRun, 1);
      r->started = int_of (lr, "started", 0);
      r->finished = int_of (lr, "finished", 0);
      r->result = str_of (lr, "result");
      r->exit_code = (int) int_of (lr, "exit_code", 0);
      r->touched = int_of (lr, "touched", 0);
      r->skipped = int_of (lr, "skipped", 0);
      r->errors = int_of (lr, "errors", 0);
      r->warnings = int_of (lr, "warnings", 0);
      r->trigger = str_of (lr, "trigger");
      r->message = str_of (lr, "message");
      s->last_run = r;
    }
  return s;
}

YtdlSubscriptionList *
ytdl_subscriptions_parse (const char *json, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (json == NULL || !json_parser_load_from_data (parser, json, -1, error))
    {
      if (json == NULL)
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "the pipeline printed nothing");
      return NULL;
    }
  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "not a subscriptions document");
      return NULL;
    }
  JsonObject *o = json_node_get_object (root);
  gint64 version = int_of (o, "subscriptions_version", 0);
  if (version < 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "not a subscriptions document");
      return NULL;
    }
  if (version > YTDL_SUPPORTED_SUBSCRIPTIONS_VERSION)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "the pipeline's subscriptions are version %" G_GINT64_FORMAT
                   "; this app reads version %d. Update the app.",
                   version, YTDL_SUPPORTED_SUBSCRIPTIONS_VERSION);
      return NULL;
    }

  YtdlSubscriptionList *l = g_new0 (YtdlSubscriptionList, 1);
  l->version = (int) version;
  l->now = int_of (o, "now", g_get_real_time () / G_USEC_PER_SEC);

  JsonObject *sc = obj_of (o, "schedule");
  l->schedule.mechanism = str_of (sc, "mechanism");
  l->schedule.supported = bool_of (sc, "supported", FALSE);
  l->schedule.installed = bool_of (sc, "installed", FALSE);
  l->schedule.active = bool_of (sc, "active", FALSE);
  l->schedule.running = bool_of (sc, "running", FALSE);
  l->schedule.next_check = int_of (sc, "next_check", 0);
  {
    JsonNode *n = member (sc, "linger");
    l->schedule.linger =
        (n != NULL && JSON_NODE_HOLDS_VALUE (n)
         && json_node_get_value_type (n) == G_TYPE_BOOLEAN)
            ? (json_node_get_boolean (n) ? 1 : 0)
            : -1;
  }
  l->schedule.detail = str_of (sc, "detail");

  l->subscriptions = g_ptr_array_new_with_free_func (subscription_free);
  JsonNode *subs = member (o, "subscriptions");
  if (subs != NULL && JSON_NODE_HOLDS_ARRAY (subs))
    {
      JsonArray *a = json_node_get_array (subs);
      for (guint i = 0; i < json_array_get_length (a); i++)
        {
          JsonNode *e = json_array_get_element (a, i);
          if (JSON_NODE_HOLDS_OBJECT (e))
            {
              YtdlSubscription *s = read_subscription (json_node_get_object (e));
              /* Without an id there is nothing to send back; a row that
               * cannot be acted on is left out rather than shown dead. */
              if (s->id != NULL && s->url != NULL)
                g_ptr_array_add (l->subscriptions, s);
              else
                subscription_free (s);
            }
        }
    }
  return l;
}

/* ---------------------------------------------------------------------- */
/* Wording                                                                */
/* ---------------------------------------------------------------------- */

/* Half-up integer rounding. Not round(): the three apps must agree to the
 * minute on the same fixture, and floating-point rounding of x.5 is the
 * one place C, Swift and C# are allowed to differ. */
static gint64
round_div (gint64 n, gint64 d)
{
  return (n + d / 2) / d;
}

char *
ytdl_format_ago (gint64 then, gint64 now)
{
  gint64 s = MAX (0, now - then);
  if (s < 90)
    return g_strdup ("just now");
  if (s < 5400)
    return g_strdup_printf ("%" G_GINT64_FORMAT " min ago", round_div (s, 60));
  if (s < 129600)
    return g_strdup_printf ("%" G_GINT64_FORMAT " h ago", round_div (s, 3600));
  return g_strdup_printf ("%" G_GINT64_FORMAT " d ago", round_div (s, 86400));
}

char *
ytdl_format_in (gint64 when, gint64 now)
{
  gint64 d = when - now;
  if (d <= 0)
    return g_strdup ("due now");
  if (d < 5400)
    return g_strdup_printf ("in %" G_GINT64_FORMAT " min",
                            MAX (1, round_div (d, 60)));
  if (d < 129600)
    return g_strdup_printf ("in %" G_GINT64_FORMAT " h", round_div (d, 3600));
  return g_strdup_printf ("in %" G_GINT64_FORMAT " d", round_div (d, 86400));
}

const char *
ytdl_subscription_title (const YtdlSubscription *s)
{
  if (s->name != NULL && *s->name != '\0')
    return s->name;
  return s->url;
}

char *
ytdl_subscription_every_label (int hours)
{
  if (hours > 0 && hours % 24 == 0)
    return hours == 24 ? g_strdup ("Every day")
                       : g_strdup_printf ("Every %d days", hours / 24);
  return hours == 1 ? g_strdup ("Every hour")
                    : g_strdup_printf ("Every %d hours", hours);
}

static char *
found_phrase (const YtdlSubscriptionRun *r)
{
  GString *s = g_string_new (NULL);
  if (r->touched > 0)
    g_string_append_printf (s, "%" G_GINT64_FORMAT " new", r->touched);
  else
    g_string_append (s, "nothing new");
  if (r->errors > 0)
    g_string_append_printf (s, ", %" G_GINT64_FORMAT " error%s", r->errors,
                            r->errors == 1 ? "" : "s");
  return g_string_free (s, FALSE);
}

char *
ytdl_subscription_status_line (const YtdlSubscription *s, gint64 now)
{
  const YtdlSubscriptionRun *r = s->last_run;

  if (!s->enabled)
    {
      if (r == NULL)
        return g_strdup ("Paused · never checked");
      g_autofree char *ago = ytdl_format_ago (r->started, now);
      return g_strdup_printf ("Paused · last checked %s", ago);
    }

  g_autofree char *next =
      ytdl_format_in (s->next_due > 0 ? s->next_due : now, now);
  if (r == NULL)
    return g_strdup_printf ("Not checked yet · %s", next);

  g_autofree char *ago = ytdl_format_ago (r->started, now);
  /* "next due now" reads badly; "due now" on its own says it. */
  g_autofree char *tail = g_str_has_prefix (next, "in ")
                              ? g_strdup_printf ("next %s", next)
                              : g_strdup (next);
  if (g_strcmp0 (r->result, "failed") == 0)
    return g_strdup_printf ("Check failed %s · %s", ago, tail);
  g_autofree char *found = found_phrase (r);
  return g_strdup_printf ("Checked %s · %s · %s", ago, found, tail);
}

char *
ytdl_schedule_status_line (const YtdlSchedule *s, gint64 now)
{
  if (!s->supported)
    return g_strdup (s->detail != NULL ? s->detail
                                       : "This machine has no scheduler the "
                                         "pipeline can use.");

  GString *out = g_string_new (NULL);
  if (s->installed && s->active)
    {
      g_string_append (out, "On · checks hourly");
      if (s->next_check > 0)
        {
          g_autofree char *in = ytdl_format_in (s->next_check, now);
          g_string_append_printf (out, " · next check %s", in);
        }
      if (s->linger == 0)
        g_string_append (out, " · only while you are logged in");
    }
  else if (s->installed)
    g_string_append (out,
                     "Installed but not running · turn it off and on again");
  else
    g_string_append (
        out, "Off · subscriptions are checked only when you press Check now");

  if (s->running)
    g_string_append (out, " · checking now");
  return g_string_free (out, FALSE);
}

/* ---------------------------------------------------------------------- */
/* Arguments                                                              */
/* ---------------------------------------------------------------------- */

static GStrv
strv_from (const char *first, ...)
{
  GPtrArray *v = g_ptr_array_new ();
  va_list ap;
  va_start (ap, first);
  for (const char *a = first; a != NULL; a = va_arg (ap, const char *))
    g_ptr_array_add (v, g_strdup (a));
  va_end (ap);
  g_ptr_array_add (v, NULL);
  return (GStrv) g_ptr_array_free (v, FALSE);
}

GStrv
ytdl_subscriptions_list_args (void)
{
  return strv_from ("--subscriptions", "--json", NULL);
}

GStrv
ytdl_subscribe_args (const YtdlRunOptions *opts, int every_hours,
                     const char *name)
{
  g_return_val_if_fail (opts != NULL, NULL);
  /* The run's own argument list, then the three subscription flags after it.
   * Built from ytdl_run_options_to_args rather than a second list of fields,
   * so an option added to the form is subscribable the day it is added. */
  g_autoptr (YtdlRunOptions) copy = ytdl_run_options_copy (opts);
  g_clear_pointer (&copy->subscription_id, g_free);
  copy->refresh = FALSE;
  g_auto (GStrv) run = ytdl_run_options_to_args (copy);

  GPtrArray *v = g_ptr_array_new ();
  for (gsize i = 0; run[i] != NULL; i++)
    g_ptr_array_add (v, g_strdup (run[i]));
  g_ptr_array_add (v, g_strdup ("--subscribe"));
  if (every_hours > 0)
    {
      g_ptr_array_add (v, g_strdup ("--every"));
      g_ptr_array_add (v, (every_hours % 24 == 0)
                              ? g_strdup_printf ("%dd", every_hours / 24)
                              : g_strdup_printf ("%dh", every_hours));
    }
  if (name != NULL && *name != '\0')
    {
      g_ptr_array_add (v, g_strdup ("--name"));
      g_ptr_array_add (v, g_strdup (name));
    }
  g_ptr_array_add (v, NULL);
  return (GStrv) g_ptr_array_free (v, FALSE);
}

GStrv
ytdl_subscription_edit_args (const char *id, int every_hours, int pause)
{
  GPtrArray *v = g_ptr_array_new ();
  g_ptr_array_add (v, g_strdup ("--edit-subscription"));
  g_ptr_array_add (v, g_strdup (id));
  if (every_hours > 0)
    {
      g_ptr_array_add (v, g_strdup ("--every"));
      g_ptr_array_add (v, (every_hours % 24 == 0)
                              ? g_strdup_printf ("%dd", every_hours / 24)
                              : g_strdup_printf ("%dh", every_hours));
    }
  if (pause == 1)
    g_ptr_array_add (v, g_strdup ("--pause"));
  else if (pause == 0)
    g_ptr_array_add (v, g_strdup ("--resume"));
  g_ptr_array_add (v, NULL);
  return (GStrv) g_ptr_array_free (v, FALSE);
}

GStrv
ytdl_unsubscribe_args (const char *id)
{
  return strv_from ("--unsubscribe", id, NULL);
}

GStrv
ytdl_schedule_args (gboolean install)
{
  return strv_from ("--schedule", install ? "install" : "remove", NULL);
}

YtdlRunOptions *
ytdl_subscription_run_options (const YtdlSubscription *s)
{
  YtdlRunOptions *o = ytdl_run_options_new ();
  o->subscription_id = g_strdup (s->id);
  o->url = g_strdup (s->url);
  o->data_root = g_strdup (s->data_root);
  return o;
}

/* ---------------------------------------------------------------------- */
/* Running                                                                */
/* ---------------------------------------------------------------------- */

void
ytdl_command_result_free (YtdlCommandResult *r)
{
  if (r == NULL)
    return;
  g_free (r->out);
  g_free (r->err);
  g_free (r);
}

char *
ytdl_command_result_message (const YtdlCommandResult *r)
{
  const char *sources[] = { r->err, r->out };
  /* An "Error:" line anywhere wins: ytdl.ps1 prints notes before it gets to
   * the refusal ("Note: without --sync ..."), and the note is not why the
   * command failed. */
  for (gsize k = 0; k < G_N_ELEMENTS (sources); k++)
    {
      if (sources[k] == NULL)
        continue;
      g_auto (GStrv) lines = g_strsplit (sources[k], "\n", -1);
      for (gsize i = 0; lines[i] != NULL; i++)
        {
          g_strstrip (lines[i]);
          if (g_str_has_prefix (lines[i], "Error:")
              || g_str_has_prefix (lines[i], "[subscriptions]"))
            return g_strdup (lines[i]);
        }
    }
  for (gsize k = 0; k < G_N_ELEMENTS (sources); k++)
    {
      if (sources[k] == NULL)
        continue;
      g_auto (GStrv) lines = g_strsplit (sources[k], "\n", -1);
      for (gsize i = 0; lines[i] != NULL; i++)
        {
          g_strstrip (lines[i]);
          if (*lines[i] != '\0')
            return g_strdup (lines[i]);
        }
    }
  return g_strdup_printf ("ytdl exited with code %d", r->exit_code);
}

gboolean
ytdl_command_result_means_too_old (const YtdlCommandResult *r)
{
  if (r == NULL || r->exit_code == 0)
    return FALSE;
  const char *text = r->err != NULL ? r->err : "";
  return strstr (text, "predates subscriptions") != NULL
         || strstr (text, "Unknown option: --json") != NULL
         || strstr (text, "Unknown option: --subscribe") != NULL;
}

static void
command_thread (GTask *task, gpointer source, gpointer task_data,
                GCancellable *cancellable)
{
  GStrv args = task_data;
  g_autofree char *pwsh = ytdl_find_pwsh ();
  g_autofree char *scripts = ytdl_scripts_dir ();
  g_autofree char *script = g_build_filename (scripts, "ytdl.ps1", NULL);
  if (pwsh == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "pwsh was not found, so the pipeline cannot "
                               "be run. See the Health pane.");
      return;
    }
  if (!g_file_test (script, G_FILE_TEST_IS_REGULAR))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "%s does not exist. Install the pipeline, or "
                               "set YTDLP_INSTALL_ROOT to where it lives.",
                               script);
      return;
    }

  g_autoptr (GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv, g_strdup (pwsh));
  g_ptr_array_add (argv, g_strdup ("-NoProfile"));
  g_ptr_array_add (argv, g_strdup ("-File"));
  g_ptr_array_add (argv, g_strdup (script));
  for (gsize i = 0; args != NULL && args[i] != NULL; i++)
    g_ptr_array_add (argv, g_strdup (args[i]));
  g_ptr_array_add (argv, NULL);

  GError *error = NULL;
  g_autoptr (GSubprocess) proc = g_subprocess_newv (
      (const char *const *) argv->pdata,
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
  if (proc == NULL)
    {
      g_task_return_error (task, error);
      return;
    }
  YtdlCommandResult *r = g_new0 (YtdlCommandResult, 1);
  if (!g_subprocess_communicate_utf8 (proc, NULL, cancellable, &r->out, &r->err,
                                      &error))
    {
      g_subprocess_force_exit (proc);
      ytdl_command_result_free (r);
      g_task_return_error (task, error);
      return;
    }
  r->exit_code = g_subprocess_get_if_exited (proc)
                     ? g_subprocess_get_exit_status (proc)
                     : -1;
  g_task_return_pointer (task, r, (GDestroyNotify) ytdl_command_result_free);
}

void
ytdl_command_run_async (const char *const *args, GCancellable *cancellable,
                        GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, ytdl_command_run_async);
  g_task_set_task_data (task, g_strdupv ((GStrv) args),
                        (GDestroyNotify) g_strfreev);
  g_task_run_in_thread (task, command_thread);
  g_object_unref (task);
}

YtdlCommandResult *
ytdl_command_run_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}
