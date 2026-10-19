# Control-socketprotocol

De client luistert op `~/.rdp-mcp/<pid>.sock` (Unix stream socket, of `$RDP_MCP_DIR`).
Elke verbinding mag meerdere commando's sturen. Eén commando staat op één regel
(`\n`), en elk antwoord begint met `OK` of `ERR <code> <uitleg>`.

Coördinaten zijn pixels in de remote resolutie. Waarden buiten het scherm worden
begrensd tot de rand.

| commando | antwoord | opmerking |
|---|---|---|
| `INFO` | `OK <b> <h> <ai 0\|1> <unicode 0\|1> <host>` | altijd toegestaan |
| `SCREENSHOT` | `OK <b> <h> <n>` + `n` bytes | ruwe pixels, BGRX (4 bytes/pixel), stride = b×4; altijd toegestaan |
| `MOVE <x> <y>` | `OK` | |
| `BUTTON <left\|right\|middle> <down\|up> <x> <y>` | `OK` | |
| `WHEEL <v\|h> <notches> <x> <y>` | `OK` | v: positief = omhoog, h: positief = rechts; max. ±30 |
| `KEY <down\|up> <scancode-hex>` | `OK` | RDP-scancode set 1, `0x100` = extended (bv. `15b` = linker Windows-toets) |
| `UNICODE <hex>` | `OK` | één UTF-16 code unit, indrukken + loslaten |

Alle invoercommando's geven `ERR ai-disabled ...` als de gebruiker AI-besturing
heeft uitgezet, en `ERR not-connected ...` als de sessie (nog) niet verbonden is.
`UNICODE` geeft `ERR unicode-unsupported ...` als de server geen Unicode-invoer
ondersteunt.

Voorbeeld:

```
$ nc -U ~/.rdp-mcp/12345.sock
INFO
OK 1920 1080 1 1 server.voorbeeld.be
MOVE 100 200
OK
```
