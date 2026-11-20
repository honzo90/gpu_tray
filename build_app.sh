#!/bin/bash
# GPU Tray — budowanie, pakowanie i instalacja aplikacji menu bar.
# Wymagania: Xcode (swift) — zero innych zależności.
#
# Użycie:
#   ./build_app.sh              → buduje + instaluje do /Applications (fallback: ~/Applications)
#   ./build_app.sh --user       → instaluje do ~/Applications
#   ./build_app.sh <katalog>    → instaluje do wskazanego katalogu
set -euo pipefail
cd "$(dirname "$0")"

echo "▸ Budowanie (release, arm64)…"
swift build -c release

BIN=".build/release/gpu-tray"
APP_ROOT="build/GPUTray.app"
rm -rf "$APP_ROOT"
mkdir -p "$APP_ROOT/Contents/MacOS"

echo "▸ Pakowanie ${APP_ROOT}…"
cp "$BIN" "$APP_ROOT/Contents/MacOS/GPUTray"
cat > "$APP_ROOT/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>GPUTray</string>
    <key>CFBundleIdentifier</key><string>com.gputray.app</string>
    <key>CFBundleName</key><string>GPU Tray</string>
    <key>CFBundleDisplayName</key><string>GPU Tray</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>LSUIElement</key><true/>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSAppTransportSecurity</key>
    <dict><key>NSAllowsArbitraryLoads</key><true/></dict>
    <key>NSLocalNetworkUsageDescription</key>
    <string>Aplikacja łączy się z lokalnym serwera Ollama, aby wyświetlić status modeli i użycie GPU.</string>
</dict>
</plist>
PLIST

# Podpis stałym certyfikatem → stała tożsamość (identyfikator + certyfikat), więc zgody
# macOS (Sieć lokalna, powiadomienia) przetrwają przebudowy. Ad-hoc wiąże zgody z cdhash,
# który zmienia się przy każdym buildzie → okienko „Sieć lokalna" wraca.
# SIGN_ID=<SHA-1 lub nazwa> wymusza certyfikat; domyślnie pierwszy „Apple Development".
SIGN_ID="${SIGN_ID:-$(security find-identity -v -p codesigning 2>/dev/null | awk '/Apple Development/{print $2; exit}')}"
if [ -n "$SIGN_ID" ]; then
    echo "▸ Podpisywanie certyfikatem ${SIGN_ID}…"
    codesign --force --sign "$SIGN_ID" --timestamp=none "$APP_ROOT"
else
    echo "⚠ Brak certyfikatu do podpisu — podpis ad-hoc (macOS zapyta o Sieć lokalną po każdym buildzie)"
    codesign --force --sign - --timestamp=none "$APP_ROOT"
fi

DEST=""
case "${1:-}" in
  "") if [ -w /Applications ]; then DEST="/Applications"; else DEST="$HOME/Applications"; fi ;;
  --user) DEST="$HOME/Applications" ;;
  *) DEST="$1" ;;
esac
mkdir -p "$DEST"
rm -rf "$DEST/GPUTray.app"
echo "▸ Instalacja do ${DEST}…"
cp -R "$APP_ROOT" "$DEST/GPUTray.app"

echo ""
echo "✓ Gotowe: $DEST/GPUTray.app"
echo "  Uruchomienie:   open \"$DEST/GPUTray.app\""
echo "  Diagnostyka:    \"$DEST/GPUTray.app/Contents/MacOS/GPUTray\" --once"
