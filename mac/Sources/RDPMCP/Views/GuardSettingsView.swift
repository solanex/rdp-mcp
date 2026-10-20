import SwiftUI

/// Instellingen → AI Safety: de trefwoorden en commandoregels van het vangnet
struct GuardSettingsView: View {
    @ObservedObject private var settings = GuardSettings.shared
    @State private var tryText = ""

    var body: some View {
        Form {
            Section {
                Text("The app asks before the AI clicks or runs something that matches these rules. Turn the check on or off per host in the address book. Words are matched case- and accent-insensitively, one per line.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Clicking these words always asks") {
                WordList(words: $settings.rules.clickWords)
            }
            Section {
                WordList(words: $settings.rules.contextWords)
            } header: {
                Text("Only when the power menu or shutdown dialog is open")
            } footer: {
                Text("For words like “Afsluiten”, which also closes an app.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Section("Titles of the shutdown dialog") {
                WordList(words: $settings.rules.dialogTitles)
            }
            Section("Buttons that close the shutdown dialog safely") {
                WordList(words: $settings.rules.cancelWords)
            }

            Section {
                ForEach($settings.rules.commands) { $rule in
                    HStack(alignment: .firstTextBaseline) {
                        Image(systemName: GuardRules.isValid(pattern: rule.pattern) ? "checkmark.circle" : "exclamationmark.triangle.fill")
                            .foregroundStyle(GuardRules.isValid(pattern: rule.pattern) ? Color.secondary : Color.red)
                            .help(GuardRules.isValid(pattern: rule.pattern) ? "" : String(localized: "Not a valid regular expression; this rule is skipped."))
                        VStack(alignment: .leading, spacing: 4) {
                            TextField("Pattern", text: $rule.pattern, prompt: Text("regular expression"))
                                .font(.system(.body, design: .monospaced))
                                .labelsHidden()
                            TextField("Explanation", text: $rule.reason, prompt: Text("what the command does"))
                                .labelsHidden()
                                .foregroundStyle(.secondary)
                        }
                        Button {
                            settings.rules.commands.removeAll { $0.id == rule.id }
                        } label: {
                            Image(systemName: "minus.circle")
                        }
                        .buttonStyle(.borderless)
                        .help("Remove rule")
                    }
                }
                Button {
                    settings.rules.commands.append(.init(pattern: "", reason: ""))
                } label: {
                    Label("Add Rule", systemImage: "plus")
                }
            } header: {
                Text("Commands")
            } footer: {
                Text("Checked when the AI presses Enter after typing, puts text on the clipboard, or presses Enter in a console or the Run dialog. Regular expressions, case-insensitive.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Try a command") {
                TextField("Command", text: $tryText, prompt: Text("e.g. Restart-Computer -Force"))
                    .font(.system(.body, design: .monospaced))
                    .labelsHidden()
                if !tryText.isEmpty {
                    if let reason = CommandGuard.check(tryText) {
                        Label(reason, systemImage: "hand.raised.fill").foregroundStyle(.orange)
                    } else {
                        Label("No rule matches", systemImage: "checkmark").foregroundStyle(.secondary)
                    }
                }
            }

            Section {
                HStack {
                    Spacer()
                    Button("Restore Defaults") { settings.restoreDefaults() }
                        .disabled(settings.isDefault)
                }
            }
        }
        .formStyle(.grouped)
    }
}

/// Lijst met woorden als tekst: één per regel, lege regels tellen niet
private struct WordList: View {
    @Binding var words: [String]
    @State private var text = ""

    var body: some View {
        TextEditor(text: $text)
            .font(.body)
            .frame(minHeight: 60, maxHeight: 130)
            .onAppear { text = words.joined(separator: "\n") }
            .onChange(of: text) { _, value in
                let parsed = Self.parse(value)
                if parsed != words { words = parsed }
            }
            .onChange(of: words) { _, value in
                // van buitenaf gewijzigd (standaardwaarden terugzetten)
                if Self.parse(text) != value { text = value.joined(separator: "\n") }
            }
    }

    private static func parse(_ text: String) -> [String] {
        text.split(whereSeparator: \.isNewline)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
    }
}
