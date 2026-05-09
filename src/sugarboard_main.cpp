#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <esp_log.h>
#include <esp_system.h>
#include <time.h>
#include <Preferences.h>
#include <FastLED.h>
#include "DisplayManager.h"
#include "DexcomBLE_NB.h"


// ── Boutons TC001 (Ulanzi) — pins matériels ─────────────────────────────────
// Détection via interruptions GPIO (pas Button2/polling) : garantie hardware
// que chaque press est capté même si CPU saturé par BLE/HTTP.
#define BTN_LEFT_PIN   26    // bouton gauche  (UP physique)   = baisse luminosité
#define BTN_RIGHT_PIN  14    // bouton droite  (DOWN physique) = monte luminosité
#define BTN_SELECT_PIN 27    // bouton centre  (réservé pour usage futur)

static const uint32_t BTN_DEBOUNCE_MS = 60;  // 60ms entre 2 press sur même pin

static volatile uint32_t g_lastIsrLeft       = 0;
static volatile uint32_t g_lastIsrRight      = 0;
static volatile bool     g_btnLeftPending       = false;
static volatile bool     g_btnRightPending      = false;
static volatile bool     g_btnSelectShortPending = false;
static volatile bool     g_btnSelectLongPending  = false;

// SELECT : ISR sur CHANGE pour mesurer la durée press→release. Long press
// (≥1s) = toggle ON/OFF de l'écran, short press = réservé futur usage.
static volatile uint32_t g_selectPressMs   = 0;     // 0 = pas de press en cours
static volatile bool     g_selectLongFired = false; // évite double-fire pendant le poll

static void IRAM_ATTR isrBtnLeft() {
    uint32_t now = millis();
    if (now - g_lastIsrLeft >= BTN_DEBOUNCE_MS) {
        g_lastIsrLeft = now;
        g_btnLeftPending = true;
    }
}
static void IRAM_ATTR isrBtnRight() {
    uint32_t now = millis();
    if (now - g_lastIsrRight >= BTN_DEBOUNCE_MS) {
        g_lastIsrRight = now;
        g_btnRightPending = true;
    }
}
static void IRAM_ATTR isrBtnSelect() {
    uint32_t now = millis();
    bool isLow = (digitalRead(BTN_SELECT_PIN) == LOW);
    if (isLow) {
        // Press : enregistre le début (le poll dans buttonProcessTask déclenche
        // le long press dès 1s sans attendre le release).
        g_selectPressMs   = now;
        g_selectLongFired = false;
    } else {
        // Release : si pas déjà fired en long, c'est un short press.
        if (g_selectPressMs > 0 && !g_selectLongFired) {
            uint32_t duration = now - g_selectPressMs;
            if (duration >= 30 && duration < 1000) g_btnSelectShortPending = true;
        }
        g_selectPressMs = 0;
    }
}

// Forward decl : remoteLog est défini plus bas dans le fichier
static void remoteLog(const String& msg);

// 5 niveaux de luminosité fixes (smartphone-like) : nuit / chambre / bureau / pièce lumineuse / plein jour.
// Index sauvegardé en NVS (0..4), valeur effective = BRIGHTNESS_LEVELS[index].
static const uint8_t BRIGHTNESS_LEVELS[]   = {1, 8, 32, 96, 255};
static const uint8_t BRIGHTNESS_LEVELS_N   = sizeof(BRIGHTNESS_LEVELS);
static int8_t  brightnessLevel = 2;          // default niveau médian (= 128)
static uint8_t brightness      = 128;

// Couleur grise pour les tirets et les digits "périmés" (>20 min).
// Tri-mode selon brightness — physique : pixel_visible = color * brightness / 255
// doit être >= 1 (sinon LED éteinte).
//   - brightness >= 32 (levels 2,3,4) : 0x4208 (#404040) — discret
//   - brightness >= 8  (level  1)     : 0x8410 (#808080) — plus clair
//   - brightness <  8  (level  0)     : 0xFFFF (#FFFFFF) — seul le blanc pur
//     est perceptible à brightness=1 (255*1/255 = 1 PWM minimum).
// Toujours équilibré R8=G8=B8 → vrai gris, jamais teinté vert/rouge.
static uint16_t ageGreyFixed() {
    if (brightness >= 32) return 0x4208;  // #404040
    if (brightness >= 8)  return 0x8410;  // #808080
    return 0xFFFF;                         // #FFFFFF
}

// Forward decls : utilisés par les handlers boutons définis plus bas dans ce fichier.
static bool     g_displayOff       = false;
static bool     hasGlucose         = false;
static uint16_t lastGlucoseMgdl    = 0;
static int8_t   lastGlucoseTrend   = 0;
static uint32_t lastGlucoseRxMs    = 0;   // millis() de la dernière lecture
static int      g_lastBarsCount    = -1;  // dernière count de tirets dessinés
static uint32_t g_simAgeOffsetMs   = 0;   // offset simulé pour TEST_AGE_CYCLE
static void     displayGlucose(uint16_t mgdl, int8_t trend);
static void     drawAgeBars();

// Calcule l'âge effectif de la lecture (avec offset simulé pour test)
static uint32_t glucoseElapsedMs() {
    if (lastGlucoseRxMs == 0) return g_simAgeOffsetMs;
    return (millis() - lastGlucoseRxMs) + g_simAgeOffsetMs;
}

static void saveBrightness() {
    Preferences prefs;
    prefs.begin("display", false);
    prefs.putChar("level", brightnessLevel);
    prefs.end();
}

static void loadBrightness() {
    Preferences prefs;
    prefs.begin("display", true);
    brightnessLevel = prefs.getChar("level", 2);
    prefs.end();
    if (brightnessLevel < 0) brightnessLevel = 0;
    if (brightnessLevel >= (int8_t)BRIGHTNESS_LEVELS_N) brightnessLevel = BRIGHTNESS_LEVELS_N - 1;
    brightness = BRIGHTNESS_LEVELS[brightnessLevel];
}

static void applyBrightness() {
    DisplayManager.setBrightness(brightness);
    Serial.printf("[BTN] level=%d brightness=%u\n", (int)brightnessLevel, (unsigned)brightness);
}

// Handlers exécutés par buttonProcessTask (core 1, prio 10), indépendant du
// main loop. Garantit la réactivité même si le main loop est dans un HTTP
// timeout 3-8s. setBrightness() inclut un show() qui rend immédiatement la
// nouvelle luminosité visible — pas besoin de feedback texte.
static void handleBtnLeft() {
    if (brightnessLevel > 0) brightnessLevel--;
    brightness = BRIGHTNESS_LEVELS[brightnessLevel];
    DisplayManager.setBrightness(brightness);
    saveBrightness();
    // Re-render pour appliquer la couleur grise selon la nouvelle brightness
    if (hasGlucose && !g_displayOff) displayGlucose(lastGlucoseMgdl, lastGlucoseTrend);
    char buf[40];
    snprintf(buf, sizeof(buf), "[BTN-L] level=%d brightness=%u", (int)brightnessLevel, (unsigned)brightness);
    remoteLog(buf);
}

static void handleBtnRight() {
    if (brightnessLevel < (int8_t)BRIGHTNESS_LEVELS_N - 1) brightnessLevel++;
    brightness = BRIGHTNESS_LEVELS[brightnessLevel];
    DisplayManager.setBrightness(brightness);
    saveBrightness();
    // Re-render pour appliquer la couleur grise selon la nouvelle brightness
    if (hasGlucose && !g_displayOff) displayGlucose(lastGlucoseMgdl, lastGlucoseTrend);
    char buf[40];
    snprintf(buf, sizeof(buf), "[BTN-R] level=%d brightness=%u", (int)brightnessLevel, (unsigned)brightness);
    remoteLog(buf);
}

static void handleBtnSelectLong() {
    g_displayOff = !g_displayOff;
    if (g_displayOff) {
        DisplayManager.setBrightness(0);
        DisplayManager.clearMatrix();
        DisplayManager.update();
        remoteLog("[BTN-S] long → display OFF");
    } else {
        DisplayManager.setBrightness(brightness);
        if (hasGlucose) displayGlucose(lastGlucoseMgdl, lastGlucoseTrend);
        else            DisplayManager.update();
        remoteLog("[BTN-S] long → display ON");
    }
}

// Task dédié : process les flags posés par les ISR. Préempte le main loop
// (priority 10 vs loopTask 1) → réagit même quand main loop bloqué sur HTTP.
// Poll aussi le press-en-cours du SELECT pour fire le long press dès 1s écoulée.
static void buttonProcessTask(void* /*p*/) {
    for (;;) {
        if (g_btnLeftPending)        { g_btnLeftPending        = false; handleBtnLeft();         }
        if (g_btnRightPending)       { g_btnRightPending       = false; handleBtnRight();        }
        if (g_btnSelectLongPending)  { g_btnSelectLongPending  = false; handleBtnSelectLong();   }
        if (g_btnSelectShortPending) { g_btnSelectShortPending = false; /* TODO short SELECT */  }

        // Détection long press en temps réel : si pin LOW depuis ≥1s sans avoir
        // déjà fire, on déclenche maintenant (pas besoin d'attendre le release).
        if (g_selectPressMs > 0 && !g_selectLongFired
            && (millis() - g_selectPressMs >= 1000)) {
            g_selectLongFired = true;
            g_btnSelectLongPending = true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ── Couleurs RGB565 (format Adafruit GFX) ────────────────────────────────────
#define COL_BLACK   0x0000
#define COL_RED     0xF800
#define COL_GREEN   0x07E0
#define COL_BLUE    0x001F
#define COL_WHITE   0xFFFF
#define COL_YELLOW  0xFFE0
#define COL_ORANGE  0xFC00
#define COL_CYAN    0x07FF
#define COL_MAGENTA 0xF81F
#define COL_GRAY    0x8410

// Logique 5 zones (alignée avec glucose-reader/src/ble/glucose.js getZone) :
//   < 54           → ROUGE  (Hypoglycémie sévère)
//   54-69          → JAUNE  (Hypoglycémie)
//   70-180         → VERT   (Dans la cible)
//   181-250        → JAUNE  (Hyperglycémie)
//   > 250          → ROUGE  (Hyperglycémie sévère)
static uint16_t glucoseColorMgdl(uint16_t mgdl) {
    if (mgdl < 54 || mgdl > 250) return COL_RED;
    if (mgdl < 70 || mgdl > 180) return COL_YELLOW;
    return COL_GREEN;
}

// ── Bitmaps des flèches tendance (monochromes, couleur appliquée par firmware) ──
// Single arrows : 5×5 = 5 octets (1 octet/ligne, 5 bits utiles MSB)
// Double arrows : 10×5 = 10 octets (2 octets/ligne pour packing GFX 10-wide)
static const uint8_t arrow_singleUp[]      PROGMEM = {0x20, 0x70, 0xA8, 0x20, 0x20};
static const uint8_t arrow_fortyFiveUp[]   PROGMEM = {0x78, 0x18, 0x28, 0x48, 0x80};
static const uint8_t arrow_flat[]          PROGMEM = {0x20, 0x10, 0xF8, 0x10, 0x20};
static const uint8_t arrow_fortyFiveDown[] PROGMEM = {0x80, 0x48, 0x28, 0x18, 0x78};
static const uint8_t arrow_singleDown[]    PROGMEM = {0x20, 0x20, 0xA8, 0x70, 0x20};
// Double arrows 10×5 : deux flèches simples côte à côte (cols 0-4 + cols 5-9)
static const uint8_t arrow_doubleUp[]      PROGMEM = { 0x22, 0x00, 0x77, 0x00, 0xAA, 0x80, 0x22, 0x00, 0x22, 0x00 };
static const uint8_t arrow_doubleDown[]    PROGMEM = { 0x22, 0x00, 0x22, 0x00, 0xAA, 0x80, 0x77, 0x00, 0x22, 0x00 };

// Métadonnée flèche : pointeur bitmap + largeur (hauteur toujours 5)
struct ArrowInfo { const uint8_t* bitmap; uint8_t width; };


// ── Bitmaps de labels plein-écran 32×8 en RGB565 multi-couleur ──────────────
// Stockage : 256 uint16_t (1 par pixel), ordre row-major (256 = 32 × 8).
// 0x0000 = pixel transparent (LED éteinte). Sinon = couleur RGB565 affichée.
// Designés dans tools/sprite_editor.html (peinture pixel-par-pixel).
//
// Macro pour générer un label entièrement à zéro (256 valeurs = écran noir)
#define LABEL_EMPTY_256 \
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, \
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, \
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, \
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, \
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, \
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, \
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, \
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0

static const uint16_t label_boot[] PROGMEM = { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF81F, 0xF81F, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0xF81F, 0x0000, 0x0000, 0xF81F, 0xF81F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF81F, 0x0000, 0x0000, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF81F, 0xF81F, 0xF81F, 0x0000, 0xF81F, 0xF81F, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF81F, 0x0000, 0x0000, 0xF81F, 0x0000, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF81F, 0xF81F, 0xF81F, 0x0000, 0x0000, 0xF81F, 0x0000, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0xF81F, 0x0000, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 };
static const uint16_t label_scan[] PROGMEM = { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF81F, 0xF81F, 0xF81F, 0x0000, 0xF81F, 0xF81F, 0x0000, 0xF81F, 0xF81F, 0x0000, 0x0000, 0xF81F, 0xF81F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF81F, 0x0000, 0x0000, 0x0000, 0xF81F, 0x0000, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF81F, 0xF81F, 0xF81F, 0x0000, 0xF81F, 0x0000, 0x0000, 0xF81F, 0xF81F, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF81F, 0xF81F, 0xF81F, 0x0000, 0xF81F, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xF81F, 0x0000, 0xFFE0, 0x0000, 0xFFE0, 0x0000, 0xFFE0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 };
static const uint16_t label_wait[]  PROGMEM = { LABEL_EMPTY_256 };
static const uint16_t label_off[]   PROGMEM = { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x5800, 0x8800, 0xC800, 0xF800, 0xF800, 0xF800, 0xC800, 0x8800, 0x5800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF800, 0xF800, 0xF800, 0x0000, 0xF800, 0xF800, 0xF800, 0x0000, 0xF800, 0xF800, 0xF800, 0x5800, 0x8800, 0xC800, 0xF800, 0xF800, 0xFFFF, 0xF800, 0xF800, 0xC800, 0x8800, 0x5800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF800, 0x0000, 0xF800, 0x0000, 0xF800, 0x0000, 0x0000, 0x0000, 0xF800, 0x0000, 0x0000, 0x5800, 0x8800, 0xC800, 0xF800, 0xF800, 0xFFFF, 0xF800, 0xF800, 0xC800, 0x8800, 0x5800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF800, 0x0000, 0xF800, 0x0000, 0xF800, 0xF800, 0xF800, 0x0000, 0xF800, 0xF800, 0xF800, 0x5800, 0x8800, 0xC800, 0xF800, 0xF800, 0xFFFF, 0xF800, 0xF800, 0xC800, 0x8800, 0x5800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF800, 0x0000, 0xF800, 0x0000, 0xF800, 0x0000, 0x0000, 0x0000, 0xF800, 0x0000, 0x0000, 0x5800, 0x8800, 0xC800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xC800, 0x8800, 0x5800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF800, 0x0000, 0xF800, 0x0000, 0xF800, 0x0000, 0x0000, 0x0000, 0xF800, 0x0000, 0x0000, 0x5800, 0x8800, 0xC800, 0xF800, 0xF800, 0xFFFF, 0xF800, 0xF800, 0xC800, 0x8800, 0x5800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF800, 0xF800, 0xF800, 0x0000, 0xF800, 0x0000, 0x0000, 0x0000, 0xF800, 0x0000, 0x0000, 0x0000, 0x8800, 0xC800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xC800, 0x8800, 0x5800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x5800, 0x8800, 0xC800, 0xC800, 0xC800, 0xC800, 0xC800, 0x8800, 0x5800, 0x0000, 0x0000 };
static const uint16_t label_err[]   PROGMEM = { LABEL_EMPTY_256 };
static const uint16_t label_conn[]  PROGMEM = { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07FF, 0x07FF, 0x0000, 0x07FF, 0x07FF, 0x07FF, 0x0000, 0x07FF, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07FF, 0x07FF, 0x0000, 0x07FF, 0x07FF, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 };
static const uint16_t label_auth[]  PROGMEM = { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07FF, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x07FF, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07FF, 0x07FF, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x07FF, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x07FF, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 };
static const uint16_t label_bond[]  PROGMEM = { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07FF, 0x07FF, 0x07FF, 0x0000, 0x07FF, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x07FF, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x07FF, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07FF, 0x07FF, 0x07FF, 0x0000, 0x07FF, 0x07FF, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x07FF, 0x0000, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07FF, 0x0000, 0x0000, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07FF, 0x0000, 0x0000, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x07FF, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x07FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 };
static const uint16_t label_time[]  PROGMEM = { LABEL_EMPTY_256 };
static const uint16_t label_found[] PROGMEM = { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x87E0, 0x87E0, 0x87E0, 0x0000, 0x87E0, 0x87E0, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x87E0, 0x0000, 0x0000, 0x87E0, 0x87E0, 0x0000, 0x0000, 0xFFE0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x87E0, 0x0000, 0x0000, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0xFFE0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x87E0, 0x87E0, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0xFFE0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x87E0, 0x0000, 0x0000, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x87E0, 0x0000, 0x0000, 0x0000, 0x87E0, 0x87E0, 0x87E0, 0x0000, 0x87E0, 0x87E0, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x0000, 0x87E0, 0x87E0, 0x0000, 0x0000, 0xFFE0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 };


// ── Decor presets 32×8 RGB565 — slots avec nom modifiable ──────────────────
// 8 slots disponibles. Chaque slot = un nom user (string) + 256 pixels RGB565.
// Designés dans sprite_editor.html. Captive portal /decor liste les slots non vides.
// Slot vide = nom == "" OU tous pixels à 0 → invisible dans le menu portal.
static const char     preset_1_name[]      = "spiderman";
static const uint16_t preset_1_pixels[] PROGMEM = { 0x4208, 0x4208, 0x4208, 0xC800, 0xF800, 0xC800, 0x4208, 0x4208, 0x4208, 0x4208, 0x4208, 0xC800, 0xC800, 0xC800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xC800, 0xC800, 0xC800, 0x4208, 0x4208, 0x4208, 0x4208, 0x4208, 0xC800, 0xF800, 0xC800, 0x4208, 0x4208, 0x4208, 0x4208, 0x4208, 0xC800, 0xF800, 0xF800, 0xC800, 0x4208, 0x4208, 0x4208, 0x4208, 0xC800, 0x0000, 0xC800, 0xC800, 0xF800, 0xF800, 0xF800, 0xF800, 0xC800, 0x0000, 0xC800, 0x4208, 0x4208, 0x4208, 0x4208, 0xC800, 0xF800, 0xF800, 0xC800, 0x4208, 0x4208, 0x4208, 0x4208, 0x4208, 0x4208, 0xC800, 0xF800, 0xF800, 0xC800, 0x4208, 0x4208, 0x4208, 0xC800, 0xFFFF, 0x0000, 0x0000, 0xC800, 0xF800, 0xF800, 0x0000, 0x0000, 0xFFFF, 0xC800, 0x4208, 0x4208, 0x4208, 0xC800, 0xF800, 0xF800, 0xC800, 0x4208, 0x4208, 0x4208, 0x4208, 0x8800, 0x8800, 0x8800, 0x8800, 0xF800, 0xF800, 0xF800, 0x4208, 0x4208, 0x4208, 0xC800, 0xFFFF, 0xFFFF, 0x0000, 0xC800, 0xF800, 0xC800, 0x0000, 0xFFFF, 0xFFFF, 0xC800, 0x4208, 0x4208, 0x4208, 0xF800, 0xF800, 0xF800, 0x5800, 0x4208, 0x4208, 0x4208, 0x8800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0x5800, 0x4208, 0x4208, 0xC800, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0xF800, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xC800, 0x4208, 0x4208, 0x5800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0x8800, 0xC800, 0x8800, 0x8800, 0x8800, 0x8800, 0xF800, 0xF800, 0x5800, 0x5800, 0x4208, 0xC800, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0xF800, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0xC800, 0x4208, 0x5800, 0x5800, 0xF800, 0xF800, 0xF800, 0x8800, 0x8800, 0x8800, 0xC800, 0x8800, 0xC800, 0xC800, 0xC800, 0x8800, 0x8800, 0xF800, 0xF800, 0x5800, 0x5800, 0x4208, 0xC800, 0xC800, 0x0000, 0x0000, 0xC800, 0xC800, 0xC800, 0x0000, 0x0000, 0xC800, 0xC800, 0x4208, 0x5800, 0x5800, 0xF800, 0xF800, 0xF800, 0x8800, 0x8800, 0xC800, 0xC800, 0x4208, 0x8800, 0xC800, 0xC800, 0xC800, 0x8800, 0xC800, 0xF800, 0x5800, 0x5800, 0x5800, 0x5800, 0xC800, 0xC800, 0xC800, 0xF800, 0xF800, 0xF800, 0xC800, 0xC800, 0xC800, 0x5800, 0x5800, 0x5800, 0x5800, 0xF800, 0xF800, 0xC800, 0x8800, 0xC800, 0xC800, 0x8800 };
static const char     preset_2_name[]      = "pika";
static const uint16_t preset_2_pixels[] PROGMEM = { 0xFFFF, 0xFFFF, 0xFFFF, 0x4208, 0x4208, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x4208, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xFFE0, 0xCB20, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFE0, 0xFC00, 0xFFFF, 0xFFFF, 0xFFFF, 0xFC00, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xFFE0, 0xFFE0, 0xFFE0, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFE0, 0xFFE0, 0xFFE0, 0xFFE0, 0xFC00, 0xFFFF, 0xFFFF, 0x0000, 0xCB20, 0xFFE0, 0xFFE0, 0xFFE0, 0xCB20, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFC00, 0xFC00, 0xFC00, 0xFFFF, 0xFFE0, 0x0000, 0xFFE0, 0xFFE0, 0x0000, 0xFFFF, 0x0000, 0xCB20, 0xFFE0, 0xFFE0, 0xFFE0, 0xCB20, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFC00, 0xFC00, 0xFC00, 0xFFFF, 0xC800, 0xFFE0, 0xFFE0, 0xFFE0, 0xFC00, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xCB20, 0xFFE0, 0xFFE0, 0xFFE0, 0xFFE0, 0xCB20, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x8A20, 0xFFFF, 0xFFE0, 0xFC00, 0xFC00, 0xFC00, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xCB20, 0xFFE0, 0xFFE0, 0xCB20, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x8A20, 0xFFE0, 0xFC00, 0xFFE0, 0xFC00, 0xFFE0, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xCB20, 0xFFE0, 0xFFE0, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFE0, 0xFC00, 0x8A20, 0x8A20, 0xFC00, 0xFFFF, 0xFFFF, 0x0000, 0xCB20, 0xCB20, 0xFFE0, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
static const char     preset_3_name[]      = "";
static const uint16_t preset_3_pixels[] PROGMEM = { LABEL_EMPTY_256 };
static const char     preset_4_name[]      = "";
static const uint16_t preset_4_pixels[] PROGMEM = { LABEL_EMPTY_256 };
static const char     preset_5_name[]      = "";
static const uint16_t preset_5_pixels[] PROGMEM = { LABEL_EMPTY_256 };
static const char     preset_6_name[]      = "";
static const uint16_t preset_6_pixels[] PROGMEM = { LABEL_EMPTY_256 };
static const char     preset_7_name[]      = "";
static const uint16_t preset_7_pixels[] PROGMEM = { LABEL_EMPTY_256 };
static const char     preset_8_name[]      = "";
static const uint16_t preset_8_pixels[] PROGMEM = { LABEL_EMPTY_256 };

// Référence pour itération depuis le captive portal
struct PresetRef { const char* name; const uint16_t* pixels; };
static const PresetRef PRESETS[] = {
    {preset_1_name, preset_1_pixels},
    {preset_2_name, preset_2_pixels},
    {preset_3_name, preset_3_pixels},
    {preset_4_name, preset_4_pixels},
    {preset_5_name, preset_5_pixels},
    {preset_6_name, preset_6_pixels},
    {preset_7_name, preset_7_pixels},
    {preset_8_name, preset_8_pixels},
};
static const int N_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

// ── Chiffres custom 5×8 (designés dans sprite_editor.html) ───────────────────
// 8 octets = 8 lignes, MSB first, 5 bits utiles par octet (cols 0-4).
// Initialisés avec les glyphes AwtrixFont d'origine (3×5 décalés pour être
// centrés dans 5 cols, placés sur rows 1-5 = position firmware d'origine).
// Modifie ce que tu veux dans le sprite editor.
static const uint8_t digit_0[] PROGMEM = {0x00, 0x70, 0x50, 0x50, 0x50, 0x70, 0x00, 0x00};
static const uint8_t digit_1[] PROGMEM = { 0x00, 0x10, 0x30, 0x10, 0x10, 0x10, 0x00, 0x00 };
static const uint8_t digit_2[] PROGMEM = {0x00, 0x70, 0x10, 0x70, 0x40, 0x70, 0x00, 0x00};
static const uint8_t digit_3[] PROGMEM = {0x00, 0x70, 0x10, 0x70, 0x10, 0x70, 0x00, 0x00};
static const uint8_t digit_4[] PROGMEM = {0x00, 0x50, 0x50, 0x70, 0x10, 0x10, 0x00, 0x00};
static const uint8_t digit_5[] PROGMEM = {0x00, 0x70, 0x40, 0x70, 0x10, 0x70, 0x00, 0x00};
static const uint8_t digit_6[] PROGMEM = {0x00, 0x70, 0x40, 0x70, 0x50, 0x70, 0x00, 0x00};
static const uint8_t digit_7[] PROGMEM = {0x00, 0x70, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00};
static const uint8_t digit_8[] PROGMEM = {0x00, 0x70, 0x50, 0x70, 0x50, 0x70, 0x00, 0x00};
static const uint8_t digit_9[] PROGMEM = {0x00, 0x70, 0x50, 0x70, 0x10, 0x70, 0x00, 0x00};

static const uint8_t* const DIGITS[10] PROGMEM = {
    digit_0, digit_1, digit_2, digit_3, digit_4,
    digit_5, digit_6, digit_7, digit_8, digit_9
};

// Dexcom G6 envoie trend en mg/dL/min × 10 (signed). Mapping standard CGM,
// retourne bitmap monochrome + largeur (10 pour double, 5 sinon). Hauteur toujours 5.
static ArrowInfo trendArrow(int8_t t) {
    if (t > 30)   return {arrow_doubleUp,      10};
    if (t > 20)   return {arrow_singleUp,       5};
    if (t > 10)   return {arrow_fortyFiveUp,    5};
    if (t >= -10) return {arrow_flat,           5};
    if (t >= -20) return {arrow_fortyFiveDown,  5};
    if (t >= -30) return {arrow_singleDown,     5};
    return        {arrow_doubleDown,           10};
}

// Une fois qu'on a reçu une glycémie, les labels d'état ne doivent plus
// l'écraser : on garde la valeur lisible en permanence. (hasGlucose, lastGlucoseMgdl,
// lastGlucoseTrend, g_displayOff définis plus haut pour les handlers boutons.)
static bool hasReceivedReading = false;  // true dès le 1er onReading (valide ou capteur OFF)

// Lectures et labels d'état en attente de traitement par le main loop.
// Les callbacks BLE s'exécutent dans la BTC_TASK (core 0) ; display et network
// ne sont pas thread-safe depuis ce contexte → on diffère ici, loop() traite.
static volatile bool            g_pendingReady = false;
static DexcomReadingNB          g_pendingData;
static volatile const uint16_t* g_pendingLabel = nullptr;  // label d'état à afficher

// Dessine un sprite RGB565 multi-couleur 32×8 pixel par pixel.
// 0x0000 = pixel transparent (skip), sinon = couleur RGB565 du pixel.
static void drawColorSprite(int16_t x, int16_t y, const uint16_t* sprite, int w, int h) {
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            uint16_t color = pgm_read_word(&sprite[r * w + c]);
            if (color != 0) DisplayManager.drawPixel(x + c, y + r, color);
        }
    }
}

// Affichage label plein-écran via sprite RGB565 multi-couleur (32×8)
static void displayLabel(const uint16_t* sprite) {
    DisplayManager.clearMatrix();
    drawColorSprite(0, 0, sprite, 32, 8);
    DisplayManager.update();
}

// Vrai si le sprite N octets est entièrement à zéro
static bool spriteIsEmpty(const uint8_t* sprite, int n) {
    for (int i = 0; i < n; i++) if (pgm_read_byte(&sprite[i])) return false;
    return true;
}

// Affichage glycémie : custom digits 5×8 si tous designés, fallback AwtrixFont sinon
static void displayGlucose(uint16_t mgdl, int8_t trend) {
    DisplayManager.clearMatrix();

    char buf[6];
    snprintf(buf, sizeof(buf), "%u", mgdl);
    int nDigits = strlen(buf);
    // Couleur dépend de l'âge : >20 min → tout en gris fixe uniforme.
    uint32_t age_min = glucoseElapsedMs() / 60000UL;
    bool aged = (age_min >= 20);
    uint16_t color      = aged ? ageGreyFixed() : glucoseColorMgdl(mgdl);
    uint16_t arrowColor = aged ? ageGreyFixed() : COL_WHITE;

    // Vérifie si tous les digits utilisés dans cette valeur sont designés
    bool allCustom = true;
    for (int i = 0; i < nDigits; i++) {
        int d = buf[i] - '0';
        if (spriteIsEmpty(DIGITS[d], 8)) { allCustom = false; break; }
    }

    const int ARROW_GAP = 0;   // collé : padding du dernier digit donne déjà ~1 col visuel
    ArrowInfo arr      = trendArrow(trend);
    const int ARROW_W  = arr.width;   // 5 pour simple, 10 pour double

    if (allCustom) {
        // Custom digits 5×8 centrés + flèche.
        // GAP=-1 compense le padding latéral des digits (contenu 3-wide centré
        // dans sprite 5-wide → 1 col vide à gauche + 1 à droite). Avec GAP=-1 on
        // obtient 1 col visuel entre chiffres (= rendu Awtrix natif).
        const int DIGIT_W   = 5;
        const int DIGIT_GAP = -1;
        int textWidth  = nDigits * DIGIT_W + (nDigits - 1) * DIGIT_GAP;
        int totalWidth = textWidth + ARROW_GAP + ARROW_W;
        int startX     = (MATRIX_WIDTH - totalWidth) / 2;
        if (startX < 0) startX = 0;

        for (int i = 0; i < nDigits; i++) {
            int d = buf[i] - '0';
            DisplayManager.drawBitmap(startX + i * (DIGIT_W + DIGIT_GAP), 0,
                                       DIGITS[d], DIGIT_W, 8, color);
        }
        DisplayManager.drawBitmap(startX + textWidth + ARROW_GAP, 1,
                                   arr.bitmap, arr.width, 5, arrowColor);
    } else {
        // Fallback AwtrixFont (3×5) : aussi centré, même logique
        // Largeur visible : N × xAdvance(4) - 1 trailing = 4N - 1
        int textWidth  = 4 * nDigits - 1;
        int totalWidth = textWidth + ARROW_GAP + ARROW_W;
        int startX     = (MATRIX_WIDTH - totalWidth) / 2;
        if (startX < 0) startX = 0;

        DisplayManager.setTextColor(color);
        DisplayManager.printText(startX, 6, buf, TEXT_ALIGNMENT::LEFT, 1);
        DisplayManager.drawBitmap(startX + textWidth + ARROW_GAP, 1,
                                   arr.bitmap, arr.width, 5, arrowColor);
    }
    drawAgeBars();   // tirets row 6 (avant-dernière ligne) : 1 par 5 min écoulées
    DisplayManager.update();
}

// Indicateur d'âge : 5 tirets max sur la rangée 7 (dernière ligne), 1 tiret
// par tranche de 5 min écoulée. Plafond fixe à 5 tirets (au-delà de 25 min).
// Layout : tirets 3 pixels + 1 pixel d'espace = 4 px par bar.
//   5×4 - 1 trailing = 19 cols → centrage : (32-19)/2 = 6 → start x=6.
// Couleur gris foncé uniforme (#404040) — même teinte que les digits/flèche grisés.
static void drawAgeBars() {
#ifndef TEST_AGE_CYCLE
    if (!hasGlucose || lastGlucoseRxMs == 0) return;
#else
    if (!hasGlucose) return;
#endif
    uint32_t elapsed = glucoseElapsedMs();
    int bars = (int)(elapsed / (5UL * 60UL * 1000UL));
    if (bars > 5) bars = 5;  // plafond à 5 tirets fixes
    const uint16_t BAR_COLOR = ageGreyFixed();  // gris fixe (même que digits grisés)
    // Effacer row 7
    for (int x = 0; x < MATRIX_WIDTH; x++) DisplayManager.drawPixel(x, 7, COL_BLACK);
    // 5 tirets de 3 px + 1 px d'espace, centrés (start x=6, total 19 cols)
    for (int b = 0; b < bars; b++) {
        int x0 = 6 + b * 4;  // 3 px tiret + 1 px espace
        DisplayManager.drawPixel(x0,     7, BAR_COLOR);
        DisplayManager.drawPixel(x0 + 1, 7, BAR_COLOR);
        DisplayManager.drawPixel(x0 + 2, 7, BAR_COLOR);
    }
    g_lastBarsCount = bars;
}

// Re-render la dernière glycémie (utile après un check OTA qui a pu corrompre la matrice)
static void refreshGlucose() {
    if (hasGlucose) displayGlucose(lastGlucoseMgdl, lastGlucoseTrend);
}

// ── Log buffer → ntfy.sh ─────────────────────────────────────────────────────
// Tableaux de char fixes (pas de String) : pas d'allocation heap depuis BLE task.
static const uint8_t RLOG_CAP  = 50;
static const uint8_t RLOG_LINE = 110;
static char    rlogBuf[RLOG_CAP][RLOG_LINE];
static uint8_t rlogHead = 0, rlogCount = 0;

static const char* resetReasonStr() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "WDT_INT";
        case ESP_RST_TASK_WDT:  return "WDT_TASK";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        default:                return "OTHER";
    }
}

static void remoteLog(const String& msg) {
    Serial.println(msg);
    snprintf(rlogBuf[rlogHead], RLOG_LINE, "[%lus] %s", millis() / 1000, msg.c_str());
    rlogHead = (rlogHead + 1) % RLOG_CAP;
    if (rlogCount < RLOG_CAP) rlogCount++;
}

// Résout manuellement la redirection 302 GitHub → objects.githubusercontent.com (S3).
// httpUpdate.update() avec redirect-during-stream cassait Update.end() (Flash Read Failed),
// probablement à cause du re-handshake TLS qui fragmentait le heap pendant le download.
static String resolveRedirect(const String& url) {
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    const char* hdr[] = {"Location"};
    http.collectHeaders(hdr, 1);
    http.begin(client, url);
    int code = http.GET();
    String loc = (code == 301 || code == 302) ? http.header("Location") : url;
    http.end(); client.stop();
    return loc.length() ? loc : url;
}

// ── OTA boot check (heap propre, max mémoire dispo pour TLS) ─────────────────
static void bootOtaCheck() {
    Serial.println("[BOOT-OTA] Tentative OTA précoce (heap frais)...");
    Serial.printf("[BOOT-OTA] heap=%u largest=%u\n",
        (unsigned)ESP.getFreeHeap(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    WiFi.mode(WIFI_STA);
    WiFi.begin(DEV_WIFI_SSID, DEV_WIFI_PASS);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 8000) delay(200);
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[BOOT-OTA] WiFi indispo, skip → BLE");
        WiFi.mode(WIFI_OFF); delay(300); return;
    }
    configTime(0, 0, "pool.ntp.org");  // lance NTP sans attendre
    delay(300);

    // 1. Check version
    int sv = -1;
    {
        WiFiClientSecure client; client.setInsecure();
        HTTPClient http;
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.begin(client, String(DEV_OTA_BASE_URL) + "/version_ble.txt");
        int code = http.GET();
        if (code == 200) sv = http.getString().toInt();
        else Serial.printf("[BOOT-OTA] HTTP %d sur version_ble.txt\n", code);
        http.end();
        client.stop();
    }

    Serial.printf("[BOOT-OTA] serveur=%d local=%d\n", sv, FIRMWARE_VERSION);
    if (sv <= FIRMWARE_VERSION) {
        WiFi.mode(WIFI_OFF); delay(500); return;
    }

    // 2. Pré-résoudre le redirect : GitHub → URL S3 signée
    Serial.println("[BOOT-OTA] Résolution redirect GitHub...");
    String s3Url = resolveRedirect(String(DEV_OTA_BASE_URL) + "/firmware_ble.bin");
    Serial.printf("[BOOT-OTA] direct: %s\n", s3Url.c_str());
    delay(300);

    Serial.printf("[BOOT-OTA] heap=%u largest=%u (avant flash)\n",
        (unsigned)ESP.getFreeHeap(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    Serial.println("[BOOT-OTA] MAJ disponible → flash...");

    {
        WiFiClientSecure otaClient; otaClient.setInsecure();
        otaClient.setTimeout(30000);
        // Pas de redirect pendant le stream : URL S3 directe, pas de re-handshake TLS
        httpUpdate.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        auto ret = httpUpdate.update(otaClient, s3Url);
        if (ret == HTTP_UPDATE_OK) ESP.restart();
        Serial.printf("[BOOT-OTA] Echec %d : %s\n",
            httpUpdate.getLastError(),
            httpUpdate.getLastErrorString().c_str());
    }
    WiFi.mode(WIFI_OFF);
    delay(500);
}

// Le runtime OTA HTTPS a été retiré : il échoue systématiquement (mbedtls demande
// ~32 KB contigus alors que BLE+app fragmentent le heap à ~13 KB max).
// Le seul vrai chemin de MAJ en prod = bootOtaCheck() au démarrage (heap frais),
// déclenché soit par power-cycle, soit par le reboot programmé 24h ci-dessous.
// En dev, le poll ntfy.sh HTTP plain (devOtaPoll) prend le relais.

// ── DexcomBLENB ──────────────────────────────────────────────────────────────
static DexcomBLENB* dexcom       = nullptr;
static unsigned long lastCheckMs  = 0;
static const unsigned long REBOOT_INTERVAL_MS = 24UL * 60 * 60 * 1000;  // 24h

// ── Métriques fuite mémoire ─────────────────────────────────────────────────
// Tracking long-terme du heap pour identifier les fuites résiduelles.
// `lowMin` = pire largest jamais vu depuis boot (descend = fragmentation/leak).
static uint32_t g_heapLowFree    = 0xFFFFFFFF;  // pire free heap vu
static uint32_t g_heapLowLargest = 0xFFFFFFFF;  // pire largest contigu vu
static uint32_t g_bleCycles      = 0;           // onReading() = lectures réussies
static uint32_t g_bleAttempts    = 0;           // transitions vers WAIT_NEXT (succès+échecs)

// Snapshot heap (free + largest) pour mesurer les deltas autour d'opérations.
struct HeapSnap { uint32_t freeB; uint32_t largestB; };
static HeapSnap heapSnap() {
    return { (uint32_t)ESP.getFreeHeap(),
             (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) };
}
// Log delta autour d'une opération si significatif. Désactivé en mode prod
// (instrumentation v459-v467 a confirmé la fuite : POST body → heap leak proportionnel).
// Pour réactiver l'instrumentation : -DDEBUG_HEAP_DELTAS dans build_flags.
static void heapDelta(const char* op, const HeapSnap& before, const HeapSnap& after) {
#ifdef DEBUG_HEAP_DELTAS
    int32_t dFree = (int32_t)after.freeB    - (int32_t)before.freeB;
    int32_t dLarg = (int32_t)after.largestB - (int32_t)before.largestB;
    if (abs(dFree) < 256 && abs(dLarg) < 256) return;
    char buf[140];
    snprintf(buf, sizeof(buf),
        "[HEAP-Δ] %s free %d→%d (%+d) largest %d→%d (%+d)",
        op,
        (int)before.freeB,    (int)after.freeB,    (int)dFree,
        (int)before.largestB, (int)after.largestB, (int)dLarg);
    remoteLog(buf);
#else
    (void)op; (void)before; (void)after;
#endif
}

// Met à jour les low-water-marks. Désactivé en prod ; activable via -DDEBUG_HEAP_DELTAS.
static void heapTrack(const char* where) {
#ifdef DEBUG_HEAP_DELTAS
    static bool initDone = false;
    uint32_t freeNow    = ESP.getFreeHeap();
    uint32_t largestNow = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    if (!initDone) {
        g_heapLowFree    = freeNow;
        g_heapLowLargest = largestNow;
        initDone = true;
        return;
    }
    bool alarm = false;
    if (freeNow < g_heapLowFree) {
        if (g_heapLowFree - freeNow >= 1024) alarm = true;
        g_heapLowFree = freeNow;
    }
    if (largestNow < g_heapLowLargest) {
        if (g_heapLowLargest - largestNow >= 1024) alarm = true;
        g_heapLowLargest = largestNow;
    }
    if (alarm) {
        char buf[120];
        snprintf(buf, sizeof(buf), "[HEAP-MIN] %s free=%u(min=%u) largest=%u(min=%u)",
            where, (unsigned)freeNow, (unsigned)g_heapLowFree,
            (unsigned)largestNow, (unsigned)g_heapLowLargest);
        remoteLog(buf);
    }
#else
    (void)where;
#endif
}

// ── DEV OTA via ntfy.sh (HTTP plain, marche avec heap fragmenté) ─────────────
#ifdef DEV_OTA_ENABLE
static const unsigned long DEV_OTA_POLL_MS = 120UL * 1000;  // 2 min : limite rate-limit ntfy
static int  lastDevVersionSeen = 0;

// Back-off rate-limit : si ntfy nous renvoie 429, on suspend tous les POST
// pendant g_rateLimitUntil pour ne pas leak (chaque 429 fuite ~2-7KB).
static volatile uint32_t g_rateLimitUntil = 0;
static bool isRateLimited() { return millis() < g_rateLimitUntil; }
static void noteRateLimit429() {
    g_rateLimitUntil = millis() + 60UL * 1000;  // suspend 60s
    Serial.printf("[RATE-LIMIT] 429 detected, suspending POSTs for 60s\n");
}

// Flush les logs accumulés vers ntfy.sh en HTTP plain (DEV uniquement).
// Ne vide le buffer qu'après POST=200 → "partial WiFi" (associé sans internet)
// ne perd plus les logs : ils restent en buffer pour le prochain flush.
static void devFlushLogs() {
    if (rlogCount == 0 || WiFi.status() != WL_CONNECTED) return;
    if (isRateLimited()) return;  // ntfy nous a 429ed récemment
    HeapSnap _hb = heapSnap();

    // Snapshot : nouveaux logs ajoutés pendant le POST (BLE callback core 0)
    // resteront en buffer après consommation, ne sont pas envoyés ce coup-ci.
    uint8_t snapshotCount = rlogCount;
    uint8_t snapshotTail  = (rlogHead - snapshotCount + RLOG_CAP) % RLOG_CAP;

    String body;
    body.reserve((size_t)snapshotCount * (RLOG_LINE + 1));
    for (uint8_t i = 0; i < snapshotCount; i++) {
        body += rlogBuf[(snapshotTail + i) % RLOG_CAP];
        body += '\n';
    }

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(3000);
    http.begin(client, "http://ntfy.sh/" DEV_NTFY_TOPIC);
    http.addHeader("Title", "TC001-DEV v" + String(FIRMWARE_VERSION));
    http.addHeader("Content-Type", "text/plain; charset=utf-8");
    int code = http.POST(body);
    http.end();
    client.stop();  // force libération buffers TCP/lwIP même en cas d'erreur

    if (code != 200) {
        Serial.printf("[FLUSH] POST=%d, %u logs gardés en buffer\n", code, snapshotCount);
        if (code == 429) noteRateLimit429();
        heapTrack("flush-fail");
        heapDelta("flush-fail", _hb, heapSnap());
        return;  // partial WiFi / ntfy down → retry au prochain flush
    }

    // Succès : consommer snapshotCount entrées (les plus anciennes).
    // Les logs ajoutés pendant le POST sont préservés dans rlogCount résiduel.
    if (rlogCount >= snapshotCount) rlogCount -= snapshotCount;
    else                            rlogCount = 0;
    heapTrack("flush-ok");
    heapDelta("flush-ok", _hb, heapSnap());
}

// Parse stream ndjson de ntfy ligne par ligne (jamais tout en RAM, heap-safe)
// Trouve la dernière ligne contenant "name":"fw_NNN.bin", extrait version + URL.
static void devOtaPoll() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[DEV-OTA] WiFi déconnecté, skip");
        return;
    }
    if (isRateLimited()) return;  // attendre la fin du back-off avant de poll
    Serial.printf("[DEV-OTA] poll heap=%u largest=%u\n",
        (unsigned)ESP.getFreeHeap(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    HeapSnap _hb_poll = heapSnap();

    devFlushLogs();

    WiFiClient client;  // TCP plain — surtout PAS WiFiClientSecure
    HTTPClient http;
    http.setTimeout(8000);
    // since=12h : voit toute MAJ des dernières 12h (TTL ntfy free tier).
    // Topic partagé avec les logs → on parse en stream pour ne jamais charger
    // tout le body en RAM (peut faire >50 KB avec les logs accumulés).
    http.begin(client, "http://ntfy.sh/" DEV_NTFY_OTA_TOPIC "/json?poll=1&since=12h");
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[DEV-OTA] ntfy HTTP %d\n", code);
        http.end();
        return;
    }

    WiFiClient* stream = http.getStreamPtr();
    int latestVer = 0;
    String latestUrl;
    String line;
    line.reserve(800);
    unsigned long deadline = millis() + 8000;

    while ((stream->connected() || stream->available()) && millis() < deadline) {
        if (!stream->available()) { delay(5); continue; }
        char c = stream->read();
        if (c != '\n') { line += c; if (line.length() < 4096) continue; }

        // Ligne complète : ne traite que celles qui contiennent une attachment fw_
        int idx = line.indexOf("\"name\":\"fw_");
        if (idx >= 0) {
            int verStart = idx + 11;
            int verEnd = line.indexOf(".bin", verStart);
            if (verEnd > 0) {
                int v = line.substring(verStart, verEnd).toInt();
                if (v > latestVer) {
                    int urlStart = line.indexOf("\"url\":\"");
                    if (urlStart >= 0) {
                        urlStart += 7;
                        int urlEnd = line.indexOf("\"", urlStart);
                        if (urlEnd > urlStart) {
                            latestVer = v;
                            latestUrl = line.substring(urlStart, urlEnd);
                        }
                    }
                }
            }
        }
        line = "";
    }
    if (code == 429) noteRateLimit429();
    http.end();
    client.stop();
    heapDelta("ota-poll", _hb_poll, heapSnap());

    if (latestVer == 0) return;  // aucune MAJ vue
    if (latestVer == lastDevVersionSeen) return;  // déjà tenté ce cycle
    lastDevVersionSeen = latestVer;
    if (latestVer <= FIRMWARE_VERSION) {
        Serial.printf("[DEV-OTA] dispo=%d local=%d, rien à faire\n", latestVer, FIRMWARE_VERSION);
        return;
    }

    // NVS : skip si ce numéro de version a déjà échoué (binaire ntfy expiré).
    // Évite la boot-loop infinie (chaque reboot retente, expire → reboot → ...).
    // La version échouée est effacée quand une version supérieure apparaît.
    {
        Preferences prefs;
        prefs.begin("ota_dev", true);
        int failedVer = prefs.getInt("failed_ver", 0);
        prefs.end();
        if (failedVer >= latestVer) {
            Serial.printf("[DEV-OTA] v%d précédemment échoué (NVS), skip\n", latestVer);
            return;
        }
    }

    latestUrl.replace("\\/", "/");
    latestUrl.replace("https://", "http://");

    char dbg[140];
    snprintf(dbg, sizeof(dbg), "[DEV-OTA] MAJ %d → %d, démarrage download", FIRMWARE_VERSION, latestVer);
    remoteLog(dbg);

    // Affiche "MAJ" cyan plein-écran pendant le download
    DisplayManager.clearMatrix();
    DisplayManager.setTextColor(0x07FF);
    DisplayManager.printText(0, 6, "MAJ", TEXT_ALIGNMENT::CENTER, 1);
    DisplayManager.update();

    // CRUCIAL : libère ~150 KB en désactivant complètement BLE Bluedroid.
    // Sans ça, Update.begin() malloc échoue (heap fragmenté à ~9 KB largest).
    if (dexcom) dexcom->stopScan();
    http.end();
    client.stop();
    delay(200);
    // NimBLE consomme déjà peu (~100KB libres au boot), pas besoin de deinit.
    // NimBLEDevice::deinit(true) crash avec "assert failed: npl_freertos_mutex_pend"
    // (bug NimBLE-Arduino 1.4.x) → on garde juste le stopScan() au-dessus.
    delay(500);

    snprintf(dbg, sizeof(dbg), "[DEV-OTA] heap après BLE deinit: %u/%u",
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
        (unsigned)ESP.getFreeHeap());
    remoteLog(dbg);
    devFlushLogs();

    // Progression du download (0/25/50/75/100%)
    Update.onProgress([](unsigned int cur, unsigned int total) {
        static int lastQ = -1;
        if (total == 0) return;
        int q = (cur * 100 / total) / 25;
        if (q != lastQ) {
            lastQ = q;
            char b[80];
            snprintf(b, sizeof(b), "[DEV-OTA] DL %d%% (%u/%u octets)", q*25, cur, total);
            remoteLog(b);
        }
    });

    WiFiClient otaClient;
    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    auto ret = httpUpdate.update(otaClient, latestUrl);
    if (ret == HTTP_UPDATE_OK) {
        // Succès : effacer NVS pour ne pas bloquer une future version identique
        { Preferences prefs; prefs.begin("ota_dev", false); prefs.remove("failed_ver"); prefs.end(); }
        remoteLog("[DEV-OTA] ✓ flash OK → reboot");
        devFlushLogs();
        ESP.restart();
    }
    // Échec : mémoriser la version en NVS pour éviter la boot-loop (binaire expiré ntfy).
    // BLE est deinit → reboot nécessaire pour récupérer. Au prochain boot, NVS bloquera
    // ce numéro de version ; seule une version supérieure relancera l'OTA.
    { Preferences prefs; prefs.begin("ota_dev", false); prefs.putInt("failed_ver", latestVer); prefs.end(); }
    snprintf(dbg, sizeof(dbg), "[DEV-OTA] ❌ ÉCHEC code=%d : %s → reboot (v%d bloqué NVS)",
        httpUpdate.getLastError(),
        httpUpdate.getLastErrorString().c_str(),
        latestVer);
    remoteLog(dbg);
    devFlushLogs();
    delay(1000);
    ESP.restart();
}
#endif

// Queue glucose : garde les dernières lectures si WiFi/ntfy down.
// 5 entrées = 25 min de tampon (lectures Dexcom toutes les 5 min) avant qu'on
// commence à drop les plus anciennes. Suffit pour la plupart des coupures WiFi.
#ifdef DEV_NTFY_DATA_TOPIC
struct PendingGluc { DexcomReadingNB reading; time_t ts; };
static const uint8_t PEND_GLUC_CAP = 5;
static PendingGluc pendGluc[PEND_GLUC_CAP];
static uint8_t     pendGlucCount = 0;
static uint8_t     pendGlucHead  = 0;

static void flushPendingGlucose() {
    if (pendGlucCount == 0 || WiFi.status() != WL_CONNECTED) return;
    if (isRateLimited()) return;
    HeapSnap _hb = heapSnap();

    while (pendGlucCount > 0) {
        uint8_t tail = (pendGlucHead - pendGlucCount + PEND_GLUC_CAP) % PEND_GLUC_CAP;
        const PendingGluc& pg = pendGluc[tail];
        const DexcomReadingNB& r = pg.reading;

        char body[200];
        snprintf(body, sizeof(body),
            "{\"mgdl\":%u,\"mmol\":%.2f,\"trend\":%d,\"sensorOk\":%s,\"tx\":\"%s\",\"fw\":%d,\"ts\":%lu}",
            r.mgdl, r.mmol, (int)r.trend,
            r.sensorOk ? "true" : "false",
            DEV_TRANSMITTER_ID, FIRMWARE_VERSION,
            (unsigned long)pg.ts);

        WiFiClient client;
        HTTPClient http;
        http.setTimeout(3000);
        http.begin(client, "http://ntfy.sh/" DEV_NTFY_DATA_TOPIC);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Title", "glucose");
        http.addHeader("Tags", "glucose");
        int code = http.POST(body);
        http.end();
        client.stop();

        if (code != 200) {
            char dbg[80];
            snprintf(dbg, sizeof(dbg), "[PUB] POST=%d, %u en queue", code, pendGlucCount);
            remoteLog(dbg);
            if (code == 429) noteRateLimit429();
            heapTrack("pub-fail");
            heapDelta("pub-fail", _hb, heapSnap());
            return;  // garde le reste de la queue pour retry
        }
        pendGlucCount--;
    }
    heapTrack("pub-ok");
    heapDelta("pub-ok", _hb, heapSnap());
}
#endif

// Publie une lecture sur ntfy.sh (topic data) — appelé après chaque onReading.
// Capture le timestamp à la lecture, enqueue, puis flush ce qu'on peut.
// Si WiFi/ntfy down : la lecture reste en queue, retry au prochain flush.
static void publishGlucose(const DexcomReadingNB& r) {
#ifdef DEV_NTFY_DATA_TOPIC
    time_t ts = time(NULL);
    pendGluc[pendGlucHead] = { r, ts > 1000000L ? ts : 0 };
    pendGlucHead = (pendGlucHead + 1) % PEND_GLUC_CAP;
    if (pendGlucCount < PEND_GLUC_CAP) pendGlucCount++;
    // sinon overflow : la plus ancienne est écrasée (FIFO drop)
    flushPendingGlucose();
#endif
}

// Callback BLE (BTC_TASK, core 0) : log uniquement, pas de display ni network.
// Tout le traitement display/network est différé au main loop via g_pending.
static void onReading(const DexcomReadingNB& r) {
    hasReceivedReading = true;
    g_bleCycles++;
    if (!r.sensorOk) {
        remoteLog("[GLUC] Capteur hors du corps");
    } else {
        char buf[80];
        snprintf(buf, sizeof(buf), "[GLUC] %u mg/dL (%.1f mmol) trend=%d",
                 r.mgdl, r.mmol, r.trend);
        remoteLog(buf);
    }
    g_pendingData  = r;
    g_pendingReady = true;  // signal au loop() — positionné en dernier
}

// ── setup / loop ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    pinMode(15, OUTPUT); digitalWrite(15, LOW);  // buzzer mute

    // DisplayManager init (NeoMatrix + Adafruit GFX)
    DisplayManager.setup();
    DisplayManager.clearMatrix();
    DisplayManager.update();

    // Boutons : LEFT/RIGHT contrôlent la luminosité, SELECT inactif pour l'instant.
    // Charge la dernière luminosité depuis NVS et l'applique avant tout affichage utile.
    loadBrightness();
    applyBrightness();

    // Indicateur de boot : 3 points cyan "..." pendant que WiFi/BLE s'initialisent.
    // Remplacés automatiquement par les labels SCAN/AUTH/etc dès que le BLE émet
    // un event, puis par la glycémie à la 1ère lecture.
    DisplayManager.clearMatrix();
    DisplayManager.setTextColor(0x867D);  // sky blue ~ #87CEEB
    DisplayManager.printText(0, 6, "...", TEXT_ALIGNMENT::CENTER, 1);
    DisplayManager.update();
    // Détection via interruptions GPIO : déclenchée au niveau hardware sur front
    // descendant (bouton appuyé → pin LOW). Insensible au scheduling tasks.
    pinMode(BTN_LEFT_PIN, INPUT_PULLUP);
    pinMode(BTN_RIGHT_PIN, INPUT_PULLUP);
    pinMode(BTN_SELECT_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BTN_LEFT_PIN),   isrBtnLeft,   FALLING);
    attachInterrupt(digitalPinToInterrupt(BTN_RIGHT_PIN),  isrBtnRight,  FALLING);
    // SELECT en CHANGE pour mesurer durée press→release (long vs short)
    attachInterrupt(digitalPinToInterrupt(BTN_SELECT_PIN), isrBtnSelect, CHANGE);
    // Task qui process les flags d'ISR : prio 10 sur core 1 → préempte main loop
    // bloqué dans HTTP, garantit la réactivité du visual feedback.
    xTaskCreatePinnedToCore(buttonProcessTask, "btnProc", 4096, nullptr, 10, nullptr, 1);

#ifdef DEV_OTA_ENABLE
    // DEV : WiFi STA permanent (coex avec BLE), poll ntfy au boot
    Serial.println("[DEV] WiFi STA permanent...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(DEV_WIFI_SSID, DEV_WIFI_PASS);
    {
        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(200);
    }
    Serial.printf("[DEV] WiFi: %s\n",
        WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString().c_str() : "FAIL");
    if (WiFi.status() == WL_CONNECTED) {
        configTime(0, 0, "pool.ntp.org");  // lance NTP sans attendre
        devOtaPoll();
    }
#else
    bootOtaCheck();
#endif

    remoteLog("[BOOT] TC001-PROD fw v" + String(FIRMWARE_VERSION) + " Bluedroid tx=" DEV_TRANSMITTER_ID " reset=" + resetReasonStr());
    // (label_boot supprimé — le splash "Sugarboard" remplace, l'écran reste vide
    // jusqu'au 1er label d'état BLE ou à la 1ère lecture)
#ifdef DEV_OTA_ENABLE
    devFlushLogs();  // envoie [BOOT] immédiatement — si BLE init crash ensuite, on le sait
#endif

    dexcom = new DexcomBLENB(DEV_TRANSMITTER_ID);
    dexcom->begin(
        onReading,
        [](const String& s) {
            remoteLog("[BLE] " + s);
            // Labels d'état uniquement avant la toute première lecture (debug branchement).
            // hasReceivedReading reste true même si capteur OFF → évite d'écraser label_off.
            if (hasReceivedReading) return;

            // Pose un pointeur sur le sprite voulu — le loop() appelle displayLabel() (thread-safe)
            if      (s.startsWith("SCAN"))      g_pendingLabel = label_scan;
            else if (s.startsWith("CONNECT"))   g_pendingLabel = label_found;
            else if (s.startsWith("CONN ") ||
                     s.startsWith("CONNECTED")) g_pendingLabel = label_conn;
            else if (s.startsWith("AUTH"))      g_pendingLabel = label_auth;
            else if (s.startsWith("BOND"))      g_pendingLabel = label_bond;
            else if (s.startsWith("TIME"))      g_pendingLabel = label_time;
            else if (s.startsWith("ATTENTE"))   g_pendingLabel = label_wait;
            else if (s.startsWith("TIMEOUT"))   g_pendingLabel = label_err;
        }
    );
}

void loop() {
    // getStateNameC() retourne un const char* vers un string literal statique — pas d'allocation heap.
    // Log heap=free/largest à chaque transition pour pinpointer la fragmentation par cycle BLE.
    static const char* lastState = "";
    static HeapSnap    cycleStartSnap = {0, 0};   // snap au début d'un cycle BLE
    const char* cur = dexcom->getStateNameC();
    if (cur != lastState) {
#ifdef DEBUG_HEAP_DELTAS
        // En mode debug : log heap à chaque transition pour pinpoint exact
        char st[110];
        snprintf(st, sizeof(st), "[STATE] %s → %s heap=%u/%u",
            lastState, cur,
            (unsigned)ESP.getFreeHeap(),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        remoteLog(st);
        // Mesure delta heap d'un cycle BLE complet (CONNECTING → WAIT_NEXT)
        if (strcmp(cur, "CONNECTING") == 0) cycleStartSnap = heapSnap();
        else if (strcmp(cur, "WAIT_NEXT") == 0 && cycleStartSnap.freeB > 0) {
            heapDelta("ble-cycle", cycleStartSnap, heapSnap());
            cycleStartSnap = {0, 0};
        }
#else
        char st[60];
        snprintf(st, sizeof(st), "[STATE] %s → %s", lastState, cur);
        remoteLog(st);
#endif
        if (strcmp(cur, "WAIT_NEXT") == 0) g_bleAttempts++;
        heapTrack(cur);
        lastState = cur;
    }

    unsigned long now = millis();

    // Label d'état BLE différé (depuis BTC_TASK core 0 → traité ici sur core 1)
    const uint16_t* lbl = const_cast<const uint16_t*>(g_pendingLabel);
    if (lbl) {
        g_pendingLabel = nullptr;
        if (!g_displayOff) displayLabel(lbl);
    }

    // Traitement lecture BLE différé (display + network depuis main loop, thread-safe)
    if (g_pendingReady) {
        g_pendingReady = false;
        DexcomReadingNB r = g_pendingData;
        if (!r.sensorOk) {
            hasGlucose = false;
            if (!g_displayOff) displayLabel(label_off);
        } else {
            hasGlucose       = true;
            lastGlucoseMgdl  = r.mgdl;
            lastGlucoseTrend = r.trend;
            lastGlucoseRxMs  = millis();   // reset timer âge → 0 tiret au redraw
            g_lastBarsCount  = 0;
            if (!g_displayOff) displayGlucose(r.mgdl, r.trend);
        }
        publishGlucose(r);  // continue de publier même écran OFF
    }

    // Reboot programmé toutes les 24h pour rafraîchir le heap + check OTA au boot
    if (now > REBOOT_INTERVAL_MS) {
        Serial.println("[REBOOT] 24h écoulées → reboot pour refresh heap + OTA check");
        delay(500);
        ESP.restart();
    }

    // Watchdog heap : si le heap descend critiquement bas, reboot avant le crash
    // (15 KB = seuil sécurité avant que mbedtls/BLE allocations échouent)
    if (ESP.getFreeHeap() < 15000) {
        char wd[80];
        snprintf(wd, sizeof(wd), "[WATCHDOG] heap=%u < 15000 → reboot",
            (unsigned)ESP.getFreeHeap());
        remoteLog(wd);
#ifdef DEV_OTA_ENABLE
        devFlushLogs();
#endif
        delay(500);
        ESP.restart();
    }

    // Watchdog "pas de lecture" : si on en a déjà reçu mais plus rien depuis 30 min
    // (6 lectures attendues manquées = BLE stack figé), reboot.
    // PAS de reboot si jamais connecté — un Dexcom légitimement absent ne doit pas faire rebooter.
    unsigned long lastRx = dexcom->lastReadingMs();
    if (lastRx > 0 && (now - lastRx) > (30UL * 60 * 1000)) {
        remoteLog("[WATCHDOG] Aucune lecture depuis 30 min → reboot");
#ifdef DEV_OTA_ENABLE
        devFlushLogs();
#endif
        delay(500);
        ESP.restart();
    }

#ifdef DEV_OTA_ENABLE
    // Surveillance WiFi : log les transitions, flush immédiat au retour, reconnect actif si déco
    {
        static bool wifiWasUp = true;
        bool wifiUp = (WiFi.status() == WL_CONNECTED);
        if (wifiWasUp && !wifiUp) {
            remoteLog("[WIFI] Déconnecté");
            wifiWasUp = false;
        } else if (!wifiWasUp && wifiUp) {
            remoteLog("[WIFI] Reconnecté");
            wifiWasUp = true;
            devFlushLogs();          // envoie immédiatement les logs accumulés
#ifdef DEV_NTFY_DATA_TOPIC
            flushPendingGlucose();   // et les data glucose en attente
#endif
        } else if (!wifiUp) {
            static uint32_t lastReconnectMs = 0;
            if (now - lastReconnectMs > 30000) {
                lastReconnectMs = now;
                WiFi.reconnect();
            }
        }
    }

    // Heartbeat toutes les 2 min — confirme que le firmware est actif.
    static uint32_t lastHeartbeatMs = 0;
    if (now - lastHeartbeatMs > 2UL * 60 * 1000) {
        lastHeartbeatMs = now;
        char hb[140];
        snprintf(hb, sizeof(hb),
            "[HB] up=%lus state=%s last=%umg heap=%u/%u cyc=%u/%u wifi=%s",
            now / 1000, dexcom->getStateNameC(),
            lastGlucoseMgdl,
            (unsigned)ESP.getFreeHeap(),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
            (unsigned)g_bleCycles, (unsigned)g_bleAttempts,
            WiFi.status() == WL_CONNECTED ? "OK" : "DOWN");
        remoteLog(hb);
    }
    // Flush logs toutes les 30s quelle que soit l'état BLE ; OTA poll en plus si WAIT_NEXT
    if (now - lastCheckMs > DEV_OTA_POLL_MS) {
        lastCheckMs = now;
#ifdef DEV_NTFY_DATA_TOPIC
        flushPendingGlucose();   // retry des data glucose en attente
#endif
        if (dexcom->isWaiting()) {
            devOtaPoll();        // appelle devFlushLogs() en interne
            refreshGlucose();
        } else {
            devFlushLogs();      // flush pendant SCAN / AUTH / CONNECT aussi
        }
    }
#endif
    // En prod : pas de runtime OTA (HTTPS impossible avec heap fragmenté).
    // Le reboot 24h ci-dessus déclenche un bootOtaCheck propre.

    // Boutons : ISR détecte + buttonProcessTask exécute (core 1, prio 10).
    // setBrightness() applique directement, pas de feedback texte ni restauration.

    // Indicateur d'âge : check toutes les 30s. Ajoute un tiret quand 5 min de
    // plus se sont écoulées. À 20 min on déclenche un re-render full pour passer
    // la valeur en gris.
    static uint32_t lastBarCheckMs = 0;
    if (hasGlucose && !g_displayOff && lastGlucoseRxMs > 0
        && (now - lastBarCheckMs > 30000)) {
        lastBarCheckMs = now;
        uint32_t age_min = (now - lastGlucoseRxMs) / 60000UL;
        int bars = (int)(age_min / 5);
        if (bars > 8) bars = 8;
        // Bascule en gris à 20 min : full redraw pour changer la couleur du nombre
        if (age_min == 20 && g_lastBarsCount < 4) {
            displayGlucose(lastGlucoseMgdl, lastGlucoseTrend);  // recolore + re-tirets
        } else if (bars != g_lastBarsCount) {
            // Juste un tiret de plus à dessiner sur row 6
            drawAgeBars();
            DisplayManager.update();
        }
    }

#ifdef TEST_AGE_CYCLE
    // Mode TEST : cycle 7 âges simulés toutes les 2s (0,5,10,15,20,25,30 min)
    // → permet de voir 0,1,2,3,4,5 tirets + le plafond fixe à 30 min.
    // Bascule grise visible à partir de 20 min.
    static uint32_t lastTestMs = 0;
    static int testPhase = 0;
    if (!g_displayOff && (now - lastTestMs > 2000)) {
        lastTestMs = now;
        g_simAgeOffsetMs = (uint32_t)testPhase * 5UL * 60UL * 1000UL;  // 0..30 min
        testPhase = (testPhase + 1) % 7;
        if (!hasGlucose) {
            hasGlucose      = true;
            lastGlucoseMgdl = 142;
            lastGlucoseTrend = 0;
        }
        displayGlucose(lastGlucoseMgdl, lastGlucoseTrend);
    }
#endif

    DisplayManager.tick();
    dexcom->tick();
    delay(10);
}
