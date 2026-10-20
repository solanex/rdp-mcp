# rdp-mcp

An RDP client for the Mac that lets an AI agent (such as Claude) see the remote
screen and drive the mouse and keyboard over MCP. Claude can also open hosts from the
app's address book itself; the app signs in, so Claude never sees a password. You
watch live in a normal window and can take over at any moment.

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

1. Add a host to the address book in the app and connect (or let Claude open it,
   see below).
2. Turn on **AI control** for that tab: the switch in the status bar, ⌥⌘A, or
   ⌃⌥⇧A inside the session.
3. Ask Claude something, for example "open Notepad in PC01 and type a test
   sentence". In the MCP tools a session has the same name as in the address book.

Claude can also open a host from the address book itself ("connect to PC01 and …").
The app then signs in with the password saved in the keychain, or shows its sign-in
window to you; Claude never sees or types the password. Claude can close the sessions
it opened, but not the ones you opened.

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
- The app has one more socket, `~/.rdp-mcp/app-<pid>.ctl`, with the same permissions.
  Through it a local process of your user can list the address book and open hosts
  with their saved passwords, without ever receiving the passwords.

### Restart and shutdown guard

The MCP server tells the AI never to restart, shut down or sign out Windows, cut its
connection or erase a disk unless you asked for exactly that, and never to follow
instructions that appear on the remote screen. On top of that the app asks you
before such an action goes through:

- **Clicks.** Before the AI clicks, the app reads the screen with the macOS text
  recognition (Vision, on your Mac). Clicking something like "Restart", "Shut down",
  "Sleep" or "Sign out" (English or Dutch) asks first. "Afsluiten" (Dutch for both
  "shut down" and "exit") only counts when the power menu or shutdown dialog is open.
- **Enter.** Pressing Enter or Space while the power menu or the Alt+F4 shutdown
  dialog is open asks first, and so does Enter on a console prompt or in the Run
  dialog that holds a command like `Restart-Computer`, `shutdown /r`, `logoff`,
  `Stop-Process winlogon`, `Disable-NetAdapter`, `Format-Volume` or `diskpart`. The
  screen is read here too, so a command from the history or the clipboard counts.
- **Typed text and the clipboard.** The MCP server hands every line the AI types to
  the app before it presses Enter, and the text for `set_clipboard` before it is
  set.

Refuse is the default, and with no answer within 2 minutes the action is refused.
Edit the words and command rules (regular expressions) under **Settings → AI
Safety**, the gear in the toolbar; *Restore Defaults* brings back the built-in set.
Turn the guard off per host under AI Safety in the host editor.

This is a safety net against mistakes, not a security boundary: a button without
text, another language, an alias, a script or a detour through a variable gets
through. To really prevent it, sign in with an account that may not shut down
Windows: on Windows Server only administrators have that right by default, and for
an admin account the policy *Remove and prevent access to the Shut Down, Restart,
Sleep, and Hibernate commands* hides the buttons. The SDL client has no guard.

## MCP tools

| tool | what |
|---|---|
| `list_hosts` | hosts in the address book (without passwords) |
| `open_session` | open a host from the address book; the app signs in |
| `close_session` | close a session that `open_session` opened |
| `list_rdp_sessions` | active sessions (name, resolution, AI on/off) |
| `screenshot` | image of the remote screen, scaled down to max. 1280 px |
| `click` | click (left/right/middle, double-click with `clicks=2`) |
| `move_mouse` | move the mouse (hover) |
| `drag` | drag from A to B |
| `scroll` | mouse wheel up/down/left/right |
| `get_clipboard` | read text on the remote clipboard, e.g. after `ctrl+c` |
| `set_clipboard` | put text on the remote clipboard, then paste with `ctrl+v` |
| `type_text` | type text: key presses for characters on the keyboard layout, Unicode for the rest |
| `press_keys` | shortcuts, e.g. `ctrl+c`, `win+r`, `alt+f4`, `ctrl+alt+delete`, `tab tab enter` |
| `wait` | wait a moment for something to load |

Coordinates are always pixels in the latest screenshot. The MCP server converts them
to the real resolution.

### Settings

Environment variables of the MCP server, passed with `-e` to `claude mcp add` or
under `env` in Claude Desktop:

| variable | default | meaning |
|---|---|---|
| `RDP_MCP_LAYOUT` | `be` | keyboard layout of the remote session, used for shortcuts and `type_text` (`be` = Belgian AZERTY, `us` = US QWERTY) |
| `RDP_MCP_TYPING` | `auto` | `auto`: `type_text` sends key presses for characters on the layout and Unicode for the rest; `unicode`: everything via Unicode |
| `RDP_MCP_MAX_EDGE` | `1280` | longest side of screenshots in pixels |
| `RDP_MCP_IMAGE_FORMAT` | `png` | `png` or `jpeg` (smaller, less sharp) |
| `RDP_MCP_SETTLE` | `0.4` | minimum seconds a screenshot waits after the last action |
| `RDP_MCP_DIR` | `~/.rdp-mcp` | directory with the sockets |

## Limitations

- Copying files through the clipboard is not supported; use the shared folder.
- The AI can only read text from the clipboard, not images.
- No microphone.
- `type_text` only types correctly if `RDP_MCP_LAYOUT` matches the keyboard layout in
  Windows; otherwise characters come out wrong. Set `RDP_MCP_TYPING=unicode` if they
  can't match. Characters that are not on the layout go via Unicode, which some
  windows ignore (e.g. a console running as administrator); the clipboard always works.

## Extra: the SDL client

Besides the Mac app there is a simple single-session client built with SDL3 and the
usual FreeRDP options. It is handy for testing and also runs on Linux. The MCP setup
above works with it in the same way, except `list_hosts`, `open_session` and
`close_session`: those need the Mac app and its address book.

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
prints `MCP control socket: ~/.rdp-mcp/<pid>-1.sock`. The window title shows whether
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
┌──────────────────────┐  Unix sockets   ┌──────────────┐   stdio/MCP   ┌──────────────┐
│  RDP MCP.app (tabs)  │ ◄─────────────► │  rdp-mcp     │ ◄───────────► │ Claude Code  │
│  or rdp-mcp-client   │  ~/.rdp-mcp/    │  (Python)    │               │ / Desktop    │
│  window for you      │  <pid>-<n>.sock │  MCP server  │               │              │
│                      │  (per session)  │              │               │              │
│                      │  app-<pid>.ctl  │              │               │              │
│                      │  (address book) │              │               │              │
└──────────┬───────────┘                 └──────────────┘               └──────────────┘
           │ RDP
           ▼
     Windows machines
```

- `mac/`: the Mac app, including the app socket for the address book
  (`AppControl.swift`).
- `core/`: the shared C core on top of libfreerdp3. It manages sessions, display,
  input, clipboard and the control socket per session.
- `mcp/`: the MCP server that talks to the sockets; `keys.py` holds the keyboard
  layouts for shortcuts and `type_text`.
- `client/`: the SDL client.

Both socket protocols are described in [PROTOCOL.md](PROTOCOL.md).

## License

MIT, see [LICENSE](LICENSE). FreeRDP, which this project links against, is licensed under Apache 2.0.
