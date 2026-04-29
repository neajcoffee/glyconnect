#pragma once
#include <Arduino.h>

/**
 * Portail captif pour la saisie du serial transmetteur G6.
 * Lance un AP Wi-Fi "GlucoseClock-XXXX" et sert une page HTML
 * sur 192.168.4.1 jusqu'à ce que le serial soit saisi et validé.
 */
namespace GlucoseCaptivePortal {
    void start();   // bloquant jusqu'à soumission valide
}
