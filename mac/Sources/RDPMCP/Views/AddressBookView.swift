import SwiftUI

struct AddressBookView: View {
    @EnvironmentObject private var book: AddressBook
    @EnvironmentObject private var manager: SessionManager
    @Binding var editing: HostEntry?
    @State private var selection: UUID?
    @State private var quickHost = ""
    @State private var deleting: HostEntry?

    var body: some View {
        List(selection: $selection) {
            Section("Quick Connect") {
                TextField("host or IP address", text: $quickHost)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit(quickConnect)
            }
            ForEach(book.groups, id: \.self) { group in
                Section(group.isEmpty ? String(localized: "Other") : group) {
                    ForEach(book.entries(in: group)) { entry in
                        HostRow(entry: entry, session: manager.session(for: entry))
                            .tag(entry.id)
                    }
                }
            }
        }
        .listStyle(.sidebar)
        .contextMenu(forSelectionType: UUID.self) { ids in
            if let id = ids.first, let entry = book.entry(id: id) {
                Button("Connect") { Task { await manager.connect(entry) } }
                Button("Edit…") { editing = entry }
                Button("Duplicate") {
                    var copy = entry
                    copy.id = UUID()
                    copy.name = String(localized: "\(entry.displayName) (copy)")
                    copy.hasSavedPassword = false
                    copy.requireTouchID = false
                    book.upsert(copy)
                }
                Divider()
                Button("Delete…", role: .destructive) { deleting = entry }
            }
        } primaryAction: { ids in
            if let id = ids.first, let entry = book.entry(id: id) {
                Task { await manager.connect(entry) }
            }
        }
        .toolbar {
            ToolbarItem {
                Button {
                    editing = HostEntry()
                } label: {
                    Label("New Host", systemImage: "plus")
                }
                .help("Add a new host (⌥⌘N)")
            }
            ToolbarItem {
                SettingsLink {
                    Label("Settings", systemImage: "gearshape")
                }
                .help("Settings, including the AI safety rules (⌘,)")
            }
        }
        .confirmationDialog("Delete \(deleting?.displayName ?? "")?",
                            isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } })) {
            Button("Delete", role: .destructive) {
                if let entry = deleting { book.delete(entry) }
                deleting = nil
            }
        } message: {
            Text("A saved password is also removed from the keychain.")
        }
    }

    private func quickConnect() {
        let text = quickHost.trimmingCharacters(in: .whitespaces)
        guard !text.isEmpty else { return }
        var entry = HostEntry()
        let parts = text.split(separator: ":")
        entry.host = String(parts[0])
        if parts.count == 2, let port = Int(parts[1]) { entry.port = port }
        quickHost = ""
        Task { await manager.connect(entry) }
    }
}

private struct HostRow: View {
    let entry: HostEntry
    let session: RDPSession?

    var body: some View {
        HStack {
            Image(systemName: "desktopcomputer")
                .foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 1) {
                Text(entry.displayName)
                if !entry.username.isEmpty || entry.displayName != entry.host {
                    Text(subtitle)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            Spacer()
            if entry.hasSavedPassword {
                Image(systemName: entry.requireTouchID ? "touchid" : "key.fill")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .help(entry.requireTouchID ? Text("Password saved, Touch ID required") : Text("Password saved"))
            }
            if let session {
                StatusDot(status: session.status)
            }
        }
    }

    private var subtitle: String {
        let user = entry.domain.isEmpty ? entry.username : "\(entry.domain)\\\(entry.username)"
        let host = entry.displayName != entry.host ? entry.host : ""
        return [user, host].filter { !$0.isEmpty }.joined(separator: " · ")
    }
}
