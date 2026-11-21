// Konfiguracja Wi-Fi na ekranie CYD: kalibracja dotyku → lista sieci → klawiatura hasła.
// Dane zapisywane w NVS (Preferences) — po restarcie łączy się automatycznie.
#pragma once
#include "gfx.h"
#include <WiFi.h>
#include <Preferences.h>

// ---- Dotyk z własną kalibracją (getTouchRaw zwraca surowe 0..4095)
struct TouchCal {
  uint16_t sx1 = 0, sx2 = 1;   // raw dla ekran X=24 / X=296
  uint16_t sy1 = 0, sy2 = 1;   // raw dla ekran Y=24 / Y=216
  bool xFromY = false;         // ekranowa oś X pochodzi z raw Y (swap dla części płytek)
  bool yFromX = false;
  bool valid = false;
  // Obrót w ćwierćobrotach względem orientacji bazowej (setRotation 1): 0 = 0°, 1 = 90°,
  // 2 = 180°, 3 = 270°. Kalibracja jest zawsze zapisana dla orientacji bazowej — wynik
  // tylko przeliczamy (jak Panel_Device::convertRawXY w LovyanGFX).
  uint8_t rot = 0;
};

// setRotation LovyanGFX dla ćwierćobrotu q (bazowa = 1, obroty idą 1 → 2 → 3 → 0).
inline uint8_t lcdRotFor(uint8_t q) {
  static const uint8_t R[4] = {1, 2, 3, 0};
  return R[q & 3];
}

inline int32_t calMap(int32_t v, int32_t a, int32_t b, int32_t s1, int32_t s2) {
  int32_t span = b - a;
  if (span == 0) return s1;
  return (v - a) * (s2 - s1) / span + s1;
}

inline bool touchXY(LGFX& lcd, const TouchCal& c, int* ox, int* oy) {
  uint16_t rx = 0, ry = 0;
  if (lcd.getTouchRaw(&rx, &ry) == 0) return false;
  int32_t rawX = c.xFromY ? ry : rx;
  int32_t rawY = c.yFromX ? rx : ry;
  int32_t x = calMap(rawX, c.sx1, c.sx2, 24, 296);
  int32_t y = calMap(rawY, c.sy1, c.sy2, 24, 216);
  x = x < 0 ? 0 : (x > 319 ? 319 : x);
  y = y < 0 ? 0 : (y > 239 ? 239 : y);
  int32_t bx = x, by = y;                         // współrzędne w orientacji bazowej 320x240
  switch (c.rot & 3) {
    case 1: x = by;        y = 319 - bx; break;   // rotation 2: pion 240x320
    case 2: x = 319 - bx;  y = 239 - by; break;   // rotation 3: poziom odwrócony
    case 3: x = 239 - by;  y = bx;       break;   // rotation 0: pion odwrócony
  }
  if (ox) *ox = x;
  if (oy) *oy = y;
  return true;
}

inline bool touchDown(LGFX& lcd, const TouchCal& c) {
  return touchXY(lcd, c, nullptr, nullptr);
}

inline void waitRelease(LGFX& lcd, const TouchCal& c) {
  while (touchDown(lcd, c)) delay(20);
  delay(60);   // debounce drgania styku
}

// ---- Kalibracja: 4 cele (osie rozdzielone, wykrywa też zamienione osie X/Y)
// Zawsze w orientacji bazowej (rotation 1, rot = 0) — patrz runCalibration niżej.
inline bool runCalibrationBase(LGFX& lcd, Preferences& prefs, TouchCal& cal) {
  struct Target { int x, y; const char* tag; };
  const Target t[4] = {
    {24, 120, "1/4"}, {296, 120, "2/4"}, {160, 24, "3/4"}, {160, 216, "4/4"},
  };
  uint16_t rx[4], ry[4];
  for (int i = 0; i < 4; i++) {
    lcd.fillScreen(ui::BG);
    lcd.setFont(&fonts::Font4);
    lcd.setTextColor(ui::TEXT);
    lcd.setTextDatum(lgfx::middle_center);
    lcd.drawString("Kalibracja dotyku", 160, 24);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(ui::DIM);
    lcd.drawString("Dotknij i przytrzymaj znak, potem pusc", 160, 44);
    lcd.setTextColor(ui::DIM);
    lcd.drawString(t[i].tag, 160, 234);
    lcd.setTextDatum(lgfx::top_left);

    // cel: pierścień + krzyżyk
    lcd.drawCircle(t[i].x, t[i].y, 10, ui::BLUE);
    lcd.drawCircle(t[i].x, t[i].y, 3, ui::BLUE);
    lcd.fillRect(t[i].x - 12, t[i].y, 25, 1, ui::PANEL2);
    lcd.fillRect(t[i].x, t[i].y - 12, 1, 25, ui::PANEL2);

    // czekaj na dotyk
    while (!touchDown(lcd, cal)) { delay(15); }
    // średnia z przytrzymania (~350 ms)
    uint32_t sx = 0, sy = 0; int n = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < 350) {
      uint16_t r2x = 0, r2y = 0;
      if (lcd.getTouchRaw(&r2x, &r2y)) { sx += r2x; sy += r2y; n++; }
      delay(25);
    }
    if (n < 5) { i--; continue; }               // zbyt krótko — powtórz ten sam cel
    rx[i] = sx / n; ry[i] = sy / n;
    // wizualne potwierdzenie
    lcd.fillCircle(t[i].x, t[i].y, 4, ui::GREEN);
    waitRelease(lcd, cal);
  }

  // Oś X z par (24,120)→(296,120): surowa oś z większą rozpiętością należy do ekranowej X.
  int32_t dRx = (int32_t)rx[1] - (int32_t)rx[0];
  int32_t dRy = (int32_t)ry[1] - (int32_t)ry[0];
  bool xFromY = (dRy < 0 ? -dRy : dRy) > (dRx < 0 ? -dRx : dRx);
  cal.xFromY = xFromY;
  cal.sx1 = xFromY ? ry[0] : rx[0];
  cal.sx2 = xFromY ? ry[1] : rx[1];

  // Oś Y z par (160,24)→(160,216)
  dRx = (int32_t)rx[3] - (int32_t)rx[2];
  dRy = (int32_t)ry[3] - (int32_t)ry[2];
  bool yFromX = (dRx < 0 ? -dRx : dRx) > (dRy < 0 ? -dRy : dRy);
  cal.yFromX = yFromX;
  cal.sy1 = yFromX ? rx[2] : ry[2];
  cal.sy2 = yFromX ? rx[3] : ry[3];
  cal.valid = true;

  // Weryfikacja: dotknij OK; pudło → kalibracja od nowa.
  lcd.fillScreen(ui::BG);
  lcd.setFont(&fonts::Font4);
  lcd.setTextColor(ui::TEXT);
  lcd.setTextDatum(lgfx::middle_center);
  lcd.drawString("Sprawdzam dotyk", 160, 30);
  lcd.setTextDatum(lgfx::top_left);
  uiRect ok = uiButton(lcd, 120, 100, 80, 40, "OK");
  int x = 0, y = 0;
  while (!touchXY(lcd, cal, &x, &y)) delay(15);
  bool good = uiHit(ok, x, y);
  waitRelease(lcd, cal);
  if (!good) return runCalibrationBase(lcd, prefs, cal);   // pudło → kalibracja od nowa

  prefs.putUShort("sx1", cal.sx1);
  prefs.putUShort("sx2", cal.sx2);
  prefs.putUShort("sy1", cal.sy1);
  prefs.putUShort("sy2", cal.sy2);
  prefs.putBool("xfy", cal.xFromY);
  prefs.putBool("yfx", cal.yFromX);
  prefs.putBool("calv", true);
  return true;
}

// Kalibracja niezależna od obrotu: na jej czas orientacja bazowa, potem powrót.
inline bool runCalibration(LGFX& lcd, Preferences& prefs, TouchCal& cal) {
  uint8_t rot = cal.rot;
  cal.rot = 0;
  lcd.setRotation(1);
  bool r = runCalibrationBase(lcd, prefs, cal);
  cal.rot = rot;
  lcd.setRotation(lcdRotFor(rot));
  lcd.fillScreen(ui::BG);
  return r;
}

// ---- Ekrany łączenia (sterowane z gpu_cyd.ino)
inline void drawConnecting(LGFX& lcd, const String& ssid) {
  lcd.fillScreen(ui::BG);
  lcd.setFont(&fonts::Font4);
  lcd.setTextColor(ui::TEXT);
  lcd.setTextDatum(lgfx::middle_center);
  lcd.drawString("Laczenie z", 160, 78);
  lcd.drawString(truncStr(ssid, 24), 160, 100);
  lcd.setTextDatum(lgfx::top_left);
}

inline void connectingTick(LGFX& lcd, uint32_t ms, bool hasIP, const String& ip) {
  static uint32_t last = 0;
  if (ms - last < 180) return;
  last = ms;
  int dots = (ms / 300) % 4;
  char buf[24];
  snprintf(buf, sizeof(buf), "%.*s", dots + 1, "....");
  lcd.setFont(&fonts::Font4);
  lcd.setTextColor(ui::DIM);
  lcd.setTextDatum(lgfx::middle_center);
  lcd.fillRect(120, 130, 80, 20, ui::BG);
  lcd.drawString(buf, 160, 140);
  lcd.fillRect(20, 168, 280, 20, ui::BG);
  if (hasIP) {
    lcd.setTextColor(ui::GREEN);
    lcd.drawString("IP: " + ip, 160, 178);
  }
  lcd.setTextColor(ui::TEXT);
  lcd.setTextDatum(lgfx::top_left);
}

inline void showConnectFail(LGFX& lcd, const String& ssid) {
  lcd.fillScreen(ui::BG);
  lcd.setFont(&fonts::Font4);
  lcd.setTextColor(ui::RED);
  lcd.setTextDatum(lgfx::middle_center);
  lcd.drawString("Nie udalo sie polaczyc", 160, 92);
  lcd.setTextColor(ui::DIM);
  lcd.setFont(&fonts::Font2);
  lcd.drawString("Sprawdz haslo sieci " + truncStr(ssid, 20), 160, 116);
  lcd.setTextColor(ui::TEXT);
  lcd.setTextDatum(lgfx::top_left);
}

// ---- Strona listy sieci + strona klawiatury
namespace wifi_setup {

constexpr int KB_X0 = 6, KB_W = 28, KB_GAP = 3, KB_H = 26;
constexpr int ROW_Y[3] = {58, 88, 118};
constexpr int SP_Y = 148;

struct Network { String ssid; int rssi; bool open; };

// Rysuje pasek sygnału (4 poziomy)
inline void rssiIcon(lgfx::LGFXBase& g, int x, int y, int rssi) {
  int lvl = rssi >= -55 ? 4 : rssi >= -66 ? 3 : rssi >= -77 ? 2 : rssi >= -88 ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    int h = 3 + i * 3;
    g.fillRect(x + i * 5, y + 12 - h, 3, h, i < lvl ? ui::GREEN : ui::PANEL2);
  }
}

inline void lockIcon(lgfx::LGFXBase& g, int x, int y) {
  g.drawArc(x + 5, y + 5, 4, 2, 180, 360, ui::DIM);
  g.fillRect(x, y + 4, 11, 8, ui::DIM);
  g.fillRect(x + 4, y + 6, 3, 4, ui::BG);
}

// ============================== LISTA SIECI ==============================
// Zwraca true, gdy użytkownik wybrał sieć (ssid), false → [Zamknij] ( możliwe tylko gdy Wi-Fi już działa).
inline bool listPage(LGFX& lcd, const TouchCal& cal, String& pickSsid, bool allowClose) {
  static Network nets[24];
  int netCount = 0;

  lcd.fillScreen(ui::BG);
  lcd.setFont(&fonts::Font4);
  lcd.setTextColor(ui::TEXT);
  lcd.setTextDatum(lgfx::middle_left);
  lcd.drawString("Wybierz siec Wi-Fi", MARG + 2, 12);
  lcd.setTextDatum(lgfx::top_left);
  uiRect bRescan = uiButton(lcd, 216, 0, 46, 22, "Skan", true);
  uiRect bClose  = allowClose ? uiButton(lcd, 268, 0, 46, 22, "X") : uiRect{0, 0, 0, 0};

  // start skanowania (asynchroniczny)
  WiFi.scanNetworks(true);
  bool scanning = true, drawn = false;
  int page = 0;
  uint32_t scanStart = millis();

  while (true) {
    if (scanning) {
      static uint32_t lastAnim = 0;
      if (millis() - lastAnim > 220) {
        lastAnim = millis();
        lcd.setFont(&fonts::Font2);
        lcd.setTextColor(ui::BLUE);
        lcd.setTextDatum(lgfx::middle_center);
        int dots = (millis() / 300) % 4;
        char buf[24];
        snprintf(buf, sizeof(buf), "Skanowanie%.*s", dots, "...");
        lcd.fillRect(60, 196, 200, 14, ui::BG);
        lcd.drawString(buf, 160, 204);
        lcd.setTextColor(ui::TEXT);
        lcd.setTextDatum(lgfx::top_left);
      }
      int n = WiFi.scanComplete();
      if (n >= 0) {
        scanning = false;
        netCount = 0;
        // Najpierw czytamy WSZYSTKIE wyniki, dopiero potem scanDelete() —
        // w rdzeniu 3.x scanDelete zwalnia bufor wyników (use-after-free!).
        for (int i = 0; i < n && netCount < 24; i++) {
          String s = WiFi.SSID(i);
          if (s.isEmpty()) continue;
          bool dup = false;
          for (int j = 0; j < netCount; j++) if (nets[j].ssid == s) { dup = true; break; }
          if (dup) continue;
          nets[netCount++] = {s, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN};
        }
        WiFi.scanDelete();
        // sort po sile sygnału (malejąco)
        for (int i = 1; i < netCount; i++) {
          Network k = nets[i]; int j = i - 1;
          while (j >= 0 && nets[j].rssi < k.rssi) { nets[j + 1] = nets[j]; j--; }
          nets[j + 1] = k;
        }
        page = 0;
        drawn = false;
      } else if (n == -2 || millis() - scanStart > 25000) {
        WiFi.scanNetworks(true);   // brak aktywnego skanu albo skan utknął → startuj ponownie
        scanStart = millis();
      }
    }

    if (!scanning && !drawn) {
      lcd.fillRect(0, 24, 320, 240 - 24, ui::BG);
      lcd.setFont(&fonts::Font2);
      lcd.setTextColor(ui::DIM);
      lcd.setTextDatum(lgfx::middle_center);
      lcd.drawString(netCount ? "Dotknij sieci, aby podac haslo" : "Brak sieci w zasiegu", 160, 204);
      lcd.setTextDatum(lgfx::top_left);
      int per = 6, pages = (netCount + per - 1) / per;
      if (page >= pages) page = pages - 1;
      for (int r = 0; r < per; r++) {
        int idx = page * per + r;
        if (idx >= netCount) break;
        int y = 28 + r * 27;
        lcd.fillRoundRect(MARG, y, W_IN, 24, 4, ui::PANEL);
        lcd.setFont(&fonts::Font4);
        lcd.setTextColor(ui::TEXT);
        lcd.setTextDatum(lgfx::middle_left);
        lcd.drawString(truncStr(nets[idx].ssid, 20), 26, y + 12);
        lcd.setTextDatum(lgfx::top_left);
        if (!nets[idx].open) lockIcon(lcd, 6, y + 4);
        rssiIcon(lcd, 268, y + 5, nets[idx].rssi);
      }
      // pasek stronicowania ze strzałkami
      char pg[12];
      snprintf(pg, sizeof(pg), "%d/%d", page + 1, pages > 0 ? pages : 1);
      if (page > 0) uiButton(lcd, MARG, 206, 46, 24, "<<", false, 2);
      if (page < pages - 1) uiButton(lcd, W - MARG - 46, 206, 46, 24, ">>", false, 2);
      lcd.setFont(&fonts::Font2);
      lcd.setTextColor(ui::DIM);
      lcd.setTextDatum(lgfx::middle_center);
      lcd.drawString(pg, 160, 224);
      lcd.setTextDatum(lgfx::top_left);
      drawn = true;
    }

    int x = 0, y = 0;
    if (touchXY(lcd, cal, &x, &y)) {
      if (uiHit(bRescan, x, y)) { waitRelease(lcd, cal); WiFi.scanNetworks(true); scanning = true; drawn = false; continue; }
      if (allowClose && uiHit(bClose, x, y)) { waitRelease(lcd, cal); return false; }
      if (!scanning && drawn) {
        int per = 6, pages = (netCount + per - 1) / per;
        // strzałki strony
        if (x < 58 && y > 202 && page > 0) { waitRelease(lcd, cal); page--; drawn = false; continue; }
        if (x > 262 && y > 202 && page < pages - 1) { waitRelease(lcd, cal); page++; drawn = false; continue; }
        int r = (y - 28) / 27;
        if (r >= 0 && r < per) {
          int idx = page * per + r;
          if (idx < netCount && x > MARG && x < W - MARG) {
            lcd.fillRoundRect(MARG, 28 + r * 27, W_IN, 24, 4, ui::BLUE);
            waitRelease(lcd, cal);
            pickSsid = nets[idx].ssid;
            return true;
          }
        }
      }
      waitRelease(lcd, cal);
    }
    delay(20);
  }
}

// ============================== KLAWIATURA ==============================
enum class KbdExit { OK, Back };

inline KbdExit keyboardPage(LGFX& lcd, const TouchCal& cal, const String& ssid, String& pass) {
  static const char* ROWS[2][3] = {
    {"qwertyuiop", "asdfghjkl", "zxcvbnm,."},
    {"1234567890", "@#$%&*+-_=", "()[]{}<>?!"},
  };
  bool shift = false, sym = false, show = false;
  pass = "";

  auto drawCharKey = [&](int cx, int row, char ch, bool invert) {
    char s[2] = {ch, 0};
    uiButton(lcd, KB_X0 + cx * (KB_W + KB_GAP), ROW_Y[row], KB_W, KB_H, s, invert, 2);
  };

  auto drawLayout = [&]() {
    lcd.fillScreen(ui::BG);
    // pasek tytułu: [← Wroc] + nazwa sieci
    uiButton(lcd, MARG, 0, 62, 22, "Wroc", true);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(ui::TEXT);
    lcd.setTextDatum(lgfx::middle_right);
    lcd.drawString(truncStr(ssid, 24), W - MARG, 11);
    lcd.setTextDatum(lgfx::top_left);

    // pole hasła + [pokaz]
    lcd.fillRoundRect(MARG, 26, W_IN - 48, 24, 4, ui::PANEL);
    uiButton(lcd, W - MARG - 44, 26, 44, 24, show ? "abc" : "***", true, 2);

    // rząd klawiszy znakowych
    for (int row = 0; row < 3; row++) {
      int n = strlen(ROWS[sym ? 1 : 0][row]);
      for (int i = 0; i < n; i++) {
        char ch = ROWS[sym ? 1 : 0][row][i];
        if (!sym && shift && ch >= 'a' && ch <= 'z') ch = toupper(ch);
        drawCharKey(i, row, ch, false);
      }
    }
    // rząd specjalny: [SHIFT/ABC][123][spacja][DEL][POLACZ]
    int x = MARG;
    const char* shLbl = sym ? "ABC" : (shift ? "SHIFT*" : "SHIFT");
    uiButton(lcd, x, SP_Y, 46, KB_H, shLbl, shift && !sym, 2);          x += 46 + KB_GAP;
    // 123 ↔ ABC: warstwa trwa (w trybie 123 klawisz jest podświetlony);
    // do liter wraca się przez ABC (slot SHIFT) albo ten klawisz.
    uiButton(lcd, x, SP_Y, 50, KB_H, "123", sym, 2);                    x += 50 + KB_GAP;
    uiButton(lcd, x, SP_Y, 84, KB_H, "___", false, 2);                 x += 84 + KB_GAP;   // spacja
    uiButton(lcd, x, SP_Y, 46, KB_H, "DEL", false, 2);                   x += 46 + KB_GAP;
    uiButton(lcd, x, SP_Y, 70, KB_H, "POLACZ", true, 2);
    // podpowiedź
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(ui::DIM);
    lcd.setTextDatum(lgfx::middle_center);
    lcd.drawString("Puste haslo = siec otwarta", 160, 196);
    lcd.setTextDatum(lgfx::top_left);
  };

  auto drawField = [&]() {
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(ui::TEXT);
    lcd.setTextDatum(lgfx::middle_left);
    lcd.fillRect(MARG + 4, 27, W_IN - 56, 22, ui::PANEL);
    String shown = pass;
    if (!show) {
      shown = "";
      for (unsigned i = 0; i < pass.length(); i++) shown += '.';
    }
    int maxCh = (W_IN - 60) / 6;
    if ((int)shown.length() > maxCh) shown = shown.substring(shown.length() - maxCh);
    lcd.drawString(shown, MARG + 8, 38);
    // kursor
    int cx = MARG + 8 + shown.length() * 6;
    lcd.fillRect(cx, 31, 6, 13, ui::DIM);
    lcd.setTextDatum(lgfx::top_left);
  };

  drawLayout();
  drawField();

  // prostokąty stref (zgodne z drawLayout)
  int spX = MARG;
  uiRect rShift {spX, SP_Y, 46, KB_H};            spX += 46 + KB_GAP;
  uiRect rLayer {spX, SP_Y, 50, KB_H};           spX += 50 + KB_GAP;
  uiRect rSpace {spX, SP_Y, 84, KB_H};           spX += 84 + KB_GAP;
  uiRect rDel   {spX, SP_Y, 46, KB_H};           spX += 46 + KB_GAP;
  uiRect rOk    {spX, SP_Y, 70, KB_H};
  uiRect rBack  {MARG, 0, 62, 22};
  uiRect rShow  {W - MARG - 44, 26, 44, 24};

  uint32_t lastRepeat = 0;
  while (true) {
    int x = 0, y = 0;
    if (touchXY(lcd, cal, &x, &y)) {
      // przytrzymanie DEL → kasowanie ciągłe
      if (uiHit(rDel, x, y)) {
        if (pass.length()) pass.remove(pass.length() - 1);
        drawField();
        delay(300); lastRepeat = millis();
        while (touchXY(lcd, cal, &x, &y) && uiHit(rDel, x, y)) {
          if (millis() - lastRepeat > 130) {
            if (pass.length()) pass.remove(pass.length() - 1);
            drawField();
            lastRepeat = millis();
          }
          delay(20);
        }
        continue;
      }
      if (uiHit(rBack, x, y)) { waitRelease(lcd, cal); return KbdExit::Back; }
      if (uiHit(rOk, x, y))   { waitRelease(lcd, cal); return KbdExit::OK; }
      if (uiHit(rShow, x, y)) { waitRelease(lcd, cal); show = !show; drawLayout(); drawField(); continue; }
      if (uiHit(rShift, x, y)) { waitRelease(lcd, cal); if (sym) { sym = false; } else shift = !shift; drawLayout(); drawField(); continue; }
      if (uiHit(rLayer, x, y)) { waitRelease(lcd, cal); sym = !sym; drawLayout(); drawField(); continue; }
      if (uiHit(rSpace, x, y)) { waitRelease(lcd, cal); if (pass.length() < 63) pass += ' '; drawField(); continue; }

      // klawisze znakowe: 3 rzędy
      for (int row = 0; row < 3; row++) {
        if (y >= ROW_Y[row] && y < ROW_Y[row] + KB_H && x >= KB_X0) {
          int col = (x - KB_X0) / (KB_W + KB_GAP);
          int inKey = (x - KB_X0) % (KB_W + KB_GAP);
          if (col < 10 && inKey < KB_W) {
            const char* r = ROWS[sym ? 1 : 0][row];
            if (col < (int)strlen(r)) {
              char ch = r[col];
              if (!sym && shift && ch >= 'a' && ch <= 'z') ch = toupper(ch);
              // mignięcie klawisza
              drawCharKey(col, row, ch, true);
              if (pass.length() < 63) pass += ch;
              drawField();
              delay(60);
              drawCharKey(col, row, ch, false);
              waitRelease(lcd, cal);
            }
          }
          break;
        }
      }
      continue;
    }
    delay(15);
  }
}

} // namespace wifi_setup

// ============================== ORKIESTRACJA ==============================
// true  → użytkownik wybrał sieć i hasło (już zapisane w NVS)
// false → [Zamknij] (możliwe tylko gdy Wi-Fi działa — powrót do dashboardu)
inline bool runWifiSetup(LGFX& lcd, const TouchCal& cal, Preferences& prefs,
                        const String& openSSID, String& outSSID, String& outPass) {
  String ssid = openSSID;
  while (true) {
    String pass;
    if (!ssid.isEmpty()) {
      // retry po złym haśle → od razu klawiatura
      wifi_setup::KbdExit r = wifi_setup::keyboardPage(lcd, cal, ssid, pass);
      if (r == wifi_setup::KbdExit::Back) ssid = "";          // → lista sieci
      else {
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        outSSID = ssid; outPass = pass;
        return true;
      }
    }
    String pick;
    bool allowClose = (WiFi.status() == WL_CONNECTED);
    if (!wifi_setup::listPage(lcd, cal, pick, allowClose)) return false;
    ssid = pick;
    // następna iteracja: klawiatura dla wybranej sieci
  }
}
