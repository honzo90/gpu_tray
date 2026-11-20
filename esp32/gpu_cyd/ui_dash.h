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
    if (_canvas->createSprite(W, H) == nullptr) {
      delete _canvas;
      _canvas = nullptr;                // rzadkie braki pamięci → rysuj wprost na LCD
    }
  }

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
    if (st.gpu >= 0) {
      st.gpuEMA = st.gpuEMA < 0 ? st.gpu : st.gpuEMA * 0.6 + st.gpu * 0.4;
      pushHist();
    }
    st.rxMs = nowMs;
    st.everRx = true;
  }

  // Czy dotknięto ⚙ (prawy górny róg) → konfiguracja Wi-Fi.
  bool hitGear(int x, int y) const { return x >= W - 26 && y <= 24; }
  // Czy dotknięto ↻ (obok ⚙) → obrót ekranu o 180°.
  bool hitRotate(int x, int y) const { return x >= W - 54 && x < W - 26 && y <= 24; }
  // Czy dotknięto ☀ (obok ↻) → następny poziom jasności.
  bool hitBright(int x, int y) const { return x >= W - 82 && x < W - 54 && y <= 24; }

  // Pokazuje na ~1.5 s okienko z poziomem jasności (level 0..levels-1).
  void showBrightness(int level, int levels, uint32_t now) {
    _briLevel = level;
    _briLevels = levels;
    _toastUntil = now + 1500;
  }

  // Zrzut ekranu przez Serial: nagłówek "[shot] W H 8\n", potem W*H bajtów RGB332
  // (bufor sprite'a, orientacja logiczna — niezależnie od obrotu 180°). Odbiór: esp32/cyd_screenshot.py
  void dumpScreen(uint32_t nowMs) {
    if (!_canvas) { Serial.println("[shot] brak sprite (rysowanie wprost na LCD)"); return; }
    drawAll(*_canvas, nowMs);                       // świeża klatka
    Serial.printf("[shot] %d %d 8\n", W, H);
    Serial.write((const uint8_t*)_canvas->getBuffer(), W * H);
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

  // ================= RYSOWANIE =================
  // Układ 320x240 (wszystko na sprite 8bpp, fonty DejaVu = wygładzane, tylko ASCII):
  //   0..22    nagłówek: kropka live · host · zegar · ⚙
  //   26..120  [wskaźnik GPU 104px] [historia GPU 60 s 200px]
  //   124..146 pasek Ollama
  //   150..236 kafelki 2x2: CPU | RAM / VRAM | SIEC

  void drawAll(lgfx::LGFXBase& g, uint32_t now) {
    g.fillScreen(ui::BG);
    bool fresh = st.everRx && now - st.rxMs < 5000;
    drawHeader(g, fresh);
    drawGauge(g, MARG, 26, 104, 94);
    drawHistory(g, MARG + 108, 26, 200, 94);
    drawOllama(g, now, fresh, MARG, 124, W_IN, 22);

    const int tw = 152, th = 41, x2 = MARG + tw + 4;
    float memF = st.memT > 0 && st.mem >= 0 ? st.mem / st.memT : -1;
    float vF   = st.memT > 0 && st.vgpu >= 0 ? st.vgpu / st.memT : -1;
    tile(g, MARG, 150, tw, th, "CPU",  fmtPct(st.cpu), "", st.cpu >= 0 ? st.cpu / 100.0f : -1);
    tile(g, x2,   150, tw, th, "RAM",  fmtNum(st.mem), fmtTot(st.memT), memF);
    tile(g, MARG, 195, tw, th, "VRAM", fmtNum(st.vgpu), fmtTot(st.memT), vF);
    netTile(g, x2, 195, tw, th);

    if ((int32_t)(_toastUntil - now) > 0) drawBrightToast(g);   // odporne na przepełnienie millis()
  }

  // ── okienko „Jasnosc" z segmentami poziomu (na wierzchu)
  void drawBrightToast(lgfx::LGFXBase& g) {
    const int w = 168, h = 58, x = (W - w) / 2, y = 92;
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
  void drawHeader(lgfx::LGFXBase& g, bool fresh) {
    const int cy = 11;
    // kropka „live": zielona = dane świeże, czerwona = urwało się, szara = jeszcze nic
    g.fillCircle(MARG + 4, cy, 3, fresh ? ui::GREEN : (st.everRx ? ui::RED : ui::DIM));
    g.setFont(&fonts::DejaVu12);
    // zegar przy ikonach, host przycięty do miejsca, które zostaje
    const int timeRight = W - 86;
    g.setTextColor(ui::DIM);
    g.setTextDatum(lgfx::middle_right);
    g.drawString(st.time, timeRight, cy);
    int hostMax = timeRight - g.textWidth(st.time) - 10 - (MARG + 13);
    g.setTextColor(ui::TEXT);
    g.setTextDatum(lgfx::middle_left);
    g.drawString(fitText(g, st.host[0] ? st.host : "gpu-tray", hostMax), MARG + 13, cy);

    drawSunIcon(g, W - 68, cy, ui::DIM);
    drawRotateIcon(g, W - 40, cy);

    // ⚙ (pierścień + zęby)
    int cx = W - 14, r = 7;
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
    card(g, x, y, w, h);
    const int cx = x + w / 2, cy = y + 47, r0 = 32, r1 = 40;
    const float A0 = 135, SPAN = 270;
    bool has = st.gpuEMA >= 0;
    float v = has ? clamp01(st.gpuEMA / 100.0f) : 0;
    uint16_t c = has ? uiLevelColor(v) : ui::DIM;

    g.fillArc(cx, cy, r0, r1, A0, A0 + SPAN, ui::PANEL2);
    capDot(g, cx, cy, (r0 + r1) / 2, A0, (r1 - r0) / 2, ui::PANEL2);
    capDot(g, cx, cy, (r0 + r1) / 2, A0 + SPAN, (r1 - r0) / 2, ui::PANEL2);
    if (v > 0.005f) {
      g.fillArc(cx, cy, r0, r1, A0, A0 + SPAN * v, c);
      capDot(g, cx, cy, (r0 + r1) / 2, A0, (r1 - r0) / 2, c);
      capDot(g, cx, cy, (r0 + r1) / 2, A0 + SPAN * v, (r1 - r0) / 2, c);
    }

    char b[8];
    if (has) snprintf(b, sizeof(b), "%d", (int)(st.gpuEMA + 0.5f));
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
    g.drawString("GPU", cx, cy + 38);
    g.setTextDatum(lgfx::top_left);
  }

  static void capDot(lgfx::LGFXBase& g, int cx, int cy, int rm, float deg, int rad, uint16_t c) {
    float a = deg * PI / 180.0f;
    g.fillCircle(cx + (int)roundf(cosf(a) * rm), cy + (int)roundf(sinf(a) * rm), rad, c);
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
