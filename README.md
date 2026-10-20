# rdp-mcp

An RDP client for the Mac that lets an AI agent (such as Claude) see the remote
screen and drive the mouse and keyboard over MCP. You watch live in a normal window
and can take over at any moment.

Most people need two steps: install the Mac app and register the MCP server with
Claude once. The standalone SDL client further down is an extra for testing and
Linux; you don't need it.

## Install

You need [Homebrew](https://brew.sh) and the Xcode Command Line Tools
(`xcode-select --install`).

```sh
brew install freerdp uv
git clone https://github.com/solanex/rdp-mcp.git ~/Projects/rdp-mcp
cd ~/Projects/rdp-mcp
./mac/build-app.sh            # builds mac/build/RDP MCP.app
open "mac/build/RDP MCP.app"
```

You can drag the app to `/Applications`. It also works as a plain RDP client
without Claude.

## Connect Claude

You only do this once. For Claude Code:

```sh
claude mcp add rdp -s user -e RDP_MCP_LAYOUT=be -- uv run --directory ~/Projects/rdp-mcp/mcp rdp-mcp
```

For Claude Desktop, add this to `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "rdp": {
      "command": "uv",
      "args": ["run", "--directory", "/Users/<you>/Projects/rdp-mcp/mcp", "rdp-mcp"],
      "env": { "RDP_MCP_LAYOUT": "be" }
    }
  }
}
```

If you cloned the repo somewhere else, adjust the path. Use `RDP_MCP_LAYOUT=us` if
the remote session uses a QWERTY layout. `uv` installs the Python dependencies by
itself on first start.

## Usage

1. Add a host to the address book in the app and connect.
2. Turn on **AI control** for that tab: the switch in the status bar, ⌥⌘A, or
   ⌃⌥⇧A inside the session.
3. Ask Claude something, for example "open Notepad in PC01 and type a test
   sentence". In the MCP tools a session has the same name as in the address book.

Every AI action briefly outlines the screen in orange and puts a crosshair where the
AI placed the mouse. Your own mouse and keyboard always keep working. With AI
control off, the AI can still take screenshots, but every mouse or keyboard action
is refused.

## What the app does

- **Address book** in the sidebar, with groups. Name, host, port, user, domain,
  gateway, resolution, keyboard and options are stored in
  `~/Library/Application Support/rdp-mcp/hosts.json`. Passwords are never stored there.
- **Passwords** go to your login keychain. Optionally the app asks for Touch ID or
  your Mac password before using them. That check happens in the app itself: a hard
  keychain rule would require signing the app with an Apple Developer account.
  Because the app is ad-hoc signed, macOS asks once after every new build whether it
  may access the keychain.
- **Tabs:** one tab per session. Switch with ⌥⌘← / ⌥⌘→, close with ⌥⌘W, reconnect
  with ⌥⌘R.
- **Full screen** with ⌃⌘F or the button on the right of the tab bar. The sidebar and
  tab bar disappear; move the mouse to the top edge to see the tabs.
- **Separate window:** ⌥⌘D, the window icon in the status bar or a tab's context menu
  moves a session into its own window, for example for a second display. Close that
  window and the session returns as a tab; the connection stays open.
- **Sound** from the session plays on the Mac (can be turned off per host).
- **Shared folder:** per host you pick a Mac folder that shows up in Windows under
  This PC. With AI control on, the AI can reach those files through Explorer too.
- **Dynamic resolution:** with "Automatisch" (automatic) the Windows resolution
  follows the size of the tab, also in full screen.
- **Keyboard:** everything goes to Windows, including Cmd combinations (Cmd = Ctrl,
  configurable in Settings). Only ⌥⌘ combinations, ⌃⌘F and ⌘Q stay on the Mac.

## Clipboard

Text and images go both ways, as with any RDP client. Copy on the Mac and paste
remotely with Cmd+V; copy remotely and it is on your Mac clipboard.

Two rules apply to the AI:

- **The AI never reads your Mac clipboard.** `get_clipboard` only returns text that
  was copied in the remote session or that the AI set itself. As long as the
  clipboard holds something you copied on the Mac, the AI gets a refusal.
- **The AI cannot paste your Mac clipboard remotely.** If the AI presses Ctrl+V while
  Mac content is active, the app does not pass that content on. If you paste
  yourself, it just works. The app decides based on who gave the last key press or
  click.

`set_clipboard` only changes the remote clipboard; your Mac clipboard stays as it
was. With AI control off, the AI can neither read nor set the clipboard.

## Security

- Passwords and certificate prompts go through the app. The MCP server and the AI
  never see them.
- Each session has a socket in `~/.rdp-mcp/` with permissions `0700`/`0600`. Only
  your own user can reach it, and nothing goes over the network.
- Anyone who can open that socket can control the session. Use an account with no
  more rights than needed.

## MCP tools

| tool | what |
|---|---|
| `list_rdp_sessions` | active sessions (name, resolution, AI on/off) |
| `screenshot` | image of the remote screen, scaled down to max. 1280 px |
| `click` | click (left/right/middle, double-click with `clicks=2`) |
| `move_mouse` | move the mouse (hover) |
| `drag` | drag from A to B |
| `scroll` | mouse wheel up/down/left/right |
| `get_clipboard` | read text on the remote clipboard, e.g. after `ctrl+c` |
| `set_clipboard` | put text on the remote clipboard, then paste with `ctrl+v` |
| `type_text` | type text via Unicode, independent of the keyboard layout |
| `press_keys` | shortcuts, e.g. `ctrl+c`, `win+r`, `alt+f4`, `ctrl+alt+delete`, `tab tab enter` |
| `wait` | wait a moment for something to load |

Coordinates are always pixels in the latest screenshot. The MCP server converts them
to the real resolution.

### Settings

Environment variables of the MCP server, passed with `-e` to `claude mcp add` or
under `env` in Claude Desktop:

| variable | default | meaning |
|---|---|---|
| `RDP_MCP_LAYOUT` | `be` | layout of the remote session, used for letters in shortcuts (`be` = AZERTY, `us` = QWERTY) |
| `RDP_MCP_MAX_EDGE` | `1280` | longest side of screenshots in pixels |
| `RDP_MCP_IMAGE_FORMAT` | `png` | `png` or `jpeg` (smaller, less sharp) |
| `RDP_MCP_SETTLE` | `0.4` | minimum seconds a screenshot waits after the last action |
| `RDP_MCP_DIR` | `~/.rdp-mcp` | directory with the sockets |

## Limitations

- Copying files through the clipboard is not supported; use the shared folder.
- The AI can only read text from the clipboard, not images.
- No microphone.
- Without Unicode support on the server (Windows has it), `type_text` can only type
  letters, digits and spaces.

## Extra: the SDL client

Besides the Mac app there is a simple single-session client built with SDL3 and the
usual FreeRDP options. It is handy for testing and also runs on Linux. The MCP setup
above works with it in the same way.

```sh
brew install freerdp cmake
cd client
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix ~/.local   # puts rdp-mcp-client in ~/.local/bin
```

On Linux you need the development packages of FreeRDP 3 and SDL3.

```sh
rdp-mcp-client /v:server.example.com /u:jane /d:EXAMPLE /kbd:layout:0x80C
```

It asks for the password in the terminal. After connecting, the window opens and it
prints `MCP-control-socket: ~/.rdp-mcp/<pid>-1.sock`. The window title shows whether
AI control is on or off; Ctrl+Alt+Shift+A (Ctrl+Option+Shift+A on the Mac) toggles
it.

Useful options:

- `/size:1920x1080`: resolution of the remote session
- `/kbd:layout:0x80C`: Belgian (AZERTY) keyboard on the server (`0x409` for US)
- `/cert:tofu` or `/cert:ignore`: trust the certificate the first time, or don't check it
- `/g:gateway.example.com`: connect through an RD Gateway
- `--ai-off`: start with AI control disabled
- `--cmd-as-win`: the Mac Cmd key becomes the Windows key (default is Cmd = Ctrl)
- `--no-clipboard`: don't share the clipboard at all, not even with the AI

`rdp-mcp-client /help` lists all options.

Differences from the Mac app: the SDL client only shares text through the clipboard,
and the remote resolution does not follow the window size (a bigger window scales
the image).

### Testing without Windows

FreeRDP ships a sample server with a test image:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -keyout /tmp/s.key -out /tmp/s.crt -days 7 -subj /CN=localhost
cd "$(brew --prefix freerdp)/share/FreeRDP/images"
sfreerdp-server --port=33890 --cert=/tmp/s.crt --key=/tmp/s.key
rdp-mcp-client /v:127.0.0.1:33890 /cert:ignore /sec:tls /u:test /p:test
```

This test server listens on all network interfaces and does not support Unicode
input.

## Repository layout

```
┌──────────────────────┐  Unix socket    ┌──────────────┐   stdio/MCP   ┌──────────────┐
│  RDP MCP.app (tabs)  │ ◄─────────────► │  rdp-mcp     │ ◄───────────► │ Claude Code  │
│  or rdp-mcp-client   │  ~/.rdp-mcp/    │  (Python)    │               │ / Desktop    │
│  window for you      │  <pid>-<n>.sock │  MCP server  │               │              │
└──────────┬───────────┘  (per session)  └──────────────┘               └──────────────┘
           │ RDP
           ▼
     Windows machines
```

- `mac/`: the Mac app.
- `core/`: the shared C core on top of libfreerdp3. It manages sessions, display,
  input, clipboard and the control socket per session.
- `mcp/`: the MCP server that talks to the sockets.
- `client/`: the SDL client.

The socket protocol is described in [PROTOCOL.md](PROTOCOL.md).
