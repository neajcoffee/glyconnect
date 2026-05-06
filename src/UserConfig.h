#ifndef USERCONFIG_H
#define USERCONFIG_H

#include <Arduino.h>

// Config utilisateur stockée en NVS (Preferences) et configurable via captive portal.
// Si DEV_* sont définis à la compilation, ils servent de fallback (mode dev).
struct UserConfig {
    String wifiSsid;        // SSID WiFi
    String wifiPass;        // password WiFi
    String txId;            // Transmitter ID Dexcom (6 chars)
    String ntfyLogTopic;    // ntfy.sh topic pour logs distants
    String ntfyOtaTopic;    // ntfy.sh topic pour OTA dev
    String otaBaseUrl;      // base URL GitHub releases pour boot OTA

    // ── Décoration optionnelle affichée quand glycémie en cible ──
    bool     decorEnabled;          // toggle on/off
    uint16_t decorPixels[256];      // 32×8 RGB565 row-major (512 octets)

    // True si on a au moins SSID + TX ID (le minimum pour booter)
    bool valid() const { return !wifiSsid.isEmpty() && !txId.isEmpty(); }
};

// Charge la config depuis NVS, avec fallback sur les DEV_* définis à la compilation
UserConfig loadUserConfig();

// Sauvegarde la config en NVS (appelé par le captive portal après formulaire)
void saveUserConfig(const UserConfig& c);

// Efface la config NVS (geste de reset)
void clearUserConfig();

// Lance le captive portal. Bloque jusqu'à ce que l'utilisateur sauvegarde
// via le formulaire web → reboot automatique. Ne retourne jamais.
void startCaptivePortal();

#endif
