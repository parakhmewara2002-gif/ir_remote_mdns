#pragma once
// ============================================================
//  bluetooth_module.h  —  BLE Multi-Role Module v1.0
//
//  Roles supported (runtime-selectable):
//    PERIPHERAL: ESP32 advertises as custom BLE device
//                Other phones/tablets connect to it
//    HID_KB    : Keyboard HID (inject keystrokes to paired device)
//    HID_MEDIA : Media control (play/pause/vol/next/prev)
//    HID_GAMEPAD: Gamepad (8 buttons + 2 axes)
//
//  WiFi bridge: /api/ble/* REST routes served over WiFi so any
//  browser can control BLE devices.
//
//  Memory note: BLE stack uses ~90 KB RAM. Disable when not needed.
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>

// Arduino's esp32-hal-gpio.h #defines DISABLED 0x00 as a pinMode() value.
// The project doesn't actually call pinMode(p, DISABLED) anywhere, so we
// permanently undef it here to free the identifier for our BleRole enum
// (both the enum body AND every BleRole::DISABLED use site below).
#undef DISABLED

// ── BLE save file ────────────────────────────────────────────
#define BLE_CONFIG_FILE   "/ble_config.json"
#define BLE_BONDS_FILE    "/ble_bonds.json"

// ── Standard BLE Service UUIDs (16-bit) ──────────────────────
#define UUID_SVC_HEART_RATE       "0000180d-0000-1000-8000-00805f9b34fb"
#define UUID_CHR_HR_MEASUREMENT   "00002a37-0000-1000-8000-00805f9b34fb"
#define UUID_SVC_HEALTH_THERM     "00001809-0000-1000-8000-00805f9b34fb"
#define UUID_SVC_BATTERY          "0000180f-0000-1000-8000-00805f9b34fb"
#define UUID_CHR_BATTERY_LEVEL    "00002a19-0000-1000-8000-00805f9b34fb"
#define UUID_SVC_DEVICE_INFO      "0000180a-0000-1000-8000-00805f9b34fb"
#define UUID_CHR_MANUFACTURER     "00002a29-0000-1000-8000-00805f9b34fb"
#define UUID_CHR_MODEL_NUMBER     "00002a24-0000-1000-8000-00805f9b34fb"
#define UUID_CHR_FIRMWARE_REV     "00002a26-0000-1000-8000-00805f9b34fb"
// Generic Access
#define UUID_SVC_GENERIC_ACCESS   "00001800-0000-1000-8000-00805f9b34fb"
#define UUID_CHR_DEVICE_NAME      "00002a00-0000-1000-8000-00805f9b34fb"

// ── BLE role enum ─────────────────────────────────────────────
// DISABLED macro is #undef'd at the top of this header (see note above).
enum class BleRole : uint8_t {
    DISABLED   = 0,
    PERIPHERAL = 3,   // advertise as custom device
    HID_KB     = 4,   // keyboard HID
    HID_MEDIA  = 5,   // media control HID
    HID_GAMEPAD= 6,   // gamepad HID
};

// ── HID Gamepad state ─────────────────────────────────────────
struct GamepadState {
    int8_t  axisX  = 0;   // -127..127
    int8_t  axisY  = 0;
    int8_t  axisRX = 0;
    int8_t  axisRY = 0;
    uint16_t buttons = 0;  // bitmask, 16 buttons
};

// ── Bonded device entry (Feature #8) ─────────────────────────
struct BondedDevice {
    String  address;       // "AA:BB:CC:DD:EE:FF"
    String  name;          // friendly name
    uint8_t addrType = 0;  // 0=public, 1=random
    uint32_t bondedAt = 0; // millis() at bond time (for display)
};

// ── BLE config (persisted) ────────────────────────────────────
struct BleConfig {
    bool      enabled       = false;
    BleRole   role          = BleRole::PERIPHERAL;
    String    deviceName    = "ESP32-BLE";   // peripheral name
    bool      autoConnect   = false;
    String    lastAddress;
    bool      hidPassthrough= false;
    bool      bondingOnly   = false;         // Feature #8: reject unbonded devices
};

// ── Bluetooth Module class ────────────────────────────────────
// Forward-declare the friend callback class (defined in bluetooth_module.cpp).
class BtServerCallbacks;

class BluetoothModule {
public:
    BluetoothModule() = default;

    // BLE-stack callbacks reach into our private state directly.
    friend class BtServerCallbacks;

    void begin();
    void loop();

    // ── Config ───────────────────────────────────────────────
    BleConfig getConfig() const { return _cfg; }
    void      saveConfig(const BleConfig& cfg);
    void      applyRole(BleRole role);

    // ── Feature #4: set WebSocket broadcast callback ──────────
    void setWsBroadcastCb(std::function<void(const String&)> cb) { _wsBroadcastCb = cb; }

    // ── HID ───────────────────────────────────────────────────
    void hidSendKey(uint8_t keyCode, uint8_t modifier = 0);
    void hidSendMedia(uint8_t usage);   // play/pause/vol+/vol-/next/prev
    void hidSendGamepad(const GamepadState& gs);
    bool hidIsConnected() const { return _hidPeerConnected; }

    // ── Peripheral ────────────────────────────────────────────
    bool peripheralIsAdvertising() const { return _advertising; }
    int  peripheralPeerRssi()      const { return _peerRssi; }

    // ── Feature #8: Bonding Manager ──────────────────────────
    bool        bondDevice(const String& address, const String& name, uint8_t addrType = 0);
    bool        removeBond(const String& address);
    void        clearAllBonds();
    bool        isBonded(const String& address) const;
    const std::vector<BondedDevice>& bondedDevices() const { return _bondedDevices; }
    String      bondsJson() const;

    // ── Feature #4: BLE Status WebSocket broadcast ────────────
    void        broadcastBleStatus();  // called from loop(), pushes WS event

    // ── Status ────────────────────────────────────────────────
    BleRole role()       const { return _cfg.role; }
    bool    isEnabled()  const { return _cfg.enabled; }
    String  roleString() const;
    String  statusJson() const;

private:
    BleConfig  _cfg;
    bool       _advertising  = false;
    bool       _hidPeerConnected = false;
    int        _peerRssi     = 0;

    // Internal BLE stack handles (opaque pointers, impl in .cpp)
    void*  _pServer     = nullptr;
    void*  _pAdvertising= nullptr;
    void*  _pHidDevice  = nullptr;

    // Feature #8: bonding
    std::vector<BondedDevice> _bondedDevices;
    void _loadBonds();
    void _saveBonds();

    // Feature #4: WS status broadcast
    unsigned long _lastBleBroadcast = 0;
    String        _lastBleStatusJson;
    std::function<void(const String&)> _wsBroadcastCb; // set by main.cpp

    void _loadConfig();
    void _initStack();
    void _startPeripheral();
    void _startHid(BleRole hidRole);
    void _stopAll();
};

extern BluetoothModule btModule;
