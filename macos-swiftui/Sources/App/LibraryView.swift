/* The library grid.
 *
 * A LazyVGrid in a ScrollView rather than an eager stack of cards: the grid
 * builds only what is visible, so an archive with several thousand videos costs
 * the same to open as one with twenty. That is the shape of archive this tool
 * exists to produce.
 *
 * The grid hands over the opaque KEY, not the entry, so a rescan that finishes
 * between the click and the push cannot leave the detail page showing a video
 * that is no longer in the index -- it looks the key up again against whatever
 * index is current.
 */

import SwiftUI

struct LibraryView: View {
    @EnvironmentObject private var model: AppModel

    private let columns = [GridItem(.adaptive(minimum: 240, maximum: 320), spacing: 14)]

    var body: some View {
        NavigationStack(path: $model.libraryPath) {
            content
                .navigationTitle("Library")
                .navigationDestination(for: String.self) { key in
                    DetailView(key: key)
                }
                .toolbar {
                    ToolbarItem(placement: .primaryAction) {
                        Button {
                            model.startScan()
                        } label: {
                            Label("Rescan", systemImage: "arrow.clockwise")
                        }
                        .disabled(model.scanning)
                        .help("Re-read the archive from disk")
                    }
                }
        }
        .searchable(
            text: $model.searchText,
            placement: .toolbar,
            prompt: "Search title, channel or id"
        )
        .onChange(of: model.searchText) { _ in model.updateCounts() }
    }

    @ViewBuilder
    private var content: some View {
        let entries = model.filteredEntries
        if entries.isEmpty {
            /* ContentUnavailableView rather than a centred label: it is the
             * widget every macOS application uses for this, so the icon size,
             * the type scale and the vertical centring are the ones the rest of
             * the system uses rather than three guesses made here. */
            ContentUnavailableView {
                Label("Nothing to show", systemImage: "film.stack")
            } description: {
                Text(model.searchText.isEmpty
                     ? "Point the app at the same path you would pass to `ytdl --path` on the "
                       + "Health pane, then press Rescan."
                     : "No video matches “\(model.searchText)”.")
            }
        } else {
            ScrollView {
                LazyVGrid(columns: columns, spacing: 14) {
                    ForEach(entries) { entry in
                        VideoCard(entry: entry)
                            .onTapGesture(count: 2) { model.libraryPath.append(entry.key) }
                            /* Double-click opens, which is the platform's
                             * convention for "activate this". The button keeps
                             * a single-click route for anyone who expects one,
                             * and makes the card reachable from the keyboard. */
                            .accessibilityAddTraits(.isButton)
                    }
                }
                .padding(14)
            }
        }
    }
}

private struct VideoCard: View {
    @EnvironmentObject private var model: AppModel
    let entry: ArchiveEntry

    var body: some View {
        Button {
            model.libraryPath.append(entry.key)
        } label: {
            VStack(alignment: .leading, spacing: 0) {
                ZStack(alignment: .bottomTrailing) {
                    ThumbnailImage(path: entry.thumbnailPath)
                    /* The duration belongs ON the thumbnail, where every video
                     * player and every video site puts it -- not in the subtitle
                     * competing with the channel name and the date for the same
                     * run of small grey text. Hidden rather than blank when
                     * there is no duration: an empty badge is a dark smudge in
                     * the corner of the image. */
                    let duration = Format.duration(entry.duration)
                    if !duration.isEmpty {
                        Text(duration)
                            .font(.caption2.weight(.bold))
                            .foregroundStyle(.white)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 1)
                            .background(.black.opacity(0.72), in: RoundedRectangle(cornerRadius: 6))
                            .padding(6)
                    }
                }

                VStack(alignment: .leading, spacing: 4) {
                    Text(entry.title)
                        .font(.system(.subheadline, weight: .semibold))
                        .lineLimit(2)
                        .multilineTextAlignment(.leading)
                        .frame(maxWidth: .infinity, alignment: .leading)

                    Text(subtitle)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .frame(maxWidth: .infinity, alignment: .leading)

                    if !badges.isEmpty {
                        HStack(spacing: 6) {
                            ForEach(badges, id: \.text) { badge in
                                Pill(text: badge.text, variant: badge.variant)
                            }
                        }
                    }
                }
                .padding(10)
            }
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 10))
            .overlay(
                RoundedRectangle(cornerRadius: 10)
                    .strokeBorder(.separator, lineWidth: 1)
            )
            .clipShape(RoundedRectangle(cornerRadius: 10))
        }
        .buttonStyle(.plain)
        .help(entry.title)
        .contextMenu {
            Button("Open") { model.libraryPath.append(entry.key) }
            Button("Reveal in Finder") { ExternalOpen.revealInFinder(entry.dir) }
        }
    }

    private var subtitle: String {
        let date = Format.uploadDate(entry.uploadDate)
        if date.isEmpty { return entry.uploader }
        return "\(entry.uploader) · \(date)"
    }

    private var badges: [(text: String, variant: Pill.Variant)] {
        var out: [(text: String, variant: Pill.Variant)] = []
        if entry.mediaIndex == nil {
            /* Not an error. --mode metadata-only, comments-only and subs-only
             * all write a complete folder with no media, and so does an
             * interrupted run. Saying WHICH is the manifest's job, not a guess
             * from here. */
            out.append((entry.downloadMode ?? "no media file", .neutral))
        }
        if entry.layoutTooNew {
            out.append(("newer archive layout", .warn))
        }
        return out
    }
}
