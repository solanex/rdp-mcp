"""Toetsnamen naar RDP-scancodes (set 1; 0x100 = extended key).

Scancodes zijn positioneel: welk teken een toets oplevert, hangt af van de
toetsenbordindeling van de remote sessie. Voor sneltoetsen (ctrl+a, win+r, ...)
en voor tekst typen houden we daarom rekening met de indeling, in te stellen
met RDP_MCP_LAYOUT (be = Belgisch azerty, us = qwerty). Tekens die niet op die
indeling staan, gaan via Unicode-events.
"""

from __future__ import annotations

import os

EXT = 0x100

_LETTERS_QWERTY = {
    "q": 0x10, "w": 0x11, "e": 0x12, "r": 0x13, "t": 0x14, "y": 0x15, "u": 0x16,
    "i": 0x17, "o": 0x18, "p": 0x19,
    "a": 0x1E, "s": 0x1F, "d": 0x20, "f": 0x21, "g": 0x22, "h": 0x23, "j": 0x24,
    "k": 0x25, "l": 0x26,
    "z": 0x2C, "x": 0x2D, "c": 0x2E, "v": 0x2F, "b": 0x30, "n": 0x31, "m": 0x32,
}

# Belgisch azerty: a/q en z/w gewisseld, m staat rechts van l
_LETTERS_AZERTY_BE = {
    **_LETTERS_QWERTY,
    "a": 0x10, "q": 0x1E, "z": 0x11, "w": 0x2C, "m": 0x27,
}

_NAMED = {
    "esc": 0x01, "escape": 0x01,
    "backspace": 0x0E, "tab": 0x0F, "enter": 0x1C, "return": 0x1C, "space": 0x39,
    "ctrl": 0x1D, "control": 0x1D, "lctrl": 0x1D, "rctrl": EXT | 0x1D,
    "shift": 0x2A, "lshift": 0x2A, "rshift": 0x36,
    "alt": 0x38, "lalt": 0x38, "ralt": EXT | 0x38, "altgr": EXT | 0x38,
    "win": EXT | 0x5B, "super": EXT | 0x5B, "meta": EXT | 0x5B, "cmd": EXT | 0x5B,
    "lwin": EXT | 0x5B, "rwin": EXT | 0x5C, "menu": EXT | 0x5D, "apps": EXT | 0x5D,
    "capslock": 0x3A, "numlock": 0x45, "scrolllock": 0x46,
    "f1": 0x3B, "f2": 0x3C, "f3": 0x3D, "f4": 0x3E, "f5": 0x3F, "f6": 0x40,
    "f7": 0x41, "f8": 0x42, "f9": 0x43, "f10": 0x44, "f11": 0x57, "f12": 0x58,
    "printscreen": EXT | 0x37, "prtsc": EXT | 0x37,
    "insert": EXT | 0x52, "ins": EXT | 0x52,
    "delete": EXT | 0x53, "del": EXT | 0x53,
    "home": EXT | 0x47, "end": EXT | 0x4F,
    "pageup": EXT | 0x49, "pgup": EXT | 0x49, "pagedown": EXT | 0x51, "pgdn": EXT | 0x51,
    "up": EXT | 0x48, "down": EXT | 0x50, "left": EXT | 0x4B, "right": EXT | 0x4D,
    "kpenter": EXT | 0x1C,
    # cijferrij (positioneel; op azerty zijn dit zonder shift &é"'(§è!çà)
    "1": 0x02, "2": 0x03, "3": 0x04, "4": 0x05, "5": 0x06,
    "6": 0x07, "7": 0x08, "8": 0x09, "9": 0x0A, "0": 0x0B,
}


def _is_azerty() -> bool:
    return os.environ.get("RDP_MCP_LAYOUT", "be").lower() in ("be", "azerty", "be-azerty")


def _letters() -> dict[str, int]:
    return _LETTERS_AZERTY_BE if _is_azerty() else _LETTERS_QWERTY


def scancode(name: str) -> int:
    key = name.strip().lower()
    if key in _NAMED:
        return _NAMED[key]
    letters = _letters()
    if key in letters:
        return letters[key]
    raise ValueError(
        f"unknown key '{name}'. Use e.g. ctrl, alt, shift, win, enter, tab, "
        "esc, f1-f12, up/down/left/right, home, end, delete, a-z, 0-9"
    )


def parse_combo(combo: str) -> list[int]:
    """'ctrl+alt+delete' -> lijst scancodes in indrukvolgorde."""
    parts = [p for p in combo.split("+") if p.strip()]
    if not parts:
        raise ValueError("empty key combination")
    return [scancode(p) for p in parts]


_SHIFT = _NAMED["shift"]
_ALTGR = _NAMED["altgr"]

# (scancode, zonder shift, met shift, met AltGr); None = geen teken of een dode toets.
# Dode toetsen (^ ¨ ´ ` ~ op azerty) laten we weg: die wachten op een volgend teken.
_KEYS_QWERTY = [
    (0x29, "`", "~", None),
    (0x02, "1", "!", None), (0x03, "2", "@", None), (0x04, "3", "#", None),
    (0x05, "4", "$", None), (0x06, "5", "%", None), (0x07, "6", "^", None),
    (0x08, "7", "&", None), (0x09, "8", "*", None), (0x0A, "9", "(", None),
    (0x0B, "0", ")", None), (0x0C, "-", "_", None), (0x0D, "=", "+", None),
    (0x1A, "[", "{", None), (0x1B, "]", "}", None), (0x2B, "\\", "|", None),
    (0x27, ";", ":", None), (0x28, "'", '"', None),
    (0x33, ",", "<", None), (0x34, ".", ">", None), (0x35, "/", "?", None),
]

_KEYS_AZERTY_BE = [
    (0x29, "²", "³", None),
    (0x02, "&", "1", "|"), (0x03, "é", "2", "@"), (0x04, '"', "3", "#"),
    (0x05, "'", "4", None), (0x06, "(", "5", None), (0x07, "§", "6", None),
    (0x08, "è", "7", None), (0x09, "!", "8", None), (0x0A, "ç", "9", "{"),
    (0x0B, "à", "0", "}"), (0x0C, ")", "°", None), (0x0D, "-", "_", None),
    (0x1A, None, None, "["), (0x1B, "$", "*", "]"),
    (0x28, "ù", "%", None), (0x2B, "µ", "£", None), (0x56, "<", ">", "\\"),
    (0x32, ",", "?", None), (0x33, ";", ".", None), (0x34, ":", "/", None),
    (0x35, "=", "+", None),
]


def _build_chars(letters: dict[str, int], keys: list) -> dict[str, list[int]]:
    table: dict[str, list[int]] = {" ": [_NAMED["space"]]}
    for ch, code in letters.items():
        table[ch] = [code]
        table[ch.upper()] = [_SHIFT, code]
    for code, plain, shifted, altgr in keys:
        if plain:
            table[plain] = [code]
        if shifted:
            table[shifted] = [_SHIFT, code]
        if altgr:
            table[altgr] = [_ALTGR, code]
    return table


_CHARS_QWERTY = _build_chars(_LETTERS_QWERTY, _KEYS_QWERTY)
_CHARS_AZERTY_BE = {**_build_chars(_LETTERS_AZERTY_BE, _KEYS_AZERTY_BE), "€": [_ALTGR, 0x12]}


def char_to_scancodes(ch: str) -> list[int] | None:
    """Toetsen (modifiers eerst) die dit teken typen op de ingestelde indeling.
    None = het teken staat niet op de indeling; dan moet het via Unicode."""
    table = _CHARS_AZERTY_BE if _is_azerty() else _CHARS_QWERTY
    return table.get(ch)
