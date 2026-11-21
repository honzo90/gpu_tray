// gpu_cyd — mini-monitor Maca na CYD (ESP32-2432S028R, 320x240 + dotyk).
//
// Strumień danych: gpu-tray (macOS) → UDP, port 45678.
//   ESP32 rozgłasza "CYDREQ\n" (broadcast, póki nie zna IP Maca),
//   gpu-tray odpowiada jednym JSON-em ze statystykami na port źródłowy (45679).
//
// Pierwsze uruchomienie: kalibracja dotyku → wybór sieci Wi-Fi → hasło z klawiatury
// ekranowej → zapis do NVS. Później łączy się automatycznie. ⚙ w rogu dashboardu
// otwiera konfigurację ponownie. Panel WWW z ustawieniami: http://gpu-cyd.local (web.h).
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include "gfx.h"
#include "ui_dash.h"
#include "ui_wifi.h"

#define FW_VERSION   "1.2.0"
#define WEB_HOSTNAME "gpu-cyd"

LGFX lcd;
Preferences prefs;
Dashboard dash;
WiFiUDP udp;
TouchCal tcal;

String cfgSSID, cfgPass;
String retrySSID;   // niepuste → konfiguracja otwiera od razu klawiaturę tej sieci (po złym haśle)

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

// ---- Tryb nocny: w godzinach nightFrom..nightTo podświetlenie wyłączone (0).
// Czas z zegara Maca (pole "t" w pakiecie), między pakietami dociągany z millis().
// Dotknięcie budzi ekran na nightWakeS sekund (pierwsze dotknięcie tylko budzi).
// Ustawienia z panelu WWW, w NVS: non (wł.), nfr/nto (minuty od północy), nwk (s).
bool     nightOn    = true;
int      nightFrom  = 22 * 60;                // 22:00
int      nightTo    = 7 * 60;                 // 7:00
uint16_t nightWakeS = 30;
uint32_t wakeUntil = 0;
bool nightLogged = false;

void saveNight() {
  prefs.putBool("non", nightOn);
  prefs.putUShort("nfr", nightFrom);
  prefs.putUShort("nto", nightTo);
  prefs.putUShort("nwk", nightWakeS);
  Serial.printf("[noc] %s %02d:%02d-%02d:%02d, wybudzenie %ds\n", nightOn ? "wl" : "wyl",
                nightFrom / 60, nightFrom % 60, nightTo / 60, nightTo % 60, nightWakeS);
}

// Minuta doby wg zegara Maca; -1 gdy jeszcze nie znamy czasu.
int macMinuteOfDay(uint32_t now) {
  if (!dash.st.everRx) return -1;
  int h = 0, m = 0;
  if (sscanf(dash.st.time, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return -1;
  uint32_t elapsedMin = (now - dash.st.rxMs) / 60000;
  return (int)((h * 60 + m + elapsedMin) % 1440);
}

bool isNight(uint32_t now) {
  if (!nightOn || nightFrom == nightTo) return false;
  int m = macMinuteOfDay(now);
  if (m < 0) return false;                   // bez czasu — normalna jasność
  return nightFrom > nightTo ? (m >= nightFrom || m < nightTo)   // okno przez północ
                             : (m >= nightFrom && m < nightTo);
}

bool nightAwake(uint32_t now) { return (int32_t)(wakeUntil - now) > 0; }

// Obrót ekranu o 0/90/180/270° (ćwierćobroty, NVS "rot"). 90° i 270° = układ pionowy 240x320
// tylko dla dashboardu — kalibracja, konfiguracja Wi-Fi, łączenie, kod logowania i OTA są
// projektowane na 320x240, więc pokazujemy je w najbliższej orientacji poziomej (90°→0°, 270°→180°).
uint8_t rotQ = 0;
int8_t  curQ = -1;                           // orientacja ustawiona teraz na LCD (-1 = żadna)

void setOrient(bool dashView) {
  uint8_t q = dashView ? rotQ : (rotQ & 1 ? rotQ - 1 : rotQ);
  if (q == curQ) return;
  curQ = q;
  tcal.rot = q;
  lcd.setRotation(lcdRotFor(q));
  if (dashView) dash.setPortrait(q & 1);
  tLastDraw = 0;                             // przerysuj od razu w nowej orientacji
}

void applyRot(uint8_t q, bool save) {
  rotQ = q & 3;
  if (save) prefs.putUChar("rot", rotQ);
  curQ = -1;
  setOrient(stage == Stage::Dash);
  Serial.printf("[ekran] obrot %d\n", rotQ * 90);
}

#include "web.h"   // panel WWW — używa globali i funkcji powyżej

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
  // "rot" (0..3) od v1.2; starsze wersje miały tylko "rot180"
  rotQ = prefs.isKey("rot") ? prefs.getUChar("rot", 0) & 3 : (prefs.getBool("rot180", false) ? 2 : 0);
  nightOn    = prefs.getBool("non", true);
  nightFrom  = prefs.getUShort("nfr", 22 * 60) % 1440;
  nightTo    = prefs.getUShort("nto", 7 * 60) % 1440;
  nightWakeS = constrain(prefs.getUShort("nwk", 30), 5, 600);
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
  WiFi.setHostname(WEB_HOSTNAME);
  // IPv6 (link-local) → mDNS odpowiada też na AAAA; bez tego macOS czeka ~5 s na
  // timeout zapytania AAAA przy każdym otwarciu http://gpu-cyd.local
  WiFi.enableIPv6();
  setOrient(false);
  Serial.printf("[boot] fw=%s stage=%d kalibracja=%d ssid='%s'\n", FW_VERSION, (int)stage, (int)tcal.valid, cfgSSID.c_str());
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
    webBegin();
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(ui::DIM);
    lcd.setTextDatum(lgfx::middle_center);
    lcd.drawString("Ustawienia: http://" WEB_HOSTNAME ".local", 160, 204);
    lcd.setTextDatum(lgfx::top_left);
    delay(1500);                  // pokazujemy IP i adres panelu przez chwilę
    return true;
  }
  WiFi.disconnect();
  Serial.println("[wifi] porazka");
  return false;
}

// Czeka, aż palec będzie trzymany ms milisekund (krótkie zaniki odczytu < 150 ms wybaczamy).
bool holdTouch(uint32_t ms) {
  uint32_t t0 = millis(), lastOn = t0;
  int x, y;
  while (millis() - t0 < ms) {
    if (touchXY(lcd, tcal, &x, &y)) lastOn = millis();
    else if (millis() - lastOn > 150) return false;
    delay(15);
  }
  return true;
}

void dashboardLoop() {
  webLoop();
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

  // kod logowania do panelu WWW zajmuje ekran (dane z Maca wyżej nadal odbieramy)
  if (loginTick(now)) { delay(10); return; }
  setOrient(true);                           // dashboard w wybranej orientacji (też pionowej)

  // 'S' na porcie szeregowym → zrzut ekranu (esp32/cyd_screenshot.py)
  while (Serial.available()) {
    if (Serial.read() == 'S') dash.dumpScreen(now);
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
      wakeUntil = now + nightWakeS * 1000UL;      // każde dotknięcie przedłuża
      if (!wasAwake) {                       // wygaszony ekran: dotyk tylko budzi
        dash.draw(now);
        setBacklight(BRI_LEVELS[briIdx]);
        waitRelease(lcd, tcal);
        tLastDraw = millis();
        return;
      }
    }
    if (dash.hitHost(x, y)) {      // ukryty gest: przytrzymaj 1,5 s → kod logowania WWW
      if (holdTouch(1500)) startLogin(true);
      waitRelease(lcd, tcal);
      return;
    }
    if (dash.hitGear(x, y)) {
      waitRelease(lcd, tcal);
      stage = Stage::Setup;
      return;
    }
    if (dash.hitRotate(x, y)) {
      waitRelease(lcd, tcal);
      applyRot(rotQ + 1, true);    // ↻: kolejne 90° (0 → 90 → 180 → 270 → 0)
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
      setOrient(false);
      setBacklight(BRI_LEVELS[briIdx]);     // konfiguracja zawsze widoczna (też w nocy)
      runCalibration(lcd, prefs, tcal);
      stage = cfgSSID.isEmpty() ? Stage::Setup : Stage::Connect;
      break;

    case Stage::Setup: {
      Serial.println("[etap] konfiguracja Wi-Fi");
      setOrient(false);
      setBacklight(BRI_LEVELS[briIdx]);     // konfiguracja zawsze widoczna (też w nocy)
      String ssid, pass;
      String open = retrySSID;     // ⚙ z dashboardu → lista sieci; po porażce → klawiatura
      retrySSID = "";
      if (runWifiSetup(lcd, tcal, prefs, open, ssid, pass)) {
        cfgSSID = ssid;
        cfgPass = pass;
        stage = Stage::Connect;
      } else {
        stage = Stage::Dash;   // zamknięto listę → Wi-Fi już działa (wejście przez ⚙)
      }
      break;
    }

    case Stage::Connect:
      setOrient(false);
      if (connectFlow()) {
        stage = Stage::Dash;
      } else {
        showConnectFail(lcd, cfgSSID);
        delay(1800);
        // wracamy prosto do klawiatury tej sieci, by poprawić hasło
        retrySSID = cfgSSID;
        stage = Stage::Setup;
      }
      break;

    case Stage::Dash:
      dashboardLoop();
      break;
  }
}
