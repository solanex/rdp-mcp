"""Toetsnamen naar RDP-scancodes (set 1; 0x100 = extended key).

Scancodes zijn positioneel: welk teken een toets oplevert, hangt af van de
toetsenbordindeling van de remote sessie. Voor letters in sneltoetsen
(ctrl+a, win+r, ...) houden we daarom rekening met de indeling, in te stellen
met RDP_MCP_LAYOUT (be = Belgisch azerty, us = qwerty). Gewone tekst typen
gebeurt via Unicode-events en is onafhankelijk van de indeling.
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


def char_to_scancodes(ch: str) -> list[int] | None:
    """Terugval als de server geen Unicode-invoer kent: tekens die we zeker via
    scancodes kunnen typen (letters, cijfers, spatie). None = niet mogelijk."""
    shift = _NAMED["shift"]
    letters = _letters()
    if ch == " ":
        return [_NAMED["space"]]
    if ch in letters:
        return [letters[ch]]
    if ch.lower() in letters and ch.isupper():
        return [shift, letters[ch.lower()]]
    if ch.isdigit() and ch.isascii():
        # op azerty zitten de cijfers onder shift
        return [shift, _NAMED[ch]] if _is_azerty() else [_NAMED[ch]]
    return None
