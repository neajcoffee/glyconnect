#include <Arduino.h>
#include <esp32-hal.h>

#include "BGAlarmManager.h"
#include "BGDisplayManager.h"
#include "BGSourceManager.h"
#include "DisplayManager.h"
#include "GlucoseMain.h"
#include "PeripheryManager.h"
#include "ServerManager.h"
#include "SettingsManager.h"
#include "globals.h"
#include "improv_consume.h"

float apModeHintPosition = MATRIX_WIDTH;

void setup() {
    pinMode(15, OUTPUT);
    digitalWrite(15, LOW);
    delay(2000);
    Serial.begin(115200);

    DisplayManager.setup();
    SettingsManager.setup();
    if (!SettingsManager.loadSettingsFromFile()) {
        DisplayManager.showFatalError("Error loading software, please reinstall");
    }

    DisplayManager.applySettings();
    DisplayManager.HSVtext(3, 6, String("V " + String(VERSION)).c_str(), true, 0);
    delay(2000);

    // ── Lecture du mode (BLE ou Wi-Fi) et injection éventuelle de config ─────
    glucosePreSetup();

    if (isBLEMode) {
        // ── Mode BLE : Wi-Fi OFF, affichage direct depuis le transmetteur ────
        bgSourceManager.setup(BG_SOURCE::BLE_DIRECT);
        bgDisplayManager.setup();
        bgAlarmManager.setup();
        glucoseSetupBLE();   // démarre DexcomBLE après bgDisplayManager.setup()

    } else {
        // ── Mode Wi-Fi : comportement Nightscout existant inchangé ───────────
        PeripheryManager.setup();

        if (PeripheryManager.isButtonSelectPressed()) {
            DEBUG_PRINTLN("Center button pressed, resetting to factory defaults...");
            DisplayManager.scrollColorfulText("Factory reset initiated...");
            SettingsManager.factoryReset();
        }

        ServerManager.setup();
        bgSourceManager.setup(SettingsManager.settings.bg_source);
        bgDisplayManager.setup();
        bgAlarmManager.setup();

        DEBUG_PRINTLN("Setup done");
        if (ServerManager.isConnected) {
            String msg = "Nightscout clock | To configure go to http://" +
                         ServerManager.myIP.toString() + "/";
            DisplayManager.scrollColorfulText(msg);
            DisplayManager.clearMatrix();
            DisplayManager.setTextColor(COLOR_WHITE);
            DisplayManager.printText(0, 6, "Connect", TEXT_ALIGNMENT::CENTER, 2);
        }
    }
}

void showJoinAP() {
    SettingsManager.settings.brightness_mode = BRIGHTNES_MODE::MANUAL;
    DisplayManager.setBrightness(70);
    String hint = "Join " + SettingsManager.settings.hostname +
                  " Wi-fi network and go to http://" +
                  ServerManager.myIP.toString() + "/";
    if (apModeHintPosition < -240) {
        apModeHintPosition = 32;
        DisplayManager.clearMatrix();
    }
    DisplayManager.HSVtext(apModeHintPosition, 6, hint.c_str(), true, 1);
    apModeHintPosition -= 0.18;
}

void loop() {
#ifdef DEBUG_MEMORY
    static unsigned long lastMemoryCheck = 0;
    unsigned long currentMillis = millis();
    if (currentMillis - lastMemoryCheck >= 10000) {
        lastMemoryCheck = currentMillis;
        DEBUG_PRINTLN("Free memory: " + String(ESP.getFreeHeap()));
    }
#endif

    glucoseLoop();   // gère reset G+D 3s + tick BLE si mode BLE

    if (isBLEMode) {
        // ── Mode BLE : le display manager est alimenté par BGSourceBLEDirect ─
        bgDisplayManager.tick();
        bgAlarmManager.tick();

    } else {
        // ── Mode Wi-Fi : comportement existant inchangé ───────────────────────
        ServerManager.tick();
        if (ServerManager.isConnected) {
            bgSourceManager.tick();
            bgDisplayManager.tick();
            bgAlarmManager.tick();
        } else if (ServerManager.isInAPMode) {
            showJoinAP();
        }
        checckForImprovWifiConnection();
    }

    DisplayManager.tick();

    if (!isBLEMode) {
        PeripheryManager.tick();
    }
}
