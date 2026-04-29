#include "DexcomBLE.h"
#include <esp_random.h>

DexcomBLE* DexcomBLE::_instance = nullptr;

// ─── CRC-16-CCITT (poly=0x1021, init=0x0000, résultat LE) ───────────────────
static const uint16_t CRC16_TABLE[256] = {
    0x0000,0x1021,0x2042,0x3063,0x4084,0x50A5,0x60C6,0x70E7,
    0x8108,0x9129,0xA14A,0xB16B,0xC18C,0xD1AD,0xE1CE,0xF1EF,
    0x1231,0x0210,0x3273,0x2252,0x52B5,0x4294,0x72F7,0x62D6,
    0x9339,0x8318,0xB37B,0xA35A,0xD3BD,0xC39C,0xF3FF,0xE3DE,
    0x2462,0x3443,0x0420,0x1401,0x64E6,0x74C7,0x44A4,0x5485,
    0xA56A,0xB54B,0x8528,0x9509,0xE5EE,0xF5CF,0xC5AC,0xD58D,
    0x3653,0x2672,0x1611,0x0630,0x76D7,0x66F6,0x5695,0x46B4,
    0xB75B,0xA77A,0x9719,0x8738,0xF7DF,0xE7FE,0xD79D,0xC7BC,
    0x48C4,0x58E5,0x6886,0x78A7,0x0840,0x1861,0x2802,0x3823,
    0xC9CC,0xD9ED,0xE98E,0xF9AF,0x8948,0x9969,0xA90A,0xB92B,
    0x5AF5,0x4AD4,0x7AB7,0x6A96,0x1A71,0x0A50,0x3A33,0x2A12,
    0xDBFD,0xCBDC,0xFBBF,0xEB9E,0x9B79,0x8B58,0xBB3B,0xAB1A,
    0x6CA6,0x7C87,0x4CE4,0x5CC5,0x2C22,0x3C03,0x0C60,0x1C41,
    0xEDAE,0xFD8F,0xCDEC,0xDDCD,0xAD2A,0xBD0B,0x8D68,0x9D49,
    0x7E97,0x6EB6,0x5ED5,0x4EF4,0x3E13,0x2E32,0x1E51,0x0E70,
    0xFF9F,0xEFBE,0xDFDD,0xCFFC,0xBF1B,0xAF3A,0x9F59,0x8F78,
    0x9188,0x81A9,0xB1CA,0xA1EB,0xD10C,0xC12D,0xF14E,0xE16F,
    0x1080,0x00A1,0x30C2,0x20E3,0x5004,0x4025,0x7046,0x6067,
    0x83B9,0x9398,0xA3FB,0xB3DA,0xC33D,0xD31C,0xE37F,0xF35E,
    0x02B1,0x1290,0x22F3,0x32D2,0x4235,0x5214,0x6277,0x7256,
    0xB5EA,0xA5CB,0x95A8,0x8589,0xF56E,0xE54F,0xD52C,0xC50D,
    0x34E2,0x24C3,0x14A0,0x0481,0x7466,0x6447,0x5424,0x4405,
    0xA7DB,0xB7FA,0x8799,0x97B8,0xE75F,0xF77E,0xC71D,0xD73C,
    0x26D3,0x36F2,0x0691,0x16B0,0x6657,0x7676,0x4615,0x5634,
    0xD94C,0xC96D,0xF90E,0xE92F,0x99C8,0x89E9,0xB98A,0xA9AB,
    0x5844,0x4865,0x7806,0x6827,0x18C0,0x08E1,0x3882,0x28A3,
    0xCB7D,0xDB5C,0xEB3F,0xFB1E,0x8BF9,0x9BD8,0xABBB,0xBB9A,
    0x4A75,0x5A54,0x6A37,0x7A16,0x0AF1,0x1AD0,0x2AB3,0x3A92,
    0xFD2E,0xED0F,0xDD6C,0xCD4D,0xBDAA,0xAD8B,0x9DE8,0x8DC9,
    0x7C26,0x6C07,0x5C64,0x4C45,0x3CA2,0x2C83,0x1CE0,0x0CC1,
    0xEF1F,0xFF3E,0xCF5D,0xDF7C,0xAF9B,0xBFBA,0x8FD9,0x9FF8,
    0x6E17,0x7E36,0x4E55,0x5E74,0x2E93,0x3EB2,0x0ED1,0x1EF0,
};

// ─── Constructeur ─────────────────────────────────────────────────────────────
DexcomBLE::DexcomBLE(const String& transmitterId) : txId(transmitterId) {
    txId.toUpperCase();
    targetName = "Dexcom" + txId.substring(txId.length() - 2);
    buildCryptoKey();
    _instance = this;
}

// ─── Clé AES : "00" + id + "00" + id (UTF-8) ─────────────────────────────────
void DexcomBLE::buildCryptoKey() {
    String keyStr = "00" + txId + "00" + txId;
    for (int i = 0; i < 16 && i < (int)keyStr.length(); i++)
        cryptoKey[i] = (uint8_t)keyStr[i];
}

// ─── AES-128-ECB ─────────────────────────────────────────────────────────────
void DexcomBLE::aes128ecb(const uint8_t* in16, uint8_t* out16) const {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, cryptoKey, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in16, out16);
    mbedtls_aes_free(&ctx);
}

void DexcomBLE::computeResponse(const uint8_t* challenge, uint8_t* out8) const {
    uint8_t input[16], output[16];
    memcpy(input,     challenge, 8);
    memcpy(input + 8, challenge, 8);
    aes128ecb(input, output);
    memcpy(out8, output, 8);
}

// ─── CRC-16 ──────────────────────────────────────────────────────────────────
uint16_t DexcomBLE::crc16(const uint8_t* data, size_t len) const {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; i++)
        crc = (crc << 8) ^ CRC16_TABLE[((crc >> 8) ^ data[i]) & 0xFF];
    return crc;
}

void DexcomBLE::makeG6Msg(uint8_t opcode, uint8_t* out3) const {
    out3[0] = opcode;
    uint16_t c = crc16(&opcode, 1);
    out3[1] = c & 0xFF;
    out3[2] = (c >> 8) & 0xFF;
}

// ─── begin() ─────────────────────────────────────────────────────────────────
void DexcomBLE::begin(DexcomReadingCb onReading, DexcomStateCb onState) {
    cbReading = onReading;
    cbState   = onState;

    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    pScan = NimBLEDevice::getScan();
    pScan->setScanCallbacks(nullptr, false);
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);

    startScan();
}

// ─── Scan ─────────────────────────────────────────────────────────────────────
void DexcomBLE::startScan() {
    foundDevice = nullptr;
    setState(State::SCANNING);
    emit("SCAN...");
    pScan->clearResults();
    pScan->start(0, false); // scan continu
}

void DexcomBLE::onScanResult(NimBLEAdvertisedDevice* adv) {
    if (!_instance) return;
    String name = adv->getName().c_str();
    auto   uuids = adv->getServiceUUIDs();
    bool hasFebc = false;
    for (int i = 0; i < (int)uuids.size(); i++)
        if (String(uuids[i].toString().c_str()).indexOf("febc") >= 0) { hasFebc = true; break; }

    if (name == _instance->targetName || hasFebc) {
        _instance->pScan->stop();
        _instance->foundDevice = adv;
    }
}

// ─── setState() ──────────────────────────────────────────────────────────────
void DexcomBLE::setState(State s) {
    state   = s;
    stateMs = millis();
}

// ─── tick() — appelé dans loop() ─────────────────────────────────────────────
void DexcomBLE::tick() {
    unsigned long now = millis();

    switch (state) {
    case State::SCANNING:
        if (foundDevice) {
            setState(State::CONNECTING);
        }
        // Timeout 90s → relancer
        if (now - stateMs > 90000 && !foundDevice) {
            pScan->stop();
            startScan();
        }
        break;

    case State::CONNECTING:
        if (connect()) {
            sendAuthRequest();
        } else {
            emit("ERR CONNECT");
            startScan();
        }
        break;

    case State::AUTH_REQ:
    case State::AUTH_CHAL:
    case State::AUTH_BOND:
    case State::TIME_REQ:
    case State::GLUCOSE_REQ:
        // Timeout 10s → déconnexion et rescan
        if (now - stateMs > 10000) {
            emit("TIMEOUT");
            disconnect();
            delay(2000);
            startScan();
        }
        break;

    case State::READING_DONE:
        disconnect();
        setState(State::WAIT_NEXT);
        emit("OK");
        break;

    case State::WAIT_NEXT:
        if (now - stateMs > 10000) startScan();
        break;

    default: break;
    }
}

// ─── Connexion ───────────────────────────────────────────────────────────────
bool DexcomBLE::connect() {
    emit("CONNECT");
    if (!pClient) {
        pClient = NimBLEDevice::createClient();
        pClient->setConnectionParams(12, 12, 0, 51);
        pClient->setTimeout(10);
    }

    if (!pClient->connect(foundDevice)) return false;
    if (!discoverChars())              return false;

    setState(State::CONNECTING);
    return true;
}

bool DexcomBLE::discoverChars() {
    NimBLERemoteService* svc = pClient->getService(DEXCOM_SVC_UUID);
    if (!svc) { emit("NO SVC"); return false; }

    authChar    = svc->getCharacteristic(DEXCOM_AUTH_UUID);
    controlChar = svc->getCharacteristic(DEXCOM_CONTROL_UUID);

    if (!authChar || !controlChar) { emit("NO CHAR"); return false; }

    authChar->subscribe(true, onAuthNotify, true);    // indications
    controlChar->subscribe(true, onControlNotify, true);
    return true;
}

// ─── Auth — Étape 1 ──────────────────────────────────────────────────────────
void DexcomBLE::sendAuthRequest() {
    esp_fill_random(token, 8);
    uint8_t msg[10];
    msg[0] = OP_AUTH_REQ_TX;
    memcpy(msg + 1, token, 8);
    msg[9] = 0x01;  // end byte G6
    authChar->writeValue(msg, 10, false);
    setState(State::AUTH_REQ);
    emit("AUTH...");
}

// ─── Auth — callback indications f8083535 ────────────────────────────────────
void DexcomBLE::onAuthNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool isNotify) {
    if (!_instance || len < 1) return;
    _instance->handleAuthRx(data, len);
}

void DexcomBLE::handleAuthRx(const uint8_t* d, size_t len) {
    if (d[0] == OP_AUTH_REQ_RX && len >= 17 && state == State::AUTH_REQ) {
        // Vérification token hash (optionnelle)
        // d[1..8] = hash de notre token
        // d[9..16] = challenge
        uint8_t response[8];
        computeResponse(d + 9, response);

        uint8_t msg[9];
        msg[0] = OP_AUTH_CHAL_TX;
        memcpy(msg + 1, response, 8);
        authChar->writeValue(msg, 9, false);
        setState(State::AUTH_CHAL);
    }
    else if (d[0] == OP_AUTH_CHAL_RX && len >= 3 && state == State::AUTH_CHAL) {
        handleAuthStatus(d, len);
    }
    else if (d[0] == OP_BOND_RX && state == State::AUTH_BOND) {
        handleBondRx(d, len);
    }
}

void DexcomBLE::handleAuthStatus(const uint8_t* d, size_t len) {
    bool authenticated = (d[1] == 0x01);
    bonded             = (d[2] == 0x01);

    if (!authenticated) { emit("AUTH FAIL"); disconnect(); startScan(); return; }

    if (!bonded) {
        sendBondRequest();
    } else {
        sendTimeRequest();
    }
}

// ─── Bond ────────────────────────────────────────────────────────────────────
void DexcomBLE::sendBondRequest() {
    uint8_t ka[2] = { OP_KEEPALIVE, 25 };
    authChar->writeValue(ka, 2, false);
    uint8_t br[1] = { OP_BOND_REQ };
    authChar->writeValue(br, 1, false);
    setState(State::AUTH_BOND);
    emit("BOND...");
}

void DexcomBLE::handleBondRx(const uint8_t* d, size_t len) {
    bonded = true;
    sendTimeRequest();
}

// ─── TransmitterTime ─────────────────────────────────────────────────────────
void DexcomBLE::sendTimeRequest() {
    uint8_t msg[3];
    makeG6Msg(OP_TIME_TX, msg);
    controlChar->writeValue(msg, 3, false);
    setState(State::TIME_REQ);
}

// ─── Glucose ─────────────────────────────────────────────────────────────────
void DexcomBLE::sendGlucoseRequest() {
    uint8_t msg[3];
    makeG6Msg(OP_GLUCOSE_TX, msg);
    controlChar->writeValue(msg, 3, false);
    setState(State::GLUCOSE_REQ);
}

// ─── Callback control (f8083534) ─────────────────────────────────────────────
void DexcomBLE::onControlNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    if (!_instance || len < 1) return;
    _instance->handleTimeRx(data, len);
}

void DexcomBLE::handleTimeRx(const uint8_t* d, size_t len) {
    if (d[0] == OP_TIME_RX && len >= 10 && state == State::TIME_REQ) {
        uint32_t currentTime  = (uint32_t)d[2] | ((uint32_t)d[3]<<8) | ((uint32_t)d[4]<<16) | ((uint32_t)d[5]<<24);
        uint32_t sessionStart = (uint32_t)d[6] | ((uint32_t)d[7]<<8) | ((uint32_t)d[8]<<16) | ((uint32_t)d[9]<<24);
        lastReading.sensorAge = (currentTime > sessionStart) ? (currentTime - sessionStart) : 0;
        sendGlucoseRequest();
    }
    else if (d[0] == OP_GLUCOSE_RX && len >= 14 && state == State::GLUCOSE_REQ) {
        handleGlucoseRx(d, len);
    }
}

void DexcomBLE::handleGlucoseRx(const uint8_t* d, size_t len) {
    uint8_t  st    = d[1];
    uint16_t gRaw  = (uint16_t)d[10] | ((uint16_t)d[11] << 8);
    uint16_t mgdl  = gRaw & 0x0FFF;
    uint8_t  state_ = d[12];
    int8_t   trend = (int8_t)d[13];

    lastReading.mgdl     = mgdl;
    lastReading.mmol     = (float)mgdl / 18.01559f;
    lastReading.trend    = (trend == 0x7F) ? 0 : trend;
    lastReading.sensorOk = (state_ == 0x06);
    lastReading.predicted= (len >= 16) ? ((uint16_t)d[14] | ((uint16_t)d[15] << 8)) & 0x03FF : 0;
    lastReading.rxMs     = millis();

    setState(State::READING_DONE);

    if (cbReading) cbReading(lastReading);
}

// ─── Déconnexion ─────────────────────────────────────────────────────────────
void DexcomBLE::disconnect() {
    if (pClient && pClient->isConnected()) pClient->disconnect();
    bonded = false;
}
