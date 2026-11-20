// Konfiguracja ekranu CYD (ESP32-2432S028R) pod LovyanGFX + paleta i pomocniki.
#pragma once
#include <LovyanGFX.hpp>

// Uwaga: świadomie NIE robimy `using namespace lgfx;` — importowałby też
// lgfx::millis()/lgfx::delay(), kolidujące z Arduino przy niekwalifikowanych
// wywołaniach. Fonty (fonts::Font2…) LovyanGFX eksportuje już sam;
// dodatkowo importujemy tylko dwa potrzebne typy:
using lgfx::IFont;
using lgfx::LGFXBase;

// Ekran CYD: ILI9341 320x240 (portret 240x320) na HSPI, dotyk XPT2046 na osobnych pinach.
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
  lgfx::Touch_XPT2046 _touch;

public:
  LGFX() {
    { // Magistrala SPI ekranu (piny CYD).
      auto cfg = _bus.config();
      cfg.spi_host   = SPI2_HOST;      // HSPI: piny 12/13/14
      cfg.spi_mode  = 0;
      cfg.freq_write = 40000000;      // 40 MHz zapisu — bezpiecznie dla CYD
      cfg.freq_read  = 16000000;
      cfg.pin_sclk   = 14;
      cfg.pin_mosi   = 13;
      cfg.pin_miso   = 12;
      cfg.pin_dc     = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    { // Panel ILI9341 (natywnie 240x320 — do 320x240 obracamy setRotation).
      auto cfg = _panel.config();
      cfg.pin_cs         = 15;
      cfg.pin_rst        = -1;        // RST zwarte do EN
      cfg.panel_width   = 240;
      cfg.panel_height  = 320;
      cfg.memory_width  = 240;
      cfg.memory_height = 320;
      cfg.offset_rotation = 0;
      cfg.invert        = false;
      cfg.rgb_order     = false;
      _panel.config(cfg);
    }
    { // Podświetlenie: GPIO21, PWM 12 kHz.
      auto cfg = _light.config();
      cfg.pin_bl      = 21;
      cfg.freq        = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    { // Dotyk XPT2046 — osobne piny CYD (routing przez macierz GPIO).
      auto cfg = _touch.config();
      cfg.spi_host   = SPI3_HOST;     // VSPI przez macierz GPIO
      cfg.pin_sclk   = 25;
      cfg.pin_mosi   = 32;
      cfg.pin_miso   = 39;
      cfg.pin_cs     = 33;
      cfg.pin_int    = 36;            // PENIRQ (aktywny stan niski)
      cfg.bus_shared = false;
      cfg.freq       = 1000000;
      // Pełny zakres raw — mapowanie na ekran robimy sami (kalibracja trzymana w NVS).
      cfg.x_min = 0; cfg.x_max = 4095;
      cfg.y_min = 0; cfg.y_max = 4095;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

// ---- Geometria ekranu
constexpr int W = 320, H = 240, MARG = 6, W_IN = W - 2 * MARG;  // 308

// Skraca Arduino-String do n znaków (z kropką na końcu).
inline String truncStr(const String& s, int n) {
  return (int)s.length() <= n ? s : s.substring(0, n - 1) + ".";
}

// ---- Paleta (spójna z trayem macOS: zielony < 50% < pomarańczowy < czerwony ≥ 85%)
// Wartości dobrane do poziomów RGB332 (R/G: 0,36,73,109,146,182,219,255; B: 0,85,170,255),
// bo dashboard rysuje na sprite 8bpp — inne kolory byłyby obcinane (ciemne szarości → czerń/oliwka).
namespace ui {
  const uint16_t BG     = lgfx::color565(0, 0, 0);
  const uint16_t PANEL  = lgfx::color565(36, 36, 85);
  const uint16_t PANEL2 = lgfx::color565(73, 73, 85);
  const uint16_t TEXT   = lgfx::color565(255, 255, 255);
  const uint16_t DIM    = lgfx::color565(146, 146, 170);
  const uint16_t GREEN  = lgfx::color565(0, 219, 85);
  const uint16_t ORANGE = lgfx::color565(255, 146, 0);
  const uint16_t RED    = lgfx::color565(255, 36, 85);
  const uint16_t BLUE   = lgfx::color565(73, 182, 255);
}

// ---- Pomocniki rysowania (działają i na LCD, i na sprite-cie)

inline uint16_t uiMix(uint16_t a, uint16_t b, float t) {  // miesza dwa rgb565
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  float r = ((a >> 11) & 0x1F) * (1 - t) + ((b >> 11) & 0x1F) * t;
  float g = ((a >> 5) & 0x3F) * (1 - t) + ((b >> 5) & 0x3F) * t;
  float l = ((a & 0x1F) * (1 - t) + ((b & 0x1F) * t));
  return (uint16_t(r) << 11) | (uint16_t(g) << 5) | uint16_t(l);
}

// Kolor poziomu 0..1 (próg pomarańczowy 50%, czerwony 85% — jak w trayu).
inline uint16_t uiLevelColor(float v) {
  return v >= 0.85f ? ui::RED : (v >= 0.5f ? ui::ORANGE : ui::GREEN);
}

// Pasek postępu z zaokrglonym tłem.
inline void uiBar(lgfx::LGFXBase& g, int x, int y, int w, int h, float frac, uint16_t color) {
  frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
  g.fillRoundRect(x, y, w, h, h / 2, ui::PANEL2);
  int fw = (int)((w - 4) * frac);
  if (fw > 2) g.fillRoundRect(x + 2, y + 2, fw, h - 4, (h - 4) / 2, color);
}

// Przycisk tekstowy; zwraca prostokąt.
struct uiRect { int x, y, w, h; };
inline uiRect uiButton(lgfx::LGFXBase& g, int x, int y, int w, int h,
                       const char* label, bool inverted = false, uint8_t font = 1) {
  g.fillRoundRect(x, y, w, h, 5, inverted ? ui::BLUE : ui::PANEL2);
  g.setTextColor(inverted ? ui::BG : ui::TEXT);
  g.setTextDatum(lgfx::middle_center);
  const void* f = (font == 2) ? (const void*)&fonts::Font2
              : (font == 4) ? (const void*)&fonts::Font4
              : (const void*)&fonts::Font4;
  g.setFont(static_cast<const IFont*>(f));
  g.drawString(label, x + w / 2, y + h / 2);
  g.setTextDatum(lgfx::top_left);
  g.setTextColor(ui::TEXT);
  return {x, y, w, h};
}

inline bool uiHit(const uiRect& r, int x, int y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}
