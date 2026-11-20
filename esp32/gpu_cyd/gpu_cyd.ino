// gpu_cyd — mini-monitor Maca na CYD (ESP32-2432S028R, 320x240 + dotyk).
//
// Strumień danych: gpu-tray (macOS) → UDP, port 45678.
//   ESP32 rozgłasza "CYDREQ\n" (broadcast, póki nie zna IP Maca),
//   gpu-tray odpowiada jednym JSON-em ze statystykami na port źródłowy (45679).
//
// Pierwsze uruchomienie: kalibracja dotyku → wybór sieci Wi-Fi → hasło z klawiatury
// ekranowej → zapis do NVS. Później łączy się automatycznie. ⚙ w rogu dashboardu
// otwiera konfigurację ponownie.
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include "gfx.h"
#include "ui_dash.h"
#include "ui_wifi.h"

LGFX lcd;
Preferences prefs;
Dashboard dash;
WiFiUDP udp;
TouchCal tcal;

String cfgSSID, cfgPass;

enum class Stage { Calibrate, Setup, Connect, Dash };
Stage stage;

static const uint8_t REQ_MSG[] = "CYDREQ\n";
const uint16_t PORT_MAC = 45678;   // port gpu-tray na Macu
const uint16_t PORT_ESP = 45679;   // nasz port (REQ stąd, odpowiedzi tu)

IPAddress macIP;
bool macKnown = false;
uint32_t tLastReq = 0, tLastDraw = 0;

// Jasność podświetlenia: 5 poziomów, maks. 180/255 — nie pełne 255: CYD ma słabą sekcję
// zasilania (AMS1117) i pełne podświetlenie + skok prądu Wi-Fi potrafi wybić zasilanie
// całej płytki. Indeks poziomu w NVS (klucz "bri").
const uint8_t BRI_LEVELS[] = {20, 50, 90, 135, 180};
const uint8_t BRI_COUNT = sizeof(BRI_LEVELS);
const uint8_t BRI_DEFAULT = BRI_COUNT - 1;   // 180 — jak dotychczas
uint8_t briIdx = BRI_DEFAULT;
int16_t blNow = -1;                          // aktualnie ustawione podświetlenie (-1 = nieznane)

// Ustawia podświetlenie tylko przy zmianie (setBrightness to zapis PWM — bez sensu co pętlę).
void setBacklight(uint8_t v) {
  if (blNow == v) return;
  lcd.setBrightness(v);
  blNow = v;
}

void applyBrightness(uint8_t idx, bool save) {
  briIdx = idx < BRI_COUNT ? idx : BRI_DEFAULT;
  setBacklight(BRI_LEVELS[briIdx]);
  if (save) prefs.putUChar("bri", briIdx);
  Serial.printf("[ekran] jasnosc %d/%d (%d)\n", briIdx + 1, BRI_COUNT, BRI_LEVELS[briIdx]);
}

// ---- Tryb nocny: w godzinach NIGHT_FROM..NIGHT_TO podświetlenie wyłączone (0).
// Czas z zegara Maca (pole "t" w pakiecie), między pakietami dociągany z millis().
// Dotknięcie budzi ekran na NIGHT_WAKE_MS (pierwsze dotknięcie tylko budzi).
const int NIGHT_FROM = 22 * 60;              // minuty od północy: 22:00
const int NIGHT_TO   = 7 * 60;               // 7:00
const uint32_t NIGHT_WAKE_MS = 30000;
uint32_t wakeUntil = 0;
bool nightLogged = false;

// Minuta doby wg zegara Maca; -1 gdy jeszcze nie znamy czasu.
int macMinuteOfDay(uint32_t now) {
  if (!dash.st.everRx) return -1;
  int h = 0, m = 0;
  if (sscanf(dash.st.time, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return -1;
  uint32_t elapsedMin = (now - dash.st.rxMs) / 60000;
  return (int)((h * 60 + m + elapsedMin) % 1440);
}

bool isNight(uint32_t now) {
  int m = macMinuteOfDay(now);
  if (m < 0) return false;                   // bez czasu — normalna jasność
  return NIGHT_FROM > NIGHT_TO ? (m >= NIGHT_FROM || m < NIGHT_TO)   // okno przez północ
                               : (m >= NIGHT_FROM && m < NIGHT_TO);
}

bool nightAwake(uint32_t now) { return (int32_t)(wakeUntil - now) > 0; }

// Obrót o 180°: rotation 3 zamiast 1 + odbicie dotyku (TouchCal::flip); zapis w NVS.
void applyFlip(bool flip, bool save) {
  tcal.flip = flip;
  lcd.setRotation(flip ? 3 : 1);
  if (save) prefs.putBool("rot180", flip);
  Serial.printf("[ekran] obrot 180 = %d\n", (int)flip);
}

void setup() {
  Serial.begin(115200);
  lcd.init();
  lcd.setRotation(1);
  lcd.fillScreen(ui::BG);

  prefs.begin("gpucyd", false);
  applyBrightness(prefs.getUChar("bri", BRI_DEFAULT), false);
  if (prefs.getBool("calv", false)) {
    tcal.sx1 = prefs.getUShort("sx1", 0);  tcal.sx2 = prefs.getUShort("sx2", 1);
    tcal.sy1 = prefs.getUShort("sy1", 0);  tcal.sy2 = prefs.getUShort("sy2", 1);
    tcal.xFromY = prefs.getBool("xfy", false);
    tcal.yFromX = prefs.getBool("yfx", false);
    tcal.valid = true;
  }
  cfgSSID = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  applyFlip(prefs.getBool("rot180", false), false);
  lcd.fillScreen(ui::BG);

  dash.begin(lcd);
  WiFi.mode(WIFI_STA);
  // 11 dBm zamiast domyślnych 19,5: skan/nadawanie zużywa najwięcej prądu,
  // a przy ekranie stojącym obok routera różnicy w zasięgu nie ma.
  WiFi.setTxPower(WIFI_POWER_11dBm);
  WiFi.setSleep(false);

  stage = !tcal.valid ? Stage::Calibrate
       : cfgSSID.isEmpty() ? Stage::Setup
       : Stage::Connect;
  Serial.printf("[boot] stage=%d kalibracja=%d ssid='%s'\n", (int)stage, (int)tcal.valid, cfgSSID.c_str());
}

// Łączy z zapisanymi danymi; false = porażka (złe hasło / sieć zniknęła).
bool connectFlow() {
  Serial.printf("[wifi] lacze z '%s'\n", cfgSSID.c_str());
  drawConnecting(lcd, cfgSSID);
  WiFi.begin(cfgSSID.c_str(), cfgPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 25000) {
    delay(120);
    connectingTick(lcd, millis(), false, "");
  }
  if (WiFi.status() == WL_CONNECTED) {
    connectingTick(lcd, millis(), true, WiFi.localIP().toString());
    Serial.printf("[wifi] OK, IP=%s\n", WiFi.localIP().toString().c_str());
    udp.begin(PORT_ESP);
    macKnown = false;             // po restarcie sieci MAC mógł zmienić IP
    delay(900);                   // pokazujemy IP przez chwilę
    return true;
  }
  WiFi.disconnect();
  Serial.println("[wifi] porazka");
  return false;
}

void dashboardLoop() {
  uint32_t now = millis();

  // Wi-Fi spadło? Najpierw cicha próba, potem pełny ekran łączenia.
  static uint32_t wifiDownSince = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiDownSince) { wifiDownSince = now; WiFi.reconnect(); }
    if (now - wifiDownSince > 12000) { wifiDownSince = 0; stage = Stage::Connect; return; }
  } else {
    wifiDownSince = 0;
  }

  // REQ co 1 s: unicast do znanego Maca, inaczej broadcast podsieci.
  if (now - tLastReq >= 1000) {
    tLastReq = now;
    bool haveMac = macKnown && dash.st.everRx && (now - dash.st.rxMs < 10000);
    IPAddress dst = haveMac ? macIP
                   : IPAddress(((uint32_t)WiFi.localIP()) | ~((uint32_t)WiFi.subnetMask()));
    if (udp.beginPacket(dst, PORT_MAC)) {
      udp.write(REQ_MSG, sizeof(REQ_MSG) - 1);
      udp.endPacket();
    }
  }

  // odpowiedzi gpu-tray
  int n = udp.parsePacket();
  while (n) {
    char buf[384];
    int len = udp.read(buf, sizeof(buf) - 1);
    if (len > 0 && buf[0] == '{') {
      buf[len] = 0;
      macIP = udp.remoteIP();
      macKnown = true;
      dash.parse(buf, now);
    }
    n = udp.parsePacket();
  }

  bool night = isNight(now);
  if (night != nightLogged) {
    nightLogged = night;
    Serial.printf("[noc] tryb nocny %s (%s)\n", night ? "ON" : "OFF", dash.st.time);
  }

  // ⚙ → konfiguracja, ↻ → obrót ekranu o 180°, ☀ → następny poziom jasności (w kółko)
  int x = 0, y = 0;
  if (touchXY(lcd, tcal, &x, &y)) {
    if (night) {
      bool wasAwake = nightAwake(now);
      wakeUntil = now + NIGHT_WAKE_MS;      // każde dotknięcie przedłuża
      if (!wasAwake) {                       // wygaszony ekran: dotyk tylko budzi
        dash.draw(now);
        setBacklight(BRI_LEVELS[briIdx]);
        waitRelease(lcd, tcal);
        tLastDraw = millis();
        return;
      }
    }
    if (dash.hitGear(x, y)) {
      waitRelease(lcd, tcal);
      stage = Stage::Setup;
      return;
    }
    if (dash.hitRotate(x, y)) {
      waitRelease(lcd, tcal);
      applyFlip(!tcal.flip, true);
      tLastDraw = 0;               // przerysuj od razu w nowej orientacji
      return;
    }
    if (dash.hitBright(x, y)) {
      applyBrightness((briIdx + 1) % BRI_COUNT, true);
      dash.showBrightness(briIdx, BRI_COUNT, millis());
      dash.draw(millis());         // okienko od razu, jeszcze przed puszczeniem palca
      waitRelease(lcd, tcal);
      tLastDraw = millis();
      return;
    }
  }

  // podświetlenie: noc bez wybudzenia → 0, inaczej wybrany poziom
  setBacklight(night && !nightAwake(now) ? 0 : BRI_LEVELS[briIdx]);

  // rysowanie: 250 ms gdy Ollama pracuje (puls), inaczej 500 ms
  bool busyLive = dash.st.busy && dash.st.everRx && (now - dash.st.rxMs < 5000);
  if (now - tLastDraw >= (busyLive ? 250 : 500)) {
    tLastDraw = now;
    dash.draw(now);
  }
  delay(10);
}

void loop() {
  switch (stage) {
    case Stage::Calibrate:
      Serial.println("[etap] kalibracja dotyku");
      setBacklight(BRI_LEVELS[briIdx]);     // konfiguracja zawsze widoczna (też w nocy)
      runCalibration(lcd, prefs, tcal);
      stage = cfgSSID.isEmpty() ? Stage::Setup : Stage::Connect;
      break;

    case Stage::Setup: {
      Serial.println("[etap] konfiguracja Wi-Fi");
      setBacklight(BRI_LEVELS[briIdx]);     // konfiguracja zawsze widoczna (też w nocy)
      String ssid, pass;
      if (runWifiSetup(lcd, tcal, prefs, cfgSSID, ssid, pass)) {
        cfgSSID = ssid;
        cfgPass = pass;
        stage = Stage::Connect;
      } else {
        stage = Stage::Dash;   // zamknięto listę → Wi-Fi już działa (wejście przez ⚙)
      }
      break;
    }

    case Stage::Connect:
      if (connectFlow()) {
        stage = Stage::Dash;
      } else {
        showConnectFail(lcd, cfgSSID);
        delay(1800);
        // wracamy prosto do klawiatury tej sieci (openSSID=cfgSSID), by poprawić hasło
        stage = Stage::Setup;
      }
      break;

    case Stage::Dash:
      dashboardLoop();
      break;
  }
}
