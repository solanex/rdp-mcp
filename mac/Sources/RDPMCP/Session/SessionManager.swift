import AppKit
import Carbon.HIToolbox
import SwiftUI

/// Beheert de open sessies (tabs), het klembord en de toetsenbordroutering.
final class SessionManager: ObservableObject {
    @Published private(set) var sessions: [RDPSession] = []
    @Published var selectedID: UUID? {
        didSet { updateActiveViews() }
    }
    /// Volledig scherm: zijbalk en tabbalk verborgen
    @Published var focusMode = false

    /// Grootte van het sessiegebied, voor de beginresolutie van nieuwe tabs
    var contentSize = CGSize(width: 1280, height: 800)
    @AppStorage("cmdAsWin") var cmdAsWin = false

    private let book: AddressBook
    private var keyMonitor: Any?
    private var clipboardTimer: Timer?
    private static var lastChangeCount = NSPasteboard.general.changeCount

    init(book: AddressBook) {
        self.book = book
        installKeyMonitor()
        clipboardTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.pollPasteboard()
        }
    }

    var selected: RDPSession? { sessions.first { $0.id == selectedID } }

    func session(for entry: HostEntry) -> RDPSession? {
        sessions.first { $0.entry.id == entry.id && $0.status.isActive }
    }

    // MARK: - tabs

    @MainActor
    func connect(_ entry: HostEntry) async {
        if let open = session(for: entry) {
            selectedID = open.id
            return
        }
        var password: String?
        if entry.hasSavedPassword {
            if entry.requireTouchID {
                let ok = await Keychain.authenticate(reason: "het wachtwoord voor \(entry.displayName) gebruiken")
                guard ok else { return }
            }
            password = Keychain.load(account: entry.id)
        }

        let session = RDPSession(entry: entry)
        session.view.cmdAsWin = cmdAsWin
        session.onNeedsAttention = { [weak self] s in self?.selectedID = s.id }
        if book.entry(id: entry.id) != nil {
            session.onSavePassword = { [weak self] entry, user, domain, pass in
                self?.saveCredentials(entry: entry, username: user, domain: domain, password: pass)
            }
        }
        sessions.append(session)
        selectedID = session.id
        _ = session.start(password: password, size: contentSize)
        password = nil
        if entry.clipboard { session.localClipboardChanged(NSPasteboard.general.string(forType: .string)) }
    }

    @MainActor
    func reconnect(_ session: RDPSession) async {
        guard let index = sessions.firstIndex(where: { $0.id == session.id }) else { return }
        let entry = book.entry(id: session.entry.id) ?? session.entry
        session.close()
        sessions.remove(at: index)
        await connect(entry)
        // nieuwe tab op dezelfde plaats zetten
        if let new = sessions.last, sessions.count > index {
            sessions.removeLast()
            sessions.insert(new, at: index)
        }
    }

    func close(_ session: RDPSession) {
        guard let index = sessions.firstIndex(where: { $0.id == session.id }) else { return }
        session.close()
        sessions.remove(at: index)
        if selectedID == session.id {
            selectedID = sessions.isEmpty ? nil : sessions[min(index, sessions.count - 1)].id
        }
    }

    func closeSelected() {
        if let s = selected { close(s) }
    }

    func selectNext(_ step: Int) {
        guard !sessions.isEmpty else { return }
        let i = sessions.firstIndex { $0.id == selectedID } ?? 0
        selectedID = sessions[(i + step + sessions.count) % sessions.count].id
    }

    func closeAllAndWait() {
        for s in sessions { s.closeAndWait() }
        sessions.removeAll()
    }

    private func updateActiveViews() {
        for s in sessions { s.view.isActive = s.id == selectedID }
    }

    private func saveCredentials(entry: HostEntry, username: String, domain: String, password: String) {
        guard var stored = book.entry(id: entry.id) else { return }
        do {
            try Keychain.save(password: password, account: stored.id, label: stored.displayName)
            stored.hasSavedPassword = true
            if stored.username.isEmpty { stored.username = username }
            if stored.domain.isEmpty { stored.domain = domain }
            book.upsert(stored)
        } catch {
            selected?.show(notice: "Wachtwoord niet bewaard: \(error.localizedDescription)")
        }
    }

    // MARK: - toetsenbord

    /// Alle toetsen gaan naar de actieve sessie zolang die focus heeft, ook Cmd-combinaties
    /// (Cmd = Ctrl remote). Enkel de app-sneltoetsen blijven lokaal.
    private func installKeyMonitor() {
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) {
            [weak self] event in
            guard let self, let session = self.selected, session.status == .connected else { return event }
            let view = session.view
            guard event.window === view.window, view.window?.firstResponder === view else { return event }
            if event.type != .flagsChanged, Self.isAppShortcut(event) { return event }
            view.handleKey(event)
            return nil
        }
    }

    static func isAppShortcut(_ event: NSEvent) -> Bool {
        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        let key = Int(event.keyCode)
        if flags.contains(.command) && flags.contains(.option) { return true } // Cmd+Opt+…: tabs
        if flags.contains(.command) && flags.contains(.control) && key == kVK_ANSI_F { return true }
        if flags == [.command] && key == kVK_ANSI_Q { return true }
        return false
    }

    // MARK: - klembord

    private func pollPasteboard() {
        let pb = NSPasteboard.general
        guard pb.changeCount != Self.lastChangeCount else { return }
        Self.lastChangeCount = pb.changeCount
        let text = pb.string(forType: .string)
        for s in sessions where s.entry.clipboard { s.localClipboardChanged(text) }
    }

    /// Tekst die remote gekopieerd werd op het Mac-klembord zetten. De polling geeft het
    /// daarna ook aan de andere tabs; de bronsessie herkent haar eigen tekst en negeert ze.
    static func setPasteboard(_ text: String, from session: RDPSession) {
        guard session.handle != nil else { return }
        let pb = NSPasteboard.general
        pb.clearContents()
        pb.setString(text, forType: .string)
    }
}
