/* Named option sets.
 *
 * A profile is a RunOptions with the URL removed. That is the whole design, and
 * it is deliberate: profiles are NOT a second description of what this window
 * can do. Every field the runner accepts becomes profileable the moment it
 * exists, and a field added to RunOptions cannot be forgotten here, because
 * there is no per-field list in this file to forget it from -- the
 * serialisation is RunOptions.toJSON, shared with the queue.
 *
 * What that costs is that an old profiles.json may not name a field a newer
 * build added. Every field is optional on read, so a profile written before an
 * option existed keeps working and simply does not set that option.
 *
 * THE URL IS DROPPED, not stored empty at the caller's discretion. A profile
 * carrying a URL would turn "select a profile" into "select a profile and
 * silently replace what I was about to download", which is the one thing a
 * preset must never do. It is dropped on the way IN as well as on the way out,
 * so a profiles.json edited by hand cannot hijack a download.
 *
 * Stored beside settings.json and written through a temp file and a rename:
 * losing a set of profiles built up over months is losing real work, unlike an
 * index one rescan rebuilds.
 */

import Combine
import Foundation

let profileMaxNameLength = 60
let profileMaxCount = 100

struct Profile: Identifiable, Equatable {
    var name: String
    var opts: RunOptions
    /// Unix seconds, for "last saved" in the UI.
    var saved: Int64

    var id: String { name.lowercased() }
}

final class ProfileStore: ObservableObject {
    /// The profile selected when the window last closed, restored at startup.
    /// nil means "no profile", which is a real state -- it is what the window is
    /// in before anything has been saved.
    @Published private(set) var active: String?
    /// In creation order. Appended rather than sorted: a menu that reshuffles
    /// itself under the pointer is worse than one in an arbitrary but stable
    /// order.
    @Published private(set) var profiles: [Profile] = []

    enum ProfileError: LocalizedError {
        case emptyName
        case nameTooLong(Int)
        case tooMany(Int)
        case notFound(String)
        case exists(String)
        case writeFailed(String)

        var errorDescription: String? {
            switch self {
            case .emptyName:
                return "A profile needs a name."
            case .nameTooLong(let max):
                return "Profile names are limited to \(max) characters."
            case .tooMany(let max):
                return "That would be more than \(max) profiles. Delete one first."
            case .notFound(let name):
                return "There is no profile called “\(name)”."
            case .exists(let name):
                return "There is already a profile called “\(name)”."
            case .writeFailed(let path):
                return "Could not write \(path)."
            }
        }
    }

    private static func path() -> String {
        Paths.join(Paths.stateDir(), "profiles.json")
    }

    // MARK: - Loading

    static func load() -> ProfileStore {
        let store = ProfileStore()
        guard let obj = JSONFile.object(at: path()) else { return store }

        store.active = obj.str("active")

        for po in obj.objects("profiles") {
            guard let name = po.str("name") else { continue }
            var opts = RunOptions.fromJSON(po.object("opts"))
            /* A stored URL is dropped on read as well as on write. A
             * profiles.json hand-edited to carry one must not be able to hijack
             * a download. */
            opts.url = ""
            store.profiles.append(Profile(name: name, opts: opts, saved: po.int("saved")))
        }

        /* An active name that no longer matches anything is cleared rather than
         * left pointing at nothing. */
        if let a = store.active, store.position(of: a) == nil {
            store.active = nil
        }
        return store
    }

    // MARK: - Lookup

    /// Case-insensitive, so "Archival" and "archival" are one profile rather
    /// than two indistinguishable rows in a menu.
    private func position(of name: String?) -> Int? {
        guard let name else { return nil }
        return profiles.firstIndex { $0.name.caseInsensitiveCompare(name) == .orderedSame }
    }

    func profile(named name: String?) -> Profile? {
        guard let i = position(of: name) else { return nil }
        return profiles[i]
    }

    // MARK: - Operations

    private func cleanName(_ name: String) throws -> String {
        let n = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if n.isEmpty { throw ProfileError.emptyName }
        if n.count > profileMaxNameLength {
            throw ProfileError.nameTooLong(profileMaxNameLength)
        }
        return n
    }

    /// Create or overwrite by name, and make it active. The URL is cleared from
    /// `opts` before storing; the caller's own options are untouched, because
    /// saving a profile must not clear the URL box the user is still working in.
    func save(name: String, opts: RunOptions) throws {
        let clean = try cleanName(name)

        var copy = opts
        copy.url = ""

        if let i = position(of: clean) {
            profiles[i].opts = copy
            profiles[i].saved = Int64(Date().timeIntervalSince1970)
            /* Keep the name as newly typed, so re-saving "Archival" over
             * "archival" fixes the capitalisation rather than ignoring it. */
            profiles[i].name = clean
        } else {
            if profiles.count >= profileMaxCount {
                throw ProfileError.tooMany(profileMaxCount)
            }
            profiles.append(Profile(
                name: clean, opts: copy, saved: Int64(Date().timeIntervalSince1970)
            ))
        }

        active = clean
        try write()
    }

    func delete(name: String) throws {
        guard let i = position(of: name) else { throw ProfileError.notFound(name) }
        let wasActive = active?.caseInsensitiveCompare(profiles[i].name) == .orderedSame
        profiles.remove(at: i)
        /* Left pointing at a name that no longer exists, the menu would show a
         * selection that cannot be applied. */
        if wasActive { active = nil }
        try write()
    }

    func rename(from: String, to: String) throws {
        let clean = try cleanName(to)
        guard let i = position(of: from) else { throw ProfileError.notFound(from) }

        /* Renaming onto an existing name collides -- unless it is this same
         * profile being re-capitalised, which is a rename people actually do. */
        if let j = position(of: clean), j != i { throw ProfileError.exists(clean) }

        let wasActive = active?.caseInsensitiveCompare(profiles[i].name) == .orderedSame
        profiles[i].name = clean
        if wasActive { active = clean }
        try write()
    }

    /// nil clears the selection. A name that no longer exists is an error rather
    /// than a silent no-op, because the only way to reach it is a stale window.
    func activate(_ name: String?) throws {
        guard let name else {
            active = nil
            try write()
            return
        }
        guard let p = profile(named: name) else { throw ProfileError.notFound(name) }
        active = p.name
        try write()
    }

    // MARK: - Writing

    private func write() throws {
        let obj: [String: Any] = [
            "active": active as Any? ?? NSNull(),
            "profiles": profiles.map { p -> [String: Any] in
                ["name": p.name, "saved": p.saved, "opts": p.opts.toJSON()]
            },
        ]
        guard AtomicFile.write(JSONFile.data(from: obj), to: ProfileStore.path()) else {
            throw ProfileError.writeFailed(ProfileStore.path())
        }
    }
}
