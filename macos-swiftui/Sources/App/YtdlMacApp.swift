/* ytdl-macos -- a native front end for the yt-dlp archival pipeline.
 *
 * WHAT THIS IS NOT: a reimplementation of the pipeline. Downloads are started
 * by handing a command line to the installed ytdl.ps1, exactly as a terminal
 * would. Nothing here knows what run_ytdlp.ps1 or postprocess.ps1 do with it.
 *
 * WHAT THIS DOES OWN: reading the archive. That is a fourth independent
 * implementation of docs/archive-layout.md, and the price of standalone apps
 * sharing no engine. Tests/ArchiveConformanceTests.swift is what keeps it
 * honest.
 *
 * No Rust, no webview, no bundled runtime, and no third-party package of any
 * kind -- SwiftUI, AppKit, AVFoundation, ImageIO and CryptoKit, all of which
 * every Mac already has. The one external process it depends on is the
 * pipeline's own, plus ffprobe for stream details.
 *
 * THE SHELL IS APPKIT'S, NOT HAND-BUILT. NavigationSplitView owns the sidebar
 * and the four pages; NavigationStack owns library-to-detail, which is where
 * the back button, the swipe and the ⌘[ shortcut come from; .searchable owns the
 * search field. There is deliberately no adaptive-width story: the GTK app has
 * one because GNOME targets phone-shaped windows, and macOS does not.
 */

import AppKit
import SwiftUI
import UserNotifications

@main
struct YtdlMacApp: App {
    @StateObject private var model = AppModel()
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate

    var body: some Scene {
        WindowGroup("yt-dlp Archive") {
            ContentView()
                .environmentObject(model)
                .environmentObject(model.settings)
                .environmentObject(model.runner)
                .environmentObject(model.profiles)
                .environmentObject(model.downloads)
                .environmentObject(model.subscriptions)
                .frame(minWidth: 900, minHeight: 600)
                .onAppear {
                    /* The worker starts only once the window it will publish
                     * into exists. A restored queue would otherwise begin
                     * producing events with nothing to receive them. */
                    model.runner.start()
                    model.startScan()
                    delegate.runner = model.runner
                    delegate.model = model
                }
        }
        .defaultSize(width: 1180, height: 880)
        .commands {
            CommandGroup(after: .toolbar) {
                Button("Rescan Archive") { model.startScan() }
                    .keyboardShortcut("r", modifiers: .command)
                    .disabled(model.scanning)
            }
            // Nothing here creates documents, so the New/Open group is noise.
            CommandGroup(replacing: .newItem) {}
        }
    }
}

/* Present for two reasons, both hooks SwiftUI's scene API does not have.
 *
 * To stop the download when the app quits. A run is a process TREE --
 * ytdl.ps1, a child pwsh, yt-dlp, ffmpeg -- and quitting the window does not
 * touch it. Left alone it goes on writing to the archive with nothing reading
 * its output, which is the same failure cancel exists to prevent, reached by
 * a different route.
 *
 * And to receive a click on one of Notifier's notifications, which arrives as
 * a UNUserNotificationCenterDelegate call and nowhere else. willPresent is
 * deliberately NOT implemented: without it the system does not show a
 * notification while the app is frontmost, which is the rule Notifier already
 * applies before posting -- so a notice that races the user coming back to
 * the window is swallowed rather than shown to somebody already looking. */
final class AppDelegate: NSObject, NSApplicationDelegate, UNUserNotificationCenterDelegate {
    weak var runner: Runner?
    weak var model: AppModel?

    func applicationDidFinishLaunching(_ notification: Notification) {
        /* Set here, during launch, so a click on a notification that
         * relaunched the app is delivered too. Guarded for the same reason
         * Notifier.post is: with no bundle identifier current() raises. */
        if Bundle.main.bundleIdentifier != nil {
            UNUserNotificationCenter.current().delegate = self
        }
    }

    /// A click: bring the window forward on the page that explains it.
    func userNotificationCenter(_ center: UNUserNotificationCenter,
                                didReceive response: UNNotificationResponse,
                                withCompletionHandler completionHandler: @escaping () -> Void) {
        Task { @MainActor in
            NSApp.activate()
            self.model?.section = .downloads
        }
        completionHandler()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func applicationWillTerminate(_ notification: Notification) {
        runner?.stop()
    }
}

/// The four top-level pages, which the sidebar lists and the window switches
/// between.
enum AppSection: String, CaseIterable, Identifiable {
    case library, downloads, subscriptions, health

    var id: String { rawValue }

    var title: String {
        switch self {
        case .library: return "Library"
        case .downloads: return "Downloads"
        case .subscriptions: return "Subscriptions"
        case .health: return "Health"
        }
    }

    var symbol: String {
        switch self {
        case .library: return "square.grid.2x2"
        case .downloads: return "arrow.down.circle"
        case .subscriptions: return "dot.radiowaves.left.and.right"
        case .health: return "stethoscope"
        }
    }
}

/* Everything the window shares: the index, the scan, and the two stores the
 * panes write through.
 *
 * @MainActor on the whole class rather than on its members: every property here
 * is read by a view body, and the one thing that is not on the main thread --
 * the scan -- is explicitly hopped back. */
@MainActor
final class AppModel: ObservableObject {
    @Published var index = ArchiveIndex()
    @Published var scanning = false
    @Published var status = "Starting…"
    @Published var section: AppSection = .library
    @Published var searchText = ""

    /* Sort and facets. Published so a change in the popover redraws the grid;
     * the RULES live in Core/LibraryFilter.swift rather than here, so they can
     * be tested without a window and read against the C and C# copies. */
    @Published var filter = LibraryFilter()

    /* Verification results, borrowed by the "failed verification" facet. Not
     * @Published: it is a reference type the facet reads through a closure,
     * and a verification recorded on the detail page redraws the grid through
     * the objectWillChange below rather than by being observed. */
    let verifyCache = VerifyCache()

    /* Collection-wide comment and transcript search.
     *
     * The index is LOADED at startup and never built there. Reading every
     * info.json in an archive is the most expensive thing this app can do, and
     * doing it unasked on every launch would make opening the window cost what
     * opening every video costs -- which is the exact rule the archive scan
     * already follows. The banner offers it when a scope needs it. */
    let searchIndex = SearchIndex()

    /* Watch state, resume points and playlists: the one store here whose
     * contents came from the person rather than from the pipeline, which is
     * why it lives in the state directory rather than under ~/Library/Caches.
     * Not @Published for the same reason verifyCache is not -- it is a
     * reference type, and the two @Published mirrors below are what SwiftUI
     * actually observes. */
    let userData = UserData()

    /// The playlist the Library is restricted to, by id, or nil for all
    /// videos. Kept beside the filter rather than in it, because the filter
    /// wants a SET of keys and this is the user's choice of which playlist.
    @Published var playlistID: String? {
        didSet { filter.playlistKeys = playlistID.flatMap { userData.playlistKeys($0) } }
    }

    /// Bumped whenever the store changes, so every view that shows watch state
    /// -- the grid's badges, the facet's note, the playlist menu -- redraws.
    /// A counter rather than the store itself: UserData is a class, and
    /// publishing a reference that mutates in place announces nothing.
    @Published var userDataRevision = 0

    /* Multi-select.
     *
     * A MODE rather than "command-click always multi-selects": the primary
     * gesture on a card is "open this", and a grid where a stray click adds to
     * a hidden selection does the wrong thing quietly. */
    @Published var selecting = false {
        /* Leaving the mode clears the selection. A selection nothing on screen
         * is showing is not a selection. */
        didSet { if !selecting { selectedKeys = [] } }
    }
    @Published var selectedKeys: Set<String> = []

    /// Progress for the one bulk action that takes real time.
    @Published var verifyingBulk = false
    @Published var verifyProgress = ""

    @Published var searchScope: SearchScope = .metadata
    /// The keys the current collection-wide search admits. nil when the scope
    /// needs no index or the field is empty, which is NOT the same as empty:
    /// nil means "not narrowing", empty means "narrowing to nothing".
    @Published var searchHits: Set<String>?
    @Published var indexing = false
    @Published var indexProgress = ""

    /// The library's navigation stack: a path of opaque keys, never entries.
    @Published var libraryPath: [String] = []
    /// Set when a scan fails outright, which is the one thing that must
    /// interrupt rather than sit in the status line.
    @Published var alert: String?

    let settings: Settings
    let runner = Runner()
    /* Owned here, not by the view: the panes are a switch, so DownloadsView is
     * destroyed and rebuilt on every sidebar change and anything held in its
     * @State goes with it. */
    let profiles: ProfileStore
    let downloads: DownloadsModel
    /* The pipeline's subscriptions, read through `ytdl`. Owned here for the
     * same reason `downloads` is. This app runs no timer -- see
     * Core/Subscriptions.swift.
     *
     * lazy, because it is built from `runner` and Swift will not let an
     * initialiser read a stored property until every one is set -- the reason
     * `notifier` is an optional. Its outcomes go to the status line, which is
     * this app's toast. */
    lazy var subscriptions: SubscriptionsModel = {
        let m = SubscriptionsModel(runner: runner)
        m.onMessage = { [weak self] text in self?.status = text }
        return m
    }()
    /* Optional only because it is built from `runner`, and Swift will not
     * let an initialiser read a stored property until every one is set. It
     * is set at the end of init and never cleared. */
    private var notifier: Notifier?

    private(set) var archiveRoot: String?

    /* Written by the scan thread, read by a timer on the main thread.
     *
     * In its OWN class, and not two properties here: this model is @MainActor,
     * and the scan closure runs on a background queue -- reaching into
     * main-actor state from there is exactly the thing the isolation annotation
     * exists to refuse. A lock rather than an actor because they are two
     * counters and a tick that reads a stale value simply redraws 100ms later. */
    private let counter = ScanCounter()
    private var tick: Timer?

    init() {
        let loaded = Settings.load()
        settings = loaded
        /* Before the store is read, because a fresh install has to have its
         * default profile on disk by the time the Profiles menu is built.
         * Does nothing on every launch after the first. */
        ProfileStore.seedDefaultIfMissing()
        profiles = ProfileStore.load()
        downloads = DownloadsModel(settings: loaded, store: profiles)
        /* Before anything can enqueue, so the very first run -- a re-fetch
         * started from a video's page before Downloads is ever opened included
         * -- goes out with the saved cookies and proxy. The Downloads pane
         * updates it whenever the Connection settings change. */
        runner.setConnection(loaded.connection())
        archiveRoot = AppModel.resolveRoot(settings: loaded)

        /* The saved ordering, before the first scan, so the first grid ever
         * drawn is already in the order this user chose. The FACETS
         * deliberately do not persist -- see Settings: an app that reopens
         * showing a fifth of the archive with no visible reason looks like it
         * lost your videos. */
        filter.sort = SortKey.from(id: loaded.librarySort)
        filter.descending = loaded.librarySortDescending

        /* The watched set, before the first scan for the same reason. Unlike
         * the facets this is not a filter the user set -- it is the store's
         * answer, and the "unwatched" facet is wrong without it. */
        filter.watchedKeys = userData.watchedKeys

        /* Seeded from the history the Runner restored, before its worker
         * starts, so last session's runs are never announced as if they had
         * just finished. */
        notifier = Notifier(runner: runner, settings: loaded)
    }

    /// Remember an ordering the user chose. Called from the sort control, not
    /// from the facet controls, which are not persisted.
    func persistSort() {
        settings.librarySort = filter.sort.rawValue
        settings.librarySortDescending = filter.descending
        settings.save()
        updateCounts()
    }

    private static func resolveRoot(settings: Settings) -> String? {
        let configured = settings.archiveRoot.trimmingCharacters(in: .whitespacesAndNewlines)
        if !configured.isEmpty, let resolved = Paths.resolveArchiveRoot(configured) {
            return resolved
        }
        return Paths.autodetectArchiveRoot()
    }

    /// Point the app at a different tree, and remember it.
    func setArchiveRoot(_ path: String) {
        settings.archiveRoot = path
        settings.save()
        archiveRoot = AppModel.resolveRoot(settings: settings)
        startScan()
    }

    // MARK: - Scanning

    func startScan() {
        guard !scanning else { return }

        guard let root = archiveRoot else {
            let message = "Could not find an archive. Looked for “Youtube Videos/Complete "
                + "Archive” under the usual locations. Choose the folder you would pass to "
                + "`ytdl --path` on the Health pane."
            status = message
            alert = message
            index = ArchiveIndex()
            return
        }

        scanning = true
        libraryPath.removeAll()
        ThumbnailCache.shared.clear()
        counter.reset()
        status = "Scanning…"

        /* A ticker rather than a published counter per folder: a scan of a large
         * archive calls the progress closure thousands of times a second, and
         * one @Published write per call is a re-render per call. */
        tick?.invalidate()
        let t = Timer(timeInterval: 0.1, repeats: true) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor in self.publishScanProgress() }
        }
        RunLoop.main.add(t, forMode: .common)
        tick = t

        let counter = self.counter
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            do {
                let built = try ArchiveIndex.scan(root: root) { done, total, _ in
                    counter.set(done: done, total: total)
                }
                Task { @MainActor in self?.finishScan(.success(built)) }
            } catch {
                Task { @MainActor in self?.finishScan(.failure(error)) }
            }
        }
    }

    private func publishScanProgress() {
        guard scanning else { return }
        let (done, total) = counter.get()
        status = total > 0 ? "Scanning… \(done) of \(total)" : "Scanning…"
    }

    private func finishScan(_ result: Result<ArchiveIndex, Error>) {
        scanning = false
        tick?.invalidate()
        tick = nil

        switch result {
        case .success(let built):
            index = built
            updateCounts()
        case .failure(let error):
            /* Named in full rather than reduced to "scan failed". An empty
             * library with no explanation is the exact outcome the layout
             * contract exists to prevent, and the same rule applies to not
             * finding one at all. */
            index = ArchiveIndex()
            status = error.localizedDescription
            alert = error.localizedDescription
        }
    }

    /// The status line: what is in the archive, and how much of it is showing.
    func updateCounts() {
        guard !scanning else { return }
        let size = Format.bytes(index.totalBytes)
        let shown = filteredEntries.count
        if shown != index.videoCount {
            status = "\(shown) of \(index.videoCount) videos · "
                + "\(index.channelCount) channels · \(size)"
        } else {
            status = "\(index.videoCount) videos · \(index.channelCount) channels · \(size)"
        }
    }

    // MARK: - Filtering

    /// The search field and the facets are one filter, so the needle is pushed
    /// into it here rather than being a second, parallel narrowing that the
    /// count and the empty state would each have to remember to apply.
    var filteredEntries: [ArchiveEntry] {
        var f = filter
        let text = searchText.trimmingCharacters(in: .whitespacesAndNewlines)

        /* The three shapes are deliberately different, and the difference is
         * the whole reason this is not one expression:
         *
         *   .metadata    the substring match the Library always had.
         *   .comments
         *   .transcript  the index answers alone. The needle is cleared,
         *                because leaving it set would AND the metadata match
         *                on top and a search for a word SAID in a video would
         *                return only the videos with that word in the TITLE as
         *                well.
         *   .everything  the union of both, which is folded into the key set
         *                by updateSearch -- a union cannot be expressed as a
         *                needle plus a key set, because those AND. */
        if searchScope == .metadata || text.isEmpty {
            f.needle = text
            f.keyAllow = nil
        } else {
            f.needle = ""
            f.keyAllow = searchHits ?? []
        }

        return f.apply(to: index.entries, verify: verifyCache.state(for:))
    }

    /// Recompute the collection-wide hit set for the current scope and field.
    func updateSearch() {
        let text = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard searchScope != .metadata, !text.isEmpty else {
            searchHits = nil
            updateCounts()
            return
        }

        var hits = searchIndex.query(text, scope: searchScope)
        if searchScope == .everything {
            for e in index.entries where LibraryFilter.metadataMatches(e, needle: text) {
                hits.insert(e.key)
            }
        }
        searchHits = hits
        updateCounts()
    }

    /// How many videos a collection-wide search currently cannot see.
    ///
    /// The banner is the only place the app can be honest about this. A
    /// comment search against an index that covers none of the archive returns
    /// nothing, and "no results" is a lie about the archive rather than a fact
    /// about it.
    var searchIndexOutdated: Int {
        searchIndex.outdated(in: index.entries)
    }

    func buildSearchIndex() {
        guard !indexing, !scanning else { return }
        indexing = true
        indexProgress = "Reading comments and captions…"

        let entries = index.entries
        let store = searchIndex
        Task.detached(priority: .utility) {
            store.build(over: entries) { done, total in
                Task { @MainActor in
                    self.indexProgress =
                        "Reading comments and captions… \(done) of \(total)"
                }
            }
            store.save()
            await MainActor.run {
                self.indexing = false
                /* Re-run the search rather than just redrawing: the whole
                 * point of having built the index is that the query the user
                 * already typed can now be answered. */
                self.updateSearch()
            }
        }
    }

    /// Whether anything at all is narrowing the library, INCLUDING the search
    /// field. Drives the empty state, which has to tell "there is no archive
    /// here" apart from "your filters exclude everything" -- they look
    /// identical as a blank grid and only one of them is the user's own doing.
    var isNarrowing: Bool {
        !searchText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            || filter.facetCount > 0
    }

    /// True when a collection-wide scope is selected and the index cannot
    /// answer for part of the archive. The banner keys off this.
    var searchNeedsIndex: Bool {
        searchScope.needsIndex && searchIndexOutdated > 0
    }

    /// Clear the facets AND the search field. The needle is part of the filter
    /// as far as the user is concerned, so leaving the search box populated
    /// after "Clear filters" would leave a term visibly applied that is not.
    func clearFilters() {
        filter.reset()
        /* reset() drops the playlist RESTRICTION; this drops the user's
         * selection with it. Leaving the id set would leave the popover
         * claiming a playlist while the grid showed the whole archive. */
        playlistID = nil
        searchText = ""
        searchHits = nil
        updateCounts()
    }

    // MARK: - Watch state and playlists

    /// Hand the store's watched set to the filter and redraw.
    ///
    /// Called after every change rather than the set being handed over once,
    /// because Swift's Set is a VALUE: unlike the GTK port's borrowed hash
    /// table, a copy taken at startup would never see a later change. That is
    /// the same difference that made `userDataRevision` necessary.
    private func syncWatchState() {
        filter.watchedKeys = userData.watchedKeys
        if let id = playlistID {
            filter.playlistKeys = userData.playlistKeys(id)
        }
        userDataRevision &+= 1
        updateCounts()
    }

    func setWatched(_ key: String, _ watched: Bool) {
        userData.setWatched(key, watched)
        userData.save()
        syncWatchState()
    }

    func isWatched(_ key: String) -> Bool { userData.isWatched(key) }

    /// Record where playback got to. The three rules -- finished clears the
    /// resume point, a glance stores nothing, anything else is kept -- are
    /// UserData's, not this file's.
    func recordPosition(_ key: String, seconds: Double, duration: Double) {
        let wasWatched = userData.isWatched(key)
        userData.setPosition(key, seconds: seconds, duration: duration)
        userData.save()
        /* Only redraw when the WATCHED flag moved. A resume point changes
         * every five seconds while something is playing, and a published
         * change that often would re-render the whole window for a number
         * nothing on screen is showing. */
        if userData.isWatched(key) != wasWatched { syncWatchState() }
    }

    func resumePosition(_ key: String) -> Double { userData.position(key) }

    @discardableResult
    func createPlaylist(named name: String, adding key: String? = nil) -> Playlist? {
        guard let pl = userData.createPlaylist(named: name) else { return nil }
        if let key { userData.addToPlaylist(pl.id, key: key) }
        userData.save()
        syncWatchState()
        return pl
    }

    func togglePlaylistMembership(_ id: String, key: String) {
        if userData.playlistContains(id, key: key) {
            userData.removeFromPlaylist(id, key: key)
        } else {
            userData.addToPlaylist(id, key: key)
        }
        userData.save()
        syncWatchState()
    }

    func deletePlaylist(_ id: String) {
        userData.deletePlaylist(id)
        if playlistID == id { playlistID = nil }
        userData.save()
        syncWatchState()
    }

    // MARK: - Bulk actions

    /// Everything the FILTER is showing, not the whole archive. The other
    /// reading is how somebody marks four thousand videos watched by accident
    /// from inside a filtered view.
    func selectAllShown() {
        selectedKeys = Set(filteredEntries.map(\.key))
    }

    func toggleSelection(_ key: String) {
        if selectedKeys.contains(key) { selectedKeys.remove(key) }
        else { selectedKeys.insert(key) }
    }

    /// One save for the whole batch. Saving per video would rewrite the store a
    /// few hundred times for one button press.
    func bulkSetWatched(_ watched: Bool) {
        guard !selectedKeys.isEmpty, !userData.isReadOnly else { return }
        for key in selectedKeys { userData.setWatched(key, watched) }
        userData.save()
        let n = selectedKeys.count
        status = "Marked \(n) video\(n == 1 ? "" : "s") "
            + (watched ? "watched." : "unwatched.")
        syncWatchState()
    }

    /* ADD-ONLY, and deliberately without a tick state. A mixed selection where
     * some videos are in a playlist and some are not has no honest checkbox
     * state, and a control that flipped each one independently would remove
     * half of them. */
    func bulkAddToPlaylist(_ id: String) {
        guard !selectedKeys.isEmpty, !userData.isReadOnly else { return }
        var added = 0
        for key in selectedKeys where userData.addToPlaylist(id, key: key) {
            added += 1
        }
        userData.save()
        let name = userData.playlist(id)?.name ?? "the playlist"
        status = "Added \(added) video\(added == 1 ? "" : "s") to \(name)."
        syncWatchState()
    }

    /// The selected videos' source URLs, newline-separated, or nil when none of
    /// them recorded one.
    ///
    /// A folder with no original_url contributes NOTHING rather than a blank
    /// line: a list with holes in it is worse than a shorter list, because the
    /// holes are invisible once it is pasted somewhere.
    func selectedURLs() -> (text: String, have: Int, total: Int)? {
        let entries = filteredEntries.filter { selectedKeys.contains($0.key) }
        let urls = entries.compactMap { e -> String? in
            guard let u = e.originalURL, !u.isEmpty else { return nil }
            return u
        }
        guard !urls.isEmpty else { return nil }
        return (urls.joined(separator: "\n"), urls.count, entries.count)
    }

    /* ONE RUN PER VIDEO, not one run with many URLs. `ytdl --refresh` refreshes
     * the video it is given; a session with several URLs would be a --sync-like
     * shape the refusal list rejects, and one that failed halfway would leave
     * no way to tell which videos were reached. Separate queue entries also
     * mean a single failure is one red row rather than the whole batch. */
    func bulkRefetch(mode: String) {
        let entries = filteredEntries.filter { selectedKeys.contains($0.key) }

        var queued = 0
        for e in entries {
            guard let url = e.originalURL, !url.isEmpty else { continue }
            var opts = RunOptions()
            opts.url = url
            opts.mode = mode
            opts.refresh = true
            do {
                try runner.enqueue(opts)
                queued += 1
            } catch {
                /* A failure to QUEUE -- a full queue, an unwritable state
                 * directory -- stops the batch rather than silently dropping
                 * the rest of it. Carrying on would report a count that was
                 * never true. */
                status = "Queued \(queued) before failing: "
                    + error.localizedDescription
                section = .downloads
                return
            }
        }

        guard queued > 0 else {
            status = "None of the selected videos recorded a source URL."
            return
        }
        status = "Queued \(queued) \(mode) refresh\(queued == 1 ? "" : "es")."
        section = .downloads
    }

    /// Verify every selected folder. Seconds per video, so it runs off the main
    /// thread with a progress readout.
    func bulkVerify() {
        guard !verifyingBulk, !selectedKeys.isEmpty else { return }

        /* The keys and directories are copied out HERE, while the index is
         * known to be current. A worker holding entries would be holding them
         * against an index a rescan may have replaced. */
        let targets = filteredEntries
            .filter { selectedKeys.contains($0.key) }
            .map { (key: $0.key, dir: $0.dir) }
        guard !targets.isEmpty else { return }

        verifyingBulk = true
        verifyProgress = "Verifying 0 of \(targets.count)…"

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            var results: [(String, VerifyState)] = []
            for (i, t) in targets.enumerated() {
                let r = Health.verifyChecksums(videoDir: t.dir)
                /* UNKNOWN, not ok. A folder with no checksums.sha256 has not
                 * passed and has not failed -- the layout contract says to
                 * tolerate one -- and recording it as a pass would put a green
                 * answer in the cache for a folder nothing hashed. */
                let state: VerifyState
                if !r.present { state = .unknown }
                else if !r.failed.isEmpty || !r.missing.isEmpty { state = .failed }
                else { state = .ok }
                results.append((t.key, state))

                let done = i + 1
                DispatchQueue.main.async {
                    self?.verifyProgress = "Verifying \(done) of \(targets.count)…"
                }
            }

            DispatchQueue.main.async { self?.finishBulkVerify(results) }
        }
    }

    /* On the MAIN thread, because the cache stamps every record with the
     * folder's archive_creation_time and that means a lookup in the index.
     *
     * The results go into the same cache the detail page writes: a bulk verify
     * whose findings the "failed verification" facet could not see would be a
     * summary you read once and then had no way to act on. */
    private func finishBulkVerify(_ results: [(String, VerifyState)]) {
        verifyingBulk = false
        verifyProgress = ""

        var checked = 0
        var bad = 0
        var unchecked = 0

        for (key, state) in results {
            guard state != .unknown else { unchecked += 1; continue }
            checked += 1
            if state == .failed { bad += 1 }
            if let e = index.entry(forKey: key) { verifyCache.set(state, for: e) }
        }
        verifyCache.save()

        /* The summary separates "passed" from "had nothing to check". A folder
         * with no checksums.sha256 is not a failure, but it is not a pass
         * either, and folding it into the pass count would be this app claiming
         * it checked folders it never opened a single hash in. */
        var note: String
        if checked == 0 {
            note = "Nothing to verify: \(unchecked) folder"
                + (unchecked == 1 ? " has" : "s have") + " no checksums.sha256."
        } else if bad == 0 {
            note = "All \(checked) verified."
        } else {
            note = "\(bad) of \(checked) failed verification."
        }
        if checked > 0 && unchecked > 0 {
            note += " \(unchecked) had no checksums.sha256."
        }

        status = note
        objectWillChange.send()
        updateCounts()
    }

    /// How many videos are marked watched, so the facet can say what it is a
    /// subset of. On a fresh install "unwatched" means "all of them", and a
    /// facet that appears to do nothing reads as broken.
    var watchedCount: Int { userData.watchedCount }

    /// Record a verification result and redraw anything reading the facet.
    /// Called from the detail page and, later, from a bulk verify.
    func recordVerification(_ state: VerifyState, for entry: ArchiveEntry) {
        verifyCache.set(state, for: entry)
        objectWillChange.send()
    }

    func entry(forKey key: String) -> ArchiveEntry? {
        index.entry(forKey: key)
    }
}

/// Two counters shared between the scan thread and the main thread, and
/// nothing else. Deliberately outside AppModel, which is @MainActor.
final class ScanCounter {
    private let lock = NSLock()
    private var done = 0
    private var total = 0

    func reset() { set(done: 0, total: 0) }

    func set(done: Int, total: Int) {
        lock.lock()
        self.done = done
        self.total = total
        lock.unlock()
    }

    func get() -> (done: Int, total: Int) {
        lock.lock()
        defer { lock.unlock() }
        return (done, total)
    }
}
