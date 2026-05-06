#pragma once
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <mbedtls/aes.h>
#include <functional>

#define DEXCOM_SVC_UUID_BD      "f8083532-849e-531c-c594-30f1f86a4ea5"
#define DEXCOM_AUTH_UUID_BD     "f8083535-849e-531c-c594-30f1f86a4ea5"
#define DEXCOM_CONTROL_UUID_BD  "f8083534-849e-531c-c594-30f1f86a4ea5"

#define OP_AUTH_REQ_TX_BD   0x01
#define OP_AUTH_REQ_RX_BD   0x03
#define OP_AUTH_CHAL_TX_BD  0x04
#define OP_AUTH_CHAL_RX_BD  0x05
#define OP_KEEPALIVE_BD     0x06
#define OP_BOND_REQ_BD      0x07
#define OP_BOND_RX_BD       0x08
#define OP_TIME_TX_BD       0x24
#define OP_TIME_RX_BD       0x25
#define OP_GLUCOSE_TX_BD    0x4E
#define OP_GLUCOSE_RX_BD    0x4F

struct DexcomReadingBD {
    uint16_t mgdl       = 0;
    float    mmol       = 0.0f;
    int8_t   trend      = 0;
    bool     sensorOk   = false;
    uint16_t predicted  = 0;
    uint32_t sensorAge  = 0;
    unsigned long rxMs  = 0;
};

using DexcomReadingCbBD = std::function<void(const DexcomReadingBD&)>;
using DexcomStateCbBD   = std::function<void(const String&)>;

class DexcomBLEBD {
public:
    explicit DexcomBLEBD(const String& transmitterId);
    void begin(DexcomReadingCbBD onReading, DexcomStateCbBD onState = nullptr);
    void tick();
    void stopScan() { if (pScan) pScan->stop(); }

    static const char* const STATE_NAMES[];
    const char* getStateNameC() const { return STATE_NAMES[(int)state]; }
    String getStateName() const { return getStateNameC(); }

    bool isWaiting() const {
        if (state == State::WAIT_NEXT) return true;
        if (state == State::SCANNING && !deviceFound && millis() - stateMs > 30000) return true;
        return false;
    }

    // Diagnostic / watchdog : timestamp millis() de la dernière lecture réussie (0 si jamais)
    unsigned long lastReadingMs() const { return lastReadingTimestamp; }

    // Appelé par les ClientCallbacks BLE quand le transmetteur disconnecte
    // (public car appelé par la subclass interne, mais pas API publique)
    void onClientDisconnect();

private:
    String  txId;
    String  targetName;
    uint8_t cryptoKey[16];

    DexcomReadingCbBD cbReading;
    DexcomStateCbBD   cbState;

    BLEClient*               pClient      = nullptr;
    BLERemoteCharacteristic* authChar     = nullptr;
    BLERemoteCharacteristic* controlChar  = nullptr;

    enum class State { IDLE, SCANNING, CONNECTING, AUTH_REQ, AUTH_CHAL, AUTH_BOND,
                       TIME_REQ, GLUCOSE_REQ, READING_DONE, WAIT_NEXT };
    State   state    = State::IDLE;
    unsigned long stateMs = 0;

    // File de commandes BLE — les callbacks BT enfilent, tick() exécute (comme Node.js)
    struct BLECmd { uint8_t data[10]; uint8_t len; bool ctrl; };
    static const uint8_t CMD_CAP = 8;
    BLECmd   cmdQueue[CMD_CAP];
    volatile uint8_t cmdHead = 0, cmdTail = 0;

    // Drapeaux pour différer les opérations BLE depuis les callbacks BT
    volatile bool needSubscribeControl = false;
    unsigned long bondOkMs = 0;
    unsigned long lastReadingTimestamp = 0;  // millis() du dernier handleGlucoseRx() réussi

    void enqueueWrite(const uint8_t* d, uint8_t n, bool ctrl) {
        uint8_t next = (cmdTail + 1) % CMD_CAP;
        if (next != cmdHead) {
            memcpy(cmdQueue[cmdTail].data, d, n);
            cmdQueue[cmdTail].len  = n;
            cmdQueue[cmdTail].ctrl = ctrl;
            cmdTail = next;
        } else {
            Serial.println("[BLE] cmdQueue plein — write droppé");
        }
    }
    void drainWrites() {
        while (cmdHead != cmdTail) {
            auto& c = cmdQueue[cmdHead];
            // Garde-fou : caractéristique invalide si déconnecté entre enqueue et drain
            if (c.ctrl) {
                if (controlChar) controlChar->writeValue(c.data, c.len, true);
                else Serial.println("[BLE] drain skip: controlChar null");
            } else {
                if (authChar) authChar->writeValue(c.data, c.len, true);
                else Serial.println("[BLE] drain skip: authChar null");
            }
            cmdHead = (cmdHead + 1) % CMD_CAP;
        }
    }

    bool    bonded   = false;
    uint8_t token[8] = {};
    DexcomReadingBD lastReading;

    BLEScan*          pScan       = nullptr;
    bool              deviceFound = false;
    BLEAdvertisedDevice foundDevice;

    void setState(State s);
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
    void handleGlucoseRx(const uint8_t* d, size_t len);
    void disconnect();

    void    buildCryptoKey();
    void    aes128ecb(const uint8_t* in16, uint8_t* out16) const;
    void    computeResponse(const uint8_t* challenge, uint8_t* out8) const;
    uint16_t crc16(const uint8_t* data, size_t len) const;
    void    makeG6Msg(uint8_t opcode, uint8_t* out3) const;

    void emit(const String& s) { if (cbState) cbState(s); }

    static DexcomBLEBD* _instance;
    static void onAuthNotify(BLERemoteCharacteristic*, uint8_t*, size_t, bool);
    static void onControlNotify(BLERemoteCharacteristic*, uint8_t*, size_t, bool);

    class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
    public:
        ScanCallbacks(DexcomBLEBD* p) : parent(p) {}
        void onResult(BLEAdvertisedDevice device) override;
    private:
        DexcomBLEBD* parent;
    };
    ScanCallbacks* scanCbs = nullptr;

    // Callback BLE : détecte les déconnexions inattendues du transmetteur
    class ClientCb : public BLEClientCallbacks {
    public:
        ClientCb(DexcomBLEBD* p) : parent(p) {}
        void onConnect(BLEClient*) override {}
        void onDisconnect(BLEClient*) override { parent->onClientDisconnect(); }
    private:
        DexcomBLEBD* parent;
    };
    ClientCb* clientCbs = nullptr;
};
