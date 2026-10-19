"""MCP-server: geeft een AI-agent ogen en handen in een rdp-mcp-sessie.

Alle coördinaten in de tools zijn pixels in de screenshot zoals de agent die
krijgt. De screenshot wordt verkleind tot de langste zijde maximaal
RDP_MCP_MAX_EDGE pixels is (standaard 1280); de server rekent zelf terug naar
remote coördinaten.
"""

from __future__ import annotations

import io
import os
import time
from typing import Literal

from mcp.server.mcpserver import Image, MCPServer
from mcp.server.mcpserver.exceptions import ToolError
from PIL import Image as PILImage

from . import keys as keymap
from .session import ClientError, Connection, Session, SessionInfo, list_sessions

MAX_EDGE = int(os.environ.get("RDP_MCP_MAX_EDGE", "1280"))
IMAGE_FORMAT = os.environ.get("RDP_MCP_IMAGE_FORMAT", "png").lower()

BUTTONS = ("left", "right", "middle")

# Het remote scherm loopt even achter op de invoer; een screenshot vlak na een
# actie wacht daarom tot er minstens zoveel tijd verstreken is.
SETTLE_SECONDS = float(os.environ.get("RDP_MCP_SETTLE", "0.4"))
_last_input = 0.0

mcp = MCPServer(
    "rdp-mcp",
    instructions=(
        "Bestuurt een remote desktop (RDP) die de gebruiker live meevolgt in het "
        "rdp-mcp-venster. Neem eerst een screenshot; alle coördinaten zijn pixels "
        "in de laatste screenshot. Neem na acties opnieuw een screenshot om het "
        "resultaat te controleren. Wachtwoorden typt de gebruiker zelf."
    ),
)


def _pick(session: str | None) -> tuple[Session, SessionInfo]:
    sessions = list_sessions()
    if not sessions:
        raise ToolError(
            "geen actieve rdp-mcp-sessie gevonden. Vraag de gebruiker om de "
            "client te starten (rdp-mcp-client /v:host /u:gebruiker)."
        )
    if session is None:
        if len(sessions) > 1:
            ids = ", ".join(f"{i.id} ({i.host})" for _, i in sessions)
            raise ToolError(f"meerdere sessies actief, geef 'session' op: {ids}")
        return sessions[0]
    for s, info in sessions:
        if s.id == session:
            return s, info
    raise ToolError(f"sessie '{session}' niet gevonden")


def _scale(info: SessionInfo) -> float:
    return min(1.0, MAX_EDGE / max(info.width, info.height))


def _to_remote(info: SessionInfo, x: float, y: float) -> tuple[int, int]:
    s = _scale(info)
    rx = min(max(round(x / s), 0), info.width - 1)
    ry = min(max(round(y / s), 0), info.height - 1)
    return rx, ry


def _run(conn: Connection, line: str) -> None:
    global _last_input
    try:
        conn.cmd(line)
        _last_input = time.monotonic()
    except ClientError as e:
        if e.code == "ai-disabled":
            raise ToolError(
                "de gebruiker heeft AI-besturing uitgeschakeld in het rdp-mcp-venster "
                "(Ctrl+Alt+Shift+A). Vraag de gebruiker om ze terug aan te zetten."
            ) from e
        raise ToolError(str(e)) from e


def _press(conn: Connection, codes: list[int]) -> None:
    for code in codes:
        _run(conn, f"KEY down {code:x}")
        time.sleep(0.01)
    for code in reversed(codes):
        _run(conn, f"KEY up {code:x}")
        time.sleep(0.01)


def _click(conn: Connection, button: str, rx: int, ry: int, clicks: int) -> None:
    _run(conn, f"MOVE {rx} {ry}")
    time.sleep(0.03)
    for i in range(clicks):
        _run(conn, f"BUTTON {button} down {rx} {ry}")
        time.sleep(0.03)
        _run(conn, f"BUTTON {button} up {rx} {ry}")
        if i < clicks - 1:
            time.sleep(0.06)


@mcp.tool()
def list_rdp_sessions() -> list[dict]:
    """Toon de actieve rdp-mcp-sessies (id, host, resolutie, AI-besturing aan/uit)."""
    return [
        {
            "session": info.id,
            "host": info.host,
            "width": info.width,
            "height": info.height,
            "ai_control_enabled": info.ai_enabled,
        }
        for _, info in list_sessions()
    ]


@mcp.tool(structured_output=False)
def screenshot(session: str | None = None) -> list:
    """Neem een screenshot van de remote desktop.

    Coördinaten voor de andere tools zijn pixels in deze afbeelding.
    """
    s, info = _pick(session)
    pending = SETTLE_SECONDS - (time.monotonic() - _last_input)
    if pending > 0:
        time.sleep(pending)
    with s.connect() as conn:
        w, h, data = conn.screenshot()
    img = PILImage.frombuffer("RGB", (w, h), data, "raw", "BGRX", w * 4, 1)
    scale = min(1.0, MAX_EDGE / max(w, h))
    if scale < 1.0:
        img = img.resize((round(w * scale), round(h * scale)), PILImage.Resampling.LANCZOS)
    buf = io.BytesIO()
    if IMAGE_FORMAT in ("jpg", "jpeg"):
        img.save(buf, format="JPEG", quality=80)
        fmt = "jpeg"
    else:
        img.save(buf, format="PNG", optimize=True)
        fmt = "png"
    note = (
        f"Sessie {info.id} ({info.host}), remote {w}x{h}, screenshot "
        f"{img.width}x{img.height}. AI-besturing: {'aan' if info.ai_enabled else 'UIT'}."
    )
    return [note, Image(data=buf.getvalue(), format=fmt)]


@mcp.tool()
def click(
    x: int,
    y: int,
    button: Literal["left", "right", "middle"] = "left",
    clicks: int = 1,
    session: str | None = None,
) -> str:
    """Klik op (x, y). clicks=2 voor een dubbelklik."""
    s, info = _pick(session)
    rx, ry = _to_remote(info, x, y)
    with s.connect() as conn:
        _click(conn, button, rx, ry, max(1, min(clicks, 3)))
    return f"{button}-klik x{clicks} op ({x}, {y})"


@mcp.tool()
def move_mouse(x: int, y: int, session: str | None = None) -> str:
    """Beweeg de muis naar (x, y) zonder te klikken (bv. voor hover-menu's)."""
    s, info = _pick(session)
    rx, ry = _to_remote(info, x, y)
    with s.connect() as conn:
        _run(conn, f"MOVE {rx} {ry}")
    return f"muis op ({x}, {y})"


@mcp.tool()
def drag(
    start_x: int,
    start_y: int,
    end_x: int,
    end_y: int,
    button: Literal["left", "right", "middle"] = "left",
    session: str | None = None,
) -> str:
    """Sleep met ingedrukte muisknop van (start_x, start_y) naar (end_x, end_y)."""
    s, info = _pick(session)
    sx, sy = _to_remote(info, start_x, start_y)
    ex, ey = _to_remote(info, end_x, end_y)
    steps = 15
    with s.connect() as conn:
        _run(conn, f"MOVE {sx} {sy}")
        time.sleep(0.05)
        _run(conn, f"BUTTON {button} down {sx} {sy}")
        for i in range(1, steps + 1):
            time.sleep(0.02)
            _run(conn, f"MOVE {sx + (ex - sx) * i // steps} {sy + (ey - sy) * i // steps}")
        time.sleep(0.05)
        _run(conn, f"BUTTON {button} up {ex} {ey}")
    return f"gesleept van ({start_x}, {start_y}) naar ({end_x}, {end_y})"


@mcp.tool()
def scroll(
    x: int,
    y: int,
    direction: Literal["up", "down", "left", "right"] = "down",
    amount: int = 3,
    session: str | None = None,
) -> str:
    """Scroll met het muiswiel op (x, y). amount = aantal 'klikjes' van het wiel."""
    s, info = _pick(session)
    rx, ry = _to_remote(info, x, y)
    amount = max(1, min(amount, 30))
    axis = "v" if direction in ("up", "down") else "h"
    notches = amount if direction in ("up", "right") else -amount
    with s.connect() as conn:
        _run(conn, f"MOVE {rx} {ry}")
        _run(conn, f"WHEEL {axis} {notches} {rx} {ry}")
    return f"{amount}x gescrold naar {direction} op ({x}, {y})"


@mcp.tool()
def type_text(text: str, session: str | None = None) -> str:
    """Typ tekst in het veld dat focus heeft. Onafhankelijk van de toetsenbordindeling.

    Een newline wordt Enter, een tab wordt Tab. Gebruik press_keys voor sneltoetsen.
    """
    s, info = _pick(session)
    if not info.unicode:
        bad = sorted({ch for ch in text if ch not in "\n\t\r" and keymap.char_to_scancodes(ch) is None})
        if bad:
            raise ToolError(
                "deze server ondersteunt geen Unicode-invoer; zonder Unicode kan ik enkel "
                f"letters, cijfers en spaties typen. Niet mogelijk: {' '.join(bad)}"
            )
    with s.connect() as conn:
        for ch in text:
            if ch == "\n":
                _press(conn, [keymap.scancode("enter")])
            elif ch == "\t":
                _press(conn, [keymap.scancode("tab")])
            elif ch == "\r":
                continue
            elif not info.unicode:
                _press(conn, keymap.char_to_scancodes(ch))
            else:
                units = ch.encode("utf-16-le")
                for i in range(0, len(units), 2):
                    _run(conn, f"UNICODE {int.from_bytes(units[i:i + 2], 'little'):x}")
            time.sleep(0.005)
    return f"{len(text)} tekens getypt"


@mcp.tool()
def press_keys(keys: str, session: str | None = None) -> str:
    """Druk een toets of sneltoets in, bv. 'enter', 'ctrl+c', 'win+r', 'alt+f4',
    'ctrl+alt+delete'. Meerdere na elkaar scheiden met een spatie: 'tab tab enter'.
    """
    s, _ = _pick(session)
    combos = keys.split()
    if not combos:
        raise ToolError("geen toetsen opgegeven")
    try:
        parsed = [keymap.parse_combo(c) for c in combos]
    except ValueError as e:
        raise ToolError(str(e)) from e
    with s.connect() as conn:
        for codes in parsed:
            _press(conn, codes)
            time.sleep(0.05)
    return f"ingedrukt: {keys}"


@mcp.tool()
def wait(seconds: float = 1.0) -> str:
    """Wacht even, bv. tot een venster of programma geladen is (max 30 s)."""
    seconds = max(0.0, min(seconds, 30.0))
    time.sleep(seconds)
    return f"{seconds:g} s gewacht"


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
