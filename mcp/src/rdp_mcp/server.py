"""MCP server: gives an AI agent eyes and hands in an rdp-mcp session.

All coordinates in the tools are pixels in the screenshot as the agent receives
it. The screenshot is scaled down until its longest side is at most
RDP_MCP_MAX_EDGE pixels (default 1280); the server converts back to remote
coordinates itself.
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
        "Controls a remote desktop (RDP) that the user watches live in the rdp-mcp "
        "window. Take a screenshot first; all coordinates are pixels in the latest "
        "screenshot. Take another screenshot after actions to check the result. "
        "The user types passwords themselves."
    ),
)


def _pick(session: str | None) -> tuple[Session, SessionInfo]:
    sessions = list_sessions()
    if not sessions:
        raise ToolError(
            "no active rdp-mcp session found. Ask the user to connect to a host in "
            "the RDP MCP app (or start rdp-mcp-client /v:host /u:user)."
        )
    if session is None:
        if len(sessions) > 1:
            ids = ", ".join(f"{i.id} ({i.name})" for _, i in sessions)
            raise ToolError(f"several sessions are active, pass 'session' (id or name): {ids}")
        found = sessions[0]
    else:
        matches = [x for x in sessions if x[1].id == session] or [
            x for x in sessions if x[1].name.lower() == session.lower()
        ]
        if not matches:
            raise ToolError(f"session '{session}' not found")
        if len(matches) > 1:
            raise ToolError(f"several sessions are named '{session}', use the id")
        found = matches[0]
    if found[1].width <= 0:
        raise ToolError(f"session {found[1].name} is still connecting; try again shortly")
    return found


def _scale(info: SessionInfo) -> float:
    return min(1.0, MAX_EDGE / max(info.width, info.height))


def _to_remote(info: SessionInfo, x: float, y: float) -> tuple[int, int]:
    s = _scale(info)
    rx = min(max(round(x / s), 0), info.width - 1)
    ry = min(max(round(y / s), 0), info.height - 1)
    return rx, ry


def _tool_error(e: ClientError) -> ToolError:
    if e.code == "ai-disabled":
        return ToolError(
            "the user has turned AI control off in the rdp-mcp window "
            "(Ctrl+Alt+Shift+A). Ask the user to turn it back on."
        )
    if e.code == "clipboard-local":
        return ToolError(
            "the clipboard holds something the user copied on their own Mac; that is "
            "deliberately not shared. Copy something in the remote session first (e.g. ctrl+c)."
        )
    return ToolError(str(e))


def _run(conn: Connection, line: str) -> None:
    global _last_input
    try:
        conn.cmd(line)
        _last_input = time.monotonic()
    except ClientError as e:
        raise _tool_error(e) from e


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
    """List the active rdp-mcp sessions (id, name, host, resolution, AI control on/off).

    When there are several, pass 'session' to the other tools with the id or the name.
    """
    return [
        {
            "session": info.id,
            "name": info.name,
            "host": info.host,
            "connected": info.width > 0,
            "width": info.width,
            "height": info.height,
            "ai_control_enabled": info.ai_enabled,
        }
        for _, info in list_sessions()
    ]


@mcp.tool(structured_output=False)
def screenshot(session: str | None = None) -> list:
    """Take a screenshot of the remote desktop.

    Coordinates for the other tools are pixels in this image.
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
        f"Session {info.id} '{info.name}' ({info.host}), remote {w}x{h}, screenshot "
        f"{img.width}x{img.height}. AI control: {'on' if info.ai_enabled else 'OFF'}."
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
    """Click at (x, y). clicks=2 for a double-click."""
    s, info = _pick(session)
    rx, ry = _to_remote(info, x, y)
    with s.connect() as conn:
        _click(conn, button, rx, ry, max(1, min(clicks, 3)))
    return f"{button} click x{clicks} at ({x}, {y})"


@mcp.tool()
def move_mouse(x: int, y: int, session: str | None = None) -> str:
    """Move the mouse to (x, y) without clicking (e.g. for hover menus)."""
    s, info = _pick(session)
    rx, ry = _to_remote(info, x, y)
    with s.connect() as conn:
        _run(conn, f"MOVE {rx} {ry}")
    return f"mouse at ({x}, {y})"


@mcp.tool()
def drag(
    start_x: int,
    start_y: int,
    end_x: int,
    end_y: int,
    button: Literal["left", "right", "middle"] = "left",
    session: str | None = None,
) -> str:
    """Drag with the mouse button held from (start_x, start_y) to (end_x, end_y)."""
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
    return f"dragged from ({start_x}, {start_y}) to ({end_x}, {end_y})"


@mcp.tool()
def scroll(
    x: int,
    y: int,
    direction: Literal["up", "down", "left", "right"] = "down",
    amount: int = 3,
    session: str | None = None,
) -> str:
    """Scroll the mouse wheel at (x, y). amount = number of wheel notches."""
    s, info = _pick(session)
    rx, ry = _to_remote(info, x, y)
    amount = max(1, min(amount, 30))
    axis = "v" if direction in ("up", "down") else "h"
    notches = amount if direction in ("up", "right") else -amount
    with s.connect() as conn:
        _run(conn, f"MOVE {rx} {ry}")
        _run(conn, f"WHEEL {axis} {notches} {rx} {ry}")
    return f"scrolled {direction} {amount}x at ({x}, {y})"


@mcp.tool()
def type_text(text: str, session: str | None = None) -> str:
    """Type text into the focused field. Independent of the keyboard layout.

    A newline becomes Enter, a tab becomes Tab. Use press_keys for shortcuts.
    """
    s, info = _pick(session)
    if not info.unicode:
        bad = sorted({ch for ch in text if ch not in "\n\t\r" and keymap.char_to_scancodes(ch) is None})
        if bad:
            raise ToolError(
                "this server does not support Unicode input; without Unicode only letters, "
                f"digits and spaces can be typed. Not possible: {' '.join(bad)}"
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
    return f"typed {len(text)} characters"


@mcp.tool()
def press_keys(keys: str, session: str | None = None) -> str:
    """Press a key or shortcut, e.g. 'enter', 'ctrl+c', 'win+r', 'alt+f4',
    'ctrl+alt+delete'. Separate several in a row with a space: 'tab tab enter'.
    """
    s, _ = _pick(session)
    combos = keys.split()
    if not combos:
        raise ToolError("no keys given")
    try:
        parsed = [keymap.parse_combo(c) for c in combos]
    except ValueError as e:
        raise ToolError(str(e)) from e
    with s.connect() as conn:
        for codes in parsed:
            _press(conn, codes)
            time.sleep(0.05)
    return f"pressed: {keys}"


@mcp.tool()
def get_clipboard(session: str | None = None) -> str:
    """Read the text on the remote session's clipboard, e.g. after selecting + press_keys('ctrl+c').

    Useful to get text out of a window without reading it off a screenshot.
    """
    s, _ = _pick(session)
    pending = SETTLE_SECONDS - (time.monotonic() - _last_input)
    if pending > 0:
        time.sleep(pending)  # de server meldt nieuwe klembordinhoud pas na ctrl+c
    with s.connect() as conn:
        try:
            return conn.clip_get()
        except ClientError as e:
            raise _tool_error(e) from e


@mcp.tool()
def set_clipboard(text: str, session: str | None = None) -> str:
    """Put text on the remote session's clipboard. Then paste with press_keys('ctrl+v').

    Much faster than type_text for long texts, and every character arrives exactly.
    The clipboard on the user's Mac is left alone.
    """
    global _last_input
    s, _ = _pick(session)
    with s.connect() as conn:
        try:
            conn.clip_set(text)
        except ClientError as e:
            raise _tool_error(e) from e
    _last_input = time.monotonic()
    return f"put {len(text)} characters on the remote clipboard; paste with press_keys('ctrl+v')"


@mcp.tool()
def wait(seconds: float = 1.0) -> str:
    """Wait a moment, e.g. until a window or program has loaded (max 30 s)."""
    seconds = max(0.0, min(seconds, 30.0))
    time.sleep(seconds)
    return f"waited {seconds:g} s"


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
