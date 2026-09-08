/* The window: a sidebar, the three pages, and a status line under them.
 *
 * The sidebar is the macOS equivalent of the GTK app's AdwViewSwitcher, and
 * NOT a TabView. Both are in AppKit's vocabulary; the sidebar wins because
 * these three pages are places rather than modes of one thing, and because the
 * source-list treatment is what a Mac user reads as "sections of this app".
 *
 * THE STATUS LINE REPLACES THE TOAST OVERLAY, and that is a real decision
 * rather than an omission. libadwaita has AdwToast; macOS has nothing
 * equivalent and no honest way to build one -- a floating capsule that fades
 * away is an iOS import, and the third-party packages that provide it are the
 * kind of dependency this project exists without. So transient state lives in a
 * permanent, quiet readout at the bottom of the window, and the one case that
 * must interrupt -- there is no archive to read at all -- gets an alert.
 */

import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        NavigationSplitView {
            /* An explicit ForEach with .tag, not List(data, selection:): that
             * initialiser makes the selection the element's ID -- a String
             * here -- rather than the section itself, which is a mismatch the
             * compiler reports several types away from the cause. The optional
             * binding is what List selection takes; nothing here ever sets it
             * to nil, so a click that lands between rows keeps the page. */
            List(selection: Binding<AppSection?>(
                get: { model.section },
                set: { if let new = $0 { model.section = new } }
            )) {
                ForEach(AppSection.allCases) { section in
                    Label(section.title, systemImage: section.symbol)
                        .tag(section)
                }
            }
            .navigationSplitViewColumnWidth(min: 160, ideal: 190, max: 260)
        } detail: {
            detail
                .safeAreaInset(edge: .bottom, spacing: 0) { statusBar }
        }
        .alert(
            "Cannot read the archive",
            isPresented: Binding(
                get: { model.alert != nil },
                set: { if !$0 { model.alert = nil } }
            ),
            actions: { Button("OK", role: .cancel) { model.alert = nil } },
            message: { Text(model.alert ?? "") }
        )
    }

    @ViewBuilder
    private var detail: some View {
        switch model.section {
        case .library:
            LibraryView()
        case .downloads:
            DownloadsView()
        case .health:
            HealthView()
        }
    }

    private var statusBar: some View {
        VStack(spacing: 0) {
            Divider()
            HStack(spacing: 8) {
                if model.scanning {
                    ProgressView()
                        .controlSize(.small)
                        .scaleEffect(0.7)
                }
                Text(model.status)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Spacer()
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 5)
        }
        .background(.bar)
    }
}
