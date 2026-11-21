// Panel WWW z ustawieniami CYD: http://gpu-cyd.local (lub http://<IP CYD>).
//
// Endpointy:
//   GET  /               strona (HTML+JS, poniżej)
//   GET  /api/status     stan: IP, RSSI, Mac, czas, uptime, wersja
//   GET  /api/settings   ustawienia (bez hasła Wi-Fi)
//   POST /api/settings   JSON {bri, rot (0/90/180/270), night:{on, from, to, wake}} — dowolny podzbiór
//   GET  /api/scan       lista sieci Wi-Fi
//   POST /api/wifi       JSON {ssid, pass} → zapis + restart (pusty pass = bez zmiany przy tym samym SSID)
//   POST /api/restart    restart
//   POST /api/calibrate  kasuje kalibrację dotyku → restart → kalibracja na ekranie
//   POST /api/login/start, /api/login, /api/logout {all}   logowanie (patrz niżej)
//   GET  /api/ota/check  najnowsze wydanie z GitHub Releases (OTA_REPO) vs FW_VERSION
//   POST /api/ota/install {force}  pobiera gpu_cyd-<v>-update.bin z wydania, sprawdza SHA-256
//                        z SHA256SUMS.txt, instaluje i restartuje (HTTPS z wbudowanym zestawem CA)
//   POST /api/update     upload pliku .bin (gpu_cyd-<v>-update.bin) → OTA → restart
//
// Logowanie kodem z ekranu: POST /api/login/start (albo ukryty gest na CYD: przytrzymanie
// nazwy hosta w nagłówku 1,5 s) → CYD pokazuje przez 60 s kod QR + 6 cyfr →
// telefon skanuje QR (GET /login?c=KOD) albo wpisujesz cyfry (POST /api/login {code})
// → token w cookie (HttpOnly, rok). Do 4 zapamiętanych przeglądarek
// (NVS "toks"). Bez tokenu każde /api/* (poza logowaniem) → 401; OTA sprawdza token,
// zanim cokolwiek zapisze do flasha. Nowy kod najwyżej raz na 60 s, 5 prób na kod.
// Plik dołączany w gpu_cyd.ino PO definicjach globali i funkcji apply* (używa ich wprost).
#pragma once
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <mbedtls/sha256.h>
#include <esp_ota_ops.h>

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
.row label:not(.sw){flex:1}
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
.sw{position:relative;display:inline-block;width:46px;height:26px;flex:0 0 46px}
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
.seg button.on{background:var(--acc);color:#fff}
.code{font:600 28px/1 ui-monospace,SFMono-Regular,Menlo,monospace;letter-spacing:.35em;text-align:center;width:100%;padding:12px}
.center{text-align:center}
</style></head><body><main>
<h1>GPU CYD</h1>
<div class="sub" id="sub">Ustawienia ekranu</div>

<div class="card" id="login" hidden><h2>Logowanie</h2>
 <div class="stack">
  <p class="hint" style="margin:0 0 4px">Panel jest chroniony. Kod (QR + 6 cyfr) pojawi się na ekranie CYD na 60 sekund —
  zeskanuj QR telefonem albo wpisz cyfry tutaj.</p>
  <button class="pri" id="lStart">Pokaż kod na ekranie CYD</button>
  <div class="stack" id="lForm" hidden>
   <input class="code" id="lCode" inputmode="numeric" autocomplete="one-time-code" maxlength="6" placeholder="000000">
   <button class="pri" id="lGo">Zaloguj</button>
   <div class="hint center" id="lTtl"></div>
  </div>
 </div>
</div>
<div id="app" hidden>
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
 <div class="row" style="margin-top:8px"><span>Obrót</span></div>
 <div class="seg" id="rot"><button data-r="0">0°</button><button data-r="90">90°</button><button data-r="180">180°</button><button data-r="270">270°</button></div>
 <p class="hint">90° i 270° — dashboard w pionie. Konfiguracja Wi-Fi i kod logowania zostają w poziomie.</p>
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
 <div class="row"><span class="k">Najnowsze wydanie</span><span class="v" id="otaV">-</span></div>
 <div class="actions" style="margin-top:8px">
  <button id="otaChk">Sprawdź aktualizacje</button>
  <button class="pri" id="otaGo" hidden>Zainstaluj</button>
 </div>
 <p class="hint" id="otaMsg">CYD pobiera wydanie z GitHuba (HTTPS) i przed instalacją sprawdza sumę SHA-256. Ustawienia zostają.</p>
 <p class="hint">Ręcznie: plik <code>gpu_cyd-&lt;wersja&gt;-update.bin</code> z GitHub Releases (wyżej).</p>
</div>

<div class="card"><h2>Urządzenie</h2>
 <div class="actions">
  <button id="restart">Uruchom ponownie</button>
  <button class="danger" id="calib">Kalibracja dotyku</button>
 </div>
 <div class="actions" style="margin-top:8px">
  <button id="logout">Wyloguj</button>
  <button id="logoutAll">Wyloguj wszystkie przeglądarki</button>
 </div>
</div>
</div>
</main><div id="toast"></div>
<script>
const $=id=>document.getElementById(id);
const BRI=[20,50,90,135,180];
let saveT=null;
function toast(t){const e=$('toast');e.textContent=t;e.classList.add('on');clearTimeout(e._t);e._t=setTimeout(()=>e.classList.remove('on'),1800)}
let authed=false,ttlT=null,rot=0;
function setRot(r){rot=r;document.querySelectorAll('#rot button').forEach(b=>b.classList.toggle('on',+b.dataset.r==r))}
document.querySelectorAll('#rot button').forEach(b=>b.onclick=()=>{setRot(+b.dataset.r);save()});
async function api(p,body){const r=await fetch(p,body===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
 if(r.status==401&&!p.startsWith('/api/login')){showLogin();throw new Error('zaloguj się')}
 if(!r.ok)throw new Error(await r.text()||r.status);return r.headers.get('content-type')?.includes('json')?r.json():r.text()}
function hm(m){return String(Math.floor(m/60)).padStart(2,'0')+':'+String(m%60).padStart(2,'0')}
function dur(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+' d ':'')+(d||h?h+' h ':'')+m+' min'}
function briLabel(){$('briV').textContent=$('bri').value+'/5'}
async function load(){const s=await api('/api/settings');
 $('bri').value=s.bri+1;briLabel();setRot(s.rot);
 $('non').checked=s.night.on;$('nfr').value=s.night.from;$('nto').value=s.night.to;$('nwk').value=s.night.wake;
 $('ssid').value=s.ssid}
function showLogin(){authed=false;$('app').hidden=true;$('login').hidden=false;$('sub').textContent='Zaloguj się kodem z ekranu CYD'}
async function showApp(){await load();authed=true;$('login').hidden=true;$('app').hidden=false;status()}
function countdown(ttl){clearInterval(ttlT);let t=ttl;const f=()=>{$('lTtl').textContent=t>0?'Kod ważny jeszcze '+t+' s':'Kod wygasł — poproś o nowy';
 $('lStart').disabled=t>0;if(t--<=0)clearInterval(ttlT)};f();ttlT=setInterval(f,1000)}
$('lStart').onclick=async()=>{try{const r=await api('/api/login/start',{});$('lForm').hidden=false;$('lCode').value='';$('lCode').focus();countdown(r.ttl)}
 catch(e){toast(e.message)}};
async function doLogin(){const code=$('lCode').value.trim();if(code.length!=6)return toast('Kod ma 6 cyfr');
 try{await api('/api/login',{code});clearInterval(ttlT);$('lForm').hidden=true;$('lStart').disabled=false;toast('Zalogowano');showApp()}
 catch(e){toast(e.message);$('lCode').select()}}
$('lGo').onclick=doLogin;
$('lCode').oninput=e=>{e.target.value=e.target.value.replace(/\D/g,'').slice(0,6);if(e.target.value.length==6)doLogin()};
$('logout').onclick=async()=>{await api('/api/logout',{}).catch(()=>{});showLogin()};
$('logoutAll').onclick=async()=>{if(!confirm('Wylogować wszystkie przeglądarki (też tę)?'))return;await api('/api/logout',{all:true}).catch(()=>{});showLogin()};
async function status(){if(!authed)return;try{const s=await api('/api/status');
 $('sub').textContent=s.ip+' · '+s.hostname+'.local';
 $('sMac').innerHTML=s.mac.ok?'<span class="dot ok"></span>'+(s.mac.host||'')+' ('+s.mac.ip+')'
   :'<span class="dot bad"></span>'+(s.mac.ip?'brak danych od '+s.mac.age+' s':'nie znaleziono');
 $('sTime').textContent=s.time||'-';
 $('sWifi').textContent=s.ssid+' · '+s.rssi+' dBm';
 $('sNight').textContent=s.night?(s.awake?'noc (wybudzony)':'noc (wygaszony)'):'dzień';
 $('sUp').textContent=dur(s.uptime);$('sFw').textContent=s.fw}catch(e){if(authed)$('sub').textContent='Brak połączenia z CYD'}}
function collect(){return{bri:$('bri').value-1,rot,
 night:{on:$('non').checked,from:$('nfr').value,to:$('nto').value,wake:+$('nwk').value}}}
function save(){clearTimeout(saveT);saveT=setTimeout(async()=>{try{await api('/api/settings',collect());toast('Zapisano');status()}catch(e){toast('Błąd: '+e.message)}},250)}
$('bri').oninput=()=>{briLabel();save()};
['non','nfr','nto','nwk'].forEach(i=>$(i).onchange=save);
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
 x.onload=()=>{$('fwUp').disabled=false;if(x.status==401)return showLogin();toast(x.status==200?'Wgrano — restart':'Błąd: '+x.responseText);if(x.status==200)setTimeout(()=>location.reload(),9000)};
 x.onerror=()=>{$('fwUp').disabled=false;toast('Błąd wysyłania')};
 x.open('POST','/api/update');x.send(fd)};
{const e=new URLSearchParams(location.search).get('e');if(e){history.replaceState(null,'','/');
 setTimeout(()=>toast(e=='400'?'Kod wygasł — poproś o nowy':e=='429'?'Za dużo prób':'Zły kod'),300)}}
let otaForce=false;
$('otaChk').onclick=async()=>{const b=$('otaChk');b.disabled=true;$('otaMsg').textContent='Sprawdzam GitHub…';
 try{const r=await api('/api/ota/check');
  $('otaV').textContent=r.latest+(r.newer?'  (nowsze)':'  (zainstalowane: '+r.current+')');
  otaForce=!r.newer;$('otaGo').hidden=false;$('otaGo').textContent=r.newer?'Zainstaluj '+r.latest:'Zainstaluj ponownie';
  $('otaGo').className=r.newer?'pri':'';
  $('otaMsg').textContent=r.newer?'Dostępna wersja '+r.latest+' (masz '+r.current+').':'Masz najnowszą wersję.'}
 catch(e){$('otaMsg').textContent='Błąd: '+e.message}b.disabled=false};
$('otaGo').onclick=async()=>{const v=$('otaV').textContent.split(' ')[0];
 if(!confirm((otaForce?'Zainstalować ponownie ':'Zainstalować ')+v+'? CYD pobierze plik z GitHuba i uruchomi się ponownie.'))return;
 $('otaGo').disabled=$('otaChk').disabled=true;$('otaMsg').textContent='Pobieranie i instalacja… (ok. 30 s, nie wyłączaj CYD)';
 try{await api('/api/ota/install',{force:otaForce});$('otaMsg').textContent='Zainstalowano — restart…';setTimeout(()=>location.reload(),12000)}
 catch(e){$('otaMsg').textContent='Błąd: '+e.message;$('otaGo').disabled=$('otaChk').disabled=false}};
showApp().catch(e=>{if($('login').hidden)$('sub').textContent='Brak połączenia z CYD'});setInterval(status,3000);
</script></body></html>)HTML";

// ---- Logowanie: kod z ekranu CYD → token w cookie
static void sendJson(JsonDocument& d, int code = 200);
const uint8_t TOK_MAX = 4;
String tokens[TOK_MAX];                // zapamiętane przeglądarki (najnowsza pierwsza)
uint32_t loginCode = 0;                // != 0 → kod pokazany na ekranie
uint32_t loginUntil = 0;               // ważność kodu; zarazem blokada nowego kodu
uint8_t  loginTries = 0;
bool     loginShown = false;
const uint32_t LOGIN_TTL_MS = 60000;
const uint8_t  LOGIN_MAX_TRIES = 5;

static bool loginActive(uint32_t now) { return loginCode && (int32_t)(loginUntil - now) > 0; }

static void loadTokens() {
  String s = prefs.getString("toks", "");
  for (uint8_t i = 0; i < TOK_MAX && s.length(); i++) {
    int c = s.indexOf(',');
    tokens[i] = c < 0 ? s : s.substring(0, c);
    s = c < 0 ? String() : s.substring(c + 1);
  }
}

static void saveTokens() {
  String s;
  for (auto& t : tokens) if (t.length()) { if (s.length()) s += ','; s += t; }
  prefs.putString("toks", s);
}

static String cookieToken() {
  String c = web.header("Cookie");
  int i = c.indexOf("cydtok=");
  if (i < 0) return "";
  i += 7;
  int e = c.indexOf(';', i);
  return c.substring(i, e < 0 ? c.length() : e);
}

// Porównanie bez wczesnego wyjścia (czas nie zdradza, ile znaków się zgadza).
static bool tokEq(const String& a, const String& b) {
  if (a.length() != b.length()) return false;
  uint8_t d = 0;
  for (size_t i = 0; i < a.length(); i++) d |= a[i] ^ b[i];
  return d == 0;
}

static int tokenIndex(const String& t) {
  if (t.length() != 32) return -1;
  for (int i = 0; i < TOK_MAX; i++) if (tokEq(tokens[i], t)) return i;
  return -1;
}

static bool authed() { return tokenIndex(cookieToken()) >= 0; }

static bool requireAuth() {
  if (authed()) return true;
  web.send(401, "text/plain", "zaloguj sie");
  return false;
}

// Nowy kod na ekranie. Z sieci: najwyżej raz na okno 60 s (spalony kod blokuje do końca okna);
// gest na ekranie (physical) — zawsze, bo kto dotyka CYD, ten i tak widzi kod.
static bool startLogin(bool physical) {
  uint32_t now = millis();
  if (loginActive(now) && !physical) return true;          // ten sam kod
  if (!physical && (int32_t)(loginUntil - now) > 0) return false;
  loginCode = 100000 + esp_random() % 900000;
  loginUntil = now + LOGIN_TTL_MS;
  loginTries = 0;
  loginShown = false;
  Serial.printf("[web] kod logowania na ekranie (%s)\n", physical ? "gest" : "przegladarka");
  return true;
}

static void handleLoginStart() {
  if (!startLogin(false)) { web.send(429, "text/plain", "za duzo prob - sprobuj za chwile"); return; }
  JsonDocument d;
  d["ttl"] = (loginUntil - millis()) / 1000;
  sendJson(d);
}

// Sprawdza kod; przy sukcesie dopisuje token i ustawia cookie. Zwraca kod HTTP (200 = OK).
static int checkCode(const char* code, const char** err) {
  uint32_t now = millis();
  if (!loginActive(now)) { *err = "kod wygasl - popros o nowy"; return 400; }
  if (!code || strlen(code) != 6 || strtoul(code, nullptr, 10) != loginCode) {
    if (++loginTries >= LOGIN_MAX_TRIES) {
      loginCode = 0;                         // ekran gaśnie, nowy kod z sieci dopiero po loginUntil
      *err = "za duzo prob - kod uniewazniony";
      return 429;
    }
    *err = "zly kod";
    return 403;
  }
  char t[33];
  for (int i = 0; i < 4; i++) snprintf(t + i * 8, 9, "%08lx", (unsigned long)esp_random());
  for (int i = TOK_MAX - 1; i > 0; i--) tokens[i] = tokens[i - 1];   // najstarszy wypada
  tokens[0] = t;
  saveTokens();
  loginCode = 0;
  loginUntil = now;                        // od razu można poprosić o kolejny kod
  Serial.println("[web] zalogowano przegladarke");
  web.sendHeader("Set-Cookie", String("cydtok=") + t + "; Max-Age=31536000; Path=/; HttpOnly; SameSite=Lax");
  *err = "ok";
  return 200;
}

static void handleLogin() {
  JsonDocument d;
  String body = web.arg("plain");
  if (deserializeJson(d, body) || !d["code"].is<const char*>()) { web.send(400, "text/plain", "brak kodu"); return; }
  const char* err;
  int code = checkCode(d["code"], &err);
  web.send(code, "text/plain", err);
}

// Link z kodu QR: loguje i przekierowuje na panel (przy błędzie panel pokaże logowanie + powód).
static void handleLoginLink() {
  const char* err;
  String c = web.arg("c");
  int code = checkCode(c.c_str(), &err);
  web.sendHeader("Location", code == 200 ? "/" : String("/?e=") + code);
  web.send(302, "text/plain", err);
}

static void handleLogout() {
  if (!requireAuth()) return;
  JsonDocument d;
  String body = web.arg("plain");
  deserializeJson(d, body);
  if (d["all"] | false) for (auto& t : tokens) t = "";
  else tokens[tokenIndex(cookieToken())] = "";
  saveTokens();
  web.sendHeader("Set-Cookie", "cydtok=; Max-Age=0; Path=/; HttpOnly; SameSite=Lax");
  web.send(200, "text/plain", "ok");
}

// Ekran logowania na CYD: QR (link z kodem) po lewej, 6 cyfr po prawej, pasek ważności.
// true = zajmuje ekran (dashboard wtedy nie rysuje). Dotknięcie ekranu anuluje kod.
bool loginTick(uint32_t now) {
  static int lastSec = -1;
  if (!loginActive(now)) {
    if (loginShown) { loginShown = false; loginCode = 0; tLastDraw = 0; }
    return false;
  }
  setBacklight(BRI_LEVELS[briIdx]);         // też w nocy
  const int QX = 8, QY = 30, QS = 176;      // biały kwadrat z kodem QR
  const int RX = QX + QS + 8, RW = W - RX - 8, RC = RX + RW / 2;   // prawa kolumna
  if (!loginShown) {
    setOrient(false);                        // ekran kodu projektowany na 320x240
    loginShown = true;
    lastSec = -1;
    lcd.fillScreen(ui::BG);
    lcd.setTextDatum(lgfx::middle_center);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(ui::DIM);
    lcd.drawString("Logowanie do panelu WWW", W / 2, 14);
    char url[64];
    snprintf(url, sizeof(url), "http://%s/login?c=%06lu", WiFi.localIP().toString().c_str(),
             (unsigned long)loginCode);
    lcd.fillRoundRect(QX, QY, QS, QS, 8, TFT_WHITE);
    lcd.qrcode(url, QX + 14, QY + 14, QS - 28, 1, false);   // ~3 moduły marginesu z białej ramki
    char a[4], b[4];
    snprintf(a, sizeof(a), "%03lu", (unsigned long)(loginCode / 1000));
    snprintf(b, sizeof(b), "%03lu", (unsigned long)(loginCode % 1000));
    lcd.setFont(&fonts::Font7);
    lcd.setTextColor(ui::TEXT);
    lcd.drawString(a, RC, 70);
    lcd.drawString(b, RC, 128);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(ui::DIM);
    lcd.drawString("Zeskanuj telefonem", W / 2, 218 - 2);
    lcd.drawString("lub wpisz kod  -  dotknij = anuluj", W / 2, 232);
    lcd.setTextDatum(lgfx::top_left);
  }
  int sec = ((loginUntil - now) + 999) / 1000;
  if (sec != lastSec) {
    lastSec = sec;
    uiBar(lcd, RX, 166, RW, 10, sec / 60.0f, ui::BLUE);
    char t[8];
    snprintf(t, sizeof(t), "%d s", sec);
    lcd.fillRect(RX, 180, RW, 20, ui::BG);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(ui::DIM);
    lcd.setTextDatum(lgfx::middle_center);
    lcd.drawString(t, RC, 190);
    lcd.setTextDatum(lgfx::top_left);
  }
  int x, y;
  if (touchXY(lcd, tcal, &x, &y)) {
    waitRelease(lcd, tcal);
    loginCode = 0;                           // blokada nowego kodu z sieci zostaje do loginUntil
    Serial.println("[web] kod anulowany dotykiem");
  }
  return true;
}

static void sendJson(JsonDocument& d, int code) {
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
  if (!requireAuth()) return;
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
  if (!requireAuth()) return;
  JsonDocument d;
  d["bri"] = briIdx;
  d["rot"] = rotQ * 90;
  d["ssid"] = cfgSSID;              // hasła nie zwracamy
  JsonObject n = d["night"].to<JsonObject>();
  n["on"] = nightOn;
  n["from"] = fmtHM(nightFrom);
  n["to"] = fmtHM(nightTo);
  n["wake"] = nightWakeS;
  sendJson(d);
}

static void handlePostSettings() {
  if (!requireAuth()) return;
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
  int q = -1;
  if (d["rot"].is<int>()) {
    int r = d["rot"];
    if (r % 90) { web.send(400, "text/plain", "rot: 0, 90, 180 lub 270"); return; }
    q = ((r / 90) % 4 + 4) % 4;
  } else if (d["rot180"].is<bool>()) {       // zgodność z API v1.1
    q = d["rot180"] ? 2 : 0;
  }
  if (q >= 0 && q != rotQ) applyRot(q, true);
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
  if (!requireAuth()) return;
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
  if (!requireAuth()) return;
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
  if (!requireAuth()) return;
  web.send(200, "text/plain", "ok");
  restartAt = millis() + 500;
}

static void handleCalibrate() {
  if (!requireAuth()) return;
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
    setOrient(false);
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
    otaOk = false;
    otaErr = nullptr;
    if (!authed()) { otaErr = "zaloguj sie"; return; }   // nic nie zapisujemy bez tokenu
    Serial.printf("[ota] start: %s\n", u.filename.c_str());
    otaScreen(-1, nullptr);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (otaErr) return;
    // obraz aplikacji ESP32 zaczyna się bajtem 0xE9; -full.bin (od 0x0) ma tam 0xFF
    if (u.totalSize == 0 && u.currentSize > 0 && u.buf[0] != 0xE9) {
      otaErr = "to nie jest plik aplikacji - uzyj gpu_cyd-<v>-update.bin (nie -full.bin)";
      Update.abort();
    }
    if (otaErr) return;
    if (Update.isRunning() && Update.write(u.buf, u.currentSize) != u.currentSize) Update.printError(Serial);
    // rozmiar pliku nieznany z góry — postęp względem rozmiaru partycji OTA
    size_t total = u.totalSize + u.currentSize;
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    size_t cap = next ? next->size : 1966080;
    otaScreen(min(99, (int)(total * 100 / cap)), nullptr);
  } else if (u.status == UPLOAD_FILE_END) {
    if (otaErr) return;
    otaOk = Update.end(true);
    if (!otaOk) Update.printError(Serial);
    Serial.printf("[ota] koniec: %u B, ok=%d\n", (unsigned)u.totalSize, (int)otaOk);
    otaScreen(100, otaOk ? "OK - restart" : "Blad aktualizacji");
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    if (otaErr) return;
    Update.abort();
    otaOk = false;
    otaScreen(0, "Przerwano");
  }
}

static void handleUpdateDone() {
  if (!authed()) { web.send(401, "text/plain", "zaloguj sie"); return; }
  if (otaOk) {
    web.send(200, "text/plain", "ok");
    restartAt = millis() + 1000;
  } else {
    web.send(500, "text/plain", otaErr ? otaErr : Update.hasError() ? Update.errorString() : "blad");
    tLastDraw = 0;                       // wracamy do dashboardu
    restartAt = millis() + 2500;         // ekran OTA zostawił ślady — czysty start
  }
}

// ---- Aktualizacja z GitHub Releases
String otaTag, otaBinUrl, otaSumsUrl;   // wynik ostatniego sprawdzenia

static void verParse(const char* v, int out[3]) {
  out[0] = out[1] = out[2] = 0;
  if (*v == 'v' || *v == 'V') v++;
  sscanf(v, "%d.%d.%d", &out[0], &out[1], &out[2]);
}

static int verCmp(const char* a, const char* b) {
  int x[3], y[3];
  verParse(a, x); verParse(b, y);
  for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}

// GET przez HTTPS z weryfikacją certyfikatu (wbudowany zestaw CA rdzenia) i przekierowaniami
// (pliki wydań: github.com → release-assets.githubusercontent.com).
static bool httpsBegin(HTTPClient& http, NetworkClientSecure& cli, const String& url) {
  cli.useBuiltinCACertBundle();
  cli.setTimeout(15000);            // ms (rdzeń 3.x: Stream::setTimeout — nie sekundy)
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setUserAgent("gpu-cyd/" FW_VERSION);
  http.setTimeout(15000);
  http.useHTTP10(true);             // bez chunked — strumień da się parsować wprost
  return http.begin(cli, url);
}

static void handleOtaCheck() {
  if (!requireAuth()) return;
  NetworkClientSecure cli;
  HTTPClient http;
  if (!httpsBegin(http, cli, "https://api.github.com/repos/" OTA_REPO "/releases/latest")) {
    web.send(502, "text/plain", "nie moge polaczyc sie z GitHubem"); return;
  }
  http.addHeader("Accept", "application/vnd.github+json");
  int code = http.GET();
  if (code != 200) {
    web.send(502, "text/plain", String("GitHub: HTTP ") + code); http.end(); return;
  }
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  JsonDocument rel;
  DeserializationError e = deserializeJson(rel, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (e) { web.send(502, "text/plain", String("GitHub: zly JSON (") + e.c_str() + ")"); return; }

  otaTag = rel["tag_name"] | "";
  String ver = otaTag.startsWith("v") ? otaTag.substring(1) : otaTag;
  String binName = "gpu_cyd-" + ver + "-update.bin";
  otaBinUrl = ""; otaSumsUrl = "";
  for (JsonObject a : rel["assets"].as<JsonArray>()) {
    String n = a["name"] | "";
    if (n == binName) otaBinUrl = a["browser_download_url"] | "";
    if (n == "SHA256SUMS.txt") otaSumsUrl = a["browser_download_url"] | "";
  }
  Serial.printf("[ota] GitHub: %s (bin %d, sumy %d)\n", otaTag.c_str(), !otaBinUrl.isEmpty(), !otaSumsUrl.isEmpty());

  JsonDocument d;
  d["current"] = FW_VERSION;
  d["latest"] = otaTag;
  d["newer"] = verCmp(otaTag.c_str(), FW_VERSION) > 0;
  d["ready"] = !otaBinUrl.isEmpty() && !otaSumsUrl.isEmpty();
  sendJson(d);
}

static void handleOtaInstall() {
  if (!requireAuth()) return;
  JsonDocument b;
  String body = web.arg("plain");
  deserializeJson(b, body);
  bool force = b["force"] | false;
  if (otaTag.isEmpty() || otaBinUrl.isEmpty() || otaSumsUrl.isEmpty()) {
    web.send(400, "text/plain", "najpierw sprawdz aktualizacje"); return;
  }
  if (!force && verCmp(otaTag.c_str(), FW_VERSION) <= 0) {
    web.send(400, "text/plain", "brak nowszej wersji"); return;
  }

  // 1) suma SHA-256 pliku z SHA256SUMS.txt
  String binName = otaBinUrl.substring(otaBinUrl.lastIndexOf('/') + 1);
  String want;
  {
    NetworkClientSecure cli;
    HTTPClient http;
    if (!httpsBegin(http, cli, otaSumsUrl) || http.GET() != 200) {
      http.end(); web.send(502, "text/plain", "nie moge pobrac SHA256SUMS.txt"); return;
    }
    String sums = http.getString();
    http.end();
    int i = sums.indexOf(binName);
    if (i < 66) { web.send(502, "text/plain", "brak sumy dla " + binName); return; }
    int ls = sums.lastIndexOf('\n', i) + 1;
    want = sums.substring(ls, ls + 64);
    want.toLowerCase();
  }

  // 2) pobieranie pliku wprost do partycji OTA + liczenie SHA-256 po drodze
  NetworkClientSecure cli;
  HTTPClient http;
  int code = httpsBegin(http, cli, otaBinUrl) ? http.GET() : -1;
  int len = http.getSize();
  if (code != 200 || len <= 0) {
    http.end(); web.send(502, "text/plain", String("pobieranie: HTTP ") + code); return;
  }
  Serial.printf("[ota] %s: %d B, sha %s\n", binName.c_str(), len, want.c_str());
  otaOk = false;
  otaScreen(-1, nullptr);
  if (!Update.begin(len, U_FLASH)) {
    http.end(); otaScreen(0, "Brak miejsca"); restartAt = millis() + 2500;
    web.send(500, "text/plain", Update.errorString()); return;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  NetworkClient* st = http.getStreamPtr();
  static uint8_t buf[2048];
  int got = 0;
  const char* err = nullptr;
  uint32_t lastData = millis();
  while (got < len) {
    size_t avail = st->available();
    if (!avail) {
      if (!http.connected() || millis() - lastData > 15000) { err = "przerwane pobieranie"; break; }
      delay(2);
      continue;
    }
    int n = st->readBytes(buf, min(avail, sizeof(buf)));
    if (n <= 0) continue;
    lastData = millis();
    if (got == 0 && buf[0] != 0xE9) { err = "to nie jest plik aplikacji"; break; }
    mbedtls_sha256_update(&sha, buf, n);
    if (Update.write(buf, n) != (size_t)n) { err = Update.errorString(); break; }
    got += n;
    otaScreen(got * 100 / len, nullptr);
  }
  http.end();
  uint8_t h[32];
  mbedtls_sha256_finish(&sha, h);
  mbedtls_sha256_free(&sha);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", h[i]);
  if (!err && want != hex) err = "zla suma SHA-256";

  if (err) {
    Update.abort();
    Serial.printf("[ota] blad: %s\n", err);
    otaScreen(0, "Blad aktualizacji");
    restartAt = millis() + 2500;          // czysty start (ekran OTA zostawił ślady)
    web.send(500, "text/plain", err);
    return;
  }
  otaOk = Update.end(true);
  otaScreen(100, otaOk ? "OK - restart" : "Blad aktualizacji");
  Serial.printf("[ota] %s zainstalowane: %d\n", otaTag.c_str(), (int)otaOk);
  web.send(otaOk ? 200 : 500, "text/plain", otaOk ? "ok" : Update.errorString());
  restartAt = millis() + (otaOk ? 1000 : 2500);
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
  web.on("/api/ota/check", HTTP_GET, handleOtaCheck);
  web.on("/api/ota/install", HTTP_POST, handleOtaInstall);
  web.on("/api/login/start", HTTP_POST, handleLoginStart);
  web.on("/api/login", HTTP_POST, handleLogin);
  web.on("/api/logout", HTTP_POST, handleLogout);
  web.on("/login", HTTP_GET, handleLoginLink);
  const char* hdrs[] = {"Cookie"};
  web.collectHeaders(hdrs, 1);
  loadTokens();
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
