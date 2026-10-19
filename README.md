# rdp-mcp

Een RDP-client waarin een AI-agent (zoals Claude) via MCP mee kan kijken en de
muis en het toetsenbord kan bedienen, terwijl jij live meekijkt in een gewoon
venster en op elk moment zelf kunt ingrijpen.

```
┌──────────────────────┐  Unix-socket   ┌──────────────┐   stdio/MCP   ┌──────────────┐
│  rdp-mcp-client      │ ◄────────────► │  rdp-mcp     │ ◄───────────► │ Claude Code  │
│  (C, FreeRDP + SDL3) │  ~/.rdp-mcp/   │  (Python)    │               │ / Desktop    │
│  venster voor jou    │   <pid>.sock   │  MCP-server  │               │              │
└──────────┬───────────┘                └──────────────┘               └──────────────┘
           │ RDP
           ▼
     Windows-machine
```

- **client/**: een kleine RDP-client op basis van libfreerdp3 en SDL3. Hij toont de
  sessie, stuurt jouw muis en toetsenbord door en opent een control-socket.
- **mcp/**: een MCP-server die met die socket praat en tools aanbiedt voor
  screenshot, klikken, typen, sneltoetsen, scrollen en slepen.

Wachtwoorden en certificaatvragen lopen via de terminal waarin je de client start.
Ze komen nooit bij de AI terecht.

## Installeren (macOS)

```sh
brew install freerdp cmake
cd client
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix ~/.local   # optioneel: zet rdp-mcp-client in ~/.local/bin

cd ../mcp
uv sync
```

Op Linux werkt het ook. Je hebt dan de ontwikkelpakketten van FreeRDP 3 en SDL3 nodig.

## Gebruik

1. **Start de client.** Hij gebruikt de gewone FreeRDP-opties:

   ```sh
   rdp-mcp-client /v:server.voorbeeld.be /u:jan /d:INT /kbd:layout:0x80C
   ```

   Je krijgt de wachtwoordvraag in de terminal. Na het verbinden opent het venster
   en verschijnt `MCP-control-socket: ~/.rdp-mcp/<pid>.sock`.

   Handige opties:
   - `/size:1920x1080`: resolutie van de remote sessie
   - `/kbd:layout:0x80C`: Belgisch (azerty) toetsenbord op de server
   - `/cert:tofu` of `/cert:ignore`: certificaat de eerste keer vertrouwen of niet controleren
   - `/g:gateway.example.be`: via RD Gateway
   - `--ai-off`: start met AI-besturing uitgeschakeld
   - `--cmd-as-win`: de Mac-Cmd-toets wordt de Windows-toets. Standaard is Cmd = Ctrl,
     zodat Cmd+C en Cmd+V werken zoals op de Mac.

   `rdp-mcp-client /help` toont alle opties.

2. **Registreer de MCP-server in Claude Code.** Dit hoeft maar één keer:

   ```sh
   claude mcp add rdp -s user -e RDP_MCP_LAYOUT=be -- uv run --directory ~/Projects/rdp-mcp/mcp rdp-mcp
   ```

   Voor Claude Desktop zet je in `claude_desktop_config.json`:

   ```json
   {
     "mcpServers": {
       "rdp": {
         "command": "uv",
         "args": ["run", "--directory", "/Users/<jij>/Projects/rdp-mcp/mcp", "rdp-mcp"],
         "env": { "RDP_MCP_LAYOUT": "be" }
       }
     }
   }
   ```

3. **Vraag Claude iets te doen**, bijvoorbeeld "open in de RDP-sessie Kladblok en typ
   een testzin". Jij ziet alles live in het venster.

## Meekijken en ingrijpen

- Elke AI-actie licht het venster even **oranje** op en zet een **kruisje** op de plek
  waar de AI de muis neerzette.
- De venstertitel toont of AI-besturing **AAN** of **UIT** staat.
- **Ctrl+Alt+Shift+A** in het venster zet AI-besturing aan of uit. Op de Mac is dat
  Ctrl+Option+Shift+A. Als ze uit staat, kan de AI nog screenshots nemen, maar elke
  muis- of toetsactie wordt geweigerd met een duidelijke melding.
- Je eigen muis en toetsenbord blijven altijd werken.

## MCP-tools

| tool | wat |
|---|---|
| `list_rdp_sessions` | actieve sessies (host, resolutie, AI aan/uit) |
| `screenshot` | afbeelding van het remote scherm, verkleind tot max. 1280 px |
| `click` | klikken (links/rechts/midden, dubbelklik met `clicks=2`) |
| `move_mouse` | muis verplaatsen (hover) |
| `drag` | slepen van A naar B |
| `scroll` | muiswiel op/neer/links/rechts |
| `type_text` | tekst typen via Unicode, dus onafhankelijk van de toetsenbordindeling |
| `press_keys` | sneltoetsen, bv. `ctrl+c`, `win+r`, `alt+f4`, `ctrl+alt+delete`, `tab tab enter` |
| `wait` | even wachten tot iets geladen is |

Coördinaten zijn altijd pixels in de laatste screenshot. De MCP-server rekent ze om
naar de echte resolutie.

## Instellingen (omgevingsvariabelen van de MCP-server)

| variabele | standaard | betekenis |
|---|---|---|
| `RDP_MCP_LAYOUT` | `be` | indeling van de remote sessie voor letters in sneltoetsen (`be` = azerty, `us` = qwerty) |
| `RDP_MCP_MAX_EDGE` | `1280` | langste zijde van screenshots in pixels |
| `RDP_MCP_IMAGE_FORMAT` | `png` | `png` of `jpeg` (kleiner, minder scherp) |
| `RDP_MCP_DIR` | `~/.rdp-mcp` | map met de sockets (ook voor de client) |

## Beveiliging

- De socket staat in `~/.rdp-mcp/` met rechten `0700`/`0600`. Alleen jouw eigen
  gebruiker kan erbij, en er gaat niets over het netwerk.
- Iedereen die die socket kan openen, kan de sessie besturen. Gebruik dus een
  account met niet meer rechten dan nodig.
- Wachtwoorden typ je zelf in de terminal of in het venster. De MCP-server kent ze niet.

## Beperkingen (v0.1)

- Geen klembord, geluid, drive-redirectie of dynamische resolutie. Een groter
  venster schaalt het beeld, maar de remote resolutie blijft gelijk.
- De muiscursor in het venster is de lokale systeemcursor; de cursorvorm van de server wordt niet overgenomen.
- Zonder Unicode-ondersteuning op de server (Windows heeft die wel) kan
  `type_text` alleen letters, cijfers en spaties typen.

## Testen zonder Windows

FreeRDP levert een voorbeeldserver mee met een testbeeld:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -keyout /tmp/s.key -out /tmp/s.crt -days 7 -subj /CN=localhost
cd "$(brew --prefix freerdp)/share/FreeRDP/images"
sfreerdp-server --port=33890 --cert=/tmp/s.crt --key=/tmp/s.key
rdp-mcp-client /v:127.0.0.1:33890 /cert:ignore /sec:tls /u:test /p:test
```

Let op: deze testserver luistert op alle netwerkinterfaces en ondersteunt geen
Unicode-invoer.

Het socketprotocol staat in [PROTOCOL.md](PROTOCOL.md).
