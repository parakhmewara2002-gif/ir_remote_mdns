#pragma once
// ============================================================
//  bt_a2dp.h  —  Classic BT A2DP Sink + AVRC Controller
//  Phone se music → ESP32 → PAM8043 speaker
//  Uses ESP-IDF Bluedroid stack (same as BLE module)
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>

#define BTA2DP_CONFIG_FILE  "/bt_a2dp.json"

enum class A2dpState : uint8_t {
    STOPPED     = 0,
    CONNECTING  = 1,
    CONNECTED   = 2,
    PLAYING     = 3,
    PAUSED      = 4,
};

struct BtA2dpConfig {
    String  deviceName  = "IR-Remote Speaker";
    bool    enabled     = false;
    uint8_t volume      = 80;   // 0-127 (A2DP absolute volume)
    bool    autoConnect = false;
    String  lastPeerAddr = "";
};

class BtA2dpModule {
public:
    void   begin();
    void   end();

    bool   enable();
    void   disable();
    bool   isEnabled() const { return _enabled; }

    // Playback control (AVRC)
    void   play();
    void   pause();
    void   next();
    void   prev();
    void   setVolume(uint8_t v);   // 0-100

    // Status
    A2dpState  state()    const { return _state; }
    String     peerName() const { return _peerName; }
    String     peerAddr() const { return _peerAddr; }
    uint8_t    volume()   const { return _cfg.volume; }
    String     trackTitle()  const { return _trackTitle; }
    String     trackArtist() const { return _trackArtist; }
    uint32_t   trackDuration() const { return _trackDuration; }
    uint32_t   trackPosition() const { return _trackPosition; }

    // Config
    void   saveConfig(const BtA2dpConfig& cfg);
    BtA2dpConfig loadConfig() const;
    void   applyConfig(const BtA2dpConfig& cfg);

    String statusJson() const;

    // Called from IDF callbacks (public for static callback access)
    void   _onAudioData(const uint8_t* buf, uint32_t len);
    void   _onConnectionState(bool connected, const uint8_t* addr);
    void   _onAudioState(bool playing);
    void   _onTrackChanged(const char* title, const char* artist, uint32_t duration);
    void   _onVolumeChanged(uint8_t v);
    void   _onPeerName(const char* name);

private:
    BtA2dpConfig _cfg;
    bool         _enabled    = false;
    bool         _initialized = false;
    A2dpState    _state      = A2dpState::STOPPED;
    String       _peerName;
    String       _peerAddr;
    String       _trackTitle;
    String       _trackArtist;
    uint32_t     _trackDuration = 0;
    uint32_t     _trackPosition = 0;

    void   _avrcSendCmd(uint8_t cmd);
    bool   _initStack();
    void   _deinitStack();
};

extern BtA2dpModule btA2dp;
