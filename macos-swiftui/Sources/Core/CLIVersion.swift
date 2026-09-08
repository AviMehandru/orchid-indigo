/* The repository's CLI_VERSION pin.
 *
 * Plain KEY=VALUE, deliberately, so bash, PowerShell, a C test and this can all
 * read it without a library. Parsing it here rather than in the test file is
 * what lets the Health pane show the same numbers the conformance suite
 * asserts on.
 *
 * REQUIRES_ARCHIVE_LAYOUT and `supportedArchiveLayout` in Archive.swift are two
 * copies of one fact. The pin is what an installer and a human read; the
 * constant is what actually decides, per video, whether a folder is rendered or
 * flagged. Them disagreeing is how an app ends up claiming to read a layout it
 * does not, so the suite asserts they match.
 */

import Foundation

struct CLIVersion {
    var values: [String: String] = [:]

    var cliRepo: String? { values["CLI_REPO"] }
    var cliRef: String? { values["CLI_REF"] }
    var requiresArchiveLayout: Int64? {
        values["REQUIRES_ARCHIVE_LAYOUT"].flatMap { Int64($0) }
    }

    static func parse(_ text: String) -> CLIVersion {
        var v = CLIVersion()
        for rawLine in text.components(separatedBy: "\n") {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            guard let eq = line.firstIndex(of: "=") else { continue }
            let key = String(line[line.startIndex..<eq]).trimmingCharacters(in: .whitespaces)
            let value = String(line[line.index(after: eq)...]).trimmingCharacters(in: .whitespaces)
            if !key.isEmpty { v.values[key] = value }
        }
        return v
    }

    static func load(atPath path: String) -> CLIVersion? {
        guard let text = try? String(contentsOfFile: path, encoding: .utf8) else { return nil }
        return parse(text)
    }
}
