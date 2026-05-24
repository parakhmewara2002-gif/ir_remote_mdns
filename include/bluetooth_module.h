#pragma once
// ============================================================
//  bluetooth_module.h  —  BLE Multi-Role Module v1.0
//
//  Roles supported (runtime-selectable):
//    CENTRAL   : Scan, connect, read GATT services
//                Smartwatch data: HR, SpO2, Steps, Sleep, Battery
//                Car Android screen, other ESP32, generic BLE
//    PERIPHERAL: ESP32 advertises as custom BLE device
//                Other phones/tablets connect to it
//    HID_KB    : Keyboard HID (inject keystrokes to paired device)
//    HID_MEDIA : Media control (play/pause/vol/next/prev)
//    HID_GAMEPAD: Gamepad (8 buttons + 2 axes)
//    SCANNER   : Passive scan only, no connection
//
//  Smart watch profiles auto-detected via service UUIDs:
//    Heart Rate (0x180D), SpO2/Health Therm (0x1809), Battery (0x180F),
//    Device Info (0x180A), Steps via custom vendor UUIDs
//
//  WiFi bridge: /api/ble/* REST routes served over WiFi so any
//  browser can scan, connect, and read BLE devices.
//
//  Memory note: BLE stack uses ~90 KB RAM. Disable when not needed.
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>
#include <map>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>  // PROXY FIX: _proxyMutex protects pointers
                              // shared between host BT task and API task

// Arduino's esp32-hal-gpio.h #defines DISABLED 0x00 as a pinMode() value.
// The project doesn't actually call pinMode(p, DISABLED) anywhere, so we
// permanently undef it here to free the identifier for our BleRole enum
// (both the enum body AND every BleRole::DISABLED use site below).
#undef DISABLED

// ── BLE save file ────────────────────────────────────────────
#define BLE_CONFIG_FILE   "/ble_config.json"
#define BLE_BONDS_FILE    "/ble_bonds.json"
#define BLE_MAX_DEVICES   32
#define BLE_SCAN_SECONDS  10
#define BLE_CONNECT_TIMEOUT_MS  8000

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
#define UUID_SVC_USER_DATA        "0000181c-0000-1000-8000-00805f9b34fb"
#define UUID_CHR_STEP_COUNT       "00002aff-0000-1000-8000-00805f9b34fb"  // steps (some vendors)
// Generic Access
#define UUID_SVC_GENERIC_ACCESS   "00001800-0000-1000-8000-00805f9b34fb"
#define UUID_CHR_DEVICE_NAME      "00002a00-0000-1000-8000-00805f9b34fb"

// ── BLE role enum ─────────────────────────────────────────────
// DISABLED macro is #undef'd at the top of this header (see note above).
enum class BleRole : uint8_t {
    DISABLED   = 0,
    SCANNER    = 1,   // passive scan only
    CENTRAL    = 2,   // scan + connect + read GATT
    PERIPHERAL = 3,   // advertise as custom device
    HID_KB     = 4,   // keyboard HID
    HID_MEDIA  = 5,   // media control HID
    HID_GAMEPAD= 6,   // gamepad HID
    PROXY      = 7    // Feature #P: Watch<-ESP32->Phone dual-role proxy
};

// ── Proxy state machine ───────────────────────────────────────
enum class ProxyState : uint8_t {
    IDLE            = 0,  // proxy not running
    SCANNING        = 1,  // scanning for watch
    CONNECTING_WATCH= 2,  // connecting to watch (central side)
    WATCH_CONNECTED = 3,  // watch connected, now advertising to phone
    PHONE_CONNECTED = 4,  // both sides live — full proxy active
    ERROR           = 5
};

// ── Proxy config (persisted in /ble_proxy.json) ───────────────
struct ProxyConfig {
    String  watchAddress;      // target watch MAC  e.g. "AA:BB:CC:DD:EE:FF"
    String  spoofName;         // name ESP32 shows phone  e.g. "Mi Band 6"
    bool    forwardHR    = true;
    bool    forwardBatt  = true;
    bool    forwardTemp  = true;
    bool    autoStart    = false;  // start proxy automatically on boot
};

// ── Scan result entry ─────────────────────────────────────────
struct BleScanResult {
    String  address;         // "AA:BB:CC:DD:EE:FF"
    String  name;            // advertised name or ""
    int     rssi;            // signal strength dBm
    bool    connectable;
    // Decoded service hints
    bool    hasHeartRate   = false;
    bool    hasHealthTherm = false;
    bool    hasBattery     = false;
    bool    hasDeviceInfo  = false;
    bool    isWatch        = false;  // heuristic: HR + battery + name match
    bool    isPhone        = false;
    bool    isESP32        = false;
    String  rawServices;     // comma-separated UUIDs
};

// ── Live smartwatch data ──────────────────────────────────────
struct WatchData {
    bool    connected      = false;
    String  address;
    String  name;
    int     heartRate      = 0;      // bpm
    int     spo2           = 0;      // %
    int     steps          = 0;
    int     battery        = -1;     // % or -1 unknown
    float   temperature    = 0.0f;   // °C from health therm
    String  manufacturer;
    String  model;
    String  firmwareRev;
    unsigned long lastUpdate = 0;    // millis()
    bool    hrNotifying    = false;
    bool    tempNotifying  = false;
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
    BleRole   role          = BleRole::SCANNER;
    String    deviceName    = "ESP32-BLE";   // peripheral name
    bool      autoConnect   = false;         // reconnect to last device
    String    lastAddress;                   // last connected address
    bool      hidPassthrough= false;         // forward IR events as HID keys
    bool      bondingOnly   = false;         // Feature #8: reject unbonded devices
};

// ── Bluetooth Module class ────────────────────────────────────
// Forward-declare the friend callback classes (defined in bluetooth_module.cpp).
// These touch private members (state, broadcast cb, watch data) directly
// because they're invoked from the BLE stack with no `this` to dispatch on.
class BtServerCallbacks;
class BtClientCallbacks;
class ProxyServerCb;
class ProxyWatchClientCb;

class BluetoothModule {
public:
    BluetoothModule() = default;

    // BLE-stack callbacks reach into our private state directly.
    friend class BtServerCallbacks;
    friend class BtClientCallbacks;
    friend class ProxyServerCb;
    friend class ProxyWatchClientCb;

    void begin();
    void loop();

    // ── Config ───────────────────────────────────────────────
    BleConfig getConfig() const { return _cfg; }
    void      saveConfig(const BleConfig& cfg);
    void      applyRole(BleRole role);

    // ── Feature #4: set WebSocket broadcast callback ──────────
    void setWsBroadcastCb(std::function<void(const String&)> cb) { _wsBroadcastCb = cb; }

    // ── Scan ─────────────────────────────────────────────────
    void startScan(uint8_t durationSec = BLE_SCAN_SECONDS);
    void stopScan();
    bool isScanning()  const { return _scanning; }
    const std::vector<BleScanResult>& scanResults() const { return _scanResults; }

    // ── Central / Connect ─────────────────────────────────────
    bool connect(const String& address);
    void disconnect();
    bool isConnected() const { return _connected; }
    const WatchData& watchData() const { return _watchData; }

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

    // ── Feature #12: Multi-Connect (up to 3 centrals) ────────
    bool        connectMulti(const String& address, uint8_t slot = 0xFF); // 0xFF = auto slot
    void        disconnectSlot(uint8_t slot);
    bool        isSlotConnected(uint8_t slot) const;
    uint8_t     connectedCount() const;
    String      multiStatusJson() const;

    // ── Feature #13: BLE Time Sync (CTS) ─────────────────────
    bool        syncTimeFromPhone();   // reads CTS from connected central
    bool        isTimeSynced()   const { return _timeSynced; }
    time_t      lastSyncTime()   const { return _lastSyncEpoch; }
    String      timeSyncJson()   const;

    // ── Feature #4: BLE Status WebSocket broadcast ────────────
    void        broadcastBleStatus();  // called from loop(), pushes WS event

    // ── Feature #P: BLE Proxy (Watch<->ESP32<->Phone) ────────
    bool        proxyStart(const ProxyConfig& cfg);  // begin proxy
    void        proxyStop();                          // stop proxy, restore normal mode
    bool        proxyIsRunning()  const { return _proxyState != ProxyState::IDLE && _proxyState != ProxyState::ERROR; }
    ProxyState  proxyGetState()   const { return _proxyState; }
    String      proxyStateStr()   const;
    String      proxyStatusJson() const;
    void        proxySaveConfig(const ProxyConfig& cfg);
    ProxyConfig proxyLoadConfig() const;
    const WatchData& proxyWatchData() const { return _proxyWatchData; }

    // ── Status ────────────────────────────────────────────────
    BleRole role()       const { return _cfg.role; }
    bool    isEnabled()  const { return _cfg.enabled; }
    String  roleString() const;
    String  statusJson() const;

private:
    BleConfig  _cfg;
    bool       _scanning     = false;
    bool       _connected    = false;
    bool       _advertising  = false;
    bool       _hidPeerConnected = false;
    int        _peerRssi     = 0;
    WatchData  _watchData;
    std::vector<BleScanResult> _scanResults;

    // Internal BLE stack handles (opaque pointers, impl in .cpp)
    void*  _pClient     = nullptr;
    void*  _pScan       = nullptr;
    void*  _pServer     = nullptr;
    void*  _pAdvertising= nullptr;
    void*  _pHidDevice  = nullptr;

    // Feature #8: bonding
    std::vector<BondedDevice> _bondedDevices;
    void _loadBonds();
    void _saveBonds();

    // Feature #12: multi-connect slots (max 3)
    static constexpr uint8_t BLE_MAX_SLOTS = 3;
    struct CentralSlot {
        bool    active    = false;
        bool    connected = false;
        String  address;
        String  name;
        void*   pClient   = nullptr;
        WatchData watchData;
    };
    CentralSlot _slots[BLE_MAX_SLOTS];
    uint8_t     _slotCount = 0;

    // Feature #13: CTS time sync
    bool    _timeSynced    = false;
    time_t  _lastSyncEpoch = 0;
    unsigned long _lastSyncMs = 0;

    // Feature #4: WS status broadcast
    unsigned long _lastBleBroadcast = 0;
    String        _lastBleStatusJson;
    std::function<void(const String&)> _wsBroadcastCb; // set by main.cpp

    // Feature #P: Proxy state
    ProxyState    _proxyState     = ProxyState::IDLE;
    ProxyConfig   _proxyCfg;
    WatchData     _proxyWatchData;           // data received from watch
    void*         _proxyWatchClient  = nullptr;  // BLEClient* to watch
    void*         _proxyPhoneServer  = nullptr;  // BLEServer* for phone
    void*         _proxyHrChr        = nullptr;  // phone-side HR characteristic
    void*         _proxyBattChr      = nullptr;  // phone-side Battery characteristic
    void*         _proxyTempChr      = nullptr;  // phone-side Temp characteristic
    bool          _proxyPhoneConnected = false;
    // PROXY FIX: guards _proxy*Chr / _proxyPhoneServer / _proxyWatchClient
    // against torn reads. proxyStop() (called from API task) sets them to
    // nullptr while _proxyForward{HR,Battery,Temp} (called from the BLE
    // host task) deref them - without this mutex the forwarders could see
    // a half-cleared pointer and crash, or worse use-after-free if the
    // host task had already read the pointer just before proxyStop freed
    // the underlying server. Recursive mutex because some callers nest.
    SemaphoreHandle_t _proxyMutex = nullptr;
    unsigned long _proxyLastRetry    = 0;
    uint8_t       _proxyRetryCount   = 0;

    void _proxyConnectWatch();
    void _proxySetupPhoneServer();
    void _proxyForwardHR(uint8_t* data, size_t len);
    void _proxyForwardBattery(uint8_t level);
    void _proxyForwardTemp(uint8_t* data, size_t len);
    void _proxyLoop();  // called from loop()
    static void _proxyOnHrNotify(void* chr, uint8_t* data, size_t len, bool notify);
    static void _proxyOnTempNotify(void* chr, uint8_t* data, size_t len, bool notify);

    unsigned long _scanStartMs   = 0;
    unsigned long _connectTimeMs = 0;
    bool          _pendingConnect= false;
    String        _pendingAddr;

    void _loadConfig();
    void _initStack();
    void _startCentral();
    void _startPeripheral();
    void _startHid(BleRole hidRole);
    void _stopAll();
    void _parseScanResult(void* pAdvertisedDevice);
    bool _readGattServices();
    void _subscribeNotifications();
    static void _onHrNotify(void* pBLERemoteCharacteristic,
                            uint8_t* pData, size_t length, bool isNotify);
    static void _onTempNotify(void* pBLERemoteCharacteristic,
                              uint8_t* pData, size_t length, bool isNotify);
};

extern BluetoothModule btModule;
