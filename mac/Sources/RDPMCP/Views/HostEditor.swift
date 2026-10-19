import SwiftUI

struct HostEditor: View {
    @EnvironmentObject private var book: AddressBook
    @Environment(\.dismiss) private var dismiss

    @State private var entry: HostEntry
    @State private var password = ""
    @State private var savePassword: Bool
    @State private var error: String?
    private let isNew: Bool

    init(entry: HostEntry) {
        _entry = State(initialValue: entry)
        _savePassword = State(initialValue: entry.hasSavedPassword)
        isNew = entry.host.isEmpty
    }

    var body: some View {
        VStack(spacing: 0) {
            Form {
                Section("Verbinding") {
                    TextField("Naam", text: $entry.name, prompt: Text("bv. Lokaal 2.14"))
                    TextField("Groep", text: $entry.group, prompt: Text("bv. School"))
                    TextField("Host", text: $entry.host, prompt: Text("server.voorbeeld.be of IP"))
                    TextField("Poort", value: $entry.port, format: .number.grouping(.never))
                    TextField("RD Gateway", text: $entry.gateway, prompt: Text("optioneel"))
                }

                Section("Aanmelden") {
                    TextField("Gebruikersnaam", text: $entry.username)
                    TextField("Domein", text: $entry.domain, prompt: Text("bv. VOORBEELD"))
                    SecureField("Wachtwoord", text: $password,
                                prompt: Text(entry.hasSavedPassword ? "•••••••• (bewaard — leeg laten om te behouden)"
                                                                    : "leeg = vragen bij verbinden"))
                    Toggle("Wachtwoord bewaren in de sleutelhanger", isOn: $savePassword)
                    Toggle("Touch ID vragen voor het bewaarde wachtwoord", isOn: $entry.requireTouchID)
                        .disabled(!savePassword)
                }

                Section("Scherm en invoer") {
                    Picker("Resolutie", selection: Binding(
                        get: { "\(entry.width)x\(entry.height)" },
                        set: { value in
                            let parts = value.split(separator: "x").compactMap { Int($0) }
                            if parts.count == 2 { entry.width = parts[0]; entry.height = parts[1] }
                        })) {
                        ForEach(HostEntry.resolutions, id: \.name) { r in
                            Text(r.name).tag("\(r.w)x\(r.h)")
                        }
                    }
                    Picker("Toetsenbord (Windows)", selection: $entry.keyboardLayout) {
                        ForEach(HostEntry.keyboardLayouts, id: \.id) { k in
                            Text(k.name).tag(k.id)
                        }
                    }
                    Toggle("Klembord delen", isOn: $entry.clipboard)
                    Toggle("AI-besturing aan bij verbinden", isOn: $entry.aiEnabledAtStart)
                    Toggle("Certificaat niet controleren", isOn: $entry.ignoreCertificate)
                }
            }
            .formStyle(.grouped)

            if let error {
                Text(error)
                    .foregroundStyle(.red)
                    .padding(.horizontal)
            }

            HStack {
                if !isNew {
                    Button("Verwijderen", role: .destructive) {
                        book.delete(entry)
                        dismiss()
                    }
                }
                Spacer()
                Button("Annuleren", role: .cancel) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button(isNew ? "Toevoegen" : "Bewaren") { save() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(entry.host.trimmingCharacters(in: .whitespaces).isEmpty)
            }
            .padding()
        }
        .frame(width: 480, height: 640)
    }

    private func save() {
        entry.host = entry.host.trimmingCharacters(in: .whitespaces)
        do {
            if savePassword {
                if !password.isEmpty {
                    try Keychain.save(password: password, account: entry.id, label: entry.displayName)
                    entry.hasSavedPassword = true
                }
            } else if entry.hasSavedPassword {
                Keychain.delete(account: entry.id)
                entry.hasSavedPassword = false
            }
            if !entry.hasSavedPassword { entry.requireTouchID = false }
        } catch {
            self.error = "Wachtwoord niet bewaard: \(error.localizedDescription)"
            return
        }
        password = ""
        book.upsert(entry)
        dismiss()
    }
}
