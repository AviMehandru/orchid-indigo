/* The Downloads pane: build a `ytdl` command line, run it, watch it.
 *
 * The command preview above the button is not decoration. A GUI that hides the
 * command it runs makes the CLI harder to learn rather than easier, and every
 * problem report about this app is easier to answer when the user can paste the
 * exact line the window would have run.
 *
 * The form is a grouped Form, which is AppKit's answer to the boxed
 * AdwPreferencesGroup lists the GTK app uses: the label, the control, the row
 * height and the separators all come from the platform, and every option can
 * carry a one-line explanation underneath instead of a tooltip nobody hovers
 * over.
 *
 * The split between the form and the run area is draggable because the two
 * halves are wanted in different proportions at different times: all form while
 * setting a run up, all log while watching one. A fixed ratio would be wrong in
 * both states.
 */

import AppKit
import SwiftUI
import UniformTypeIdentifiers

/* The form's state, and why it does not live in the view.
 *
 * The three panes are a `switch` in ContentView, so SwiftUI destroys and
 * rebuilds this view every time the sidebar selection changes -- taking @State
 * with it. A half-typed URL and every un-saved option would be gone after a
 * trip to the Library and back, silently. The GTK app keeps one
 * YtdlDownloadsView alive inside its AdwViewStack for exactly this reason; an
 * object owned by AppModel is the same guarantee. */
@MainActor
final class DownloadsModel: ObservableObject {
    @Published var opts = RunOptions()
    /// One --ytdlp-arg per line, because a real --match-filter expression
    /// contains commas and spaces and there is no separator that would be safe
    /// to split a single-line field on.
    @Published var extraArgsText = ""
    @Published var selectedProfile: String?
    @Published var status = ""
    @Published var statusIsError = false

    init(settings: Settings, store: ProfileStore) {
        opts.dataRoot = settings.dataRoot
        opts.workers = settings.defaultWorkers
        opts.mode = "full"
        opts.quality = "best"
        opts.codec = "any"
        opts.audioCodec = "any"
        opts.container = "mkv"

        selectedProfile = store.active
        if let name = store.active, let p = store.profile(named: name) {
            apply(p.opts)
        }
    }

    /* Applies a profile's options and LEAVES THE URL ALONE. Fields with no
     * control of their own -- --items, --after, --pot-port, --skip-pot-update --
     * are applied too rather than dropped: they are part of the option set
     * somebody saved, the command preview shows every flag that will run, and
     * silently discarding stored settings is the worse of the two failures. */
    func apply(_ p: RunOptions) {
        opts.mode = p.mode.isEmpty ? "full" : p.mode
        opts.quality = p.quality.isEmpty ? "best" : p.quality
        opts.codec = p.codec.isEmpty ? "any" : p.codec
        opts.audioCodec = p.audioCodec.isEmpty ? "any" : p.audioCodec
        opts.container = p.container.isEmpty ? "mkv" : p.container
        opts.workers = p.workers > 0 ? p.workers : 1
        opts.sync = p.sync
        opts.lazy = p.lazy
        opts.noPot = p.noPot
        opts.skipPotUpdate = p.skipPotUpdate
        opts.potPort = p.potPort
        opts.items = p.items
        opts.after = p.after
        opts.noComments = p.noComments
        opts.noSubs = p.noSubs
        opts.noThumbnail = p.noThumbnail
        opts.noMetadata = p.noMetadata

        /* The destination is part of the profile, but an empty one must not wipe
         * a destination the user has set for this session. */
        if !p.dataRoot.isEmpty { opts.dataRoot = p.dataRoot }

        extraArgsText = p.ytdlpArgs.joined(separator: "\n")
        opts.ytdlpArgs = p.ytdlpArgs
    }

    func setStatus(_ text: String, isError: Bool) {
        status = text
        statusIsError = isError
    }
}

struct DownloadsView: View {
    @EnvironmentObject private var runner: Runner
    @EnvironmentObject private var settings: Settings
    @EnvironmentObject private var form: DownloadsModel
    @EnvironmentObject private var store: ProfileStore

    @State private var nameSheet: NameSheet?
    @State private var errorMessage: String?

    private struct NameSheet: Identifiable {
        let renaming: Bool
        let initial: String
        var id: String { (renaming ? "rename:" : "save:") + initial }
    }

    var body: some View {
        VSplitView {
            optionsForm
                .frame(minHeight: 220, idealHeight: 470)
            runArea
                .frame(minHeight: 200)
        }
        .navigationTitle("Downloads")
        .sheet(item: $nameSheet) { sheet in
            ProfileNameSheet(
                renaming: sheet.renaming,
                initial: sheet.initial,
                onCommit: { commitName($0, renaming: sheet.renaming) }
            )
        }
        .alert("Cannot do that", isPresented: Binding(
            get: { errorMessage != nil },
            set: { if !$0 { errorMessage = nil } }
        ), actions: {
            Button("OK", role: .cancel) { errorMessage = nil }
        }, message: {
            Text(errorMessage ?? "")
        })
    }

    // MARK: - The form

    /* Not named `form`: that is the DownloadsModel this view reads its state
     * from, and a computed property cannot share a name with a stored one. */
    private var optionsForm: some View {
        Form {
            profileSection
            downloadSection
            formatSection
            passesSection
            advancedSection
        }
        .formStyle(.grouped)
    }

    private var profileSection: some View {
        Section {
            Picker("Active profile", selection: Binding(
                get: { form.selectedProfile },
                set: { selectProfile($0) }
            )) {
                /* "(no profile)" is a real state, not a placeholder: it is what
                 * the window is in before anything has been saved. */
                Text("(no profile)").tag(String?.none)
                ForEach(store.profiles) { profile in
                    Text(profile.name).tag(String?.some(profile.name))
                }
            }
            Text("Applies its options and leaves the URL alone — a preset that replaced what "
                 + "you were about to download would be the one thing a preset must never do.")
                .font(.caption)
                .foregroundStyle(.secondary)

            if !form.status.isEmpty {
                Text(form.status)
                    .font(.caption)
                    .foregroundStyle(form.statusIsError ? Color.red : Color.secondary)
            }
        } header: {
            HStack {
                Text("Profile")
                Spacer()
                Button {
                    nameSheet = NameSheet(renaming: false, initial: form.selectedProfile ?? "")
                } label: {
                    Image(systemName: "square.and.arrow.down")
                }
                .help("Save these options as a profile — every option except the URL.")

                Button {
                    guard let name = form.selectedProfile else {
                        form.setStatus("Select a profile to rename.", isError: true)
                        return
                    }
                    nameSheet = NameSheet(renaming: true, initial: name)
                } label: {
                    Image(systemName: "pencil")
                }
                .help("Rename the selected profile")

                Button {
                    deleteProfile()
                } label: {
                    Image(systemName: "trash")
                }
                .help("Delete the selected profile")
            }
        }
    }

    private var downloadSection: some View {
        Section("Download") {
            TextField("Video, playlist or channel URL", text: $form.opts.url)
                .textFieldStyle(.roundedBorder)

            HStack {
                TextField(
                    "Destination — empty means the pipeline's own default",
                    text: $form.opts.dataRoot
                )
                .textFieldStyle(.roundedBorder)
                /* Kept in memory as it is typed and written on commit, not on
                 * every keystroke: a destination path is thirty characters and
                 * thirty writes of settings.json is thirty writes too many. */
                .onChange(of: form.opts.dataRoot) { newValue in
                    settings.dataRoot = newValue
                }
                .onSubmit { settings.save() }

                /* The platform's own folder chooser -- with its sidebar, its
                 * recents and its keyboard navigation. This is one of the things
                 * going native actually buys: the webview build had to route a
                 * folder pick through a plugin. */
                Button("Choose…") { chooseFolder() }
            }

            Button {
                addToQueue()
            } label: {
                Label("Add to queue", systemImage: "plus.circle.fill")
            }
            .keyboardShortcut(.return, modifiers: .command)
        }
    }

    private var formatSection: some View {
        Section("Format") {
            Picker("Mode", selection: $form.opts.mode) {
                Text("Everything").tag("full")
                Text("Video only").tag("video-only")
                Text("Audio only").tag("audio-only")
                Text("Metadata only").tag("metadata-only")
                Text("Comments only").tag("comments-only")
                Text("Subtitles only").tag("subs-only")
            }
            Text("Which parts of each video to fetch.")
                .font(.caption)
                .foregroundStyle(.secondary)

            Picker("Quality", selection: $form.opts.quality) {
                Text("Best").tag("best")
                ForEach(["2160", "1440", "1080", "720", "480", "360"], id: \.self) { h in
                    Text("\(h)p").tag(h)
                }
            }
            Text("A ceiling, not a demand — a video that was never published at this height "
                 + "comes down at the best it has.")
                .font(.caption)
                .foregroundStyle(.secondary)

            Picker("Video codec", selection: $form.opts.codec) {
                Text("Any").tag("any")
                Text("AVC1 / H.264").tag("avc1")
                Text("VP9").tag("vp9")
                Text("AV1").tag("av01")
            }
            Picker("Audio codec", selection: $form.opts.audioCodec) {
                Text("Any").tag("any")
                Text("Opus").tag("opus")
                Text("AAC").tag("aac")
                Text("MP3").tag("mp3")
                Text("FLAC").tag("flac")
            }
            Picker("Container", selection: $form.opts.container) {
                Text("MKV").tag("mkv")
                Text("MP4").tag("mp4")
                Text("WebM").tag("webm")
            }
            Text("MKV keeps everything the pipeline embeds. MP4 is the one AVFoundation can "
                 + "play in this window.")
                .font(.caption)
                .foregroundStyle(.secondary)

            Stepper(
                "Workers: \(max(1, form.opts.workers))",
                value: Binding(
                    get: { max(1, form.opts.workers) },
                    set: { form.opts.workers = $0; settings.defaultWorkers = $0; settings.save() }
                ),
                in: 1...16
            )
            Text("The pipeline's own parallelism. The queue here is always sequential, because "
                 + "independent ytdl invocations race on the shared manifests — --workers is "
                 + "the supported way to run several at once.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    /* Every switch carries its own explanation, the way the GTK rows do. These
     * were tooltips once, and a tooltip nobody hovers over is not documentation
     * -- the reason to turn a pass off is the thing somebody needs to read at
     * the moment they are deciding. */
    private var passesSection: some View {
        Section {
            passToggle("Sync", isOn: $form.opts.sync,
                       note: "Walk the whole channel, stopping at the first video already "
                           + "archived.")
            passToggle("Lazy", isOn: $form.opts.lazy,
                       note: "Skip the up-front enumeration.")
            passToggle("Skip PO token", isOn: $form.opts.noPot,
                       note: "Do not run the PO token provider for this run.")
            passToggle("Skip comments", isOn: $form.opts.noComments,
                       note: "Do not run the comments pass.")
            passToggle("Skip subtitles", isOn: $form.opts.noSubs,
                       note: "Do not capture subtitles.")
            passToggle("Skip thumbnail", isOn: $form.opts.noThumbnail,
                       note: "Do not capture the thumbnail.")
            passToggle("Skip metadata", isOn: $form.opts.noMetadata,
                       note: "Do not run the metadata pass.")
        } header: {
            Text("Passes")
        } footer: {
            Text("Everything is on by default. Turning a pass off makes a run faster and the "
                 + "archive less complete.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private func passToggle(
        _ title: String, isOn: Binding<Bool>, note: String
    ) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Toggle(title, isOn: isOn)
            Text(note)
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var advancedSection: some View {
        Section("Advanced") {
            VStack(alignment: .leading, spacing: 4) {
                Text("Extra yt-dlp arguments, one per line")
                TextEditor(text: $form.extraArgsText)
                    .font(.system(.body, design: .monospaced))
                    .frame(height: 60)
                    .overlay(
                        RoundedRectangle(cornerRadius: 5).strokeBorder(.separator)
                    )
                    .onChange(of: form.extraArgsText) { text in
                        form.opts.ytdlpArgs = text
                            .components(separatedBy: "\n")
                            .map { $0.trimmingCharacters(in: .whitespaces) }
                            .filter { !$0.isEmpty }
                    }
            }

            VStack(alignment: .leading, spacing: 4) {
                Text("This is the command that will run")
                Text(previewCommand)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    /* With no URL typed the preview would read ytdl "" -- which looks like a bug
     * rather than an empty field, and is what it showed right after a queue add
     * cleared the box. A placeholder keeps the rest of the command visible, so
     * the options you have set are still readable while you paste a URL. */
    private var previewCommand: String {
        var shown = form.opts
        if shown.url.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            shown.url = "<URL>"
        }
        return shown.commandPreview()
    }

    // MARK: - The run area

    private var runArea: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Button(role: .destructive) {
                    if !runner.cancel() { errorMessage = "Nothing is running." }
                } label: {
                    Label("Cancel run", systemImage: "stop.circle")
                }
                .disabled(!runner.isRunning)
                .help("Kills the whole process tree — ytdl.ps1, the child pwsh, yt-dlp, "
                      + "postprocess.ps1 and ffmpeg. Killing only the top process would leave "
                      + "a download running with nothing reading its output.")

                Button {
                    runner.setPaused(!runner.paused)
                } label: {
                    Label(runner.paused ? "Resume queue" : "Pause queue",
                          systemImage: runner.paused ? "play.circle" : "pause.circle")
                }

                Text(stageLine)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                Spacer()
            }

            if runner.progress.percent >= 0 {
                ProgressView(value: min(max(runner.progress.percent / 100, 0), 1))
            } else {
                ProgressView(value: 0)
                    .opacity(runner.isRunning ? 1 : 0.35)
            }

            RunLogView()
                .frame(minHeight: 110)

            HStack(alignment: .top, spacing: 12) {
                queueList
                historyList
            }
            .frame(minHeight: 120)
        }
        .padding(12)
    }

    private var stageLine: String {
        guard runner.isRunning else { return "Idle" }
        let p = runner.progress
        var parts = [p.stage ?? "running"]
        if let id = p.videoID { parts.append(id) }
        if let speed = p.speed { parts.append(speed) }
        if let eta = p.eta { parts.append("ETA \(eta)") }
        if let total = p.total { parts.append("of \(total)") }
        return parts.joined(separator: " · ")
    }

    private var queueList: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("Queue (\(runner.queue.count))").font(.headline)
            if runner.queue.isEmpty {
                Text("Nothing queued. Paste a URL above and press Add to queue.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                List(runner.queue) { record in
                    HStack {
                        Text(record.opts.url)
                            .lineLimit(1)
                            .truncationMode(.middle)
                        Spacer()
                        Button {
                            runner.removeQueued(id: record.id)
                        } label: {
                            Image(systemName: "minus.circle")
                        }
                        .buttonStyle(.borderless)
                        .help("Remove from the queue")
                    }
                }
                .listStyle(.bordered)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var historyList: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("History (\(runner.history.count))").font(.headline)
                Spacer()
                if !runner.history.isEmpty {
                    Button("Clear") { runner.clearHistory() }
                        .buttonStyle(.borderless)
                        .font(.caption)
                }
            }
            if runner.history.isEmpty {
                Text("No runs yet.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                List(runner.history) { record in
                    HistoryRow(record: record)
                }
                .listStyle(.bordered)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    // MARK: - Actions

    private func addToQueue() {
        do {
            try runner.enqueue(form.opts)
            /* The URL is cleared; the options are not. Queueing five videos with
             * the same settings is the common case, and re-picking them each
             * time would be the wrong kind of tidy. */
            form.opts.url = ""
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Choose"
        panel.message = "Choose a destination folder"
        let current = Paths.expandTilde(form.opts.dataRoot)
        if Paths.isDirectory(current) {
            panel.directoryURL = URL(fileURLWithPath: current)
        }
        guard panel.runModal() == .OK, let url = panel.url else { return }

        form.opts.dataRoot = url.path
        settings.dataRoot = url.path
        settings.save()
    }

    // MARK: - Profiles

    private func selectProfile(_ name: String?) {
        form.selectedProfile = name
        do {
            if let name, let p = store.profile(named: name) {
                form.apply(p.opts)
                try store.activate(name)
            } else {
                /* Clearing the selection does NOT reset the form. The options
                 * stay exactly as they are; you have simply stopped calling them
                 * a profile. */
                try store.activate(nil)
            }
            form.setStatus("", isError: false)
        } catch {
            form.setStatus(error.localizedDescription, isError: true)
        }
    }

    private func commitName(_ typed: String, renaming: Bool) {
        do {
            if renaming {
                guard let from = form.selectedProfile else { return }
                try store.rename(from: from, to: typed)
                form.selectedProfile = store.active
                form.setStatus("Renamed to “\(typed)”.", isError: false)
            } else {
                try store.save(name: typed, opts: form.opts)
                form.selectedProfile = store.active
                form.setStatus("Saved “\(typed)”.", isError: false)
            }
        } catch {
            form.setStatus(error.localizedDescription, isError: true)
        }
    }

    private func deleteProfile() {
        guard let name = form.selectedProfile else {
            form.setStatus("Select a profile to delete.", isError: true)
            return
        }
        do {
            try store.delete(name: name)
            form.selectedProfile = nil
            form.setStatus("Deleted “\(name)”.", isError: false)
        } catch {
            form.setStatus(error.localizedDescription, isError: true)
        }
    }

}

// MARK: - Pieces

private struct HistoryRow: View {
    let record: RunRecord

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 6) {
                Pill(text: record.state, variant: variant)
                Text(record.command)
                    .font(.caption)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Text(subtitle)
                .font(.caption2)
                .foregroundStyle(.secondary)
                .lineLimit(2)
        }
    }

    private var variant: Pill.Variant {
        switch record.state {
        case "done": return .ok
        case "failed": return .error
        default: return .warn
        }
    }

    /* The four counts exist only in the pipeline's own session summary line,
     * which is why they are parsed out of it as the run goes. A run that was
     * cancelled or died early never printed one, and shows nothing rather than
     * four zeroes that would read as "it ran and found nothing". */
    private var subtitle: String {
        var parts = [Format.when(record.started)]
        if record.videosTouched >= 0 {
            parts.append("\(record.videosTouched) touched · \(record.archiveSkipped) skipped "
                         + "· \(record.errors) errors · \(record.warnings) warnings")
        }
        if !record.lastLine.isEmpty { parts.append(record.lastLine) }
        return parts.filter { !$0.isEmpty }.joined(separator: " · ")
    }
}

/* The live log.
 *
 * A list of lines rather than one big string: appending to a String in a Text
 * view re-lays out the whole document on every redraw, and a --sync of a large
 * channel produces thousands of lines. The transient line is kept separate and
 * drawn last, which is what makes a \r redraw replace its predecessor instead of
 * adding a row. */
private struct RunLogView: View {
    @EnvironmentObject private var runner: Runner

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 1) {
                    if runner.logLines.isEmpty && runner.transientLine == nil {
                        Text("Output from ytdl.ps1 appears here once a run starts.")
                            .font(.system(.caption, design: .monospaced))
                            .foregroundStyle(.secondary)
                    }
                    ForEach(Array(runner.logLines.enumerated()), id: \.offset) { _, line in
                        Text(line)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    if let transient = runner.transientLine {
                        Text(transient)
                            .font(.system(.caption, design: .monospaced))
                            .foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .id("tail")
                    } else {
                        Color.clear.frame(height: 1).id("tail")
                    }
                }
                .padding(8)
            }
            .background(.quaternary.opacity(0.35), in: RoundedRectangle(cornerRadius: 6))
            .onChange(of: runner.logLines.count) { _ in
                proxy.scrollTo("tail", anchor: .bottom)
            }
        }
    }
}

/// Naming a profile. A sheet rather than an inline row, because it is a
/// question with two answers and Escape has to mean one of them.
private struct ProfileNameSheet: View {
    let renaming: Bool
    let initial: String
    let onCommit: (String) -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var name = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(renaming ? "Rename profile" : "Save these options as a profile")
                .font(.headline)
            Text(renaming
                 ? "The options stay as they are; only the name changes."
                 : "Every option except the URL is saved under this name.")
                .font(.caption)
                .foregroundStyle(.secondary)

            TextField("Name", text: $name)
                .textFieldStyle(.roundedBorder)
                .onSubmit(commit)

            HStack {
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button(renaming ? "Rename" : "Save", action: commit)
                    .keyboardShortcut(.defaultAction)
                    .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
        .padding(16)
        .frame(width: 380)
        .onAppear { name = initial }
    }

    private func commit() {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return }
        onCommit(trimmed)
        dismiss()
    }
}
