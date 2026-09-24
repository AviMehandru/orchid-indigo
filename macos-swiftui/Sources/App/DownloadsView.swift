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
    /* SponsorBlock is one choice -- off, mark, or cut -- plus one category
     * list, and the two RunOptions fields it becomes are derived from those in
     * `runOptions`. Kept apart so that switching the mode off and on again
     * does not make anybody retype a list. */
    @Published var sponsorMode = "off"
    @Published var sponsorCats = "sponsor"
    @Published var status = ""
    @Published var statusIsError = false

    /* The URL preview.
     *
     * The form used to know nothing about the URL in it until the run failed:
     * the Quality picker was a fixed ladder, asking for 1440p AV1 was a
     * request that silently resolved to something else, and a typo'd URL was
     * discovered by a failed row in the history list. Everything below exists
     * to answer the question before Add to queue rather than after. */
    @Published var probe: UrlProbe?
    @Published var probeStatus = ""
    @Published var probeStatusIsError = false
    @Published var probeRunning = false
    /// The ticked playlist positions. 1-based, because that is what
    /// --playlist-items counts.
    @Published var selectedItems: Set<Int> = []

    /// Non-nil only while a probe is in flight.
    private var cancellation: UrlProbeCancellation?

    /// Set while `opts.items` is being rewritten from the tick boxes, so the
    /// change handler does not immediately re-derive the boxes from the text
    /// it just wrote.
    private var writingItems = false

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
        opts.fps = p.fps
        opts.subLangs = p.subLangs
        opts.noChapters = p.noChapters
        /* A profile written before these existed has neither list, which
         * reads as SponsorBlock off -- the behaviour it always had. The
         * category field keeps whatever it held. */
        if !p.sponsorblockRemove.isEmpty {
            sponsorMode = "remove"
            sponsorCats = p.sponsorblockRemove
        } else if !p.sponsorblockMark.isEmpty {
            sponsorMode = "mark"
            sponsorCats = p.sponsorblockMark
        } else {
            sponsorMode = "off"
        }

        /* The destination is part of the profile, but an empty one must not wipe
         * a destination the user has set for this session. */
        if !p.dataRoot.isEmpty { opts.dataRoot = p.dataRoot }

        extraArgsText = p.ytdlpArgs.joined(separator: "\n")
        opts.ytdlpArgs = p.ytdlpArgs
    }

    /* What the form MEANS, as opposed to what its controls hold: the
     * SponsorBlock choice folded into its two fields, and whatever the form is
     * showing greyed out dropped (RunOptions.dropInapplicable). This is what
     * is queued, previewed and saved as a profile. The connection is not here
     * -- the Runner stamps it on at enqueue. */
    var runOptions: RunOptions {
        var o = opts
        var cats = sponsorCats.trimmingCharacters(in: .whitespacesAndNewlines)
        /* An empty list with a mode chosen is sent as "sponsor" rather than
         * dropped: the picker says SponsorBlock is on, and a run that quietly
         * did nothing about it would contradict the form. */
        if cats.isEmpty { cats = "sponsor" }
        o.sponsorblockMark = sponsorMode == "mark" ? cats : ""
        o.sponsorblockRemove = sponsorMode == "remove" ? cats : ""
        o.dropInapplicable()
        return o
    }

    /// Mirrors RunOptions.dropInapplicable rule for rule: the one decides what
    /// is greyed out, the other what is sent.
    var mediaOptionsApply: Bool {
        !["metadata-only", "comments-only", "subs-only"].contains(opts.mode)
    }

    func setStatus(_ text: String, isError: Bool) {
        status = text
        statusIsError = isError
    }

    // MARK: - The preview

    /* Everything the preview put on screen goes away, and every picker goes
     * back to its full static list.
     *
     * Called when the URL changes, which is the important case: a Quality
     * picker still showing the heights of the PREVIOUS video, against a URL
     * that has been replaced, is worse than one showing the generic ladder --
     * it looks like knowledge and is not. */
    func clearProbe() {
        cancellation?.cancel()
        cancellation = nil
        probe = nil
        probeRunning = false
        probeStatus = ""
        probeStatusIsError = false
        selectedItems = []
    }

    /// `connection` is `settings.connection()` -- cookies and proxy change
    /// what yt-dlp can see, so the preview has to use them too.
    func startProbe(connection: RunOptions) {
        /* A second press while one is running cancels it rather than starting
         * a race between two answers for the same form. */
        if probeRunning {
            cancellation?.cancel()
            cancellation = nil
            probeRunning = false
            probeStatus = ""
            return
        }
        guard !opts.url.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            probeStatus = "Paste a URL first."
            probeStatusIsError = true
            return
        }

        let token = UrlProbeCancellation()
        cancellation = token
        probeRunning = true
        probeStatus = "Reading the URL…"
        probeStatusIsError = false

        let request = UrlProbeRunner.Request(
            url: opts.url,
            items: opts.items,
            noPot: opts.noPot,
            potPort: opts.potPort,
            /* The same list the run would send, so a URL that needs
             * --cookies-from-browser is probed with it too -- a preview that
             * fails where the download would succeed is worse than none. */
            extraArgs: opts.ytdlpArgs,
            /* Cookies and proxy only: `ytdl --probe` refuses the speed limit
             * and the downloader, which govern moving media bytes. */
            connectionArgs: connection.connectionArgs(forProbe: true)
        )

        UrlProbeRunner.run(request, cancellation: token) { [weak self] result in
            /* The hop is here rather than inside the runner: everything below
             * touches this @MainActor model's state, and a plain
             * DispatchQueue.main.async closure is nonisolated no matter which
             * queue it happens to run on. */
            Task { @MainActor in
                guard let self = self else { return }
                /* A probe whose token has been replaced belongs to a URL the
                 * user has since changed. Its answer describes a different
                 * video, so it is dropped rather than written into the form. */
                guard self.cancellation === token else { return }
                self.cancellation = nil
                self.probeRunning = false

                switch result {
                case .failure(let error):
                    if let e = error as? UrlProbeRunner.RunError,
                       case .cancelled = e {
                        self.probeStatus = ""
                        self.probeStatusIsError = false
                        return
                    }
                    /* The message is yt-dlp's or ytdl.ps1's own sentence --
                     * "Video unavailable", "Sign in to confirm your age" --
                     * not one invented here, because theirs says what to do
                     * about it. */
                    self.probeStatus = error.localizedDescription
                    self.probeStatusIsError = true
                case .success(let p):
                    self.apply(probe: p)
                }
            }
        }
    }

    private func apply(probe p: UrlProbe) {
        probe = p
        /* Said, not guessed around. The contract's rule is that fields are
         * added and never redefined, so a newer document is still readable --
         * but the one thing this app must not do is present a partial reading
         * of it as a complete one. */
        if p.probeVersion > UrlProbe.supportedVersion {
            probeStatus = "This pipeline's probe is newer than this app knows "
                + "about; some of what it reported is not shown."
            probeStatusIsError = false
        } else {
            probeStatus = ""
            probeStatusIsError = false
        }

        clampSelectionsToProbe()
        syncSelectedItemsFromText()
    }

    /* A selection the probed URL does not offer cannot stay: leaving 1440p
     * chosen for a video that turned out to be 1080p at best reads as
     * honoured and is not. The view says so in its note row. */
    private func clampSelectionsToProbe() {
        guard let p = probe else { return }
        if !p.containers.isEmpty, !p.containers.contains(opts.container) {
            opts.container = p.containers[0]
        }
        if opts.codec != "any", !p.videoCodecs.contains(opts.codec) {
            opts.codec = "any"
        }
        if opts.audioCodec != "any", !p.audioCodecs.contains(opts.audioCodec) {
            opts.audioCodec = "any"
        }
        if opts.quality != "best" {
            let available = p.heights(forCodec: opts.codec).map(String.init)
            if !available.contains(opts.quality) { opts.quality = "best" }
        }
    }

    /// Turn the ticked rows into an --items value.
    ///
    /// The empty string is a meaningful answer and not a failure to produce
    /// one: ytdl with no --items takes the whole listing, which is what
    /// "everything is ticked" means. Writing out "1-200" instead would
    /// silently CAP a 4,000-video channel at the entries this window happened
    /// to enumerate -- the run would succeed and quietly archive a twentieth
    /// of what was asked for. So a full selection only collapses to "" when
    /// the list is known to be complete.
    func writeItemsFromSelection() {
        guard let p = probe, !p.entries.isEmpty else { return }
        let all = p.entries.allSatisfy { selectedItems.contains($0.index) }
        let complete = all && !p.entriesTruncated

        writingItems = true
        opts.items = complete ? "" : ItemsRange.compact(Array(selectedItems))
        writingItems = false
    }

    /// The reverse: a range typed, or restored from a profile, re-ticks the
    /// rows, so the two halves of the same statement cannot disagree on
    /// screen.
    func syncSelectedItemsFromText() {
        guard let p = probe, !p.entries.isEmpty else { return }
        let spec = opts.items.trimmingCharacters(in: .whitespaces)
        if spec.isEmpty {
            selectedItems = Set(p.entries.map { $0.index })
            return
        }
        selectedItems = Set(ItemsRange.parse(spec))
    }

    func itemsTextChanged() {
        guard !writingItems else { return }
        syncSelectedItemsFromText()
    }

    func setItem(_ index: Int, selected: Bool) {
        if selected { selectedItems.insert(index) } else { selectedItems.remove(index) }
        writeItemsFromSelection()
    }

    func selectAllItems(_ on: Bool) {
        guard let p = probe else { return }
        selectedItems = on ? Set(p.entries.map { $0.index }) : []
        writeItemsFromSelection()
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
            previewSection
            formatSection
            passesSection
            extrasSection
            connectionSection
            notificationsSection
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
            HStack {
                TextField("Video, playlist or channel URL", text: $form.opts.url)
                    .textFieldStyle(.roundedBorder)
                    /* Editing the URL invalidates any preview on screen: a
                     * Quality picker still listing the previous video's
                     * heights is worse than one listing the generic ladder,
                     * because it looks like knowledge. */
                    .onChange(of: form.opts.url) { _ in
                        if form.probe != nil || form.probeRunning { form.clearProbe() }
                    }

                Button {
                    form.startProbe(connection: settings.connection())
                } label: {
                    Label(form.probeRunning ? "Stop" : "Preview",
                          systemImage: form.probeRunning ? "stop.circle" : "magnifyingglass")
                }
                .help("Read this URL without downloading it")
            }

            /* --items, which had no control at all before the preview existed
             * -- it was part of a saved profile and reachable only by typing
             * yt-dlp's range syntax into the Advanced box as a passthrough. It
             * stays a text field rather than becoming purely a set of tick
             * boxes, for two reasons: the open-ended forms ("200-") cannot be
             * expressed by ticking a list that was truncated, and a range is
             * what a profile stores. Ticking writes here; typing here
             * re-ticks. */
            TextField(
                "Playlist items — empty means all of them (e.g. 1-20, 5,8,10-15)",
                text: $form.opts.items
            )
            .textFieldStyle(.roundedBorder)
            .onChange(of: form.opts.items) { _ in form.itemsTextChanged() }

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

    // MARK: - What the URL actually is

    /* Hidden entirely until there is something to say. An empty Preview
     * section sitting above Format would read as a section that failed to
     * load. */
    @ViewBuilder
    private var previewSection: some View {
        if form.probeRunning || form.probe != nil || !form.probeStatus.isEmpty {
            Section("Preview") {
                if !form.probeStatus.isEmpty {
                    HStack(spacing: 6) {
                        if form.probeRunning { ProgressView().controlSize(.small) }
                        Text(form.probeStatus)
                            .foregroundStyle(form.probeStatusIsError ? .red : .secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                } else if form.probeRunning {
                    HStack(spacing: 6) {
                        ProgressView().controlSize(.small)
                        Text("Reading the URL…").foregroundStyle(.secondary)
                    }
                }

                if let p = form.probe {
                    metadataRow(p)
                    if !previewNote(p).isEmpty {
                        Text(previewNote(p))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    formatDisclosure(p)
                    if !p.entries.isEmpty { entriesDisclosure(p) }
                }
            }
        }
    }

    private func metadataRow(_ p: UrlProbe) -> some View {
        HStack(alignment: .top, spacing: 10) {
            /* AsyncImage rather than a hand-rolled fetch: it is the platform's
             * own, it cancels with the view, and a thumbnail that fails to
             * load leaves the rest of the preview exactly as it was. The
             * thumbnail is the one part of this allowed to be absent. */
            AsyncImage(url: URL(string: p.thumbnail)) { image in
                image.resizable().aspectRatio(contentMode: .fill)
            } placeholder: {
                Color.clear
            }
            .frame(width: 128, height: 72)
            .clipShape(RoundedRectangle(cornerRadius: 4))

            VStack(alignment: .leading, spacing: 2) {
                Text(p.title.isEmpty ? "(no title)" : p.title)
                    .font(.headline)
                    .fixedSize(horizontal: false, vertical: true)
                Text(metadataLine(p))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
        }
    }

    private func metadataLine(_ p: UrlProbe) -> String {
        var bits: [String] = []
        if !p.uploader.isEmpty { bits.append(p.uploader) }
        if p.duration > 0 { bits.append(Format.duration(p.duration)) }
        if p.viewCount > 0 { bits.append("\(Format.count(p.viewCount)) views") }
        if !p.uploadDate.isEmpty {
            let d = Format.uploadDate(p.uploadDate)
            if !d.isEmpty { bits.append(d) }
        }
        if p.isPlaylist {
            bits.append("\(p.entryCount) item\(p.entryCount == 1 ? "" : "s")")
        }
        return bits.joined(separator: " · ")
    }

    /// Everything the user should not have to infer.
    private func previewNote(_ p: UrlProbe) -> String {
        var note = ""
        if p.fromFallback {
            note += "Read with yt-dlp directly: the installed pipeline predates "
                + "`ytdl --probe`, so the PO token provider was not used and the "
                + "list may be short. "
        } else if !p.potHealthy, !p.potNote.isEmpty {
            note += p.potNote + " "
        }
        if !p.formatsFromID.isEmpty {
            /* Named rather than presented as the playlist's own, because a
             * channel can serve 4K AV1 for a recent upload and 360p AVC for
             * one from 2011. */
            let which = p.formatsFromTitle.isEmpty ? p.formatsFromID : p.formatsFromTitle
            note += "Formats shown are for \"\(which)\". "
        }
        if p.ageLimit > 0 { note += "Age restricted (\(p.ageLimit)+). " }
        if p.liveStatus == "is_live" { note += "This is live right now. " }
        return note.trimmingCharacters(in: .whitespaces)
    }

    private func formatDisclosure(_ p: UrlProbe) -> some View {
        DisclosureGroup(
            "Formats — \(p.formats.count) rendition\(p.formats.count == 1 ? "" : "s") this video actually has"
        ) {
            ForEach(p.formats, id: \.formatID) { f in
                HStack {
                    Text(f.height > 0 ? "\(f.height)p" : "Audio")
                        .frame(width: 60, alignment: .leading)
                    Text(formatDetail(f))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer()
                }
            }
        }
    }

    private func formatDetail(_ f: UrlProbeFormat) -> String {
        var bits = [f.formatID.isEmpty ? "?" : f.formatID]
        if f.hasVideoStream { bits.append(f.vcodec) }
        if f.hasAudioStream { bits.append(f.acodec) }
        if !f.ext.isEmpty { bits.append(f.ext) }
        /* An exact size and an estimate are shown differently on purpose: the
         * tilde is the difference between a fact and yt-dlp's tbr*duration
         * guess, and presenting the guess as a fact is how a 4 GB download
         * surprises someone. */
        if f.filesize > 0 {
            bits.append(Format.bytes(UInt64(f.filesize)))
        } else if f.filesizeApprox > 0 {
            bits.append("~" + Format.bytes(UInt64(f.filesizeApprox)))
        }
        return bits.joined(separator: " · ")
    }

    private func entriesDisclosure(_ p: UrlProbe) -> some View {
        DisclosureGroup(entriesLabel(p)) {
            HStack {
                Button("All") { form.selectAllItems(true) }
                Button("None") { form.selectAllItems(false) }
                Spacer()
            }
            .buttonStyle(.link)

            ForEach(p.entries) { e in
                Toggle(isOn: Binding(
                    get: { form.selectedItems.contains(e.index) },
                    set: { form.setItem(e.index, selected: $0) }
                )) {
                    VStack(alignment: .leading, spacing: 1) {
                        Text("\(e.index). \(e.title.isEmpty ? "(untitled)" : e.title)")
                        Text(entryDetail(e))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
        }
    }

    private func entriesLabel(_ p: UrlProbe) -> String {
        var label = "Playlist items — \(p.entryCount) item\(p.entryCount == 1 ? "" : "s")"
        if p.playlistCount > p.entryCount { label += " of \(p.playlistCount)" }
        if p.entriesTruncated {
            /* Said out loud, because a silently short list of a 4,000-upload
             * channel reads as a complete one. */
            label += " · the listing was cut short; use the range field to reach the rest"
        }
        return label
    }

    private func entryDetail(_ e: UrlProbeEntry) -> String {
        var bits: [String] = []
        if e.duration > 0 { bits.append(Format.duration(e.duration)) }
        if !e.videoID.isEmpty { bits.append(e.videoID) }
        return bits.joined(separator: " · ")
    }

    // MARK: - Format

    /* The four pickers below are REBUILT from the probe when there is one, and
     * fall back to the static lists when there is not. "Best" and "Any" are
     * pinned first in their lists and are not probed values: they are pipeline
     * concepts, always available, meaning "no cap" and "no preference". */
    private var qualityChoices: [String] {
        guard let p = form.probe else { return ["2160", "1440", "1080", "720", "480", "360"] }
        /* Cross-filtered by the selected codec: 1440p is commonly published
         * only in VP9, so a list built from the union of every height offers a
         * combination this video does not have -- the same silent wrong answer
         * the static ladder gave, with better-looking numbers in it. */
        return p.heights(forCodec: form.opts.codec).map(String.init)
    }

    private var codecChoices: [String] {
        form.probe?.videoCodecs ?? ["avc1", "vp9", "av01"]
    }

    private var audioCodecChoices: [String] {
        form.probe?.audioCodecs ?? ["opus", "aac", "mp3", "flac"]
    }

    private var containerChoices: [String] {
        let c = form.probe?.containers ?? ["mkv", "mp4", "webm"]
        /* Never empty: a Picker with no rows renders as a blank control that
         * cannot be opened, which reads as a broken widget rather than as
         * "this video has none of these". mkv is always muxable. */
        return c.isEmpty ? ["mkv"] : c
    }

    private static func videoCodecLabel(_ id: String) -> String {
        switch id {
        case "avc1": return "AVC1 / H.264"
        case "vp9": return "VP9"
        case "av01": return "AV1"
        default: return id
        }
    }

    private static func audioCodecLabel(_ id: String) -> String {
        switch id {
        case "opus": return "Opus"
        case "aac": return "AAC"
        case "mp3": return "MP3"
        case "flac": return "FLAC"
        default: return id
        }
    }

    private static func containerLabel(_ id: String) -> String {
        switch id {
        case "mkv": return "MKV"
        case "mp4": return "MP4"
        case "webm": return "WebM"
        default: return id
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
                ForEach(qualityChoices, id: \.self) { h in
                    Text("\(h)p").tag(h)
                }
            }
            Text(form.probe == nil
                 ? "A ceiling, not a demand — a video that was never published at this height "
                   + "comes down at the best it has."
                 : "The heights this video actually has, for the codec selected below.")
                .font(.caption)
                .foregroundStyle(.secondary)

            Picker("Video codec", selection: $form.opts.codec) {
                Text("Any").tag("any")
                ForEach(codecChoices, id: \.self) { c in
                    Text(DownloadsView.videoCodecLabel(c)).tag(c)
                }
            }
            /* Changing the codec re-filters the Quality list, so a height that
             * only VP9 offers cannot stay selected once AV1 is chosen. */
            .onChange(of: form.opts.codec) { _ in
                guard let p = form.probe, form.opts.quality != "best" else { return }
                let available = p.heights(forCodec: form.opts.codec).map(String.init)
                if !available.contains(form.opts.quality) { form.opts.quality = "best" }
            }

            Picker("Audio codec", selection: $form.opts.audioCodec) {
                Text("Any").tag("any")
                ForEach(audioCodecChoices, id: \.self) { c in
                    Text(DownloadsView.audioCodecLabel(c)).tag(c)
                }
            }
            Picker("Container", selection: $form.opts.container) {
                ForEach(containerChoices, id: \.self) { c in
                    Text(DownloadsView.containerLabel(c)).tag(c)
                }
            }
            Text("MKV keeps everything the pipeline embeds. MP4 is the one AVFoundation can "
                 + "play in this window.")
                .font(.caption)
                .foregroundStyle(.secondary)

            /* A CEILING, the same shape as Quality: ytdl --fps is a predicate
             * with a fallback. Three values, because at most 60 already
             * admits 50 and at most 30 already admits 25 and 24. */
            Picker("Frame rate", selection: $form.opts.fps) {
                Text("Any").tag(0)
                Text("≤ 60 fps").tag(60)
                Text("≤ 30 fps").tag(30)
            }
            .disabled(!form.mediaOptionsApply)
            Text("A ceiling, like Quality. On a 60 fps upload, ≤ 30 can mean 480p.")
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

    /* Greyed out rather than hidden when they cannot apply, so the value is
     * still visible and comes back with the mode. */
    private var extrasSection: some View {
        Section("Subtitles, chapters and SponsorBlock") {
            /* Free text rather than a picker, because yt-dlp's own syntax is
             * the thing worth exposing -- regexes, exclusions, "all" -- and a
             * picker would need a language list only a probe knows. */
            TextField("Subtitle languages — empty means English (en.*); e.g. en.*,de,-live_chat",
                      text: $form.opts.subLangs)
                .textFieldStyle(.roundedBorder)
                .disabled(form.opts.noSubs)

            VStack(alignment: .leading, spacing: 2) {
                Toggle("Embed chapters", isOn: Binding(
                    get: { !form.opts.noChapters || form.sponsorMode == "mark" },
                    set: { form.opts.noChapters = !$0 }
                ))
                .disabled(!form.mediaOptionsApply || form.sponsorMode == "mark")
                Text("Chapter markers inside the media file. They are kept in the info.json "
                     + "either way.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Picker("SponsorBlock", selection: $form.sponsorMode) {
                Text("Off").tag("off")
                Text("Mark as chapters").tag("mark")
                Text("Cut out of the file").tag("remove")
            }
            .disabled(!form.mediaOptionsApply)
            Text("Cutting changes the archived file: it is no longer the one YouTube served. "
                 + "The uncut streams stay in Pre-merge streams, and the manifest records the cut.")
                .font(.caption)
                .foregroundStyle(.secondary)
            TextField("SponsorBlock categories — e.g. sponsor,selfpromo,intro or all",
                      text: $form.sponsorCats)
                .textFieldStyle(.roundedBorder)
                .disabled(!form.mediaOptionsApply || form.sponsorMode == "off")
        }
    }

    /* Settings, not per run and not in a profile. Every change is saved and
     * handed straight to the Runner, which stamps it onto every run the app
     * starts. Saved per change rather than on submit -- unlike the
     * destination -- because a proxy typed, used for a run, and then gone on
     * the next launch would be a setting that does not behave like one; the
     * file is a few hundred bytes. */
    private var connectionSection: some View {
        Section {
            Picker("Cookies", selection: $settings.cookiesSource) {
                Text("None").tag("none")
                Text("From a browser").tag("browser")
                Text("From a cookies.txt file").tag("file")
            }
            Text("For members-only, age-restricted and private videos, and YouTube Premium's "
                 + "higher bitrate.")
                .font(.caption)
                .foregroundStyle(.secondary)

            if settings.cookiesSource == "browser" {
                Picker("Browser", selection: $settings.cookiesBrowser) {
                    ForEach(DownloadsView.browsers.indices, id: \.self) { i in
                        Text(DownloadsView.browsers[i].1).tag(DownloadsView.browsers[i].0)
                    }
                }
                Text("Safari's cookies can only be read once this app has Full Disk Access "
                     + "(System Settings → Privacy & Security).")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                TextField("Browser profile — empty means the default",
                          text: $settings.cookiesProfile)
                    .textFieldStyle(.roundedBorder)
            }
            if settings.cookiesSource == "file" {
                HStack {
                    TextField("cookies.txt — read, never written; each run gets a private copy",
                              text: $settings.cookiesFile)
                        .textFieldStyle(.roundedBorder)
                    Button("Choose…") { chooseCookieFile() }
                }
            }

            TextField("Proxy — e.g. socks5h://127.0.0.1:1080; a password is masked in logs",
                      text: $settings.proxy)
                .textFieldStyle(.roundedBorder)
            TextField("Speed limit — bytes per second, e.g. 2M; per worker; empty means none",
                      text: $settings.limitRate)
                .textFieldStyle(.roundedBorder)
            Picker("Downloader", selection: $settings.downloader) {
                Text("Built in").tag("native")
                Text("aria2c").tag("aria2c")
            }
            Text("aria2c must be installed, and reports no progress here.")
                .font(.caption)
                .foregroundStyle(.secondary)
        } header: {
            Text("Connection")
        } footer: {
            Text("Saved as you change it, and used by every run this app starts — downloads, "
                 + "previews and re-fetches alike. Not part of a profile.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .onChange(of: settings.cookiesSource) { _ in connectionChanged() }
        .onChange(of: settings.cookiesBrowser) { _ in connectionChanged() }
        .onChange(of: settings.cookiesProfile) { _ in connectionChanged() }
        .onChange(of: settings.cookiesFile) { _ in connectionChanged() }
        .onChange(of: settings.proxy) { _ in connectionChanged() }
        .onChange(of: settings.limitRate) { _ in connectionChanged() }
        .onChange(of: settings.downloader) { _ in connectionChanged() }
    }

    /* Its own section rather than a row under Connection, which is about how
     * YouTube is reached; beside it because it is the same kind of thing -- a
     * setting, saved as it changes, not part of a profile. Notifier reads the
     * flag each time it has something to say, so saving is all there is. */
    private var notificationsSection: some View {
        Section {
            Toggle("Notify when the queue finishes or a run fails", isOn: $settings.notify)
            Text("Only while this app is in the background. One summary per queue, not one "
                 + "per run; a failure is announced as it happens. macOS asks for permission "
                 + "the first time there is something to say.")
                .font(.caption)
                .foregroundStyle(.secondary)
        } header: {
            Text("When you are away")
        }
        .onChange(of: settings.notify) { _ in settings.save() }
    }

    /// yt-dlp's --cookies-from-browser names. Safari first: this is the Mac.
    fileprivate static let browsers: [(String, String)] = [
        ("safari", "Safari"), ("chrome", "Chrome"), ("firefox", "Firefox"),
        ("brave", "Brave"), ("edge", "Edge"), ("chromium", "Chromium"),
        ("opera", "Opera"), ("vivaldi", "Vivaldi"), ("whale", "Whale"),
    ]

    private func connectionChanged() {
        settings.save()
        runner.setConnection(settings.connection())
    }

    private func chooseCookieFile() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = false
        panel.prompt = "Choose"
        panel.message = "Choose a cookies.txt file"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        settings.cookiesFile = url.path
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
        /* With the connection stamped on, as the Runner will: the preview is
         * the command that runs. commandPreview masks a proxy password. */
        var shown = form.runOptions
        shown.setConnection(from: settings.connection())
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
            /* The queue is sequential by design and that is correct -- but
             * thirty runs from a bulk re-fetch going one at a time look like
             * a stuck app next to a competitor running eight at once, unless
             * something says the wait is deliberate and where the real
             * parallelism lives. Shown only while something is waiting. */
            if !runner.queue.isEmpty {
                Text("\(runner.queue.count) waiting. Runs go one at a time on purpose: two ytdl "
                     + "runs at once would race on the archive's shared manifests. To download "
                     + "several videos of one playlist or channel at the same time, raise "
                     + "Workers before adding it.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
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
            try runner.enqueue(form.runOptions)
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
                try store.save(name: typed, opts: form.runOptions)
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
    @EnvironmentObject private var runner: Runner
    let record: RunRecord

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
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
            .frame(maxWidth: .infinity, alignment: .leading)

            /* Re-enqueues the record's OWN options, which RunRecord already
             * carries whole. Reconstructing them by parsing `record.command`
             * back into flags would be a second, worse copy of the argument
             * builder -- and the one place it would go wrong is a quoted URL
             * with a space in it, which is exactly the run someone wants to
             * repeat. */
            Button {
                try? runner.enqueue(record.opts)
            } label: {
                Image(systemName: "arrow.clockwise")
            }
            .buttonStyle(.borderless)
            .help("Queue this run again with the same options")
            /* A run with no URL cannot be repeated: it came from a build
             * before options were recorded, or from a hand-edited file. */
            .disabled(record.opts.url.isEmpty)
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
