# GPU Tray (macOS)

Natywna aplikacja w pasku menu dla **Apple Silicon**, która pokazuje:

- **użycie GPU na żywo** (co 1 s, bez uprawnień root — odczyt z IOKit `AGXAccelerator`),
- **status Ollama**: czy właśnie generuje (⚡ pulsująca ikona), jakie modele siedzą w VRAM, ile zostało do auto-unloadu,
- **szybkość generowania** (tok/s) parsowaną z logu `~/.ollama/logs/server.log`,
- **powiadomienia macOS** przy starcie i końcu generowania.

Zbudowana i przetestowana na **Mac Studio M1 Max, 32 GB, macOS 27.2** z Ollama 0.40.16.

## Instalacja

```bash
./build_app.sh          # buduje + instaluje do /Applications i ad-hoc podpisuje
open /Applications/GPUTray.app
```

Alternatywnie do katalogu użytkownika: `./build_app.sh --user`.

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
- akcje: **Zatrzymaj model** (POST `/api/generate` z `keep_alive: 0`), przełącznik autostartu, otwarcie logów Ollama.

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
/Applications/GPUTray.app/Contents/MacOS/GPUTray --once   # jeden snapshot wszystkich źródeł
GPU_TRAY_DEBUG=1 /Applications/GPUTray.app/Contents/MacOS/GPUTray   # log do ~/Library/Logs/GPUTray-debug.log
```

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
  OllamaClient.swift       # HTTP /api/ps, auto-detekcja adresu, unload
  LogParser.swift          # tail server.log → tok/s
  ActivityMonitor.swift    # maszyna stanów pracuje/bezczynny
  Notifier.swift           # powiadomienia UNUserNotificationCenter
  Autostart.swift          # LaunchAgent
  Formatters.swift, DebugLog.swift, Diagnostics.swift
build_app.sh               # build → GPUTray.app → codesign → instalacja
```
