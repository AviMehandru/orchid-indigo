/* Which videos the Library shows, and in what order.
 *
 * The Swift half of a rule set three independent apps have to agree on. The
 * C version is linux-gtk/src/library_filter.{c,h} and the C# one is
 * windows-winui/YtdlWin/Core/LibraryFilter.cs; all three are asserted against
 * the SAME fixture values, which is the same mitigation the probe's
 * derivation uses and the only thing that makes three blind implementations
 * of one contract survivable.
 *
 * NOTHING HERE READS THE DISK. The index is already in memory, every field
 * this consults was populated by the scan, and `totalSize` sums the sizes the
 * scan recorded. That is what makes it safe to re-run the whole filter on
 * every keystroke.
 *
 * WHAT THE THREE APPS AGREE ON, AND WHAT THEY DO NOT. The rules are the
 * contract: which videos a facet admits, that an empty channel set means all
 * of them, that an unparseable date is excluded by a range rather than kept,
 * that a missing key sorts last in BOTH directions, and that the order is
 * total. The collation of two present titles is NOT: this uses the system's
 * localized comparison, the GTK app uses GLib's, and the C# app uses .NET's.
 * That is deliberate rather than an oversight -- a German user's Library
 * should sort the way the rest of their desktop sorts, and three apps forced
 * to agree byte-for-byte would all have to be wrong somewhere to do it. So
 * the shared fixtures assert the RULES on inputs whose ordering every
 * reasonable collation agrees on, and never assert a specific collation.
 */

import Foundation

// MARK: - Sort keys

/// The sort keys, in the order they are offered.
///
/// `.date` first because it is what the grid was already implicitly ordered by
/// and what someone opening the app expects. Every key falls back to title,
/// then to the archive-relative path, so the order is TOTAL: two videos with
/// the same duration do not swap places between one rebuild and the next,
/// which is the kind of flicker that reads as a bug.
///
/// The raw value is the STABLE ID the setting is persisted as. Persisting the
/// case's position would mean inserting a key in the middle silently changes
/// what a saved setting means.
enum SortKey: String, CaseIterable, Identifiable {
    case date = "date"
    case title = "title"
    case channel = "channel"
    case duration = "duration"
    case size = "size"

    var id: String { rawValue }

    var label: String {
        switch self {
        case .date: return "Upload date"
        case .title: return "Title"
        case .channel: return "Channel"
        case .duration: return "Duration"
        case .size: return "Size"
        }
    }

    /// A key written by a newer build falls back rather than refusing -- the
    /// same rule the profile store uses for an option it has never heard of.
    static func from(id: String?) -> SortKey {
        guard let id, let k = SortKey(rawValue: id) else { return .date }
        return k
    }
}

// MARK: - Facets

/// The flag facets.
///
/// All of them are AND-ed: asking for "audio only" and "no media file"
/// together is asking for something no folder can be, and it correctly shows
/// nothing rather than quietly becoming an OR.
///
/// `.verifyFailed` is different in kind from the other three and the
/// difference is worth stating, because it is the one a user can misread. The
/// other three are properties of the manifest and are known for every video
/// the moment it is indexed. Whether a folder still verifies is only knowable
/// by hashing every file in it, which takes seconds per video -- so this facet
/// filters on RECORDED results only, and a video that has never been verified
/// is not shown by it. It never claims an unverified video passes; it says "of
/// the ones I have checked, these failed". The UI has to say so too.
struct FacetFlags: OptionSet, Hashable {
    let rawValue: Int

    static let audioOnly = FacetFlags(rawValue: 1 << 0)
    static let noMedia = FacetFlags(rawValue: 1 << 1)
    static let layoutTooNew = FacetFlags(rawValue: 1 << 2)
    static let verifyFailed = FacetFlags(rawValue: 1 << 3)

    static let all: [FacetFlags] = [.audioOnly, .noMedia, .layoutTooNew, .verifyFailed]

    var label: String {
        switch self {
        case .audioOnly: return "Audio only"
        case .noMedia: return "No media file"
        case .layoutTooNew: return "Newer archive layout"
        case .verifyFailed: return "Failed verification"
        default: return ""
        }
    }
}

/// What is known about a video's last checksum verification.
enum VerifyState {
    /// Never checked, or checked before the folder changed.
    case unknown
    case ok
    case failed
}

// MARK: - The filter

struct LibraryFilter: Equatable {
    /// The substring search: title, uploader, id, channel, case-folded. Empty
    /// matches everything.
    var needle: String = ""

    /// Selected uploader folder names. EMPTY MEANS ALL, not none -- a facet
    /// nobody has touched must not hide the whole library.
    var channels: Set<String> = []

    /// "YYYYMMDD" bounds, inclusive, either may be nil. Compared as strings,
    /// which is correct for this format and is also why a folder whose
    /// uploadDate came from the folder-name fallback still sorts and filters
    /// sensibly: the fallback produces the same eight digits. An uploadDate
    /// that is not eight digits is EXCLUDED by any date bound rather than
    /// silently kept -- it is a video whose date is unknown, and claiming it
    /// falls inside a range would be inventing one.
    var dateFrom: String?
    var dateTo: String?

    var flags: FacetFlags = []

    var sort: SortKey = .date
    /// Newest first. The only default that is not a coin toss: an archive is
    /// added to at the newest end, so what someone wants to see when the
    /// window opens is what arrived last.
    var descending: Bool = true

    // MARK: Narrowing

    /// Drop every facet and the needle. The SORT is deliberately left alone:
    /// it is a view preference, not a filter, and "Clear filters" throwing
    /// away someone's chosen ordering would be a surprise.
    mutating func reset() {
        needle = ""
        channels = []
        dateFrom = nil
        dateTo = nil
        flags = []
    }

    /// How many facets are set, for the count beside the filter control. The
    /// needle is not counted -- it has its own visible search field.
    var facetCount: Int {
        var n = 0
        if !channels.isEmpty { n += 1 }
        /* One date facet, not two: "2024 only" is a single idea the user had,
         * and counting it twice makes the count read as more narrowing than
         * it is. */
        if dateFrom != nil || dateTo != nil { n += 1 }
        for f in FacetFlags.all where flags.contains(f) { n += 1 }
        return n
    }

    /// True when anything at all is narrowing the library. Drives the
    /// "filters active" indicator, so it must NOT count the sort.
    var isNarrowing: Bool {
        !needle.isEmpty || facetCount > 0
    }

    mutating func setChannel(_ channel: String, on: Bool) {
        if on { channels.insert(channel) } else { channels.remove(channel) }
    }

    // MARK: Matching

    /// The eight-digit form the layout contract specifies, and the same form
    /// the folder-name fallback produces. Anything else is a date this reader
    /// does not have, which is not the same as a date outside the range.
    static func isYYYYMMDD(_ s: String?) -> Bool {
        guard let s, s.count == 8 else { return false }
        return s.allSatisfy { $0.isASCII && $0.isNumber }
    }

    private func matchesNeedle(_ e: ArchiveEntry) -> Bool {
        if needle.isEmpty { return true }
        let want = needle.lowercased()
        for field in [e.title, e.uploader, e.videoID ?? "", e.channel]
        where !field.isEmpty {
            if field.lowercased().contains(want) { return true }
        }
        return false
    }

    /// One entry against the filter. `verify` may be nil, in which case every
    /// video reads as `.unknown` and the verify facet matches nothing.
    func matches(_ e: ArchiveEntry, verify: ((ArchiveEntry) -> VerifyState)? = nil) -> Bool {
        guard matchesNeedle(e) else { return false }

        /* Empty set means "every channel". The alternative -- empty means
         * none -- would make the library go blank the instant someone opened
         * the facet list and unticked the one channel they had ticked. */
        if !channels.isEmpty, !channels.contains(e.channel) { return false }

        if dateFrom != nil || dateTo != nil {
            guard LibraryFilter.isYYYYMMDD(e.uploadDate), let d = e.uploadDate else {
                return false
            }
            if let from = dateFrom, d < from { return false }
            if let to = dateTo, d > to { return false }
        }

        if flags.contains(.audioOnly), !e.isAudioOnly { return false }
        if flags.contains(.noMedia), e.mediaIndex != nil { return false }
        if flags.contains(.layoutTooNew), !e.layoutTooNew { return false }

        if flags.contains(.verifyFailed) {
            /* `.unknown` is not a match. A video nobody has verified has not
             * passed and has not failed, and showing it here would turn
             * "these are broken" into "these might be broken", which is a
             * different and much less useful claim. */
            let state = verify?(e) ?? .unknown
            if state != .failed { return false }
        }

        return true
    }

    // MARK: Sorting

    private static func hasText(_ s: String?) -> Bool {
        guard let s else { return false }
        return !s.isEmpty
    }

    /// The sort field a key reads, as "is it there at all". Kept separate from
    /// the comparison because absence must NOT be reversed along with the
    /// direction -- see `compare`.
    private func keyIsPresent(_ e: ArchiveEntry) -> Bool {
        switch sort {
        case .date: return LibraryFilter.hasText(e.uploadDate)
        case .title: return LibraryFilter.hasText(e.title)
        case .channel:
            return LibraryFilter.hasText(e.uploader) || LibraryFilter.hasText(e.channel)
        case .duration:
            /* No info.json, or one with no duration, reads as <= 0. That is
             * "not known", not "a zero-second video" -- there is no such thing
             * in an archive. */
            return e.duration > 0
        case .size: return e.totalSize > 0
        }
    }

    /// Localized, not byte-ordered: "Ärger" belongs next to "Arger" in a list
    /// a person reads, and a byte comparison puts it after "Zebra".
    private static func compareText(_ a: String, _ b: String) -> Int {
        switch a.localizedStandardCompare(b) {
        case .orderedAscending: return -1
        case .orderedDescending: return 1
        case .orderedSame: return 0
        }
    }

    /// The comparison a given key implies, with `descending` already applied.
    /// Exposed because sorting is the half of this file where an error is
    /// invisible in a screenshot and obvious in an assertion.
    func compare(_ a: ArchiveEntry, _ b: ArchiveEntry) -> Int {
        /* ABSENCE IS DECIDED BEFORE THE DIRECTION FLIP, and this is the single
         * subtlest thing in the file. A video with no upload date is missing
         * information, and information that is missing belongs at the BOTTOM
         * of the list in both directions. Folding that into the comparison and
         * then negating the result for a descending sort flips it too, so
         * pressing the sort-direction control fills the first screen with
         * blank cards -- which reads as a rendering bug, not as an ordering
         * choice. Caught by a test rather than by reading this code. */
        let aHas = keyIsPresent(a)
        let bHas = keyIsPresent(b)
        if aHas != bHas { return aHas ? -1 : 1 }

        var r = 0
        if aHas {
            switch sort {
            case .date:
                /* The eight-digit string, not a parsed date: it is already
                 * lexicographically ordered, and an entry whose date came from
                 * the folder-name fallback is in the same form. */
                r = LibraryFilter.compareText(a.uploadDate ?? "", b.uploadDate ?? "")
            case .title:
                r = LibraryFilter.compareText(a.title, b.title)
            case .channel:
                r = LibraryFilter.compareText(
                    a.uploader.isEmpty ? a.channel : a.uploader,
                    b.uploader.isEmpty ? b.channel : b.uploader)
            case .duration:
                r = a.duration < b.duration ? -1 : (a.duration > b.duration ? 1 : 0)
            case .size:
                r = a.totalSize < b.totalSize ? -1 : (a.totalSize > b.totalSize ? 1 : 0)
            }
        }

        if descending { r = -r }

        /* The tie-breakers are NOT reversed with the direction, and that is
         * the point of them: they exist to make the order TOTAL so the grid
         * does not reshuffle equal-keyed videos between rebuilds. Title first
         * because it is what a person would expect to see grouped; then the
         * archive-relative path, which is unique by construction, so the
         * comparison can never return 0 for two different videos. */
        if r == 0 { r = LibraryFilter.compareText(a.title, b.title) }
        if r == 0 { r = LibraryFilter.compareText(a.rel, b.rel) }
        return r
    }

    /// Filter and sort a whole index in one pass.
    func apply(to entries: [ArchiveEntry],
               verify: ((ArchiveEntry) -> VerifyState)? = nil) -> [ArchiveEntry] {
        entries
            .filter { matches($0, verify: verify) }
            .sorted { compare($0, $1) < 0 }
    }
}
