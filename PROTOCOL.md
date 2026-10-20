# Control socket protocol

Each session listens on `~/.rdp-mcp/<pid>-<n>.sock` (Unix stream socket, or `$RDP_MCP_DIR`).
A connection may send several commands. One command takes one line (`\n`), and
every reply starts with `OK` or `ERR <code> <explanation>`.

Coordinates are pixels in the remote resolution. Values outside the screen are
clamped to the edge.

| command | reply | note |
|---|---|---|
| `INFO` | `OK <w> <h> <ai 0\|1> <unicode 0\|1> <host> <name>` | always allowed; name may contain spaces; w = h = 0 while connecting |
| `SCREENSHOT` | `OK <w> <h> <n>` + `n` bytes | raw pixels, BGRX (4 bytes/pixel), stride = w×4; always allowed |
| `MOVE <x> <y>` | `OK` | |
| `BUTTON <left\|right\|middle> <down\|up> <x> <y>` | `OK` | |
| `WHEEL <v\|h> <notches> <x> <y>` | `OK` | v: positive = up, h: positive = right; max. ±30 |
| `KEY <down\|up> <scancode-hex>` | `OK` | RDP scancode set 1, `0x100` = extended (e.g. `15b` = left Windows key) |
| `UNICODE <hex>` | `OK` | one UTF-16 code unit, press + release |
| `CLIPGET` | `OK <n>` + `n` bytes | text on the remote clipboard (UTF-8, LF); waits up to 2 s for the server |
| `CLIPSET <n>` + `n` bytes | `OK` | puts text (UTF-8) on the remote clipboard, max. 4 MiB |

All input commands return `ERR ai-disabled ...` when the user has turned AI control
off, and `ERR not-connected ...` when the session is not connected (yet).
`UNICODE` returns `ERR unicode-unsupported ...` when the server does not support
Unicode input.

`CLIPGET` and `CLIPSET` can also return these errors:

- `ERR clipboard-local ...`: the clipboard holds content from the user's Mac, which
  is not shared.
- `ERR clipboard-disabled ...`: the clipboard is turned off (`--no-clipboard`).
- `ERR clipboard-not-ready ...`: the clipboard channel is not active yet.

`CLIPSET` always reads the full payload first, even if the command is then refused.

Example:

```
$ nc -U ~/.rdp-mcp/12345-1.sock
INFO
OK 1920 1080 1 1 server.example.com Test PC 2.14
MOVE 100 200
OK
```
