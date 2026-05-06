#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include "DexcomBLE_BD.h"

// ── Log buffer → ntfy.sh ─────────────────────────────────────────────────────
static const uint8_t RLOG_CAP = 50;
static String  rlogBuf[RLOG_CAP];
static uint8_t rlogHead  = 0;
static uint8_t rlogCount = 0;

static void remoteLog(const String& msg) {
    Serial.println(msg);
    rlogBuf[rlogHead] = "[" + String(millis() / 1000) + "s] " + msg;
    rlogHead  = (rlogHead + 1) % RLOG_CAP;
    if (rlogCount < RLOG_CAP) rlogCount++;
}

// ── WiFi : flush logs + check OTA ────────────────────────────────────────────
static void flushAndCheckOTA(DexcomBLEBD* dexcom) {
    if (dexcom) dexcom->stopScan();
    delay(200);

    WiFi.mode(WIFI_STA);
    WiFi.begin(DEV_WIFI_SSID, DEV_WIFI_PASS);

    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) delay(200);

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[OTA] WiFi timeout");
        WiFi.mode(WIFI_OFF);
        delay(500);
        return;
    }
    remoteLog("[OTA] WiFi OK " + WiFi.localIP().toString());

    WiFiClientSecure client;
    client.setInsecure();

    // 1. Envoyer les logs
    if (rlogCount > 0) {
        String body;
        uint8_t start = (rlogCount < RLOG_CAP) ? 0 : rlogHead;
        for (uint8_t i = 0; i < rlogCount; i++)
            body += rlogBuf[(start + i) % RLOG_CAP] + "\n";
        rlogHead = 0; rlogCount = 0;

        HTTPClient http;
        http.begin(client, "https://ntfy.sh/" DEV_NTFY_TOPIC);
        http.addHeader("Title", "TC001-BD v" + String(FIRMWARE_VERSION));
        http.addHeader("Content-Type", "text/plain; charset=utf-8");
        int code = http.POST(body);
        Serial.printf("[LOG] ntfy → %d\n", code);
        http.end();
    }

    // 2. Vérifier OTA
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.begin(client, String(DEV_OTA_BASE_URL) + "/version.txt");
    int code = http.GET();
    if (code == 200) {
        int serverVer = http.getString().toInt();
        Serial.printf("[OTA] serveur=%d local=%d\n", serverVer, FIRMWARE_VERSION);
        http.end();
        if (serverVer > FIRMWARE_VERSION) {
            remoteLog("[OTA] Mise à jour v" + String(serverVer) + " → flash...");
            // Client frais pour le téléchargement (évite état sale après version.txt)
            WiFiClientSecure otaClient;
            otaClient.setInsecure();
            httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            t_httpUpdate_return ret = httpUpdate.update(otaClient, String(DEV_OTA_BASE_URL) + "/firmware.bin");
            if (ret == HTTP_UPDATE_OK) {
                remoteLog("[OTA] Succès → reboot");
                ESP.restart();
            } else {
                remoteLog("[OTA] Echec " + String(httpUpdate.getLastError()) + ": " + httpUpdate.getLastErrorString());
            }
        }
    } else {
        http.end();
    }

    WiFi.mode(WIFI_OFF);
    delay(500);
}

// ── DexcomBLEBD instance ──────────────────────────────────────────────────────
static DexcomBLEBD* dexcom = nullptr;
static unsigned long lastCheckMs = 0;
static const unsigned long CHECK_INTERVAL = 60UL * 1000;

static void onReading(const DexcomReadingBD& r) {
    if (!r.sensorOk) {
        remoteLog("[GLUC] Capteur hors du corps");
        return;
    }
    char buf[80];
    snprintf(buf, sizeof(buf), "[GLUC] %u mg/dL (%.1f mmol) trend=%d pred=%u age=%luh",
             r.mgdl, r.mmol, r.trend, r.predicted,
             (unsigned long)r.sensorAge / 3600);
    remoteLog(buf);
}

// ── Arduino setup/loop ───────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("[BOOT] TC001-BD fw v" + String(FIRMWARE_VERSION));
    remoteLog("[BOOT] TC001-BD fw v" + String(FIRMWARE_VERSION) + " Bluedroid tx=" DEV_TRANSMITTER_ID);

    dexcom = new DexcomBLEBD(DEV_TRANSMITTER_ID);
    dexcom->begin(
        onReading,
        [](const String& s) { remoteLog("[BLE] " + s); }
    );
}

void loop() {
    // Log transitions d'état
    static String lastStateName = "";
    String cur = dexcom->getStateName();
    if (cur != lastStateName) {
        remoteLog("[STATE] " + lastStateName + " → " + cur);
        lastStateName = cur;
    }

    // Flush logs + OTA quand BLE idle
    if (dexcom->isWaiting()) {
        unsigned long now = millis();
        if (now - lastCheckMs > CHECK_INTERVAL) {
            lastCheckMs = now;
            flushAndCheckOTA(dexcom);
        }
    }

    dexcom->tick();
    delay(10);
}
