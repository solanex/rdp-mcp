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
    /// Er staat een wachtwoord in de sleutelhanger
    var hasSavedPassword = false
    /// Touch ID of het Mac-wachtwoord vragen voor het bewaarde wachtwoord gebruikt wordt
    var requireTouchID = false

    var displayName: String { name.isEmpty ? host : name }
    var isAutoSize: Bool { width == 0 || height == 0 }

    static let keyboardLayouts: [(name: String, id: Int)] = [
        ("Standaard", 0),
        ("Belgisch-Frans", 0x80C),
        ("Belgisch (punt)", 0x813),
        ("Belgisch (komma)", 0x1080C),
        ("Frans", 0x40C),
        ("Nederlands", 0x413),
        ("VS", 0x409),
    ]

    static let resolutions: [(name: String, w: Int, h: Int)] = [
        ("Automatisch (tabblad)", 0, 0),
        ("1280 × 800", 1280, 800),
        ("1440 × 900", 1440, 900),
        ("1600 × 900", 1600, 900),
        ("1920 × 1080", 1920, 1080),
        ("2560 × 1440", 2560, 1440),
    ]
}
