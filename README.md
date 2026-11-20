# GPU Tray (macOS)

Natywna aplikacja w pasku menu dla **Apple Silicon**, która pokazuje:

- **użycie GPU na żywo** (co 1 s, bez uprawnień root — odczyt z IOKit `AGXAccelerator`),
- **status Ollama**: czy właśnie generuje (⚡ pulsująca ikona), jakie modele siedzą w VRAM, ile zostało do auto-unloadu,
- **szybkość generowania** (tok/s) parsowaną z logu `~/.ollama/logs/server.log`,
- **powiadomienia macOS** przy starcie i końcu generowania,
- **zewnętrzny ekranik** [CYD (ESP32)](#ekran-cyd-esp32) z dashboardem GPU/CPU/RAM/sieci/Ollamy — dane po UDP w sieci lokalnej.

Zbudowana i przetestowana na **Mac Studio M1 Max, 32 GB, macOS 27.2** z Ollama 0.40.16.

## Instalacja

```bash
./build_app.sh          # buduje, podpisuje i instaluje do /Applications
open /Applications/GPUTray.app
```

Alternatywnie do katalogu użytkownika: `./build_app.sh --user`.

**Podpis:** skrypt podpisuje pierwszym certyfikatem „Apple Development" z pęku kluczy
(inny wskażesz przez `SIGN_ID=<SHA-1 lub nazwa> ./build_app.sh`). Stała tożsamość sprawia,
że zgody macOS (powiadomienia, Sieć lokalna) przetrwają przebudowy. Bez certyfikatu skrypt
podpisuje ad-hoc — wtedy po każdym buildzie macOS może pytać o zgody od nowa.

- **Autostart przy logowaniu** włącz w menu aplikacji: „🚀 Uruchamiaj przy logowaniu"
  (tworzy `~/Library/LaunchAgents/com.gputray.agent.plist`).
- Przy pierwszym uruchomieniu macOS zapyta o zgodę na **powiadomienia** — kliknij „Zezwól",
  inaczej powiadomienia start/koniec generowania nie będą się pokazywać.

## Ikona w pasku menu

| Wygląd | Znaczenie |
|---|---|
| `42%` (kolor standardowy) | GPU poniżej 50% |
| `42%` pomarańczowe | GPU ≥ 50% |
| `42%` czerwone | GPU ≥ 85% |
| `● 100%` pulsująca kropka | **Ollama generuje** (pomarańczowa ↔ czerwona, ~1,4 Hz) |
| `—` | brak odczytu GPU (nie Apple Silicon?) |

## Menu rozwijane

- nagłówek statusu: ⚡ pracuje / 💤 bezczynny / ⚠️ brak serwera,
- paski: GPU (ogółem), Renderer, Tiler, Pamięć GPU, Pamięć zintegrowana,
- lista załadowanych modeli: VRAM, % na GPU, kontekst, licznik auto-unloadu,
- ostatnia generacja: tok/s + godzina,
- akcje: **Zatrzymaj model** (POST `/api/generate` z `keep_alive: 0`), przełącznik autostartu, otwarcie logów Ollama,
- „🖥 Ekran ESP32": `podłączony (IP)` gdy CYD pyta o dane, inaczej czas od ostatniego zapytania.

## Konfiguracja (`defaults write com.gputray.app <klucz> <wartość`)

| Klucz | Domyślnie | Opis |
|---|---|---|
| `gpuBusyThreshold` | `60` | próg % GPU uznawany za „Ollama pracuje" (np. `-float 75`) |
| `ollamaBaseURL` | auto | wymuszony adres serwera (np. `http://192.168.1.50:11434`) |
| `ollamaPort` | `11434` | port serwera Ollama |

Auto-detekcja adresu serwera: `ollamaBaseURL` (jeśli ustawiony) → `127.0.0.1` → `localhost` → adresy nasłuchu z `lsof`.
Jeśli Ollama słucha tylko na IP LAN (np. `OLLAMA_HOST=192.168.1.50`), aplikacja sama to wykrywa.

Przykład — wymuszenie adresu i powrót do auto-detekcji (po zmianie zrestartuj GPU Tray):

```bash
defaults write com.gputray.app ollamaBaseURL "http://192.168.1.50:11434"
defaults delete com.gputray.app ollamaBaseURL
```

## Diagnostyka

```bash
/Applications/GPUTray.app/Contents/MacOS/GPUTray --once   # jeden snapshot wszystkich źródeł (też sieć i CPU)
GPU_TRAY_DEBUG=1 /Applications/GPUTray.app/Contents/MacOS/GPUTray   # log do ~/Library/Logs/GPUTray-debug.log
.build/debug/gpu-tray --screen-test   # responder UDP z danymi syntetycznymi (test CYD bez GUI)
```

## Ekran CYD (ESP32)

Opcjonalny ekranik **ESP32-2432S028R** („Cheap Yellow Display": 2,8" ILI9341 320×240 + dotyk XPT2046)
pokazuje statystyki Maca. Firmware w `esp32/gpu_cyd/`.

### Wgrywanie

Wymagania: `arduino-cli` z rdzeniem `esp32:esp32` (3.x) oraz bibliotekami **LovyanGFX** i **ArduinoJson** (7.x).

```bash
cd esp32
./build_esp32.sh           # kompilacja + wgranie (port /dev/cu.usbserial-* wykrywany sam)
./build_esp32.sh build     # sama kompilacja
./build_esp32.sh monitor   # podgląd portu szeregowego (115200)
./build_esp32.sh erase     # wymazanie flasha, w tym wszystkich ustawień
CYD_PORT=/dev/cu.usbserial-XXXX ./build_esp32.sh   # port ręcznie
```

Pierwszy build trwa kilka minut (indeksowanie LovyanGFX), kolejne ok. 20 s.
Jeśli portu nie ma w `/dev/cu.usbserial-*`, najczęściej winny jest kabel tylko do ładowania
albo luźna wtyczka micro-USB.

### Pierwsze uruchomienie

1. **Kalibracja dotyku** — dotknij i chwilę przytrzymaj kolejno 4 cele, potem dotknij OK (pudło = kalibracja od nowa).
2. **Wi-Fi** — wybierz sieć z listy (sortowana po sile sygnału), wpisz hasło klawiaturą ekranową
   (`123` przełącza cyfry/symbole na stałe, `***` pokazuje hasło), naciśnij **POLACZ**.
   Złe hasło → powrót do klawiatury.
3. Ekran sam znajduje Maca (broadcast w podsieci) — wystarczy uruchomiony GPU Tray.

Wszystko zapisuje się w pamięci płytki; po restarcie CYD łączy się automatycznie.

### Dashboard

```
● host                    12:34:56  ☀ ↻ ⚙
┌ wskaźnik GPU ┐ ┌ historia GPU 60 s ──┐
└──────────────┘ └─────────────────────┘
┌ ● model  3:07 ──────────── 38.5 tok/s ┐
┌ CPU          ┐ ┌ RAM      24.1 / 32 GB┐
┌ VRAM         ┐ ┌ SIEC  ▼ 1.23  ▲ 0.12 ┐
```

- **kropka w nagłówku**: zielona = dane świeże, czerwona = Mac przestał odpowiadać, szara = jeszcze brak danych,
- **wskaźnik GPU** (wygładzony) i **historia 60 s** z liniami progów 50% / 85%,
- **pasek Ollama**: kropka pulsuje przy generowaniu (model + czas generacji), zielona = niedawny pomiar tok/s,
- **kafelki** CPU, RAM, VRAM i sieć (pobieranie/wysyłanie w MB/s z historią w tle),
- kolory progów jak w pasku menu: zielony < 50% ≤ pomarańczowy < 85% ≤ czerwony.

### Sterowanie (ikony w nagłówku)

| Ikona | Działanie |
|---|---|
| ☀ | następny poziom jasności (5 poziomów: 20–180 z 255, w kółko); na chwilę pokazuje okienko z poziomem |
| ↻ | obrót ekranu o 180° (dotyk działa bez ponownej kalibracji) |
| ⚙ | konfiguracja Wi-Fi (lista sieci; `X` zamyka bez zmian) |

Jasność jest ograniczona do 180/255 celowo: CYD ma słaby stabilizator (AMS1117) i pełne
podświetlenie razem ze skokiem prądu Wi-Fi potrafi zresetować płytkę.

### Tryb nocny

W godzinach **22:00–7:00** (wg zegara Maca) podświetlenie jest wyłączone. Dotknięcie budzi ekran
na 30 s z wybraną jasnością — pierwsze dotknięcie tylko budzi (nie klika przycisków), kolejne
przedłużają. Bez danych z Maca tryb nocny jest nieaktywny. Godziny zmienisz stałymi
`NIGHT_FROM` / `NIGHT_TO` w `gpu_cyd.ino` (wymaga ponownego wgrania).

### Ustawienia w pamięci płytki (NVS, przestrzeń `gpucyd`)

| Klucz | Znaczenie |
|---|---|
| `ssid`, `pass` | dane Wi-Fi |
| `sx1`, `sx2`, `sy1`, `sy2`, `xfy`, `yfx`, `calv` | kalibracja dotyku |
| `rot180` | obrót o 180° |
| `bri` | poziom jasności (indeks 0–4) |

Reset wszystkich ustawień: `./build_esp32.sh erase`, potem ponowne wgranie.

### Protokół (UDP, port 45678 na Macu)

CYD co 1 s wysyła `CYDREQ\n` — broadcastem, dopóki nie zna IP Maca, potem bezpośrednio.
GPU Tray odpowiada jednym JSON-em na adres i port nadawcy:

```json
{"h":"host","t":"HH:mm:ss","gpu":42,"cpu":17.5,"mem":24.1,"memT":32,"vgpu":21.6,
 "nd":1.23,"nu":0.12,"busy":1,"gen":187,"model":"qwen2.5-coder","tok":38.5,"tokA":12}
```

`gpu`/`cpu` w %, pamięć w GB, sieć w MB/s, `gen` = czas bieżącej generacji (s, tylko gdy `busy`),
`tokA` = wiek ostatniego pomiaru tok/s (s). Pola bez odczytu są pomijane.

## Jak to działa (źródła danych, wszystkie bez roota)

| Dane | Źródło | Częstotliwość |
|---|---|---|
| Użycie GPU % + pamięć GPU | IOKit `AGXAccelerator` → `PerformanceStatistics` (`ioreg -r -d 1 -c AGXAccelerator`) | 1 s |
| Modele, VRAM, auto-unload | HTTP `GET /api/ps` | 2 s |
| tok/s | tail `~/.ollama/logs/server.log` (llama.cpp `print_timing`) | na żywo |
| „Ollama pracuje" | GPU ≥ próg przy załadowanym modelu, debounce: start po 2 s, koniec po 3 s ciszy | 1 s |

Uwaga: przy odtwarzaniu wideo/grach GPU też rośnie — jeśli model jest wtedy załadowany,
może pojawić się fałszywe „pracuje". Podnieś wtedy `gpuBusyThreshold`.

## Struktura projektu

```
Sources/GPUTray/
  main.swift               # bootstrap aplikacji (LSUIElement — bez ikony w Docku)
  StatusBarController.swift # pasek menu + menu rozwijane
  GPUStats.swift           # IOKit: GPU + pamięć hosta
  NetStats.swift           # ruch sieci (en*) i CPU
  OllamaClient.swift       # HTTP /api/ps, auto-detekcja adresu, unload
  LogParser.swift          # tail server.log → tok/s
  ActivityMonitor.swift    # maszyna stanów pracuje/bezczynny
  Notifier.swift           # powiadomienia UNUserNotificationCenter
  Autostart.swift          # LaunchAgent
  ESP32Link.swift          # responder UDP dla ekranu CYD
  ScreenTest.swift         # --screen-test: dane syntetyczne dla CYD
  Formatters.swift, DebugLog.swift, Diagnostics.swift
build_app.sh               # build → GPUTray.app → codesign → instalacja
esp32/
  build_esp32.sh           # kompilacja/wgranie/monitor/erase firmware
  gpu_cyd/
    gpu_cyd.ino            # maszyna stanów, UDP, jasność, obrót, tryb nocny
    gfx.h                  # konfiguracja LovyanGFX dla CYD, paleta, helpery
    ui_dash.h              # dashboard (sprite 8bpp, bez migotania)
    ui_wifi.h              # kalibracja dotyku, lista sieci, klawiatura
```
