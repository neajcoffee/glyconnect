#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <esp_log.h>
#include <esp_system.h>
#include <time.h>
#include <Preferences.h>
#include "DisplayManager.h"
#include "DexcomBLE_BD.h"
#include <BLEDevice.h>

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
// l'écraser : on garde la valeur lisible en permanence.
static bool     hasGlucose         = false;
static bool     hasReceivedReading = false;  // true dès le 1er onReading (valide ou capteur OFF)
static uint16_t lastGlucoseMgdl   = 0;
static int8_t   lastGlucoseTrend  = 0;
static uint32_t lastGlucoseRxMs   = 0;  // timestamp réception (pour barre de progression)

// Lectures et labels d'état en attente de traitement par le main loop.
// Les callbacks BLE s'exécutent dans la BTC_TASK (core 0) ; display et network
// ne sont pas thread-safe depuis ce contexte → on diffère ici, loop() traite.
static volatile bool            g_pendingReady = false;
static DexcomReadingBD          g_pendingData;
static volatile const uint16_t* g_pendingLabel = nullptr;  // label d'état à afficher

// Intervalle attendu entre 2 lectures Dexcom (5 min)
static const uint32_t READING_INTERVAL_MS = 5UL * 60UL * 1000UL;

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
    uint16_t color = glucoseColorMgdl(mgdl);

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
                                   arr.bitmap, arr.width, 5, COL_WHITE);
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
                                   arr.bitmap, arr.width, 5, COL_WHITE);
    }
    DisplayManager.update();
}

// Re-render la dernière glycémie (utile après un check OTA qui a pu corrompre la matrice)
static void refreshGlucose() {
    if (hasGlucose) displayGlucose(lastGlucoseMgdl, lastGlucoseTrend);
}

// Barre de progression sur la rangée 7 : remplit en 5 min jusqu'au prochain scan attendu.
// Couleur grise discrète. Si la lecture est en retard (> 5 min), le dernier pixel clignote.
static void drawLoadingBar() {
    if (!hasGlucose) return;

    const uint16_t COL_BAR_DIM   = 0x4208;  // gris (RGB565 ~ #404040) — discret mais visible
    const uint16_t COL_BAR_BLINK = 0x8410;  // gris plus clair (clignotement)

    uint32_t now     = millis();
    uint32_t elapsed = now - lastGlucoseRxMs;
    bool     overdue = elapsed >= READING_INTERVAL_MS;

    // Efface la rangée 7
    for (int x = 0; x < MATRIX_WIDTH; x++) DisplayManager.drawPixel(x, 7, COL_BLACK);

    if (overdue) {
        // Barre pleine + dernier pixel clignote (500ms ON / 500ms OFF)
        for (int x = 0; x < MATRIX_WIDTH - 1; x++) DisplayManager.drawPixel(x, 7, COL_BAR_DIM);
        bool blinkOn = ((now / 500) % 2) == 0;
        if (blinkOn) DisplayManager.drawPixel(MATRIX_WIDTH - 1, 7, COL_BAR_BLINK);
    } else {
        uint8_t filled = (uint8_t)((uint64_t)elapsed * MATRIX_WIDTH / READING_INTERVAL_MS);
        for (int x = 0; x < filled; x++) DisplayManager.drawPixel(x, 7, COL_BAR_DIM);
    }
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

// ── DexcomBLEBD ──────────────────────────────────────────────────────────────
static DexcomBLEBD* dexcom       = nullptr;
static unsigned long lastCheckMs  = 0;
static const unsigned long REBOOT_INTERVAL_MS = 24UL * 60 * 60 * 1000;  // 24h

// ── DEV OTA via ntfy.sh (HTTP plain, marche avec heap fragmenté) ─────────────
#ifdef DEV_OTA_ENABLE
static const unsigned long DEV_OTA_POLL_MS = 30UL * 1000;  // 30s en dev
static int  lastDevVersionSeen = 0;

// Flush les logs accumulés vers ntfy.sh en HTTP plain (DEV uniquement).
// Ne vide le buffer qu'après POST=200 → "partial WiFi" (associé sans internet)
// ne perd plus les logs : ils restent en buffer pour le prochain flush.
static void devFlushLogs() {
    if (rlogCount == 0 || WiFi.status() != WL_CONNECTED) return;

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

    if (code != 200) {
        Serial.printf("[FLUSH] POST=%d, %u logs gardés en buffer\n", code, snapshotCount);
        return;  // partial WiFi / ntfy down → retry au prochain flush
    }

    // Succès : consommer snapshotCount entrées (les plus anciennes).
    // Les logs ajoutés pendant le POST sont préservés dans rlogCount résiduel.
    if (rlogCount >= snapshotCount) rlogCount -= snapshotCount;
    else                            rlogCount = 0;
}

// Parse stream ndjson de ntfy ligne par ligne (jamais tout en RAM, heap-safe)
// Trouve la dernière ligne contenant "name":"fw_NNN.bin", extrait version + URL.
static void devOtaPoll() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[DEV-OTA] WiFi déconnecté, skip");
        return;
    }
    Serial.printf("[DEV-OTA] poll heap=%u largest=%u\n",
        (unsigned)ESP.getFreeHeap(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

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
    http.end();

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
    BLEDevice::deinit(true);
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
struct PendingGluc { DexcomReadingBD reading; time_t ts; };
static const uint8_t PEND_GLUC_CAP = 5;
static PendingGluc pendGluc[PEND_GLUC_CAP];
static uint8_t     pendGlucCount = 0;
static uint8_t     pendGlucHead  = 0;

static void flushPendingGlucose() {
    if (pendGlucCount == 0 || WiFi.status() != WL_CONNECTED) return;

    while (pendGlucCount > 0) {
        uint8_t tail = (pendGlucHead - pendGlucCount + PEND_GLUC_CAP) % PEND_GLUC_CAP;
        const PendingGluc& pg = pendGluc[tail];
        const DexcomReadingBD& r = pg.reading;

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

        if (code != 200) {
            char dbg[80];
            snprintf(dbg, sizeof(dbg), "[PUB] POST=%d, %u en queue", code, pendGlucCount);
            remoteLog(dbg);
            return;  // garde le reste de la queue pour retry
        }
        pendGlucCount--;
    }
}
#endif

// Publie une lecture sur ntfy.sh (topic data) — appelé après chaque onReading.
// Capture le timestamp à la lecture, enqueue, puis flush ce qu'on peut.
// Si WiFi/ntfy down : la lecture reste en queue, retry au prochain flush.
static void publishGlucose(const DexcomReadingBD& r) {
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
static void onReading(const DexcomReadingBD& r) {
    hasReceivedReading = true;
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
    displayLabel(label_boot);
#ifdef DEV_OTA_ENABLE
    devFlushLogs();  // envoie [BOOT] immédiatement — si BLE init crash ensuite, on le sait
#endif

    dexcom = new DexcomBLEBD(DEV_TRANSMITTER_ID);
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
    static const char* lastState = "";
    const char* cur = dexcom->getStateNameC();
    if (cur != lastState) {
        remoteLog(String("[STATE] ") + lastState + " → " + cur);
        lastState = cur;
    }

    unsigned long now = millis();

    // Label d'état BLE différé (depuis BTC_TASK core 0 → traité ici sur core 1)
    const uint16_t* lbl = const_cast<const uint16_t*>(g_pendingLabel);
    if (lbl) {
        g_pendingLabel = nullptr;
        displayLabel(lbl);
    }

    // Traitement lecture BLE différé (display + network depuis main loop, thread-safe)
    if (g_pendingReady) {
        g_pendingReady = false;
        DexcomReadingBD r = g_pendingData;
        if (!r.sensorOk) {
            hasGlucose = false;
            displayLabel(label_off);
        } else {
            hasGlucose       = true;
            lastGlucoseMgdl  = r.mgdl;
            lastGlucoseTrend = r.trend;
            lastGlucoseRxMs  = millis();
            displayGlucose(r.mgdl, r.trend);
            drawLoadingBar();
            DisplayManager.update();
        }
        publishGlucose(r);
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

    // Heartbeat toutes les 2 min — confirme que le firmware est actif même sans lecture BLE
    // heap=free/largest : si largest descend bien sous free, fragmentation à surveiller
    static uint32_t lastHeartbeatMs = 0;
    if (now - lastHeartbeatMs > 2UL * 60 * 1000) {
        lastHeartbeatMs = now;
        char hb[140];
        snprintf(hb, sizeof(hb), "[HB] up=%lus state=%s last=%umg heap=%u/%u wifi=%s",
            now / 1000, dexcom->getStateNameC(),
            lastGlucoseMgdl,
            (unsigned)ESP.getFreeHeap(),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
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

    // Rafraîchit la barre de progression toutes les 500ms (progression + clignotement)
    static uint32_t lastBarMs = 0;
    if (hasGlucose && (now - lastBarMs > 500)) {
        lastBarMs = now;
        drawLoadingBar();
        DisplayManager.update();
    }

    DisplayManager.tick();
    dexcom->tick();
    delay(10);
}
