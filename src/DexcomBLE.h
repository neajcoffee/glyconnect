#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <mbedtls/aes.h>
#include <functional>

#define DEXCOM_ADV_UUID       "febc"
#define DEXCOM_SVC_UUID       "f8083532-849e-531c-c594-30f1f86a4ea5"
#define DEXCOM_AUTH_UUID      "f8083535-849e-531c-c594-30f1f86a4ea5"
#define DEXCOM_CONTROL_UUID   "f8083534-849e-531c-c594-30f1f86a4ea5"

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

struct DexcomReading {
    uint16_t mgdl       = 0;
    float    mmol       = 0.0f;
    int8_t   trend      = 0;
    bool     sensorOk   = false;
    uint16_t predicted  = 0;
    uint32_t sensorAge  = 0;
    unsigned long rxMs  = 0;
};

using DexcomReadingCb = std::function<void(const DexcomReading&)>;
using DexcomStateCb   = std::function<void(const String&)>;

class DexcomBLE {
public:
    explicit DexcomBLE(const String& transmitterId);
    void begin(DexcomReadingCb onReading, DexcomStateCb onState = nullptr);
    void tick();

    void stopScan() { if (pScan) pScan->stop(); }

    String getStateName() const {
        static const char* names[] = {
            "IDLE","SCANNING","CONNECTING","AUTH_REQ","AUTH_CHAL",
            "AUTH_BOND","TIME_REQ","GLUCOSE_REQ","READING_DONE","WAIT_NEXT"
        };
        return names[(int)state];
    }

    bool isWaiting() const {
        if (state == State::WAIT_NEXT) return true;
        if (state == State::SCANNING && !deviceFound && millis() - stateMs > 30000) return true;
        return false;
    }

private:
    String  txId;
    String  targetName;
    uint8_t cryptoKey[16];

    DexcomReadingCb cbReading;
    DexcomStateCb   cbState;

    NimBLEClient*               pClient      = nullptr;
    NimBLERemoteCharacteristic* authChar     = nullptr;
    NimBLERemoteCharacteristic* controlChar  = nullptr;

    enum class State { IDLE, SCANNING, CONNECTING, AUTH_REQ, AUTH_CHAL, AUTH_BOND,
                       TIME_REQ, GLUCOSE_REQ, READING_DONE, WAIT_NEXT };
    State   state        = State::IDLE;
    unsigned long stateMs = 0;

    bool    bonded       = false;
    uint8_t token[8]     = {};
    DexcomReading lastReading;

    NimBLEScan*    pScan        = nullptr;
    bool           deviceFound  = false;
    NimBLEAddress  foundAddress;
    NimBLEAddress  altAddress;

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

    static DexcomBLE* _instance;
    static void onAuthNotify(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool);
    static void onControlNotify(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool);

    class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    public:
        ScanCallbacks(DexcomBLE* p) : parent(p) {}
        void onResult(NimBLEAdvertisedDevice* adv) override;
    private:
        DexcomBLE* parent;
    };
    ScanCallbacks* scanCbs = nullptr;
};
