/* Building the fabrications every other test file works against.
 *
 * NOTHING IN THIS SUITE TOUCHES YOUTUBE, and nothing touches the machine's real
 * archive, install or preferences. YouTube is unreachable from the environments
 * this project is developed in and no real download has ever been started
 * through any window in it, so every test here runs against something built on
 * disk a moment earlier: a fixture tree in the documented archive shape, a fake
 * ytdl.ps1 that emits the SHAPE of real output, a .vtt written by hand.
 *
 * Anything that would write to ~/Library goes through Paths.environmentOverride
 * with HOME pointed at a temp directory, so running the suite cannot clobber the
 * profiles or queue of whoever runs it.
 */

import Foundation
import XCTest
@testable import YtdlMac

private final class BundleMarker {}

enum FixtureSupport {
    static func makeTempDir(prefix: String) throws -> String {
        let path = Paths.join(
            NSTemporaryDirectory(), "\(prefix)-\(UUID().uuidString)"
        )
        try FileManager.default.createDirectory(
            atPath: path, withIntermediateDirectories: true
        )
        return path
    }

    static func mkdirp(_ path: String) throws {
        try FileManager.default.createDirectory(
            atPath: path, withIntermediateDirectories: true
        )
    }

    static func write(_ contents: String, to path: String) throws {
        try mkdirp((path as NSString).deletingLastPathComponent)
        try contents.write(toFile: path, atomically: true, encoding: .utf8)
    }

    /* A file that belongs to the REPOSITORY rather than to this app -- today
     * that means CLI_VERSION, which all three apps assert against.
     *
     * The test target carries it as a resource, which is what makes this work
     * from a build anywhere. The walk up from #filePath is the fallback for a
     * project edited by hand: #filePath is this source file's path on the
     * machine that compiled it, so four levels up from Tests/ is the repo root.
     * If both fail the test says so rather than silently passing. */
    static func repositoryFile(named name: String) -> String? {
        if let url = Bundle(for: BundleMarker.self).url(forResource: name, withExtension: nil) {
            return url.path
        }

        var dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        for _ in 0..<6 {
            let candidate = dir.appendingPathComponent(name)
            if FileManager.default.fileExists(atPath: candidate.path) {
                return candidate.path
            }
            dir = dir.deletingLastPathComponent()
        }
        return nil
    }
}
