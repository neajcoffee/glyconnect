#!/bin/bash
# Publie une mise à jour OTA sur GitHub Releases.
# Chaque release contient les DEUX firmwares (ble + wifi).
# Chaque firmware vérifie son propre version_<kind>.txt et télécharge firmware_<kind>.bin.
#
# Usage: ./ota_release.sh <ble|wifi> <version>
# Exemples:
#   ./ota_release.sh ble  215   (bumpe ble_prod à v215, garde wifi_prod actuel)
#   ./ota_release.sh wifi 305   (bumpe wifi_prod à v305, garde ble_prod actuel)

if [ -z "$2" ]; then
    echo "Usage: ./ota_release.sh <ble|wifi> <version>"
    exit 1
fi

KIND=$1
VERSION=$2

case "$KIND" in
    ble)  ENV=ble_prod  ;;
    wifi) ENV=wifi_prod ;;
    *) echo "ERREUR: kind doit être 'ble' ou 'wifi'"; exit 1 ;;
esac

# 1. Mettre à jour FIRMWARE_VERSION dans la bonne section [env:$ENV]
echo "→ FIRMWARE_VERSION = $VERSION dans [env:$ENV]"
awk -v env="[env:$ENV]" -v ver="$VERSION" '
    $0==env { in_env=1 }
    in_env && /FIRMWARE_VERSION/ { sub(/FIRMWARE_VERSION=[0-9]+/, "FIRMWARE_VERSION=" ver); in_env=0 }
    { print }
' platformio.ini > platformio.ini.tmp && mv platformio.ini.tmp platformio.ini

# 2. Compiler les DEUX envs (release contient les 2 firmwares)
echo "→ Compilation ble_prod..."
pio run -e ble_prod || { echo "ERREUR compilation ble_prod"; exit 1; }

echo "→ Compilation wifi_prod..."
pio run -e wifi_prod || { echo "ERREUR compilation wifi_prod"; exit 1; }

# 3. Lire les versions courantes depuis platformio.ini
BLE_VER=$(awk '/\[env:ble_prod\]/{f=1} f && /FIRMWARE_VERSION/{print; exit}' platformio.ini | grep -oE '[0-9]+')
WIFI_VER=$(awk '/\[env:wifi_prod\]/{f=1} f && /FIRMWARE_VERSION/{print; exit}' platformio.ini | grep -oE '[0-9]+')
echo "→ Versions: ble=$BLE_VER, wifi=$WIFI_VER"

# 4. Préparer les assets avec des noms uniques (gh ne supporte pas 2 firmware.bin)
ASSETS_DIR="$(mktemp -d)"
cp .pio/build/ble_prod/firmware.bin   "$ASSETS_DIR/firmware_ble.bin"
cp .pio/build/wifi_prod/firmware.bin  "$ASSETS_DIR/firmware_wifi.bin"
echo -n "$BLE_VER"  > "$ASSETS_DIR/version_ble.txt"
echo -n "$WIFI_VER" > "$ASSETS_DIR/version_wifi.txt"

# 5. Tag basé sur ce qui est bumpé
TAG="${KIND}-v${VERSION}"
gh release delete "$TAG" --repo neajcoffee/glyconnect --yes 2>/dev/null || true

# 6. Créer la release avec les 2 firmwares + 2 version.txt
echo "→ Publication GitHub Release $TAG..."
gh release create "$TAG" \
    "$ASSETS_DIR/firmware_ble.bin" \
    "$ASSETS_DIR/firmware_wifi.bin" \
    "$ASSETS_DIR/version_ble.txt" \
    "$ASSETS_DIR/version_wifi.txt" \
    --repo neajcoffee/glyconnect \
    --title "$TAG (ble=$BLE_VER, wifi=$WIFI_VER)" \
    --notes "Update $KIND → v$VERSION (ble=$BLE_VER, wifi=$WIFI_VER)" \
    --latest

rm -rf "$ASSETS_DIR"

# 7. Copier le firmware concerné sur le bureau pour flash manuel
#    Nommage cohérent avec le release GitHub (firmware_ble.bin / firmware_wifi.bin)
BUILD_DIR=".pio/build/$ENV"
cp "$BUILD_DIR/firmware.bin"   ~/Desktop/firmware_${KIND}.bin
cp "$BUILD_DIR/bootloader.bin" ~/Desktop/bootloader.bin
cp "$BUILD_DIR/partitions.bin" ~/Desktop/partitions.bin
cp "$BUILD_DIR/littlefs.bin"   ~/Desktop/littlefs.bin 2>/dev/null || true
echo "→ Binaires $ENV copiés sur le bureau (firmware_${KIND}.bin)"

echo ""
echo "✓ Release $TAG publiée (ble=$BLE_VER, wifi=$WIFI_VER)"
echo "  Le TC001 en mode $KIND téléchargera firmware_${KIND}.bin au prochain check"
