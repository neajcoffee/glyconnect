#include "DexcomBLE_BD.h"
#include <esp_random.h>
#include <esp_log.h>
#include <esp_gap_ble_api.h>

DexcomBLEBD* DexcomBLEBD::_instance = nullptr;

const char* const DexcomBLEBD::STATE_NAMES[] = {
    "IDLE","SCANNING","CONNECTING","AUTH_REQ","AUTH_CHAL",
    "AUTH_BOND","TIME_REQ","GLUCOSE_REQ","READING_DONE","WAIT_NEXT"
};

static const uint16_t CRC16[256] = {
    0x0000,0x1021,0x2042,0x3063,0x4084,0x50A5,0x60C6,0x70E7,0x8108,0x9129,0xA14A,0xB16B,0xC18C,0xD1AD,0xE1CE,0xF1EF,
    0x1231,0x0210,0x3273,0x2252,0x52B5,0x4294,0x72F7,0x62D6,0x9339,0x8318,0xB37B,0xA35A,0xD3BD,0xC39C,0xF3FF,0xE3DE,
    0x2462,0x3443,0x0420,0x1401,0x64E6,0x74C7,0x44A4,0x5485,0xA56A,0xB54B,0x8528,0x9509,0xE5EE,0xF5CF,0xC5AC,0xD58D,
    0x3653,0x2672,0x1611,0x0630,0x76D7,0x66F6,0x5695,0x46B4,0xB75B,0xA77A,0x9719,0x8738,0xF7DF,0xE7FE,0xD79D,0xC7BC,
    0x48C4,0x58E5,0x6886,0x78A7,0x0840,0x1861,0x2802,0x3823,0xC9CC,0xD9ED,0xE98E,0xF9AF,0x8948,0x9969,0xA90A,0xB92B,
    0x5AF5,0x4AD4,0x7AB7,0x6A96,0x1A71,0x0A50,0x3A33,0x2A12,0xDBFD,0xCBDC,0xFBBF,0xEB9E,0x9B79,0x8B58,0xBB3B,0xAB1A,
    0x6CA6,0x7C87,0x4CE4,0x5CC5,0x2C22,0x3C03,0x0C60,0x1C41,0xEDAE,0xFD8F,0xCDEC,0xDDCD,0xAD2A,0xBD0B,0x8D68,0x9D49,
    0x7E97,0x6EB6,0x5ED5,0x4EF4,0x3E13,0x2E32,0x1E51,0x0E70,0xFF9F,0xEFBE,0xDFDD,0xCFFC,0xBF1B,0xAF3A,0x9F59,0x8F78,
    0x9188,0x81A9,0xB1CA,0xA1EB,0xD10C,0xC12D,0xF14E,0xE16F,0x1080,0x00A1,0x30C2,0x20E3,0x5004,0x4025,0x7046,0x6067,
    0x83B9,0x9398,0xA3FB,0xB3DA,0xC33D,0xD31C,0xE37F,0xF35E,0x02B1,0x1290,0x22F3,0x32D2,0x4235,0x5214,0x6277,0x7256,
    0xB5EA,0xA5CB,0x95A8,0x8589,0xF56E,0xE54F,0xD52C,0xC50D,0x34E2,0x24C3,0x14A0,0x0481,0x7466,0x6447,0x5424,0x4405,
    0xA7DB,0xB7FA,0x8799,0x97B8,0xE75F,0xF77E,0xC71D,0xD73C,0x26D3,0x36F2,0x0691,0x16B0,0x6657,0x7676,0x4615,0x5634,
    0xD94C,0xC96D,0xF90E,0xE92F,0x99C8,0x89E9,0xB98A,0xA9AB,0x5844,0x4865,0x7806,0x6827,0x18C0,0x08E1,0x3882,0x28A3,
    0xCB7D,0xDB5C,0xEB3F,0xFB1E,0x8BF9,0x9BD8,0xABBB,0xBB9A,0x4A75,0x5A54,0x6A37,0x7A16,0x0AF1,0x1AD0,0x2AB3,0x3A92,
    0xFD2E,0xED0F,0xDD6C,0xCD4D,0xBDAA,0xAD8B,0x9DE8,0x8DC9,0x7C26,0x6C07,0x5C64,0x4C45,0x3CA2,0x2C83,0x1CE0,0x0CC1,
    0xEF1F,0xFF3E,0xCF5D,0xDF7C,0xAF9B,0xBFBA,0x8FD9,0x9FF8,0x6E17,0x7E36,0x4E55,0x5E74,0x2E93,0x3EB2,0x0ED1,0x1EF0,
};

DexcomBLEBD::DexcomBLEBD(const String& id) : txId(id) {
    txId.toUpperCase();
    targetName = "Dexcom" + txId.substring(txId.length() - 2);
    String k = "00" + txId + "00" + txId;
    for (int i = 0; i < 16; i++) cryptoKey[i] = (uint8_t)k[i];
    _instance = this;
}

void DexcomBLEBD::aes128ecb(const uint8_t* in, uint8_t* out) const {
    mbedtls_aes_context c; mbedtls_aes_init(&c);
    mbedtls_aes_setkey_enc(&c, cryptoKey, 128);
    mbedtls_aes_crypt_ecb(&c, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&c);
}

void DexcomBLEBD::computeResponse(const uint8_t* ch, uint8_t* out) const {
    uint8_t in[16], o[16];
    memcpy(in, ch, 8); memcpy(in+8, ch, 8);
    aes128ecb(in, o); memcpy(out, o, 8);
}

uint16_t DexcomBLEBD::crc16(const uint8_t* d, size_t n) const {
    uint16_t crc = 0;
    for (size_t i=0;i<n;i++) crc=(crc<<8)^CRC16[((crc>>8)^d[i])&0xFF];
    return crc;
}

void DexcomBLEBD::makeG6Msg(uint8_t op, uint8_t* o) const {
    o[0]=op; uint16_t c=crc16(&op,1); o[1]=c&0xFF; o[2]=(c>>8)&0xFF;
}

static void hx(const char* l, const uint8_t* d, size_t n) {
    Serial.printf("[BLE] %s: ", l);
    for (size_t i=0;i<n;i++) Serial.printf("%02X ",d[i]);
    Serial.println();
}

// Helper : efface tous les bonds Bluedroid stockés (LTK)
static void clearAllBonds() {
    int n = esp_ble_get_bond_device_num();
    if (n <= 0) return;
    esp_ble_bond_dev_t* list = (esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t) * n);
    if (!list) return;
    esp_ble_get_bond_device_list(&n, list);
    for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
    free(list);
    Serial.printf("[BLE] %d bond(s) effacés\n", n);
}

void DexcomBLEBD::begin(DexcomReadingCbBD onR, DexcomStateCbBD onS) {
    cbReading=onR; cbState=onS;
    BLEDevice::init(""); BLEDevice::setPower(ESP_PWR_LVL_P9);
    clearAllBonds();
    Serial.println("[BLE] Bluedroid init");
    pScan=BLEDevice::getScan();
    scanCbs=new ScanCallbacks(this);
    pScan->setAdvertisedDeviceCallbacks(scanCbs);
    pScan->setActiveScan(true); pScan->setInterval(100); pScan->setWindow(99);

    // Crée le BLEClient une seule fois (pour TOUTE la durée de vie du firmware).
    // BLEDevice::createClient() ajoute le client au map interne ; faire delete + recreate
    // à chaque connexion fuite des entrées dans ce map → crash heap après quelques heures.
    pClient = BLEDevice::createClient();
    clientCbs = new ClientCb(this);
    pClient->setClientCallbacks(clientCbs);

    startScan();
}

void DexcomBLEBD::setState(State s) {
    const char* n[]={"IDLE","SCANNING","CONNECTING","AUTH_REQ","AUTH_CHAL","AUTH_BOND","TIME_REQ","GLUCOSE_REQ","READING_DONE","WAIT_NEXT"};
    Serial.printf("[STATE] %s→%s\n",n[(int)state],n[(int)s]);
    state=s; stateMs=millis();
}

void DexcomBLEBD::startScan() {
    deviceFound=false; setState(State::SCANNING); emit("SCAN...");
    pScan->clearResults(); pScan->start(0,false);
}

void DexcomBLEBD::ScanCallbacks::onResult(BLEAdvertisedDevice dev) {
    String name=dev.getName().c_str();
    bool febc=false;
    for(int i=0;i<dev.getServiceUUIDCount();i++)
        if(dev.getServiceUUID(i).equals(BLEUUID((uint16_t)0xFEBC))){febc=true;break;}
    if(!name.isEmpty()||febc)
        Serial.printf("[SCAN] \"%s\" %s febc=%d\n",name.c_str(),dev.getAddress().toString().c_str(),febc);
    // Match strict par nom : FEBC seul matchait n'importe quel Dexcom à proximité,
    // → connexion au mauvais transmetteur → AUTH fail en boucle si plusieurs G6 présents.
    // G6 broadcast toujours son nom en active scan (setActiveScan(true) déjà actif).
    if(name==parent->targetName){
        Serial.printf("[SCAN] Cible: %s\n",dev.getAddress().toString().c_str());
        parent->foundDevice=dev; parent->deviceFound=true; parent->pScan->stop();
    }
}

void DexcomBLEBD::tick() {
    // Exécute les writes en attente depuis les callbacks BT (pattern Node.js)
    drainWrites();

    unsigned long now=millis();

    // Détection précoce d'un disconnect inattendu (onDisconnect a nullé authChar)
    // pendant qu'on était en plein dialogue → court-circuite le timeout 10/20s.
    bool inDialog = (state==State::AUTH_REQ||state==State::AUTH_CHAL||
                     state==State::AUTH_BOND||state==State::TIME_REQ||
                     state==State::GLUCOSE_REQ);
    if (inDialog && (!pClient || !pClient->isConnected())) {
        emit("LIEN COUPE"); setState(State::WAIT_NEXT); return;
    }

    switch(state){
    case State::SCANNING:
        if(deviceFound) setState(State::CONNECTING);
        // Bluedroid scan(duration=0) tourne en continu → pas besoin de restart fréquent.
        // Restart toutes les 2h seulement, en défense contre une dérive silencieuse de la stack.
        // Original 90s causait un WDT après ~8 restarts (accumulation Bluedroid).
        if(now-stateMs>2UL*60*60*1000&&!deviceFound){pScan->stop();startScan();}
        break;
    case State::CONNECTING:
        if(now-stateMs<500) break;
        if(connect()) {
            // Le transmetteur peut avoir envoyé [0x03] pendant connect()
            // → state déjà passé à AUTH_CHAL dans handleAuthRx
            if(state==State::CONNECTING) sendAuthRequest();
        } else {
            emit("ATTENTE..."); setState(State::WAIT_NEXT);
        }
        break;
    case State::AUTH_REQ: case State::AUTH_CHAL:
    case State::TIME_REQ: case State::GLUCOSE_REQ:
        if(now-stateMs>10000){emit("TIMEOUT");disconnect();delay(500);setState(State::WAIT_NEXT);}
        break;
    case State::AUTH_BOND:
        // Timeout plus long : bond + SMP peut prendre plusieurs secondes
        if(now-stateMs>20000){emit("TIMEOUT");disconnect();delay(500);setState(State::WAIT_NEXT);}
        break;
    case State::READING_DONE:
        disconnect();setState(State::WAIT_NEXT);emit("OK");
        break;
    case State::WAIT_NEXT:
        if(now-stateMs>30000) startScan();
        break;
    default:break;
    }

    // Pending subscribe controlChar + TIME_TX (après le switch pour ne pas
    // créer de race entre stateMs nouveau et 'now' ancien)
    // Attendre 1.5s après BOND OK pour laisser SMP/encryption se compléter
    if (needSubscribeControl && millis() - bondOkMs > 1500) {
        needSubscribeControl = false;
        // Garde-fou : si onDisconnect a nullé controlChar entre BOND_OK et ici,
        // sendTimeRequest crasherait. Skip silencieusement, le timeout 10s gérera.
        if (controlChar) sendTimeRequest();
        else { emit("BOND→TIME annulé (déconnecté)"); }
    }
}

bool DexcomBLEBD::connect() {
    emit("CONNECT");
    // Effacer les bonds avant CHAQUE connexion : le transmetteur Dexcom peut
    // changer de LTK, et garder un ancien fait échouer SMP silencieusement
    clearAllBonds();

    // Réutilise le pClient créé dans begin() ; juste cleanup avant re-connect.
    // Les BLERemoteCharacteristic appartenaient au service de la connexion précédente
    // et sont libérés au disconnect → on les invalide explicitement.
    if (pClient->isConnected()) pClient->disconnect();
    delay(100);
    authChar = nullptr;
    controlChar = nullptr;
    needSubscribeControl = false;

    std::string ps=foundDevice.getAddress().toString();
    int lb=strtol(ps.substr(15).c_str(),nullptr,16);
    char as[18]; snprintf(as,sizeof(as),"%s%02x",ps.substr(0,15).c_str(),(lb+1)&0xFF);
    const char* addrs[2]={ps.c_str(),as};
    for(int i=0;i<2;i++){
        String sh=String(addrs[i]).substring(15);
        emit("CONN "+String(i+1)+"/2 "+sh);
        if(pClient->connect(BLEAddress(addrs[i]),BLE_ADDR_TYPE_RANDOM)){
            emit("CONNECTED "+sh);
            if(discoverChars()) return true;
            pClient->disconnect();
            authChar = nullptr;
            controlChar = nullptr;
            return false;
        }
        emit("CONN FAIL "+String(i+1));
    }
    return false;
}

bool DexcomBLEBD::discoverChars() {
    BLERemoteService* svc=pClient->getService(DEXCOM_SVC_UUID_BD);
    if(!svc){emit("NO SVC");return false;}
    authChar=svc->getCharacteristic(DEXCOM_AUTH_UUID_BD);
    controlChar=svc->getCharacteristic(DEXCOM_CONTROL_UUID_BD);
    if(!authChar||!controlChar){emit("NO CHAR");return false;}
    // INDICATE comme xDrip/CGMBLEKit (G6 chars sont indicate-only)
    Serial.println("[BLE] Subscribe authChar INDICATE");
    authChar->registerForNotify(onAuthNotify, false);  // false = INDICATE
    return true;
}

void DexcomBLEBD::sendAuthRequest() {
    esp_fill_random(token,8);
    uint8_t msg[10]; msg[0]=OP_AUTH_REQ_TX_BD;
    memcpy(msg+1,token,8); msg[9]=0x01;
    // setState AVANT writeValue : si [0x03] arrive pendant le blocage,
    // handleAuthRx voit AUTH_REQ (correct) et passe à AUTH_CHAL sans être écrasé
    setState(State::AUTH_REQ); emit("AUTH...");
    hx("→ AuthReq",msg,10);
    authChar->writeValue(msg,10,true);
}

void DexcomBLEBD::onAuthNotify(BLERemoteCharacteristic*,uint8_t* d,size_t n,bool){
    if(!_instance||n<1) return; _instance->handleAuthRx(d,n);
}
void DexcomBLEBD::onControlNotify(BLERemoteCharacteristic*,uint8_t* d,size_t n,bool){
    if(!_instance||n<1) return; _instance->handleTimeRx(d,n);
}

void DexcomBLEBD::handleAuthRx(const uint8_t* d,size_t n){
    hx("← Auth",d,n);
    // Accepte [0x03] en CONNECTING : le transmetteur envoie le challenge
    // dès la subscription CCCD, avant qu'on envoie [0x01]
    if(d[0]==OP_AUTH_REQ_RX_BD&&n>=17&&
       (state==State::AUTH_REQ||state==State::CONNECTING)){
        emit("AUTH CHALLENGE recu");
        uint8_t r[8]; computeResponse(d+9,r);
        uint8_t msg[9]; msg[0]=OP_AUTH_CHAL_TX_BD; memcpy(msg+1,r,8);
        enqueueWrite(msg, 9, false);  // auth char, non-ctrl
        emit("AUTH RESPONSE envoyee"); setState(State::AUTH_CHAL);
    } else if(d[0]==OP_AUTH_CHAL_RX_BD&&n>=3&&state==State::AUTH_CHAL){
        emit("AUTH STATUS auth="+String(d[1])+" bond="+String(d[2]));
        handleAuthStatus(d,n);
    } else if(d[0]==OP_BOND_RX_BD&&state==State::AUTH_BOND){
        emit("BOND RX st="+String(n>1?d[1]:0xFF)); handleBondRx(d,n);
    } else {
        emit("AUTH ERR op=0x"+String(d[0],HEX));
    }
}

void DexcomBLEBD::handleAuthStatus(const uint8_t* d,size_t n){
    if(d[1]!=0x01){emit("AUTH FAIL");disconnect();startScan();return;}
    bonded=(d[2]==0x01);
    if(!bonded) {
        sendBondRequest();
    } else {
        // Déjà bondé : différer le subscribe controlChar à tick() pour éviter
        // l'appel registerForNotify depuis le callback BT (deadlock)
        bondOkMs = millis();
        needSubscribeControl = true;
        setState(State::AUTH_BOND);
    }
}

void DexcomBLEBD::sendBondRequest(){
    uint8_t ka[2]={OP_KEEPALIVE_BD,25}; enqueueWrite(ka,2,false);
    uint8_t br[1]={OP_BOND_REQ_BD};     enqueueWrite(br,1,false);
    setState(State::AUTH_BOND); emit("BOND...");
}

void DexcomBLEBD::handleBondRx(const uint8_t*,size_t){
    // Différer subscribe + write au tick() main thread
    // pour éviter deadlock registerForNotify depuis callback BT
    bonded = true;
    bondOkMs = millis();
    stateMs = millis();   // RESET timeout : AUTH_BOND 20s fire sinon ici
    needSubscribeControl = true;
    emit("BOND OK");
}

void DexcomBLEBD::sendTimeRequest(){
    // Appelé uniquement depuis tick() (main thread) via needSubscribeControl
    Serial.println("[BLE] Subscribe controlChar INDICATE");
    controlChar->registerForNotify(onControlNotify, false);  // false = INDICATE
    delay(50);  // laisser le CCCD se settle
    uint8_t msg[3]; makeG6Msg(OP_TIME_TX_BD,msg);
    enqueueWrite(msg,3,true);  // control char
    setState(State::TIME_REQ);
}

void DexcomBLEBD::handleTimeRx(const uint8_t* d,size_t n){
    if(d[0]==OP_TIME_RX_BD&&n>=10&&state==State::TIME_REQ){
        uint32_t cur=(uint32_t)d[2]|((uint32_t)d[3]<<8)|((uint32_t)d[4]<<16)|((uint32_t)d[5]<<24);
        uint32_t ss =(uint32_t)d[6]|((uint32_t)d[7]<<8)|((uint32_t)d[8]<<16)|((uint32_t)d[9]<<24);
        lastReading.sensorAge=(cur>ss)?(cur-ss):0;
        emit("TIME OK "+String(lastReading.sensorAge/3600)+"h");
        uint8_t msg[3]; makeG6Msg(OP_GLUCOSE_TX_BD,msg);
        enqueueWrite(msg,3,true);  // control char
        setState(State::GLUCOSE_REQ);
    } else if(d[0]==OP_GLUCOSE_RX_BD&&n>=14&&state==State::GLUCOSE_REQ){
        handleGlucoseRx(d,n);
    }
}

void DexcomBLEBD::handleGlucoseRx(const uint8_t* d,size_t n){
    uint16_t mgdl=((uint16_t)d[10]|((uint16_t)d[11]<<8))&0x0FFF;
    int8_t trend=(int8_t)d[13];
    uint16_t pred=(n>=16)?((uint16_t)d[14]|((uint16_t)d[15]<<8))&0x03FF:0;
    lastReading.mgdl=mgdl; lastReading.mmol=mgdl/18.01559f;
    lastReading.trend=(trend==0x7F)?0:trend;
    lastReading.sensorOk=(d[12]==0x06); lastReading.predicted=pred;
    lastReading.rxMs=millis();
    lastReadingTimestamp = millis();   // alimente le watchdog "no-reading"
    setState(State::READING_DONE);
    if(cbReading) cbReading(lastReading);
}

void DexcomBLEBD::disconnect(){
    if(pClient&&pClient->isConnected()) pClient->disconnect();
    bonded=false;
    // Caractéristiques libérées par BLE lib au disconnect → invalider les pointeurs
    authChar = nullptr;
    controlChar = nullptr;
    needSubscribeControl = false;
}

// Appelé par BLE lib (BTC_TASK) quand le transmetteur disparait inopinément.
// Ne touche pas state directement (race avec tick) ; juste cleanup des flags
// et invalidation des pointeurs. Le timeout dans tick() finalisera la transition.
void DexcomBLEBD::onClientDisconnect() {
    Serial.println("[BLE] onDisconnect callback (transmitter)");
    bonded = false;
    needSubscribeControl = false;
    authChar = nullptr;
    controlChar = nullptr;
    emit("DISCONNECTED");
}
