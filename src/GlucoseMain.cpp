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

// ── GPIO boutons TC001 ────────────────────────────────────────────────────────
// Ajuster selon la révision hardware (vérifier dans platformio.ini)
#define BTN_LEFT  26
#define BTN_RIGHT 14

// ── État global ───────────────────────────────────────────────────────────────
bool isBLEMode = false;

static DexcomBLE*       dexcom     = nullptr;
static BGSourceBLEDirect* bleSrc   = nullptr;
static unsigned long    resetStart = 0;

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
        // Capteur hors du corps : vider les lectures pour effacer l'affichage
        bleSrc->push(GlucoseReading());  // lecture vide
        return;
    }

    GlucoseReading gr;
    gr.sgv   = r.mgdl;
    gr.trend = rateToBGTrend(r.trend);
    // epoch : secondes Unix approx (millis + offset 2024-01-01)
    gr.epoch = (millis() / 1000UL) + 1704067200UL;

    bleSrc->push(gr);
}

// ── glucosePreSetup() ─────────────────────────────────────────────────────────
// Appelé dans main.cpp APRÈS loadSettingsFromFile()
void glucosePreSetup() {
    pinMode(BTN_LEFT,  INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);

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

    dexcom = new DexcomBLE(txId);
    dexcom->begin(
        onBLEReading,
        [](const String& state) {
            Serial.println("[BLE] " + state);
            // Optionnel : afficher l'état sur la matrice
            // DisplayManager.printText(0, 6, state.c_str(), TEXT_ALIGNMENT::CENTER, 2);
        }
    );
}

// ── glucoseLoop() ─────────────────────────────────────────────────────────────
void glucoseLoop() {
    // ── Bouton RESET (G+D simultanés 3 secondes) ────────────────────────────
    bool L = (digitalRead(BTN_LEFT)  == LOW);
    bool R = (digitalRead(BTN_RIGHT) == LOW);

    if (L && R) {
        if (resetStart == 0) resetStart = millis();
        if (millis() - resetStart > 3000) {
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
        resetStart = 0;
    }

    // ── Tick BLE (mode BLE uniquement) ──────────────────────────────────────
    if (isBLEMode && dexcom) dexcom->tick();
}
