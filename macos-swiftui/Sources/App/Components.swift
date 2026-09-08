/* The handful of pieces every pane uses.
 *
 * There is no stylesheet here and no theme. The GTK app carries one because
 * libadwaita has no label-sized status widget and no media card; AppKit's
 * material and shape vocabulary covers both, so a pill is a Capsule in a
 * tinted fill and a card is a rounded rectangle in .background(.regularMaterial)
 * -- both of which follow the system appearance and accent colour on their own,
 * which is the property that mattered about the CSS it replaces.
 */

import AppKit
import ImageIO
import SwiftUI

// MARK: - Pills

/* A coloured pill rather than a coloured word. On a table of seven rows the
 * SHAPE is what you scan -- "missing" in red text beside "required" in grey text
 * reads as one run-on phrase. */
struct Pill: View {
    enum Variant {
        case neutral, ok, warn, error, accent

        var color: Color {
            switch self {
            case .neutral: return .secondary
            case .ok: return .green
            case .warn: return .orange
            case .error: return .red
            case .accent: return .accentColor
            }
        }
    }

    let text: String
    var variant: Variant = .neutral

    var body: some View {
        Text(text)
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 8)
            .padding(.vertical, 2)
            .background(variant.color.opacity(0.15), in: Capsule())
            .foregroundStyle(variant == .neutral ? Color.secondary : variant.color)
    }
}

// MARK: - Key/value rows

/// One fact per row: the name on the left, the value on the right. Selectable,
/// because most of these are paths and ids somebody wants to paste somewhere.
struct KeyValueRow: View {
    let key: String
    let value: String?
    var monospaced = false

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 12) {
            Text(key)
                .foregroundStyle(.secondary)
                .frame(width: 150, alignment: .leading)
            Text(value?.isEmpty == false ? value! : "—")
                .font(monospaced ? .system(.body, design: .monospaced) : .body)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                /* Wrapping, not truncating, and this is the macOS spelling of a
                 * trap the GTK app hit: a label with neither wrap nor ellipsis
                 * reports minimum == natural and every container above it
                 * inherits that, which is how one transcript note ended up
                 * setting the whole window's minimum width to 507px. */
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

/// A titled block of rows, which is what the Health and Detail panes are made of.
struct SectionBox<Content: View>: View {
    let title: String
    var trailing: AnyView? = nil
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(title).font(.headline)
                Spacer()
                if let trailing { trailing }
            }
            VStack(alignment: .leading, spacing: 6) {
                content
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(12)
            .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 8))
        }
    }
}

// MARK: - Thumbnails

/* Decoded ONCE and reused as the grid scrolls past them, and decoded SCALED:
 * a 1920x1080 thumbnail decoded whole costs about eight megabytes per card, and
 * an archive has thousands of cards.
 *
 * CGImageSourceCreateThumbnailAtIndex is what does the scaling during the
 * decode. NSImage(contentsOfFile:) would decode the full frame and scale
 * afterwards, which is the version of this that makes scrolling stutter. */
final class ThumbnailCache {
    static let shared = ThumbnailCache()

    private let cache = NSCache<NSString, NSImage>()

    private init() {
        // Enough for a long scroll without holding an archive's worth of pixels.
        cache.countLimit = 600
    }

    func cached(_ path: String) -> NSImage? {
        cache.object(forKey: path as NSString)
    }

    /// BLOCKS on a decode. Call it off the main thread.
    func load(_ path: String, maxPixel: Int = 480) -> NSImage? {
        if let hit = cached(path) { return hit }

        let url = URL(fileURLWithPath: path) as CFURL
        guard let source = CGImageSourceCreateWithURL(url, nil) else { return nil }
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixel,
        ]
        guard let cg = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else {
            return nil
        }

        let image = NSImage(cgImage: cg, size: NSSize(width: cg.width, height: cg.height))
        cache.setObject(image, forKey: path as NSString)
        return image
    }

    func clear() { cache.removeAllObjects() }
}

/// A frame that is always the same size whether or not an image loads, so the
/// grid does not reflow underneath the pointer as thumbnails decode.
struct ThumbnailImage: View {
    let path: String?
    var height: CGFloat = 135

    @State private var image: NSImage?

    var body: some View {
        ZStack {
            Rectangle().fill(.quaternary)
            if let image {
                Image(nsImage: image)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
            } else {
                Image(systemName: "film")
                    .font(.title2)
                    .foregroundStyle(.tertiary)
            }
        }
        .frame(height: height)
        .clipped()
        .task(id: path) { await load() }
    }

    private func load() async {
        guard let path else { return }
        if let hit = ThumbnailCache.shared.cached(path) {
            image = hit
            return
        }
        let loaded = await withCheckedContinuation { (c: CheckedContinuation<NSImage?, Never>) in
            DispatchQueue.global(qos: .userInitiated).async {
                c.resume(returning: ThumbnailCache.shared.load(path))
            }
        }
        image = loaded
    }
}

// MARK: - Opening things elsewhere

/* What "open this in something else" means on macOS.
 *
 * IINA and VLC are asked for by bundle identifier rather than found on PATH,
 * because that is how a macOS application is installed -- an .app in
 * /Applications with nothing on PATH at all. mpv is the exception and is looked
 * up on PATH, since Homebrew installs it as a plain executable. */
enum ExternalOpen {
    private static let playerBundleIDs = [
        "com.colliderli.iina",      // IINA
        "org.videolan.vlc",         // VLC
        "io.mpv",                   // mpv.app
    ]

    /// True when at least one player that reads the pipeline's containers is
    /// installed, so the button can say what it will actually do.
    static func availablePlayerName() -> String? {
        for id in playerBundleIDs {
            if NSWorkspace.shared.urlForApplication(withBundleIdentifier: id) != nil {
                switch id {
                case "com.colliderli.iina": return "IINA"
                case "org.videolan.vlc": return "VLC"
                default: return "mpv"
                }
            }
        }
        return Paths.which("mpv") != nil ? "mpv" : nil
    }

    /// Open a media file in a player that can actually decode it, falling back
    /// to whatever the system would use.
    static func inPlayer(_ path: String) {
        let url = URL(fileURLWithPath: path)

        for id in playerBundleIDs {
            if let app = NSWorkspace.shared.urlForApplication(withBundleIdentifier: id) {
                NSWorkspace.shared.open(
                    [url], withApplicationAt: app,
                    configuration: NSWorkspace.OpenConfiguration()
                )
                return
            }
        }
        if let mpv = Paths.which("mpv") {
            let p = Process()
            p.executableURL = URL(fileURLWithPath: mpv)
            p.arguments = [path]
            try? p.run()
            return
        }
        NSWorkspace.shared.open(url)
    }

    /// Reveal in the Finder rather than opening the folder, which is what a
    /// macOS user means by "show me where this is".
    static func revealInFinder(_ path: String) {
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
    }

    static func openFolder(_ path: String) {
        NSWorkspace.shared.open(URL(fileURLWithPath: path))
    }
}
