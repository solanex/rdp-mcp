import Foundation

/// Trefwoorden en commandoregels van PowerGuard en CommandGuard. De gebruiker past ze
/// aan in Instellingen; zolang er niets gewijzigd is, gelden de standaardwaarden (in
/// de taal van de app).
struct GuardRules: Codable, Equatable {
    struct CommandRule: Codable, Hashable, Identifiable {
        var id = UUID()
        /// reguliere expressie, hoofdletterongevoelig
        var pattern: String
        /// wat het commando doet, voor de dialoog
        var reason: String

        // id telt niet mee: anders zijn de standaardregels nooit gelijk aan zichzelf
        static func == (a: CommandRule, b: CommandRule) -> Bool { a.pattern == b.pattern && a.reason == b.reason }
        func hash(into h: inout Hasher) {
            h.combine(pattern)
            h.combine(reason)
        }
    }

    /// Een klik op een tekst met een van deze woorden vraagt altijd bevestiging
    var clickWords: [String]
    /// Enkel gevaarlijk als het aan/uit-menu of het afsluitvenster op het scherm staat
    /// ("Afsluiten" is ook het menu-item om een app te sluiten)
    var contextWords: [String]
    /// Titels van het afsluitvenster (Alt+F4 op het bureaublad)
    var dialogTitles: [String]
    /// Klikken hierop sluit het afsluitvenster zonder iets te doen
    var cancelWords: [String]
    var commands: [CommandRule]

    static var defaults: GuardRules {
        let power = String(localized: "This restarts or shuts down Windows.")
        let network = String(localized: "This can cut the network connection, and with it this session.")
        return GuardRules(
            clickWords: [
                "opnieuw opstarten", "slaapstand", "sluimerstand", "afmelden",
                "restart", "shut down", "shutdown", "power off", "sleep", "hibernate", "sign out", "log off",
            ],
            contextWords: ["afsluiten", "uitschakelen"],
            dialogTitles: [
                "windows afsluiten", "wat wilt u dat de computer doet", "wat moet de computer doen",
                "shut down windows", "what do you want the computer to do",
            ],
            cancelWords: ["annuleren", "cancel", "help"],
            commands: [
                CommandRule(pattern: #"\b(restart|stop)-computer\b"#, reason: power),
                // shutdown /a breekt een geplande afsluiting af en mag wel
                CommandRule(pattern: #"\bshutdown(\.exe)?\b(?!\s+[/-]a\b)"#, reason: power),
                CommandRule(pattern: #"\b(psshutdown|tsshutdn)(\.exe)?\b"#, reason: power),
                CommandRule(pattern: #"\bwin32shutdown(tracker)?\b|\.(reboot|shutdown)\s*\(|-methodname\s+['"]?(reboot|shutdown)"#, reason: power),
                CommandRule(pattern: #"\bwmic\b.*\b(reboot|shutdown)\b"#, reason: power),
                CommandRule(pattern: #"\bsetsuspendstate\b"#, reason: String(localized: "This puts Windows to sleep.")),
                CommandRule(pattern: #"\b(logoff|rwinsta)(\.exe)?\b|\breset\s+session\b"#,
                            reason: String(localized: "This signs a user out.")),
                CommandRule(pattern: #"\b(stop-process|taskkill|kill)\b.*\b(winlogon|csrss|wininit|lsass|smss)\b"#,
                            reason: String(localized: "Stopping this system process crashes or restarts Windows.")),
                CommandRule(pattern: #"\b(restart|stop)-vm\b"#,
                            reason: String(localized: "This restarts or shuts down a virtual machine.")),
                CommandRule(pattern: #"\bdisable-netadapter\b|\bnetsh\b.*\binterface\b.*\bdisable"#, reason: network),
                CommandRule(pattern: #"\b(stop|restart)-service\b.*\b(termservice|remote desktop)"#, reason: network),
                CommandRule(pattern: #"\b(format-volume|clear-disk|initialize-disk)\b|\bdiskpart\b|\bformat(\.com)?\s+[a-z]:"#,
                            reason: String(localized: "This can erase a disk.")),
                CommandRule(pattern: #"\bbcdedit\b.*\s[/-](set|delete|deletevalue)\b"#,
                            reason: String(localized: "This changes how Windows starts.")),
                CommandRule(pattern: #"\b(remove|add)-computer\b"#,
                            reason: String(localized: "This changes the domain membership and usually restarts Windows.")),
            ])
    }

    /// Of het patroon een geldige reguliere expressie is
    static func isValid(pattern: String) -> Bool {
        !pattern.isEmpty && (try? NSRegularExpression(pattern: pattern)) != nil
    }
}

/// De actuele regels. Gelezen vanuit de control-threads, gewijzigd vanuit Instellingen.
final class GuardSettings: ObservableObject {
    static let shared = GuardSettings()
    private static let key = "guardRules"

    @Published var rules: GuardRules {
        didSet { store() }
    }

    private let lock = NSLock()
    private var snapshot: GuardRules

    private init() {
        let saved = UserDefaults.standard.data(forKey: Self.key)
            .flatMap { try? JSONDecoder().decode(GuardRules.self, from: $0) }
        let initial = saved ?? .defaults
        snapshot = initial
        rules = initial
    }

    /// Thread-safe kopie voor de control-threads
    static var current: GuardRules {
        shared.lock.lock()
        defer { shared.lock.unlock() }
        return shared.snapshot
    }

    var isDefault: Bool { rules == .defaults }

    func restoreDefaults() { rules = .defaults }

    private func store() {
        lock.lock()
        snapshot = rules
        lock.unlock()
        // enkel bewaren wat afwijkt, zodat de standaard de taal van de app volgt
        if rules == .defaults {
            UserDefaults.standard.removeObject(forKey: Self.key)
        } else if let data = try? JSONEncoder().encode(rules) {
            UserDefaults.standard.set(data, forKey: Self.key)
        }
    }
}
