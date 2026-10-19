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

                Section {
                    TextField("Gebruikersnaam", text: $entry.username)
                    TextField("Domein", text: $entry.domain, prompt: Text("bv. VOORBEELD"))
                    Toggle("Wachtwoord bewaren in de sleutelhanger", isOn: $savePassword)
                        .onChange(of: savePassword) { _, on in
                            if !on { password = "" }
                        }
                    if savePassword {
                        SecureField("Wachtwoord", text: $password,
                                    prompt: Text(entry.hasSavedPassword ? "•••••••• (bewaard; leeg laten om te behouden)"
                                                                        : "wachtwoord"))
                        Toggle("Touch ID vragen voor het bewaarde wachtwoord", isOn: $entry.requireTouchID)
                    }
                } header: {
                    Text("Aanmelden")
                } footer: {
                    Text(passwordFooter)
                        .font(.caption)
                        .foregroundStyle(missingPassword ? .red : .secondary)
                }

                Section {
                    Toggle("Geluid op de Mac afspelen", isOn: $entry.sound)
                    LabeledContent("Gedeelde map") {
                        HStack {
                            Text(entry.sharedFolderPath.isEmpty ? "geen"
                                 : (entry.sharedFolderPath as NSString).abbreviatingWithTildeInPath)
                                .foregroundStyle(entry.sharedFolderPath.isEmpty ? .secondary : .primary)
                                .lineLimit(1)
                                .truncationMode(.middle)
                            Button("Kies…", action: chooseFolder)
                            if !entry.sharedFolderPath.isEmpty {
                                Button("Geen") { entry.sharedFolderPath = "" }
                            }
                        }
                    }
                    if !entry.sharedFolderPath.isEmpty {
                        TextField("Naam in Windows", text: $entry.sharedFolderName, prompt: Text("Mac"))
                    }
                } header: {
                    Text("Delen")
                } footer: {
                    if !entry.sharedFolderPath.isEmpty {
                        Text("De map verschijnt in Windows onder Deze pc. Let op: met AI-besturing aan kan de AI via de Verkenner ook bij de bestanden in deze map.")
                            .font(.caption)
                            .foregroundStyle(.orange)
                    }
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
                    .disabled(entry.host.trimmingCharacters(in: .whitespaces).isEmpty || missingPassword)
            }
            .padding()
        }
        .frame(width: 500, height: 720)
    }

    private func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Delen"
        panel.message = "Kies de map die in Windows als schijf verschijnt"
        if panel.runModal() == .OK, let url = panel.url {
            entry.sharedFolderPath = url.path
        }
    }

    /// Bewaren staat aan, maar er is nog geen wachtwoord bewaard of ingevuld
    private var missingPassword: Bool {
        savePassword && !entry.hasSavedPassword && password.isEmpty
    }

    private var passwordFooter: String {
        if !savePassword {
            return entry.hasSavedPassword
                ? "Het bewaarde wachtwoord wordt uit de sleutelhanger verwijderd. Bij elke verbinding wordt het gevraagd."
                : "Het wachtwoord wordt bij elke verbinding gevraagd. Daar kun je het alsnog laten bewaren."
        }
        if missingPassword { return "Vul het wachtwoord in dat bewaard moet worden." }
        return "Het wachtwoord staat versleuteld in je login-sleutelhanger, nooit in het adresboek."
    }

    private func save() {
        entry.host = entry.host.trimmingCharacters(in: .whitespaces)
        // de komma scheidt naam en pad in de FreeRDP-optie
        entry.sharedFolderName = entry.sharedFolderName.replacingOccurrences(of: ",", with: " ")
        if entry.sharedFolderName.trimmingCharacters(in: .whitespaces).isEmpty { entry.sharedFolderName = "Mac" }
        if entry.sharedFolderPath.contains(",") {
            error = "Het pad van de gedeelde map mag geen komma bevatten."
            return
        }
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
