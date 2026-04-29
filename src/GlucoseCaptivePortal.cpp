#include "GlucoseCaptivePortal.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// ─── Page HTML ────────────────────────────────────────────────────────────────
static const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="fr"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Glucose Clock</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0a0a0a;color:#eee;font-family:system-ui,sans-serif;
     display:flex;align-items:center;justify-content:center;min-height:100vh;padding:16px}
.card{background:#111;border:1px solid #1e1e1e;border-radius:14px;
      padding:28px 24px;max-width:400px;width:100%}
h1{font-size:19px;color:#fff;margin-bottom:4px}
.sub{color:#444;font-size:13px;margin-bottom:24px}
/* Choix mode */
.modes{display:flex;gap:10px;margin-bottom:24px}
.mode-btn{flex:1;padding:16px 8px;border:1px solid #222;border-radius:10px;
          background:#0e0e0e;color:#555;font-size:13px;cursor:pointer;text-align:center;
          transition:all .15s}
.mode-btn.active{border-color:#00DD44;color:#00DD44;background:#001a08}
.mode-btn span{display:block;font-size:22px;margin-bottom:6px}
/* Formulaires */
.form{display:none}.form.visible{display:block}
label{display:block;color:#555;font-size:11px;letter-spacing:.06em;
      text-transform:uppercase;margin-bottom:5px;margin-top:14px}
input{width:100%;background:#1a1a1a;border:1px solid #2a2a2a;border-radius:8px;
      color:#fff;font-size:15px;padding:10px 12px;outline:none}
input:focus{border-color:#00DD44}
input.mono{letter-spacing:.12em;text-transform:uppercase;font-size:17px}
.hint{color:#333;font-size:11px;margin-top:5px;line-height:1.5}
button[type=submit]{width:100%;margin-top:22px;background:#00DD44;color:#000;
                    border:none;border-radius:8px;font-size:16px;
                    font-weight:700;padding:13px;cursor:pointer}
button[type=submit]:active{background:#00b838}
.err{color:#FF3B30;font-size:12px;margin-top:8px;display:none}
</style></head><body>
<div class="card">
  <h1>🩸 Glucose Clock</h1>
  <p class="sub">Choisissez votre mode de fonctionnement</p>

  <div class="modes">
    <div class="mode-btn" id="btn-ble" onclick="pick('ble')">
      <span>📡</span>Bluetooth<br><small style="color:#333;font-size:10px">Direct G6</small>
    </div>
    <div class="mode-btn" id="btn-wifi" onclick="pick('wifi')">
      <span>🌐</span>Nightscout<br><small style="color:#333;font-size:10px">Via Wi-Fi</small>
    </div>
  </div>

  <!-- ── Formulaire BLE ── -->
  <form id="form-ble" class="form" method="POST" action="/save-ble"
        onsubmit="return validateBle()">
    <label>Serial du transmetteur G6</label>
    <input class="mono" type="text" name="serial" id="s-ble"
           maxlength="6" placeholder="8G1234"
           autocomplete="off" autocorrect="off" spellcheck="false">
    <p class="hint">Inscrit sur le transmetteur blanc collé sur votre peau.</p>
    <p class="err" id="err-ble">6 caractères alphanumériques requis.</p>
    <button type="submit">Configurer en Bluetooth</button>
  </form>

  <!-- ── Formulaire Wi-Fi / Nightscout ── -->
  <form id="form-wifi" class="form" method="POST" action="/save-wifi"
        onsubmit="return validateWifi()">
    <label>Nom Wi-Fi (SSID)</label>
    <input type="text" name="ssid" id="s-ssid" placeholder="MonReseau" autocomplete="off">

    <label>Mot de passe Wi-Fi</label>
    <input type="password" name="password" id="s-pwd" placeholder="••••••••">

    <label>URL Nightscout</label>
    <input type="url" name="ns_url" id="s-url"
           placeholder="https://monsite.fly.dev">
    <p class="hint">Sans slash final. Ex : https://monsite.nightscout.io</p>

    <label>Clé API Nightscout <small style="text-transform:none">(optionnel)</small></label>
    <input type="text" name="api_key" placeholder="myAPIkey">

    <p class="err" id="err-wifi">SSID et URL Nightscout requis.</p>
    <button type="submit">Configurer en Nightscout</button>
  </form>
</div>

<script>
function pick(mode){
  ['ble','wifi'].forEach(function(m){
    document.getElementById('btn-'+m).classList.toggle('active', m===mode);
    document.getElementById('form-'+m).classList.toggle('visible', m===mode);
  });
}
function validateBle(){
  var v=document.getElementById('s-ble').value.trim().toUpperCase();
  if(!/^[A-Z0-9]{6}$/.test(v)){
    document.getElementById('err-ble').style.display='block'; return false;
  }
  document.getElementById('s-ble').value=v; return true;
}
function validateWifi(){
  var ssid=document.getElementById('s-ssid').value.trim();
  var url =document.getElementById('s-url').value.trim();
  if(!ssid||!url){
    document.getElementById('err-wifi').style.display='block'; return false;
  }
  return true;
}
</script>
</body></html>
)HTML";

static const char PAGE_OK[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>body{background:#0a0a0a;color:#eee;font-family:system-ui;
display:flex;align-items:center;justify-content:center;min-height:100vh;text-align:center}
h2{color:#00DD44;margin-bottom:10px}p{color:#444;font-size:14px}</style></head>
<body><div><h2>✅ Configuration sauvegardée</h2><p>Redémarrage en cours...</p></div></body></html>
)HTML";

// ─── start() ─────────────────────────────────────────────────────────────────
void GlucoseCaptivePortal::start() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char apName[24];
    snprintf(apName, sizeof(apName), "GlucoseClock-%02X%02X", mac[4], mac[5]);

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
    WiFi.softAP(apName);

    DNSServer dns;
    dns.start(53, "*", IPAddress(192,168,4,1));

    WebServer server(80);

    // Redirect captive portal
    server.onNotFound([&]() {
        server.sendHeader("Location", "http://192.168.4.1/");
        server.send(302);
    });

    server.on("/", HTTP_GET, [&]() {
        server.send_P(200, "text/html", PAGE);
    });

    // ── Sauvegarde mode BLE ──────────────────────────────────────────────────
    server.on("/save-ble", HTTP_POST, [&]() {
        String serial = server.arg("serial");
        serial.trim(); serial.toUpperCase();
        bool ok = (serial.length() == 6);
        if (ok) for (char c : serial) if (!isAlphaNumeric(c)) { ok=false; break; }

        if (!ok) { server.sendHeader("Location","/"); server.send(302); return; }

        Preferences prefs;
        prefs.begin("glucose", false);
        prefs.putString("mode", "ble");
        prefs.putString("transmitter_id", serial);
        prefs.end();

        server.send_P(200, "text/html", PAGE_OK);
        delay(3000);
        WiFi.mode(WIFI_OFF);
        ESP.restart();
    });

    // ── Sauvegarde mode Wi-Fi / Nightscout ───────────────────────────────────
    server.on("/save-wifi", HTTP_POST, [&]() {
        String ssid    = server.arg("ssid");
        String pwd     = server.arg("password");
        String ns_url  = server.arg("ns_url");
        String api_key = server.arg("api_key");

        ssid.trim(); ns_url.trim();
        if (ssid.isEmpty() || ns_url.isEmpty()) {
            server.sendHeader("Location","/"); server.send(302); return;
        }

        Preferences prefs;
        prefs.begin("glucose", false);
        prefs.putString("mode",    "wifi");
        prefs.putString("ssid",    ssid);
        prefs.putString("pwd",     pwd);
        prefs.putString("ns_url",  ns_url);
        prefs.putString("api_key", api_key);
        prefs.end();

        server.send_P(200, "text/html", PAGE_OK);
        delay(3000);
        WiFi.mode(WIFI_OFF);
        ESP.restart();
    });

    server.begin();

    while (true) {
        dns.processNextRequest();
        server.handleClient();
        delay(10);
    }
}
