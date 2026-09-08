/* The repository's CLI_VERSION pin.
 *
 * Plain KEY=VALUE, deliberately, so bash, PowerShell, a C test, a Swift test
 * and this can all read it without a library. Parsing it here rather than in
 * the test file is what lets the Health pane show the same numbers the
 * conformance suite asserts on.
 *
 * REQUIRES_ARCHIVE_LAYOUT and Archive.SupportedArchiveLayout are two copies of
 * one fact. The pin is what an installer and a human read; the constant is what
 * actually decides, per video, whether a folder is rendered or flagged. Them
 * disagreeing is how an app ends up claiming to read a layout it does not, so
 * the suite asserts they match.
 */

using System;
using System.Collections.Generic;
using System.Globalization;

namespace YtdlWin.Core;

public sealed class CliVersion
{
    public Dictionary<string, string> Values { get; } = new(StringComparer.Ordinal);

    public string? CliRepo => Values.TryGetValue("CLI_REPO", out var v) ? v : null;
    public string? CliRef => Values.TryGetValue("CLI_REF", out var v) ? v : null;

    public long? RequiresArchiveLayout
    {
        get
        {
            if (!Values.TryGetValue("REQUIRES_ARCHIVE_LAYOUT", out var raw)) return null;
            return long.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out var n)
                ? n : null;
        }
    }

    public static CliVersion Parse(string text)
    {
        var v = new CliVersion();
        // Split on '\n' after normalising, so a file checked out with CRLF
        // endings -- which on Windows is the default -- does not leave a '\r'
        // on the end of every value. "main\r" is not "main", and a pin that
        // compares unequal to itself is a bug that only appears on this
        // platform.
        var normalised = text.Replace("\r\n", "\n").Replace('\r', '\n');
        foreach (var rawLine in normalised.Split('\n'))
        {
            var line = rawLine.Trim();
            if (line.Length == 0 || line.StartsWith('#')) continue;

            var eq = line.IndexOf('=');
            if (eq < 0) continue;

            var key = line[..eq].Trim();
            var value = line[(eq + 1)..].Trim();
            if (key.Length > 0) v.Values[key] = value;
        }
        return v;
    }

    public static CliVersion? Load(string path)
    {
        var text = Paths.ReadAllTextOrNull(path);
        return text is null ? null : Parse(text);
    }
}
