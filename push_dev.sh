#!/bin/bash
# Push une nouvelle version dev sur ntfy.sh comme attachment.
# Le TC001 (env ble_dev) poll ntfy toutes les 30s en HTTP plain → flash auto.
#
# Usage : ./push_dev.sh <version>
# Ex.   : ./push_dev.sh 401
set -e

VERSION=$1
if [ -z "$VERSION" ]; then
    echo "Usage: ./push_dev.sh <version>"
    exit 1
fi

# Récupère le topic ntfy OTA depuis secrets.ini (référencé par platformio.ini)
if [ ! -f secrets.ini ]; then
    echo "ERREUR: secrets.ini absent — copier secrets.ini.example et remplir"
    exit 1
fi
TOPIC=$(awk -F'=' '/^\[secrets\]/{f=1; next} /^\[/{f=0} f && $1 ~ /^[[:space:]]*ntfy_ota_topic[[:space:]]*$/ {gsub(/^[[:space:]]+|[[:space:]]+$/,"",$2); print $2; exit}' secrets.ini)
if [ -z "$TOPIC" ]; then
    echo "ERREUR: ntfy_ota_topic introuvable dans [secrets] de secrets.ini"
    exit 1
fi

# 1. Bump FIRMWARE_VERSION dans [env:ble_dev]
echo "→ FIRMWARE_VERSION = $VERSION dans [env:ble_dev]"
awk -v env="[env:ble_dev]" -v ver="$VERSION" '
    $0==env { in_env=1 }
    in_env && /FIRMWARE_VERSION/ { sub(/FIRMWARE_VERSION=[0-9]+/, "FIRMWARE_VERSION=" ver); in_env=0 }
    { print }
' platformio.ini > platformio.ini.tmp && mv platformio.ini.tmp platformio.ini

# 2. Compile
echo "→ Compilation ble_dev..."
pio run -e ble_dev || { echo "ERREUR compilation ble_dev"; exit 1; }

# 3. Upload sur ntfy.sh comme attachment
echo "→ Upload sur ntfy.sh/$TOPIC..."
curl --silent --fail \
     -T .pio/build/ble_dev/firmware.bin \
     -H "Filename: fw_${VERSION}.bin" \
     -H "Title: OTA dev v${VERSION}" \
     "https://ntfy.sh/${TOPIC}" >/dev/null

# 4. Copie locale pour fallback flash USB si besoin
cp .pio/build/ble_dev/firmware.bin   ~/Desktop/firmware_ble_dev.bin
cp .pio/build/ble_dev/bootloader.bin ~/Desktop/bootloader.bin
cp .pio/build/ble_dev/partitions.bin ~/Desktop/partitions.bin

echo ""
echo "✓ v${VERSION} pushé sur ntfy.sh/${TOPIC}"
echo "  Le TC001 le détectera dans max 30s (poll WAIT_NEXT)"
echo "  Fallback USB : ~/Desktop/firmware_ble_dev.bin (0x10000)"
