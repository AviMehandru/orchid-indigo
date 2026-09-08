/* Turning numbers into the strings the window shows.
 *
 * Kept in one place and out of the views, because several of these have a
 * reason to exist that a call site would not carry with it.
 *
 * EVERY ONE OF THESE IS CULTURE-INVARIANT, on purpose, and that is a bigger
 * decision here than it was in either of the other two apps. .NET formats
 * numbers and dates against CultureInfo.CurrentCulture by default, and the
 * culture a process gets is the machine's, not necessarily the one its user
 * reads -- and it changes what a decimal point is. The GTK app hit the same
 * class of bug from the other side: printf's "%'" grouping flag does nothing
 * under the C locale, which is what a desktop launcher gets, so a view count
 * came out as 24913882. Formatting explicitly keeps the answer the same
 * everywhere, and keeps the tests' expectations true on a machine set to any
 * locale.
 */

using System;
using System.Globalization;

namespace YtdlWin.Core;

public static class Format
{
    private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    /// "9:47", or "1:02:03" past an hour. Durations ROUND.
    public static string Clock(double seconds)
    {
        var total = (int)Math.Round(Math.Max(0, seconds), MidpointRounding.AwayFromZero);
        var h = total / 3600;
        var m = (total % 3600) / 60;
        var s = total % 60;
        return h > 0
            ? string.Format(Inv, "{0}:{1:00}:{2:00}", h, m, s)
            : string.Format(Inv, "{0}:{1:00}", m, s);
    }

    /* A position in a file, TRUNCATED rather than rounded.
     *
     * A chapter that starts at 90.7s is at 1:30, not 1:31: rounding a timestamp
     * up names a moment the thing has not started yet, which is wrong in the
     * one way a seek point can be wrong. Durations round -- a 90.7-second video
     * is a 1:31 video -- so these are two functions rather than one. */
    public static string Timecode(double seconds)
    {
        var total = (int)Math.Max(0, seconds);
        var h = total / 3600;
        var m = (total % 3600) / 60;
        var s = total % 60;
        return h > 0
            ? string.Format(Inv, "{0}:{1:00}:{2:00}", h, m, s)
            : string.Format(Inv, "{0}:{1:00}", m, s);
    }

    /// Empty for a video whose duration is unknown, so the badge can be hidden
    /// rather than drawn empty -- an empty badge is a black smudge in the corner
    /// of a thumbnail.
    public static string Duration(double seconds) => seconds <= 0 ? "" : Clock(seconds);

    /* "20240131" -> "31 Jan 2024". Left as-is if it is not the documented
     * shape, because the folder-name fallback can produce anything. */
    private static readonly string[] Months =
        { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

    public static string UploadDate(string? yyyymmdd)
    {
        if (string.IsNullOrEmpty(yyyymmdd)) return "";
        var s = yyyymmdd;
        if (s.Length != 8) return s;
        foreach (var c in s)
        {
            if (c is < '0' or > '9') return s;
        }

        var month = int.Parse(s.AsSpan(4, 2), NumberStyles.Integer, Inv);
        if (month is < 1 or > 12) return s;
        return $"{s[6]}{s[7]} {Months[month - 1]} {s[..4]}";
    }

    /* Thousands separators, done by hand.
     *
     * n.ToString("N0") would do this and is deliberately not used: it is
     * culture-dependent, so the same count reads "24,913,882" on one machine
     * and "24.913.882" or "24 913 882" on another. A view count is a count, not
     * a localised measurement. */
    public static string Count(long n)
    {
        var negative = n < 0;
        // long.MinValue has no positive counterpart, so the magnitude is taken
        // as an unsigned value rather than by negating.
        var digits = ((ulong)Math.Abs((decimal)n)).ToString(Inv);

        var sb = new System.Text.StringBuilder(digits.Length + digits.Length / 3 + 1);
        if (negative) sb.Append('-');
        for (var i = 0; i < digits.Length; i++)
        {
            if (i > 0 && (digits.Length - i) % 3 == 0) sb.Append(',');
            sb.Append(digits[i]);
        }
        return sb.ToString();
    }

    /* Sizes in BINARY units, which is what Windows itself shows.
     *
     * Explorer's "Size" column divides by 1024 and calls the result KB, so a
     * file this app calls "412 MB" is the "412 MB" in a Properties dialog. That
     * matches the platform at the cost of disagreeing with the GTK app, which
     * shows the same file as "432 MB" because g_format_size is SI -- and the
     * SwiftUI app, which matches the Finder. All three are right about their own
     * platform, and matching the platform is worth more than matching each
     * other for a number whose only job is to be recognised. */
    private static readonly string[] Units = { "bytes", "KB", "MB", "GB", "TB", "PB" };

    public static string Bytes(long n)
    {
        if (n < 0) n = 0;
        if (n < 1024) return string.Format(Inv, "{0} {1}", n, n == 1 ? "byte" : "bytes");

        double value = n;
        var unit = 0;
        while (value >= 1024 && unit < Units.Length - 1)
        {
            value /= 1024;
            unit++;
        }
        // One decimal below 10 and none above, which is what keeps a column of
        // these the same width without losing the difference between 1.2 and
        // 1.9 GB.
        return value < 10
            ? string.Format(Inv, "{0:0.0} {1}", value, Units[unit])
            : string.Format(Inv, "{0:0} {1}", value, Units[unit]);
    }

    /// "Sep 7, 14:03" -- for a queue or history row, where the year is noise.
    public static string When(long unixSeconds)
    {
        if (unixSeconds <= 0) return "";
        var local = DateTimeOffset.FromUnixTimeSeconds(unixSeconds).ToLocalTime();
        return local.ToString("MMM d, HH:mm", Inv);
    }

    /// "2026-09-07 14:03" -- for a file's modification time, where the date is
    /// the point.
    public static string Timestamp(DateTimeOffset? when)
        => when is null ? "—" : when.Value.ToLocalTime().ToString("yyyy-MM-dd HH:mm", Inv);

    public static long NowUnix() => DateTimeOffset.UtcNow.ToUnixTimeSeconds();
}
