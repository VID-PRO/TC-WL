#include "ble_timecode.h"
#include "config.h"
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAdvertisedDevice.h>
#include <BLEAdvertising.h>
#include <BLEUUID.h>
#include <BLE2902.h>
#include <Preferences.h>
#include <string>
#include <cstring>
#include <vector>
#include <algorithm>
#if defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE)
#include "esp32-hal-hosted.h"
extern "C" void hci_drv_init(void);
#endif

static Preferences blePrefs;
static const char *NVS_NS = "ble";

// 128-bit custom service / characteristic UUIDs
const BLEUUID bleTimecodeServiceUUID("9a6f0001-5c9a-4b3e-8a2c-f12345678901");
const BLEUUID bleTimecodeCharUUID("9a6f0002-5c9a-4b3e-8a2c-f12345678901");
const BLEUUID bleTimecodeNameCharUUID("9a6f0003-5c9a-4b3e-8a2c-f12345678901");
const BLEUUID bleTimecodeConfigCharUUID("9a6f0004-5c9a-4b3e-8a2c-f12345678901");

static BleConfigCb bleConfigCb = nullptr;
static BleStateCb bleStateCb = nullptr;

void bleTimecodeSetConfigCallback(BleConfigCb cb) { bleConfigCb = cb; }
void bleTimecodeSetStateCallback(BleStateCb cb) { bleStateCb = cb; }

class BleConfigCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        if (!bleConfigCb) return;
        auto raw = pChar->getValue();
        std::string val(raw.c_str(), raw.length());
        size_t colon = val.find(':');
        if (colon == std::string::npos) return;
        std::string cmd = val.substr(0, colon);
        std::string arg = val.substr(colon + 1);
        bleConfigCb(cmd.c_str(), arg.c_str());
    }
    void onRead(BLECharacteristic *pChar) override {
        if (bleStateCb) {
            pChar->setValue(bleStateCb());
        }
    }
};

// =========================================================================
// BLE HDMI — advertises timecode service, sends notifications
// =========================================================================
#if defined(TCWL_HDMI)

static BLEServer *server = nullptr;
static BLECharacteristic *tcChar = nullptr;
static bool deviceConnected = false;
static int hdmiTcSubscribers = 0;
static char bleName[33] = "TC-WL-HDMI";

struct PeerInfo {
    char address[18];
    char name[33];
    uint16_t connId;
    bool subscribed = false;
};
static std::vector<PeerInfo> peers;

class TcCharCallbacks : public BLECharacteristicCallbacks {
    void onSubscribe(BLECharacteristic *pChar, ble_gap_conn_desc *desc, uint16_t subValue) override {
        (void)pChar;
        bool wasSub = false;
        for (auto &p : peers) {
            if (p.connId == desc->conn_handle) {
                wasSub = p.subscribed;
                p.subscribed = (subValue & 0x0001) != 0;
                break;
            }
        }
        bool nowSub = (subValue & 0x0001) != 0;
        if (nowSub && !wasSub) hdmiTcSubscribers++;
        else if (!nowSub && wasSub) hdmiTcSubscribers--;
        if (hdmiTcSubscribers < 0) hdmiTcSubscribers = 0;
    }
};

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer) override {
        deviceConnected = true;
    }
    void onDisconnect(BLEServer *pServer) override {
        // hdmiTcSubscribers left untouched — individual disconnect
        // handlers decrement per-peer.
        deviceConnected = false;
    }

#if defined(CONFIG_BLUEDROID_ENABLED)
    void onConnect(BLEServer *pServer, esp_ble_gatts_cb_param_t *param) override {
        deviceConnected = true;
        PeerInfo pi;
        snprintf(pi.address, sizeof(pi.address),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 param->connect.remote_bda[5], param->connect.remote_bda[4],
                 param->connect.remote_bda[3], param->connect.remote_bda[2],
                 param->connect.remote_bda[1], param->connect.remote_bda[0]);
        pi.name[0] = '\0';
        pi.connId = param->connect.conn_id;
        pi.subscribed = false;
        peers.push_back(pi);
    }
    void onDisconnect(BLEServer *pServer, esp_ble_gatts_cb_param_t *param) override {
        uint16_t cid = param->disconnect.conn_id;
        for (auto it = peers.begin(); it != peers.end(); ++it) {
            if (it->connId == cid) {
                if (it->subscribed) hdmiTcSubscribers--;
                peers.erase(it);
                break;
            }
        }
        if (hdmiTcSubscribers < 0) hdmiTcSubscribers = 0;
        deviceConnected = !peers.empty();
        BLEDevice::getAdvertising()->start();
    }
#endif

#if defined(CONFIG_NIMBLE_ENABLED)
    void onConnect(BLEServer *pServer, ble_gap_conn_desc *desc) override {
        deviceConnected = true;
        PeerInfo pi;
        snprintf(pi.address, sizeof(pi.address),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 desc->peer_id_addr.val[5], desc->peer_id_addr.val[4],
                 desc->peer_id_addr.val[3], desc->peer_id_addr.val[2],
                 desc->peer_id_addr.val[1], desc->peer_id_addr.val[0]);
        pi.name[0] = '\0';
        pi.connId = desc->conn_handle;
        pi.subscribed = false;
        peers.push_back(pi);
        BLEDevice::getAdvertising()->start();
    }
    void onDisconnect(BLEServer *pServer, ble_gap_conn_desc *desc) override {
        uint16_t cid = desc->conn_handle;
        for (auto it = peers.begin(); it != peers.end(); ++it) {
            if (it->connId == cid) {
                if (it->subscribed) hdmiTcSubscribers--;
                peers.erase(it);
                break;
            }
        }
        if (hdmiTcSubscribers < 0) hdmiTcSubscribers = 0;
        deviceConnected = !peers.empty();
        BLEDevice::getAdvertising()->start();
    }
#endif
};

class SlaveNameCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        auto val = pChar->getValue();
        for (auto &p : peers) {
            size_t len = std::min((size_t)val.length(), sizeof(p.name) - 1);
            memcpy(p.name, val.c_str(), len);
            p.name[len] = '\0';
        }
    }

#if defined(CONFIG_BLUEDROID_ENABLED)
    void onWrite(BLECharacteristic *pChar, esp_ble_gatts_cb_param_t *param) override {
        uint16_t cid = param->write.conn_id;
        auto val = pChar->getValue();
        for (auto &p : peers) {
            if (p.connId == cid) {
                size_t len = std::min((size_t)val.length(), sizeof(p.name) - 1);
                memcpy(p.name, val.c_str(), len);
                p.name[len] = '\0';
                break;
            }
        }
    }
#endif

#if defined(CONFIG_NIMBLE_ENABLED)
    void onWrite(BLECharacteristic *pChar, ble_gap_conn_desc *desc) override {
        uint16_t cid = desc->conn_handle;
        auto val = pChar->getValue();
        for (auto &p : peers) {
            if (p.connId == cid) {
                size_t len = std::min((size_t)val.length(), sizeof(p.name) - 1);
                memcpy(p.name, val.c_str(), len);
                p.name[len] = '\0';
                break;
            }
        }
    }
#endif
};

int bleGetMode() { return TCWL_MODE_HDMI; }
void bleSetMode(int mode) {}

const char *bleTimecodeGetName() {
    return bleName;
}

void bleTimecodeSetName(const char *name) {
    strncpy(bleName, name, sizeof(bleName) - 1);
    bleName[sizeof(bleName) - 1] = '\0';
    blePrefs.begin(NVS_NS, false);
    blePrefs.putString("name", bleName);
    blePrefs.end();
}

void bleTimecodeDisconnectAll() {
    if (!server) return;
    for (auto &p : peers) {
        server->disconnect(p.connId);
    }
    peers.clear();
}

void bleTimecodeDisconnectPeer(const char *address) {
    if (!server) return;
    for (auto it = peers.begin(); it != peers.end(); ++it) {
        if (strcmp(it->address, address) == 0) {
            server->disconnect(it->connId);
            peers.erase(it);
            break;
        }
    }
}

uint8_t bleTimecodeConnectedCount() {
    return peers.size();
}

uint8_t bleTimecodeGetPeers(BlePeerInfo *out, uint8_t maxPeers) {
    uint8_t count = 0;
    for (auto &p : peers) {
        if (count >= maxPeers) break;
        strncpy(out[count].address, p.address, sizeof(out[count].address) - 1);
        out[count].address[sizeof(out[count].address) - 1] = '\0';
        strncpy(out[count].name, p.name, sizeof(out[count].name) - 1);
        out[count].name[sizeof(out[count].name) - 1] = '\0';
        out[count].connId = p.connId;
        count++;
    }
    return count;
}

#if defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE) && defined(TCWL_HDMI)
#include <WiFi.h>
static bool updateC6Firmware(const char *host, uint16_t port, const char *path) {
    if (!hostedIsInitialized()) {
        Serial.println("[C6] ESP-Hosted not initialized");
        return false;
    }
    Serial.printf("[C6] Connecting to %s:%u%s\n", host, port, path);
    WiFiClient client;
    if (!client.connect(host, port)) {
        Serial.println("[C6] connect failed");
        return false;
    }
    client.printf("GET %s HTTP/1.0\r\nHost: %s:%u\r\n\r\n", path, host, port);
    unsigned long deadline = millis() + 5000;
    String header;
    while (header.indexOf("\r\n\r\n") < 0 && millis() < deadline) {
        if (client.available()) {
            char c = client.read();
            header += c;
        }
    }
    if (header.indexOf("\r\n\r\n") < 0) {
        Serial.println("[C6] No HTTP response headers");
        client.stop();
        return false;
    }
    int totalLen = 0;
    int idx = header.indexOf("Content-Length:");
    if (idx >= 0) {
        totalLen = atoi(header.c_str() + idx + 15);
    }
    if (header.indexOf("200 OK") < 0) {
        Serial.printf("[C6] HTTP error: %s\n", header.c_str());
        client.stop();
        return false;
    }
    if (totalLen <= 0) {
        Serial.printf("[C6] Invalid Content-Length: %d\n", totalLen);
        client.stop();
        return false;
    }
    Serial.printf("[C6] Firmware size: %d bytes\n", totalLen);
    if (!hostedBeginUpdate()) {
        Serial.println("[C6] hostedBeginUpdate failed");
        client.stop();
        return false;
    }
    uint8_t buf[2048];
    int downloaded = 0;
    while (client.connected() && downloaded < totalLen) {
        size_t avail = client.available();
        if (avail > 0) {
            size_t toRead = (avail > sizeof(buf)) ? sizeof(buf) : avail;
            if ((size_t)(totalLen - downloaded) < toRead) toRead = totalLen - downloaded;
            int readLen = client.readBytes(buf, toRead);
            if (readLen > 0) {
                if (!hostedWriteUpdate(buf, readLen)) {
                    Serial.println("\n[C6] hostedWriteUpdate failed");
                    client.stop();
                    return false;
                }
                downloaded += readLen;
                Serial.print(".");
            }
        }
    }
    Serial.println();
    client.stop();
    if (downloaded < totalLen) {
        Serial.printf("[C6] Incomplete: %d/%d\n", downloaded, totalLen);
        return false;
    }
    if (!hostedEndUpdate()) {
        Serial.println("[C6] hostedEndUpdate failed");
        return false;
    }
    Serial.println("[C6] Update complete, activating...");
    hostedActivateUpdate();
    delay(1000);
    ESP.restart();
    return true;
}
#endif

void bleTimecodeInit() {
    blePrefs.begin(NVS_NS, false);
    String saved = blePrefs.getString("name", "TC-WL-HDMI");
    blePrefs.end();
    strncpy(bleName, saved.c_str(), sizeof(bleName) - 1);
    bleName[sizeof(bleName) - 1] = '\0';

#if defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE) && defined(TCWL_HDMI)
    hci_drv_init();
    Serial.printf("[BLE] hci_drv_init called\n");
#endif

    bool ok = false;
    for (int retry = 0; retry < 3; retry++) {
        ok = BLEDevice::init(bleName);
        if (ok) break;
        delay(500);
    }
    Serial.printf("[BLE] init(%s): %s\n", bleName, ok ? "ok" : "FAIL");
    if (!ok) {
#if defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE) && defined(TCWL_HDMI)
        Serial.println("[C6] Firmware not responding, attempting update...");
        Serial.println("[C6] Connect your computer to AP WiFi, then start:");
        Serial.println("[C6]   python3 -m http.server 8080");
        Serial.println("[C6]   in the directory with esp32c6-v2.12.8.bin");
        for (int i = 30; i > 0; i--) {
            Serial.printf("[C6] %d...\n", i);
            delay(1000);
        }
        updateC6Firmware("192.168.4.2", 8080, "/esp32c6-v2.12.8.bin");
#else
        Serial.println("[BLE] init failed, restarting...");
        delay(1000);
        ESP.restart();
#endif
    }
#if defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE)
    Serial.printf("[BLE] isHostedBLE: %s\n", BLEDevice::isHostedBLE() ? "yes" : "no");
    {
        uint32_t maj, min, pat;
        hostedGetSlaveVersion(&maj, &min, &pat);
        Serial.printf("[BLE] C6 fw: v%" PRIu32 ".%" PRIu32 ".%" PRIu32 "\n", maj, min, pat);
    }
#endif

    server = BLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());

    BLEService *svc = server->createService(bleTimecodeServiceUUID);
    tcChar = svc->createCharacteristic(
        bleTimecodeCharUUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    tcChar->setCallbacks(new TcCharCallbacks());
    uint8_t init[5] = {0, 0, 0, 0, 0};
    tcChar->setValue(init, 5);

    BLECharacteristic *nameChar = svc->createCharacteristic(
        bleTimecodeNameCharUUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    nameChar->setCallbacks(new SlaveNameCallbacks());
    nameChar->setValue("");

    BLECharacteristic *cfgChar = svc->createCharacteristic(
        bleTimecodeConfigCharUUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_READ
    );
    cfgChar->setCallbacks(new BleConfigCallbacks());
    cfgChar->setValue("");

    svc->start();

    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(bleTimecodeServiceUUID);
    adv->setScanResponse(true);
    Serial.println("[BLE] channel_map=0x07 (fixed in NimBLE reset)");
    BLEDevice::startAdvertising();
    Serial.printf("[BLE] startAdvertising called\n");
}

void bleTimecodeUpdate(uint8_t dd, uint8_t hh, uint8_t mm, uint8_t ss, uint8_t ff, uint8_t lockState, uint8_t fps, uint8_t flags, uint8_t batteryPct) {
    if (!deviceConnected || hdmiTcSubscribers == 0) return;
    uint8_t data[9] = {dd, hh, mm, ss, ff, lockState, fps, flags, batteryPct};
    tcChar->setValue(data, 9);
    tcChar->notify();
}

// LTC stubs for HDMI-only build
void bleTimecodeSetCallback(BleTimecodeCb) {}
void bleTimecodePoll() {}
bool bleTimecodeConnected() { return false; }
uint8_t bleTimecodeScan(BleScanResult *, uint8_t) { return 0; }
void bleTimecodeSelect(const char *) {}
void bleTimecodeDisconnect() {}
const char *bleTimecodeSelectedAddress() { return ""; }
const char *bleTimecodeConnectedAddress() { return ""; }
const char *bleTimecodeConnectedName() { return ""; }

// =========================================================================
// BLE LTC — dual role: LTC-input server ("master") or BLE client ("slave")
// =========================================================================
#elif defined(TCWL_LTC)

static int bleLtcRole = TCWL_MODE_LTC;

int bleGetMode() { return bleLtcRole; }
void bleSetMode(int mode) {
    bleLtcRole = mode;
    blePrefs.begin(NVS_NS, false);
    blePrefs.putUChar("role", mode);
    blePrefs.end();
}

// ── Server-side globals (master role) ──
static BLEServer *ltcServer = nullptr;
static BLECharacteristic *ltcTcChar = nullptr;
static bool ltcServerHasClients = false;
static int ltcTcSubscribers = 0;
static char ltcServerName[33] = "TC-WL-LTC";
struct LtcPeerInfo {
    char address[18];
    char name[33];
    uint16_t connId;
    bool subscribed = false;
};
static std::vector<LtcPeerInfo> ltcPeers;

class LtcTcCharCallbacks : public BLECharacteristicCallbacks {
    void onSubscribe(BLECharacteristic *pChar, ble_gap_conn_desc *desc, uint16_t subValue) override {
        (void)pChar;
        bool wasSub = false;
        for (auto &p : ltcPeers) {
            if (p.connId == desc->conn_handle) {
                wasSub = p.subscribed;
                p.subscribed = (subValue & 0x0001) != 0;
                break;
            }
        }
        bool nowSub = (subValue & 0x0001) != 0;
        if (nowSub && !wasSub) ltcTcSubscribers++;
        else if (!nowSub && wasSub) ltcTcSubscribers--;
        if (ltcTcSubscribers < 0) ltcTcSubscribers = 0;
    }
};

class LtcServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *) override {
        ltcServerHasClients = true;
    }
    void onDisconnect(BLEServer *) override {
        // ltcTcSubscribers left untouched — individual disconnect handlers
        // (Bluedroid/NimBLE) properly decrement per-peer.
        ltcServerHasClients = false;
        ltcPeers.clear();
        BLEDevice::getAdvertising()->start();
    }
#if defined(CONFIG_BLUEDROID_ENABLED)
    void onConnect(BLEServer *pServer, esp_ble_gatts_cb_param_t *param) override {
        ltcServerHasClients = true;
        LtcPeerInfo pi;
        snprintf(pi.address, sizeof(pi.address),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 param->connect.remote_bda[5], param->connect.remote_bda[4],
                 param->connect.remote_bda[3], param->connect.remote_bda[2],
                 param->connect.remote_bda[1], param->connect.remote_bda[0]);
        pi.name[0] = '\0';
        pi.connId = param->connect.conn_id;
        pi.subscribed = false;
        ltcPeers.push_back(pi);
    }
    void onDisconnect(BLEServer *pServer, esp_ble_gatts_cb_param_t *param) override {
        uint16_t cid = param->disconnect.conn_id;
        for (auto it = ltcPeers.begin(); it != ltcPeers.end(); ++it) {
            if (it->connId == cid) {
                if (it->subscribed) ltcTcSubscribers--;
                ltcPeers.erase(it);
                break;
            }
        }
        if (ltcTcSubscribers < 0) ltcTcSubscribers = 0;
        ltcServerHasClients = !ltcPeers.empty();
        BLEDevice::getAdvertising()->start();
    }
#endif
#if defined(CONFIG_NIMBLE_ENABLED)
    void onConnect(BLEServer *pServer, ble_gap_conn_desc *desc) override {
        ltcServerHasClients = true;
        LtcPeerInfo pi;
        snprintf(pi.address, sizeof(pi.address),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 desc->peer_id_addr.val[5], desc->peer_id_addr.val[4],
                 desc->peer_id_addr.val[3], desc->peer_id_addr.val[2],
                 desc->peer_id_addr.val[1], desc->peer_id_addr.val[0]);
        pi.name[0] = '\0';
        pi.connId = desc->conn_handle;
        pi.subscribed = false;
        ltcPeers.push_back(pi);
    }
    void onDisconnect(BLEServer *pServer, ble_gap_conn_desc *desc) override {
        uint16_t cid = desc->conn_handle;
        for (auto it = ltcPeers.begin(); it != ltcPeers.end(); ++it) {
            if (it->connId == cid) {
                if (it->subscribed) ltcTcSubscribers--;
                ltcPeers.erase(it);
                break;
            }
        }
        if (ltcTcSubscribers < 0) ltcTcSubscribers = 0;
        ltcServerHasClients = !ltcPeers.empty();
        BLEDevice::getAdvertising()->start();
    }
#endif
};

class LtcNameCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        auto val = pChar->getValue();
        for (auto &p : ltcPeers) {
            size_t len = std::min((size_t)val.length(), sizeof(p.name) - 1);
            memcpy(p.name, val.c_str(), len);
            p.name[len] = '\0';
        }
    }
#if defined(CONFIG_BLUEDROID_ENABLED)
    void onWrite(BLECharacteristic *pChar, esp_ble_gatts_cb_param_t *param) override {
        uint16_t cid = param->write.conn_id;
        auto val = pChar->getValue();
        for (auto &p : ltcPeers) {
            if (p.connId == cid) {
                size_t len = std::min((size_t)val.length(), sizeof(p.name) - 1);
                memcpy(p.name, val.c_str(), len);
                p.name[len] = '\0';
                break;
            }
        }
    }
#endif
#if defined(CONFIG_NIMBLE_ENABLED)
    void onWrite(BLECharacteristic *pChar, ble_gap_conn_desc *desc) override {
        uint16_t cid = desc->conn_handle;
        auto val = pChar->getValue();
        for (auto &p : ltcPeers) {
            if (p.connId == cid) {
                size_t len = std::min((size_t)val.length(), sizeof(p.name) - 1);
                memcpy(p.name, val.c_str(), len);
                p.name[len] = '\0';
                break;
            }
        }
    }
#endif
};

// ── Client-side globals (slave role) ──
static BLEClient *ltcClient = nullptr;
static BLERemoteCharacteristic *ltcRemoteChar = nullptr;
static BleTimecodeCb timecodeCb = nullptr;
static bool ltcClientConnected = false;
static bool scanDone = false;
static unsigned long lastScan = 0;
static char selectedAddr[18] = "";
static char connectedAddr[18] = "";
static char connectedName[33] = "";
static char ltcBleName[33] = "TC-WL-LTC";
static bool pendingConnect = false;
static char pendingAddr[18] = "";
static char pendingName[33] = "";

// Scan results for the OLED master-selection menu
static BleScanResult _scanResults[16];
static uint8_t _scanResultCount = 0;
static bool _scanAsyncDone = false;
static bool _scanAsyncActive = false;

class LtcClientCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient *) override {
        ltcClientConnected = true;
    }
    void onDisconnect(BLEClient *) override {
        ltcClientConnected = false;
        ltcClient = nullptr;
        ltcRemoteChar = nullptr;
        connectedAddr[0] = '\0';
        connectedName[0] = '\0';
    }
};

static void ltcNotifyCallback(BLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
    if (len >= 7 && timecodeCb) {
        timecodeCb(data[0], data[1], data[2], data[3], data[4], data[6]);
    }
}

static bool tryConnectAddr(const char *addrStr, const char *nameStr) {
    String addrStr2(addrStr);
    BLEAddress addr(addrStr2);
    ltcClient = BLEDevice::createClient();
    ltcClient->setClientCallbacks(new LtcClientCallbacks());
    if (!ltcClient->connect(addr)) {
        delete ltcClient;
        ltcClient = nullptr;
        return false;
    }
    delay(500);
    BLERemoteService *svc = ltcClient->getService(bleTimecodeServiceUUID);
    if (!svc) {
        ltcClient->disconnect();
        delete ltcClient;
        ltcClient = nullptr;
        return false;
    }
    delay(200);
    ltcRemoteChar = svc->getCharacteristic(bleTimecodeCharUUID);
    if (!ltcRemoteChar || !ltcRemoteChar->canNotify()) {
        ltcClient->disconnect();
        delete ltcClient;
        ltcClient = nullptr;
        return false;
    }
    delay(200);
    ltcRemoteChar->registerForNotify(ltcNotifyCallback);
    ltcClientConnected = true;

    strncpy(connectedAddr, addrStr, sizeof(connectedAddr) - 1);
    connectedAddr[sizeof(connectedAddr) - 1] = '\0';
    if (nameStr && nameStr[0]) {
        strncpy(connectedName, nameStr, sizeof(connectedName) - 1);
        connectedName[sizeof(connectedName) - 1] = '\0';
    } else {
        snprintf(connectedName, sizeof(connectedName),
                 "TC-WL-HDMI-%s", connectedAddr + 9);
    }

    BLERemoteCharacteristic *nameChar = svc->getCharacteristic(bleTimecodeNameCharUUID);
    if (nameChar && nameChar->canWrite()) {
        nameChar->writeValue((uint8_t *)ltcBleName, strlen(ltcBleName));
    }
    return true;
}

static bool tryConnectDevice(BLEAdvertisedDevice &dev) {
    BLEAddress addr = dev.getAddress();
    char addrStr[18];
    snprintf(addrStr, sizeof(addrStr),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             addr.getNative()[5], addr.getNative()[4],
             addr.getNative()[3], addr.getNative()[2],
             addr.getNative()[1], addr.getNative()[0]);
    const char *dn = dev.getName().c_str();
    return tryConnectAddr(addrStr, (dn && dn[0]) ? dn : nullptr);
}

void bleTimecodeInit() {
    blePrefs.begin(NVS_NS, false);
    bleLtcRole = blePrefs.getUChar("role", TCWL_MODE_LTC);
    String savedName = blePrefs.isKey("ltc_name") ? blePrefs.getString("ltc_name", "") : "TC-WL-LTC";
    String savedAddr = blePrefs.isKey("hdmi_addr") ? blePrefs.getString("hdmi_addr", "") : "";
    blePrefs.end();

    strncpy(ltcServerName, savedName.c_str(), sizeof(ltcServerName) - 1);
    ltcServerName[sizeof(ltcServerName) - 1] = '\0';
    strncpy(ltcBleName, savedName.c_str(), sizeof(ltcBleName) - 1);
    ltcBleName[sizeof(ltcBleName) - 1] = '\0';

    BLEDevice::init(ltcServerName);

    // Always create BLE server so the Android app can connect and send
    // config commands, regardless of master/slave role.
    ltcServer = BLEDevice::createServer();
    ltcServer->setCallbacks(new LtcServerCallbacks());

    BLEService *svc = ltcServer->createService(bleTimecodeServiceUUID);
    ltcTcChar = svc->createCharacteristic(
        bleTimecodeCharUUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    ltcTcChar->setCallbacks(new LtcTcCharCallbacks());
    uint8_t init[5] = {0, 0, 0, 0, 0};
    ltcTcChar->setValue(init, 5);

    BLECharacteristic *nameChar = svc->createCharacteristic(
        bleTimecodeNameCharUUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    nameChar->setCallbacks(new LtcNameCallbacks());
    nameChar->setValue("");

    BLECharacteristic *cfgChar = svc->createCharacteristic(
        bleTimecodeConfigCharUUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_READ
    );
    cfgChar->setCallbacks(new BleConfigCallbacks());
    cfgChar->setValue("");

    svc->start();

    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(bleTimecodeServiceUUID);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    adv->setMaxPreferred(0x12);
    BLEDevice::startAdvertising();

    if (bleLtcRole != TCWL_MODE_LTC_MASTER) {
        if (savedAddr.length()) {
            strncpy(selectedAddr, savedAddr.c_str(), sizeof(selectedAddr) - 1);
            selectedAddr[sizeof(selectedAddr) - 1] = '\0';
        }
    }
}

void bleTimecodeSetCallback(BleTimecodeCb cb) {
    timecodeCb = cb;
}

static void ltcScanCompleteCb(BLEScanResults scanResults) {
    scanDone = false;
    // Only auto-connect to a previously-paired address.  Without a saved
    // address the user must select a master from the OLED menu.
    if (!selectedAddr[0]) return;

    for (int i = 0; i < scanResults.getCount(); i++) {
        BLEAdvertisedDevice dev = scanResults.getDevice(i);
        if (!dev.haveServiceUUID() || !dev.isAdvertisingService(bleTimecodeServiceUUID))
            continue;

        BLEAddress addr = dev.getAddress();
        char addrStr[18];
        snprintf(addrStr, sizeof(addrStr),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 addr.getNative()[5], addr.getNative()[4],
                 addr.getNative()[3], addr.getNative()[2],
                 addr.getNative()[1], addr.getNative()[0]);

        if (strcmp(addrStr, selectedAddr) != 0) continue;

        strncpy(pendingAddr, addrStr, sizeof(pendingAddr) - 1);
        pendingAddr[sizeof(pendingAddr) - 1] = '\0';
        const char *dn = dev.getName().c_str();
        if (dn && dn[0]) {
            strncpy(pendingName, dn, sizeof(pendingName) - 1);
            pendingName[sizeof(pendingName) - 1] = '\0';
        } else {
            pendingName[0] = '\0';
        }
        pendingConnect = true;
        break;
    }
}

void bleTimecodePoll() {
    if (bleLtcRole != TCWL_MODE_LTC) return;
    if (ltcClientConnected || scanDone || _scanAsyncActive) return;

    if (pendingConnect) {
        pendingConnect = false;
        if (tryConnectAddr(pendingAddr, pendingName)) return;
    }

    unsigned long now = millis();
    if (now - lastScan < 5000) return;
    lastScan = now;

    if (ltcClient) {
        ltcClient->disconnect();
        delete ltcClient;
        ltcClient = nullptr;
        ltcRemoteChar = nullptr;
    }

    BLEScan *scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(nullptr);
    scan->setActiveScan(true);
    scan->setInterval(200);
    scan->setWindow(100);
    scanDone = scan->start(2, ltcScanCompleteCb, false);
}

bool bleTimecodeConnected() {
    if (bleLtcRole == TCWL_MODE_LTC_MASTER) return ltcServerHasClients;
    return ltcClientConnected;
}

uint8_t bleTimecodeScan(BleScanResult *results, uint8_t maxResults) {
    if (bleLtcRole != TCWL_MODE_LTC) return 0;
    BLEScan *scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(nullptr);
    scan->setActiveScan(true);
    scan->setInterval(200);
    scan->setWindow(100);
    BLEScanResults *scanResults = scan->start(3, false);

    uint8_t count = 0;
    for (int i = 0; i < scanResults->getCount() && count < maxResults; i++) {
        BLEAdvertisedDevice dev = scanResults->getDevice(i);
        if (!dev.haveServiceUUID() || !dev.isAdvertisingService(bleTimecodeServiceUUID))
            continue;

        BLEAddress addr = dev.getAddress();
        snprintf(results[count].address, sizeof(results[count].address),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 addr.getNative()[5], addr.getNative()[4],
                 addr.getNative()[3], addr.getNative()[2],
                 addr.getNative()[1], addr.getNative()[0]);

        {
            String nameStr = dev.getName();
            if (nameStr.length() == 0) {
                uint8_t *payload = dev.getPayload();
                size_t plen = dev.getPayloadLength();
                for (size_t off = 0; off + 1 < plen; ) {
                    uint8_t len = payload[off];
                    if (len == 0 || off + len >= plen) break;
                    uint8_t type = payload[off + 1];
                    if (type == 0x08 || type == 0x09) {
                        nameStr = String((const char *)payload + off + 2, len - 1);
                        break;
                    }
                    off += len + 1;
                }
            }
            strncpy(results[count].name, nameStr.length() > 0 ? nameStr.c_str() : results[count].address,
                    sizeof(results[count].name) - 1);
            results[count].name[sizeof(results[count].name) - 1] = '\0';
        }
        count++;
    }
    scan->clearResults();
    return count;
}

static void masterScanCb(BLEScanResults scanResults) {
    _scanResultCount = 0;
    for (int i = 0; i < scanResults.getCount() && _scanResultCount < 16; i++) {
        BLEAdvertisedDevice dev = scanResults.getDevice(i);
        if (!dev.haveServiceUUID() || !dev.isAdvertisingService(bleTimecodeServiceUUID))
            continue;
        BleScanResult &r = _scanResults[_scanResultCount];
        BLEAddress addr = dev.getAddress();
        snprintf(r.address, sizeof(r.address),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 addr.getNative()[5], addr.getNative()[4],
                 addr.getNative()[3], addr.getNative()[2],
                 addr.getNative()[1], addr.getNative()[0]);
        String nameStr = dev.getName();
        if (nameStr.length() == 0) {
            uint8_t *payload = dev.getPayload();
            size_t plen = dev.getPayloadLength();
            for (size_t off = 0; off + 1 < plen; ) {
                uint8_t len = payload[off];
                if (len == 0 || off + len >= plen) break;
                uint8_t type = payload[off + 1];
                if (type == 0x08 || type == 0x09) {
                    nameStr = String((const char *)payload + off + 2, len - 1);
                    break;
                }
                off += len + 1;
            }
        }
        if (nameStr.length() > 0) {
            strncpy(r.name, nameStr.c_str(), sizeof(r.name) - 1);
            r.name[sizeof(r.name) - 1] = '\0';
        } else {
            strncpy(r.name, r.address, sizeof(r.name) - 1);
        }
        _scanResultCount++;
    }
    BLEDevice::getScan()->clearResults();
    _scanAsyncDone = true;
    _scanAsyncActive = false;
}

void bleTimecodeStartScan() {
    if (bleLtcRole != TCWL_MODE_LTC || _scanAsyncActive) return;
    // Disconnect from current master so it resumes advertising and we can
    // discover it (BLE stops advertising once a connection is established).
    if (ltcClient) {
        ltcClient->disconnect();
        delete ltcClient;
        ltcClient = nullptr;
        ltcRemoteChar = nullptr;
        ltcClientConnected = false;
        connectedAddr[0] = '\0';
        connectedName[0] = '\0';
    }
    _scanResultCount = 0;
    _scanAsyncDone = false;
    _scanAsyncActive = true;
    // Reset auto-connect timer so the next bleTimecodePoll() doesn't race.
    lastScan = 0;
    BLEScan *scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(nullptr);
    scan->setActiveScan(true);
    scan->setInterval(200);
    scan->setWindow(100);
    scan->start(2, masterScanCb, false);
}

bool bleTimecodeScanDone() {
    return _scanAsyncDone;
}

uint8_t bleTimecodeScanResults(BleScanResult *results, uint8_t maxResults) {
    uint8_t n = min(_scanResultCount, maxResults);
    for (uint8_t i = 0; i < n; i++) results[i] = _scanResults[i];
    return n;
}

void bleTimecodeSelect(const char *address) {
    if (bleLtcRole != TCWL_MODE_LTC) return;
    strncpy(selectedAddr, address, sizeof(selectedAddr) - 1);
    selectedAddr[sizeof(selectedAddr) - 1] = '\0';

    blePrefs.begin(NVS_NS, false);
    blePrefs.putString("hdmi_addr", selectedAddr);
    blePrefs.end();

    if (ltcClient) {
        ltcClient->disconnect();
        delete ltcClient;
        ltcClient = nullptr;
        ltcRemoteChar = nullptr;
    }
    ltcClientConnected = false;
    lastScan = 0;
}

void bleTimecodeDisconnect() {
    if (bleLtcRole != TCWL_MODE_LTC) return;
    if (ltcClient) {
        ltcClient->disconnect();
        delete ltcClient;
        ltcClient = nullptr;
        ltcRemoteChar = nullptr;
    }
    ltcClientConnected = false;
    connectedAddr[0] = '\0';
    connectedName[0] = '\0';
    scanDone = false;
}

const char *bleTimecodeSelectedAddress() {
    return selectedAddr;
}

const char *bleTimecodeConnectedAddress() {
    return connectedAddr;
}

const char *bleTimecodeConnectedName() {
    return connectedName;
}

void bleTimecodeSetName(const char *name) {
    strncpy(ltcBleName, name, sizeof(ltcBleName) - 1);
    ltcBleName[sizeof(ltcBleName) - 1] = '\0';
    strncpy(ltcServerName, name, sizeof(ltcServerName) - 1);
    ltcServerName[sizeof(ltcServerName) - 1] = '\0';
    blePrefs.begin(NVS_NS, false);
    blePrefs.putString("ltc_name", ltcBleName);
    blePrefs.end();
}

const char *bleTimecodeGetName() {
    return ltcBleName;
}

void bleTimecodeUpdate(uint8_t dd, uint8_t hh, uint8_t mm, uint8_t ss, uint8_t ff, uint8_t lockState, uint8_t fps, uint8_t flags, uint8_t batteryPct) {
    if (!ltcServerHasClients || ltcTcSubscribers == 0) return;
    uint8_t data[9] = {dd, hh, mm, ss, ff, lockState, fps, flags, batteryPct};
    ltcTcChar->setValue(data, 9);
    ltcTcChar->notify();
}

void bleTimecodeDisconnectAll() {
    if (bleLtcRole != TCWL_MODE_LTC_MASTER || !ltcServer) return;
    for (auto &p : ltcPeers) {
        ltcServer->disconnect(p.connId);
    }
    ltcPeers.clear();
}

void bleTimecodeDisconnectPeer(const char *address) {
    if (bleLtcRole != TCWL_MODE_LTC_MASTER || !ltcServer) return;
    for (auto it = ltcPeers.begin(); it != ltcPeers.end(); ++it) {
        if (strcmp(it->address, address) == 0) {
            ltcServer->disconnect(it->connId);
            ltcPeers.erase(it);
            break;
        }
    }
}

uint8_t bleTimecodeConnectedCount() {
    return ltcPeers.size();
}

uint8_t bleTimecodeGetPeers(BlePeerInfo *out, uint8_t maxPeers) {
    if (bleLtcRole != TCWL_MODE_LTC_MASTER) return 0;
    uint8_t count = 0;
    for (auto &p : ltcPeers) {
        if (count >= maxPeers) break;
        strncpy(out[count].address, p.address, sizeof(out[count].address) - 1);
        out[count].address[sizeof(out[count].address) - 1] = '\0';
        strncpy(out[count].name, p.name, sizeof(out[count].name) - 1);
        out[count].name[sizeof(out[count].name) - 1] = '\0';
        out[count].connId = p.connId;
        count++;
    }
    return count;
}

#endif
