// Panel WWW z ustawieniami CYD: http://gpu-cyd.local (lub http://<IP CYD>).
//
// Endpointy:
//   GET  /               strona (HTML+JS, poniżej)
//   GET  /api/status     stan: IP, RSSI, Mac, czas, uptime, wersja
//   GET  /api/settings   ustawienia (bez hasła Wi-Fi)
//   POST /api/settings   JSON {bri, rot180, night:{on, from, to, wake}} — dowolny podzbiór
//   GET  /api/scan       lista sieci Wi-Fi
//   POST /api/wifi       JSON {ssid, pass} → zapis + restart (pusty pass = bez zmiany przy tym samym SSID)
//   POST /api/restart    restart
//   POST /api/calibrate  kasuje kalibrację dotyku → restart → kalibracja na ekranie
//   POST /api/update     upload pliku .bin (gpu_cyd-<v>-update.bin) → OTA → restart
//
// Bez logowania — panel działa tylko w sieci lokalnej (jak cała komunikacja z Makiem).
// Plik dołączany w gpu_cyd.ino PO definicjach globali i funkcji apply* (używa ich wprost).
#pragma once
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>

WebServer web(80);
uint32_t restartAt = 0;          // != 0 → restart o tej porze (po wysłaniu odpowiedzi)
bool otaOk = false;
const char* otaErr = nullptr;    // własny powód odrzucenia (np. zły plik)

static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="pl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GPU CYD</title>
<style>
:root{--bg:#0b0d12;--card:#151924;--line:#262c3b;--text:#e8ebf2;--dim:#8b93a7;--acc:#49b6ff;--ok:#00db55;--warn:#ff9200;--bad:#ff2455}
@media (prefers-color-scheme:light){:root{--bg:#f3f5f9;--card:#fff;--line:#dde2ec;--text:#141821;--dim:#5d6578;--acc:#0a84d6;--ok:#0a9e45;--warn:#c76e00;--bad:#d61f45}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
main{max-width:560px;margin:0 auto;padding:20px 16px 40px}
h1{font-size:20px;margin:4px 0 2px}
.sub{color:var(--dim);font-size:13px;margin-bottom:18px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px;margin-bottom:14px}
.card h2{font-size:13px;text-transform:uppercase;letter-spacing:.06em;color:var(--dim);margin:0 0 12px}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:6px 0}
.row+.row{border-top:1px solid var(--line)}
.row label{flex:1}
.k{color:var(--dim)}
.v{font-variant-numeric:tabular-nums;text-align:right;word-break:break-all}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;background:var(--dim)}
.dot.ok{background:var(--ok)}.dot.bad{background:var(--bad)}
input[type=range]{width:100%;accent-color:var(--acc)}
input[type=time],input[type=number],input[type=text],input[type=password],select{
 background:var(--bg);color:var(--text);border:1px solid var(--line);border-radius:8px;padding:7px 9px;font:inherit;min-width:0}
input[type=number]{width:80px}
.full{width:100%}
.seg{display:flex;gap:6px}
.seg button{flex:1}
button{background:var(--line);color:var(--text);border:0;border-radius:8px;padding:9px 14px;font:inherit;cursor:pointer}
button.pri{background:var(--acc);color:#fff}
button.danger{background:transparent;color:var(--bad);border:1px solid var(--bad)}
button:disabled{opacity:.5;cursor:default}
.sw{position:relative;width:46px;height:26px;flex:none}
.sw input{opacity:0;width:0;height:0}
.sw span{position:absolute;inset:0;background:var(--line);border-radius:13px;transition:.15s;cursor:pointer}
.sw span:before{content:"";position:absolute;width:20px;height:20px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.15s}
.sw input:checked+span{background:var(--acc)}
.sw input:checked+span:before{transform:translateX(20px)}
.times{display:flex;gap:8px;align-items:center}
.stack{display:flex;flex-direction:column;gap:8px}
.actions{display:flex;flex-wrap:wrap;gap:8px}
progress{width:100%;height:8px;accent-color:var(--acc)}
.hint{color:var(--dim);font-size:13px;margin:6px 0 0}
#toast{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);background:var(--text);color:var(--bg);
 padding:9px 16px;border-radius:20px;font-size:14px;opacity:0;transition:opacity .2s;pointer-events:none}
#toast.on{opacity:1}
</style></head><body><main>
<h1>GPU CYD</h1>
<div class="sub" id="sub">Ustawienia ekranu</div>

<div class="card"><h2>Stan</h2>
 <div class="row"><span class="k">Mac</span><span class="v" id="sMac">-</span></div>
 <div class="row"><span class="k">Czas (Mac)</span><span class="v" id="sTime">-</span></div>
 <div class="row"><span class="k">Wi-Fi</span><span class="v" id="sWifi">-</span></div>
 <div class="row"><span class="k">Tryb nocny</span><span class="v" id="sNight">-</span></div>
 <div class="row"><span class="k">Działa od</span><span class="v" id="sUp">-</span></div>
</div>

<div class="card"><h2>Ekran</h2>
 <div class="row"><label for="bri">Jasność <span class="k" id="briV"></span></label></div>
 <input type="range" id="bri" min="1" max="5" step="1">
 <div class="row" style="margin-top:8px"><label for="rot">Obrót o 180°</label>
  <label class="sw"><input type="checkbox" id="rot"><span></span></label></div>
</div>

<div class="card"><h2>Tryb nocny</h2>
 <div class="row"><label for="non">Wygaszanie w nocy</label>
  <label class="sw"><input type="checkbox" id="non"><span></span></label></div>
 <div class="row"><span>Godziny</span>
  <span class="times"><input type="time" id="nfr"> – <input type="time" id="nto"></span></div>
 <div class="row"><label for="nwk">Wybudzenie dotykiem (s)</label>
  <input type="number" id="nwk" min="5" max="600" step="5"></div>
 <p class="hint">Czas brany z zegara Maca. Dotknięcie ekranu w nocy włącza go na podany czas.</p>
</div>

<div class="card"><h2>Wi-Fi</h2>
 <div class="stack">
  <div class="times"><select id="ssidSel" class="full"><option value="">— wybierz sieć —</option></select>
   <button id="scan">Skanuj</button></div>
  <input type="text" id="ssid" class="full" placeholder="SSID" autocomplete="off">
  <input type="password" id="pass" class="full" placeholder="Hasło (puste = bez zmiany)" autocomplete="new-password">
  <div class="actions"><button class="pri" id="wifiSave">Zapisz i połącz</button></div>
 </div>
 <p class="hint">Po zapisie CYD uruchomi się ponownie. Przy złym haśle pokaże klawiaturę na ekranie.</p>
</div>

<div class="card"><h2>Firmware</h2>
 <div class="row"><span class="k">Wersja</span><span class="v" id="sFw">-</span></div>
 <div class="stack" style="margin-top:8px">
  <input type="file" id="fw" accept=".bin">
  <progress id="fwP" value="0" max="100" hidden></progress>
  <div class="actions"><button class="pri" id="fwUp">Wgraj aktualizację</button></div>
 </div>
 <p class="hint">Plik <code>gpu_cyd-&lt;wersja&gt;-update.bin</code> z GitHub Releases. Ustawienia zostają.</p>
</div>

<div class="card"><h2>Urządzenie</h2>
 <div class="actions">
  <button id="restart">Uruchom ponownie</button>
  <button class="danger" id="calib">Kalibracja dotyku</button>
 </div>
</div>
</main><div id="toast"></div>
<script>
const $=id=>document.getElementById(id);
const BRI=[20,50,90,135,180];
let saveT=null;
function toast(t){const e=$('toast');e.textContent=t;e.classList.add('on');clearTimeout(e._t);e._t=setTimeout(()=>e.classList.remove('on'),1800)}
async function api(p,body){const r=await fetch(p,body===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
 if(!r.ok)throw new Error(await r.text()||r.status);return r.headers.get('content-type')?.includes('json')?r.json():r.text()}
function hm(m){return String(Math.floor(m/60)).padStart(2,'0')+':'+String(m%60).padStart(2,'0')}
function dur(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+' d ':'')+(d||h?h+' h ':'')+m+' min'}
function briLabel(){$('briV').textContent=$('bri').value+'/5'}
async function load(){const s=await api('/api/settings');
 $('bri').value=s.bri+1;briLabel();$('rot').checked=s.rot180;
 $('non').checked=s.night.on;$('nfr').value=s.night.from;$('nto').value=s.night.to;$('nwk').value=s.night.wake;
 $('ssid').value=s.ssid}
async function status(){try{const s=await api('/api/status');
 $('sub').textContent=s.ip+' · '+s.hostname+'.local';
 $('sMac').innerHTML=s.mac.ok?'<span class="dot ok"></span>'+(s.mac.host||'')+' ('+s.mac.ip+')'
   :'<span class="dot bad"></span>'+(s.mac.ip?'brak danych od '+s.mac.age+' s':'nie znaleziono');
 $('sTime').textContent=s.time||'-';
 $('sWifi').textContent=s.ssid+' · '+s.rssi+' dBm';
 $('sNight').textContent=s.night?(s.awake?'noc (wybudzony)':'noc (wygaszony)'):'dzień';
 $('sUp').textContent=dur(s.uptime);$('sFw').textContent=s.fw}catch(e){$('sub').textContent='Brak połączenia z CYD'}}
function collect(){return{bri:$('bri').value-1,rot180:$('rot').checked,
 night:{on:$('non').checked,from:$('nfr').value,to:$('nto').value,wake:+$('nwk').value}}}
function save(){clearTimeout(saveT);saveT=setTimeout(async()=>{try{await api('/api/settings',collect());toast('Zapisano');status()}catch(e){toast('Błąd: '+e.message)}},250)}
$('bri').oninput=()=>{briLabel();save()};
['rot','non','nfr','nto','nwk'].forEach(i=>$(i).onchange=save);
$('scan').onclick=async()=>{const b=$('scan');b.disabled=true;b.textContent='...';
 try{const l=await api('/api/scan');const s=$('ssidSel');s.length=1;
  l.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+'  ('+n.rssi+' dBm'+(n.open?', otwarta':'')+')';s.appendChild(o)})}
 catch(e){toast('Błąd skanu')}b.disabled=false;b.textContent='Skanuj'};
$('ssidSel').onchange=e=>{if(e.target.value){$('ssid').value=e.target.value;$('pass').focus()}};
$('wifiSave').onclick=async()=>{const ssid=$('ssid').value.trim();if(!ssid)return toast('Podaj SSID');
 if(!confirm('Zapisać sieć „'+ssid+'" i uruchomić CYD ponownie?'))return;
 try{await api('/api/wifi',{ssid,pass:$('pass').value});toast('Zapisano — restart');$('pass').value=''}catch(e){toast('Błąd: '+e.message)}};
$('restart').onclick=async()=>{if(!confirm('Uruchomić CYD ponownie?'))return;await api('/api/restart',{}).catch(()=>{});toast('Restart…')};
$('calib').onclick=async()=>{if(!confirm('Skasować kalibrację? Po restarcie trzeba będzie dotknąć znaczników na ekranie CYD.'))return;
 await api('/api/calibrate',{}).catch(()=>{});toast('Restart — kalibracja na ekranie')};
$('fwUp').onclick=()=>{const f=$('fw').files[0];if(!f)return toast('Wybierz plik .bin');
 if(!confirm('Wgrać '+f.name+' ('+Math.round(f.size/1024)+' KB)?'))return;
 const x=new XMLHttpRequest(),fd=new FormData(),p=$('fwP');fd.append('fw',f);p.hidden=false;p.value=0;$('fwUp').disabled=true;
 x.upload.onprogress=e=>{if(e.lengthComputable)p.value=e.loaded*100/e.total};
 x.onload=()=>{$('fwUp').disabled=false;toast(x.status==200?'Wgrano — restart':'Błąd: '+x.responseText);if(x.status==200)setTimeout(()=>location.reload(),9000)};
 x.onerror=()=>{$('fwUp').disabled=false;toast('Błąd wysyłania')};
 x.open('POST','/api/update');x.send(fd)};
load().catch(()=>{});status();setInterval(status,3000);
</script></body></html>)HTML";

static void sendJson(JsonDocument& d, int code = 200) {
  String out;
  serializeJson(d, out);
  web.send(code, "application/json", out);
}

// "HH:MM" → minuty od północy; -1 przy błędzie.
static int parseHM(const char* s) {
  int h, m;
  if (!s || sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

static String fmtHM(int m) {
  char b[6];
  snprintf(b, sizeof(b), "%02d:%02d", m / 60, m % 60);
  return b;
}

// Treść POST jako JSON; przy błędzie od razu odsyła 400 z opisem.
static bool parseBody(JsonDocument& d) {
  String body = web.arg("plain");
  DeserializationError e = deserializeJson(d, body);
  if (!e) return true;
  web.send(400, "text/plain", String("zly JSON: ") + e.c_str() + " (" + body.length() + " B)");
  return false;
}

static void handleStatus() {
  uint32_t now = millis();
  JsonDocument d;
  d["fw"] = FW_VERSION;
  d["hostname"] = WEB_HOSTNAME;
  d["ip"] = WiFi.localIP().toString();
  d["ssid"] = WiFi.SSID();
  d["rssi"] = WiFi.RSSI();
  d["uptime"] = now / 1000;
  d["heap"] = ESP.getFreeHeap();
  JsonObject mac = d["mac"].to<JsonObject>();
  mac["ok"] = dash.st.everRx && now - dash.st.rxMs < 5000;
  mac["ip"] = macKnown ? macIP.toString() : "";
  mac["host"] = dash.st.host;
  mac["age"] = dash.st.everRx ? (now - dash.st.rxMs) / 1000 : -1;
  d["time"] = dash.st.everRx ? dash.st.time : "";
  d["night"] = isNight(now);
  d["awake"] = nightAwake(now);
  sendJson(d);
}

static void handleGetSettings() {
  JsonDocument d;
  d["bri"] = briIdx;
  d["rot180"] = tcal.flip;
  d["ssid"] = cfgSSID;              // hasła nie zwracamy
  JsonObject n = d["night"].to<JsonObject>();
  n["on"] = nightOn;
  n["from"] = fmtHM(nightFrom);
  n["to"] = fmtHM(nightTo);
  n["wake"] = nightWakeS;
  sendJson(d);
}

static void handlePostSettings() {
  JsonDocument d;
  if (!parseBody(d)) return;

  if (d["bri"].is<int>()) {
    int b = d["bri"];
    if (b >= 0 && b < BRI_COUNT && b != briIdx) {
      applyBrightness(b, true);
      dash.showBrightness(briIdx, BRI_COUNT, millis());
      tLastDraw = 0;
    }
  }
  if (d["rot180"].is<bool>()) {
    bool f = d["rot180"];
    if (f != tcal.flip) { applyFlip(f, true); tLastDraw = 0; }
  }
  JsonObject n = d["night"];
  if (!n.isNull()) {
    if (n["from"].is<const char*>() || n["to"].is<const char*>()) {
      int fr = n["from"].is<const char*>() ? parseHM(n["from"]) : nightFrom;
      int to = n["to"].is<const char*>() ? parseHM(n["to"]) : nightTo;
      if (fr < 0 || to < 0) { web.send(400, "text/plain", "zla godzina (HH:MM)"); return; }
      nightFrom = fr; nightTo = to;
    }
    if (n["on"].is<bool>()) nightOn = n["on"];
    if (n["wake"].is<int>()) nightWakeS = constrain((int)n["wake"], 5, 600);
    saveNight();
  }
  handleGetSettings();
}

static void handleScan() {
  int n = WiFi.scanNetworks(false, false);
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  // kopiujemy wyniki PRZED scanDelete (po nim WiFi.SSID(i) wskazuje w zwolnioną pamięć)
  for (int i = 0; i < n && i < 25; i++) {
    String s = WiFi.SSID(i);
    if (s.isEmpty()) continue;
    bool dup = false;
    for (JsonObject o : a) if (s == (const char*)o["ssid"]) { dup = true; break; }
    if (dup) continue;               // ta sama sieć z kilku AP — lista i tak posortowana po RSSI
    JsonObject o = a.add<JsonObject>();
    o["ssid"] = s;
    o["rssi"] = WiFi.RSSI(i);
    o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendJson(d);
}

static void handleWifi() {
  JsonDocument d;
  if (!parseBody(d)) return;
  String ssid = d["ssid"] | "";
  String pass = d["pass"] | "";
  ssid.trim();
  if (ssid.isEmpty() || ssid.length() > 32 || pass.length() > 63) {
    web.send(400, "text/plain", "zly SSID lub haslo"); return;
  }
  if (pass.isEmpty() && ssid == cfgSSID) pass = cfgPass;   // ta sama sieć + puste hasło = stare hasło
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  Serial.printf("[web] nowa siec '%s' — restart\n", ssid.c_str());
  web.send(200, "text/plain", "ok");
  restartAt = millis() + 800;
}

static void handleRestart() {
  web.send(200, "text/plain", "ok");
  restartAt = millis() + 500;
}

static void handleCalibrate() {
  prefs.putBool("calv", false);
  Serial.println("[web] kalibracja skasowana — restart");
  web.send(200, "text/plain", "ok");
  restartAt = millis() + 500;
}

// Ekran podczas OTA (rysujemy wprost na LCD — dashboard w tym czasie stoi).
static void otaScreen(int pct, const char* msg) {
  static int last = -2;
  if (pct == last && !msg) return;
  last = pct;
  if (pct < 0) {
    setBacklight(BRI_LEVELS[briIdx]);
    lcd.fillScreen(ui::BG);
    lcd.setFont(&fonts::Font4);
    lcd.setTextColor(ui::TEXT);
    lcd.setTextDatum(lgfx::middle_center);
    lcd.drawString("Aktualizacja firmware", W / 2, 90);
    lcd.setTextDatum(lgfx::top_left);
    return;
  }
  uiBar(lcd, 40, 120, W - 80, 16, pct / 100.0f, ui::BLUE);
  if (msg) {
    lcd.fillRect(0, 150, W, 30, ui::BG);
    lcd.setFont(&fonts::Font4);
    lcd.setTextColor(otaOk ? ui::GREEN : ui::RED);
    lcd.setTextDatum(lgfx::middle_center);
    lcd.drawString(msg, W / 2, 165);
    lcd.setTextDatum(lgfx::top_left);
  }
}

static void handleUpdateUpload() {
  HTTPUpload& u = web.upload();
  if (u.status == UPLOAD_FILE_START) {
    Serial.printf("[ota] start: %s\n", u.filename.c_str());
    otaOk = false;
    otaErr = nullptr;
    otaScreen(-1, nullptr);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    // obraz aplikacji ESP32 zaczyna się bajtem 0xE9; -full.bin (od 0x0) ma tam 0xFF
    if (u.totalSize == 0 && u.currentSize > 0 && u.buf[0] != 0xE9) {
      otaErr = "to nie jest plik aplikacji - uzyj gpu_cyd-<v>-update.bin (nie -full.bin)";
      Update.abort();
    }
    if (otaErr) return;
    if (Update.isRunning() && Update.write(u.buf, u.currentSize) != u.currentSize) Update.printError(Serial);
    // rozmiar pliku nieznany z góry — postęp względem wolnej partycji OTA (~1,3 MB)
    size_t total = u.totalSize + u.currentSize;
    otaScreen(min(99, (int)(total * 100 / 1310720)), nullptr);
  } else if (u.status == UPLOAD_FILE_END) {
    otaOk = !otaErr && Update.end(true);
    if (!otaOk) Update.printError(Serial);
    Serial.printf("[ota] koniec: %u B, ok=%d\n", (unsigned)u.totalSize, (int)otaOk);
    otaScreen(100, otaOk ? "OK - restart" : "Blad aktualizacji");
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaOk = false;
    otaScreen(0, "Przerwano");
  }
}

static void handleUpdateDone() {
  if (otaOk) {
    web.send(200, "text/plain", "ok");
    restartAt = millis() + 1000;
  } else {
    web.send(500, "text/plain", otaErr ? otaErr : Update.hasError() ? Update.errorString() : "blad");
    tLastDraw = 0;                       // wracamy do dashboardu
    restartAt = millis() + 2500;         // ekran OTA zostawił ślady — czysty start
  }
}

// Start po połączeniu z Wi-Fi (wywoływać po każdym connectFlow — idempotentne).
void webBegin() {
  static bool started = false;
  MDNS.end();
  if (MDNS.begin(WEB_HOSTNAME)) MDNS.addService("http", "tcp", 80);
  if (started) return;
  started = true;
  web.on("/", HTTP_GET, [] { web.send_P(200, "text/html; charset=utf-8", WEB_PAGE); });
  web.on("/api/status", HTTP_GET, handleStatus);
  web.on("/api/settings", HTTP_GET, handleGetSettings);
  web.on("/api/settings", HTTP_POST, handlePostSettings);
  web.on("/api/scan", HTTP_GET, handleScan);
  web.on("/api/wifi", HTTP_POST, handleWifi);
  web.on("/api/restart", HTTP_POST, handleRestart);
  web.on("/api/calibrate", HTTP_POST, handleCalibrate);
  web.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  web.onNotFound([] { web.send(404, "text/plain", "nie ma"); });
  web.begin();
  Serial.printf("[web] http://%s.local  http://%s\n", WEB_HOSTNAME, WiFi.localIP().toString().c_str());
}

void webLoop() {
  web.handleClient();
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) {
    Serial.println("[web] restart");
    delay(50);
    ESP.restart();
  }
}
