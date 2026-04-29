#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <mbedtls/aes.h>
#include <functional>

// ─── UUIDs ───────────────────────────────────────────────────────────────────
#define DEXCOM_ADV_UUID       "febc"
#define DEXCOM_SVC_UUID       "f8083532-849e-531c-c594-30f1f86a4ea5"
#define DEXCOM_AUTH_UUID      "f8083535-849e-531c-c594-30f1f86a4ea5"
#define DEXCOM_CONTROL_UUID   "f8083534-849e-531c-c594-30f1f86a4ea5"

// ─── Opcodes ─────────────────────────────────────────────────────────────────
#define OP_AUTH_REQ_TX    0x01
#define OP_AUTH_REQ_RX    0x03
#define OP_AUTH_CHAL_TX   0x04
#define OP_AUTH_CHAL_RX   0x05
#define OP_KEEPALIVE      0x06
#define OP_BOND_REQ       0x07
#define OP_BOND_RX        0x08
#define OP_TIME_TX        0x24
#define OP_TIME_RX        0x25
#define OP_GLUCOSE_TX     0x4E
#define OP_GLUCOSE_RX     0x4F

#define SENSOR_LIFE_SEC   (10UL * 24 * 3600)

// ─── Structures ──────────────────────────────────────────────────────────────
struct DexcomReading {
    uint16_t mgdl       = 0;
    float    mmol       = 0.0f;
    int8_t   trend      = 0;     // mg/dL/min * 0.1 (signé)
    bool     sensorOk   = false;
    uint16_t predicted  = 0;
    uint32_t sensorAge  = 0;     // secondes depuis insertion
    unsigned long rxMs  = 0;     // millis() à la réception
};

// ─── Callbacks ───────────────────────────────────────────────────────────────
using DexcomReadingCb = std::function<void(const DexcomReading&)>;
using DexcomStateCb   = std::function<void(const String&)>;

// ─── Classe principale ───────────────────────────────────────────────────────
class DexcomBLE {
public:
    explicit DexcomBLE(const String& transmitterId);

    void begin(DexcomReadingCb onReading, DexcomStateCb onState = nullptr);
    void tick();  // à appeler dans loop()

private:
    // ── Config ──
    String  txId;
    String  targetName;   // "DexcomXX"
    uint8_t cryptoKey[16];

    // ── Callbacks ──
    DexcomReadingCb cbReading;
    DexcomStateCb   cbState;

    // ── BLE ──
    NimBLEClient*               pClient      = nullptr;
    NimBLERemoteCharacteristic* authChar     = nullptr;
    NimBLERemoteCharacteristic* controlChar  = nullptr;

    // ── Machine à états ──
    enum class State { IDLE, SCANNING, CONNECTING, AUTH_REQ, AUTH_CHAL, AUTH_BOND,
                       TIME_REQ, GLUCOSE_REQ, READING_DONE, WAIT_NEXT };
    State   state        = State::IDLE;
    unsigned long stateMs = 0;   // millis() entrée dans l'état courant

    bool    bonded       = false;
    uint8_t token[8]     = {};
    DexcomReading lastReading;

    // ── Scan ──
    NimBLEScan*          pScan    = nullptr;
    NimBLEAdvertisedDevice* foundDevice = nullptr;

    void setState(State s);

    // ── Étapes protocole ──
    void startScan();
    bool connect();
    bool discoverChars();
    void sendAuthRequest();
    void handleAuthRx(const uint8_t* d, size_t len);
    void handleAuthStatus(const uint8_t* d, size_t len);
    void sendBondRequest();
    void handleBondRx(const uint8_t* d, size_t len);
    void sendTimeRequest();
    void handleTimeRx(const uint8_t* d, size_t len);
    void sendGlucoseRequest();
    void handleGlucoseRx(const uint8_t* d, size_t len);
    void disconnect();

    // ── Crypto ──
    void    buildCryptoKey();
    void    aes128ecb(const uint8_t* in16, uint8_t* out16) const;
    void    computeResponse(const uint8_t* challenge, uint8_t* out8) const;
    uint16_t crc16(const uint8_t* data, size_t len) const;
    void    makeG6Msg(uint8_t opcode, uint8_t* out3) const;

    void emit(const String& s) { if (cbState) cbState(s); }

    // ── Callbacks BLE statiques (NimBLE exige pointeurs de fonction) ──
    static DexcomBLE* _instance;
    static void onAuthNotify(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool);
    static void onControlNotify(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool);
    static void onScanResult(NimBLEAdvertisedDevice*);
};
