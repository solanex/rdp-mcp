# Control-socketprotocol

Elke sessie luistert op `~/.rdp-mcp/<pid>-<n>.sock` (Unix stream socket, of `$RDP_MCP_DIR`).
Elke verbinding mag meerdere commando's sturen. Eén commando staat op één regel
(`\n`), en elk antwoord begint met `OK` of `ERR <code> <uitleg>`.

Coördinaten zijn pixels in de remote resolutie. Waarden buiten het scherm worden
begrensd tot de rand.

| commando | antwoord | opmerking |
|---|---|---|
| `INFO` | `OK <b> <h> <ai 0\|1> <unicode 0\|1> <host> <naam>` | altijd toegestaan; naam mag spaties bevatten; b = h = 0 tijdens verbinden |
| `SCREENSHOT` | `OK <b> <h> <n>` + `n` bytes | ruwe pixels, BGRX (4 bytes/pixel), stride = b×4; altijd toegestaan |
| `MOVE <x> <y>` | `OK` | |
| `BUTTON <left\|right\|middle> <down\|up> <x> <y>` | `OK` | |
| `WHEEL <v\|h> <notches> <x> <y>` | `OK` | v: positief = omhoog, h: positief = rechts; max. ±30 |
| `KEY <down\|up> <scancode-hex>` | `OK` | RDP-scancode set 1, `0x100` = extended (bv. `15b` = linker Windows-toets) |
| `UNICODE <hex>` | `OK` | één UTF-16 code unit, indrukken + loslaten |
| `CLIPGET` | `OK <n>` + `n` bytes | tekst op het remote klembord (UTF-8, LF); wacht max. 2 s op de server |
| `CLIPSET <n>` + `n` bytes | `OK` | zet tekst (UTF-8) op het remote klembord, max. 4 MiB |

Alle invoercommando's geven `ERR ai-disabled ...` als de gebruiker AI-besturing
heeft uitgezet, en `ERR not-connected ...` als de sessie (nog) niet verbonden is.
`UNICODE` geeft `ERR unicode-unsupported ...` als de server geen Unicode-invoer
ondersteunt.

`CLIPGET` en `CLIPSET` kunnen ook deze fouten geven:

- `ERR clipboard-local ...`: het klembord bevat inhoud van de Mac van de gebruiker,
  en die wordt niet gedeeld.
- `ERR clipboard-disabled ...`: het klembord staat uit (`--no-clipboard`).
- `ERR clipboard-not-ready ...`: het klembordkanaal is nog niet actief.

`CLIPSET` leest altijd eerst de volledige payload, ook als het commando daarna
geweigerd wordt.

Voorbeeld:

```
$ nc -U ~/.rdp-mcp/12345-1.sock
INFO
OK 1920 1080 1 1 server.voorbeeld.be Lokaal 2.14
MOVE 100 200
OK
```
