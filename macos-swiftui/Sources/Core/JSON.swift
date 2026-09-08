/* Tolerant readers over JSONSerialization output.
 *
 * WHY NOT CODABLE. Every JSON file this app reads was written by something
 * else -- manifest.json by postprocess.ps1, info.json by yt-dlp, and ffprobe's
 * output by ffmpeg -- and all three vary by version and by extractor. A
 * Decodable struct is a schema, and the failure mode of a schema against
 * output like that is that ONE unexpected member fails the whole document:
 * a video with an odd info.json would vanish from the library rather than
 * showing up with a missing field.
 *
 * So the shape is read member by member, every accessor tolerates a missing
 * key, a null, and a value of the wrong type, and nothing here can throw. That
 * is the same decision json-glib forced on the GTK app, arrived at for the
 * same reason rather than copied.
 *
 * ffprobe in particular reports numbers as JSON strings more often than not
 * ("bit_rate": "128000") and omits a key entirely rather than sending null
 * when it does not know. Both are normal, neither is an error.
 */

import Foundation

enum JSONFile {
    /// Parse a file into a dictionary, or nil. A malformed or absent file is an
    /// ordinary state the layout contract tells consumers to tolerate, so this
    /// reports nothing and lets the caller's fallback take over.
    static func object(at path: String) -> [String: Any]? {
        guard Paths.isRegularFile(path) else { return nil }
        guard let data = FileManager.default.contents(atPath: path) else { return nil }
        return object(from: data)
    }

    static func object(from data: Data) -> [String: Any]? {
        let parsed = try? JSONSerialization.jsonObject(with: data, options: [])
        return parsed as? [String: Any]
    }

    /// Pretty-printed, with sorted keys so a diff of two saves is readable.
    static func data(from object: Any) -> Data? {
        try? JSONSerialization.data(
            withJSONObject: object,
            options: [.prettyPrinted, .sortedKeys]
        )
    }
}

extension Dictionary where Key == String, Value == Any {
    /// A non-empty string member, or nil. An empty string is treated as absent
    /// because that is what the manifest writes for "we did not learn this".
    func str(_ key: String) -> String? {
        guard let value = self[key] else { return nil }
        guard let s = value as? String, !s.isEmpty else { return nil }
        return s
    }

    /// An integer member. Accepts a JSON number or a numeric string, because
    /// ffprobe sends both for the same field depending on the stream.
    func int(_ key: String, default fallback: Int64 = 0) -> Int64 {
        guard let value = self[key] else { return fallback }
        if let n = value as? NSNumber {
            // NSNumber also wraps JSON booleans; a bool is not a count.
            if CFGetTypeID(n) == CFBooleanGetTypeID() { return fallback }
            return n.int64Value
        }
        if let s = value as? String, let n = Int64(s) { return n }
        if let s = value as? String, let d = Double(s) { return Int64(d) }
        return fallback
    }

    func double(_ key: String, default fallback: Double = 0) -> Double {
        guard let value = self[key] else { return fallback }
        if let n = value as? NSNumber {
            if CFGetTypeID(n) == CFBooleanGetTypeID() { return fallback }
            return n.doubleValue
        }
        if let s = value as? String, let d = Double(s) { return d }
        return fallback
    }

    func bool(_ key: String) -> Bool {
        guard let value = self[key] else { return false }
        if let n = value as? NSNumber { return n.boolValue }
        if let s = value as? String { return s == "true" || s == "1" }
        return false
    }

    func object(_ key: String) -> [String: Any]? {
        self[key] as? [String: Any]
    }

    func array(_ key: String) -> [Any]? {
        self[key] as? [Any]
    }

    /// The objects of an array member, skipping anything that is not one.
    func objects(_ key: String) -> [[String: Any]] {
        guard let raw = array(key) else { return [] }
        return raw.compactMap { $0 as? [String: Any] }
    }

    /// The non-empty strings of an array member.
    func strings(_ key: String) -> [String] {
        guard let raw = array(key) else { return [] }
        return raw.compactMap { $0 as? String }.filter { !$0.isEmpty }
    }
}
