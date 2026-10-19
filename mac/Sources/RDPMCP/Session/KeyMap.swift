import AppKit
import Carbon.HIToolbox

/// Mac-toetscodes (kVK_*, positioneel) naar RDP-scancodes (set 1, 0x100 = extended).
/// De remote toetsenbordindeling bepaalt welk teken een toets oplevert.
enum KeyMap {
    private static let ext: UInt32 = 0x100

    /// Op ISO-toetsenborden (zoals het Belgische) wisselt macOS de toets links van 1
    /// en de toets naast linker-Shift van code; we zetten dat recht.
    private static let isISO: Bool = KBGetLayoutType(Int16(LMGetKbdType())) == kKeyboardISO

    private static let table: [Int: UInt32] = [
        kVK_ANSI_A: 0x1E, kVK_ANSI_S: 0x1F, kVK_ANSI_D: 0x20, kVK_ANSI_F: 0x21,
        kVK_ANSI_H: 0x23, kVK_ANSI_G: 0x22, kVK_ANSI_Z: 0x2C, kVK_ANSI_X: 0x2D,
        kVK_ANSI_C: 0x2E, kVK_ANSI_V: 0x2F, kVK_ANSI_B: 0x30, kVK_ANSI_Q: 0x10,
        kVK_ANSI_W: 0x11, kVK_ANSI_E: 0x12, kVK_ANSI_R: 0x13, kVK_ANSI_Y: 0x15,
        kVK_ANSI_T: 0x14, kVK_ANSI_1: 0x02, kVK_ANSI_2: 0x03, kVK_ANSI_3: 0x04,
        kVK_ANSI_4: 0x05, kVK_ANSI_6: 0x07, kVK_ANSI_5: 0x06, kVK_ANSI_Equal: 0x0D,
        kVK_ANSI_9: 0x0A, kVK_ANSI_7: 0x08, kVK_ANSI_Minus: 0x0C, kVK_ANSI_8: 0x09,
        kVK_ANSI_0: 0x0B, kVK_ANSI_RightBracket: 0x1B, kVK_ANSI_O: 0x18, kVK_ANSI_U: 0x16,
        kVK_ANSI_LeftBracket: 0x1A, kVK_ANSI_I: 0x17, kVK_ANSI_P: 0x19, kVK_Return: 0x1C,
        kVK_ANSI_L: 0x26, kVK_ANSI_J: 0x24, kVK_ANSI_Quote: 0x28, kVK_ANSI_K: 0x25,
        kVK_ANSI_Semicolon: 0x27, kVK_ANSI_Backslash: 0x2B, kVK_ANSI_Comma: 0x33,
        kVK_ANSI_Slash: 0x35, kVK_ANSI_N: 0x31, kVK_ANSI_M: 0x32, kVK_ANSI_Period: 0x34,
        kVK_Tab: 0x0F, kVK_Space: 0x39, kVK_Delete: 0x0E, kVK_Escape: 0x01,
        kVK_CapsLock: 0x3A,
        kVK_Shift: 0x2A, kVK_RightShift: 0x36,
        kVK_Option: 0x38, kVK_RightOption: ext | 0x38,
        kVK_Control: 0x1D, kVK_RightControl: ext | 0x1D,
        kVK_F1: 0x3B, kVK_F2: 0x3C, kVK_F3: 0x3D, kVK_F4: 0x3E, kVK_F5: 0x3F,
        kVK_F6: 0x40, kVK_F7: 0x41, kVK_F8: 0x42, kVK_F9: 0x43, kVK_F10: 0x44,
        kVK_F11: 0x57, kVK_F12: 0x58,
        kVK_ANSI_KeypadDecimal: 0x53, kVK_ANSI_KeypadMultiply: 0x37,
        kVK_ANSI_KeypadPlus: 0x4E, kVK_ANSI_KeypadClear: 0x45, kVK_ANSI_KeypadDivide: ext | 0x35,
        kVK_ANSI_KeypadEnter: ext | 0x1C, kVK_ANSI_KeypadMinus: 0x4A,
        kVK_ANSI_KeypadEquals: 0x59, kVK_ANSI_Keypad0: 0x52, kVK_ANSI_Keypad1: 0x4F,
        kVK_ANSI_Keypad2: 0x50, kVK_ANSI_Keypad3: 0x51, kVK_ANSI_Keypad4: 0x4B,
        kVK_ANSI_Keypad5: 0x4C, kVK_ANSI_Keypad6: 0x4D, kVK_ANSI_Keypad7: 0x47,
        kVK_ANSI_Keypad8: 0x48, kVK_ANSI_Keypad9: 0x49,
        kVK_Help: ext | 0x52, // Insert
        kVK_Home: ext | 0x47, kVK_PageUp: ext | 0x49, kVK_ForwardDelete: ext | 0x53,
        kVK_End: ext | 0x4F, kVK_PageDown: ext | 0x51,
        kVK_LeftArrow: ext | 0x4B, kVK_RightArrow: ext | 0x4D,
        kVK_DownArrow: ext | 0x50, kVK_UpArrow: ext | 0x48,
    ]

    /// cmdAsWin: Cmd wordt de Windows-toets i.p.v. Ctrl
    static func scancode(for keyCode: UInt16, cmdAsWin: Bool) -> UInt32? {
        let code = Int(keyCode)
        switch code {
        case kVK_Command: return cmdAsWin ? ext | 0x5B : 0x1D
        case kVK_RightCommand: return cmdAsWin ? ext | 0x5C : ext | 0x1D
        case kVK_ISO_Section: return isISO ? 0x29 : 0x56
        case kVK_ANSI_Grave: return isISO ? 0x56 : 0x29
        default: return table[code]
        }
    }

    /// Welke modifier-vlag (inclusief links/rechts) bij een modifiertoets hoort,
    /// om in flagsChanged te weten of ze ingedrukt of losgelaten werd.
    static func isDown(modifierKey keyCode: UInt16, flags: NSEvent.ModifierFlags) -> Bool? {
        let raw = flags.rawValue
        switch Int(keyCode) {
        case kVK_Control: return raw & 0x0001 != 0      // NX_DEVICELCTLKEYMASK
        case kVK_RightControl: return raw & 0x2000 != 0 // NX_DEVICERCTLKEYMASK
        case kVK_Shift: return raw & 0x0002 != 0
        case kVK_RightShift: return raw & 0x0004 != 0
        case kVK_Command: return raw & 0x0008 != 0
        case kVK_RightCommand: return raw & 0x0010 != 0
        case kVK_Option: return raw & 0x0020 != 0
        case kVK_RightOption: return raw & 0x0040 != 0
        default: return nil
        }
    }
}
