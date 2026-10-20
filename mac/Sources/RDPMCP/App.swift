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
                Button("New Host…") { NotificationCenter.default.post(name: .newHost, object: nil) }
                    .keyboardShortcut("n", modifiers: [.command, .option])
            }
            CommandMenu("Session") {
                Button("Next Tab") { manager.selectNext(1) }
                    .keyboardShortcut(.rightArrow, modifiers: [.command, .option])
                Button("Previous Tab") { manager.selectNext(-1) }
                    .keyboardShortcut(.leftArrow, modifiers: [.command, .option])
                Button("Move to Own Window") {
                    if let s = manager.selected { manager.detach(s) }
                }
                .keyboardShortcut("d", modifiers: [.command, .option])
                Divider()
                Button("Toggle AI Control") {
                    if let s = manager.selected { s.setAI(!s.aiEnabled) }
                }
                .keyboardShortcut("a", modifiers: [.command, .option])
                Button("Reconnect") {
                    if let s = manager.selected { Task { await manager.reconnect(s) } }
                }
                .keyboardShortcut("r", modifiers: [.command, .option])
                Divider()
                Button("Close Tab") { manager.closeSelected() }
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
            Toggle("Cmd key acts as Windows key", isOn: $cmdAsWin)
            Text("By default Cmd acts as Ctrl, so Cmd+C and Cmd+V do what you expect. Applies to new tabs.")
                .font(.caption)
                .foregroundStyle(.secondary)
            Divider()
            Text("Shortcuts in a session go to Windows, except ⌥⌘ combinations (tabs), ⌃⌘F (full screen) and ⌘Q.")
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
        alert.messageText = String(localized: "Close \(manager.sessions.count) open session(s)?")
        alert.informativeText = String(localized: "The connections will be closed. You stay signed in on Windows.")
        alert.addButton(withTitle: String(localized: "Quit"))
        alert.addButton(withTitle: String(localized: "Cancel"))
        return alert.runModal() == .alertFirstButtonReturn ? .terminateNow : .terminateCancel
    }

    func applicationWillTerminate(_ notification: Notification) {
        Self.manager?.closeAllAndWait()
    }
}
