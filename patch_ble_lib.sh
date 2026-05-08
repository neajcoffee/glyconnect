#!/bin/bash
# Patch idempotent : rend BLEClient::clearServices() publique dans le framework
# arduino-esp32 BLE Bluedroid stock. Sans ce patch, m_servicesMap fuit ~1-3KB
# par cycle BLE → watchdog/PANIC après ~1h.
#
# À ré-exécuter après un `pio pkg update` ou un nettoyage de ~/.platformio.
# Idempotent : safe à lancer plusieurs fois, ne refait rien si déjà patché.
set -e

LIB="$HOME/.platformio/packages/framework-arduinoespressif32/libraries/BLE/src/BLEClient.h"
if [ ! -f "$LIB" ]; then
    echo "ERREUR: $LIB introuvable. Compile d'abord avec PlatformIO pour installer le framework."
    exit 1
fi

if grep -q "PATCHED (glyconnect)" "$LIB"; then
    echo "✓ BLEClient.h déjà patché, rien à faire"
    exit 0
fi

# Sed : remplace le bloc {void clearServices(); uint16_t m_mtu = 23; }; par
# {uint16_t m_mtu = 23; public: void clearServices(); };
python3 - "$LIB" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
src = p.read_text()
old = ("\tvoid clearServices();   // Clear any existing services.\n"
       "\tuint16_t m_mtu = 23;\n"
       "}; // class BLEDevice\n")
new = ("\tuint16_t m_mtu = 23;\n"
       "\n"
       "public:\n"
       "\t// PATCHED (glyconnect): rendu public pour libérer m_servicesMap après chaque\n"
       "\t// cycle BLE. Cf. patch_ble_lib.sh.\n"
       "\tvoid clearServices();\n"
       "}; // class BLEDevice\n")
if old not in src:
    print("ERREUR: bloc à patcher introuvable — la lib a peut-être changé.", file=sys.stderr)
    sys.exit(1)
p.write_text(src.replace(old, new))
print("✓ BLEClient.h patché : clearServices() est maintenant public")
PY
