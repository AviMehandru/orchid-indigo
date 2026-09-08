/* Turning numbers into the strings the window shows.
 *
 * Kept in one place and out of the views, because two of these have a reason
 * to exist that a call site would not carry with it.
 */

import Foundation

enum Format {
    /// "9:47", or "1:02:03" past an hour.
    static func clock(_ seconds: Double) -> String {
        let total = Int(max(0, seconds).rounded())
        let h = total / 3600
        let m = (total % 3600) / 60
        let s = total % 60
        if h > 0 { return String(format: "%d:%02d:%02d", h, m, s) }
        return String(format: "%d:%02d", m, s)
    }

    /* A position in a file, TRUNCATED rather than rounded.
     *
     * A chapter that starts at 90.7s is at 1:30, not 1:31: rounding a timestamp
     * up names a moment the thing has not started yet, which is wrong in the
     * one way a seek point can be wrong. Durations round -- a 90.7-second video
     * is a 1:31 video -- so these are two functions rather than one. */
    static func timecode(_ seconds: Double) -> String {
        let total = Int(max(0, seconds))
        let h = total / 3600
        let m = (total % 3600) / 60
        let s = total % 60
        if h > 0 { return String(format: "%d:%02d:%02d", h, m, s) }
        return String(format: "%d:%02d", m, s)
    }

    /// Empty for a video whose duration is unknown, so the badge can be hidden
    /// rather than drawn empty -- an empty badge is a black smudge in the corner
    /// of a thumbnail.
    static func duration(_ seconds: Double) -> String {
        seconds <= 0 ? "" : clock(seconds)
    }

    /* "20240131" -> "31 Jan 2024". Left as-is if it is not the documented
     * shape, because the folder-name fallback can produce anything. */
    static func uploadDate(_ yyyymmdd: String?) -> String {
        guard let s = yyyymmdd else { return "" }
        guard s.count == 8, s.allSatisfy({ $0.isASCII && $0.isNumber }) else { return s }

        let months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun",
                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
        let chars = Array(s)
        guard let month = Int(String(chars[4...5])), month >= 1, month <= 12 else { return s }
        return "\(chars[6])\(chars[7]) \(months[month - 1]) \(String(chars[0...3]))"
    }

    /* Thousands separators, done by hand.
     *
     * NumberFormatter would do this, and is deliberately not used: it is
     * locale-dependent, and the locale an app gets is not always the one its
     * user reads -- an app launched by launchd, or a test run under a CI
     * environment with no locale set, formats differently from the same code in
     * a terminal. A view count is a count, not a localised measurement, and the
     * GTK app hit exactly this: printf's "%'" grouping flag does nothing under
     * the C locale, which is what a desktop launcher gets, so a view count came
     * out as 24913882. Grouping here keeps the answer the same everywhere. */
    static func count(_ n: Int64) -> String {
        let digits = String(n.magnitude)
        var out = n < 0 ? "-" : ""
        for (i, ch) in digits.enumerated() {
            if i > 0, (digits.count - i) % 3 == 0 { out.append(",") }
            out.append(ch)
        }
        return out
    }

    /* Sizes go through ByteCountFormatter: it is what the Finder uses, so
     * "412 MB" here means the same number of bytes as "412 MB" in a Get Info
     * window. Matching the platform matters more than matching the GTK app,
     * which shows the same file as "432 MB" because g_format_size is SI. */
    private static let byteFormatter: ByteCountFormatter = {
        let f = ByteCountFormatter()
        f.countStyle = .file
        f.allowsNonnumericFormatting = false
        return f
    }()

    static func bytes(_ n: UInt64) -> String {
        byteFormatter.string(fromByteCount: Int64(clamping: n))
    }

    /// "Sep 7, 14:03" -- for a queue or history row, where the year is noise.
    static func when(_ unixSeconds: Int64) -> String {
        guard unixSeconds > 0 else { return "" }
        let f = DateFormatter()
        f.dateFormat = "MMM d, HH:mm"
        return f.string(from: Date(timeIntervalSince1970: TimeInterval(unixSeconds)))
    }

    /// "2026-09-07 14:03" -- for a file's modification time, where the date is
    /// the point.
    static func timestamp(_ date: Date?) -> String {
        guard let date else { return "—" }
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm"
        return f.string(from: date)
    }
}
