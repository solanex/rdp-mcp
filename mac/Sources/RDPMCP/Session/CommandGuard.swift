import Foundation

/// Herkent commando's (PowerShell, cmd, Uitvoeren) die Windows herstarten, afsluiten
/// of afmelden, de verbinding verbreken of een schijf wissen. Gebruikt voor tekst die
/// de AI typt of op het klembord zet (via de MCP-server) en voor de prompt-regel die
/// PowerGuard bij Enter op het scherm leest. De regels staan in GuardRules.
///
/// Zoals PowerGuard een vangnet, geen grens: een alias, een script of een omweg via
/// een variabele glipt erdoor.
enum CommandGuard {
    private static let lock = NSLock()
    private static var compiled: (rules: [GuardRules.CommandRule], regexes: [(NSRegularExpression, String)])?

    /// De reden in mensentaal, of nil als niets herkend werd
    static func check(_ text: String) -> String? {
        let range = NSRange(text.startIndex..., in: text)
        return regexes().first { $0.0.firstMatch(in: text, range: range) != nil }?.1
    }

    /// Gecompileerde regels; opnieuw opgebouwd als de instellingen veranderden.
    /// Ongeldige patronen worden overgeslagen (de instellingen tonen ze in het rood).
    private static func regexes() -> [(NSRegularExpression, String)] {
        let rules = GuardSettings.current.commands
        lock.lock()
        defer { lock.unlock() }
        if let c = compiled, c.rules == rules { return c.regexes }
        let regexes = rules.compactMap { rule in
            (try? NSRegularExpression(pattern: rule.pattern, options: [.caseInsensitive])).map { ($0, rule.reason) }
        }
        compiled = (rules, regexes)
        return regexes
    }
}
