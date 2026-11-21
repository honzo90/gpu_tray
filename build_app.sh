#!/bin/bash
# GPU Tray — budowanie, pakowanie i instalacja aplikacji menu bar.
# Wymagania: Xcode (swift) — zero innych zależności.
#
# Użycie:
#   ./build_app.sh              → buduje + instaluje do /Applications (fallback: ~/Applications)
#   ./build_app.sh --user       → instaluje do ~/Applications
#   ./build_app.sh <katalog>    → instaluje do wskazanego katalogu
#
# APP_VERSION (np. 1.2.0; domyślnie z ostatniego taga) i APP_BUILD (liczba) ustawiają wersję w Info.plist.
set -euo pipefail
cd "$(dirname "$0")"

# domyślnie wersja z ostatniego taga gita (v1.2.0 → 1.2.0); CI podaje ją z taga wydania
APP_VERSION="${APP_VERSION:-$( (git describe --tags --abbrev=0 2>/dev/null || true) | sed 's/^v//')}"
APP_VERSION="${APP_VERSION:-0.0.0}"
APP_BUILD="${APP_BUILD:-1}"

echo "▸ Budowanie (release, arm64)…"
swift build -c release

BIN=".build/release/gpu-tray"
# Paczkę składamy i podpisujemy w katalogu tymczasowym, poza repo: jeśli repo leży w iCloud Drive
# (np. Biurko/Dokumenty), File Provider od razu dokleja do bundla atrybuty (com.apple.FinderInfo,
# com.apple.fileprovider.*), a `codesign --verify --strict` odrzuca je jako "detritus".
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
APP_ROOT="$STAGE/GPUTray.app"
mkdir -p "$APP_ROOT/Contents/MacOS"

echo "▸ Pakowanie…"
cp "$BIN" "$APP_ROOT/Contents/MacOS/GPUTray"
cat > "$APP_ROOT/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>GPUTray</string>
    <key>CFBundleIdentifier</key><string>com.gputray.app</string>
    <key>CFBundleName</key><string>GPU Tray</string>
    <key>CFBundleDisplayName</key><string>GPU Tray</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>${APP_VERSION}</string>
    <key>CFBundleVersion</key><string>${APP_BUILD}</string>
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
codesign --verify --strict "$APP_ROOT"

DEST=""
case "${1:-}" in
  "") if [ -w /Applications ]; then DEST="/Applications"; else DEST="$HOME/Applications"; fi ;;
  --user) DEST="$HOME/Applications" ;;
  *) DEST="$1" ;;
esac
mkdir -p "$DEST"
rm -rf "$DEST/GPUTray.app"
echo "▸ Instalacja do ${DEST}…"
# ditto bez atrybutów i resource forków — kopia przechodzi `codesign --verify --strict`
# (cp -R przenosił FinderInfo/fileprovider z kopii w iCloud)
ditto --noextattr --norsrc "$APP_ROOT" "$DEST/GPUTray.app"
if ! codesign --verify --strict "$DEST/GPUTray.app" 2>/dev/null; then
    echo "⚠ $DEST jest w iCloud Drive — macOS dokleja atrybuty, --strict nie przejdzie (podpis sam w sobie OK)"
fi

echo ""
echo "✓ Gotowe: $DEST/GPUTray.app"
echo "  Uruchomienie:   open \"$DEST/GPUTray.app\""
echo "  Diagnostyka:    \"$DEST/GPUTray.app/Contents/MacOS/GPUTray\" --once"
