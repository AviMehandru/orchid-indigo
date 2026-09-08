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
 * and the three pages; NavigationStack owns library-to-detail, which is where
 * the back button, the swipe and the ⌘[ shortcut come from; .searchable owns the
 * search field. There is deliberately no adaptive-width story: the GTK app has
 * one because GNOME targets phone-shaped windows, and macOS does not.
 */

import AppKit
import SwiftUI

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
                .frame(minWidth: 900, minHeight: 600)
                .onAppear {
                    /* The worker starts only once the window it will publish
                     * into exists. A restored queue would otherwise begin
                     * producing events with nothing to receive them. */
                    model.runner.start()
                    model.startScan()
                    delegate.runner = model.runner
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

/* Present for one reason: to stop the download when the app quits.
 *
 * A run is a process TREE -- ytdl.ps1, a child pwsh, yt-dlp, ffmpeg -- and
 * quitting the window does not touch it. Left alone it goes on writing to the
 * archive with nothing reading its output, which is the same failure cancel
 * exists to prevent, reached by a different route. SwiftUI has no scene-level
 * termination hook, so this is an NSApplicationDelegate and nothing else. */
final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var runner: Runner?

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func applicationWillTerminate(_ notification: Notification) {
        runner?.stop()
    }
}

/// The three top-level pages, which the sidebar lists and the window switches
/// between.
enum AppSection: String, CaseIterable, Identifiable {
    case library, downloads, health

    var id: String { rawValue }

    var title: String {
        switch self {
        case .library: return "Library"
        case .downloads: return "Downloads"
        case .health: return "Health"
        }
    }

    var symbol: String {
        switch self {
        case .library: return "square.grid.2x2"
        case .downloads: return "arrow.down.circle"
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
    let profiles = ProfileStore.load()
    let downloads: DownloadsModel

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
        downloads = DownloadsModel(settings: loaded, store: profiles)
        archiveRoot = AppModel.resolveRoot(settings: loaded)
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

    /// Case-insensitive substring over title, uploader, video id and channel.
    var filteredEntries: [ArchiveEntry] {
        let needle = searchText.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard !needle.isEmpty else { return index.entries }

        return index.entries.filter { e in
            for field in [e.title, e.uploader, e.videoID ?? "", e.channel]
            where field.lowercased().contains(needle) {
                return true
            }
            return false
        }
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
