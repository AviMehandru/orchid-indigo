/* The Health pane: what is installed, what version, and does the archive still
 * verify.
 *
 * Read-only, like everything behind it. The pipeline updates yt-dlp on its own
 * 24h throttle and a second updater racing it from a window is exactly the
 * shared-state collision the pipeline spent a release removing.
 *
 * This is also where the archive root is chosen, because "the library is empty"
 * and "the app is looking at the wrong folder" are the same question and this is
 * the pane that answers it.
 */

import AppKit
import SwiftUI

struct HealthView: View {
    @EnvironmentObject private var model: AppModel
    @EnvironmentObject private var settings: Settings
    @StateObject private var health = HealthModel()

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                dependencies
                installedFiles
                config
                archive
                logs
            }
            .frame(maxWidth: 860, alignment: .leading)
            .frame(maxWidth: .infinity, alignment: .center)
            .padding(16)
        }
        .navigationTitle("Health")
        .onAppear { health.refresh(force: false, model: model, settings: settings) }
        /* A finished scan changes what half of this pane reports -- the data
         * root, global_manifest.json, archive.txt and the snapshot count all
         * come from files beside the archive that was just re-read. Without
         * this they stay as they were when the pane was last opened. */
        .onChange(of: model.scanning) { scanning in
            if !scanning { health.refresh(force: false, model: model, settings: settings) }
        }
    }

    // MARK: - Dependencies

    private var dependencies: some View {
        SectionBox(title: "Dependencies", trailing: AnyView(
            HStack(spacing: 8) {
                if health.probing {
                    ProgressView().controlSize(.small).scaleEffect(0.7)
                }
                Button("Re-probe") {
                    health.refresh(force: true, model: model, settings: settings)
                }
                .disabled(health.probing)
                .help("Skips the five-minute cache and runs every --version again. What you "
                      + "want right after installing something that was missing.")
            }
        )) {
            ForEach(health.dependencies) { dep in
                DependencyRow(dependency: dep)
            }
            if health.dependencies.isEmpty {
                Text("Probing…").foregroundStyle(.secondary)
            }
        }
    }

    // MARK: - Installed files

    private var installedFiles: some View {
        SectionBox(title: "Installed pipeline files") {
            /* What is INSTALLED, never what is in a checkout. Editing a file in
             * a clone has no effect on a live install until it is copied over,
             * which is exactly the confusion this list settles. */
            ForEach(health.files) { file in
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Pill(text: file.present ? "installed" : "missing",
                         variant: file.present ? .ok : .error)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(file.name)
                        Text(file.present
                             ? "\(Format.bytes(file.size)) · \(Format.timestamp(file.modified)) "
                               + "· \(file.path)"
                             : file.path)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    Spacer()
                }
            }
        }
    }

    // MARK: - Config

    private var config: some View {
        SectionBox(title: "yt-dlp.conf") {
            KeyValueRow(
                key: "CONFIG_VERSION",
                value: health.config?.configVersion ?? "not found",
                monospaced: true
            )
            KeyValueRow(key: "Options set", value: String(health.config?.optionCount ?? 0))
            KeyValueRow(key: "Path", value: health.config?.path, monospaced: true)

            if health.config?.present == false {
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Pill(text: "problem", variant: .error)
                    Text("yt-dlp.conf is not installed. Downloads will run with yt-dlp's own "
                         + "defaults rather than this pipeline's.")
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    // MARK: - Archive

    private var archive: some View {
        SectionBox(title: "Archive", trailing: AnyView(
            Button("Choose folder…") { chooseArchiveRoot() }
                .help("Point the Library at a different archive — a data root, the "
                      + "Youtube Videos folder, or Complete Archive itself.")
        )) {
            KeyValueRow(
                key: "Indexed",
                value: "\(model.index.videoCount) videos · \(model.index.channelCount) "
                    + "channels · \(Format.bytes(model.index.totalBytes))"
            )
            KeyValueRow(key: "Archive root", value: model.index.root.isEmpty
                        ? "none found" : model.index.root, monospaced: true)
            KeyValueRow(key: "Data root", value: health.stats?.dataRoot, monospaced: true)
            KeyValueRow(
                key: "global_manifest.json",
                value: (health.stats?.globalManifestEntries ?? -1) >= 0
                    ? String(health.stats!.globalManifestEntries) : "not readable"
            )
            KeyValueRow(
                key: "archive.txt ids",
                value: (health.stats?.archiveTxtIDs ?? -1) >= 0
                    ? String(health.stats!.archiveTxtIDs) : "not readable"
            )
            KeyValueRow(
                key: "Archive History snapshots",
                value: String(health.stats?.historySnapshots ?? 0)
            )

            /* The index counts what this app walked; global_manifest.json is
             * what the pipeline wrote. Them disagreeing is the single most
             * useful signal on this pane -- it means one of them is looking at a
             * different folder. */
            if let stats = health.stats, stats.globalManifestEntries >= 0,
               stats.globalManifestEntries != model.index.videoCount {
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Pill(text: "check", variant: .warn)
                    Text("The indexed count and global_manifest.json disagree. Usually that "
                         + "means the Library is pointed at a different folder from the one "
                         + "downloads are going to.")
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    private func chooseArchiveRoot() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Choose"
        panel.message = "Choose the archive folder (a data root, or Complete Archive itself)"
        guard panel.runModal() == .OK, let url = panel.url else { return }

        model.setArchiveRoot(url.path)
        health.refresh(force: false, model: model, settings: settings)
    }

    // MARK: - Logs

    private var logs: some View {
        SectionBox(title: "Logs", trailing: AnyView(
            Picker("", selection: $health.logChoice) {
                Text("download.log").tag(HealthModel.LogChoice.download)
                Text("archive.txt").tag(HealthModel.LogChoice.archive)
            }
            .labelsHidden()
            .frame(width: 160)
            .onChange(of: health.logChoice) { _ in health.reloadLog(settings: settings) }
        )) {
            ScrollView {
                Text(health.logText.isEmpty
                     ? "Nothing to show yet."
                     : health.logText)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(6)
            }
            .frame(height: 220)
        }
    }
}

private struct DependencyRow: View {
    let dependency: Dependency

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Pill(text: dependency.found ? "found" : "missing", variant: variant)
            VStack(alignment: .leading, spacing: 2) {
                Text(dependency.name)
                /* The subtitle is where it is, when it is there, and why it
                 * matters when it is not. The note only earns its space when
                 * something is wrong: on a healthy machine seven paragraphs of
                 * explanation is just noise to scroll past. */
                Text(dependency.found ? (dependency.path ?? "") : dependency.note)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer()
            /* Found with no version means the probe hit its eight-second
             * timeout, which is a different thing from missing and is worth
             * saying rather than showing a blank. */
            if let version = dependency.version ?? (dependency.found ? "version probe timed out" : nil) {
                Text(version)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }
            Pill(text: dependency.importance)
        }
    }

    private var variant: Pill.Variant {
        if dependency.found { return .ok }
        return dependency.importance == "required" ? .error : .warn
    }
}

/* The pane's own state. The cheap half is straight file reads and happens
 * inline; the dependency probe is seven subprocess spawns with an eight-second
 * ceiling each and goes on a background queue, for the same reason the archive
 * scan does: the pane must draw before it knows the answers, or it looks broken
 * rather than busy. */
@MainActor
final class HealthModel: ObservableObject {
    enum LogChoice: Hashable { case download, archive }

    @Published var dependencies: [Dependency] = []
    @Published var files: [Health.InstalledFile] = []
    @Published var config: Health.ConfigInfo?
    @Published var stats: Health.ArchiveStats?
    @Published var logText = ""
    @Published var logChoice: LogChoice = .download
    @Published var probing = false

    func refresh(force: Bool, model: AppModel, settings: Settings) {
        files = Health.installedFiles()
        config = Health.configInfo()
        stats = Health.archiveStats(
            dataRoot: settings.resolvedDataRoot,
            videos: model.index.videoCount,
            channels: model.index.channelCount,
            totalBytes: model.index.totalBytes
        )
        reloadLog(settings: settings)

        guard !probing else { return }
        probing = true
        DispatchQueue.global(qos: .userInitiated).async {
            let probed = Health.dependencies(force: force)
            Task { @MainActor in
                self.dependencies = probed
                self.probing = false
            }
        }
    }

    func reloadLog(settings: Settings) {
        let name = logChoice == .archive ? "archive.txt" : "download.log"
        let path = Paths.join(
            Paths.join(settings.resolvedDataRoot, "Archive Logs/Logs"), name
        )
        let tail = Health.logTail(path: path, lines: 300)
        logText = tail.isEmpty ? "Nothing to show. \(path) does not exist yet." : tail
    }
}
