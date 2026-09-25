/* The Subscriptions page's contents.
 *
 * Nothing here is stored by this app, and nothing runs on a timer. Every row is
 * read from `ytdl --subscriptions --json` each time the page is shown, and
 * every button is a `ytdl` command -- so a subscription added from a terminal,
 * from the GTK or SwiftUI app or from the Tauri one shows up here, and one
 * changed here is changed for all of them.
 *
 * Subscribing happens on the Downloads page, not here: its form already holds
 * every option a download can take and shows the command line, and a second
 * form here would be a second place for the two to disagree.
 */

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using YtdlWin.Core;

namespace YtdlWin.Views;

public sealed partial class SubscriptionsPage : Page
{
    private SubscriptionList? _list;
    private bool _loading;
    private bool _scheduleBusy;
    private (string Title, string Detail)? _problem;

    private AppModel Model => AppModel.Current;

    public SubscriptionsPage()
    {
        InitializeComponent();
    }

    /* Read on every visit: the schedule runs whether or not this app is open,
     * and this is the only way to see what it did since the last look. */
    protected override async void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        await RefreshAsync();
    }

    private async Task RefreshAsync()
    {
        if (_loading) return;
        _loading = true;
        Render();
        try
        {
            /* Off the UI thread entirely, Process.Start included: pwsh takes
             * long enough to start that doing it inline would stall a page
             * switch. */
            var r = await Task.Run(() => YtdlCommand.RunAsync(SubscriptionArgs.List()));
            if (r.MeansTooOld)
            {
                _problem = ("This pipeline has no subscriptions",
                    "The installed ytdl predates them. Re-run orchid-ochre's setup to update it; "
                    + "nothing else needs to change here.");
                return;
            }
            try
            {
                _list = SubscriptionList.Parse(r.StdOut);
                _problem = null;
            }
            catch (FormatException ex)
            {
                _problem = ("The subscription list could not be read",
                            r.ExitCode != 0 ? r.Message : ex.Message);
            }
        }
        catch (Exception ex)
        {
            _problem = ("The pipeline could not be run", ex.Message);
        }
        finally
        {
            _loading = false;
            Render();
        }
    }

    /// Run a short command, say how it went, and read the list again. Always
    /// read again: a refused command may still have changed something, and
    /// the list is the authority either way.
    private async Task RunAsync(List<string> args, string? success = null, bool schedule = false)
    {
        if (schedule) { _scheduleBusy = true; Render(); }
        try
        {
            var r = await Task.Run(() => YtdlCommand.RunAsync(args));
            if (r.ExitCode != 0) Model.Say(r.Message);
            else if (success is not null) Model.Say(success);
        }
        catch (Exception ex)
        {
            Model.Say(ex.Message);
        }
        finally
        {
            if (schedule) _scheduleBusy = false;
        }
        await RefreshAsync();
    }

    // MARK: - Rendering

    private void Render()
    {
        Sections.Children.Clear();
        if (_problem is { } p)
        {
            var retry = new Button { Content = "Try again" };
            retry.Click += async (_, _) => await RefreshAsync();
            Sections.Children.Add(Controls.SectionBox(p.Title, null,
                Secondary(p.Detail), retry));
            return;
        }
        Sections.Children.Add(BuildSchedule());
        Sections.Children.Add(BuildList());
    }

    private UIElement BuildSchedule()
    {
        var sched = _list?.Schedule;
        var now = _list?.Now ?? DateTimeOffset.UtcNow.ToUnixTimeSeconds();

        var grid = new Grid { ColumnSpacing = 12 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var text = new StackPanel { Spacing = 2 };
        text.Children.Add(new TextBlock { Text = "Check every hour" });
        text.Children.Add(Secondary(sched?.StatusLine(now) ?? "Reading…"));
        Grid.SetColumn(text, 0);
        grid.Children.Add(text);

        var toggle = new ToggleSwitch
        {
            IsOn = sched?.Installed ?? false,
            OnContent = "",
            OffContent = "",
            VerticalAlignment = VerticalAlignment.Center,
            /* Unsupported: there is no scheduler here to turn on, and a switch
             * that flips back every time it is pressed is worse than one that
             * says why it cannot move. */
            IsEnabled = sched is not null && !_scheduleBusy && (sched.Supported || sched.Installed),
        };
        /* Wired AFTER IsOn is set, so building the page does not read as the
         * user asking for the opposite of what is installed. */
        toggle.Toggled += async (_, _) =>
        {
            var on = toggle.IsOn;
            await RunAsync(SubscriptionArgs.Schedule(on),
                on ? "Subscriptions are now checked every hour while you are signed in, "
                     + "whether or not this app is open."
                   : "Hourly checks are off. Subscriptions are kept.",
                schedule: true);
        };
        Grid.SetColumn(toggle, 1);
        grid.Children.Add(toggle);

        var box = Controls.SectionBox("Automatic checks", null, grid);
        box.Children.Add(Secondary(
            "The pipeline checks each subscription on its own interval, using this computer's "
            + "own Task Scheduler — so checks happen whether or not this app is open. The app "
            + "runs no timer of its own."));
        return box;
    }

    private UIElement BuildList()
    {
        var trailing = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        if (_loading)
        {
            trailing.Children.Add(new ProgressRing
            {
                Width = 16, Height = 16, IsActive = true,
                VerticalAlignment = VerticalAlignment.Center,
            });
        }
        var refresh = new Button { Content = new SymbolIcon(Symbol.Refresh), IsEnabled = !_loading };
        ToolTipService.SetToolTip(refresh, "Read the list again");
        refresh.Click += async (_, _) => await RefreshAsync();
        trailing.Children.Add(refresh);

        UIElement[] rows;
        if (_list is null)
        {
            rows = new[] { Secondary("Reading…") };
        }
        else if (_list.Subscriptions.Count == 0)
        {
            rows = new[] { Secondary(
                "No subscriptions yet. On the Downloads page, enter a channel or playlist URL, "
                + "choose its options, and press Subscribe.") };
        }
        else
        {
            var now = _list.Now;
            rows = _list.Subscriptions.Select(s => Row(s, now)).ToArray();
        }
        return Controls.SectionBox("Subscriptions", trailing, rows);
    }

    private static Border Pill(Subscription s)
    {
        if (!s.Enabled) return Controls.Pill("paused");
        return s.LastRun?.Result switch
        {
            null => Controls.Pill("new"),
            "failed" => Controls.Pill("failed", PillVariant.Error),
            "errors" => Controls.Pill("errors", PillVariant.Warn),
            _ => Controls.Pill("ok", PillVariant.Ok),
        };
    }

    private UIElement Row(Subscription s, long now)
    {
        var grid = new Grid { ColumnSpacing = 10, Margin = new Thickness(0, 4, 0, 4) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(64) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        // One width for every state, so the titles line up down the list.
        var pill = Pill(s);
        pill.HorizontalAlignment = HorizontalAlignment.Left;
        Grid.SetColumn(pill, 0);
        grid.Children.Add(pill);

        var text = new StackPanel { Spacing = 0 };
        text.Children.Add(new TextBlock { Text = s.Title, TextTrimming = TextTrimming.CharacterEllipsis });
        text.Children.Add(Secondary($"{SubscriptionText.Every(s.EveryHours)} · {s.StatusLine(now)}"));
        if (s.LastRun is { Result: "failed", Message: { } message })
        {
            text.Children.Add(new TextBlock
            {
                Text = message,
                FontSize = 12,
                TextWrapping = TextWrapping.Wrap,
                IsTextSelectionEnabled = true,
                Foreground = Controls.Resource<Brush>("SystemFillColorCriticalBrush"),
            });
        }
        /* Everything else about it on hover: the URL when a name is shown, the
         * stored options (the pipeline has already masked a proxy password),
         * and where it archives to. */
        var tip = s.Url;
        if (s.Options.Count > 0) tip += "\n" + string.Join(" ", s.Options);
        tip += "\nInto " + (s.DataRoot ?? "the pipeline's default data root");
        ToolTipService.SetToolTip(text, tip);
        Grid.SetColumn(text, 1);
        grid.Children.Add(text);

        var check = new Button { Content = "Check now", VerticalAlignment = VerticalAlignment.Center };
        ToolTipService.SetToolTip(check, "Queue a check of this subscription now, even if it is paused");
        check.Click += (_, _) => CheckNow(s);
        Grid.SetColumn(check, 2);
        grid.Children.Add(check);

        var more = new Button
        {
            Content = new SymbolIcon(Symbol.More),
            VerticalAlignment = VerticalAlignment.Center,
            Flyout = RowMenu(s),
        };
        ToolTipService.SetToolTip(more, "Pause, change how often, or unsubscribe");
        Grid.SetColumn(more, 3);
        grid.Children.Add(more);
        return grid;
    }

    private MenuFlyout RowMenu(Subscription s)
    {
        var menu = new MenuFlyout();

        var pause = new MenuFlyoutItem { Text = s.Enabled ? "Pause" : "Resume" };
        pause.Click += async (_, _) => await RunAsync(SubscriptionArgs.Edit(s.Id, pause: s.Enabled));
        menu.Items.Add(pause);

        var every = new MenuFlyoutSubItem { Text = "Check it" };
        var choices = SubscriptionText.IntervalChoices.ToList();
        /* An interval set from a terminal that the menu does not offer is
         * shown, not silently replaced by the nearest one. */
        if (!choices.Contains(s.EveryHours)) choices.Add(s.EveryHours);
        foreach (var h in choices.OrderBy(h => h))
        {
            var item = new ToggleMenuFlyoutItem
            {
                Text = SubscriptionText.Every(h),
                IsChecked = h == s.EveryHours,
            };
            var hours = h;
            item.Click += async (_, _) =>
            {
                if (hours == s.EveryHours) { item.IsChecked = true; return; }
                await RunAsync(SubscriptionArgs.Edit(s.Id, hours));
            };
            every.Items.Add(item);
        }
        menu.Items.Add(every);

        menu.Items.Add(new MenuFlyoutSeparator());
        var remove = new MenuFlyoutItem { Text = "Unsubscribe…" };
        remove.Click += async (_, _) => await ConfirmUnsubscribe(s);
        menu.Items.Add(remove);
        return menu;
    }

    // MARK: - Actions

    /* Through the queue, like every other run: sequential with them, its
     * progress on the Downloads page, cancellable there. */
    private void CheckNow(Subscription s)
    {
        try
        {
            Model.Runner.Enqueue(s.RunOptions());
            Model.Say($"Queued a check of {s.Title}. Its progress is on the Downloads page.");
        }
        catch (Exception ex)
        {
            Model.Say(ex.Message);
        }
    }

    private async Task ConfirmUnsubscribe(Subscription s)
    {
        var dialog = new ContentDialog
        {
            Title = $"Unsubscribe from {s.Title}?",
            Content = new TextBlock
            {
                Text = "It will not be checked again. Nothing it has already archived is deleted.",
                TextWrapping = TextWrapping.Wrap,
            },
            PrimaryButtonText = "Unsubscribe",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
            XamlRoot = XamlRoot,
        };
        ContentDialogResult result;
        try { result = await dialog.ShowAsync(); }
        catch (Exception) { return; /* another dialog is already up */ }
        if (result != ContentDialogResult.Primary) return;
        await RunAsync(SubscriptionArgs.Unsubscribe(s.Id), $"Unsubscribed from {s.Title}.");
    }

    // MARK: - Small pieces

    private static TextBlock Secondary(string text) => new()
    {
        Text = text,
        FontSize = 12,
        TextWrapping = TextWrapping.Wrap,
        Foreground = Controls.Resource<Brush>("TextFillColorSecondaryBrush"),
    };
}
