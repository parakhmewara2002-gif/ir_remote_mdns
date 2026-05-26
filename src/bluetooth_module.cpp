// ============================================================
//  bluetooth_module.cpp  —  BLE Multi-Role Module v1.0
//  Supports: Peripheral, HID Keyboard, HID Media, HID Gamepad
// ============================================================
#include "bluetooth_module.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEHIDDevice.h>
#include <BLE2902.h>           // Feature #P: CCCD notify descriptor
#include <HIDTypes.h>
#include <HIDKeyboardTypes.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_gap_ble_api.h>   // Feature #8: clearAllBonds NVS access

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

// ─────────────────────────────────────────────────────────────
void BluetoothModule::begin() {
    s_bt = this;
    _loadConfig();
    _loadBonds();   // Feature #8: load bonded devices from LittleFS
    if (_cfg.enabled) {
        _initStack();
        applyRole(_cfg.role);
    }
}

void BluetoothModule::loop() {
    if (!_cfg.enabled) return;

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
    _advertising = false;
    _hidPeerConnected = false;
}

void BluetoothModule::applyRole(BleRole role) {
    _cfg.role = role;
    if (!_cfg.enabled) return;
    _initStack();
    _stopAll();
    switch (role) {
        case BleRole::PERIPHERAL:
            _startPeripheral();
            break;
        case BleRole::HID_KB:
        case BleRole::HID_MEDIA:
        case BleRole::HID_GAMEPAD:
            _startHid(role);
            break;
        default: break;
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
    // UB FIX: usage > 15 was shifting `int 1` by attacker-controlled
    // amount up to 255; >=31 is undefined behavior in C++. Cap to the
    // 16-bit report range.
    uint16_t report = (usage < 16) ? (uint16_t)(1u << usage) : 0;
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
        case BleRole::PERIPHERAL: return "peripheral";
        case BleRole::HID_KB:     return "hid_keyboard";
        case BleRole::HID_MEDIA:  return "hid_media";
        case BleRole::HID_GAMEPAD:return "hid_gamepad";
        default:                  return "unknown";
    }
}

String BluetoothModule::statusJson() const {
    JsonDocument doc;
    doc["enabled"]   = _cfg.enabled;
    doc["role"]      = roleString();
    doc["advertising"]= _advertising;
    doc["hidPaired"] = _hidPeerConnected;
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
    doc["advertising"]= _advertising;
    doc["hidPaired"]  = _hidPeerConnected;
    doc["bondCount"]  = (int)_bondedDevices.size();

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
