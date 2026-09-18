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

import AppKit
import SwiftUI

struct LibraryView: View {
    @EnvironmentObject private var model: AppModel

    private let columns = [GridItem(.adaptive(minimum: 240, maximum: 320), spacing: 14)]

    var body: some View {
        NavigationStack(path: $model.libraryPath) {
            VStack(spacing: 0) {
                IndexBanner()
                content
                /* Below the grid rather than above it: the bar acts on what is
                 * selected, and a control strip that pushed the grid down
                 * every time the mode was entered would move the very cards
                 * the user was about to click. */
                if model.selecting { BulkBar() }
            }
                .navigationTitle("Library")
                .navigationDestination(for: String.self) { key in
                    DetailView(key: key)
                }
                .toolbar {
                    ToolbarItem(placement: .primaryAction) {
                        FilterMenu()
                    }
                    /* The selection-mode switch, in the toolbar rather than in
                     * the bulk bar: a control that dismissed the thing it
                     * lives inside would have nowhere to be when the bar was
                     * hidden. */
                    ToolbarItem(placement: .primaryAction) {
                        Toggle(isOn: $model.selecting) {
                            Label("Select", systemImage: "checkmark.circle")
                        }
                        .toggleStyle(.button)
                        .help("Select several videos to act on at once")
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
            prompt: searchPrompt
        )
        /* WHERE a search looks, attached to the search field rather than
         * buried in the filter menu. The scope changes what the same typed
         * words MEAN, so it belongs where the words are -- and it is the only
         * affordance that tells anyone the comments and captions are
         * searchable at all, which was the entire gap. */
        .searchScopes($model.searchScope) {
            ForEach(SearchScope.allCases) { scope in
                Text(scope.label).tag(scope)
            }
        }
        .onChange(of: model.searchText) { _ in model.updateSearch() }
        .onChange(of: model.searchScope) { _ in model.updateSearch() }
    }

    private var searchPrompt: String {
        switch model.searchScope {
        case .metadata: return "Search title, channel or id"
        case .comments: return "Search every archived comment"
        case .transcript: return "Search every caption line"
        case .everything: return "Search titles, comments and captions"
        }
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
                        card(for: entry)
                    }
                }
                .padding(14)
            }
        }
    }

    /* Two gestures, decided by the mode rather than by a modifier key.
     *
     * In selection mode a SINGLE click toggles: requiring a double-click to
     * tick a box would be the one gesture nobody tries. Out of it, a double
     * click opens, which is the platform's convention for "activate this". */
    @ViewBuilder
    private func card(for entry: ArchiveEntry) -> some View {
        if model.selecting {
            VideoCard(entry: entry)
                .overlay(alignment: .topLeading) {
                    Image(systemName: model.selectedKeys.contains(entry.key)
                          ? "checkmark.circle.fill" : "circle")
                        .font(.title2)
                        .foregroundStyle(model.selectedKeys.contains(entry.key)
                                         ? Color.accentColor : .secondary)
                        .padding(8)
                }
                .onTapGesture { model.toggleSelection(entry.key) }
                .accessibilityAddTraits(.isButton)
        } else {
            VideoCard(entry: entry)
                .onTapGesture(count: 2) { model.libraryPath.append(entry.key) }
                .accessibilityAddTraits(.isButton)
        }
    }
}

/* The bulk bar.
 *
 * Revealed with selection mode, and every control stays VISIBLE and goes
 * disabled on an empty selection: a bar whose contents appear and disappear as
 * you tick boxes is a bar that moves under the pointer. */
struct BulkBar: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        HStack(spacing: 8) {
            Text(model.selectedKeys.isEmpty
                 ? "Nothing selected"
                 : "\(model.selectedKeys.count) selected")
                .font(.caption)
                .foregroundStyle(.secondary)

            Button("Select all") { model.selectAllShown() }
                .help("Everything the current filter is showing — not the whole archive.")

            Button("Mark watched") { model.bulkSetWatched(true) }
                .disabled(disabled)
            Button("Mark unwatched") { model.bulkSetWatched(false) }
                .disabled(disabled)

            Menu("Add to playlist") {
                if model.userData.playlists.isEmpty {
                    Text("No playlists yet")
                } else {
                    ForEach(model.userData.playlists) { pl in
                        Button(pl.name) { model.bulkAddToPlaylist(pl.id) }
                    }
                }
            }
            .disabled(disabled)
            .fixedSize()

            Spacer()

            if model.verifyingBulk {
                Text(model.verifyProgress)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Button("Copy URLs") { copyURLs() }
                .disabled(model.selectedKeys.isEmpty)

            Button("Verify") { model.bulkVerify() }
                .disabled(model.selectedKeys.isEmpty || model.verifyingBulk)
                .help("Re-hashes every file in each selected folder. Seconds per video.")

            /* Only the three no-media modes are refreshable, which is
             * ytdl.ps1's own rule. Offering "full" here would queue a batch
             * the pipeline refuses one run at a time. */
            Menu("Re-fetch") {
                Button("Re-fetch comments") { model.bulkRefetch(mode: "comments-only") }
                Button("Re-fetch subtitles") { model.bulkRefetch(mode: "subs-only") }
                Button("Re-fetch metadata") { model.bulkRefetch(mode: "metadata-only") }
            }
            .disabled(model.selectedKeys.isEmpty)
            .fixedSize()
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(.bar)
    }

    private var disabled: Bool {
        model.selectedKeys.isEmpty || model.userData.isReadOnly
    }

    private func copyURLs() {
        guard let result = model.selectedURLs() else {
            model.status = "None of the selected videos recorded a source URL."
            return
        }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(result.text, forType: .string)
        model.status = result.have == result.total
            ? "Copied \(result.have) URL\(result.have == 1 ? "" : "s")."
            : "Copied \(result.have) of \(result.total) URLs — the rest recorded none."
    }
}

/* The only place the app can be honest about what a collection-wide search can
 * currently SEE.
 *
 * A comment search against an index that covers none of the archive returns
 * nothing, and "no results" is a lie about the archive rather than a fact about
 * it. A persistent bar rather than an alert or a transient message: the fact it
 * carries stays true until somebody acts on it, and it goes away by itself when
 * the condition does.
 */
private struct IndexBanner: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        if model.indexing {
            bar(model.indexProgress, action: nil)
        } else if model.searchNeedsIndex {
            bar(message) { model.buildSearchIndex() }
        }
    }

    private var message: String {
        let stale = model.searchIndexOutdated
        let plural = stale == 1 ? "" : "s"
        if model.searchIndex.count == 0 {
            return "Searching comments and captions needs an index. "
                + "\(stale) video\(plural) to read."
        }
        return "\(stale) video\(plural) changed since the index was built."
    }

    @ViewBuilder
    private func bar(_ text: String, action: (() -> Void)?) -> some View {
        HStack(spacing: 12) {
            Text(text).font(.callout)
            Spacer(minLength: 8)
            if let action {
                Button("Build index", action: action)
            } else {
                ProgressView().controlSize(.small)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(.selection.opacity(0.35))
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

            /* No playlists is not a choice either, exactly like one channel.
             * The menu appears the moment there is one to pick. */
            if !model.userData.playlists.isEmpty {
                Picker("Playlist", selection: playlistBinding) {
                    Text("All videos").tag(String?.none)
                    ForEach(model.userData.playlists) { pl in
                        Text(pl.name).tag(String?.some(pl.id))
                    }
                }
                .pickerStyle(.inline)
            }

            Section("Show only") {
                ForEach(FacetFlags.all, id: \.rawValue) { flag in
                    Toggle(flag.label, isOn: flagBinding(flag))
                        .disabled(flag == .verifyFailed && model.verifyCache.knownCount == 0)
                }
            }

            /* The mirror image of the verification note, and honest for the
             * opposite reason: this facet DOES see the whole archive -- a
             * video nobody has marked is unwatched, which is the correct
             * answer rather than an unknown one. What it has to say is how
             * much is already marked. */
            Text(watchedNote).font(.caption)

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

    private var watchedNote: String {
        if model.userData.isReadOnly {
            return "userdata.json could not be read; watch state is read-only "
                + "this session."
        }
        let n = model.watchedCount
        if n == 0 {
            return "Nothing is marked watched yet, so this shows everything."
        }
        return "\(n) video\(n == 1 ? " is" : "s are") marked watched."
    }

    private var playlistBinding: Binding<String?> {
        Binding(get: { model.playlistID }, set: { model.playlistID = $0 })
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
        /* Watch state. A "watched" flag you can only FILTER by and never see
         * is half a feature: the question in front of someone scrolling a
         * library is "have I seen this one", and a facet answers it only by
         * hiding everything else.
         *
         * Read through the model rather than off a captured set, so
         * userDataRevision is what redraws this -- a card holding its own
         * copy would keep showing yesterday's answer. */
        _ = model.userDataRevision
        if model.isWatched(entry.key) {
            out.append(("watched", .neutral))
        }
        return out
    }
}
