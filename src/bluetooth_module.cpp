// ============================================================
//  bluetooth_module.cpp  —  BLE Multi-Role Module v1.0
//  Supports: Scanner, Central (smartwatch), Peripheral,
//            HID Keyboard, HID Media, HID Gamepad
// ============================================================
#include "bluetooth_module.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEClient.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEHIDDevice.h>
#include <BLE2902.h>           // Feature #P: CCCD notify descriptor
#include <HIDTypes.h>
#include <HIDKeyboardTypes.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_gap_ble_api.h>   // Feature #8: clearAllBonds NVS access
#include <sys/time.h>          // Feature #13: settimeofday

// BLEHIDDevice.h defines several HID device-class macros (HID_GAMEPAD,
// HID_KEYBOARD, HID_MOUSE, HID_JOYSTICK …) that would mangle our enum
// names below. Undef the ones our enums use; the BLE library itself has
// already consumed them above and doesn't need them anymore in this TU.
#undef HID_GAMEPAD
#undef HID_KEYBOARD
#undef HID_MOUSE

// ── Global instance ───────────────────────────────────────────
BluetoothModule btModule;

// ── Static pointer to module for callbacks ───────────────────
static BluetoothModule* s_bt = nullptr;

// ── BLE Advertised Device Callback (scanner) ─────────────────
class BtScanCallback : public BLEAdvertisedDeviceCallbacks {
public:
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (!s_bt) return;
        BleScanResult r;
        r.address     = advertisedDevice.getAddress().toString().c_str();
        r.name        = advertisedDevice.getName().c_str();
        r.rssi        = advertisedDevice.getRSSI();
        r.connectable = advertisedDevice.isAdvertisingService(BLEUUID((uint16_t)0x1800))
                        || advertisedDevice.haveName();

        // Detect services
        if (advertisedDevice.haveServiceUUID()) {
            int n = advertisedDevice.getServiceUUIDCount();
            for (int i = 0; i < n; i++) {
                String u = advertisedDevice.getServiceUUID(i).toString().c_str();
                u.toLowerCase();
                if (r.rawServices.length()) r.rawServices += ",";
                r.rawServices += u;
                if (u.indexOf("180d") >= 0) r.hasHeartRate   = true;
                if (u.indexOf("1809") >= 0) r.hasHealthTherm = true;
                if (u.indexOf("180f") >= 0) r.hasBattery     = true;
                if (u.indexOf("180a") >= 0) r.hasDeviceInfo  = true;
            }
        }

        // Heuristics
        String nm = r.name;
        nm.toLowerCase();
        r.isWatch  = r.hasHeartRate || r.hasBattery ||
                     nm.indexOf("watch") >= 0 || nm.indexOf("band") >= 0 ||
                     nm.indexOf("fit") >= 0   || nm.indexOf("mi ") >= 0 ||
                     nm.indexOf("amazfit") >= 0|| nm.indexOf("huawei") >= 0 ||
                     nm.indexOf("garmin") >= 0 || nm.indexOf("polar") >= 0;
        r.isPhone  = nm.indexOf("iphone") >= 0 || nm.indexOf("android") >= 0 ||
                     nm.indexOf("phone") >= 0  || nm.indexOf("galaxy") >= 0;
        r.isESP32  = nm.indexOf("esp32") >= 0  || nm.indexOf("esp-") >= 0 ||
                     nm.indexOf("ir-remote") >= 0;

        // Deduplicate by address
        auto& results = const_cast<std::vector<BleScanResult>&>(s_bt->scanResults());
        for (auto& existing : results) {
            if (existing.address == r.address) {
                existing = r;  // update with fresh data
                return;
            }
        }
        if (results.size() < BLE_MAX_DEVICES)
            results.push_back(r);
    }
};

// ── Server callbacks (peripheral mode) ────────────────────────
class BtServerCallbacks : public BLEServerCallbacks {
public:
    void onConnect(BLEServer* pServer) override {
        if (s_bt) s_bt->_advertising = false;
    }
    void onDisconnect(BLEServer* pServer) override {
        if (s_bt) {
            s_bt->_advertising = true;
            // Restart advertising so new clients can connect
            BLEDevice::startAdvertising();
        }
    }
};

// ── Client callbacks (central mode) ───────────────────────────
class BtClientCallbacks : public BLEClientCallbacks {
public:
    void onConnect(BLEClient* pClient) override {
        if (s_bt) s_bt->_connected = true;
    }
    void onDisconnect(BLEClient* pClient) override {
        if (s_bt) {
            s_bt->_connected = false;
            s_bt->_watchData.connected = false;
        }
    }
};

// ── HR notification callback ──────────────────────────────────
void BluetoothModule::_onHrNotify(void* chr, uint8_t* data, size_t len, bool notify) {
    if (!s_bt || len < 2) return;
    // Byte 0: flags. Bit 0 = 1 means HR is uint16, else uint8
    bool isU16 = (data[0] & 0x01);
    int hr = isU16 ? (data[1] | (data[2] << 8)) : data[1];
    s_bt->_watchData.heartRate  = hr;
    s_bt->_watchData.lastUpdate = millis();
}

// ── Temperature notification callback ────────────────────────
void BluetoothModule::_onTempNotify(void* chr, uint8_t* data, size_t len, bool notify) {
    if (!s_bt || len < 5) return;
    // IEEE-11073 float: byte 1-4
    int32_t mantissa = (int32_t)(((int32_t)data[3] << 16) |
                                 ((uint32_t)data[2] << 8)  |
                                  (uint32_t)data[1]);
    int8_t  exponent = (int8_t)data[4];
    float   temp = mantissa * powf(10.0f, exponent);
    s_bt->_watchData.temperature = temp;
    s_bt->_watchData.lastUpdate  = millis();
}

// ─────────────────────────────────────────────────────────────
void BluetoothModule::begin() {
    s_bt = this;
    // PROXY FIX: create the proxy pointer-guard mutex once, up front, so
    // _proxyForward* and proxyStop() can both rely on it being available.
    if (!_proxyMutex) _proxyMutex = xSemaphoreCreateMutex();
    _loadConfig();
    _loadBonds();   // Feature #8: load bonded devices from LittleFS
    if (_cfg.enabled) {
        _initStack();
        applyRole(_cfg.role);
    }
}

void BluetoothModule::loop() {
    if (!_cfg.enabled) return;

    // Auto-stop scan after timeout
    if (_scanning && (millis() - _scanStartMs > (uint32_t)BLE_SCAN_SECONDS * 1000 + 2000)) {
        stopScan();
    }

    // Auto-reconnect in central mode
    if (_cfg.role == BleRole::CENTRAL && _cfg.autoConnect &&
        !_connected && !_scanning && _cfg.lastAddress.length() > 0) {
        static unsigned long _lastReconnect = 0;
        if (millis() - _lastReconnect > 15000) {
            _lastReconnect = millis();
            connect(_cfg.lastAddress);
        }
    }

    // Feature #P: proxy state machine tick
    if (_cfg.role == BleRole::PROXY) _proxyLoop();

    // Feature #4: push WS status event when something changes
    broadcastBleStatus();
}

// ── Config ───────────────────────────────────────────────────
void BluetoothModule::_loadConfig() {
    if (!LittleFS.exists(BLE_CONFIG_FILE)) return;
    File f = LittleFS.open(BLE_CONFIG_FILE, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok) { f.close(); return; }
    f.close();
    _cfg.enabled      = doc["enabled"]     | false;
    _cfg.role         = (BleRole)(doc["role"] | 0);
    _cfg.deviceName   = doc["name"]         | "ESP32-BLE";
    _cfg.autoConnect  = doc["autoConnect"]  | false;
    _cfg.lastAddress  = doc["lastAddr"]     | "";
    _cfg.bondingOnly  = doc["bondingOnly"]  | false;  // Feature #8
}

void BluetoothModule::saveConfig(const BleConfig& cfg) {
    _cfg = cfg;
    JsonDocument doc;
    doc["enabled"]     = cfg.enabled;
    doc["role"]        = (uint8_t)cfg.role;
    doc["name"]        = cfg.deviceName;
    doc["autoConnect"] = cfg.autoConnect;
    doc["lastAddr"]    = cfg.lastAddress;
    doc["bondingOnly"] = cfg.bondingOnly;   // Feature #8
    File f = LittleFS.open(BLE_CONFIG_FILE, "w");
    if (f) { serializeJson(doc, f); f.close(); }
    else Serial.printf("[BLE] open(%s,w) failed\n", BLE_CONFIG_FILE);
}

// ── Stack init ────────────────────────────────────────────────
void BluetoothModule::_initStack() {
    if (BLEDevice::getInitialized()) return;
    BLEDevice::init(_cfg.deviceName.c_str());
    BLEDevice::setPower(ESP_PWR_LVL_P9);   // max TX power
}

void BluetoothModule::_stopAll() {
    stopScan();
    if (_connected) disconnect();
    _advertising = false;
    _hidPeerConnected = false;
}

void BluetoothModule::applyRole(BleRole role) {
    _cfg.role = role;
    if (!_cfg.enabled) return;
    _initStack();
    _stopAll();
    switch (role) {
        case BleRole::SCANNER:
        case BleRole::CENTRAL:
            _startCentral();
            break;
        case BleRole::PERIPHERAL:
            _startPeripheral();
            break;
        case BleRole::HID_KB:
        case BleRole::HID_MEDIA:
        case BleRole::HID_GAMEPAD:
            _startHid(role);
            break;
        case BleRole::PROXY:
            // Proxy starts via proxyStart() — needs watch address
            // Just init stack here; actual start triggered by API
            _initStack();
            break;
        default: break;
    }
}

// ── Scanner / Central ─────────────────────────────────────────
void BluetoothModule::_startCentral() {
    // Nothing special — scan on demand
}

void BluetoothModule::startScan(uint8_t durationSec) {
    if (_scanning || _cfg.role == BleRole::DISABLED) return;
    _initStack();
    _scanResults.clear();
    BLEScan* scan = BLEDevice::getScan();
    static BtScanCallback scanCb;
    scan->setAdvertisedDeviceCallbacks(&scanCb, false);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    scan->start(durationSec, false);
    _scanning    = true;
    _scanStartMs = millis();
}

void BluetoothModule::stopScan() {
    if (!_scanning) return;
    BLEDevice::getScan()->stop();
    BLEDevice::getScan()->clearResults();
    _scanning = false;
}

bool BluetoothModule::connect(const String& address) {
    if (_connected) disconnect();
    _initStack();
    BLEAddress bleAddr(address.c_str());
    BLEClient* client = BLEDevice::createClient();
    static BtClientCallbacks clientCb;
    client->setClientCallbacks(&clientCb);
    // setConnectionParams() was removed in arduino-esp32 2.x BLE lib.
    // Default conn interval (~30-50 ms) is used. To re-tune, call
    // esp_ble_gap_update_conn_params() via raw IDF after connect.
    client->setMTU(247);

    if (!client->connect(bleAddr)) {
        delete client;   // lib doesn't expose deleteClient(); free directly
        return false;
    }

    _connected = true;
    _pClient   = client;
    _cfg.lastAddress = address;

    // Populate watch data basics
    _watchData.address   = address;
    _watchData.connected = true;
    _watchData.lastUpdate= millis();

    // Read GATT services
    _readGattServices();
    _subscribeNotifications();
    return true;
}

void BluetoothModule::disconnect() {
    if (!_connected || !_pClient) return;
    BLEClient* c = reinterpret_cast<BLEClient*>(_pClient);
    if (c->isConnected()) c->disconnect();
    delete c;   // free the BLEClient allocated by BLEDevice::createClient()
    _pClient   = nullptr;
    _connected = false;
    _watchData.connected = false;
}

bool BluetoothModule::_readGattServices() {
    if (!_pClient) return false;
    BLEClient* c = reinterpret_cast<BLEClient*>(_pClient);

    // ── Device Info ───────────────────────────────────────────
    BLERemoteService* svc = c->getService(BLEUUID(UUID_SVC_DEVICE_INFO));
    if (svc) {
        auto* chr = svc->getCharacteristic(BLEUUID(UUID_CHR_MANUFACTURER));
        if (chr && chr->canRead()) _watchData.manufacturer = chr->readValue().c_str();
        chr = svc->getCharacteristic(BLEUUID(UUID_CHR_MODEL_NUMBER));
        if (chr && chr->canRead()) _watchData.model        = chr->readValue().c_str();
        chr = svc->getCharacteristic(BLEUUID(UUID_CHR_FIRMWARE_REV));
        if (chr && chr->canRead()) _watchData.firmwareRev  = chr->readValue().c_str();
    }

    // ── Battery ───────────────────────────────────────────────
    svc = c->getService(BLEUUID(UUID_SVC_BATTERY));
    if (svc) {
        auto* chr = svc->getCharacteristic(BLEUUID(UUID_CHR_BATTERY_LEVEL));
        if (chr && chr->canRead()) {
            std::string v = chr->readValue();
            if (!v.empty()) _watchData.battery = (uint8_t)v[0];
        }
    }

    // ── Device name ───────────────────────────────────────────
    svc = c->getService(BLEUUID(UUID_SVC_GENERIC_ACCESS));
    if (svc) {
        auto* chr = svc->getCharacteristic(BLEUUID(UUID_CHR_DEVICE_NAME));
        if (chr && chr->canRead() && _watchData.name.isEmpty())
            _watchData.name = chr->readValue().c_str();
    }

    return true;
}

void BluetoothModule::_subscribeNotifications() {
    if (!_pClient) return;
    BLEClient* c = reinterpret_cast<BLEClient*>(_pClient);

    // ── Heart Rate notifications ──────────────────────────────
    BLERemoteService* svc = c->getService(BLEUUID(UUID_SVC_HEART_RATE));
    if (svc) {
        auto* chr = svc->getCharacteristic(BLEUUID(UUID_CHR_HR_MEASUREMENT));
        if (chr && chr->canNotify()) {
            chr->registerForNotify(
                [](BLERemoteCharacteristic* c, uint8_t* d, size_t l, bool n){
                    BluetoothModule::_onHrNotify(c, d, l, n);
                });
            _watchData.hrNotifying = true;
        }
    }

    // ── Health Thermometer (temp / SpO2 proxy on some devices) ─
    svc = c->getService(BLEUUID(UUID_SVC_HEALTH_THERM));
    if (svc) {
        auto* chr = svc->getCharacteristic(BLEUUID("00002a1c-0000-1000-8000-00805f9b34fb"));
        if (chr && chr->canIndicate()) {
            chr->registerForNotify(
                [](BLERemoteCharacteristic* c, uint8_t* d, size_t l, bool n){
                    BluetoothModule::_onTempNotify(c, d, l, n);
                }, false);
            _watchData.tempNotifying = true;
        }
    }
}

// ── Peripheral ────────────────────────────────────────────────
void BluetoothModule::_startPeripheral() {
    _initStack();
    BLEServer* srv = BLEDevice::createServer();
    static BtServerCallbacks srvCb;
    srv->setCallbacks(&srvCb);

    // Generic Info service
    BLEService* infoSvc = srv->createService(BLEUUID(UUID_SVC_DEVICE_INFO));
    BLECharacteristic* mfr = infoSvc->createCharacteristic(
        BLEUUID(UUID_CHR_MANUFACTURER),
        BLECharacteristic::PROPERTY_READ);
    mfr->setValue("ESP32-IR-Remote");
    BLECharacteristic* mdl = infoSvc->createCharacteristic(
        BLEUUID(UUID_CHR_MODEL_NUMBER),
        BLECharacteristic::PROPERTY_READ);
    mdl->setValue("v8.0.0");
    infoSvc->start();

    // Start advertising
    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(BLEUUID(UUID_SVC_DEVICE_INFO));
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    adv->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
    _advertising = true;
    _pServer = srv;
}

// ── HID ───────────────────────────────────────────────────────
void BluetoothModule::_startHid(BleRole hidRole) {
    _initStack();
    BLEServer* srv = BLEDevice::createServer();
    static BtServerCallbacks srvCb;
    srv->setCallbacks(&srvCb);

    BLEHIDDevice* hid = new BLEHIDDevice(srv);
    _pHidDevice = hid;

    // Set device info
    hid->manufacturer()->setValue("Espressif");
    hid->pnp(0x02, 0x045E, 0x02FD, 0x0110);
    hid->hidInfo(0x00, 0x01);

    // Minimal HID descriptors
    if (hidRole == BleRole::HID_KB) {
        // Keyboard descriptor
        static const uint8_t kbDesc[] = {
            0x05, 0x01,  // USAGE_PAGE (Generic Desktop)
            0x09, 0x06,  // USAGE (Keyboard)
            0xA1, 0x01,  // COLLECTION (Application)
            0x85, 0x01,  //   REPORT_ID (1)
            0x05, 0x07,  //   USAGE_PAGE (Key Codes)
            0x19, 0xE0,  //   USAGE_MIN (224)
            0x29, 0xE7,  //   USAGE_MAX (231)
            0x15, 0x00,  //   LOGICAL_MIN (0)
            0x25, 0x01,  //   LOGICAL_MAX (1)
            0x75, 0x01,  //   REPORT_SIZE (1)
            0x95, 0x08,  //   REPORT_COUNT (8)
            0x81, 0x02,  //   INPUT (Data,Var,Abs) - modifiers
            0x95, 0x01,  //   REPORT_COUNT (1)
            0x75, 0x08,  //   REPORT_SIZE (8)
            0x81, 0x03,  //   INPUT (Cnst,Var,Abs) - reserved
            0x95, 0x06,  //   REPORT_COUNT (6)
            0x75, 0x08,  //   REPORT_SIZE (8)
            0x15, 0x00,  //   LOGICAL_MIN (0)
            0x25, 0x65,  //   LOGICAL_MAX (101)
            0x05, 0x07,  //   USAGE_PAGE (Key Codes)
            0x19, 0x00,  //   USAGE_MIN (0)
            0x29, 0x65,  //   USAGE_MAX (101)
            0x81, 0x00,  //   INPUT (Data,Array)
            0xC0         // END_COLLECTION
        };
        hid->reportMap((uint8_t*)kbDesc, sizeof(kbDesc));
    } else if (hidRole == BleRole::HID_MEDIA) {
        // Consumer Control descriptor
        static const uint8_t mediaDesc[] = {
            0x05, 0x0C,  // USAGE_PAGE (Consumer)
            0x09, 0x01,  // USAGE (Consumer Control)
            0xA1, 0x01,  // COLLECTION (Application)
            0x85, 0x02,  //   REPORT_ID (2)
            0x15, 0x00,  //   LOGICAL_MIN (0)
            0x25, 0x01,  //   LOGICAL_MAX (1)
            0x75, 0x01,  //   REPORT_SIZE (1)
            0x95, 0x10,  //   REPORT_COUNT (16)
            0x09, 0xB5,  //   USAGE (Scan Next Track)
            0x09, 0xB6,  //   USAGE (Scan Prev Track)
            0x09, 0xB7,  //   USAGE (Stop)
            0x09, 0xCD,  //   USAGE (Play/Pause)
            0x09, 0xE9,  //   USAGE (Vol+)
            0x09, 0xEA,  //   USAGE (Vol-)
            0x09, 0xE2,  //   USAGE (Mute)
            0x09, 0xB3,  //   USAGE (Fast Forward)
            0x09, 0xB4,  //   USAGE (Rewind)
            0x0A, 0x83, 0x01, // USAGE (AL Media Select)
            0x0A, 0x8A, 0x01, // USAGE (AL Email)
            0x0A, 0x94, 0x01, // USAGE (AL Internet Browser)
            0x09, 0x94,  //   USAGE (Stop/Eject)
            0x09, 0x00,  //   USAGE (Undefined/padding)
            0x09, 0x00,
            0x09, 0x00,
            0x81, 0x02,  //   INPUT (Data,Var,Abs)
            0xC0         // END_COLLECTION
        };
        hid->reportMap((uint8_t*)mediaDesc, sizeof(mediaDesc));
    } else if (hidRole == BleRole::HID_GAMEPAD) {
        // Gamepad descriptor
        static const uint8_t gpDesc[] = {
            0x05, 0x01,  // USAGE_PAGE (Generic Desktop)
            0x09, 0x05,  // USAGE (Gamepad)
            0xA1, 0x01,  // COLLECTION (Application)
            0x85, 0x03,  //   REPORT_ID (3)
            // Buttons
            0x05, 0x09,  //   USAGE_PAGE (Buttons)
            0x19, 0x01,  //   USAGE_MIN (1)
            0x29, 0x10,  //   USAGE_MAX (16)
            0x15, 0x00,  //   LOGICAL_MIN (0)
            0x25, 0x01,  //   LOGICAL_MAX (1)
            0x75, 0x01,  //   REPORT_SIZE (1)
            0x95, 0x10,  //   REPORT_COUNT (16)
            0x81, 0x02,  //   INPUT (Data,Var,Abs)
            // Axes
            0x05, 0x01,  //   USAGE_PAGE (Generic Desktop)
            0x09, 0x30,  //   USAGE (X)
            0x09, 0x31,  //   USAGE (Y)
            0x09, 0x32,  //   USAGE (Rx)
            0x09, 0x35,  //   USAGE (Ry)
            0x15, 0x81,  //   LOGICAL_MIN (-127)
            0x25, 0x7F,  //   LOGICAL_MAX (127)
            0x75, 0x08,  //   REPORT_SIZE (8)
            0x95, 0x04,  //   REPORT_COUNT (4)
            0x81, 0x02,  //   INPUT (Data,Var,Abs)
            0xC0         // END_COLLECTION
        };
        hid->reportMap((uint8_t*)gpDesc, sizeof(gpDesc));
    }

    hid->startServices();

    // Security & pairing
    BLESecurity* sec = new BLESecurity();
    sec->setAuthenticationMode(ESP_LE_AUTH_BOND);
    sec->setCapability(ESP_IO_CAP_NONE);
    sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    // arduino-esp32 2.x: BLEHIDDevice no longer exposes startAdvertising().
    // Use the global advertising object instead.
    BLEAdvertising* adv = BLEDevice::getAdvertising();
    BLEDevice::startAdvertising();
    (void)adv;   // suppress unused-var warning if compiled with -Wall
    _advertising = true;
    _pServer   = srv;
    _pHidDevice= hid;
}

// ── HID send ─────────────────────────────────────────────────
void BluetoothModule::hidSendKey(uint8_t keyCode, uint8_t modifier) {
    if (!_pHidDevice || _cfg.role != BleRole::HID_KB) return;
    BLEHIDDevice* hid = reinterpret_cast<BLEHIDDevice*>(_pHidDevice);
    BLECharacteristic* input = hid->inputReport(1);
    if (!input) return;
    uint8_t report[8] = { modifier, 0, keyCode, 0, 0, 0, 0, 0 };
    input->setValue(report, 8);
    input->notify();
    // Release
    memset(report, 0, 8);
    input->setValue(report, 8);
    input->notify();
}

void BluetoothModule::hidSendMedia(uint8_t usage) {
    if (!_pHidDevice || _cfg.role != BleRole::HID_MEDIA) return;
    BLEHIDDevice* hid = reinterpret_cast<BLEHIDDevice*>(_pHidDevice);
    BLECharacteristic* input = hid->inputReport(2);
    if (!input) return;
    uint16_t report = (1 << usage) & 0xFFFF;
    uint8_t buf[2] = { (uint8_t)(report & 0xFF), (uint8_t)(report >> 8) };
    input->setValue(buf, 2);
    input->notify();
    // Release
    buf[0] = buf[1] = 0;
    input->setValue(buf, 2);
    input->notify();
}

void BluetoothModule::hidSendGamepad(const GamepadState& gs) {
    if (!_pHidDevice || _cfg.role != BleRole::HID_GAMEPAD) return;
    BLEHIDDevice* hid = reinterpret_cast<BLEHIDDevice*>(_pHidDevice);
    BLECharacteristic* input = hid->inputReport(3);
    if (!input) return;
    uint8_t buf[6];
    buf[0] = gs.buttons & 0xFF;
    buf[1] = (gs.buttons >> 8) & 0xFF;
    buf[2] = (uint8_t)gs.axisX;
    buf[3] = (uint8_t)gs.axisY;
    buf[4] = (uint8_t)gs.axisRX;
    buf[5] = (uint8_t)gs.axisRY;
    input->setValue(buf, 6);
    input->notify();
}

// ── Status helpers ────────────────────────────────────────────
String BluetoothModule::roleString() const {
    switch (_cfg.role) {
        case BleRole::DISABLED:   return "disabled";
        case BleRole::SCANNER:    return "scanner";
        case BleRole::CENTRAL:    return "central";
        case BleRole::PERIPHERAL: return "peripheral";
        case BleRole::HID_KB:     return "hid_keyboard";
        case BleRole::HID_MEDIA:  return "hid_media";
        case BleRole::HID_GAMEPAD:return "hid_gamepad";
        case BleRole::PROXY:      return "proxy";
        default:                  return "unknown";
    }
}

String BluetoothModule::statusJson() const {
    JsonDocument doc;
    doc["enabled"]   = _cfg.enabled;
    doc["role"]      = roleString();
    doc["scanning"]  = _scanning;
    doc["connected"] = _connected;
    doc["advertising"]= _advertising;
    doc["hidPaired"] = _hidPeerConnected;
    doc["timeSynced"]= _timeSynced;           // Feature #13
    doc["multiSlots"]= _slotCount;            // Feature #12
    if (_connected) {
        JsonObject w = doc["watch"].to<JsonObject>();
        w["address"]     = _watchData.address;
        w["name"]        = _watchData.name;
        w["heartRate"]   = _watchData.heartRate;
        w["spo2"]        = _watchData.spo2;
        w["steps"]       = _watchData.steps;
        w["battery"]     = _watchData.battery;
        w["temperature"] = _watchData.temperature;
        w["manufacturer"]= _watchData.manufacturer;
        w["model"]       = _watchData.model;
        w["firmwareRev"] = _watchData.firmwareRev;
        w["hrNotifying"] = _watchData.hrNotifying;
        w["lastUpdate"]  = _watchData.lastUpdate;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

// ─────────────────────────────────────────────────────────────
//  FEATURE #4 — BLE Status WebSocket Broadcast
//  Called from loop() every 2s; only pushes if status changed.
// ─────────────────────────────────────────────────────────────
void BluetoothModule::broadcastBleStatus() {
    if (!_cfg.enabled) return;
    if (!_wsBroadcastCb) return;

    unsigned long now = millis();
    if (now - _lastBleBroadcast < 2000) return;
    _lastBleBroadcast = now;

    // Build event payload
    JsonDocument doc;
    doc["event"]      = "ble_status";
    doc["role"]       = roleString();
    doc["enabled"]    = _cfg.enabled;
    doc["scanning"]   = _scanning;
    doc["connected"]  = _connected;
    doc["advertising"]= _advertising;
    doc["hidPaired"]  = _hidPeerConnected;
    doc["timeSynced"] = _timeSynced;
    doc["bondCount"]  = (int)_bondedDevices.size();
    doc["slotCount"]  = _slotCount;

    if (_connected) {
        JsonObject w = doc["watch"].to<JsonObject>();
        w["name"]      = _watchData.name;
        w["heartRate"] = _watchData.heartRate;
        w["battery"]   = _watchData.battery;
        w["spo2"]      = _watchData.spo2;
        w["steps"]     = _watchData.steps;
    }

    // Multi-slot summary
    JsonArray slots = doc["slots"].to<JsonArray>();
    for (uint8_t i = 0; i < BLE_MAX_SLOTS; i++) {
        JsonObject s = slots.add<JsonObject>();
        s["slot"]      = i;
        s["active"]    = _slots[i].active;
        s["connected"] = _slots[i].connected;
        s["address"]   = _slots[i].address;
        s["name"]      = _slots[i].name;
        if (_slots[i].connected) {
            s["battery"]   = _slots[i].watchData.battery;
            s["heartRate"] = _slots[i].watchData.heartRate;
        }
    }

    String json;
    serializeJson(doc, json);

    // Only broadcast if something actually changed
    if (json == _lastBleStatusJson) return;
    _lastBleStatusJson = json;
    _wsBroadcastCb(json);
}

// ─────────────────────────────────────────────────────────────
//  FEATURE #8 — BLE Bonding Manager
// ─────────────────────────────────────────────────────────────
void BluetoothModule::_loadBonds() {
    _bondedDevices.clear();
    if (!LittleFS.exists(BLE_BONDS_FILE)) return;
    File f = LittleFS.open(BLE_BONDS_FILE, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok) { f.close(); return; }
    f.close();
    JsonArray arr = doc["bonds"].as<JsonArray>();
    for (JsonObject o : arr) {
        BondedDevice bd;
        bd.address  = o["address"] | "";
        bd.name     = o["name"]    | "";
        bd.addrType = o["addrType"]| 0;
        bd.bondedAt = o["bondedAt"]| 0;
        if (bd.address.length()) _bondedDevices.push_back(bd);
    }
    Serial.printf("[BLE] Loaded %u bonded devices\n", (unsigned)_bondedDevices.size());
}

void BluetoothModule::_saveBonds() {
    JsonDocument doc;
    JsonArray arr = doc["bonds"].to<JsonArray>();
    for (const auto& bd : _bondedDevices) {
        JsonObject o = arr.add<JsonObject>();
        o["address"]  = bd.address;
        o["name"]     = bd.name;
        o["addrType"] = bd.addrType;
        o["bondedAt"] = bd.bondedAt;
    }
    File f = LittleFS.open(BLE_BONDS_FILE, "w");
    if (f) { serializeJson(doc, f); f.close(); }
    else Serial.printf("[BLE] open(%s,w) failed\n", BLE_BONDS_FILE);
}

bool BluetoothModule::bondDevice(const String& address, const String& name, uint8_t addrType) {
    // Update if already exists
    for (auto& bd : _bondedDevices) {
        if (bd.address == address) {
            bd.name     = name;
            bd.addrType = addrType;
            bd.bondedAt = (uint32_t)millis();
            _saveBonds();
            Serial.printf("[BLE] Updated bond: %s (%s)\n", name.c_str(), address.c_str());
            return true;
        }
    }
    if (_bondedDevices.size() >= 16) {
        Serial.println("[BLE] Bond list full (16 max) — remove one first");
        return false;
    }
    BondedDevice bd;
    bd.address  = address;
    bd.name     = name.length() ? name : address;
    bd.addrType = addrType;
    bd.bondedAt = (uint32_t)millis();
    _bondedDevices.push_back(bd);
    _saveBonds();
    Serial.printf("[BLE] Bonded: %s (%s)\n", bd.name.c_str(), address.c_str());
    return true;
}

bool BluetoothModule::removeBond(const String& address) {
    for (auto it = _bondedDevices.begin(); it != _bondedDevices.end(); ++it) {
        if (it->address == address) {
            Serial.printf("[BLE] Removed bond: %s\n", address.c_str());
            _bondedDevices.erase(it);
            _saveBonds();
            return true;
        }
    }
    return false;
}

void BluetoothModule::clearAllBonds() {
    _bondedDevices.clear();
    _saveBonds();
    // Also clear ESP32 NVS BLE bonds
    esp_ble_bond_dev_t list[20];
    int count = 20;
    esp_ble_get_bond_device_list(&count, list);
    for (int i = 0; i < count; i++)
        esp_ble_remove_bond_device(list[i].bd_addr);
    Serial.println("[BLE] All bonds cleared");
}

bool BluetoothModule::isBonded(const String& address) const {
    for (const auto& bd : _bondedDevices)
        if (bd.address == address) return true;
    return false;
}

String BluetoothModule::bondsJson() const {
    JsonDocument doc;
    JsonArray arr = doc["bonds"].to<JsonArray>();
    for (const auto& bd : _bondedDevices) {
        JsonObject o = arr.add<JsonObject>();
        o["address"]  = bd.address;
        o["name"]     = bd.name;
        o["addrType"] = bd.addrType;
        o["bondedAt"] = bd.bondedAt;
    }
    doc["count"]       = (int)_bondedDevices.size();
    doc["bondingOnly"] = _cfg.bondingOnly;
    String out;
    serializeJson(doc, out);
    return out;
}

// ─────────────────────────────────────────────────────────────
//  FEATURE #12 — Multi-Connect (up to 3 simultaneous centrals)
// ─────────────────────────────────────────────────────────────
bool BluetoothModule::connectMulti(const String& address, uint8_t slot) {
    _initStack();

    // Find free slot
    if (slot == 0xFF) {
        slot = 0xFF;
        for (uint8_t i = 0; i < BLE_MAX_SLOTS; i++) {
            if (!_slots[i].active) { slot = i; break; }
        }
        if (slot == 0xFF) {
            Serial.println("[BLE] Multi-connect: all slots busy");
            return false;
        }
    } else if (slot >= BLE_MAX_SLOTS) {
        return false;
    }

    // Disconnect slot if already active
    if (_slots[slot].active) disconnectSlot(slot);

    // Feature #8: bondingOnly check
    if (_cfg.bondingOnly && !isBonded(address)) {
        Serial.printf("[BLE] Multi-connect blocked — %s not bonded\n", address.c_str());
        return false;
    }

    BLEAddress bleAddr(address.c_str());
    BLEClient* client = BLEDevice::createClient();

    // Per-slot client callbacks using lambda capture
    class SlotClientCb : public BLEClientCallbacks {
    public:
        BluetoothModule* mod;
        uint8_t          slotIdx;
        void onConnect(BLEClient*) override {
            mod->_slots[slotIdx].connected = true;
            mod->_slotCount++;
            Serial.printf("[BLE] Slot %u connected\n", slotIdx);
        }
        void onDisconnect(BLEClient*) override {
            mod->_slots[slotIdx].connected = false;
            if (mod->_slotCount) mod->_slotCount--;
            Serial.printf("[BLE] Slot %u disconnected\n", slotIdx);
        }
    };
    auto* cb = new SlotClientCb();
    cb->mod     = this;
    cb->slotIdx = slot;
    client->setClientCallbacks(cb);
    // setConnectionParams removed in arduino-esp32 2.x — see central connect().
    client->setMTU(247);

    if (!client->connect(bleAddr)) {
        delete client;
        delete cb;
        Serial.printf("[BLE] Slot %u connect FAILED: %s\n", slot, address.c_str());
        return false;
    }

    _slots[slot].active    = true;
    _slots[slot].connected = true;
    _slots[slot].address   = address;
    _slots[slot].pClient   = client;
    _slots[slot].watchData.address   = address;
    _slots[slot].watchData.connected = true;
    _slots[slot].watchData.lastUpdate= millis();

    // Auto-bond on successful connect
    bondDevice(address, "", 0);

    // Read basic GATT for this slot (battery + name)
    BLERemoteService* svc = client->getService(BLEUUID(UUID_SVC_BATTERY));
    if (svc) {
        auto* chr = svc->getCharacteristic(BLEUUID(UUID_CHR_BATTERY_LEVEL));
        if (chr && chr->canRead()) {
            std::string v = chr->readValue();
            if (!v.empty()) _slots[slot].watchData.battery = (uint8_t)v[0];
        }
    }
    svc = client->getService(BLEUUID(UUID_SVC_GENERIC_ACCESS));
    if (svc) {
        auto* chr = svc->getCharacteristic(BLEUUID(UUID_CHR_DEVICE_NAME));
        if (chr && chr->canRead()) {
            _slots[slot].name = chr->readValue().c_str();
            _slots[slot].watchData.name = _slots[slot].name;
        }
    }

    Serial.printf("[BLE] Slot %u connected: %s (%s)\n",
                  slot, _slots[slot].name.c_str(), address.c_str());
    return true;
}

void BluetoothModule::disconnectSlot(uint8_t slot) {
    if (slot >= BLE_MAX_SLOTS || !_slots[slot].active) return;
    BLEClient* c = reinterpret_cast<BLEClient*>(_slots[slot].pClient);
    if (c) {
        if (c->isConnected()) c->disconnect();
        delete c;
    }
    _slots[slot] = CentralSlot{};  // reset to default
    if (_slotCount) _slotCount--;
    Serial.printf("[BLE] Slot %u disconnected\n", slot);
}

bool BluetoothModule::isSlotConnected(uint8_t slot) const {
    if (slot >= BLE_MAX_SLOTS) return false;
    return _slots[slot].connected;
}

uint8_t BluetoothModule::connectedCount() const {
    uint8_t n = _connected ? 1 : 0;
    for (uint8_t i = 0; i < BLE_MAX_SLOTS; i++)
        if (_slots[i].connected) n++;
    return n;
}

String BluetoothModule::multiStatusJson() const {
    JsonDocument doc;
    doc["totalConnected"] = connectedCount();
    JsonArray arr = doc["slots"].to<JsonArray>();
    for (uint8_t i = 0; i < BLE_MAX_SLOTS; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["slot"]      = i;
        o["active"]    = _slots[i].active;
        o["connected"] = _slots[i].connected;
        o["address"]   = _slots[i].address;
        o["name"]      = _slots[i].name;
        if (_slots[i].connected) {
            o["battery"]   = _slots[i].watchData.battery;
            o["heartRate"] = _slots[i].watchData.heartRate;
            o["lastUpdate"]= _slots[i].watchData.lastUpdate;
        }
    }
    String out;
    serializeJson(doc, out);
    return out;
}

// ─────────────────────────────────────────────────────────────
//  FEATURE #13 — BLE Time Sync via CTS (Current Time Service)
//
//  NOTE: Watch phone se connected hogi toh CTS sirf PHONE se
//  milega (watch BLE-CTS expose nahi karti usually).
//  Is function ko sirf tab call karo jab phone connected ho
//  (Central mode, address = phone ka address).
// ─────────────────────────────────────────────────────────────
bool BluetoothModule::syncTimeFromPhone() {
    if (!_connected || !_pClient) {
        Serial.println("[BLE-CTS] No central connection — cannot sync time");
        return false;
    }
    BLEClient* c = reinterpret_cast<BLEClient*>(_pClient);

    // CTS Service UUID: 0x1805
    // Current Time Characteristic: 0x2A2B  (10 bytes)
    //   Byte 0-1: Year (little-endian)
    //   Byte 2:   Month (1-12)
    //   Byte 3:   Day
    //   Byte 4:   Hours
    //   Byte 5:   Minutes
    //   Byte 6:   Seconds
    //   Byte 7:   Day of week (1=Mon)
    //   Byte 8:   Fractions256
    //   Byte 9:   Adjust reason

    BLERemoteService* cts = c->getService(BLEUUID("00001805-0000-1000-8000-00805f9b34fb"));
    if (!cts) {
        Serial.println("[BLE-CTS] Phone does not expose CTS — trying Local Time Info");
        // Try reading time from a slot (multi-connect) as fallback
        for (uint8_t i = 0; i < BLE_MAX_SLOTS; i++) {
            if (!_slots[i].connected || !_slots[i].pClient) continue;
            BLEClient* sc = reinterpret_cast<BLEClient*>(_slots[i].pClient);
            cts = sc->getService(BLEUUID("00001805-0000-1000-8000-00805f9b34fb"));
            if (cts) { Serial.printf("[BLE-CTS] Found CTS on slot %u\n", i); break; }
        }
        if (!cts) { Serial.println("[BLE-CTS] CTS not found on any connection"); return false; }
    }

    BLERemoteCharacteristic* chr = cts->getCharacteristic(
        BLEUUID("00002a2b-0000-1000-8000-00805f9b34fb"));
    if (!chr || !chr->canRead()) {
        Serial.println("[BLE-CTS] Current Time characteristic not readable");
        return false;
    }

    std::string val = chr->readValue();
    if (val.size() < 7) {
        Serial.println("[BLE-CTS] CTS response too short");
        return false;
    }

    const uint8_t* d = (const uint8_t*)val.data();
    uint16_t year  = d[0] | (d[1] << 8);
    uint8_t  month = d[2];
    uint8_t  day   = d[3];
    uint8_t  hour  = d[4];
    uint8_t  min   = d[5];
    uint8_t  sec   = d[6];

    // Sanity check
    if (year < 2020 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) {
        Serial.printf("[BLE-CTS] Suspicious time: %04u-%02u-%02u %02u:%02u:%02u — rejected\n",
                      year, month, day, hour, min, sec);
        return false;
    }

    // Build timeval and set system clock
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = min;
    t.tm_sec  = sec;
    time_t epoch = mktime(&t);

    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    _timeSynced    = true;
    _lastSyncEpoch = epoch;
    _lastSyncMs    = millis();

    Serial.printf("[BLE-CTS] Time synced from phone: %04u-%02u-%02u %02u:%02u:%02u (epoch=%ld)\n",
                  year, month, day, hour, min, sec, (long)epoch);
    return true;
}

String BluetoothModule::timeSyncJson() const {
    JsonDocument doc;
    doc["synced"]      = _timeSynced;
    doc["lastEpoch"]   = (long)_lastSyncEpoch;
    doc["lastSyncMs"]  = _lastSyncMs;
    if (_timeSynced) {
        char buf[32];
        struct tm* tm = localtime(&_lastSyncEpoch);
        if (tm) {
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
            doc["lastSyncStr"] = buf;
        }
    }
    String out;
    serializeJson(doc, out);
    return out;
}

// =============================================================
//  FEATURE #P — BLE PROXY  (Watch <─ ESP32 ─> Phone)
//
//  How it works:
//  Step 1: ESP32 scans and connects to watch as CENTRAL
//          → Watch disconnects from phone automatically
//  Step 2: ESP32 reads watch GATT (HR, Battery, Temp services)
//          and subscribes to notifications
//  Step 3: ESP32 starts a PERIPHERAL (fake watch server)
//          advertising the same name as the real watch
//  Step 4: Phone reconnects to ESP32 thinking it's the watch
//  Step 5: Every notification from watch → forwarded to phone
//
//  Memory note: Proxy uses ~120KB RAM (central + peripheral
//  stacks both active). Disable WiFi scan or NFC if OOM.
// =============================================================

#define PROXY_CONFIG_FILE  "/ble_proxy.json"
#define PROXY_RETRY_MAX    5
#define PROXY_RETRY_MS     8000

// ── Proxy server callbacks (phone side) ──────────────────────
class ProxyServerCb : public BLEServerCallbacks {
public:
    BluetoothModule* mod;
    void onConnect(BLEServer*) override {
        mod->_proxyPhoneConnected = true;
        mod->_proxyState = ProxyState::PHONE_CONNECTED;
        Serial.println("[PROXY] Phone connected to ESP32 proxy");
        if (mod->_wsBroadcastCb) {
            mod->_wsBroadcastCb(
                "{\"event\":\"ble_proxy\",\"state\":\"phone_connected\"}");
        }
    }
    void onDisconnect(BLEServer* srv) override {
        mod->_proxyPhoneConnected = false;
        if (mod->_proxyState == ProxyState::PHONE_CONNECTED)
            mod->_proxyState = ProxyState::WATCH_CONNECTED;
        Serial.println("[PROXY] Phone disconnected — re-advertising");
        BLEDevice::startAdvertising();
        if (mod->_wsBroadcastCb) {
            mod->_wsBroadcastCb(
                "{\"event\":\"ble_proxy\",\"state\":\"watch_connected\"}");
        }
    }
};

// ── Proxy watch client callbacks ──────────────────────────────
class ProxyWatchClientCb : public BLEClientCallbacks {
public:
    BluetoothModule* mod;
    void onConnect(BLEClient*) override {
        Serial.println("[PROXY] Watch connected");
    }
    void onDisconnect(BLEClient*) override {
        Serial.println("[PROXY] Watch disconnected — will retry");
        mod->_proxyState      = ProxyState::ERROR;
        mod->_proxyLastRetry  = millis();
        mod->_proxyPhoneConnected = false;
        if (mod->_wsBroadcastCb)
            mod->_wsBroadcastCb(
                "{\"event\":\"ble_proxy\",\"state\":\"watch_disconnected\"}");
    }
};

// ── Static HR notification from watch → forward to phone ─────
void BluetoothModule::_proxyOnHrNotify(void* chr,
        uint8_t* data, size_t len, bool /*notify*/) {
    if (!s_bt || !s_bt->_proxyCfg.forwardHR) return;

    // Update local mirror
    if (len >= 2) {
        bool isU16 = (data[0] & 0x01);
        s_bt->_proxyWatchData.heartRate =
            isU16 ? (data[1] | (data[2] << 8)) : data[1];
        s_bt->_proxyWatchData.lastUpdate = millis();
    }

    // Forward raw bytes to phone characteristic
    s_bt->_proxyForwardHR(data, len);
}

// ── Static Temp notification from watch → forward to phone ───
void BluetoothModule::_proxyOnTempNotify(void* chr,
        uint8_t* data, size_t len, bool /*notify*/) {
    if (!s_bt || !s_bt->_proxyCfg.forwardTemp) return;
    s_bt->_proxyForwardTemp(data, len);
}

// ── Forward HR bytes to phone ─────────────────────────────────
// PROXY FIX: each forwarder snapshots its target pointer under
// _proxyMutex so proxyStop() (which sets the pointer to nullptr after
// destroying the underlying BLEServer) can't race with the deref. The
// callback fires from the BT host task; proxyStop runs from the API
// task. Previously a stop mid-notify would crash on use-after-free.
void BluetoothModule::_proxyForwardHR(uint8_t* data, size_t len) {
    BLECharacteristic* chr = nullptr;
    if (_proxyMutex) xSemaphoreTake(_proxyMutex, portMAX_DELAY);
    if (_proxyPhoneConnected && _proxyHrChr) {
        chr = reinterpret_cast<BLECharacteristic*>(_proxyHrChr);
    }
    if (_proxyMutex) xSemaphoreGive(_proxyMutex);
    if (!chr) return;
    chr->setValue(data, len);
    chr->notify();
}

void BluetoothModule::_proxyForwardBattery(uint8_t level) {
    BLECharacteristic* chr = nullptr;
    if (_proxyMutex) xSemaphoreTake(_proxyMutex, portMAX_DELAY);
    if (_proxyPhoneConnected && _proxyBattChr) {
        _proxyWatchData.battery = level;
        chr = reinterpret_cast<BLECharacteristic*>(_proxyBattChr);
    }
    if (_proxyMutex) xSemaphoreGive(_proxyMutex);
    if (!chr) return;
    chr->setValue(&level, 1);
    chr->notify();
}

void BluetoothModule::_proxyForwardTemp(uint8_t* data, size_t len) {
    BLECharacteristic* chr = nullptr;
    if (_proxyMutex) xSemaphoreTake(_proxyMutex, portMAX_DELAY);
    if (_proxyPhoneConnected && _proxyTempChr) {
        chr = reinterpret_cast<BLECharacteristic*>(_proxyTempChr);
    }
    if (_proxyMutex) xSemaphoreGive(_proxyMutex);
    if (!chr) return;
    chr->setValue(data, len);
    chr->indicate();
}

// ── Step 1+2: Connect to watch (central side) ─────────────────
void BluetoothModule::_proxyConnectWatch() {
    Serial.printf("[PROXY] Connecting to watch: %s\n",
                  _proxyCfg.watchAddress.c_str());
    _proxyState = ProxyState::CONNECTING_WATCH;

    BLEAddress watchAddr(_proxyCfg.watchAddress.c_str());
    BLEClient* client = BLEDevice::createClient();

    static ProxyWatchClientCb watchCb;
    watchCb.mod = this;
    client->setClientCallbacks(&watchCb);
    // setConnectionParams removed in arduino-esp32 2.x — see central connect().
    client->setMTU(247);

    if (!client->connect(watchAddr)) {
        Serial.println("[PROXY] Watch connect FAILED");
        delete client;
        _proxyState     = ProxyState::ERROR;
        _proxyLastRetry = millis();
        _proxyRetryCount++;
        return;
    }

    // PROXY FIX: publish watch client under the mutex so proxyStop and
    // the loop's battery-poll path see a consistent view.
    if (_proxyMutex) xSemaphoreTake(_proxyMutex, portMAX_DELAY);
    _proxyWatchClient = client;
    if (_proxyMutex) xSemaphoreGive(_proxyMutex);
    _proxyWatchData.address   = _proxyCfg.watchAddress;
    _proxyWatchData.connected = true;
    _proxyWatchData.lastUpdate= millis();
    _proxyRetryCount = 0;

    // Read watch device name (informational only — the spoof name was
    // committed in proxyStart() and can no longer be changed without
    // tearing the stack down again).
    BLERemoteService* svc =
        client->getService(BLEUUID(UUID_SVC_GENERIC_ACCESS));
    if (svc) {
        auto* chr = svc->getCharacteristic(BLEUUID(UUID_CHR_DEVICE_NAME));
        if (chr && chr->canRead()) {
            _proxyWatchData.name = chr->readValue().c_str();
        }
    }

    // Read battery
    svc = client->getService(BLEUUID(UUID_SVC_BATTERY));
    if (svc) {
        auto* chr =
            svc->getCharacteristic(BLEUUID(UUID_CHR_BATTERY_LEVEL));
        if (chr && chr->canRead()) {
            std::string v = chr->readValue();
            if (!v.empty()) _proxyWatchData.battery = (uint8_t)v[0];
        }
    }

    // Subscribe HR notifications from watch
    if (_proxyCfg.forwardHR) {
        svc = client->getService(BLEUUID(UUID_SVC_HEART_RATE));
        if (svc) {
            auto* chr = svc->getCharacteristic(
                BLEUUID(UUID_CHR_HR_MEASUREMENT));
            if (chr && chr->canNotify()) {
                chr->registerForNotify(
                    [](BLERemoteCharacteristic* c,
                       uint8_t* d, size_t l, bool n) {
                        BluetoothModule::_proxyOnHrNotify(c, d, l, n);
                    });
                _proxyWatchData.hrNotifying = true;
            }
        }
    }

    // Subscribe temp notifications from watch
    if (_proxyCfg.forwardTemp) {
        svc = client->getService(BLEUUID(UUID_SVC_HEALTH_THERM));
        if (svc) {
            auto* chr = svc->getCharacteristic(
                BLEUUID("00002a1c-0000-1000-8000-00805f9b34fb"));
            if (chr && chr->canIndicate()) {
                chr->registerForNotify(
                    [](BLERemoteCharacteristic* c,
                       uint8_t* d, size_t l, bool n) {
                        BluetoothModule::_proxyOnTempNotify(c, d, l, n);
                    }, false);
            }
        }
    }

    Serial.printf("[PROXY] Watch connected: %s  Batt=%d%%  HR=%d\n",
                  _proxyWatchData.name.c_str(),
                  _proxyWatchData.battery,
                  _proxyWatchData.heartRate);

    _proxyState = ProxyState::WATCH_CONNECTED;

    if (_wsBroadcastCb) {
        JsonDocument doc;
        doc["event"]   = "ble_proxy";
        doc["state"]   = "watch_connected";
        doc["watch"]   = _proxyWatchData.name;
        doc["battery"] = _proxyWatchData.battery;
        String j; serializeJson(doc, j);
        _wsBroadcastCb(j);
    }

    // Now set up phone-side server
    _proxySetupPhoneServer();
}

// ── Step 3: Setup fake watch server (peripheral side) ─────────
// PROXY FIX: previously this function created the server + services +
// characteristics, then called BLEDevice::deinit(false) followed by
// init(spoofName) to change the advertised name — but never rebuilt
// the server. Every _proxy*Chr pointer was left dangling into freed
// memory, and the active watch BLEClient was also freed by deinit.
// The first forwarded notify after a phone connected dereferenced a
// dead pointer and crashed the device.
//
// New flow: spoof name is committed in proxyStart() BEFORE any server
// or client objects exist, so this function only creates the server,
// services, characteristics, and starts advertising — no destructive
// re-init. The _proxy*Chr pointers remain valid for the whole proxy
// session.
void BluetoothModule::_proxySetupPhoneServer() {
    String advName = _proxyCfg.spoofName.length()
                     ? _proxyCfg.spoofName : "ESP32-Watch";
    Serial.printf("[PROXY] Starting fake-watch server: \"%s\"\n",
                  advName.c_str());

    BLEServer* srv = BLEDevice::createServer();
    static ProxyServerCb srvCb;
    srvCb.mod = this;
    srv->setCallbacks(&srvCb);

    // Build services BEFORE publishing the chr pointers so anything
    // racing on the mutex sees consistent state.
    BLECharacteristic* hrChr   = nullptr;
    BLECharacteristic* battChr = nullptr;
    BLECharacteristic* tempChr = nullptr;

    if (_proxyCfg.forwardHR) {
        BLEService* hrSvc = srv->createService(
            BLEUUID(UUID_SVC_HEART_RATE));
        hrChr = hrSvc->createCharacteristic(
            BLEUUID(UUID_CHR_HR_MEASUREMENT),
            BLECharacteristic::PROPERTY_NOTIFY);
        hrChr->addDescriptor(new BLE2902());
        hrSvc->start();
    }

    if (_proxyCfg.forwardBatt) {
        BLEService* battSvc = srv->createService(
            BLEUUID(UUID_SVC_BATTERY));
        battChr = battSvc->createCharacteristic(
            BLEUUID(UUID_CHR_BATTERY_LEVEL),
            BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_NOTIFY);
        battChr->addDescriptor(new BLE2902());
        uint8_t batt = (uint8_t)(_proxyWatchData.battery >= 0
                                  ? _proxyWatchData.battery : 0);
        battChr->setValue(&batt, 1);
        battSvc->start();
    }

    if (_proxyCfg.forwardTemp) {
        BLEService* tempSvc = srv->createService(
            BLEUUID(UUID_SVC_HEALTH_THERM));
        tempChr = tempSvc->createCharacteristic(
            BLEUUID("00002a1c-0000-1000-8000-00805f9b34fb"),
            BLECharacteristic::PROPERTY_INDICATE);
        tempChr->addDescriptor(new BLE2902());
        tempSvc->start();
    }

    // Device Info (spoof as watch)
    BLEService* infoSvc = srv->createService(
        BLEUUID(UUID_SVC_DEVICE_INFO));
    BLECharacteristic* mfrChr = infoSvc->createCharacteristic(
        BLEUUID(UUID_CHR_MANUFACTURER),
        BLECharacteristic::PROPERTY_READ);
    String mfr = _proxyWatchData.manufacturer.length()
                 ? _proxyWatchData.manufacturer : "ESP32-Proxy";
    mfrChr->setValue(mfr.c_str());
    BLECharacteristic* mdlChr = infoSvc->createCharacteristic(
        BLEUUID(UUID_CHR_MODEL_NUMBER),
        BLECharacteristic::PROPERTY_READ);
    mdlChr->setValue(_proxyWatchData.model.length()
                     ? _proxyWatchData.model.c_str() : "v1.0");
    infoSvc->start();

    // Publish the chr pointers atomically — only after all services have
    // .start()ed so any notify that races in immediately is well-formed.
    if (_proxyMutex) xSemaphoreTake(_proxyMutex, portMAX_DELAY);
    _proxyPhoneServer = srv;
    _proxyHrChr       = hrChr;
    _proxyBattChr     = battChr;
    _proxyTempChr     = tempChr;
    if (_proxyMutex) xSemaphoreGive(_proxyMutex);

    // Start advertising with the spoof name (set during proxyStart).
    BLEAdvertising* adv = BLEDevice::getAdvertising();
    if (_proxyCfg.forwardHR)
        adv->addServiceUUID(BLEUUID(UUID_SVC_HEART_RATE));
    if (_proxyCfg.forwardBatt)
        adv->addServiceUUID(BLEUUID(UUID_SVC_BATTERY));
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    adv->setMinPreferred(0x12);
    BLEDevice::startAdvertising();

    Serial.printf("[PROXY] Advertising as \"%s\" — waiting for phone\n",
                  advName.c_str());

    if (_wsBroadcastCb) {
        JsonDocument doc;
        doc["event"]     = "ble_proxy";
        doc["state"]     = "advertising";
        doc["spoofName"] = advName;
        String j; serializeJson(doc, j);
        _wsBroadcastCb(j);
    }
}

// ── Proxy state machine — called every loop() ─────────────────
void BluetoothModule::_proxyLoop() {
    // Periodically push battery update to phone.
    // PROXY FIX: snapshot _proxyWatchClient and the connection state
    // under the mutex so proxyStop() can't free the client mid-readValue.
    static unsigned long _lastBattUpdate = 0;
    BLEClient* c = nullptr;
    bool       hasBattChr = false;
    if (_proxyMutex) xSemaphoreTake(_proxyMutex, portMAX_DELAY);
    if (_proxyPhoneConnected && _proxyBattChr &&
        millis() - _lastBattUpdate > 30000 && _proxyWatchClient) {
        c          = reinterpret_cast<BLEClient*>(_proxyWatchClient);
        hasBattChr = true;
    }
    if (_proxyMutex) xSemaphoreGive(_proxyMutex);

    if (hasBattChr && c) {
        _lastBattUpdate = millis();
        if (c->isConnected()) {
            BLERemoteService* svc =
                c->getService(BLEUUID(UUID_SVC_BATTERY));
            if (svc) {
                auto* chr = svc->getCharacteristic(
                    BLEUUID(UUID_CHR_BATTERY_LEVEL));
                if (chr && chr->canRead()) {
                    std::string v = chr->readValue();
                    if (!v.empty())
                        _proxyForwardBattery((uint8_t)v[0]);
                }
            }
        }
    }

    // Auto-retry on watch disconnect
    if (_proxyState == ProxyState::ERROR) {
        if (_proxyRetryCount >= PROXY_RETRY_MAX) {
            Serial.println("[PROXY] Max retries reached — stopping proxy");
            proxyStop();
            return;
        }
        if (millis() - _proxyLastRetry > PROXY_RETRY_MS) {
            Serial.printf("[PROXY] Retry %u/%u connecting watch\n",
                          _proxyRetryCount + 1, PROXY_RETRY_MAX);
            _proxyConnectWatch();
        }
    }
}

// ── Public API: start proxy ───────────────────────────────────
bool BluetoothModule::proxyStart(const ProxyConfig& cfg) {
    if (_proxyCfg.watchAddress.isEmpty() && cfg.watchAddress.isEmpty()) {
        Serial.println("[PROXY] Cannot start: no watch address");
        return false;
    }
    proxyStop();  // clean up previous state
    _proxyCfg       = cfg;
    _proxyRetryCount= 0;
    _proxyState     = ProxyState::SCANNING;
    _cfg.role       = BleRole::PROXY;

    // PROXY FIX: commit the spoof name BEFORE any BLE objects exist, so
    // _proxySetupPhoneServer doesn't have to do a destructive deinit
    // later. If the user left spoofName blank the placeholder is used;
    // we no longer try to read the watch's GAP name and feed it back
    // into the local BLE stack mid-session (that required a deinit
    // which was the root cause of the previous use-after-free).
    String spoofName = _proxyCfg.spoofName.length()
                       ? _proxyCfg.spoofName : "ESP32-Watch";
    if (BLEDevice::getInitialized() && _cfg.deviceName != spoofName) {
        // Stack is up under a different name. Tear it down BEFORE any
        // server/client/characteristic exists so nothing is invalidated.
        BLEDevice::deinit(false);
    }
    if (!BLEDevice::getInitialized()) {
        BLEDevice::init(spoofName.c_str());
        BLEDevice::setPower(ESP_PWR_LVL_P9);
    }
    _cfg.deviceName    = spoofName;     // persist so _initStack stays in sync
    _proxyCfg.spoofName = spoofName;

    Serial.println("[PROXY] Starting — connecting to watch first");

    if (_wsBroadcastCb)
        _wsBroadcastCb(
            "{\"event\":\"ble_proxy\",\"state\":\"starting\"}");

    _proxyConnectWatch();
    return (_proxyState != ProxyState::ERROR);
}

// ── Public API: stop proxy ────────────────────────────────────
void BluetoothModule::proxyStop() {
    Serial.println("[PROXY] Stopping proxy");

    // PROXY FIX: clear pointer state under the mutex BEFORE freeing the
    // underlying BLE objects. A forwarder running on the BT host task at
    // this exact moment will now see nullptr and bail out — before this
    // ordering the forwarder could read a still-valid pointer, then we
    // would free the BLEServer it pointed at while the chr->notify() was
    // mid-flight. The actual delete of the watch client happens AFTER we
    // release the mutex because BLEClient::disconnect blocks for ~50 ms.
    void* watchClient = nullptr;
    if (_proxyMutex) xSemaphoreTake(_proxyMutex, portMAX_DELAY);
    watchClient          = _proxyWatchClient;
    _proxyWatchClient    = nullptr;
    _proxyPhoneConnected = false;
    _proxyPhoneServer    = nullptr;
    _proxyHrChr          = nullptr;
    _proxyBattChr        = nullptr;
    _proxyTempChr        = nullptr;
    if (_proxyMutex) xSemaphoreGive(_proxyMutex);

    if (watchClient) {
        BLEClient* c = reinterpret_cast<BLEClient*>(watchClient);
        if (c->isConnected()) c->disconnect();
        delete c;
    }

    BLEDevice::stopAdvertising();
    _proxyState     = ProxyState::IDLE;
    _proxyWatchData = WatchData{};

    if (_wsBroadcastCb)
        _wsBroadcastCb(
            "{\"event\":\"ble_proxy\",\"state\":\"stopped\"}");
}

// ── Proxy state string ────────────────────────────────────────
String BluetoothModule::proxyStateStr() const {
    switch (_proxyState) {
        case ProxyState::IDLE:            return "idle";
        case ProxyState::SCANNING:        return "scanning";
        case ProxyState::CONNECTING_WATCH:return "connecting_watch";
        case ProxyState::WATCH_CONNECTED: return "watch_connected";
        case ProxyState::PHONE_CONNECTED: return "phone_connected";
        case ProxyState::ERROR:           return "error";
        default:                          return "unknown";
    }
}

// ── Proxy status JSON ─────────────────────────────────────────
String BluetoothModule::proxyStatusJson() const {
    JsonDocument doc;
    doc["running"]       = proxyIsRunning();
    doc["state"]         = proxyStateStr();
    doc["watchAddress"]  = _proxyCfg.watchAddress;
    doc["spoofName"]     = _proxyCfg.spoofName;
    doc["phoneConnected"]= _proxyPhoneConnected;
    doc["retryCount"]    = _proxyRetryCount;
    doc["forwardHR"]     = _proxyCfg.forwardHR;
    doc["forwardBatt"]   = _proxyCfg.forwardBatt;
    doc["forwardTemp"]   = _proxyCfg.forwardTemp;
    if (_proxyWatchData.connected || _proxyPhoneConnected) {
        JsonObject w = doc["watchData"].to<JsonObject>();
        w["name"]      = _proxyWatchData.name;
        w["battery"]   = _proxyWatchData.battery;
        w["heartRate"] = _proxyWatchData.heartRate;
        w["lastUpdate"]= _proxyWatchData.lastUpdate;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

// ── Save / Load proxy config ──────────────────────────────────
void BluetoothModule::proxySaveConfig(const ProxyConfig& cfg) {
    _proxyCfg = cfg;
    JsonDocument doc;
    doc["watchAddress"] = cfg.watchAddress;
    doc["spoofName"]    = cfg.spoofName;
    doc["forwardHR"]    = cfg.forwardHR;
    doc["forwardBatt"]  = cfg.forwardBatt;
    doc["forwardTemp"]  = cfg.forwardTemp;
    doc["autoStart"]    = cfg.autoStart;
    File f = LittleFS.open(PROXY_CONFIG_FILE, "w");
    if (f) { serializeJson(doc, f); f.close(); Serial.println("[PROXY] Config saved"); }
    else Serial.printf("[PROXY] open(%s,w) failed\n", PROXY_CONFIG_FILE);
}

ProxyConfig BluetoothModule::proxyLoadConfig() const {
    ProxyConfig cfg;
    if (!LittleFS.exists(PROXY_CONFIG_FILE)) return cfg;
    File f = LittleFS.open(PROXY_CONFIG_FILE, "r");
    if (!f) return cfg;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        cfg.watchAddress = doc["watchAddress"] | "";
        cfg.spoofName    = doc["spoofName"]    | "";
        cfg.forwardHR    = doc["forwardHR"]    | true;
        cfg.forwardBatt  = doc["forwardBatt"]  | true;
        cfg.forwardTemp  = doc["forwardTemp"]  | true;
        cfg.autoStart    = doc["autoStart"]    | false;
    }
    f.close();
    return cfg;
}
