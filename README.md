# rdp-mcp

Een RDP-client waarin een AI-agent (zoals Claude) via MCP mee kan kijken en de
muis en het toetsenbord kan bedienen, terwijl jij live meekijkt in een gewoon
venster en op elk moment zelf kunt ingrijpen.

```
┌──────────────────────┐  Unix-socket    ┌──────────────┐   stdio/MCP   ┌──────────────┐
│  RDP MCP.app (tabs)  │ ◄─────────────► │  rdp-mcp     │ ◄───────────► │ Claude Code  │
│  of rdp-mcp-client   │  ~/.rdp-mcp/    │  (Python)    │               │ / Desktop    │
│  venster voor jou    │  <pid>-<n>.sock │  MCP-server  │               │              │
└──────────┬───────────┘  (per sessie)   └──────────────┘               └──────────────┘
           │ RDP
           ▼
     Windows-machines
```

- **mac/**: de Mac-app "RDP MCP" met adresboek, wachtwoorden in de sleutelhanger,
  tabs en volledig scherm.
- **core/**: de gedeelde C-kern op basis van libfreerdp3. Ze beheert sessies, beeld,
  invoer, klembord en een control-socket per sessie.
- **client/**: een eenvoudige SDL3-client voor één sessie, met de gewone
  FreeRDP-opties. Handig voor tests en op Linux.
- **mcp/**: een MCP-server die met de sockets praat en tools aanbiedt voor
  screenshot, klikken, typen, sneltoetsen, scrollen, slepen en het klembord.

Wachtwoorden en certificaatvragen lopen via de app of de terminal. Ze komen nooit
bij de AI terecht.

## Mac-app

```sh
brew install freerdp
./mac/build-app.sh            # maakt mac/build/RDP MCP.app
open "mac/build/RDP MCP.app"
```

- **Adresboek** in de zijbalk, met groepen. Je bewaart naam, host, poort, gebruiker,
  domein, gateway, resolutie, toetsenbord en opties in
  `~/Library/Application Support/rdp-mcp/hosts.json`. Wachtwoorden staan daar nooit in.
- **Wachtwoorden** gaan naar je login-sleutelhanger, versleuteld door macOS. Optioneel
  vraagt de app Touch ID of je Mac-wachtwoord voor ze gebruikt worden. Dat is een
  controle in de app zelf: voor een harde sleutelhangerregel moet de app ondertekend
  zijn met een Apple Developer-account. Omdat de app ad-hoc ondertekend is, vraagt
  macOS na elke nieuwe build één keer of ze bij de sleutelhanger mag.
- **Tabs:** één tab per sessie. Wisselen met ⌥⌘← / ⌥⌘→, sluiten met ⌥⌘W, opnieuw
  verbinden met ⌥⌘R.
- **Volledig scherm** met ⌃⌘F of de knop rechts in de tabbalk. Zijbalk en tabbalk
  verdwijnen dan; beweeg de muis naar de bovenrand om de tabs te zien.
- **Dynamische resolutie:** met "Automatisch" volgt de Windows-resolutie de grootte
  van het tabblad, ook bij volledig scherm.
- **Toetsenbord:** alles gaat naar Windows, ook Cmd-combinaties (Cmd = Ctrl, in te
  stellen in Instellingen). Alleen ⌥⌘-combinaties, ⌃⌘F en ⌘Q blijven op de Mac.
- **AI-besturing** per tab: de schakelaar in de statusbalk, ⌥⌘A of ⌃⌥⇧A in de sessie.
  In de MCP-tools heet een sessie zoals in het adresboek, zodat je Claude kunt vragen
  om "in PC01" iets te doen.

## SDL-client installeren

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
   rdp-mcp-client /v:server.voorbeeld.be /u:jan /d:VOORBEELD /kbd:layout:0x80C
   ```

   Je krijgt de wachtwoordvraag in de terminal. Na het verbinden opent het venster
   en verschijnt `MCP-control-socket: ~/.rdp-mcp/<pid>-1.sock`.

   Handige opties:
   - `/size:1920x1080`: resolutie van de remote sessie
   - `/kbd:layout:0x80C`: Belgisch (azerty) toetsenbord op de server
   - `/cert:tofu` of `/cert:ignore`: certificaat de eerste keer vertrouwen of niet controleren
   - `/g:gateway.example.be`: via RD Gateway
   - `--ai-off`: start met AI-besturing uitgeschakeld
   - `--cmd-as-win`: de Mac-Cmd-toets wordt de Windows-toets. Standaard is Cmd = Ctrl,
     zodat Cmd+C en Cmd+V werken zoals op de Mac.
   - `--no-clipboard`: het klembord helemaal niet delen, ook niet met de AI

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
| `get_clipboard` | tekst op het remote klembord lezen, bv. na `ctrl+c` |
| `set_clipboard` | tekst op het remote klembord zetten, daarna plakken met `ctrl+v` |
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
| `RDP_MCP_SETTLE` | `0.4` | seconden die een screenshot minstens wacht na de laatste actie |
| `RDP_MCP_DIR` | `~/.rdp-mcp` | map met de sockets (ook voor de client) |

## Klembord

Tekst wordt in beide richtingen gedeeld, zoals bij een gewone RDP-client. Kopieer je
op de Mac, dan plak je het remote met Cmd+V; kopieer je remote, dan staat het op het
klembord van je Mac.

Voor de AI gelden twee regels:

- **De AI leest je Mac-klembord nooit.** `get_clipboard` geeft alleen tekst terug die
  in de remote sessie gekopieerd werd of die de AI zelf zette. Zolang het klembord
  iets bevat dat jij op de Mac kopieerde, krijgt de AI een weigering.
- **De AI kan je Mac-klembord niet remote plakken.** Drukt de AI zelf Ctrl+V terwijl
  je Mac-inhoud actief is, dan geeft de client die inhoud niet door en meldt dat in
  de terminal. Plak jij zelf, dan werkt het gewoon. De client beslist dat op basis van
  wie de laatste toets of klik gaf.

`set_clipboard` verandert alleen het remote klembord; het klembord van je Mac blijft
zoals het was. Als AI-besturing uit staat, kan de AI het klembord lezen noch zetten.

## Beveiliging

- De socket staat in `~/.rdp-mcp/` met rechten `0700`/`0600`. Alleen jouw eigen
  gebruiker kan erbij, en er gaat niets over het netwerk.
- Iedereen die die socket kan openen, kan de sessie besturen. Gebruik dus een
  account met niet meer rechten dan nodig.
- Wachtwoorden typ je zelf in de terminal of in het venster. De MCP-server kent ze niet.

## Beperkingen (v0.1)

- Het klembord deelt alleen tekst, geen afbeeldingen of bestanden.
- Geen geluid, drive-redirectie of dynamische resolutie. Een groter
  venster schaalt het beeld, maar de remote resolutie blijft gelijk.
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
