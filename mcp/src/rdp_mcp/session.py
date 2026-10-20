"""Praat met een lopende rdp-mcp-client via diens Unix-socket.

Protocol (tekst, één commando per regel, zie ook ../../PROTOCOL.md):
  INFO                          -> OK <breedte> <hoogte> <ai 0|1> <unicode 0|1> <host> <naam>
  SCREENSHOT                    -> OK <breedte> <hoogte> <bytes>\\n<ruwe BGRX-pixels>
  MOVE <x> <y>                  -> OK
  BUTTON <left|right|middle> <down|up> <x> <y>
  WHEEL <v|h> <notches> <x> <y> (v: positief = omhoog, h: positief = rechts)
  KEY <down|up> <scancode>      (RDP-scancode, hex; 0x100 = extended)
  UNICODE <codeunit>            (UTF-16 code unit, hex; indrukken + loslaten)
  CLIPGET                       -> OK <bytes>\n<UTF-8-tekst>
  CLIPSET <bytes>\n<UTF-8-tekst> -> OK
Fouten komen terug als: ERR <code> <uitleg>

De Mac-app heeft daarnaast een eigen socket (app-<pid>.ctl) voor het adresboek:
  HOSTS                         -> OK <bytes>\n<JSON-lijst van hosts>
  CONNECT <host-id>             -> OK <sessie-id> <new|existing>
  STATE <sessie-id>             -> OK <connecting|waiting-for-user|connected|disconnected|failed> [uitleg]
  CLOSE <sessie-id>             -> OK   (enkel sessies die via CONNECT geopend werden)
"""

from __future__ import annotations

import json
import os
import socket
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

SOCKET_DIR = Path(os.environ.get("RDP_MCP_DIR", Path.home() / ".rdp-mcp"))
# Een klik of toets kan wachten op de gebruiker: de app vraagt bevestiging bij
# herstarten of afsluiten en weigert zelf na 120 s
INPUT_TIMEOUT = 150


class ClientError(Exception):
    """Fout gemeld door de client (ERR-regel) of verbindingsprobleem."""

    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


@dataclass
class SessionInfo:
    id: str
    width: int
    height: int
    ai_enabled: bool
    unicode: bool
    host: str
    name: str


class Connection:
    def __init__(self, sock: socket.socket):
        self._sock = sock
        self._file = sock.makefile("rb")

    def _send(self, line: str) -> bytes:
        self._sock.sendall(line.encode() + b"\n")
        reply = self._file.readline()
        if not reply:
            raise ClientError("disconnected", "the client closed the connection")
        reply = reply.rstrip(b"\r\n")
        if reply.startswith(b"ERR"):
            parts = reply.decode(errors="replace").split(" ", 2)
            raise ClientError(
                parts[1] if len(parts) > 1 else "error",
                parts[2] if len(parts) > 2 else "",
            )
        if not reply.startswith(b"OK"):
            raise ClientError("protocol", f"unexpected reply: {reply!r}")
        return reply[3:]

    def cmd(self, line: str) -> str:
        return self._send(line).decode()

    def info(self, session_id: str) -> SessionInfo:
        parts = self.cmd("INFO").split(" ", 5)
        w, h, ai, uni, host = parts[:5]
        name = parts[5] if len(parts) > 5 else host  # oudere clients sturen geen naam
        return SessionInfo(session_id, int(w), int(h), ai == "1", uni == "1", host, name)

    def screenshot(self) -> tuple[int, int, bytes]:
        w, h, n = (int(v) for v in self._send("SCREENSHOT").split())
        data = self._file.read(n)
        if len(data) != n:
            raise ClientError("protocol", "screenshot received incompletely")
        return w, h, data

    def clip_get(self) -> str:
        n = int(self._send("CLIPGET").strip() or 0)
        data = self._file.read(n) if n else b""
        if len(data) != n:
            raise ClientError("protocol", "clipboard received incompletely")
        return data.decode("utf-8", errors="replace")

    def clip_set(self, text: str) -> None:
        data = text.encode("utf-8")
        self._sock.sendall(f"CLIPSET {len(data)}\n".encode() + data)
        reply = self._file.readline().rstrip(b"\r\n")
        if reply.startswith(b"ERR"):
            parts = reply.decode(errors="replace").split(" ", 2)
            raise ClientError(parts[1] if len(parts) > 1 else "error", parts[2] if len(parts) > 2 else "")
        if not reply.startswith(b"OK"):
            raise ClientError("protocol", f"unexpected reply: {reply!r}")

    def hosts(self) -> list[dict]:
        n = int(self._send("HOSTS").strip() or 0)
        data = self._file.read(n)
        if len(data) != n:
            raise ClientError("protocol", "host list received incompletely")
        return json.loads(data)

    def open_host(self, host_id: str) -> tuple[str, bool]:
        """Laat de app verbinden; geeft (sessie-id, nieuw geopend) terug."""
        sid, kind = self.cmd(f"CONNECT {host_id}").split()
        return sid, kind == "new"

    def session_state(self, session_id: str) -> tuple[str, str]:
        parts = self.cmd(f"STATE {session_id}").split(" ", 1)
        return parts[0], parts[1] if len(parts) > 1 else ""

    def guard(self, session_id: str, text: str) -> None:
        """Laat de app de tekst controleren; ClientError 'refused' als de gebruiker weigert."""
        data = text.encode("utf-8")
        self._sock.sendall(f"GUARD {session_id} {len(data)}\n".encode() + data)
        reply = self._file.readline().rstrip(b"\r\n")
        if not reply:
            raise ClientError("disconnected", "the app closed the connection")
        if reply.startswith(b"ERR"):
            parts = reply.decode(errors="replace").split(" ", 2)
            raise ClientError(parts[1] if len(parts) > 1 else "error", parts[2] if len(parts) > 2 else "")
        if not reply.startswith(b"OK"):
            raise ClientError("protocol", f"unexpected reply: {reply!r}")

    def close_session(self, session_id: str) -> None:
        self.cmd(f"CLOSE {session_id}")

    def close(self) -> None:
        self._file.close()
        self._sock.close()


class Session:
    def __init__(self, path: Path):
        self.path = path
        self.id = path.stem

    @contextmanager
    def connect(self, timeout: float = INPUT_TIMEOUT) -> Iterator[Connection]:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        try:
            sock.connect(str(self.path))
        except OSError as e:
            sock.close()
            raise ClientError("unreachable", f"session {self.id} is unreachable ({e})") from e
        conn = Connection(sock)
        try:
            yield conn
        finally:
            conn.close()


def list_sessions() -> list[tuple[Session, SessionInfo]]:
    result = []
    for path in sorted(SOCKET_DIR.glob("*.sock")):
        session = Session(path)
        try:
            with session.connect(timeout=15) as c:
                result.append((session, c.info(session.id)))
        except ClientError:
            continue  # client is weg; de socket is een overblijfsel
    return result


@contextmanager
def connect_app(timeout: float = 15) -> Iterator[Connection]:
    """Verbinding met de lopende Mac-app (de jongste als er meerdere draaien)."""
    paths = sorted(SOCKET_DIR.glob("app-*.ctl"), key=lambda p: p.stat().st_mtime, reverse=True)
    for path in paths:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        try:
            sock.connect(str(path))
        except OSError:
            sock.close()
            continue  # app is weg; de socket is een overblijfsel
        conn = Connection(sock)
        try:
            yield conn
        finally:
            conn.close()
        return
    raise ClientError("no-app", "the RDP MCP app is not running")
