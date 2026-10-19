"""Praat met een lopende rdp-mcp-client via diens Unix-socket.

Protocol (tekst, één commando per regel, zie ook ../../PROTOCOL.md):
  INFO                          -> OK <breedte> <hoogte> <ai 0|1> <unicode 0|1> <host>
  SCREENSHOT                    -> OK <breedte> <hoogte> <bytes>\\n<ruwe BGRX-pixels>
  MOVE <x> <y>                  -> OK
  BUTTON <left|right|middle> <down|up> <x> <y>
  WHEEL <v|h> <notches> <x> <y> (v: positief = omhoog, h: positief = rechts)
  KEY <down|up> <scancode>      (RDP-scancode, hex; 0x100 = extended)
  UNICODE <codeunit>            (UTF-16 code unit, hex; indrukken + loslaten)
  CLIPGET                       -> OK <bytes>\n<UTF-8-tekst>
  CLIPSET <bytes>\n<UTF-8-tekst> -> OK
Fouten komen terug als: ERR <code> <uitleg>
"""

from __future__ import annotations

import os
import socket
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

SOCKET_DIR = Path(os.environ.get("RDP_MCP_DIR", Path.home() / ".rdp-mcp"))


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


class Connection:
    def __init__(self, sock: socket.socket):
        self._sock = sock
        self._file = sock.makefile("rb")

    def _send(self, line: str) -> bytes:
        self._sock.sendall(line.encode() + b"\n")
        reply = self._file.readline()
        if not reply:
            raise ClientError("disconnected", "de client sloot de verbinding")
        reply = reply.rstrip(b"\r\n")
        if reply.startswith(b"ERR"):
            parts = reply.decode(errors="replace").split(" ", 2)
            raise ClientError(
                parts[1] if len(parts) > 1 else "error",
                parts[2] if len(parts) > 2 else "",
            )
        if not reply.startswith(b"OK"):
            raise ClientError("protocol", f"onverwacht antwoord: {reply!r}")
        return reply[3:]

    def cmd(self, line: str) -> str:
        return self._send(line).decode()

    def info(self, session_id: str) -> SessionInfo:
        w, h, ai, uni, host = self.cmd("INFO").split(" ", 4)
        return SessionInfo(session_id, int(w), int(h), ai == "1", uni == "1", host)

    def screenshot(self) -> tuple[int, int, bytes]:
        w, h, n = (int(v) for v in self._send("SCREENSHOT").split())
        data = self._file.read(n)
        if len(data) != n:
            raise ClientError("protocol", "screenshot onvolledig ontvangen")
        return w, h, data

    def clip_get(self) -> str:
        n = int(self._send("CLIPGET").strip() or 0)
        data = self._file.read(n) if n else b""
        if len(data) != n:
            raise ClientError("protocol", "klembord onvolledig ontvangen")
        return data.decode("utf-8", errors="replace")

    def clip_set(self, text: str) -> None:
        data = text.encode("utf-8")
        self._sock.sendall(f"CLIPSET {len(data)}\n".encode() + data)
        reply = self._file.readline().rstrip(b"\r\n")
        if reply.startswith(b"ERR"):
            parts = reply.decode(errors="replace").split(" ", 2)
            raise ClientError(parts[1] if len(parts) > 1 else "error", parts[2] if len(parts) > 2 else "")
        if not reply.startswith(b"OK"):
            raise ClientError("protocol", f"onverwacht antwoord: {reply!r}")

    def close(self) -> None:
        self._file.close()
        self._sock.close()


class Session:
    def __init__(self, path: Path):
        self.path = path
        self.id = path.stem

    @contextmanager
    def connect(self) -> Iterator[Connection]:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(15)
        try:
            sock.connect(str(self.path))
        except OSError as e:
            sock.close()
            raise ClientError("unreachable", f"sessie {self.id} is niet bereikbaar ({e})") from e
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
            with session.connect() as c:
                result.append((session, c.info(session.id)))
        except ClientError:
            continue  # client is weg; de socket is een overblijfsel
    return result
