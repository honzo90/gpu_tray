#!/bin/bash
# gpu_cyd — kompilacja i wgranie firmware do CYD (ESP32-2432S028R, ekran 320x240 + dotyk).
# Wymagania: arduino-cli z rdzeniem esp32:esp32 oraz bibliotekami LovyanGFX i ArduinoJson.
#
# Użycie:
#   ./build_esp32.sh              → kompilacja + wgranie (do /dev/cu.usbserial-*)
#   ./build_esp32.sh build        → sama kompilacja
#   ./build_esp32.sh monitor       → podgląd portu szeregowego (wyjście: Ctrl-])
#   ./build_esp32.sh erase        → wymazanie całego flasha (NVS + stare dane)
#
# Zmienna CYD_PORT pozwala wskazać port ręcznie, np. CYD_PORT=/dev/cu.usbserial-XX ./build_esp32.sh
set -euo pipefail
cd "$(dirname "$0")"

# Minimal SPIFFS: 2 sloty aplikacji po 1,9 MB (OTA) — firmware z HTTPS (aktualizacje z GitHuba)
# nie mieści się w domyślnych 1,25 MB. NVS (ustawienia) zostaje pod tym samym adresem.
FQBN="esp32:esp32:esp32:PartitionScheme=min_spiffs"
PORT="${CYD_PORT:-$(ls /dev/cu.usbserial-* 2>/dev/null | head -1 || true)}"

case "${1:-}" in
  build)
    arduino-cli compile --fqbn "$FQBN" --warnings default gpu_cyd
    ;;
  erase)
    [ -n "$PORT" ] || { echo "✗ Nie znaleziono portu /dev/cu.usbserial-*"; exit 1; }
    esptool --port "$PORT" erase_flash
    ;;
  monitor)
    [ -n "$PORT" ] || { echo "✗ Nie znaleziono portu /dev/cu.usbserial-*"; exit 1; }
    arduino-cli monitor -p "$PORT" --config baudrate=115200
    ;;
  *)
    arduino-cli compile --fqbn "$FQBN" --warnings default gpu_cyd
    [ -n "$PORT" ] || { echo "✗ Nie znaleziono portu /dev/cu.usbserial-* — podłącz CYD."; exit 1; }
    arduino-cli upload -p "$PORT" --fqbn "$FQBN" gpu_cyd
    echo "✓ Firmware wgrany na $PORT (monitor: ./build_esp32.sh monitor)"
    ;;
esac
