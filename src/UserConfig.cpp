#include "UserConfig.h"
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "DisplayManager.h"

// ── Defaults firmware (fallback si NVS vide) ─────────────────────────────────
#ifdef DEV_WIFI_SSID
static const char* DEFAULT_WIFI_SSID = DEV_WIFI_SSID;
#else
static const char* DEFAULT_WIFI_SSID = "";
#endif

#ifdef DEV_WIFI_PASS
static const char* DEFAULT_WIFI_PASS = DEV_WIFI_PASS;
#else
static const char* DEFAULT_WIFI_PASS = "";
#endif

#ifdef DEV_TRANSMITTER_ID
static const char* DEFAULT_TX_ID = DEV_TRANSMITTER_ID;
#else
static const char* DEFAULT_TX_ID = "";
#endif

#ifdef DEV_NTFY_TOPIC
static const char* DEFAULT_NTFY_LOG = DEV_NTFY_TOPIC;
#else
static const char* DEFAULT_NTFY_LOG = "";
#endif

#ifdef DEV_NTFY_OTA_TOPIC
static const char* DEFAULT_NTFY_OTA = DEV_NTFY_OTA_TOPIC;
#else
static const char* DEFAULT_NTFY_OTA = "";
#endif

#ifdef DEV_OTA_BASE_URL
static const char* DEFAULT_OTA_BASE = DEV_OTA_BASE_URL;
#else
static const char* DEFAULT_OTA_BASE = "https://github.com/neajcoffee/glyconnect/releases/latest/download";
#endif

// Couleur RGB565 pour le label SETUP
#define COL_CYAN 0x07FF

UserConfig loadUserConfig() {
    UserConfig c;
    Preferences p;
    p.begin("tc001", true);  // read-only
    c.wifiSsid     = p.getString("ssid",      DEFAULT_WIFI_SSID);
    c.wifiPass     = p.getString("pass",      DEFAULT_WIFI_PASS);
    c.txId         = p.getString("txid",      DEFAULT_TX_ID);
    c.ntfyLogTopic = p.getString("ntfy_log",  DEFAULT_NTFY_LOG);
    c.ntfyOtaTopic = p.getString("ntfy_ota",  DEFAULT_NTFY_OTA);
    c.otaBaseUrl   = p.getString("ota_url",   DEFAULT_OTA_BASE);
    c.decorEnabled = p.getBool("decor_on", false);
    // Bytes du sprite décoration (512 octets si présent, sinon zéros)
    size_t got = p.getBytes("decor_px", c.decorPixels, sizeof(c.decorPixels));
    if (got != sizeof(c.decorPixels)) memset(c.decorPixels, 0, sizeof(c.decorPixels));
    p.end();
    return c;
}

void saveUserConfig(const UserConfig& c) {
    Preferences p;
    p.begin("tc001", false);
    p.putString("ssid",     c.wifiSsid);
    p.putString("pass",     c.wifiPass);
    p.putString("txid",     c.txId);
    p.putString("ntfy_log", c.ntfyLogTopic);
    p.putString("ntfy_ota", c.ntfyOtaTopic);
    p.putString("ota_url",  c.otaBaseUrl);
    p.putBool("decor_on",   c.decorEnabled);
    p.putBytes("decor_px",  c.decorPixels, sizeof(c.decorPixels));
    p.end();
}

void clearUserConfig() {
    Preferences p;
    p.begin("tc001", false);
    p.clear();
    p.end();
}

// ── Captive portal ──────────────────────────────────────────────────────────
static WebServer* setupSrv = nullptr;
static DNSServer* setupDns = nullptr;

static const char* SETUP_HTML PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="fr"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TC001 — Configuration</title>
<style>
  body{font-family:-apple-system,system-ui,sans-serif;max-width:480px;margin:0 auto;padding:24px 16px;background:#0e1116;color:#c9d1d9}
  h1{font-size:20px;color:#58a6ff;margin:0 0 8px}
  p{font-size:13px;opacity:0.7;margin:0 0 24px}
  label{display:block;font-size:13px;margin:14px 0 4px;opacity:0.85}
  input,select{width:100%;padding:10px;font-size:14px;box-sizing:border-box;background:#161b22;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;font-family:inherit}
  input:focus,select:focus{outline:none;border-color:#58a6ff}
  small{display:block;font-size:11px;opacity:0.5;margin-top:3px}
  button{width:100%;padding:14px;background:#2ea043;color:white;border:none;font-size:15px;font-weight:600;margin-top:24px;cursor:pointer;border-radius:6px}
  button:hover{background:#3cb853}
  .req{color:#f85149;margin-left:2px}
  a{color:#58a6ff;text-decoration:none}
</style></head><body>
<h1>📟 TC001 — Configuration initiale</h1>
<p>Premier démarrage. Saisis tes paramètres pour finaliser le setup. Une fois sauvegardés, le device redémarre et boot normalement.</p>
<form method="POST" action="/save">
  <label>Réseau WiFi (SSID)<span class="req">*</span></label>
  <input name="ssid" required placeholder="MonWiFi">

  <label>Mot de passe WiFi</label>
  <input name="pass" type="password" placeholder="(vide si réseau ouvert)">

  <label>Transmitter ID Dexcom G6<span class="req">*</span></label>
  <input name="txid" maxlength="6" required placeholder="803XCD" style="text-transform:uppercase">
  <small>6 caractères alphanumériques imprimés sur le transmetteur ou dans l'app Dexcom.</small>

  <label>ntfy.sh topic — logs distants (optionnel)</label>
  <input name="ntfy_log" placeholder="mon-tc001-log-XYZ">
  <small>Topic ntfy.sh pour suivre les logs du device à distance. Laisser vide pour désactiver.</small>

  <label>ntfy.sh topic — OTA dev (optionnel)</label>
  <input name="ntfy_ota" placeholder="mon-tc001-ota-XYZ">
  <small>Topic ntfy.sh pour pousser des firmwares en dev. Laisser vide en mode prod.</small>

  <button type="submit">💾 Sauvegarder & démarrer</button>
</form>
<p style="margin-top:24px;text-align:center">🎨 <a href="/decor">Personnaliser une décoration in-range (optionnel) →</a></p>
</body></html>)HTML";

// Page éditeur décoration 32×8 avec canvas, palette, presets
static const char* DECOR_HTML PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="fr"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TC001 — Décoration</title>
<style>
  body{font-family:-apple-system,system-ui,sans-serif;max-width:560px;margin:0 auto;padding:20px 12px;background:#0e1116;color:#c9d1d9}
  h1{font-size:20px;color:#58a6ff;margin:0 0 6px}
  p{font-size:13px;opacity:0.7;margin:0 0 16px}
  #canvas{display:grid;grid-template-columns:repeat(32,16px);grid-template-rows:repeat(8,16px);gap:1px;background:#30363d;padding:2px;border-radius:4px;width:fit-content;margin:12px 0;user-select:none}
  #canvas div{background:#0e1116;cursor:pointer;border-radius:1px}
  #canvas div:hover{outline:1px solid #58a6ff}
  .palette{display:flex;flex-wrap:wrap;gap:3px;margin:8px 0}
  .palette div{width:22px;height:22px;border:2px solid #30363d;border-radius:3px;cursor:pointer;flex-shrink:0}
  .palette div.active{border-color:#58a6ff;border-width:3px}
  .ctrls{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:14px 0}
  button,select{padding:8px 14px;background:#161b22;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;font-family:inherit;font-size:13px;cursor:pointer}
  button.primary{background:#2ea043;color:white;border:none;padding:12px 18px;font-weight:600;width:100%;margin-top:18px;font-size:15px}
  button.primary:hover{background:#3cb853}
  label{display:flex;align-items:center;gap:8px;font-size:13px}
  input[type=checkbox]{width:18px;height:18px}
  a{color:#58a6ff;text-decoration:none}
</style></head><body>
<h1>🎨 Décoration 32×8</h1>
<p>Dessine une image qui s'affichera entre les lectures de glycémie quand tu es dans la cible (70-180 mg/dL). Optionnel.</p>

<div class="ctrls">
  <label><input type="checkbox" id="enabled" checked> Activer</label>
  <select id="presets" onchange="loadPreset()">
    <option value="">— Presets —</option>
    <option value="__clear__">⬛ Effacer tout</option>
  </select>
  <button onclick="invertColors()">Inverser</button>
</div>

<div class="palette" id="palette"></div>

<div id="canvas"></div>

<form method="POST" action="/save_decor" id="form">
  <input type="hidden" name="enabled" id="hEnabled" value="1">
  <input type="hidden" name="data" id="hData">
  <button class="primary" type="submit">💾 Sauvegarder</button>
</form>
<p style="margin-top:20px;text-align:center">← <a href="/">Retour à la config</a></p>

<script>
const W=32, H=8;
let pixels = new Array(W*H).fill('#000000');
let currentColor = '#FF0000';

// Palette généreuse : 32 couleurs (8 hues × 4 lightness, + greys)
const PALETTE = [
  '#000000','#404040','#808080','#BFBFBF','#FFFFFF',
  '#FF0000','#FF6666','#CC0000','#660000',
  '#FF8000','#FFB266','#CC6600','#663300',
  '#FFFF00','#FFFF99','#CCCC00','#666600',
  '#80FF00','#B2FF66','#66CC00','#336600',
  '#00FF00','#66FF66','#00CC00','#006600',
  '#00FFFF','#66FFFF','#00CCCC','#006666',
  '#0080FF','#66B2FF','#0066CC','#003366',
  '#0000FF','#6666FF','#0000CC','#000066',
  '#8000FF','#B266FF','#6600CC','#330066',
  '#FF00FF','#FF66FF','#CC00CC','#660066',
  '#FF0080','#FF66B2','#CC0066','#660033'
];

function buildPalette() {
  const p = document.getElementById('palette');
  p.innerHTML = '';
  for (const c of PALETTE) {
    const sw = document.createElement('div');
    sw.style.background = c;
    sw.dataset.color = c;
    sw.onclick = () => {
      currentColor = c;
      [...p.children].forEach(x => x.classList.toggle('active', x.dataset.color===c));
    };
    if (c === currentColor) sw.classList.add('active');
    p.appendChild(sw);
  }
}

function buildCanvas() {
  const c = document.getElementById('canvas');
  c.innerHTML = '';
  for (let i=0; i<W*H; i++) {
    const px = document.createElement('div');
    px.dataset.i = i;
    px.style.background = pixels[i];
    px.addEventListener('mousedown', e => paint(i));
    px.addEventListener('mouseenter', e => { if (e.buttons & 1) paint(i); });
    c.appendChild(px);
  }
}

function paint(i) {
  pixels[i] = pixels[i] === currentColor ? '#000000' : currentColor;
  document.getElementById('canvas').children[i].style.background = pixels[i];
}

function invertColors() {
  pixels = pixels.map(c => c === '#000000' ? currentColor : '#000000');
  buildCanvas();
}

// Convertit hex #RRGGBB → RGB565 16 bits
function hex2rgb565(hex) {
  if (hex === '#000000') return 0;
  const r = parseInt(hex.slice(1,3),16);
  const g = parseInt(hex.slice(3,5),16);
  const b = parseInt(hex.slice(5,7),16);
  return ((r&0xF8)<<8) | ((g&0xFC)<<3) | (b>>3);
}

// Presets injectés depuis le firmware (var FW_PRESETS définie en bas par <script>)
let FW_PRESETS = [];

function rebuildPresetsDropdown() {
  const sel = document.getElementById('presets');
  // Garde seulement la 1re option (placeholder) et "Effacer tout"
  sel.innerHTML = '<option value="">— Presets —</option>';
  for (let i = 0; i < FW_PRESETS.length; i++) {
    const opt = document.createElement('option');
    opt.value = '__fw_' + i;
    opt.textContent = FW_PRESETS[i].name;
    sel.appendChild(opt);
  }
  const clr = document.createElement('option');
  clr.value = '__clear__';
  clr.textContent = '⬛ Effacer tout';
  sel.appendChild(clr);
}

function loadPreset() {
  const v = document.getElementById('presets').value;
  if (!v) return;
  if (v === '__clear__') {
    pixels = new Array(W*H).fill('#000000');
  } else if (v.startsWith('__fw_')) {
    const idx = parseInt(v.substring(5));
    if (FW_PRESETS[idx]) pixels = FW_PRESETS[idx].pixels.slice();
  }
  buildCanvas();
  document.getElementById('presets').value = '';
}

// Soumission : encode tout en 256 valeurs hex 4 chars (RGB565)
document.getElementById('form').addEventListener('submit', e => {
  document.getElementById('hEnabled').value = document.getElementById('enabled').checked ? '1' : '0';
  let data = '';
  for (const hex of pixels) {
    data += hex2rgb565(hex).toString(16).padStart(4,'0');
  }
  document.getElementById('hData').value = data;
});

buildPalette();
buildCanvas();
</script>
</body></html>)HTML";

static void handleRoot() {
    // Pré-remplit les champs avec les valeurs NVS actuelles (mode edit)
    UserConfig c = loadUserConfig();
    String html = SETUP_HTML;
    // Substitution simple des placeholder par injection inline values via JS au load
    // (plus simple que d'utiliser des templates dans le C string)
    String injection =
        "<script>"
        "document.querySelector('[name=ssid]').value = " + String("'") + c.wifiSsid + "';"
        "document.querySelector('[name=pass]').value = '" + c.wifiPass + "';"
        "document.querySelector('[name=txid]').value = '" + c.txId + "';"
        "document.querySelector('[name=ntfy_log]').value = '" + c.ntfyLogTopic + "';"
        "document.querySelector('[name=ntfy_ota]').value = '" + c.ntfyOtaTopic + "';"
        "</script>";
    html.replace("</body>", injection + "</body>");
    setupSrv->send(200, "text/html; charset=utf-8", html);
}

static void handleSave() {
    // Charge la config existante pour préserver le décor pendant qu'on update les autres champs
    UserConfig c = loadUserConfig();
    c.wifiSsid     = setupSrv->arg("ssid");
    c.wifiPass     = setupSrv->arg("pass");
    c.txId         = setupSrv->arg("txid");
    c.txId.toUpperCase();
    c.ntfyLogTopic = setupSrv->arg("ntfy_log");
    c.ntfyOtaTopic = setupSrv->arg("ntfy_ota");
    c.otaBaseUrl   = DEFAULT_OTA_BASE;  // fixé, pas configurable user

    saveUserConfig(c);

    setupSrv->send(200, "text/html; charset=utf-8",
        "<!DOCTYPE html><html><head><meta charset='utf-8'><style>"
        "body{font-family:-apple-system,sans-serif;max-width:480px;margin:0 auto;padding:24px;"
        "background:#0e1116;color:#c9d1d9;text-align:center}"
        "h2{color:#2ea043}</style></head><body>"
        "<h2>✓ Configuration sauvegardée</h2>"
        "<p>Le device redémarre dans 2 secondes...</p></body></html>");
    delay(2000);
    ESP.restart();
}

// Convertit un sprite RGB565 (256 uint16_t en PROGMEM) en JS array de 256 hex strings
static String pixelsToJsArray(const uint16_t* pixels) {
    String s = "[";
    for (int i = 0; i < 256; i++) {
        if (i) s += ",";
        uint16_t v = pgm_read_word(&pixels[i]);
        if (v == 0) { s += "'#000000'"; }
        else {
            int r = ((v >> 11) & 0x1F) << 3;
            int g = ((v >> 5)  & 0x3F) << 2;
            int b = ( v        & 0x1F) << 3;
            char buf[10];
            snprintf(buf, sizeof(buf), "'#%02X%02X%02X'", r, g, b);
            s += buf;
        }
    }
    s += "]";
    return s;
}

// Vrai si tous les pixels du sprite sont à 0
static bool isPresetEmpty(const uint16_t* pixels) {
    for (int i = 0; i < 256; i++) {
        if (pgm_read_word(&pixels[i]) != 0) return false;
    }
    return true;
}

// Forward declarations vers les presets définis dans ble_prod_main.cpp
struct PresetRef { const char* name; const uint16_t* pixels; };
extern const PresetRef PRESETS[];
extern const int N_PRESETS;

static void handleDecor() {
    UserConfig c = loadUserConfig();
    String html = DECOR_HTML;

    // 1. Pré-remplit le canvas avec le décor NVS courant
    String pxJson = pixelsToJsArray(c.decorPixels);

    // 2. Liste des presets actifs (nom non vide ET pixels non vides)
    //    Format : [{name, pixels: [...256 hex...]}, ...]
    String presetsJson = "[";
    bool first = true;
    for (int i = 0; i < N_PRESETS; i++) {
        const char* name = PRESETS[i].name;
        if (!name || strlen(name) == 0) continue;
        if (isPresetEmpty(PRESETS[i].pixels)) continue;
        if (!first) presetsJson += ",";
        first = false;
        // Échappe les guillemets et backslashes du nom pour JS
        String safeName = name;
        safeName.replace("\\", "\\\\");
        safeName.replace("'", "\\'");
        presetsJson += "{name:'" + safeName + "',pixels:" + pixelsToJsArray(PRESETS[i].pixels) + "}";
    }
    presetsJson += "]";

    String injection =
        "<script>"
        "pixels = " + pxJson + ";"
        "document.getElementById('enabled').checked = " + (c.decorEnabled ? "true" : "false") + ";"
        "FW_PRESETS = " + presetsJson + ";"
        "rebuildPresetsDropdown();"
        "buildCanvas();"
        "</script>";
    html.replace("</body>", injection + "</body>");
    setupSrv->send(200, "text/html; charset=utf-8", html);
}

static void handleSaveDecor() {
    UserConfig c = loadUserConfig();
    c.decorEnabled = (setupSrv->arg("enabled") == "1");
    String data = setupSrv->arg("data");  // 256 × 4 chars hex = 1024 chars
    if (data.length() == 1024) {
        for (int i = 0; i < 256; i++) {
            String h = data.substring(i * 4, i * 4 + 4);
            c.decorPixels[i] = (uint16_t) strtoul(h.c_str(), nullptr, 16);
        }
    }
    saveUserConfig(c);
    setupSrv->sendHeader("Location", "/decor", true);
    setupSrv->send(303, "text/plain", "");  // PRG pattern, page se rouvre
}

static void handleNotFound() {
    // Captive portal : tout chemin redirige vers /
    setupSrv->sendHeader("Location", "/", true);
    setupSrv->send(302, "text/plain", "");
}

void startCaptivePortal() {
    // Affiche label SETUP en cyan sur la matrice
    DisplayManager.clearMatrix();
    DisplayManager.setTextColor(COL_CYAN);
    DisplayManager.printText(0, 6, "SETUP", TEXT_ALIGNMENT::CENTER, 1);
    DisplayManager.update();

    // Démarre l'AP avec un nom unique basé sur la MAC
    uint64_t mac = ESP.getEfuseMac();
    String apName = "TC001-Setup-" + String((uint32_t)(mac >> 16), HEX);
    apName.toUpperCase();

    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName.c_str());
    delay(500);
    IPAddress apIP = WiFi.softAPIP();

    Serial.println();
    Serial.println("══════════════════════════════════════════════");
    Serial.println("  CAPTIVE PORTAL — première configuration");
    Serial.println("══════════════════════════════════════════════");
    Serial.printf("  AP : %s\n", apName.c_str());
    Serial.printf("  IP : %s\n", apIP.toString().c_str());
    Serial.println("  → Connecte ton tel/PC à ce WiFi puis ouvre");
    Serial.println("    http://" + apIP.toString() + " dans le navigateur");
    Serial.println("══════════════════════════════════════════════");

    // DNS qui redirige tout vers nous (captive portal)
    setupDns = new DNSServer();
    setupDns->setErrorReplyCode(DNSReplyCode::NoError);
    setupDns->start(53, "*", apIP);

    // Web server avec formulaire
    setupSrv = new WebServer(80);
    setupSrv->on("/", HTTP_GET, handleRoot);
    setupSrv->on("/save", HTTP_POST, handleSave);
    setupSrv->on("/decor", HTTP_GET, handleDecor);
    setupSrv->on("/save_decor", HTTP_POST, handleSaveDecor);
    // Endpoints de détection captive portal (Apple, Android, Windows)
    setupSrv->on("/generate_204", HTTP_GET, handleNotFound);
    setupSrv->on("/hotspot-detect.html", HTTP_GET, handleNotFound);
    setupSrv->onNotFound(handleNotFound);
    setupSrv->begin();

    // Boucle infinie : on ne sort que par save → reboot
    while (true) {
        setupDns->processNextRequest();
        setupSrv->handleClient();
        delay(10);
    }
}
