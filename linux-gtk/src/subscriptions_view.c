#include "subscriptions_view.h"

#include "subscriptions.h"

#include <adwaita.h>
#include <string.h>

struct _YtdlSubscriptionsView
{
  GtkBox parent_instance;

  YtdlRunner *runner; /* borrowed */

  GtkWidget *content;      /* the normal page */
  GtkWidget *problem;      /* AdwStatusPage shown instead when the list
                              cannot be read at all */
  GtkWidget *spinner;
  GtkWidget *refresh;
  GtkWidget *schedule_row;
  GtkWidget *schedule_switch;
  GtkWidget *list;

  YtdlSubscriptionList *current; /* owned, may be NULL */
  GCancellable         *cancel;

  gboolean loading;
  /* Set while the switch is being moved to match what the pipeline said,
   * so that moving it does not read as the user asking for the opposite. */
  gboolean syncing_switch;
};

enum
{
  SIG_MESSAGE,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (YtdlSubscriptionsView, ytdl_subscriptions_view,
                     GTK_TYPE_BOX)

static void
say (YtdlSubscriptionsView *self, const char *text)
{
  g_signal_emit (self, signals[SIG_MESSAGE], 0, text);
}

/* ---------------------------------------------------------------------- */
/* Running a command, then reading the list again                         */
/* ---------------------------------------------------------------------- */

typedef struct
{
  YtdlSubscriptionsView *self; /* ref held */
  char                  *success; /* toast on success; may be NULL */
} Op;

static void
op_free (Op *op)
{
  g_object_unref (op->self);
  g_free (op->success);
  g_free (op);
}

static void
on_op_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  Op *op = user_data;
  YtdlSubscriptionsView *self = op->self;
  GError *error = NULL;
  g_autoptr (YtdlCommandResult) r = ytdl_command_run_finish (res, &error);

  gtk_widget_set_sensitive (self->schedule_switch, TRUE);
  if (r == NULL)
    say (self, error->message);
  else if (r->exit_code != 0)
    {
      g_autofree char *why = ytdl_command_result_message (r);
      say (self, why);
    }
  else if (op->success != NULL)
    say (self, op->success);
  g_clear_error (&error);

  /* Always: a refused command may still have changed something (a
   * --schedule install that wrote the unit and then could not enable it
   * takes the unit away again, but the list is the authority either way). */
  ytdl_subscriptions_view_refresh (self);
  op_free (op);
}

static void
run_op (YtdlSubscriptionsView *self, GStrv args, const char *success)
{
  Op *op = g_new0 (Op, 1);
  op->self = g_object_ref (self);
  op->success = g_strdup (success);
  ytdl_command_run_async ((const char *const *) args, NULL, on_op_done, op);
}

/* ---------------------------------------------------------------------- */
/* Rows                                                                   */
/* ---------------------------------------------------------------------- */

static GtkWidget *
pill (const char *text, const char *variant)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_widget_add_css_class (l, "ytdl-pill");
  if (variant != NULL)
    gtk_widget_add_css_class (l, variant);
  gtk_widget_set_valign (l, GTK_ALIGN_CENTER);
  /* One width for every state, so the titles line up down the list
   * rather than stepping in and out with the length of the word. */
  gtk_widget_set_size_request (l, 64, -1);
  return l;
}

/* Removes the ROWS, leaving the placeholder -- see health_view.c's copy of
 * this for why gtk_list_box_remove_all() is the wrong tool. */
static void
clear_list (GtkWidget *list)
{
  GtkListBoxRow *row;
  while ((row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (list), 0)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (list), GTK_WIDGET (row));
}

static const YtdlSubscription *
find (YtdlSubscriptionsView *self, const char *id)
{
  if (self->current == NULL || id == NULL)
    return NULL;
  for (guint i = 0; i < self->current->subscriptions->len; i++)
    {
      const YtdlSubscription *s =
          g_ptr_array_index (self->current->subscriptions, i);
      if (g_strcmp0 (s->id, id) == 0)
        return s;
    }
  return NULL;
}

static const char *
row_id (GtkWidget *w)
{
  return g_object_get_data (G_OBJECT (w), "ytdl-sub-id");
}

static void
set_row_id (GtkWidget *w, const char *id)
{
  g_object_set_data_full (G_OBJECT (w), "ytdl-sub-id", g_strdup (id), g_free);
}

static void
close_popover_of (GtkWidget *w)
{
  GtkWidget *pop = gtk_widget_get_ancestor (w, GTK_TYPE_POPOVER);
  if (pop != NULL)
    gtk_popover_popdown (GTK_POPOVER (pop));
}

static void
on_check_now (GtkButton *b, gpointer user_data)
{
  YtdlSubscriptionsView *self = user_data;
  const YtdlSubscription *s = find (self, row_id (GTK_WIDGET (b)));
  if (s == NULL)
    return;
  /* Through the queue, like every other run: sequential with them, its
   * progress on the Downloads pane, cancellable there. */
  g_autoptr (YtdlRunOptions) o = ytdl_subscription_run_options (s);
  GError *error = NULL;
  g_autofree char *id = ytdl_runner_enqueue (self->runner, o, &error);
  if (id == NULL)
    {
      say (self, error->message);
      g_clear_error (&error);
      return;
    }
  g_autofree char *msg = g_strdup_printf (
      "Queued a check of %s. Its progress is on the Downloads pane.",
      ytdl_subscription_title (s));
  say (self, msg);
}

static void
on_toggle_pause (GtkButton *b, gpointer user_data)
{
  YtdlSubscriptionsView *self = user_data;
  close_popover_of (GTK_WIDGET (b));
  const YtdlSubscription *s = find (self, row_id (GTK_WIDGET (b)));
  if (s == NULL)
    return;
  g_auto (GStrv) args = ytdl_subscription_edit_args (s->id, 0, s->enabled ? 1 : 0);
  run_op (self, args, NULL);
}

static void
on_set_every (GtkButton *b, gpointer user_data)
{
  YtdlSubscriptionsView *self = user_data;
  close_popover_of (GTK_WIDGET (b));
  const YtdlSubscription *s = find (self, row_id (GTK_WIDGET (b)));
  int hours = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "ytdl-hours"));
  if (s == NULL || hours <= 0 || hours == s->every_hours)
    return;
  g_auto (GStrv) args = ytdl_subscription_edit_args (s->id, hours, -1);
  run_op (self, args, NULL);
}

static void
on_remove_response (AdwAlertDialog *dlg, const char *response,
                    gpointer user_data)
{
  YtdlSubscriptionsView *self = user_data;
  if (g_strcmp0 (response, "remove") != 0)
    return;
  const YtdlSubscription *s =
      find (self, g_object_get_data (G_OBJECT (dlg), "ytdl-sub-id"));
  if (s == NULL)
    return;
  g_auto (GStrv) args = ytdl_unsubscribe_args (s->id);
  g_autofree char *msg =
      g_strdup_printf ("Unsubscribed from %s.", ytdl_subscription_title (s));
  run_op (self, args, msg);
}

static void
on_remove (GtkButton *b, gpointer user_data)
{
  YtdlSubscriptionsView *self = user_data;
  close_popover_of (GTK_WIDGET (b));
  const YtdlSubscription *s = find (self, row_id (GTK_WIDGET (b)));
  if (s == NULL)
    return;
  g_autofree char *heading =
      g_strdup_printf ("Unsubscribe from %s?", ytdl_subscription_title (s));
  AdwDialog *dlg = adw_alert_dialog_new (
      heading, "It will not be checked again. Nothing it has already "
               "archived is deleted.");
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg), "cancel", "_Cancel",
                                  "remove", "_Unsubscribe", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg), "remove",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "cancel");
  g_object_set_data_full (G_OBJECT (dlg), "ytdl-sub-id", g_strdup (s->id),
                          g_free);
  g_signal_connect (dlg, "response", G_CALLBACK (on_remove_response), self);
  adw_dialog_present (dlg, GTK_WIDGET (self));
}

static GtkWidget *
menu_button_row (const char *label, const char *id, GCallback cb,
                 YtdlSubscriptionsView *self)
{
  GtkWidget *b = gtk_button_new_with_label (label);
  gtk_widget_add_css_class (b, "flat");
  gtk_label_set_xalign (GTK_LABEL (gtk_button_get_child (GTK_BUTTON (b))), 0.0f);
  set_row_id (b, id);
  g_signal_connect (b, "clicked", cb, self);
  return b;
}

/* A popover of plain buttons rather than a GMenu: every entry carries the
 * subscription's id, and a GAction per row per interval would be a lot of
 * machinery for six rows of buttons. */
static GtkWidget *
row_menu (YtdlSubscriptionsView *self, const YtdlSubscription *s)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start (box, 4);
  gtk_widget_set_margin_end (box, 4);
  gtk_widget_set_margin_top (box, 4);
  gtk_widget_set_margin_bottom (box, 4);

  gtk_box_append (GTK_BOX (box),
                  menu_button_row (s->enabled ? "Pause" : "Resume", s->id,
                                   G_CALLBACK (on_toggle_pause), self));
  gtk_box_append (GTK_BOX (box),
                  gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

  GtkWidget *head = gtk_label_new ("Check it");
  gtk_label_set_xalign (GTK_LABEL (head), 0.0f);
  gtk_widget_add_css_class (head, "caption-heading");
  gtk_widget_add_css_class (head, "dim-label");
  gtk_widget_set_margin_start (head, 10);
  gtk_widget_set_margin_top (head, 4);
  gtk_box_append (GTK_BOX (box), head);

  gboolean listed = FALSE;
  for (gsize i = 0; i < ytdl_subscription_interval_count; i++)
    {
      int h = ytdl_subscription_interval_choices[i];
      g_autofree char *label = ytdl_subscription_every_label (h);
      /* A check mark on the current interval, as text, because a toggle
       * group here would imply a choice that applies before you click. */
      g_autofree char *shown =
          g_strdup_printf ("%s%s", h == s->every_hours ? "✓ " : "    ", label);
      GtkWidget *b = menu_button_row (shown, s->id, G_CALLBACK (on_set_every),
                                      self);
      g_object_set_data (G_OBJECT (b), "ytdl-hours", GINT_TO_POINTER (h));
      gtk_box_append (GTK_BOX (box), b);
      listed |= (h == s->every_hours);
    }
  if (!listed)
    {
      /* An interval set from a terminal that the menu does not offer is
       * shown, not silently replaced by the nearest one. */
      g_autofree char *label = ytdl_subscription_every_label (s->every_hours);
      g_autofree char *shown = g_strdup_printf ("✓ %s", label);
      GtkWidget *cur = gtk_label_new (shown);
      gtk_label_set_xalign (GTK_LABEL (cur), 0.0f);
      gtk_widget_set_margin_start (cur, 10);
      gtk_widget_add_css_class (cur, "dim-label");
      gtk_box_append (GTK_BOX (box), cur);
    }

  gtk_box_append (GTK_BOX (box),
                  gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  GtkWidget *rm = menu_button_row ("Unsubscribe…", s->id,
                                   G_CALLBACK (on_remove), self);
  gtk_widget_add_css_class (gtk_button_get_child (GTK_BUTTON (rm)), "error");
  gtk_box_append (GTK_BOX (box), rm);

  GtkWidget *pop = gtk_popover_new ();
  gtk_popover_set_child (GTK_POPOVER (pop), box);
  GtkWidget *mb = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (mb), "view-more-symbolic");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (mb), pop);
  gtk_widget_add_css_class (mb, "flat");
  gtk_widget_set_valign (mb, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (mb, "Pause, change how often, or unsubscribe");
  return mb;
}

static GtkWidget *
subscription_row (YtdlSubscriptionsView *self, const YtdlSubscription *s,
                  gint64 now)
{
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                 ytdl_subscription_title (s));

  g_autofree char *every = ytdl_subscription_every_label (s->every_hours);
  g_autofree char *status = ytdl_subscription_status_line (s, now);
  GString *sub = g_string_new (NULL);
  g_string_append_printf (sub, "%s · %s", every, status);
  if (s->last_run != NULL && g_strcmp0 (s->last_run->result, "failed") == 0
      && s->last_run->message != NULL)
    g_string_append_printf (sub, "\n%s", s->last_run->message);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub->str);
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 0);
  g_string_free (sub, TRUE);

  /* Everything else about it on hover: the URL when a name is shown, the
   * stored options (the pipeline has already masked a proxy password), and
   * where it archives to. */
  {
    GString *tip = g_string_new (s->url);
    g_autofree char *opts = g_strjoinv (" ", s->options);
    if (*opts != '\0')
      g_string_append_printf (tip, "\n%s", opts);
    g_string_append_printf (tip, "\nInto %s",
                            s->data_root != NULL ? s->data_root
                                                 : "the pipeline's default data root");
    gtk_widget_set_tooltip_text (row, tip->str);
    g_string_free (tip, TRUE);
  }

  const char *label = "new", *variant = NULL;
  if (!s->enabled)
    label = "paused";
  else if (s->last_run != NULL)
    {
      if (g_strcmp0 (s->last_run->result, "failed") == 0)
        label = "failed", variant = "err";
      else if (g_strcmp0 (s->last_run->result, "errors") == 0)
        label = "errors", variant = "warn";
      else
        label = "ok", variant = "ok";
    }
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), pill (label, variant));

  GtkWidget *check = gtk_button_new_with_label ("Check now");
  gtk_widget_add_css_class (check, "flat");
  gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (
      check, "Queue a check of this subscription now, even if it is paused");
  set_row_id (check, s->id);
  g_signal_connect (check, "clicked", G_CALLBACK (on_check_now), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), check);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), row_menu (self, s));
  return row;
}

/* ---------------------------------------------------------------------- */
/* Reading the list                                                       */
/* ---------------------------------------------------------------------- */

static void
show_problem (YtdlSubscriptionsView *self, const char *title,
              const char *description)
{
  adw_status_page_set_title (ADW_STATUS_PAGE (self->problem), title);
  adw_status_page_set_description (ADW_STATUS_PAGE (self->problem),
                                   description);
  gtk_widget_set_visible (self->content, FALSE);
  gtk_widget_set_visible (self->problem, TRUE);
}

static void
fill (YtdlSubscriptionsView *self)
{
  const YtdlSubscriptionList *l = self->current;
  gtk_widget_set_visible (self->problem, FALSE);
  gtk_widget_set_visible (self->content, TRUE);

  g_autofree char *line = ytdl_schedule_status_line (&l->schedule, l->now);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->schedule_row), line);
  self->syncing_switch = TRUE;
  gtk_switch_set_active (GTK_SWITCH (self->schedule_switch),
                         l->schedule.installed);
  self->syncing_switch = FALSE;
  /* Unsupported: there is no scheduler here to turn on, and a switch that
   * flips back every time it is pressed is worse than one that says why it
   * cannot move. The subtitle is the pipeline's own reason. */
  gtk_widget_set_sensitive (self->schedule_switch,
                            l->schedule.supported || l->schedule.installed);

  clear_list (self->list);
  for (guint i = 0; i < l->subscriptions->len; i++)
    gtk_list_box_append (
        GTK_LIST_BOX (self->list),
        subscription_row (self, g_ptr_array_index (l->subscriptions, i),
                          l->now));
}

static void
on_list_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  YtdlSubscriptionsView *self = user_data;
  GError *error = NULL;
  g_autoptr (YtdlCommandResult) r = ytdl_command_run_finish (res, &error);

  self->loading = FALSE;
  gtk_spinner_stop (GTK_SPINNER (self->spinner));
  gtk_widget_set_visible (self->spinner, FALSE);
  gtk_widget_set_sensitive (self->refresh, TRUE);

  if (r == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        show_problem (self, "The pipeline could not be run", error->message);
      g_clear_error (&error);
      g_object_unref (self);
      return;
    }

  if (ytdl_command_result_means_too_old (r))
    {
      show_problem (
          self, "This pipeline has no subscriptions",
          "The installed ytdl predates them. Re-run orchid-ochre's setup to "
          "update it; nothing else needs to change here.");
      g_object_unref (self);
      return;
    }

  YtdlSubscriptionList *l = ytdl_subscriptions_parse (r->out, &error);
  if (l == NULL)
    {
      g_autofree char *why = r->exit_code != 0
                                 ? ytdl_command_result_message (r)
                                 : g_strdup (error->message);
      show_problem (self, "The subscription list could not be read", why);
      g_clear_error (&error);
      g_object_unref (self);
      return;
    }
  g_clear_pointer (&self->current, ytdl_subscription_list_free);
  self->current = l;
  fill (self);
  g_object_unref (self);
}

void
ytdl_subscriptions_view_refresh (YtdlSubscriptionsView *self)
{
  g_return_if_fail (YTDL_IS_SUBSCRIPTIONS_VIEW (self));
  if (self->loading)
    return;
  self->loading = TRUE;
  gtk_widget_set_visible (self->spinner, TRUE);
  gtk_spinner_start (GTK_SPINNER (self->spinner));
  gtk_widget_set_sensitive (self->refresh, FALSE);
  g_auto (GStrv) args = ytdl_subscriptions_list_args ();
  ytdl_command_run_async ((const char *const *) args, self->cancel,
                          on_list_done, g_object_ref (self));
}

/* ---------------------------------------------------------------------- */

static void
on_refresh (GtkButton *b, gpointer user_data)
{
  ytdl_subscriptions_view_refresh (user_data);
}

static void
on_switch (GObject *sw, GParamSpec *pspec, gpointer user_data)
{
  YtdlSubscriptionsView *self = user_data;
  if (self->syncing_switch)
    return;
  gboolean on = gtk_switch_get_active (GTK_SWITCH (sw));
  gtk_widget_set_sensitive (GTK_WIDGET (sw), FALSE);
  g_auto (GStrv) args = ytdl_schedule_args (on);
  run_op (self, args,
          on ? "Subscriptions are now checked every hour, whether or not "
               "this app is open."
             : "Hourly checks are off. Subscriptions are kept.");
}

static void
ytdl_subscriptions_view_init (YtdlSubscriptionsView *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);
  self->cancel = g_cancellable_new ();
}

static void
ytdl_subscriptions_view_dispose (GObject *object)
{
  YtdlSubscriptionsView *self = YTDL_SUBSCRIPTIONS_VIEW (object);
  if (self->cancel != NULL)
    g_cancellable_cancel (self->cancel);
  g_clear_object (&self->cancel);
  G_OBJECT_CLASS (ytdl_subscriptions_view_parent_class)->dispose (object);
}

static void
ytdl_subscriptions_view_finalize (GObject *object)
{
  YtdlSubscriptionsView *self = YTDL_SUBSCRIPTIONS_VIEW (object);
  g_clear_pointer (&self->current, ytdl_subscription_list_free);
  G_OBJECT_CLASS (ytdl_subscriptions_view_parent_class)->finalize (object);
}

static void
ytdl_subscriptions_view_class_init (YtdlSubscriptionsViewClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = ytdl_subscriptions_view_dispose;
  G_OBJECT_CLASS (klass)->finalize = ytdl_subscriptions_view_finalize;
  signals[SIG_MESSAGE] =
      g_signal_new ("message", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                    NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static GtkWidget *
heading (const char *text, GtkWidget *trailing)
{
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (l), 0.0f);
  gtk_widget_set_hexpand (l, TRUE);
  gtk_widget_add_css_class (l, "heading");
  gtk_box_append (GTK_BOX (head), l);
  if (trailing != NULL)
    gtk_box_append (GTK_BOX (head), trailing);
  return head;
}

static GtkWidget *
boxed_list (void)
{
  GtkWidget *list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (list, "boxed-list");
  return list;
}

GtkWidget *
ytdl_subscriptions_view_new (YtdlRunner *runner)
{
  YtdlSubscriptionsView *self =
      g_object_new (YTDL_TYPE_SUBSCRIPTIONS_VIEW, NULL);
  self->runner = runner;

  self->content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (self->content, 12);
  gtk_widget_set_margin_end (self->content, 12);
  gtk_widget_set_margin_top (self->content, 18);
  gtk_widget_set_margin_bottom (self->content, 24);

  /* --- Automatic checks ------------------------------------------- */
  gtk_box_append (GTK_BOX (self->content), heading ("Automatic checks", NULL));
  GtkWidget *sched = boxed_list ();
  self->schedule_row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->schedule_row),
                                 "Check every hour");
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (self->schedule_row), 0);
  self->schedule_switch = gtk_switch_new ();
  gtk_widget_set_valign (self->schedule_switch, GTK_ALIGN_CENTER);
  g_signal_connect (self->schedule_switch, "notify::active",
                    G_CALLBACK (on_switch), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self->schedule_row),
                             self->schedule_switch);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (self->schedule_row),
                                         self->schedule_switch);
  gtk_list_box_append (GTK_LIST_BOX (sched), self->schedule_row);
  gtk_box_append (GTK_BOX (self->content), sched);

  GtkWidget *explain = gtk_label_new (
      "The pipeline checks each subscription on its own interval, using this "
      "computer's own scheduler — so checks happen whether or not this app is "
      "open. The app runs no timer of its own.");
  gtk_label_set_wrap (GTK_LABEL (explain), TRUE);
  gtk_label_set_xalign (GTK_LABEL (explain), 0.0f);
  gtk_widget_add_css_class (explain, "caption");
  gtk_widget_add_css_class (explain, "dim-label");
  gtk_widget_set_margin_bottom (explain, 20);
  gtk_box_append (GTK_BOX (self->content), explain);

  /* --- The list ----------------------------------------------------- */
  self->spinner = gtk_spinner_new ();
  gtk_widget_set_visible (self->spinner, FALSE);
  self->refresh = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_widget_add_css_class (self->refresh, "flat");
  gtk_widget_set_tooltip_text (self->refresh, "Read the list again");
  g_signal_connect (self->refresh, "clicked", G_CALLBACK (on_refresh), self);
  GtkWidget *trailing = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (trailing), self->spinner);
  gtk_box_append (GTK_BOX (trailing), self->refresh);
  gtk_box_append (GTK_BOX (self->content), heading ("Subscriptions", trailing));

  self->list = boxed_list ();
  GtkWidget *empty = gtk_label_new (
      "No subscriptions yet. On the Downloads pane, enter a channel or "
      "playlist URL, choose its options, and press Subscribe.");
  gtk_label_set_wrap (GTK_LABEL (empty), TRUE);
  gtk_widget_set_margin_top (empty, 18);
  gtk_widget_set_margin_bottom (empty, 18);
  gtk_widget_set_margin_start (empty, 18);
  gtk_widget_set_margin_end (empty, 18);
  gtk_widget_add_css_class (empty, "dim-label");
  gtk_list_box_set_placeholder (GTK_LIST_BOX (self->list), empty);
  gtk_box_append (GTK_BOX (self->content), self->list);

  self->problem = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->problem),
                                 "dialog-warning-symbolic");
  gtk_widget_set_visible (self->problem, FALSE);
  gtk_widget_set_vexpand (self->problem, TRUE);

  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (column), self->content);
  gtk_box_append (GTK_BOX (column), self->problem);

  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 860);
  adw_clamp_set_tightening_threshold (ADW_CLAMP (clamp), 660);
  adw_clamp_set_child (ADW_CLAMP (clamp), column);

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), clamp);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_box_append (GTK_BOX (self), scroller);

  return GTK_WIDGET (self);
}
