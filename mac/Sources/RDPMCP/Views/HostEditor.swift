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
                Section("Connection") {
                    TextField("Name", text: $entry.name, prompt: Text("e.g. Room 2.14"))
                    TextField("Group", text: $entry.group, prompt: Text("e.g. Office"))
                    TextField("Host", text: $entry.host, prompt: Text("server.example.com or IP"))
                    TextField("Port", value: $entry.port, format: .number.grouping(.never))
                    TextField("RD Gateway", text: $entry.gateway, prompt: Text("optional"))
                }

                Section {
                    TextField("Username", text: $entry.username)
                    TextField("Domain", text: $entry.domain, prompt: Text("e.g. EXAMPLE"))
                    Toggle("Save password in the keychain", isOn: $savePassword)
                        .onChange(of: savePassword) { _, on in
                            if !on { password = "" }
                        }
                    if savePassword {
                        SecureField("Password", text: $password,
                                    prompt: entry.hasSavedPassword ? Text("•••••••• (saved; leave empty to keep)")
                                                                   : Text("password"))
                        Toggle("Require Touch ID for the saved password", isOn: $entry.requireTouchID)
                    }
                } header: {
                    Text("Sign In")
                } footer: {
                    Text(passwordFooter)
                        .font(.caption)
                        .foregroundStyle(missingPassword ? .red : .secondary)
                }

                Section {
                    Toggle("Play sound on the Mac", isOn: $entry.sound)
                    LabeledContent("Shared folder") {
                        HStack {
                            Text(entry.sharedFolderPath.isEmpty ? String(localized: "none")
                                 : (entry.sharedFolderPath as NSString).abbreviatingWithTildeInPath)
                                .foregroundStyle(entry.sharedFolderPath.isEmpty ? .secondary : .primary)
                                .lineLimit(1)
                                .truncationMode(.middle)
                            Button("Choose…", action: chooseFolder)
                            if !entry.sharedFolderPath.isEmpty {
                                Button("None") { entry.sharedFolderPath = "" }
                            }
                        }
                    }
                    if !entry.sharedFolderPath.isEmpty {
                        TextField("Name in Windows", text: $entry.sharedFolderName, prompt: Text("Mac"))
                    }
                } header: {
                    Text("Sharing")
                } footer: {
                    if !entry.sharedFolderPath.isEmpty {
                        Text("The folder shows up in Windows under This PC. Note: with AI control on, the AI can also reach the files in this folder through Explorer.")
                            .font(.caption)
                            .foregroundStyle(.orange)
                    }
                }

                Section("Display and Input") {
                    Picker("Resolution", selection: Binding(
                        get: { "\(entry.width)x\(entry.height)" },
                        set: { value in
                            let parts = value.split(separator: "x").compactMap { Int($0) }
                            if parts.count == 2 { entry.width = parts[0]; entry.height = parts[1] }
                        })) {
                        ForEach(HostEntry.resolutions, id: \.name) { r in
                            Text(r.name).tag("\(r.w)x\(r.h)")
                        }
                    }
                    Picker("Keyboard (Windows)", selection: $entry.keyboardLayout) {
                        ForEach(HostEntry.keyboardLayouts, id: \.id) { k in
                            Text(k.name).tag(k.id)
                        }
                    }
                    Toggle("Share clipboard", isOn: $entry.clipboard)
                    Toggle("AI control on when connecting", isOn: $entry.aiEnabledAtStart)
                    Toggle("Don’t verify certificate", isOn: $entry.ignoreCertificate)
                }

                Section {
                    Toggle("Ask before the AI restarts, shuts down or signs out", isOn: $entry.confirmPowerActions)
                } header: {
                    Text("AI Safety")
                } footer: {
                    Text("The app reads the text where the AI clicks. A safety net, not a guarantee: to really prevent it, sign in with an account that may not shut down Windows.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
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
                    Button("Delete", role: .destructive) {
                        book.delete(entry)
                        dismiss()
                    }
                }
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button(isNew ? String(localized: "Add") : String(localized: "Save")) { save() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(entry.host.trimmingCharacters(in: .whitespaces).isEmpty || missingPassword)
            }
            .padding()
        }
        .frame(width: 500, height: 780)
    }

    private func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = String(localized: "Share")
        panel.message = String(localized: "Choose the folder that appears as a drive in Windows")
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
                ? String(localized: "The saved password will be removed from the keychain. You’ll be asked for it on every connection.")
                : String(localized: "You’ll be asked for the password on every connection. You can still choose to save it there.")
        }
        if missingPassword { return String(localized: "Enter the password to save.") }
        return String(localized: "The password is stored encrypted in your login keychain, never in the address book.")
    }

    private func save() {
        entry.host = entry.host.trimmingCharacters(in: .whitespaces)
        // de komma scheidt naam en pad in de FreeRDP-optie
        entry.sharedFolderName = entry.sharedFolderName.replacingOccurrences(of: ",", with: " ")
        if entry.sharedFolderName.trimmingCharacters(in: .whitespaces).isEmpty { entry.sharedFolderName = "Mac" }
        if entry.sharedFolderPath.contains(",") {
            error = String(localized: "The path of the shared folder must not contain a comma.")
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
            self.error = String(localized: "Password not saved: \(error.localizedDescription)")
            return
        }
        password = ""
        book.upsert(entry)
        dismiss()
    }
}
