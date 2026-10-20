import Foundation
import LocalAuthentication
import Security

/// Wachtwoorden in de login-sleutelhanger (versleuteld door macOS), per adresboekitem.
enum Keychain {
    private static let service = "be.solanex.rdp-mcp"

    enum Failure: LocalizedError {
        case status(OSStatus)

        var errorDescription: String? {
            switch self {
            case .status(let s):
                return (SecCopyErrorMessageString(s, nil) as String?) ?? String(localized: "keychain error \(s)")
            }
        }
    }

    static func save(password: String, account: UUID, label: String) throws {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account.uuidString,
        ]
        let attributes: [String: Any] = [
            kSecValueData as String: Data(password.utf8),
            kSecAttrLabel as String: "RDP MCP – \(label)",
            kSecAttrDescription as String: "RDP password",
            kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlocked,
        ]
        var status = SecItemUpdate(query as CFDictionary, attributes as CFDictionary)
        if status == errSecItemNotFound {
            status = SecItemAdd(query.merging(attributes) { $1 } as CFDictionary, nil)
        }
        guard status == errSecSuccess else { throw Failure.status(status) }
    }

    static func load(account: UUID) -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account.uuidString,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var result: AnyObject?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func delete(account: UUID) {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account.uuidString,
        ]
        SecItemDelete(query as CFDictionary)
    }

    /// Touch ID (of het Mac-wachtwoord) vragen. Dit is een controle in de app zelf;
    /// het sleutelhangeritem zelf blijft voor deze app leesbaar.
    static func authenticate(reason: String) async -> Bool {
        let context = LAContext()
        var error: NSError?
        guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &error) else { return false }
        return (try? await context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: reason)) ?? false
    }
}
