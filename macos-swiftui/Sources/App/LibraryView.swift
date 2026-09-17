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
                        FilterMenu()
                    }
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
            /* The empty state has to say which of two very different things
             * happened. "There is no archive here" and "your filters exclude
             * everything" look identical as a blank grid, and only one of them
             * is the user's own doing -- showing the wrong message sends
             * someone looking for a lost archive when all they did was tick a
             * facet. So it keys off whether the INDEX is empty, not off
             * whether the search field is. */
            if model.index.entries.isEmpty {
                ContentUnavailableView {
                    Label("Nothing to show", systemImage: "film.stack")
                } description: {
                    Text("Point the app at the same path you would pass to `ytdl --path` on "
                         + "the Health pane, then press Rescan.")
                }
            } else {
                ContentUnavailableView {
                    Label("No video matches", systemImage: "line.3.horizontal.decrease.circle")
                } description: {
                    Text("The archive is not empty — the current search and filters exclude "
                         + "every video in it.")
                } actions: {
                    Button("Clear filters") { model.clearFilters() }
                }
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

/* The sort and facet control.
 *
 * A Menu rather than a popover with a custom layout: on macOS a toolbar
 * control that opens a list of toggles IS a menu, and Picker/Toggle inside one
 * get the checkmarks, the keyboard handling and the section rules from the
 * system rather than from three guesses made here.
 *
 * The count of active facets is in the LABEL, not a badge drawn on the icon.
 * A facet still narrowing the library after the menu has been closed and
 * forgotten is the one state this UI can get wrong in a way that reads as lost
 * videos, so the control visibly changes while one is on.
 */
private struct FilterMenu: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        Menu {
            Picker("Sort by", selection: sortBinding) {
                ForEach(SortKey.allCases) { key in
                    Text(key.label).tag(key)
                }
            }
            .pickerStyle(.inline)

            Picker("Order", selection: directionBinding) {
                Text("Newest first").tag(true)
                Text("Oldest first").tag(false)
            }
            .pickerStyle(.inline)

            Divider()

            /* One channel is not a choice. Offering a facet whose only effect
             * is to hide everything or nothing is worse than not offering it. */
            if model.index.channels.count > 1 {
                Menu("Channels") {
                    ForEach(model.index.channels, id: \.self) { channel in
                        Toggle(channel, isOn: channelBinding(channel))
                    }
                }
            }

            Section("Show only") {
                ForEach(FacetFlags.all, id: \.rawValue) { flag in
                    Toggle(flag.label, isOn: flagBinding(flag))
                        .disabled(flag == .verifyFailed && model.verifyCache.knownCount == 0)
                }
            }

            /* The verification facet has to say what it is a subset of. It can
             * only see videos somebody has actually verified, and a facet that
             * silently means "of the four I have checked" while looking like it
             * means "of your whole archive" is a facet that will be believed. */
            Text(verifyNote).font(.caption)

            Divider()

            Button("Clear filters") { model.clearFilters() }
                .disabled(!model.isNarrowing)
        } label: {
            Label(label, systemImage: "line.3.horizontal.decrease.circle")
        }
        .help("Sort and filter the library")
    }

    private var label: String {
        let n = model.filter.facetCount
        return n > 0 ? "Filter (\(n))" : "Filter"
    }

    private var verifyNote: String {
        let known = model.verifyCache.knownCount
        if known == 0 {
            return "Nothing has been verified yet. Use Verify on a video's page."
        }
        return "Of the \(known) video\(known == 1 ? "" : "s") verified so far."
    }

    /* The sort is persisted and the facets are not, so they get different
     * setters rather than one that guesses. */
    private var sortBinding: Binding<SortKey> {
        Binding(get: { model.filter.sort },
                set: { model.filter.sort = $0; model.persistSort() })
    }

    private var directionBinding: Binding<Bool> {
        Binding(get: { model.filter.descending },
                set: { model.filter.descending = $0; model.persistSort() })
    }

    private func channelBinding(_ channel: String) -> Binding<Bool> {
        Binding(get: { model.filter.channels.contains(channel) },
                set: { model.filter.setChannel(channel, on: $0); model.updateCounts() })
    }

    private func flagBinding(_ flag: FacetFlags) -> Binding<Bool> {
        Binding(get: { model.filter.flags.contains(flag) },
                set: { on in
                    if on { model.filter.flags.insert(flag) }
                    else { model.filter.flags.remove(flag) }
                    model.updateCounts()
                })
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
        /* A folder `ytdl --refresh` has been over. Worth a badge because it is
         * the one case where the sidecars are newer than the media -- the
         * comments on this video were fetched after it was archived, which is
         * exactly the question someone re-reading an old thread is asking. */
        if entry.refreshCount > 0 {
            out.append((entry.refreshCount == 1
                        ? "refreshed"
                        : "refreshed ×\(entry.refreshCount)", .neutral))
        }
        return out
    }
}
