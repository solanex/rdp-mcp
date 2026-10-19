import Foundation

/// Adresboek in ~/Library/Application Support/rdp-mcp/hosts.json (zonder wachtwoorden).
final class AddressBook: ObservableObject {
    @Published private(set) var entries: [HostEntry] = []
    @Published var loadError: String?

    private let url: URL = {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("rdp-mcp", isDirectory: true)
            .appendingPathComponent("hosts.json")
    }()

    init() {
        load()
    }

    var groups: [String] {
        Array(Set(entries.map(\.group))).sorted { a, b in
            if a.isEmpty != b.isEmpty { return !a.isEmpty } // naamloze groep onderaan
            return a.localizedCaseInsensitiveCompare(b) == .orderedAscending
        }
    }

    func entries(in group: String) -> [HostEntry] {
        entries.filter { $0.group == group }
            .sorted { $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending }
    }

    func entry(id: UUID) -> HostEntry? {
        entries.first { $0.id == id }
    }

    func upsert(_ entry: HostEntry) {
        if let i = entries.firstIndex(where: { $0.id == entry.id }) {
            entries[i] = entry
        } else {
            entries.append(entry)
        }
        save()
    }

    func delete(_ entry: HostEntry) {
        entries.removeAll { $0.id == entry.id }
        Keychain.delete(account: entry.id)
        save()
    }

    private func load() {
        guard FileManager.default.fileExists(atPath: url.path) else { return }
        do {
            entries = try JSONDecoder().decode([HostEntry].self, from: Data(contentsOf: url))
        } catch {
            // reservekopie, zodat een volgende bewaring het origineel niet overschrijft
            let stamp = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")
            let backup = url.deletingPathExtension().appendingPathExtension("onleesbaar-\(stamp).json")
            try? FileManager.default.copyItem(at: url, to: backup)
            loadError = "Adresboek kon niet gelezen worden (\(error.localizedDescription)). "
                + "Een kopie staat in \(backup.lastPathComponent)."
        }
    }

    private func save() {
        do {
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
            try encoder.encode(entries).write(to: url, options: .atomic)
        } catch {
            loadError = "Adresboek kon niet bewaard worden: \(error.localizedDescription)"
        }
    }
}
