#!/bin/sh
# Bouwt "RDP MCP.app" in mac/build/ (ad-hoc ondertekend, voor eigen gebruik).
# Vereist: brew install freerdp
set -eu
cd "$(dirname "$0")/.."

CONFIG="${1:-release}"
swift build -c "$CONFIG" --product RDPMCP
BIN="$(swift build -c "$CONFIG" --show-bin-path)/RDPMCP"

APP="mac/build/RDP MCP.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/RDPMCP"
cp mac/Info.plist "$APP/Contents/Info.plist"

# Ad-hoc handtekening: macOS vraagt na elke nieuwe build één keer of de app
# bij de sleutelhanger mag.
codesign --force --sign - --timestamp=none "$APP"
echo "Klaar: $APP"
