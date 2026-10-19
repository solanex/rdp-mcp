import AppKit
import CRdpMcpCore

/// Eén geopende sessie (tab). Brug tussen de C-kern en de UI.
///
/// De C-callbacks komen van achtergrondthreads; alles wat de UI raakt gaat via
/// DispatchQueue.main. Certificaat- en wachtwoordvragen blokkeren de rdp-thread
/// tot de gebruiker antwoordt.
final class RDPSession: ObservableObject, Identifiable {
    enum Status: Equatable {
        case connecting
        case connected
        case disconnected(String?)
        case failed(String)

        var isActive: Bool { self == .connecting || self == .connected }
    }

    let id = UUID()
    let entry: HostEntry
    let view = RDPView()

    @Published var status: Status = .connecting
    @Published var aiEnabled: Bool
    @Published var remoteSize: CGSize = .zero
    @Published var notice: String?
    /// Staat in een eigen venster in plaats van als tab
    @Published var isDetached = false

    /// rdpmcp_session*; nil na close()
    private(set) var handle: OpaquePointer?
    var dynamicResolution: Bool { entry.isAutoSize }

    /// Wordt aangeroepen als de gebruiker in een wachtwoordvraag "bewaren" aanvinkt
    var onSavePassword: ((HostEntry, String, String, String) -> Void)?
    /// Om deze tab naar voren te halen bij een vraag
    var onNeedsAttention: ((RDPSession) -> Void)?

    private var selfRetain: Unmanaged<RDPSession>?
    private let promptLock = NSLock()
    private var pendingPrompt: DispatchSemaphore?
    private var noticeTimer: Timer?

    init(entry: HostEntry) {
        self.entry = entry
        aiEnabled = entry.aiEnabledAtStart
        view.session = self
    }

    // MARK: - starten en stoppen

    func start(password: String?, size: CGSize) -> Bool {
        let w = entry.isAutoSize ? UInt32(max(800, size.width)) & ~1 : UInt32(entry.width)
        let h = entry.isAutoSize ? UInt32(max(600, size.height)) : UInt32(entry.height)

        var strings: [UnsafeMutablePointer<CChar>?] = []
        func c(_ s: String) -> UnsafePointer<CChar>? {
            guard !s.isEmpty, let p = strdup(s) else { return nil }
            strings.append(p)
            return UnsafePointer(p)
        }
        defer {
            for p in strings {
                if let p { memset(p, 0, strlen(p)); free(p) } // ook het wachtwoord wissen
            }
        }

        var config = rdpmcp_config()
        config.name = c(entry.displayName)
        config.host = c(entry.host)
        config.port = UInt16(clamping: entry.port)
        config.username = c(entry.username)
        config.domain = c(entry.domain)
        config.password = c(password ?? "")
        config.gateway = c(entry.gateway)
        config.width = w
        config.height = h
        config.keyboard_layout = UInt32(entry.keyboardLayout)
        config.ignore_certificate = entry.ignoreCertificate
        config.dynamic_resolution = entry.isAutoSize
        config.clipboard = entry.clipboard
        config.ai_enabled = entry.aiEnabledAtStart
        config.sound = entry.sound
        config.shared_folder_name = c(entry.sharedFolderName)
        config.shared_folder_path = c(entry.sharedFolderPath)

        let retained = Unmanaged.passRetained(self)
        var cb = rdpmcp_callbacks()
        cb.user = retained.toOpaque()
        cb.frame = { user in
            let s = RDPSession.from(user)
            DispatchQueue.main.async { s.view.pullFrame() }
        }
        cb.state = { user, state, message in
            let s = RDPSession.from(user)
            let text = message.map { String(cString: $0) }
            DispatchQueue.main.async { s.apply(state: state, message: text) }
        }
        cb.ai_activity = { user, x, y in
            let s = RDPSession.from(user)
            DispatchQueue.main.async { s.view.flashAI(x: Int(x), y: Int(y)) }
        }
        cb.remote_clipboard_data = { user, text, dib, dibLen in
            let s = RDPSession.from(user)
            let value = text.map { String(cString: $0) }
            let image = dib.map { Data(bytes: $0, count: dibLen) }
            DispatchQueue.main.async {
                SessionManager.setPasteboard(text: value, image: image.flatMap(ClipboardImage.image(fromDIB:)), from: s)
            }
        }
        cb.pointer = { user, bgra, w, h, hotX, hotY in
            let s = RDPSession.from(user)
            let shape: RemoteCursor
            if let bgra, w > 0, h > 0 {
                shape = .image(Data(bytes: bgra, count: Int(w) * Int(h) * 4), Int(w), Int(h), Int(hotX), Int(hotY))
            } else {
                shape = w == 0 ? .hidden : .system
            }
            DispatchQueue.main.async { s.view.setRemoteCursor(shape) }
        }
        cb.notice = { user, message in
            guard let message else { return }
            let s = RDPSession.from(user)
            let text = String(cString: message)
            DispatchQueue.main.async { s.show(notice: text) }
        }
        cb.verify_certificate = { user, host, port, cn, subject, issuer, fingerprint, changed in
            RDPSession.from(user).askCertificate(
                host: host.map { String(cString: $0) } ?? "?", port: port,
                subject: subject.map { String(cString: $0) } ?? "",
                issuer: issuer.map { String(cString: $0) } ?? "",
                fingerprint: fingerprint.map { String(cString: $0) } ?? "",
                changed: changed)
        }
        cb.authenticate = { user, username, password, domain in
            RDPSession.from(user).askCredentials(username: username, password: password, domain: domain)
        }

        guard let h = rdpmcp_session_new(&config, &cb) else {
            retained.release()
            status = .failed("Ongeldige instellingen")
            return false
        }
        handle = h
        selfRetain = retained
        guard rdpmcp_session_start(h) else {
            close()
            status = .failed("Sessie kon niet starten")
            return false
        }
        return true
    }

    /// Sluit de sessie. Het vrijgeven gebeurt op de achtergrond, want het wacht op de rdp-thread.
    func close() {
        cancelPendingPrompt()
        guard let h = handle else { return }
        handle = nil
        rdpmcp_session_disconnect(h)
        let retained = selfRetain
        selfRetain = nil
        DispatchQueue.global(qos: .userInitiated).async {
            rdpmcp_session_free(h)
            DispatchQueue.main.async { retained?.release() }
        }
    }

    /// Bij afsluiten van de app: synchroon, zodat de sockets opgeruimd worden
    func closeAndWait() {
        cancelPendingPrompt()
        guard let h = handle else { return }
        handle = nil
        rdpmcp_session_free(h)
        selfRetain?.release()
        selfRetain = nil
    }

    // MARK: - bediening

    func setAI(_ enabled: Bool) {
        aiEnabled = enabled
        if let h = handle { rdpmcp_set_ai_enabled(h, enabled) }
        show(notice: enabled ? "AI-besturing aan" : "AI-besturing uit")
    }

    func localClipboardChanged(text: String?, dib: Data?) {
        guard let h = handle else { return }
        func send(_ t: UnsafePointer<CChar>?) {
            if let dib {
                dib.withUnsafeBytes { rdpmcp_local_clipboard_set(h, t, $0.bindMemory(to: UInt8.self).baseAddress, dib.count) }
            } else {
                rdpmcp_local_clipboard_set(h, t, nil, 0)
            }
        }
        if let text { text.withCString { send($0) } } else { send(nil) }
    }

    func show(notice text: String) {
        notice = text
        noticeTimer?.invalidate()
        noticeTimer = Timer.scheduledTimer(withTimeInterval: 4, repeats: false) { [weak self] _ in
            self?.notice = nil
        }
    }

    // MARK: - callbacks

    private static func from(_ user: UnsafeMutableRawPointer?) -> RDPSession {
        Unmanaged<RDPSession>.fromOpaque(user!).takeUnretainedValue()
    }

    private func apply(state: rdpmcp_state, message: String?) {
        switch state {
        case RDPMCP_STATE_CONNECTING: status = .connecting
        case RDPMCP_STATE_CONNECTED:
            status = .connected
            view.focusIfActive()
        case RDPMCP_STATE_DISCONNECTED: status = .disconnected(message)
        default: status = .failed(message ?? "Onbekende fout")
        }
    }

    private func beginPrompt() -> DispatchSemaphore {
        let sem = DispatchSemaphore(value: 0)
        promptLock.lock()
        pendingPrompt = sem
        promptLock.unlock()
        return sem
    }

    private func endPrompt(_ sem: DispatchSemaphore) {
        promptLock.lock()
        if pendingPrompt === sem { pendingPrompt = nil }
        promptLock.unlock()
        sem.signal()
    }

    private func cancelPendingPrompt() {
        promptLock.lock()
        let sem = pendingPrompt
        pendingPrompt = nil
        promptLock.unlock()
        sem?.signal()
    }

    /// rdp-thread: blokkeert tot de gebruiker beslist
    private func askCertificate(host: String, port: UInt16, subject: String, issuer: String,
                                fingerprint: String, changed: Bool) -> rdpmcp_cert_decision {
        var decision = RDPMCP_CERT_REJECT
        let sem = beginPrompt()
        DispatchQueue.main.async {
            self.onNeedsAttention?(self)
            let alert = NSAlert()
            alert.alertStyle = changed ? .critical : .warning
            alert.messageText = changed
                ? "Het certificaat van \(host) is gewijzigd"
                : "Onbekend certificaat voor \(host)"
            alert.informativeText = """
            \(changed ? "Dit kan wijzen op een aanval (man-in-the-middle). Vertrouw het enkel als je weet waarom het veranderde.\n\n" : "")Onderwerp: \(subject)
            Uitgever: \(issuer)
            Vingerafdruk: \(fingerprint.count > 200 ? "(certificaat in PEM-formaat)" : fingerprint)
            """
            alert.addButton(withTitle: "Altijd vertrouwen")
            alert.addButton(withTitle: "Eén keer")
            alert.addButton(withTitle: "Annuleren")
            self.present(alert) { response in
                switch response {
                case .alertFirstButtonReturn: decision = RDPMCP_CERT_ACCEPT_ALWAYS
                case .alertSecondButtonReturn: decision = RDPMCP_CERT_ACCEPT_ONCE
                default: decision = RDPMCP_CERT_REJECT
                }
                self.endPrompt(sem)
            }
        }
        sem.wait()
        return decision
    }

    /// rdp-thread: vraagt ontbrekende gegevens. Waarden zijn malloc'd; de kern geeft ze vrij.
    private func askCredentials(username: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?,
                                password: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?,
                                domain: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?) -> Bool {
        guard let username, let password, let domain else { return false }
        let oldUser = username.pointee.map { String(cString: $0) } ?? entry.username
        let oldDomain = domain.pointee.map { String(cString: $0) } ?? entry.domain

        var result: (String, String, String, Bool)?
        let sem = beginPrompt()
        DispatchQueue.main.async {
            self.onNeedsAttention?(self)
            let prompt = CredentialsPrompt(host: self.entry.displayName, username: oldUser, domain: oldDomain,
                                           canSave: self.onSavePassword != nil)
            self.present(prompt.alert) { response in
                if response == .alertFirstButtonReturn {
                    result = (prompt.username, prompt.password, prompt.domain, prompt.save)
                }
                self.endPrompt(sem)
            }
        }
        sem.wait()

        guard let (user, pass, dom, save) = result else { return false }
        func replace(_ target: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>, _ value: String) {
            if let old = target.pointee { free(old) }
            target.pointee = value.isEmpty ? nil : strdup(value)
        }
        replace(username, user)
        replace(password, pass)
        replace(domain, dom)
        if save {
            let entry = self.entry
            DispatchQueue.main.async { self.onSavePassword?(entry, user, dom, pass) }
        }
        return true
    }

    private func present(_ alert: NSAlert, completion: @escaping (NSApplication.ModalResponse) -> Void) {
        if let window = view.window ?? NSApp.keyWindow ?? NSApp.windows.first {
            alert.beginSheetModal(for: window, completionHandler: completion)
        } else {
            completion(alert.runModal())
        }
    }
}

/// Dialoog voor gebruikersnaam, domein en wachtwoord
private final class CredentialsPrompt {
    let alert = NSAlert()
    private let userField = NSTextField()
    private let domainField = NSTextField()
    private let passwordField = NSSecureTextField()
    private let saveBox = NSButton(checkboxWithTitle: "Wachtwoord bewaren in de sleutelhanger", target: nil, action: nil)

    var username: String { userField.stringValue }
    var domain: String { domainField.stringValue }
    var password: String { passwordField.stringValue }
    var save: Bool { saveBox.state == .on }

    init(host: String, username: String, domain: String, canSave: Bool) {
        alert.messageText = "Aanmelden bij \(host)"
        alert.informativeText = "Het wachtwoord gaat rechtstreeks naar de server en nooit naar de AI."
        alert.addButton(withTitle: "Verbinden")
        alert.addButton(withTitle: "Annuleren")

        userField.stringValue = username
        userField.placeholderString = "gebruikersnaam"
        domainField.stringValue = domain
        domainField.placeholderString = "domein (bv. VOORBEELD)"
        passwordField.placeholderString = "wachtwoord"
        saveBox.isHidden = !canSave

        let stack = NSStackView(views: [userField, domainField, passwordField, saveBox])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 8
        for field in [userField, domainField, passwordField] {
            field.translatesAutoresizingMaskIntoConstraints = false
            field.widthAnchor.constraint(equalToConstant: 260).isActive = true
        }
        stack.frame = NSRect(x: 0, y: 0, width: 260, height: canSave ? 110 : 84)
        alert.accessoryView = stack
        alert.window.initialFirstResponder = username.isEmpty ? userField : passwordField
    }
}
