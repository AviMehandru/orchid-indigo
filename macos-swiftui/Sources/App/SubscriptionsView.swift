/* The Subscriptions pane: the pipeline's list of sources and its hourly check,
 * shown and changed through `ytdl`.
 *
 * Nothing on this pane is stored by this app, and nothing here runs on a
 * timer. Every row is read from `ytdl --subscriptions --json` each time the
 * pane is shown, and every button is a `ytdl` command -- so a subscription
 * added from a terminal, from the GTK or WinUI app or from the Tauri one shows
 * up here, and one changed here is changed for all of them. The hourly check
 * is a launchd agent the PIPELINE installs (`ytdl --schedule install`); see
 * Core/Subscriptions.swift for why the timer is not in this app.
 *
 * Subscribing happens on the Downloads pane, not here: its form already holds
 * every option a download can take and shows the command line, and a second
 * form here would be a second place for the two to disagree.
 */

import SwiftUI

/* Owned by AppModel, not by the view: the panes are a switch, so the view is
 * destroyed and rebuilt on every sidebar change, and a list that vanished with
 * it would be read again from scratch -- with a spinner -- every time. */
@MainActor
final class SubscriptionsModel: ObservableObject {
    struct Problem: Equatable {
        var title: String
        var detail: String
    }

    @Published private(set) var list: SubscriptionList?
    @Published private(set) var loading = false
    /// Set when the list cannot be read at all; the pane shows this instead.
    @Published private(set) var problem: Problem?
    /// A --schedule install/remove is in flight; the toggle waits for it.
    @Published private(set) var scheduleBusy = false

    private let runner: Runner
    /// Where a one-line outcome goes. AppModel points it at the status line,
    /// which is this app's toast.
    var onMessage: (String) -> Void = { _ in }

    init(runner: Runner) {
        self.runner = runner
    }

    /// Read the list again. Cheap enough to call every time the pane is
    /// shown -- one pwsh start -- and it is the only way to see what the
    /// schedule did since the last look.
    func refresh() {
        guard !loading else { return }
        loading = true
        YtdlCommand.run(SubscriptionArgs.list) { [weak self] result in
            Task { @MainActor in self?.finishRefresh(result) }
        }
    }

    private func finishRefresh(_ result: Result<YtdlCommandResult, Error>) {
        loading = false
        switch result {
        case .failure(let error):
            problem = Problem(title: "The pipeline could not be run",
                              detail: error.localizedDescription)
        case .success(let r):
            if r.meansTooOld {
                problem = Problem(
                    title: "This pipeline has no subscriptions",
                    detail: "The installed ytdl predates them. Re-run orchid-ochre's setup to "
                        + "update it; nothing else needs to change here.")
                return
            }
            do {
                list = try SubscriptionList.parse(r.stdout)
                problem = nil
            } catch {
                problem = Problem(
                    title: "The subscription list could not be read",
                    detail: r.exitCode != 0 ? r.message : error.localizedDescription)
            }
        }
    }

    /// Run a short command, say how it went, and read the list again.
    /// Always read again: a refused command may still have changed
    /// something, and the list is the authority either way.
    private func run(_ args: [String], success: String? = nil, schedule: Bool = false) {
        if schedule { scheduleBusy = true }
        YtdlCommand.run(args) { [weak self] result in
            Task { @MainActor in
                guard let self else { return }
                if schedule { self.scheduleBusy = false }
                switch result {
                case .failure(let error): self.onMessage(error.localizedDescription)
                case .success(let r) where r.exitCode != 0: self.onMessage(r.message)
                case .success: if let success { self.onMessage(success) }
                }
                self.refresh()
            }
        }
    }

    /* Through the queue, like every other run: sequential with them, its
     * progress on the Downloads pane, cancellable there. */
    func checkNow(_ s: Subscription) {
        do {
            try runner.enqueue(s.runOptions())
            onMessage("Queued a check of \(s.title). Its progress is on the Downloads pane.")
        } catch {
            onMessage(error.localizedDescription)
        }
    }

    func setPaused(_ s: Subscription, _ paused: Bool) {
        run(SubscriptionArgs.edit(id: s.id, pause: paused))
    }

    func setEvery(_ s: Subscription, hours: Int) {
        guard hours != s.everyHours else { return }
        run(SubscriptionArgs.edit(id: s.id, everyHours: hours))
    }

    func unsubscribe(_ s: Subscription) {
        run(SubscriptionArgs.unsubscribe(s.id), success: "Unsubscribed from \(s.title).")
    }

    func setSchedule(on: Bool) {
        run(SubscriptionArgs.schedule(install: on),
            success: on
                ? "Subscriptions are now checked every hour, whether or not this app is open."
                : "Hourly checks are off. Subscriptions are kept.",
            schedule: true)
    }

    /// The Downloads form as a subscription. `completion` gets nil on success
    /// (the pipeline's own confirmation goes to the status line), or the
    /// sentence to show in an alert.
    func subscribe(_ opts: RunOptions, everyHours: Int, name: String,
                   completion: @escaping (String?) -> Void) {
        let args = SubscriptionArgs.subscribe(opts, everyHours: everyHours, name: name)
        YtdlCommand.run(args) { [weak self] result in
            Task { @MainActor in
                switch result {
                case .failure(let error):
                    completion(error.localizedDescription)
                case .success(let r) where r.exitCode != 0:
                    completion(r.meansTooOld
                        ? "The installed pipeline predates subscriptions. Re-run orchid-ochre's "
                            + "setup to update it."
                        : r.message)
                case .success(let r):
                    /* "Subscribed 3f2a9c1e: Name (every 1d)", or "Updated
                     * subscription ..." when the URL was already subscribed.
                     * Which of the two happened is the pipeline's to say. */
                    let first = r.stdout.split(separator: "\n").first.map(String.init) ?? "Subscribed."
                    self?.onMessage(first.trimmingCharacters(in: .whitespaces))
                    completion(nil)
                    self?.refresh()
                }
            }
        }
    }
}

struct SubscriptionsView: View {
    @EnvironmentObject private var subs: SubscriptionsModel
    @State private var confirmRemove: Subscription?

    var body: some View {
        Group {
            if let p = subs.problem {
                ContentUnavailableView {
                    Label(p.title, systemImage: "exclamationmark.triangle")
                } description: {
                    Text(p.detail)
                } actions: {
                    Button("Try again") { subs.refresh() }
                }
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 20) {
                        schedule
                        subscriptionList
                    }
                    .frame(maxWidth: 860, alignment: .leading)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .padding(16)
                }
            }
        }
        .navigationTitle("Subscriptions")
        .onAppear { subs.refresh() }
        .confirmationDialog(
            "Unsubscribe from \(confirmRemove?.title ?? "")?",
            isPresented: Binding(
                get: { confirmRemove != nil },
                set: { if !$0 { confirmRemove = nil } }
            ),
            titleVisibility: .visible,
            presenting: confirmRemove
        ) { s in
            Button("Unsubscribe", role: .destructive) { subs.unsubscribe(s) }
            Button("Cancel", role: .cancel) {}
        } message: { _ in
            Text("It will not be checked again. Nothing it has already archived is deleted.")
        }
    }

    // MARK: - Automatic checks

    private var schedule: some View {
        let sched = subs.list?.schedule
        let now = subs.list?.now ?? Int64(Date().timeIntervalSince1970)
        return VStack(alignment: .leading, spacing: 6) {
            SectionBox(title: "Automatic checks") {
                Toggle(isOn: Binding(
                    get: { sched?.installed ?? false },
                    set: { subs.setSchedule(on: $0) }
                )) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Check every hour")
                        Text(sched?.statusLine(now: now) ?? "Reading…")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .toggleStyle(.switch)
                /* Unsupported: there is no scheduler here to turn on, and a
                 * switch that flips back every time it is pressed is worse
                 * than one that says why it cannot move. */
                .disabled(sched == nil || subs.scheduleBusy
                          || !(sched!.supported || sched!.installed))
            }
            Text("The pipeline checks each subscription on its own interval, using this Mac's "
                 + "own scheduler (a launchd agent) — so checks happen whether or not this app "
                 + "is open. The app runs no timer of its own.")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    // MARK: - The list

    private var subscriptionList: some View {
        SectionBox(title: "Subscriptions", trailing: AnyView(
            HStack(spacing: 8) {
                if subs.loading {
                    ProgressView().controlSize(.small).scaleEffect(0.7)
                }
                Button {
                    subs.refresh()
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .buttonStyle(.borderless)
                .disabled(subs.loading)
                .help("Read the list again")
            }
        )) {
            if let l = subs.list, !l.subscriptions.isEmpty {
                ForEach(Array(l.subscriptions.enumerated()), id: \.element.id) { i, s in
                    if i > 0 { Divider() }
                    row(s, now: l.now)
                }
            } else if subs.list != nil {
                Text("No subscriptions yet. On the Downloads pane, enter a channel or playlist "
                     + "URL, choose its options, and press Subscribe.")
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                Text("Reading…").foregroundStyle(.secondary)
            }
        }
    }

    private func pill(_ s: Subscription) -> Pill {
        guard s.enabled else { return Pill(text: "paused") }
        switch s.lastRun?.result {
        case nil: return Pill(text: "new")
        case "failed": return Pill(text: "failed", variant: .error)
        case "errors": return Pill(text: "errors", variant: .warn)
        default: return Pill(text: "ok", variant: .ok)
        }
    }

    private func tooltip(_ s: Subscription) -> String {
        var t = s.url
        if !s.options.isEmpty { t += "\n" + s.options.joined(separator: " ") }
        t += "\nInto " + (s.dataRoot ?? "the pipeline's default data root")
        return t
    }

    private func row(_ s: Subscription, now: Int64) -> some View {
        HStack(alignment: .center, spacing: 10) {
            // One width for every state, so the titles line up down the list.
            pill(s).frame(width: 64)
            VStack(alignment: .leading, spacing: 2) {
                Text(s.title)
                Text("\(SubscriptionText.every(s.everyHours)) · \(s.statusLine(now: now))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                if s.lastRun?.result == "failed", let m = s.lastRun?.message {
                    Text(m)
                        .font(.caption)
                        .foregroundStyle(.red)
                        .fixedSize(horizontal: false, vertical: true)
                        .textSelection(.enabled)
                }
            }
            .help(tooltip(s))
            Spacer()
            Button("Check now") { subs.checkNow(s) }
                .help("Queue a check of this subscription now, even if it is paused")
            Menu {
                Button(s.enabled ? "Pause" : "Resume") { subs.setPaused(s, s.enabled) }
                Section("Check it") {
                    ForEach(SubscriptionText.intervalChoices, id: \.self) { h in
                        Button {
                            subs.setEvery(s, hours: h)
                        } label: {
                            if h == s.everyHours {
                                Label(SubscriptionText.every(h), systemImage: "checkmark")
                            } else {
                                Text(SubscriptionText.every(h))
                            }
                        }
                    }
                    /* An interval set from a terminal that the menu does not
                     * offer is shown, not silently replaced by the nearest. */
                    if !SubscriptionText.intervalChoices.contains(s.everyHours) {
                        Label(SubscriptionText.every(s.everyHours), systemImage: "checkmark")
                    }
                }
                Divider()
                Button("Unsubscribe…", role: .destructive) { confirmRemove = s }
            } label: {
                Image(systemName: "ellipsis.circle")
            }
            .menuStyle(.button)
            .buttonStyle(.borderless)
            .fixedSize()
            .help("Pause, change how often, or unsubscribe")
        }
        .padding(.vertical, 2)
    }
}
