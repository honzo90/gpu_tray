// Dashboard: rysuje statystyki z Maca (JSON po UDP) na CYD 320x240.
#pragma once
#include "gfx.h"
#include <ArduinoJson.h>

// ---- Dane z jednego pakietu gpu-tray
struct Stats {
  char   host[36] = "";
  char   time[10] = "--:--:--";
  float  gpu = -1;          // Device Utilization % (surowe)
  float  gpuEMA = -1;       // wygładzone — do dużego paska
  float  cpu = -1;
  float  mem = -1, memT = -1;   // GB
  float  vgpu = -1;         // GB — pamięć GPU "in use"
  float  nd = 0, nu = 0;    // MB/s
  bool   busy = false;
  uint32_t gen = 0;         // s trwania bieżącej generacji
  char   model[26] = "";
  float  tok = -1;          // tok/s ostatniej ukończonej generacji
  int32_t tokAge = -1;      // s od tej generacji (patrzymy z zegara Maca)
  uint32_t rxMs = 0;        // millis() ostatniego pakietu
  bool   everRx = false;
};

// ---- Drukarka Bambu Lab (obiekt "p" w pakiecie; GPU Tray łączy się z drukarką po MQTT)
struct PrintStats {
  bool  cfg = false;        // drukarka skonfigurowana w GPU Tray (obiekt "p" obecny)
  bool  ok = false;         // GPU Tray ma połączenie z drukarką
  int32_t age = -1;         // s od ostatniego raportu drukarki (-1 = jeszcze nic)
  char  st[14] = "";        // gcode_state
  int   pct = -1, rem = -1, ly = -1, lyT = -1;
  char  f[42] = "";         // plik (ASCII)
  float nz = NAN, nzT = NAN, bd = NAN, bdT = NAN, ch = NAN;
  char  fil[12] = "";       // typ filamentu
  bool  hasCol = false;
  uint32_t col = 0;         // RRGGBB
  int   fr = -1;            // % filamentu na szpuli
  int   amsN = 0;           // liczba slotów AMS
  bool  amsHas[4] = {false, false, false, false};
  uint32_t ams[4] = {0, 0, 0, 0};
  int   act = -1;           // aktywny slot AMS
  int   err = 0;
};

constexpr int GPU_HIST = 60;   // sekundy historii GPU
constexpr int NET_HIST = 45;   // sekundy historii sieci

class Dashboard {
public:
  Stats st;
  float gpuHist[GPU_HIST] = {0};
  float ndHist[NET_HIST] = {0};
  float nuHist[NET_HIST] = {0};
  bool  validHist = false;

  void begin(LGFX& lcd) {
    _lcd = &lcd;
    _canvas = new lgfx::LGFX_Sprite(&lcd);
    _canvas->setColorDepth(8);          // 320x240@8bpp = 77 KB — bez migotania
    if (_canvas->createSprite(_w, _h) == nullptr) {
      delete _canvas;
      _canvas = nullptr;                // rzadkie braki pamięci → rysuj wprost na LCD
    }
  }

  // Układ pionowy 240x320 (obrót 90°/270°) albo poziomy 320x240. Sprite ma tyle samo bajtów,
  // ale inne wymiary — tworzymy go od nowa (zwolnienie + alokacja tego samego rozmiaru).
  void setPortrait(bool p) {
    if (p == _portrait) return;
    _portrait = p;
    _w = p ? H : W;
    _h = p ? W : H;
    if (_canvas) {
      _canvas->deleteSprite();
      if (_canvas->createSprite(_w, _h) == nullptr) { delete _canvas; _canvas = nullptr; }
    }
  }
  int width() const { return _w; }

  // Parsuje jeden pakiet JSON ("{...}").
  void parse(const char* json, uint32_t nowMs) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return;
    strlcpy(st.host, doc["h"] | "", sizeof(st.host));
    strlcpy(st.time, doc["t"] | "--:--:--", sizeof(st.time));
    st.gpu = doc["gpu"] | -1.0;
    st.cpu = doc["cpu"] | -1.0;
    st.mem = doc["mem"] | -1.0;
    st.memT = doc["memT"] | -1.0;
    st.vgpu = doc["vgpu"] | -1.0;
    st.nd = doc["nd"] | 0.0;
    st.nu = doc["nu"] | 0.0;
    st.busy = doc["busy"] | 0;
    st.gen = doc["gen"] | 0;
    strlcpy(st.model, doc["model"] | "", sizeof(st.model));
    st.tok = doc["tok"] | -1.0;
    st.tokAge = doc["tokA"] | -1;
    parsePrinter(doc["p"]);
    if (st.gpu >= 0) {
      st.gpuEMA = st.gpuEMA < 0 ? st.gpu : st.gpuEMA * 0.6 + st.gpu * 0.4;
      pushHist();
    }
    st.rxMs = nowMs;
    st.everRx = true;
  }

  // Czy dotknięto ⚙ (prawy górny róg) → konfiguracja Wi-Fi.
  // ---- Strony: przesunięcie palcem z prawej na lewą → drukarka, odwrotnie → Mac
  enum Page : uint8_t { PageMac = 0, PagePrint = 1 };
  void setPage(Page p) { _page = p; }
  Page page() const { return _page; }
  PrintStats pr;

  // nazwa hosta w nagłówku (lewa część) — ukryty gest: przytrzymanie → logowanie WWW
  bool hitHost(int x, int y) const { return x < _w - 90 && y <= 24; }
  bool hitGear(int x, int y) const { return x >= _w - 26 && y <= 24; }
  // Czy dotknięto ↻ (obok ⚙) → obrót ekranu o kolejne 90°.
  bool hitRotate(int x, int y) const { return x >= _w - 54 && x < _w - 26 && y <= 24; }
  // Czy dotknięto ☀ (obok ↻) → następny poziom jasności.
  bool hitBright(int x, int y) const { return x >= _w - 82 && x < _w - 54 && y <= 24; }

  // Pokazuje na ~1.5 s okienko z poziomem jasności (level 0..levels-1).
  void showBrightness(int level, int levels, uint32_t now) {
    _briLevel = level;
    _briLevels = levels;
    _toastUntil = now + 1500;
  }

  // Zrzut ekranu przez Serial: nagłówek "[shot] W H 8\n", potem W*H bajtów RGB332
  // (bufor sprite'a, orientacja logiczna — W/H zamienione w układzie pionowym). Odbiór: esp32/cyd_screenshot.py
  void dumpScreen(uint32_t nowMs) {
    if (!_canvas) { Serial.println("[shot] brak sprite (rysowanie wprost na LCD)"); return; }
    drawAll(*_canvas, nowMs);                       // świeża klatka
    Serial.printf("[shot] %d %d 8\n", _w, _h);
    Serial.write((const uint8_t*)_canvas->getBuffer(), _w * _h);
    Serial.flush();
    Serial.println();
    Serial.println("[shot] koniec");
  }

  void draw(uint32_t nowMs) {
    LGFXBase& g = _canvas ? (LGFXBase&)*_canvas : (LGFXBase&)*_lcd;
    drawAll(g, nowMs);
    if (_canvas) {
      _canvas->pushSprite(0, 0);
    }
  }

private:
  int _briLevel = 0, _briLevels = 1;
  bool _portrait = false;
  Page _page = PageMac;
  int _w = W, _h = H;
  uint32_t _toastUntil = 0;

  LGFX* _lcd = nullptr;
  lgfx::LGFX_Sprite* _canvas = nullptr;

  void pushHist() {
    memmove(gpuHist, gpuHist + 1, sizeof(float) * (GPU_HIST - 1));
    gpuHist[GPU_HIST - 1] = st.gpu;
    memmove(ndHist, ndHist + 1, sizeof(float) * (NET_HIST - 1));
    ndHist[NET_HIST - 1] = st.nd;
    memmove(nuHist, nuHist + 1, sizeof(float) * (NET_HIST - 1));
    nuHist[NET_HIST - 1] = st.nu;
    validHist = true;
  }

  void parsePrinter(JsonVariantConst p) {
    PrintStats n;
    if (p.isNull()) { pr = n; return; }
    n.cfg = true;
    n.ok = (p["ok"] | 0) != 0;
    n.age = p["age"] | -1;
    strlcpy(n.st, p["st"] | "", sizeof(n.st));
    n.pct = p["pct"] | -1;
    n.rem = p["rem"] | -1;
    n.ly = p["ly"] | -1;
    n.lyT = p["lyT"] | -1;
    strlcpy(n.f, p["f"] | "", sizeof(n.f));
    n.nz = p["nz"] | NAN;  n.nzT = p["nzT"] | NAN;
    n.bd = p["bd"] | NAN;  n.bdT = p["bdT"] | NAN;
    n.ch = p["ch"] | NAN;
    strlcpy(n.fil, p["fil"] | "", sizeof(n.fil));
    if (p["col"].is<const char*>()) { n.col = strtoul(p["col"], nullptr, 16); n.hasCol = true; }
    n.fr = p["fr"] | -1;
    n.act = p["act"] | -1;
    n.err = p["err"] | 0;
    if (p["ams"].is<const char*>()) {          // "RRGGBB,RRGGBB,,RRGGBB" — puste = brak szpuli
      const char* a = p["ams"];
      while (n.amsN < 4) {
        const char* c = strchr(a, ',');
        size_t len = c ? (size_t)(c - a) : strlen(a);
        if (len >= 6) { n.ams[n.amsN] = strtoul(a, nullptr, 16); n.amsHas[n.amsN] = true; }
        n.amsN++;
        if (!c) break;
        a = c + 1;
      }
    }
    pr = n;
  }

  static uint16_t rgb(uint32_t c) { return lgfx::color565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }

  // ================= RYSOWANIE =================
  // Układ 320x240 (wszystko na sprite 8bpp, fonty DejaVu = wygładzane, tylko ASCII):
  //   0..22    nagłówek: kropka live · host · zegar · ⚙
  //   26..120  [wskaźnik GPU 104px] [historia GPU 60 s 200px]
  //   124..146 pasek Ollama
  //   150..236 kafelki 2x2: CPU | RAM / VRAM | SIEC
  // Układ pionowy 240x320 (szerokość wewnętrzna 228):
  //   0..22    nagłówek
  //   26..120  [wskaźnik GPU 104px] [CPU / RAM 120px]
  //   124..146 pasek Ollama
  //   150..226 historia GPU 60 s
  //   230..271 VRAM,  275..316 SIEC

  void drawAll(lgfx::LGFXBase& g, uint32_t now) {
    g.fillScreen(ui::BG);
    bool fresh = st.everRx && now - st.rxMs < 5000;
    if (_page == PagePrint) {
      drawPrintPage(g, now, fresh);
      if ((int32_t)(_toastUntil - now) > 0) drawBrightToast(g);
      return;
    }
    drawHeader(g, fresh);
    float memF = st.memT > 0 && st.mem >= 0 ? st.mem / st.memT : -1;
    float vF   = st.memT > 0 && st.vgpu >= 0 ? st.vgpu / st.memT : -1;
    if (_portrait) {
      const int wi = _w - 2 * MARG, cx = MARG + 108, cw = wi - 108;
      drawGauge(g, MARG, 26, 104, 94);
      tile(g, cx, 26, cw, 45, "CPU", fmtPct(st.cpu), "", st.cpu >= 0 ? st.cpu / 100.0f : -1);
      tile(g, cx, 75, cw, 45, "RAM", fmtNum(st.mem), fmtTot(st.memT), memF);
      drawOllama(g, now, fresh, MARG, 124, wi, 22);
      drawHistory(g, MARG, 150, wi, 76);
      tile(g, MARG, 230, wi, 41, "VRAM", fmtNum(st.vgpu), fmtTot(st.memT), vF);
      netTile(g, MARG, 275, wi, 41);
      if ((int32_t)(_toastUntil - now) > 0) drawBrightToast(g);
      return;
    }
    drawGauge(g, MARG, 26, 104, 94);
    drawHistory(g, MARG + 108, 26, 200, 94);
    drawOllama(g, now, fresh, MARG, 124, W_IN, 22);

    const int tw = 152, th = 41, x2 = MARG + tw + 4;
    tile(g, MARG, 150, tw, th, "CPU",  fmtPct(st.cpu), "", st.cpu >= 0 ? st.cpu / 100.0f : -1);
    tile(g, x2,   150, tw, th, "RAM",  fmtNum(st.mem), fmtTot(st.memT), memF);
    tile(g, MARG, 195, tw, th, "VRAM", fmtNum(st.vgpu), fmtTot(st.memT), vF);
    netTile(g, x2, 195, tw, th);

    if ((int32_t)(_toastUntil - now) > 0) drawBrightToast(g);   // odporne na przepełnienie millis()
  }

  // ── okienko „Jasnosc" z segmentami poziomu (na wierzchu)
  void drawBrightToast(lgfx::LGFXBase& g) {
    const int w = 168, h = 58, x = (_w - w) / 2, y = (_h - h) / 2 - 1;
    g.fillRoundRect(x - 2, y - 2, w + 4, h + 4, 9, ui::BG);    // obwódka odcina od tła
    g.fillRoundRect(x, y, w, h, 8, ui::PANEL2);
    drawSunIcon(g, x + 18, y + 18, ui::TEXT);
    g.setFont(&fonts::DejaVu18);
    g.setTextColor(ui::TEXT);
    g.setTextDatum(lgfx::middle_left);
    g.drawString("Jasnosc", x + 34, y + 18);
    g.setTextDatum(lgfx::top_left);
    const int gap = 4, sx = x + 12, sw = (w - 24 - gap * (_briLevels - 1)) / _briLevels;
    for (int i = 0; i < _briLevels; i++) {
      g.fillRoundRect(sx + i * (sw + gap), y + 38, sw, 10, 3, i <= _briLevel ? ui::ORANGE : ui::PANEL);
    }
  }

  // ☀ — kółko + 8 promieni
  static void drawSunIcon(lgfx::LGFXBase& g, int cx, int cy, uint16_t c) {
    g.fillCircle(cx, cy, 3, c);
    for (int i = 0; i < 8; i++) {
      float a = i * PI / 4;
      g.drawLine(cx + (int)roundf(cosf(a) * 5), cy + (int)roundf(sinf(a) * 5),
                 cx + (int)roundf(cosf(a) * 7), cy + (int)roundf(sinf(a) * 7), c);
    }
  }

  static void card(lgfx::LGFXBase& g, int x, int y, int w, int h, uint16_t c = ui::PANEL) {
    g.fillRoundRect(x, y, w, h, 6, c);
  }

  // ── nagłówek
  // title == nullptr → nazwa hosta; dot = kolor kropki „live"
  void drawHeader(lgfx::LGFXBase& g, bool fresh, const char* title = nullptr, int dotState = -1) {
    const int cy = 11;
    // kropka „live": zielona = dane świeże, czerwona = urwało się, szara = jeszcze nic
    if (dotState < 0) dotState = fresh ? 2 : (st.everRx ? 1 : 0);
    g.fillCircle(MARG + 4, cy, 3, dotState == 2 ? ui::GREEN : (dotState == 1 ? ui::RED : ui::DIM));
    g.setFont(&fonts::DejaVu12);
    // zegar przy ikonach, host przycięty do miejsca, które zostaje
    const int timeRight = _w - 86;
    g.setTextColor(ui::DIM);
    g.setTextDatum(lgfx::middle_right);
    char tm[10];
    strlcpy(tm, st.time, _portrait ? 6 : sizeof(tm));   // w pionie bez sekund (HH:MM) — miejsce na tytuł
    g.drawString(tm, timeRight, cy);
    // dwie kropki stron (aktywna jaśniejsza) — podpowiedź, że można przesunąć
    const int pdx = timeRight - g.textWidth(tm) - 16;
    g.fillCircle(pdx, cy, 2, _page == PageMac ? ui::TEXT : ui::PANEL2);
    g.fillCircle(pdx + 7, cy, 2, _page == PagePrint ? ui::TEXT : ui::PANEL2);
    int hostMax = pdx - 8 - (MARG + 13);
    g.setTextColor(ui::TEXT);
    g.setTextDatum(lgfx::middle_left);
    const char* t = title ? title : (st.host[0] ? st.host : "gpu-tray");
    g.drawString(fitText(g, t, hostMax), MARG + 13, cy);

    drawSunIcon(g, _w - 68, cy, ui::DIM);
    drawRotateIcon(g, _w - 40, cy);

    // ⚙ (pierścień + zęby)
    int cx = _w - 14, r = 7;
    g.drawCircle(cx, cy, r, ui::DIM);
    g.drawCircle(cx, cy, 2, ui::DIM);
    for (int i = 0; i < 4; i++) {
      float a = i * (PI / 2) + 0.4f;
      g.fillRect(cx + (int)(cosf(a) * r) - 1, cy + (int)(sinf(a) * r) - 1, 3, 3, ui::DIM);
    }
    g.setTextDatum(lgfx::top_left);
  }

  // ↻ — łuk 270° z grotem na końcu (kąty: 0° = godz. 3, rosną zgodnie z zegarem)
  static void drawRotateIcon(lgfx::LGFXBase& g, int cx, int cy) {
    const float A1 = 210;
    g.fillArc(cx, cy, 5, 7, 300, 360, ui::DIM);   // dwa odcinki: bez ujemnych kątów
    g.fillArc(cx, cy, 5, 7, 0, A1, ui::DIM);
    float a = A1 * PI / 180.0f;
    float px = cx + cosf(a) * 6, py = cy + sinf(a) * 6;      // koniec łuku
    float tx = -sinf(a), ty = cosf(a);                         // styczna (kierunek łuku)
    float nx = cosf(a), ny = sinf(a);                          // normalna (na zewnątrz)
    g.fillTriangle((int)roundf(px + tx * 4), (int)roundf(py + ty * 4),
                   (int)roundf(px + nx * 3.5f), (int)roundf(py + ny * 3.5f),
                   (int)roundf(px - nx * 3.5f), (int)roundf(py - ny * 3.5f), ui::DIM);
  }

  // ── okrągły wskaźnik GPU (łuk 270°, zaokrąglone końce)
  void drawGauge(lgfx::LGFXBase& g, int x, int y, int w, int h) {
    bool has = st.gpuEMA >= 0;
    float v = has ? clamp01(st.gpuEMA / 100.0f) : 0;
    ring(g, x, y, w, h, has ? st.gpuEMA : -1, "GPU", has ? uiLevelColor(v) : ui::DIM);
  }

  // Wskaźnik pierścieniowy: pct 0..100 (-1 = brak → „--"), etykieta pod liczbą, kolor łuku.
  void ring(lgfx::LGFXBase& g, int x, int y, int w, int h, float pct, const char* label, uint16_t c) {
    card(g, x, y, w, h);
    const int cx = x + w / 2, cy = y + 47, r0 = 32, r1 = 40;
    const float A0 = 135, SPAN = 270;
    bool has = pct >= 0;
    float v = has ? clamp01(pct / 100.0f) : 0;

    g.fillArc(cx, cy, r0, r1, A0, A0 + SPAN, ui::PANEL2);
    capDot(g, cx, cy, (r0 + r1) / 2, A0, (r1 - r0) / 2, ui::PANEL2);
    capDot(g, cx, cy, (r0 + r1) / 2, A0 + SPAN, (r1 - r0) / 2, ui::PANEL2);
    if (v > 0.005f) {
      g.fillArc(cx, cy, r0, r1, A0, A0 + SPAN * v, c);
      capDot(g, cx, cy, (r0 + r1) / 2, A0, (r1 - r0) / 2, c);
      capDot(g, cx, cy, (r0 + r1) / 2, A0 + SPAN * v, (r1 - r0) / 2, c);
    }

    char b[8];
    if (has) snprintf(b, sizeof(b), "%d", (int)(pct + 0.5f));
    else     strcpy(b, "--");
    g.setFont(&fonts::DejaVu24);
    g.setTextColor(ui::TEXT);
    g.setTextDatum(lgfx::middle_center);
    g.drawString(b, cx, cy - 3);
    g.setFont(&fonts::DejaVu9);
    g.setTextColor(ui::DIM);
    g.drawString("%", cx, cy + 15);
    g.setFont(&fonts::DejaVu12);
    g.setTextColor(c == ui::DIM ? ui::DIM : ui::TEXT);
    g.drawString(label, cx, cy + 38);
    g.setTextDatum(lgfx::top_left);
  }

  static void capDot(lgfx::LGFXBase& g, int cx, int cy, int rm, float deg, int rad, uint16_t c) {
    float a = deg * PI / 180.0f;
    g.fillCircle(cx + (int)roundf(cosf(a) * rm), cy + (int)roundf(sinf(a) * rm), rad, c);
  }

  // ================= STRONA DRUKARKI =================
  // Poziom 320x240:  nagłówek | [postęp 104] [informacje 200] | pasek AMS | kafelki 2x2
  //                  (dysza, stół, komora, filament)
  // Pion 240x320:    nagłówek | [postęp 104] [informacje 120] | pasek AMS | 4 kafelki w kolumnie

  void drawPrintPage(lgfx::LGFXBase& g, uint32_t now, bool fresh) {
    bool live = fresh && pr.cfg && pr.ok && pr.age >= 0 && pr.age < 30;
    drawHeader(g, fresh, "Drukarka", live ? 2 : (pr.cfg && pr.age >= 0 ? 1 : 0));
    const int wi = _w - 2 * MARG;

    const char* msg1 = nullptr;
    const char* msg2 = nullptr;
    if (!fresh) { msg1 = st.everRx ? "Czekam na gpu-tray na Macu..." : "Oczekiwanie na dane z Maca..."; }
    else if (!pr.cfg) { msg1 = "Drukarka nie skonfigurowana"; msg2 = "Ustaw ja w menu GPU Tray na Macu"; }
    else if (pr.age < 0) { msg1 = "Brak danych z drukarki"; msg2 = pr.ok ? "czekam na raport..." : "GPU Tray nie moze sie polaczyc"; }
    if (msg1) {
      card(g, MARG, 26, wi, _h - 32);
      g.setFont(&fonts::DejaVu12);
      g.setTextDatum(lgfx::middle_center);
      g.setTextColor(ui::BLUE);
      g.drawString(fitText(g, msg1, wi - 16), _w / 2, _h / 2 - (msg2 ? 9 : 0));
      if (msg2) {
        g.setTextColor(ui::DIM);
        g.drawString(fitText(g, msg2, wi - 16), _w / 2, _h / 2 + 11);
      }
      g.setTextDatum(lgfx::top_left);
      return;
    }

    bool active = isActive();
    ring(g, MARG, 26, 104, 94, active || !strcmp(pr.st, "FINISH") ? (float)pr.pct : -1, "DRUK", stateColor());
    drawPrintInfo(g, MARG + 108, 26, wi - 108, 94, active);
    drawAmsStrip(g, MARG, 124, wi, 22);

    if (_portrait) {
      tempTile(g, MARG, 150, wi, 39, "DYSZA", pr.nz, pr.nzT);
      tempTile(g, MARG, 193, wi, 39, "STOL", pr.bd, pr.bdT);
      tempTile(g, MARG, 236, wi, 39, "KOMORA", pr.ch, NAN);
      filamentTile(g, MARG, 279, wi, 39);
    } else {
      const int tw = 152, th = 41, x2 = MARG + tw + 4;
      tempTile(g, MARG, 150, tw, th, "DYSZA", pr.nz, pr.nzT);
      tempTile(g, x2, 150, tw, th, "STOL", pr.bd, pr.bdT);
      tempTile(g, MARG, 195, tw, th, "KOMORA", pr.ch, NAN);
      filamentTile(g, x2, 195, tw, th);
    }
  }

  bool isActive() const {
    return !strcmp(pr.st, "RUNNING") || !strcmp(pr.st, "PREPARE") || !strcmp(pr.st, "PAUSE")
        || !strcmp(pr.st, "SLICING");
  }

  uint16_t stateColor() const {
    if (!strcmp(pr.st, "FAILED") || pr.err) return ui::RED;
    if (!strcmp(pr.st, "PAUSE")) return ui::ORANGE;
    if (!strcmp(pr.st, "FINISH")) return ui::GREEN;
    if (!strcmp(pr.st, "RUNNING") || !strcmp(pr.st, "PREPARE") || !strcmp(pr.st, "SLICING")) return ui::BLUE;
    return ui::DIM;
  }

  const char* stateLabel() const {
    if (!strcmp(pr.st, "RUNNING")) return "Drukuje";
    if (!strcmp(pr.st, "PREPARE")) return "Przygotowanie";
    if (!strcmp(pr.st, "SLICING")) return "Slicing";
    if (!strcmp(pr.st, "PAUSE"))   return "Pauza";
    if (!strcmp(pr.st, "FINISH"))  return "Gotowe";
    if (!strcmp(pr.st, "FAILED"))  return "Blad druku";
    if (!strcmp(pr.st, "IDLE"))    return "Bezczynna";
    return pr.st[0] ? pr.st : "?";
  }

  // minuty → „1h 23m" / „23 min"
  static void fmtMin(int m, char* b, size_t n) {
    if (m < 60) snprintf(b, n, "%d min", m);
    else snprintf(b, n, "%dh %02dm", m / 60, m % 60);
  }

  // godzina zakończenia wg zegara Maca: „14:05" (+1 = jutro)
  void fmtEnd(int remMin, char* b, size_t n) const {
    int h = 0, m = 0;
    if (sscanf(st.time, "%d:%d", &h, &m) != 2) { strlcpy(b, "--:--", n); return; }
    int t = h * 60 + m + remMin;
    int days = t / 1440;
    t %= 1440;
    if (days) snprintf(b, n, "%02d:%02d +%d", t / 60, t % 60, days);
    else snprintf(b, n, "%02d:%02d", t / 60, t % 60);
  }

  void drawPrintInfo(lgfx::LGFXBase& g, int x, int y, int w, int h, bool active) {
    card(g, x, y, w, h);
    const bool wide = w >= 180;
    const int lx = x + 8, rx = x + w - 8, iw = w - 16;
    char b[24];

    // plik
    g.setFont(&fonts::DejaVu12);
    g.setTextColor(ui::TEXT);
    g.setTextDatum(lgfx::middle_left);
    g.drawString(fitText(g, pr.f[0] ? pr.f : "(bez nazwy)", iw), lx, y + 12);

    // stan (+ warstwy po prawej w szerokim układzie)
    g.setTextColor(stateColor());
    int sw = 0;
    if (wide && pr.lyT > 0) {
      snprintf(b, sizeof(b), "%d/%d", pr.ly < 0 ? 0 : pr.ly, pr.lyT);
      g.setTextColor(ui::DIM);
      g.setTextDatum(lgfx::middle_right);
      g.drawString(b, rx, y + 30);
      sw = g.textWidth(b) + 8;
      g.setFont(&fonts::DejaVu9);
      g.drawString("warstwa", rx - sw, y + 31);
      sw += g.textWidth("warstwa") + 8;
      g.setFont(&fonts::DejaVu12);
      g.setTextColor(stateColor());
      g.setTextDatum(lgfx::middle_left);
    }
    g.drawString(fitText(g, stateLabel(), iw - sw), lx, y + 30);

    // pozostały czas + godzina końca
    if (active && pr.rem >= 0) {
      fmtMin(pr.rem, b, sizeof(b));
      g.setFont(wide ? &fonts::DejaVu24 : &fonts::DejaVu18);
      g.setTextColor(ui::TEXT);
      g.setTextDatum(lgfx::middle_left);
      g.drawString(fitText(g, b, iw), lx, y + (wide ? 58 : 52));
      char e[16];
      fmtEnd(pr.rem, e, sizeof(e));
      if (wide) {
        g.setFont(&fonts::DejaVu9);
        g.setTextColor(ui::DIM);
        g.setTextDatum(lgfx::middle_right);
        g.drawString("koniec", rx, y + 50);
        g.setFont(&fonts::DejaVu12);
        g.setTextColor(ui::TEXT);
        g.drawString(e, rx, y + 64);
      } else {
        g.setFont(&fonts::DejaVu12);
        g.setTextColor(ui::DIM);
        snprintf(b, sizeof(b), "koniec %s", e);
        g.drawString(fitText(g, b, iw), lx, y + 70);
      }
    } else {
      g.setFont(&fonts::DejaVu12);
      g.setTextColor(ui::DIM);
      g.setTextDatum(lgfx::middle_left);
      const char* m = !strcmp(pr.st, "FINISH") ? (wide ? "wydruk zakonczony" : "zakonczony") : "brak zadania";
      g.drawString(fitText(g, m, iw), lx, y + (wide ? 58 : 54));
    }
    // warstwy w wąskim układzie — ostatnia linia
    if (!wide && pr.lyT > 0) {
      snprintf(b, sizeof(b), "warstwa %d/%d", pr.ly < 0 ? 0 : pr.ly, pr.lyT);
      g.setFont(&fonts::DejaVu9);
      g.setTextColor(ui::DIM);
      g.setTextDatum(lgfx::middle_left);
      g.drawString(fitText(g, b, iw), lx, y + h - 9);
    }
    // dane nieświeże
    if (pr.age >= 30) {
      snprintf(b, sizeof(b), "%lds temu", (long)pr.age);
      g.setFont(&fonts::DejaVu9);
      g.setTextColor(ui::RED);
      g.setTextDatum(lgfx::bottom_right);
      g.drawString(b, rx, y + h - 3);
    }
    g.setTextDatum(lgfx::top_left);
  }

  // Pasek AMS: 4 sloty (kolor), aktywny w białej ramce.
  void drawAmsStrip(lgfx::LGFXBase& g, int x, int y, int w, int h) {
    card(g, x, y, w, h);
    const int cy = y + h / 2;
    g.setFont(&fonts::DejaVu12);
    g.setTextColor(ui::DIM);
    g.setTextDatum(lgfx::middle_left);
    g.drawString("AMS", x + 8, cy);
    if (pr.amsN == 0) {
      g.drawString("brak", x + 44, cy);
      g.setTextDatum(lgfx::top_left);
      return;
    }
    const int sx = x + 44, gap = 6, sw = (w - 52 - gap * 3) / 4;
    for (int i = 0; i < 4; i++) {
      int bx = sx + i * (sw + gap);
      if (i == pr.act) g.fillRoundRect(bx - 2, cy - 8, sw + 4, 16, 4, ui::TEXT);
      if (i < pr.amsN && pr.amsHas[i]) {
        g.fillRoundRect(bx, cy - 6, sw, 12, 3, ui::BG);            // obwódka dla ciemnych kolorów
        g.fillRoundRect(bx + 1, cy - 5, sw - 2, 10, 3, rgb(pr.ams[i]));
      } else {
        g.fillRoundRect(bx, cy - 6, sw, 12, 3, ui::PANEL2);         // pusty slot
        g.drawFastHLine(bx + 4, cy, sw - 8, ui::PANEL);
      }
    }
    g.setTextDatum(lgfx::top_left);
  }

  // ° rysowane ręcznie (fonty tylko ASCII) + „C"; zwraca szerokość
  static int degC(lgfx::LGFXBase& g, int x, int cy, uint16_t c) {
    g.drawCircle(x + 2, cy - 4, 2, c);
    g.setFont(&fonts::DejaVu9);
    g.setTextColor(c);
    g.setTextDatum(lgfx::middle_left);
    g.drawString("C", x + 6, cy);
    return 6 + g.textWidth("C");
  }

  // Kafelek temperatury: etykieta °C | aktualna / docelowa, pasek = nagrzanie do celu.
  void tempTile(lgfx::LGFXBase& g, int x, int y, int w, int h, const char* label, float cur, float tgt) {
    card(g, x, y, w, h);
    const int cy = y + 14;
    g.setFont(&fonts::DejaVu12);
    g.setTextColor(ui::DIM);
    g.setTextDatum(lgfx::middle_left);
    g.drawString(label, x + 8, cy);
    int lw = g.textWidth(label);
    degC(g, x + 8 + lw + 3, cy, ui::DIM);

    char v[8], t[10] = "";
    if (isnan(cur)) strcpy(v, "--"); else snprintf(v, sizeof(v), "%d", (int)roundf(cur));
    bool heating = !isnan(tgt) && tgt > 0;
    if (heating) snprintf(t, sizeof(t), "/ %d", (int)roundf(tgt));
    int rx = x + w - 8;
    g.setTextDatum(lgfx::middle_right);
    if (t[0]) {
      g.setFont(&fonts::DejaVu9);
      int need = lw + 30;
      g.setFont(&fonts::DejaVu18);
      need += g.textWidth(v) + 3;
      g.setFont(&fonts::DejaVu9);
      if (need + g.textWidth(t) <= w - 16) {
        g.setTextColor(ui::DIM);
        g.drawString(t, rx, cy + 2);
        rx -= g.textWidth(t) + 3;
      }
    }
    g.setFont(&fonts::DejaVu18);
    g.setTextColor(ui::TEXT);
    g.drawString(v, rx, cy);
    g.setTextDatum(lgfx::top_left);

    // pasek: grzanie → udział w celu (pomarańczowy, zielony w celu); bez celu → skala 0..300
    int bx = x + 8, by = y + h - 12, bw = w - 16;
    g.fillRoundRect(bx, by, bw, 6, 3, ui::PANEL2);
    if (!isnan(cur)) {
      float f = heating ? cur / tgt : cur / 300.0f;
      uint16_t c = heating ? (cur >= tgt - 3 ? ui::GREEN : ui::ORANGE) : ui::DIM;
      int fw = (int)(bw * clamp01(f));
      if (fw >= 3) g.fillRoundRect(bx, by, fw, 6, 3, c);
    }
  }

  // Kafelek filamentu: próbka koloru + typ, pasek = pozostały filament.
  void filamentTile(lgfx::LGFXBase& g, int x, int y, int w, int h) {
    card(g, x, y, w, h);
    const int cy = y + 14;
    g.setFont(&fonts::DejaVu12);
    g.setTextColor(ui::DIM);
    g.setTextDatum(lgfx::middle_left);
    g.drawString("FILAMENT", x + 8, cy);
    const int labelEnd = x + 8 + g.textWidth("FILAMENT") + 8;
    const char* typ = pr.fil[0] ? pr.fil : "--";
    g.setFont(&fonts::DejaVu18);
    int need = g.textWidth(typ) + (pr.hasCol ? 18 : 0);
    g.setFont(&fonts::DejaVu9);
    // „72%" tylko gdy mieści się obok etykiety (w wąskim kafelku pozostały filament pokazuje pasek)
    bool showPct = pr.fr >= 0 && labelEnd + need + g.textWidth("100%") + 5 <= x + w - 8;
    int rx = x + w - 8;
    g.setTextDatum(lgfx::middle_right);
    if (showPct) {
      char b[8];
      snprintf(b, sizeof(b), "%d%%", pr.fr);
      g.setFont(&fonts::DejaVu9);
      g.drawString(b, rx, cy + 2);
      rx -= g.textWidth(b) + 5;
    }
    g.setFont(&fonts::DejaVu18);
    g.setTextColor(ui::TEXT);
    g.drawString(pr.fil[0] ? pr.fil : "--", rx, cy);
    int tw = g.textWidth(pr.fil[0] ? pr.fil : "--");
    if (pr.hasCol) {
      int sx = rx - tw - 18;
      g.fillRoundRect(sx - 1, cy - 7, 14, 14, 4, ui::BG);
      g.fillRoundRect(sx, cy - 6, 12, 12, 3, rgb(pr.col));
    }
    g.setTextDatum(lgfx::top_left);
    int bx = x + 8, by = y + h - 12, bw = w - 16;
    g.fillRoundRect(bx, by, bw, 6, 3, ui::PANEL2);
    if (pr.fr >= 0) {
      int fw = (int)(bw * clamp01(pr.fr / 100.0f));
      uint16_t c = pr.fr < 15 ? ui::RED : (pr.fr < 35 ? ui::ORANGE : ui::GREEN);
      if (fw >= 3) g.fillRoundRect(bx, by, fw, 6, 3, c);
    }
  }

  // ── historia GPU: wypełniony wykres 60 s + kropkowane progi 50% / 85%
  void drawHistory(lgfx::LGFXBase& g, int x, int y, int w, int h) {
    card(g, x, y, w, h);
    g.setFont(&fonts::DejaVu9);
    g.setTextColor(ui::DIM);
    g.setTextDatum(lgfx::top_left);
    g.drawString("GPU  60 s", x + 8, y + 6);

    float mx = 0;
    for (int i = 0; i < GPU_HIST; i++) mx = gpuHist[i] > mx ? gpuHist[i] : mx;
    if (validHist) {
      char b[16];
      snprintf(b, sizeof(b), "max %d%%", (int)(mx + 0.5f));
      g.setTextDatum(lgfx::top_right);
      g.drawString(b, x + w - 8, y + 6);
      g.setTextDatum(lgfx::top_left);
    }

    const int cx0 = x + 8, cw = w - 16, ch = h - 30, bottom = y + h - 8;
    // progi kolorów (jak w trayu)
    for (float t : {0.5f, 0.85f}) {
      int yy = bottom - (int)(t * ch);
      for (int xx = cx0; xx < cx0 + cw; xx += 5) g.drawFastHLine(xx, yy, 2, ui::PANEL2);
    }
    g.drawFastHLine(cx0, bottom, cw, ui::PANEL2);
    if (!validHist) return;

    const int bw = cw / GPU_HIST;                 // 3 px na sekundę
    const int ox = cx0 + (cw - bw * GPU_HIST);
    for (int i = 0; i < GPU_HIST; i++) {
      float v = clamp01(gpuHist[i] / 100.0f);
      if (v <= 0) continue;
      int hh = (int)(v * ch);
      if (hh < 2) hh = 2;
      uint16_t c = uiLevelColor(v);
      int xx = ox + i * bw;
      g.fillRect(xx, bottom - hh, bw, hh, uiMix(ui::PANEL, c, 0.4f));   // wypełnienie
      g.fillRect(xx, bottom - hh, bw, 2, c);                            // krawędź
    }
  }

  // ── pasek Ollama / „czekam na dane"
  void drawOllama(lgfx::LGFXBase& g, uint32_t now, bool fresh, int x, int y, int w, int h) {
    g.setFont(&fonts::DejaVu12);
    const int cy = y + h / 2;
    if (!fresh) {
      card(g, x, y, w, h);
      g.setTextColor(ui::BLUE);
      g.setTextDatum(lgfx::middle_center);
      g.drawString(st.everRx ? "Czekam na gpu-tray na Macu..." : "Oczekiwanie na dane z Maca...", x + w / 2, cy);
      g.setTextDatum(lgfx::top_left);
      return;
    }
    bool tokLive = st.tok >= 0 && st.tokAge >= 0 && st.tokAge < 900;   // <15 min — jak w trayu
    char right[24] = "";
    uint16_t dot, bg = ui::PANEL;
    if (st.busy) {
      // pulsowanie pomarańczowy ↔ czerwony (~1.4 Hz, jak kropka w trayu)
      float phase = (sinf(now * 0.001f * 2 * PI * 1.4f) + 1) / 2;
      dot = uiMix(ui::ORANGE, ui::RED, phase);
      bg = uiMix(ui::PANEL, dot, 0.22f);
      if (tokLive) snprintf(right, sizeof(right), "%.1f tok/s", st.tok);
    } else {
      dot = tokLive ? ui::GREEN : ui::DIM;
      if (tokLive) snprintf(right, sizeof(right), "ost. %.1f tok/s", st.tok);
    }
    card(g, x, y, w, h, bg);
    g.fillCircle(x + 11, cy, 4, dot);

    // Najpierw prawa strona (tok/s) — jej szerokość wyznacza miejsce na lewą.
    int leftX = x + 22, rightEdge = x + w - 8;
    if (right[0]) {
      g.setTextColor(st.busy ? ui::TEXT : ui::DIM);
      g.setTextDatum(lgfx::middle_right);
      g.drawString(right, rightEdge, cy);
      rightEdge -= g.textWidth(right) + 12;            // odstęp między lewą a prawą
    }
    g.setTextDatum(lgfx::middle_left);
    if (st.busy) {
      // model (przycinany do szerokości w px) + czas generacji (zawsze widoczny)
      char t[12];
      fmtDur(st.gen, t, sizeof(t));
      int tw = g.textWidth(t);
      const char* m = fitText(g, st.model[0] ? st.model : "Ollama", rightEdge - leftX - tw - 8);
      g.setTextColor(ui::TEXT);
      g.drawString(m, leftX, cy);
      g.setTextColor(ui::DIM);
      g.drawString(t, leftX + g.textWidth(m) + 8, cy);
    } else {
      g.setTextColor(ui::DIM);
      g.drawString(fitText(g, "Ollama: bezczynny", rightEdge - leftX), leftX, cy);
    }
    g.setTextDatum(lgfx::top_left);
  }

  // Przycina tekst (z „..") tak, by w bieżącym foncie miał ≤ maxW px.
  static const char* fitText(lgfx::LGFXBase& g, const char* s, int maxW) {
    static char b[40];
    strlcpy(b, s, sizeof(b));
    if (g.textWidth(b) <= maxW) return b;
    for (int n = strlen(b) - 1; n > 0; n--) {
      snprintf(b, sizeof(b), "%.*s..", n, s);
      if (g.textWidth(b) <= maxW) return b;
    }
    b[0] = 0;
    return b;
  }

  // 42s / 3:07 / 1:02:05
  static void fmtDur(uint32_t s, char* b, size_t n) {
    if (s < 60) snprintf(b, n, "%lus", (unsigned long)s);
    else if (s < 3600) snprintf(b, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    else snprintf(b, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
                  (unsigned long)(s % 60));
  }

  // ── kafelek: etykieta | wartość (+ szara „/ suma") , pod spodem cienki pasek
  void tile(lgfx::LGFXBase& g, int x, int y, int w, int h,
            const char* label, const char* value, const char* sub, float frac) {
    card(g, x, y, w, h);
    const int cy = y + 14;
    g.setFont(&fonts::DejaVu12);
    g.setTextColor(ui::DIM);
    g.setTextDatum(lgfx::middle_left);
    g.drawString(label, x + 8, cy);

    int rx = x + w - 8;
    g.setTextDatum(lgfx::middle_right);
    if (sub[0]) {                       // „/ suma" tylko gdy zmieści się obok etykiety i wartości
      int need = g.textWidth(label) + 10;
      g.setFont(&fonts::DejaVu18);
      need += g.textWidth(value) + 3;
      g.setFont(&fonts::DejaVu9);
      if (need + g.textWidth(sub) > w - 16) sub = "";
    }
    if (sub[0]) {
      g.setFont(&fonts::DejaVu9);
      g.drawString(sub, rx, cy + 2);
      rx -= g.textWidth(sub) + 3;
    }
    g.setFont(&fonts::DejaVu18);
    g.setTextColor(ui::TEXT);
    g.drawString(value, rx, cy);
    g.setTextDatum(lgfx::top_left);

    // pasek 6 px
    int bx = x + 8, by = y + h - 12, bw = w - 16;
    g.fillRoundRect(bx, by, bw, 6, 3, ui::PANEL2);
    if (frac >= 0) {
      int fw = (int)(bw * clamp01(frac));
      if (fw >= 3) g.fillRoundRect(bx, by, fw, 6, 3, uiLevelColor(frac));
    }
  }

  // ── kafelek sieci: ↓ / ↑ + blada historia w tle (lewa połowa = pobieranie, prawa = wysyłanie)
  void netTile(lgfx::LGFXBase& g, int x, int y, int w, int h) {
    card(g, x, y, w, h);
    const int hw = (w - 16) / 2 - 3;
    histBg(g, x + 8, y + 18, hw, h - 21, ndHist, uiMix(ui::PANEL, ui::BLUE, 0.35f));
    histBg(g, x + w / 2 + 3, y + 18, hw, h - 21, nuHist, uiMix(ui::PANEL, ui::GREEN, 0.35f));

    g.setFont(&fonts::DejaVu12);
    g.setTextColor(ui::DIM);
    g.setTextDatum(lgfx::middle_left);
    g.drawString("SIEC", x + 8, y + 10);
    g.setFont(&fonts::DejaVu9);
    g.setTextDatum(lgfx::middle_right);
    g.drawString("MB/s", x + w - 8, y + 10);

    const int cy = y + 28;
    g.setFont(&fonts::DejaVu18);
    g.setTextDatum(lgfx::middle_left);
    drawArrow(g, x + 8, cy, 8, true, ui::BLUE);
    g.setTextColor(ui::BLUE);
    g.drawString(fmtSpeed(st.nd), x + 20, cy);
    drawArrow(g, x + w / 2 + 3, cy, 8, false, ui::GREEN);
    g.setTextColor(ui::GREEN);
    g.drawString(fmtSpeed(st.nu), x + w / 2 + 15, cy);
    g.setTextDatum(lgfx::top_left);
  }

  // słupki historii skalowane do własnego maksimum (podłoga 0.1 MB/s)
  static void histBg(lgfx::LGFXBase& g, int x, int y, int w, int h, const float* hist, uint16_t c) {
    float mx = 0.1f;
    for (int i = 0; i < NET_HIST; i++) mx = hist[i] > mx ? hist[i] : mx;
    for (int i = 0; i < NET_HIST; i++) {
      int hh = (int)(hist[i] / mx * h);
      if (hh <= 0) continue;
      int x0 = x + i * w / NET_HIST, x1 = x + (i + 1) * w / NET_HIST;
      g.fillRect(x0, y + h - hh, x1 - x0 > 0 ? x1 - x0 : 1, hh, c);
    }
  }

  // strzałka ↓/↑ wycentrowana pionowo na cy
  static void drawArrow(lgfx::LGFXBase& g, int x, int cy, int s, bool down, uint16_t c) {
    int t = cy - s / 2;
    if (down) g.fillTriangle(x, t + 2, x + s, t + 2, x + s / 2, t + s, c);
    else      g.fillTriangle(x, t + s - 2, x + s, t + s - 2, x + s / 2, t, c);
  }

  // ── formatowanie
  static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

  static const char* fmtSpeed(float mb) {   // bez jednostki — jest w nagłówku kafelka
    static char b[12];
    if (mb >= 100) snprintf(b, sizeof(b), "%d", (int)mb);
    else if (mb >= 10) snprintf(b, sizeof(b), "%.1f", mb);
    else snprintf(b, sizeof(b), "%.2f", mb < 0.005f ? 0.0f : mb);
    return b;
  }

  static const char* fmtNum(float v) {      // GB jedno miejsce po przecinku
    static char b[12];
    if (v < 0) return "--";
    snprintf(b, sizeof(b), "%.1f", v);
    return b;
  }

  static const char* fmtTot(float t) {
    static char b[12];
    if (t <= 0) return "GB";
    snprintf(b, sizeof(b), "/ %.0f GB", t);
    return b;
  }

  static const char* fmtPct(float v) {
    static char b[10];
    if (v < 0) return "--";
    snprintf(b, sizeof(b), "%d%%", (int)(v + 0.5f));
    return b;
  }

  static const char* truncTo(const char* s, int n) {
    static char b[28];
    snprintf(b, sizeof(b), "%.*s", n, s);
    return b;
  }
};
