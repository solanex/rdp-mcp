import Foundation

/// Eén host in het adresboek. Het wachtwoord staat hier nooit in, alleen in de sleutelhanger.
struct HostEntry: Codable, Identifiable, Hashable {
    var id = UUID()
    var name = ""
    var group = ""
    var host = ""
    var port = 3389
    var username = ""
    var domain = ""
    var gateway = ""
    /// 0 x 0 = automatisch: de grootte van het tabblad, en mee schalen bij wijzigen
    var width = 0
    var height = 0
    /// Windows-toetsenbordindeling, bv. 0x80C (Belgisch-Frans); 0 = standaard
    var keyboardLayout = 0
    var ignoreCertificate = false
    var clipboard = true
    var aiEnabledAtStart = true
    /// Bevestiging vragen als de AI klikt op herstarten, afsluiten of afmelden (PowerGuard)
    var confirmPowerActions = true
    /// Er staat een wachtwoord in de sleutelhanger
    var hasSavedPassword = false
    /// Touch ID of het Mac-wachtwoord vragen voor het bewaarde wachtwoord gebruikt wordt
    var requireTouchID = false
    /// Geluid van de sessie op de Mac afspelen
    var sound = true
    /// Lokale map die in Windows als schijf verschijnt; leeg = niets delen
    var sharedFolderPath = ""
    var sharedFolderName = "Mac"

    init() {}

    /// Ontbrekende velden (oudere hosts.json) krijgen hun standaardwaarde
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = HostEntry()
        id = try c.decodeIfPresent(UUID.self, forKey: .id) ?? d.id
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? d.name
        group = try c.decodeIfPresent(String.self, forKey: .group) ?? d.group
        host = try c.decodeIfPresent(String.self, forKey: .host) ?? d.host
        port = try c.decodeIfPresent(Int.self, forKey: .port) ?? d.port
        username = try c.decodeIfPresent(String.self, forKey: .username) ?? d.username
        domain = try c.decodeIfPresent(String.self, forKey: .domain) ?? d.domain
        gateway = try c.decodeIfPresent(String.self, forKey: .gateway) ?? d.gateway
        width = try c.decodeIfPresent(Int.self, forKey: .width) ?? d.width
        height = try c.decodeIfPresent(Int.self, forKey: .height) ?? d.height
        keyboardLayout = try c.decodeIfPresent(Int.self, forKey: .keyboardLayout) ?? d.keyboardLayout
        ignoreCertificate = try c.decodeIfPresent(Bool.self, forKey: .ignoreCertificate) ?? d.ignoreCertificate
        clipboard = try c.decodeIfPresent(Bool.self, forKey: .clipboard) ?? d.clipboard
        aiEnabledAtStart = try c.decodeIfPresent(Bool.self, forKey: .aiEnabledAtStart) ?? d.aiEnabledAtStart
        confirmPowerActions = try c.decodeIfPresent(Bool.self, forKey: .confirmPowerActions) ?? d.confirmPowerActions
        hasSavedPassword = try c.decodeIfPresent(Bool.self, forKey: .hasSavedPassword) ?? d.hasSavedPassword
        requireTouchID = try c.decodeIfPresent(Bool.self, forKey: .requireTouchID) ?? d.requireTouchID
        sound = try c.decodeIfPresent(Bool.self, forKey: .sound) ?? d.sound
        sharedFolderPath = try c.decodeIfPresent(String.self, forKey: .sharedFolderPath) ?? d.sharedFolderPath
        sharedFolderName = try c.decodeIfPresent(String.self, forKey: .sharedFolderName) ?? d.sharedFolderName
    }

    var displayName: String { name.isEmpty ? host : name }
    var isAutoSize: Bool { width == 0 || height == 0 }

    static let keyboardLayouts: [(name: String, id: Int)] = [
        (String(localized: "Default"), 0),
        (String(localized: "Belgian French"), 0x80C),
        (String(localized: "Belgian (Period)"), 0x813),
        (String(localized: "Belgian (Comma)"), 0x1080C),
        (String(localized: "French"), 0x40C),
        (String(localized: "Dutch"), 0x413),
        (String(localized: "US"), 0x409),
    ]

    static let resolutions: [(name: String, w: Int, h: Int)] = [
        (String(localized: "Automatic (tab size)"), 0, 0),
        ("1280 × 800", 1280, 800),
        ("1440 × 900", 1440, 900),
        ("1600 × 900", 1600, 900),
        ("1920 × 1080", 1920, 1080),
        ("2560 × 1440", 2560, 1440),
    ]
}
