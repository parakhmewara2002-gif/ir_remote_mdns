// ============================================================
//  bt_a2dp.cpp  —  Classic BT A2DP Sink + AVRC Controller
// ============================================================
#include "bt_a2dp.h"
#include "speaker_module.h"
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_bt_api.h>
#include <esp_a2dp_api.h>
#include <esp_avrc_api.h>
#include <esp_timer.h>
#include <LittleFS.h>

BtA2dpModule btA2dp;
static BtA2dpModule* _self = nullptr;

// ── GAP callback (discovery / connection name) ────────────────
static void _gapCb(esp_bt_gap_cb_event_t ev, esp_bt_gap_cb_param_t* p) {
    if (!_self) return;
    if (ev == ESP_BT_GAP_READ_REMOTE_NAME_EVT) {
        if (p->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS)
            _self->_onPeerName((char*)p->read_rmt_name.rmt_name);
    }
}

// ── A2DP callback ─────────────────────────────────────────────
static void _a2dpCb(esp_a2d_cb_event_t ev, esp_a2d_cb_param_t* p) {
    if (!_self) return;
    switch (ev) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                _self->_onConnectionState(true, p->conn_stat.remote_bda);
            } else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                _self->_onConnectionState(false, p->conn_stat.remote_bda);
            }
            break;
        case ESP_A2D_AUDIO_STATE_EVT:
            _self->_onAudioState(p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED);
            break;
        default: break;
    }
}

// ── A2DP data callback — PCM audio frames ─────────────────────
static void _a2dpDataCb(const uint8_t* buf, uint32_t len) {
    if (_self) _self->_onAudioData(buf, len);
}

// ── AVRC callback (track info, volume) ───────────────────────
static void _avrcCtCb(esp_avrc_ct_cb_event_t ev, esp_avrc_ct_cb_param_t* p) {
    if (!_self) return;
    switch (ev) {
        case ESP_AVRC_CT_METADATA_RSP_EVT: {
            uint8_t attr = p->meta_rsp.attr_id;
            String val = String((char*)p->meta_rsp.attr_text, p->meta_rsp.attr_length);
            if (attr == ESP_AVRC_MD_ATTR_TITLE)
                _self->_onTrackChanged(val.c_str(), nullptr, 0);
            else if (attr == ESP_AVRC_MD_ATTR_ARTIST)
                _self->_onTrackChanged(nullptr, val.c_str(), 0);
            break;
        }
        case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
            _self->_onVolumeChanged(p->set_volume_rsp.volume);
            break;
        default: break;
    }
}

// ── AVRC target callback (phone controls our volume) ─────────
static void _avrcTgtCb(esp_avrc_tg_cb_event_t ev, esp_avrc_tg_cb_param_t* p) {
    if (!_self) return;
    if (ev == ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT) {
        uint8_t v = p->set_abs_vol.volume; // 0-127
        uint8_t pct = (uint8_t)((uint32_t)v * 100 / 127);
        speakerModule.setVolume(pct);
        _self->_onVolumeChanged(v);
        // Acknowledge to phone: send CHANGED response with new volume
        esp_avrc_rn_param_t rn_param;
        rn_param.volume = v;
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn_param);
    }
}

// ── _initStack ────────────────────────────────────────────────
bool BtA2dpModule::_initStack() {
    if (_initialized) return true;

    // Classic BT controller config — must be done before BLE deinit
    // We share the Bluedroid stack with BLE (BLEDevice::init already called)
    // Just need to register A2DP on top of existing stack

    esp_bt_gap_register_callback(_gapCb);

    // Set device name for classic BT discovery
    esp_bt_dev_set_device_name(_cfg.deviceName.c_str());

    // Set discoverable + connectable
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    // Init A2DP sink
    if (esp_a2d_sink_init() != ESP_OK) {
        Serial.println("[A2DP] sink init failed");
        return false;
    }
    esp_a2d_register_callback(_a2dpCb);
    esp_a2d_sink_register_data_callback(_a2dpDataCb);

    // Init AVRC controller (send commands to phone)
    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(_avrcCtCb);

    // Init AVRC target (receive volume commands from phone)
    esp_avrc_tg_init();
    esp_avrc_tg_register_callback(_avrcTgtCb);

    // Tell phone we support volume change notifications
    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    esp_avrc_tg_set_rn_evt_cap(&evt_set);

    _initialized = true;
    Serial.printf("[A2DP] init OK — device: %s\n", _cfg.deviceName.c_str());
    return true;
}

void BtA2dpModule::_deinitStack() {
    if (!_initialized) return;
    esp_avrc_tg_deinit();
    esp_avrc_ct_deinit();
    esp_a2d_sink_deinit();
    _initialized = false;
    _state = A2dpState::STOPPED;
}

// ── begin ─────────────────────────────────────────────────────
void BtA2dpModule::begin() {
    _self = this;
    _cfg  = loadConfig();
    if (_cfg.enabled) enable();
}

void BtA2dpModule::end() {
    disable();
    _self = nullptr;
}

// ── enable / disable ──────────────────────────────────────────
bool BtA2dpModule::enable() {
    if (_enabled) return true;
    if (!_initStack()) return false;
    _enabled = true;
    _state   = A2dpState::STOPPED;
    Serial.println("[A2DP] enabled — waiting for phone connection");
    return true;
}

void BtA2dpModule::disable() {
    if (!_enabled) return;
    _deinitStack();
    _enabled = false;
    _state   = A2dpState::STOPPED;
    _peerName = "";
    _peerAddr = "";
}

// ── Playback control ──────────────────────────────────────────
static void _avrcReleasedCb(void* arg) {
    uint8_t cmd = (uint8_t)(uintptr_t)arg;
    esp_avrc_ct_send_passthrough_cmd(0, (esp_avrc_pt_cmd_t)cmd,
                                     ESP_AVRC_PT_CMD_STATE_RELEASED);
}

void BtA2dpModule::_avrcSendCmd(uint8_t cmd) {
    if (!_initialized) return;
    esp_avrc_ct_send_passthrough_cmd(0, (esp_avrc_pt_cmd_t)cmd,
                                     ESP_AVRC_PT_CMD_STATE_PRESSED);
    // Schedule RELEASED via one-shot timer — avoids blocking AsyncTCP task
    esp_timer_handle_t t;
    esp_timer_create_args_t args = {
        .callback = _avrcReleasedCb,
        .arg      = (void*)(uintptr_t)cmd,
        .dispatch_method = ESP_TIMER_TASK,
        .name     = "avrc_rel",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 50000); // 50ms in microseconds
    }
}

void BtA2dpModule::play()  { _avrcSendCmd(ESP_AVRC_PT_CMD_PLAY);  }
void BtA2dpModule::pause() { _avrcSendCmd(ESP_AVRC_PT_CMD_PAUSE); }
void BtA2dpModule::next()  { _avrcSendCmd(ESP_AVRC_PT_CMD_FORWARD); }
void BtA2dpModule::prev()  { _avrcSendCmd(ESP_AVRC_PT_CMD_BACKWARD); }

void BtA2dpModule::setVolume(uint8_t pct) {
    if (pct > 100) pct = 100;
    _cfg.volume = pct;
    speakerModule.setVolume(pct);
    // Send absolute volume to phone (0-127 range)
    if (_initialized && (_state == A2dpState::CONNECTED ||
                         _state == A2dpState::PLAYING   ||
                         _state == A2dpState::PAUSED)) {
        uint8_t av = (uint8_t)((uint32_t)pct * 127 / 100);
        esp_avrc_ct_send_set_absolute_volume_cmd(0, av);
    }
}

// ── Callbacks from IDF ────────────────────────────────────────
void BtA2dpModule::_onAudioData(const uint8_t* buf, uint32_t len) {
    if (!speakerModule.isEnabled()) return;
    // A2DP delivers 16-bit STEREO PCM @ 44100 Hz (L,R interleaved)
    // Downmix to mono so speaker I2S_NUM_1 (ONLY_LEFT) plays correct pitch
    const uint32_t stereoSamples = len / (2 * sizeof(int16_t));
    static int16_t monoBuf[512];
    uint32_t outN = 0;
    const int16_t* src = reinterpret_cast<const int16_t*>(buf);
    while (outN < stereoSamples) {
        uint32_t chunk = stereoSamples - outN;
        if (chunk > 512) chunk = 512;
        for (uint32_t i = 0; i < chunk; i++)
            monoBuf[i] = (int16_t)(((int32_t)src[(outN+i)*2] + src[(outN+i)*2+1]) >> 1);
        speakerModule.playSamples(monoBuf, chunk);
        outN += chunk;
    }
    _state = A2dpState::PLAYING;
}

void BtA2dpModule::_onConnectionState(bool connected, const uint8_t* addr) {
    if (connected) {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr[0],addr[1],addr[2],addr[3],addr[4],addr[5]);
        _peerAddr = String(buf);
        _state = A2dpState::CONNECTED;
        Serial.printf("[A2DP] connected: %s\n", buf);
        esp_bt_gap_read_remote_name(const_cast<uint8_t*>(addr));
        // Request track metadata
        esp_avrc_ct_send_metadata_cmd(1, ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST);
    } else {
        _state = A2dpState::STOPPED;
        _peerAddr = "";
        _peerName = "";
        Serial.println("[A2DP] disconnected");
    }
}

void BtA2dpModule::_onAudioState(bool playing) {
    _state = playing ? A2dpState::PLAYING : A2dpState::PAUSED;
}

void BtA2dpModule::_onTrackChanged(const char* title, const char* artist, uint32_t dur) {
    if (title)  _trackTitle  = String(title);
    if (artist) _trackArtist = String(artist);
    if (dur)    _trackDuration = dur;
}

void BtA2dpModule::_onPeerName(const char* name) {
    if (name) _peerName = String(name);
}

void BtA2dpModule::_onVolumeChanged(uint8_t v) {
    // v = 0-127 from A2DP spec
    _cfg.volume = (uint8_t)((uint32_t)v * 100 / 127);
    speakerModule.setVolume(_cfg.volume);
}

// ── Config ────────────────────────────────────────────────────
void BtA2dpModule::saveConfig(const BtA2dpConfig& cfg) {
    _cfg = cfg;
    JsonDocument doc;
    doc["deviceName"]   = cfg.deviceName;
    doc["enabled"]      = cfg.enabled;
    doc["volume"]       = cfg.volume;
    doc["autoConnect"]  = cfg.autoConnect;
    doc["lastPeerAddr"] = cfg.lastPeerAddr;
    File f = LittleFS.open(BTA2DP_CONFIG_FILE, "w");
    if (f) { serializeJson(doc, f); f.close(); }
}

BtA2dpConfig BtA2dpModule::loadConfig() const {
    BtA2dpConfig cfg;
    File f = LittleFS.open(BTA2DP_CONFIG_FILE, "r");
    if (!f) return cfg;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        cfg.deviceName   = doc["deviceName"]   | "IR-Remote Speaker";
        cfg.enabled      = doc["enabled"]      | false;
        cfg.volume       = doc["volume"]       | (uint8_t)80;
        cfg.autoConnect  = doc["autoConnect"]  | false;
        cfg.lastPeerAddr = doc["lastPeerAddr"] | "";
    }
    f.close();
    return cfg;
}

void BtA2dpModule::applyConfig(const BtA2dpConfig& cfg) {
    bool wasEnabled = _enabled;
    if (wasEnabled) disable();
    _cfg = cfg;
    saveConfig(cfg);
    if (cfg.enabled) enable();
}

// ── statusJson ────────────────────────────────────────────────
String BtA2dpModule::statusJson() const {
    static const char* stateStr[] = {"stopped","connecting","connected","playing","paused"};
    uint8_t si = (uint8_t)_state;
    if (si > 4) si = 0;
    JsonDocument doc;
    doc["enabled"]    = _enabled;
    doc["state"]      = stateStr[si];
    doc["peer"]       = _peerName;
    doc["peerAddr"]   = _peerAddr;
    doc["volume"]     = _cfg.volume;
    doc["title"]      = _trackTitle;
    doc["artist"]     = _trackArtist;
    doc["duration"]   = _trackDuration;
    doc["deviceName"] = _cfg.deviceName;
    String j;
    serializeJson(doc, j);
    return j;
}
