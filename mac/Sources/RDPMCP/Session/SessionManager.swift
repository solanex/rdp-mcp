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
    private var detachedWindows: [UUID: NSWindow] = [:]
    private var keyMonitor: Any?
    private var clipboardTimer: Timer?
    private static var lastChangeCount = NSPasteboard.general.changeCount
    /// Klembordwijziging die we zelf maakten vanuit een sessie: niet terugsturen naar die sessie
    private static var ownChange: (count: Int, session: UUID)?

    init(book: AddressBook) {
        self.book = book
        installKeyMonitor()
        clipboardTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.pollPasteboard()
        }
    }

    var selected: RDPSession? { sessions.first { $0.id == selectedID } }
    var attachedSessions: [RDPSession] { sessions.filter { !$0.isDetached } }

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
        if entry.clipboard {
            let pb = NSPasteboard.general
            session.localClipboardChanged(text: pb.string(forType: .string), dib: ClipboardImage.dib(from: pb))
        }
    }

    @MainActor
    func reconnect(_ session: RDPSession) async {
        guard let index = sessions.firstIndex(where: { $0.id == session.id }) else { return }
        let entry = book.entry(id: session.entry.id) ?? session.entry
        if session.isDetached { attach(session) } // de nieuwe verbinding komt als tab
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
        if let window = detachedWindows.removeValue(forKey: session.id) {
            window.delegate = nil
            window.close()
        }
        session.close()
        sessions.remove(at: index)
        if selectedID == session.id { selectNearestAttached(from: index) }
    }

    private func selectNearestAttached(from index: Int) {
        let attached = attachedSessions
        guard !attached.isEmpty else { selectedID = nil; return }
        // de tab die nu op of vóór de oude plaats staat
        let candidates = sessions.prefix(min(index + 1, sessions.count)).filter { !$0.isDetached }
        selectedID = (candidates.last ?? attached.first)?.id
    }

    // MARK: - losse vensters

    /// Haalt een tab naar een eigen venster, bv. om hem op een tweede scherm volledig scherm te zetten
    func detach(_ session: RDPSession) {
        guard !session.isDetached, let index = sessions.firstIndex(where: { $0.id == session.id }) else { return }
        let size = session.view.bounds.size
        session.isDetached = true
        if selectedID == session.id { selectNearestAttached(from: index) }
        updateActiveViews()
        // pas in de volgende runloop: eerst moet het hoofdvenster de view loslaten
        DispatchQueue.main.async { [weak self] in
            guard let self, session.isDetached else { return }
            let window = DetachedWindow(session: session, manager: self, book: self.book,
                                        size: size.width > 200 ? size : CGSize(width: 1280, height: 800))
            window.onClose = { [weak self] in self?.attach(session) }
            self.detachedWindows[session.id] = window
            window.makeKeyAndOrderFront(nil)
        }
    }

    /// Zet een losgemaakte sessie terug als tab
    func attach(_ session: RDPSession) {
        guard session.isDetached else { return }
        if let window = detachedWindows.removeValue(forKey: session.id) {
            window.delegate = nil
            if let detached = window as? DetachedWindow, !detached.isClosing { detached.close() }
        }
        session.isDetached = false
        selectedID = session.id
        NSApp.windows.first { $0.identifier?.rawValue == "main" || $0.title == "RDP MCP" }?.makeKeyAndOrderFront(nil)
    }

    func focusDetached(_ session: RDPSession) {
        detachedWindows[session.id]?.makeKeyAndOrderFront(nil)
    }

    func closeSelected() {
        if let s = selected { close(s) }
    }

    func selectNext(_ step: Int) {
        let attached = attachedSessions
        guard !attached.isEmpty else { return }
        let i = attached.firstIndex { $0.id == selectedID } ?? 0
        selectedID = attached[(i + step + attached.count) % attached.count].id
    }

    func closeAllAndWait() {
        for window in detachedWindows.values { window.delegate = nil }
        detachedWindows.removeAll()
        for s in sessions { s.closeAndWait() }
        sessions.removeAll()
    }

    private func updateActiveViews() {
        for s in sessions { s.view.isActive = s.isDetached || s.id == selectedID }
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
            guard let self, let window = event.window,
                  let session = self.sessions.first(where: { $0.view.window === window && window.firstResponder === $0.view }),
                  session.status == .connected else { return event }
            let view = session.view
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
        let targets = sessions.filter { s in
            s.entry.clipboard && !(Self.ownChange?.count == pb.changeCount && Self.ownChange?.session == s.id)
        }
        guard !targets.isEmpty else { return }
        let text = pb.string(forType: .string)
        let dib = ClipboardImage.dib(from: pb)
        for s in targets { s.localClipboardChanged(text: text, dib: dib) }
    }

    /// Wat remote gekopieerd werd op het Mac-klembord zetten. De polling geeft het daarna
    /// ook aan de andere tabs, maar niet terug aan de bronsessie.
    static func setPasteboard(text: String?, image: NSImage?, from session: RDPSession) {
        guard session.handle != nil, text != nil || image != nil else { return }
        let pb = NSPasteboard.general
        pb.clearContents()
        if let image { pb.writeObjects([image]) }
        if let text { pb.setString(text, forType: .string) }
        ownChange = (pb.changeCount, session.id)
    }
}
