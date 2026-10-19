import SwiftUI

@main
struct RDPMCPApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var book: AddressBook
    @StateObject private var manager: SessionManager

    init() {
        let book = AddressBook()
        _book = StateObject(wrappedValue: book)
        let manager = SessionManager(book: book)
        _manager = StateObject(wrappedValue: manager)
        AppDelegate.manager = manager
    }

    var body: some Scene {
        // Eén venster: een sessie-view kan maar in één venster tegelijk staan
        Window("RDP MCP", id: "main") {
            ContentView()
                .environmentObject(book)
                .environmentObject(manager)
                .frame(minWidth: 900, minHeight: 560)
        }
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Nieuwe host…") { NotificationCenter.default.post(name: .newHost, object: nil) }
                    .keyboardShortcut("n", modifiers: [.command, .option])
            }
            CommandMenu("Sessie") {
                Button("Volgende tab") { manager.selectNext(1) }
                    .keyboardShortcut(.rightArrow, modifiers: [.command, .option])
                Button("Vorige tab") { manager.selectNext(-1) }
                    .keyboardShortcut(.leftArrow, modifiers: [.command, .option])
                Button("Naar eigen venster") {
                    if let s = manager.selected { manager.detach(s) }
                }
                .keyboardShortcut("d", modifiers: [.command, .option])
                Divider()
                Button("AI-besturing aan/uit") {
                    if let s = manager.selected { s.setAI(!s.aiEnabled) }
                }
                .keyboardShortcut("a", modifiers: [.command, .option])
                Button("Opnieuw verbinden") {
                    if let s = manager.selected { Task { await manager.reconnect(s) } }
                }
                .keyboardShortcut("r", modifiers: [.command, .option])
                Divider()
                Button("Tab sluiten") { manager.closeSelected() }
                    .keyboardShortcut("w", modifiers: [.command, .option])
            }
        }

        Settings {
            SettingsView()
        }
    }
}

struct SettingsView: View {
    @AppStorage("cmdAsWin") private var cmdAsWin = false

    var body: some View {
        Form {
            Toggle("Cmd-toets werkt als Windows-toets", isOn: $cmdAsWin)
            Text("Standaard werkt Cmd als Ctrl, zodat Cmd+C en Cmd+V doen wat je verwacht. Geldt voor nieuwe tabs.")
                .font(.caption)
                .foregroundStyle(.secondary)
            Divider()
            Text("Sneltoetsen in een sessie gaan naar Windows, behalve ⌥⌘-combinaties (tabs), ⌃⌘F (volledig scherm) en ⌘Q.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(20)
        .frame(width: 420)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    static weak var manager: SessionManager?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular) // ook als het programma zonder .app-bundel draait
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let manager = Self.manager, !manager.sessions.isEmpty else { return .terminateNow }
        let alert = NSAlert()
        alert.messageText = "\(manager.sessions.count) open sessie(s) sluiten?"
        alert.informativeText = "De verbindingen worden verbroken. Op Windows blijf je aangemeld."
        alert.addButton(withTitle: "Afsluiten")
        alert.addButton(withTitle: "Annuleren")
        return alert.runModal() == .alertFirstButtonReturn ? .terminateNow : .terminateCancel
    }

    func applicationWillTerminate(_ notification: Notification) {
        Self.manager?.closeAllAndWait()
    }
}
