#include "GlucoseMain.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

#include "BGSourceBLEDirect.h"
#include "BGSourceManager.h"
#include "BGDisplayManager.h"
#include "DexcomBLE.h"
#include "GlucoseCaptivePortal.h"
#include "SettingsManager.h"

#if defined(DEV_TRANSMITTER_ID) || defined(DEV_FORCE_WIFI)
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#endif

// ── GPIO boutons TC001 ────────────────────────────────────────────────────────
#define BTN_LEFT   26   // power button hardware — ne pas utiliser pour reset
#define BTN_RIGHT  14
#define BTN_SELECT 27   // bouton central — utilisé pour le reset BLE

// ── État global ───────────────────────────────────────────────────────────────
bool isBLEMode = false;

static DexcomBLE*         dexcom     = nullptr;
static BGSourceBLEDirect* bleSrc     = nullptr;
static unsigned long      resetStart = 0;

// ── Dev remote logging + OTA (actif uniquement si DEV_TRANSMITTER_ID défini) ──
#ifdef DEV_TRANSMITTER_ID
static unsigned long lastOtaCheckMs = 0;
static const unsigned long OTA_INTERVAL_MS = 60UL * 1000;  // 1 min

// ── Buffer de logs circulaire ─────────────────────────────────────────────────
static const uint8_t RLOG_CAP = 40;
static String  rlogBuf[RLOG_CAP];
static uint8_t rlogHead  = 0;
static uint8_t rlogCount = 0;

static void remoteLog(const String& msg) {
    Serial.println(msg);
    rlogBuf[rlogHead] = "[" + String(millis() / 1000) + "s] " + msg;
    rlogHead  = (rlogHead + 1) % RLOG_CAP;
    if (rlogCount < RLOG_CAP) rlogCount++;
}

// ── Envoi du buffer vers ntfy.sh ──────────────────────────────────────────────
static void flushLogsToNtfy(WiFiClientSecure& client) {
    if (rlogCount == 0) return;

    String body;
    uint8_t start = (rlogCount < RLOG_CAP) ? 0 : rlogHead;
    for (uint8_t i = 0; i < rlogCount; i++)
        body += rlogBuf[(start + i) % RLOG_CAP] + "\n";
    rlogHead  = 0;
    rlogCount = 0;

    HTTPClient http;
    http.begin(client, "https://ntfy.sh/" DEV_NTFY_TOPIC);
    http.addHeader("Title", "TC001 v" + String(FIRMWARE_VERSION));
    http.addHeader("Content-Type", "text/plain; charset=utf-8");
    int code = http.POST(body);
    Serial.printf("[LOG] ntfy → HTTP %d\n", code);
    http.end();
}

// ── Fenêtre WiFi : logs + OTA ─────────────────────────────────────────────────
static void checkForOTA() {
    remoteLog("[OTA] Vérification fw v" + String(FIRMWARE_VERSION) + "...");

    // Arrêter le scan BLE avant WiFi — ils partagent la radio 2.4GHz
    if (dexcom) dexcom->stopScan();
    delay(100);

    WiFi.mode(WIFI_STA);
    WiFi.begin(DEV_WIFI_SSID, DEV_WIFI_PASS);

    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) delay(200);

    // Retry si premier essai échoue
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[OTA] WiFi timeout, retry...");
        WiFi.disconnect();
        delay(500);
        WiFi.begin(DEV_WIFI_SSID, DEV_WIFI_PASS);
        t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) delay(200);
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[OTA] WiFi échec → abandon");
        WiFi.mode(WIFI_OFF);
        return;
    }
    Serial.println("[OTA] WiFi OK — " + WiFi.localIP().toString());

    WiFiClientSecure client;
    client.setInsecure();  // dev only — skip cert check GitHub HTTPS

    // 1. Envoyer les logs accumulés
    flushLogsToNtfy(client);

    // 2. Vérifier si une mise à jour est disponible
    HTTPClient http;
    String urlBase = DEV_OTA_BASE_URL;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.begin(client, urlBase + "/version.txt");
    int code = http.GET();
    if (code == 200) {
        int serverVer = http.getString().toInt();
        Serial.printf("[OTA] serveur=%d local=%d\n", serverVer, FIRMWARE_VERSION);
        http.end();

        if (serverVer > FIRMWARE_VERSION) {
            Serial.println("[OTA] Mise à jour disponible → flash...");
            httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            t_httpUpdate_return ret = httpUpdate.update(client, urlBase + "/firmware.bin");
            switch (ret) {
                case HTTP_UPDATE_OK:
                    Serial.println("[OTA] Succès → reboot");
                    ESP.restart();
                    break;
                default:
                    Serial.printf("[OTA] Échec %d : %s\n",
                        httpUpdate.getLastError(),
                        httpUpdate.getLastErrorString().c_str());
            }
        } else {
            Serial.println("[OTA] Firmware à jour");
        }
    } else {
        Serial.printf("[OTA] version.txt HTTP %d\n", code);
        http.end();
    }

    WiFi.mode(WIFI_OFF);
    delay(800);  // Laisser la radio se libérer avant que BLE reprenne
    Serial.println("[OTA] WiFi OFF → reprise BLE");
}
#endif

// ── Mapping rate (0.1 mg/dL/min) → BG_TREND ──────────────────────────────────
static BG_TREND rateToBGTrend(int8_t rate) {
    if (rate == 0)   return BG_TREND::NOT_COMPUTABLE;
    if (rate > 30)   return BG_TREND::DOUBLE_UP;
    if (rate > 20)   return BG_TREND::SINGLE_UP;
    if (rate > 10)   return BG_TREND::FORTY_FIVE_UP;
    if (rate >= -10) return BG_TREND::FLAT;
    if (rate > -20)  return BG_TREND::FORTY_FIVE_DOWN;
    if (rate > -30)  return BG_TREND::SINGLE_DOWN;
    return BG_TREND::DOUBLE_DOWN;
}

// ── Callback lecture BLE ──────────────────────────────────────────────────────
static void onBLEReading(const DexcomReading& r) {
    if (!bleSrc) return;

    if (!r.sensorOk) {
#ifdef DEV_TRANSMITTER_ID
        remoteLog("[GLUC] Capteur hors du corps");
#endif
        bleSrc->push(GlucoseReading());
        return;
    }

#ifdef DEV_TRANSMITTER_ID
    char buf[64];
    snprintf(buf, sizeof(buf), "[GLUC] %u mg/dL (%.1f mmol) trend=%d pred=%u",
             r.mgdl, r.mmol, r.trend, r.predicted);
    remoteLog(buf);
#endif

    GlucoseReading gr;
    gr.sgv   = r.mgdl;
    gr.trend = rateToBGTrend(r.trend);
    gr.epoch = (millis() / 1000UL) + 1704067200UL;

    bleSrc->push(gr);
}

// ── glucosePreSetup() ─────────────────────────────────────────────────────────
// Appelé dans main.cpp APRÈS loadSettingsFromFile()
void glucosePreSetup() {
    pinMode(BTN_LEFT,   INPUT_PULLUP);
    pinMode(BTN_RIGHT,  INPUT_PULLUP);
    pinMode(BTN_SELECT, INPUT_PULLUP);

#ifdef DEV_TRANSMITTER_ID
    // Préconfiguration dev BLE : mode + serial au démarrage
    {
        Preferences p;
        p.begin("glucose", false);
        p.putString("mode",           "ble");
        p.putString("transmitter_id", DEV_TRANSMITTER_ID);
        p.end();
        Serial.println("[DEV] Config BLE écrite : " DEV_TRANSMITTER_ID);
    }
    pinMode(15, OUTPUT); digitalWrite(15, LOW);
    Serial.println("[DEV] Buzzer muet");
#endif

#ifdef DEV_FORCE_WIFI
    // Préconfiguration dev WiFi : forcer mode WiFi + Nightscout
    {
        Preferences p;
        p.begin("glucose", false);
        p.putString("mode",     "wifi");
        p.putString("ssid",     DEV_WIFI_SSID);
        p.putString("pwd",      DEV_WIFI_PASS);
        p.putString("ns_url",   DEV_NIGHTSCOUT_URL);
        p.putString("api_key",  DEV_NIGHTSCOUT_API_KEY);
        p.end();
        Serial.println("[DEV] Config WiFi écrite (Nightscout)");
    }
#endif

    Preferences prefs;
    prefs.begin("glucose", true);
    String mode = prefs.getString("mode", "");

    if (mode.isEmpty()) {
        prefs.end();
        // Aucune config → portail captif (bloquant jusqu'à restart)
        GlucoseCaptivePortal::start();
        return; // jamais atteint (restart dans start())
    }

    if (mode == "ble") {
        isBLEMode = true;
        // Stocker le serial pour glucoseSetupBLE()
        // (lu à nouveau dans glucoseSetupBLE pour éviter de le passer en global)
        prefs.end();
        WiFi.mode(WIFI_OFF);

    } else {
        // mode == "wifi" : injecter la config dans SettingsManager
        SettingsManager.settings.ssid              = prefs.getString("ssid",    "");
        SettingsManager.settings.wifi_password     = prefs.getString("pwd",     "");
        SettingsManager.settings.nightscout_url    = prefs.getString("ns_url",  "");
        SettingsManager.settings.nightscout_api_key= prefs.getString("api_key", "");
        SettingsManager.settings.bg_source         = BG_SOURCE::NIGHTSCOUT;
        prefs.end();
    }
}

// ── glucoseSetupBLE() ─────────────────────────────────────────────────────────
// Appelé dans main.cpp si isBLEMode, après bgDisplayManager.setup()
void glucoseSetupBLE() {
    Preferences prefs;
    prefs.begin("glucose", true);
    String txId = prefs.getString("transmitter_id", "");
    prefs.end();

    // Récupérer le pointeur vers la source BLE_DIRECT
    bleSrc = static_cast<BGSourceBLEDirect*>(bgSourceManager.getSource());

#ifdef DEV_TRANSMITTER_ID
    remoteLog("[BOOT] TC001 fw v" + String(FIRMWARE_VERSION) + " tx=" + txId);
#endif
    dexcom = new DexcomBLE(txId);
    dexcom->begin(
        onBLEReading,
        [](const String& s) {
            Serial.println("[BLE] " + s);
            DisplayManager.clearMatrix();
            DisplayManager.printText(0, 6, s.c_str(), TEXT_ALIGNMENT::CENTER, 2);
        }
    );
}

// ── OTA en mode WiFi (WiFi déjà actif, pas de toggle) ────────────────────────
#if defined(DEV_FORCE_WIFI) && defined(DEV_OTA_BASE_URL)
static unsigned long wifiOtaCheckMs = 0;
static void wifiOtaCheck() {
    if (WiFi.status() != WL_CONNECTED) return;
    Serial.printf("[OTA-WIFI] heap=%u largest=%u\n",
        (unsigned)ESP.getFreeHeap(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    WiFiClientSecure client; client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.begin(client, String(DEV_OTA_BASE_URL) + "/version_wifi.txt");
    int code = http.GET();
    if (code != 200) { http.end(); return; }
    int sv = http.getString().toInt();
    Serial.printf("[OTA-WIFI] serveur=%d local=%d\n", sv, FIRMWARE_VERSION);
    http.end();
    if (sv <= FIRMWARE_VERSION) return;

    Serial.println("[OTA-WIFI] MAJ → flash...");
    client.stop();
    WiFiClientSecure otaClient; otaClient.setInsecure();
    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    auto ret = httpUpdate.update(otaClient, String(DEV_OTA_BASE_URL) + "/firmware_wifi.bin");
    if (ret == HTTP_UPDATE_OK) ESP.restart();
    Serial.printf("[OTA-WIFI] Echec %d\n", httpUpdate.getLastError());
}
#endif

// ── glucoseLoop() ─────────────────────────────────────────────────────────────
void glucoseLoop() {
    // ── Bouton RESET (G+D simultanés 3 secondes) — MODE BLE UNIQUEMENT ──────
    // En mode Wi-Fi, PeripheryManager gère les mêmes GPIO via Button2 →
    // conflit. Utiliser le factory reset existant (bouton central au démarrage).
    if (!isBLEMode) {
#if defined(DEV_FORCE_WIFI) && defined(DEV_OTA_BASE_URL)
        // Check OTA toutes les 5 minutes en mode WiFi
        unsigned long now = millis();
        if (now - wifiOtaCheckMs > 5UL * 60 * 1000) {
            wifiOtaCheckMs = now;
            wifiOtaCheck();
        }
#endif
        if (dexcom) dexcom->tick();
        return;
    }

    // Bouton central (GPIO 27) maintenu 5s → reset
    // Le bouton gauche (GPIO 26) est le power hardware — évité
    bool sel = (digitalRead(BTN_SELECT) == LOW);

    if (sel) {
        if (resetStart == 0) resetStart = millis();
        if (millis() - resetStart > 5000) {
            // Afficher "RESET" rouge sur la matrice
            DisplayManager.clearMatrix();
            DisplayManager.printText(0, 6, "RESET", TEXT_ALIGNMENT::CENTER, 2);
            delay(1500);

            Preferences prefs;
            prefs.begin("glucose", false);
            prefs.clear();
            prefs.end();
            WiFi.mode(WIFI_OFF);
            delay(200);
            ESP.restart();
        }
    } else {
        resetStart = 0; // relâché trop tôt → annuler
    }

    // ── Logging d'état verbose (dev) ────────────────────────────────────────
#ifdef DEV_TRANSMITTER_ID
    static String lastStateName = "";
    if (dexcom) {
        String cur = dexcom->getStateName();
        if (cur != lastStateName) {
            remoteLog("[STATE] " + lastStateName + " → " + cur);
            lastStateName = cur;
        }
    }
#endif

    // ── Check OTA dev (uniquement pendant WAIT_NEXT, BLE radio idle) ────────
#ifdef DEV_TRANSMITTER_ID
    if (dexcom && dexcom->isWaiting()) {
        unsigned long now = millis();
        if (now - lastOtaCheckMs > OTA_INTERVAL_MS) {
            lastOtaCheckMs = now;
            checkForOTA();
        }
    }
#endif

    // ── Tick BLE ────────────────────────────────────────────────────────────
    if (dexcom) dexcom->tick();
}
