# rdp-mcp

Een RDP-client voor de Mac waarin een AI-agent (zoals Claude) via MCP mee kan kijken
en de muis en het toetsenbord kan bedienen. Jij kijkt live mee in een gewoon venster
en kunt op elk moment zelf ingrijpen.

Voor de meeste mensen volstaan twee stappen: de Mac-app installeren en de MCP-server
één keer bij Claude registreren. De losse SDL-client onderaan is een extra voor tests
en Linux; die heb je niet nodig.

## Installeren

Je hebt [Homebrew](https://brew.sh) en de Xcode Command Line Tools nodig
(`xcode-select --install`).

```sh
brew install freerdp uv
git clone https://github.com/solanex/rdp-mcp.git ~/Projects/rdp-mcp
cd ~/Projects/rdp-mcp
./mac/build-app.sh            # maakt mac/build/RDP MCP.app
open "mac/build/RDP MCP.app"
```

Je mag de app naar `/Applications` slepen. Zonder Claude werkt ze ook gewoon als
RDP-client.

## Claude koppelen

Dit hoeft maar één keer. Voor Claude Code:

```sh
claude mcp add rdp -s user -e RDP_MCP_LAYOUT=be -- uv run --directory ~/Projects/rdp-mcp/mcp rdp-mcp
```

Voor Claude Desktop zet je dit in `claude_desktop_config.json`:

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

Clonede je de repo ergens anders, pas dan het pad aan. `uv` installeert de
Python-afhankelijkheden zelf bij de eerste start.

## Gebruik

1. Voeg in de app een host toe aan het adresboek en verbind.
2. Zet **AI-besturing** aan voor die tab: de schakelaar in de statusbalk, ⌥⌘A, of
   ⌃⌥⇧A in de sessie.
3. Vraag Claude iets, bijvoorbeeld "open in PC01 Kladblok en typ een testzin".
   In de MCP-tools heet een sessie zoals in het adresboek.

Elke AI-actie laat het beeld even oranje oplichten en zet een kruisje waar de AI de
muis neerzette. Je eigen muis en toetsenbord blijven altijd werken. Staat
AI-besturing uit, dan kan de AI nog screenshots nemen, maar elke muis- of toetsactie
wordt geweigerd.

## Wat de app kan

- **Adresboek** in de zijbalk, met groepen. Naam, host, poort, gebruiker, domein,
  gateway, resolutie, toetsenbord en opties staan in
  `~/Library/Application Support/rdp-mcp/hosts.json`. Wachtwoorden staan daar nooit in.
- **Wachtwoorden** gaan naar je login-sleutelhanger. Optioneel vraagt de app Touch ID
  of je Mac-wachtwoord voor ze gebruikt worden. Dat is een controle in de app zelf:
  voor een harde sleutelhangerregel moet de app ondertekend zijn met een Apple
  Developer-account. Omdat de app ad-hoc ondertekend is, vraagt macOS na elke nieuwe
  build één keer of ze bij de sleutelhanger mag.
- **Tabs:** één tab per sessie. Wisselen met ⌥⌘← / ⌥⌘→, sluiten met ⌥⌘W, opnieuw
  verbinden met ⌥⌘R.
- **Volledig scherm** met ⌃⌘F of de knop rechts in de tabbalk. Zijbalk en tabbalk
  verdwijnen dan; beweeg de muis naar de bovenrand om de tabs te zien.
- **Eigen venster:** met ⌥⌘D, het venstericoon in de statusbalk of het rechtsklikmenu
  van een tab zet je een sessie in een eigen venster, bijvoorbeeld voor een tweede
  scherm. Sluit je dat venster, dan komt de sessie terug als tab; de verbinding
  blijft open.
- **Geluid** van de sessie speelt af op de Mac (per host uit te zetten).
- **Gedeelde map:** per host kies je een Mac-map die in Windows onder Deze pc
  verschijnt. Met AI-besturing aan kan de AI via de Verkenner ook bij die bestanden.
- **Dynamische resolutie:** met "Automatisch" volgt de Windows-resolutie de grootte
  van het tabblad, ook bij volledig scherm.
- **Toetsenbord:** alles gaat naar Windows, ook Cmd-combinaties (Cmd = Ctrl, in te
  stellen in Instellingen). Alleen ⌥⌘-combinaties, ⌃⌘F en ⌘Q blijven op de Mac.

## Klembord

Tekst en afbeeldingen gaan in beide richtingen, zoals bij een gewone RDP-client.
Kopieer je op de Mac, dan plak je remote met Cmd+V; kopieer je remote, dan staat het
op het klembord van je Mac.

Voor de AI gelden twee regels:

- **De AI leest je Mac-klembord nooit.** `get_clipboard` geeft alleen tekst terug die
  in de remote sessie gekopieerd werd of die de AI zelf zette. Zolang het klembord
  iets bevat dat jij op de Mac kopieerde, krijgt de AI een weigering.
- **De AI kan je Mac-klembord niet remote plakken.** Drukt de AI zelf Ctrl+V terwijl
  je Mac-inhoud actief is, dan geeft de app die inhoud niet door. Plak jij zelf, dan
  werkt het gewoon. De app beslist dat op basis van wie de laatste toets of klik gaf.

`set_clipboard` verandert alleen het remote klembord; het klembord van je Mac blijft
zoals het was. Als AI-besturing uit staat, kan de AI het klembord lezen noch zetten.

## Beveiliging

- Wachtwoorden en certificaatvragen lopen via de app. De MCP-server en de AI krijgen
  ze nooit te zien.
- Elke sessie heeft een socket in `~/.rdp-mcp/` met rechten `0700`/`0600`. Alleen
  jouw eigen gebruiker kan erbij, en er gaat niets over het netwerk.
- Wie die socket kan openen, kan de sessie besturen. Gebruik dus een account met niet
  meer rechten dan nodig.

## MCP-tools

| tool | wat |
|---|---|
| `list_rdp_sessions` | actieve sessies (naam, resolutie, AI aan/uit) |
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

### Instellingen

Omgevingsvariabelen van de MCP-server, mee te geven met `-e` bij `claude mcp add`
of onder `env` in Claude Desktop:

| variabele | standaard | betekenis |
|---|---|---|
| `RDP_MCP_LAYOUT` | `be` | indeling van de remote sessie voor letters in sneltoetsen (`be` = azerty, `us` = qwerty) |
| `RDP_MCP_MAX_EDGE` | `1280` | langste zijde van screenshots in pixels |
| `RDP_MCP_IMAGE_FORMAT` | `png` | `png` of `jpeg` (kleiner, minder scherp) |
| `RDP_MCP_SETTLE` | `0.4` | seconden die een screenshot minstens wacht na de laatste actie |
| `RDP_MCP_DIR` | `~/.rdp-mcp` | map met de sockets |

## Beperkingen

- Bestanden kopiëren via het klembord kan niet; gebruik daarvoor de gedeelde map.
- De AI kan alleen tekst van het klembord lezen, geen afbeeldingen.
- Geen microfoon.
- Zonder Unicode-ondersteuning op de server (Windows heeft die wel) kan
  `type_text` alleen letters, cijfers en spaties typen.

## Extra: de SDL-client

Naast de Mac-app is er een eenvoudige client voor één sessie, gebouwd met SDL3 en de
gewone FreeRDP-opties. Hij is handig voor tests en werkt ook op Linux. De
MCP-koppeling hierboven werkt er op dezelfde manier mee.

```sh
brew install freerdp cmake
cd client
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix ~/.local   # zet rdp-mcp-client in ~/.local/bin
```

Op Linux heb je de ontwikkelpakketten van FreeRDP 3 en SDL3 nodig.

```sh
rdp-mcp-client /v:server.voorbeeld.be /u:jan /d:VOORBEELD /kbd:layout:0x80C
```

Het wachtwoord vraagt hij in de terminal. Na het verbinden opent het venster en
verschijnt `MCP-control-socket: ~/.rdp-mcp/<pid>-1.sock`. De venstertitel toont of
AI-besturing aan of uit staat; Ctrl+Alt+Shift+A (op de Mac Ctrl+Option+Shift+A)
schakelt ze om.

Handige opties:

- `/size:1920x1080`: resolutie van de remote sessie
- `/kbd:layout:0x80C`: Belgisch (azerty) toetsenbord op de server
- `/cert:tofu` of `/cert:ignore`: certificaat de eerste keer vertrouwen of niet controleren
- `/g:gateway.example.be`: via RD Gateway
- `--ai-off`: start met AI-besturing uitgeschakeld
- `--cmd-as-win`: de Mac-Cmd-toets wordt de Windows-toets (standaard is Cmd = Ctrl)
- `--no-clipboard`: het klembord helemaal niet delen, ook niet met de AI

`rdp-mcp-client /help` toont alle opties.

Verschillen met de Mac-app: de SDL-client deelt alleen tekst via het klembord, en de
remote resolutie volgt de venstergrootte niet (een groter venster schaalt het beeld).

### Testen zonder Windows

FreeRDP levert een voorbeeldserver mee met een testbeeld:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -keyout /tmp/s.key -out /tmp/s.crt -days 7 -subj /CN=localhost
cd "$(brew --prefix freerdp)/share/FreeRDP/images"
sfreerdp-server --port=33890 --cert=/tmp/s.crt --key=/tmp/s.key
rdp-mcp-client /v:127.0.0.1:33890 /cert:ignore /sec:tls /u:test /p:test
```

Deze testserver luistert op alle netwerkinterfaces en ondersteunt geen Unicode-invoer.

## Opbouw van de repo

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

- `mac/`: de Mac-app.
- `core/`: de gedeelde C-kern op basis van libfreerdp3. Ze beheert sessies, beeld,
  invoer, klembord en de control-socket per sessie.
- `mcp/`: de MCP-server die met de sockets praat.
- `client/`: de SDL-client.

Het socketprotocol staat in [PROTOCOL.md](PROTOCOL.md).
