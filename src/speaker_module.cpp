// ============================================================
//  speaker_module.cpp  -  PAM8043 I2S Speaker Driver
// ============================================================
#include "speaker_module.h"
#include <math.h>

SpeakerModule speakerModule;

// ── begin ─────────────────────────────────────────────────────
void SpeakerModule::begin() {
    _cfg = loadConfig();
    if (_cfg.enabled) {
        _initI2S();
    }
}

// ── I2S init/deinit ───────────────────────────────────────────
bool SpeakerModule::_initI2S() {
    _deinitI2S();
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = _cfg.sampleRate,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = _cfg.stereo ? I2S_CHANNEL_FMT_RIGHT_LEFT
                                            : I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = SPK_DMA_BUF_COUNT,
        .dma_buf_len          = SPK_DMA_BUF_LEN,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,
        .fixed_mclk           = 0,
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = _cfg.bclkPin,
        .ws_io_num    = _cfg.lrckPin,
        .data_out_num = _cfg.dinPin,
        .data_in_num  = I2S_PIN_NO_CHANGE,
    };
    if (i2s_driver_install(SPK_I2S_PORT, &cfg, 0, NULL) != ESP_OK) {
        Serial.println("[SPK] I2S install failed");
        return false;
    }
    if (i2s_set_pin(SPK_I2S_PORT, &pins) != ESP_OK) {
        Serial.println("[SPK] I2S pin config failed");
        i2s_driver_uninstall(SPK_I2S_PORT);
        return false;
    }
    i2s_zero_dma_buffer(SPK_I2S_PORT);
    _i2sInited = true;
    _enabled   = true;
    Serial.printf("[SPK] I2S_NUM_1 OK  BCLK=%u LRC=%u DIN=%u %uHz %s\n",
        _cfg.bclkPin, _cfg.lrckPin, _cfg.dinPin,
        _cfg.sampleRate, _cfg.stereo ? "STEREO" : "MONO");
    return true;
}

void SpeakerModule::_deinitI2S() {
    stopTone();
    if (_i2sInited) {
        i2s_driver_uninstall(SPK_I2S_PORT);
        _i2sInited = false;
    }
    _enabled = false;
    _playing = false;
}

// ── applyConfig ───────────────────────────────────────────────
bool SpeakerModule::applyConfig(const SpeakerConfig& cfg) {
    _cfg = cfg;
    if (!cfg.enabled) { _deinitI2S(); return true; }
    return _initI2S();
}

// ── save / load config ────────────────────────────────────────
void SpeakerModule::saveConfig(const SpeakerConfig& cfg) {
    _cfg = cfg;
    JsonDocument doc;
    doc["bclkPin"]    = cfg.bclkPin;
    doc["lrckPin"]    = cfg.lrckPin;
    doc["dinPin"]     = cfg.dinPin;
    doc["enabled"]    = cfg.enabled;
    doc["volume"]     = cfg.volume;
    doc["sampleRate"] = cfg.sampleRate;
    doc["stereo"]     = cfg.stereo;
    File f = LittleFS.open(SPK_CONFIG_FILE, "w");
    if (f) { serializeJson(doc, f); f.close(); }
}

SpeakerConfig SpeakerModule::loadConfig() const {
    SpeakerConfig cfg;
    File f = LittleFS.open(SPK_CONFIG_FILE, "r");
    if (!f) return cfg;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        cfg.bclkPin    = doc["bclkPin"]    | (uint8_t)27;
        cfg.lrckPin    = doc["lrckPin"]    | (uint8_t)14;
        cfg.dinPin     = doc["dinPin"]     | (uint8_t)22;
        cfg.enabled    = doc["enabled"]    | false;
        cfg.volume     = doc["volume"]     | (uint8_t)80;
        cfg.sampleRate = doc["sampleRate"] | (uint32_t)SPK_SAMPLE_RATE;
        cfg.stereo     = doc["stereo"]     | false;
    }
    f.close();
    return cfg;
}

// ── playSamples ───────────────────────────────────────────────
void SpeakerModule::playSamples(const int16_t* samples, size_t count) {
    if (!_i2sInited || !_enabled || !samples || count == 0) return;
    _playing = true;
    // Apply software volume and write
    static int16_t buf[512];
    size_t done = 0;
    while (done < count) {
        size_t chunk = min((size_t)512, count - done);
        for (size_t i = 0; i < chunk; i++)
            buf[i] = _applyVolume(samples[done + i]);
        size_t written = 0;
        i2s_write(SPK_I2S_PORT, buf, chunk * sizeof(int16_t), &written, pdMS_TO_TICKS(20));
        done += chunk;
    }
}

// ── setVolume ─────────────────────────────────────────────────
void SpeakerModule::setVolume(uint8_t v) {
    _cfg.volume = v > 100 ? 100 : v;
}

// ── _applyVolume ──────────────────────────────────────────────
int16_t SpeakerModule::_applyVolume(int32_t sample) const {
    int32_t out = (sample * _cfg.volume) / 100;
    if (out >  32767) out =  32767;
    if (out < -32768) out = -32768;
    return (int16_t)out;
}

// ── Tone generator ────────────────────────────────────────────
void SpeakerModule::startTone(SpkTone type, uint16_t freqHz) {
    if (!_i2sInited) return;
    _toneType  = type;
    _toneFreq  = freqHz;
    _tonePhase = 0;
}

void SpeakerModule::stopTone() {
    _toneType = SpkTone::NONE;
    if (_i2sInited) i2s_zero_dma_buffer(SPK_I2S_PORT);
}

void SpeakerModule::loop() {
    if (!_i2sInited || _toneType == SpkTone::NONE) return;
    static int16_t toneBuf[256];
    const uint32_t sr = _cfg.sampleRate;

    for (int i = 0; i < 256; i++) {
        int16_t s = 0;
        if (_toneType == SpkTone::SINE) {
            float angle = 2.0f * M_PI * _tonePhase * _toneFreq / sr;
            s = _applyVolume((int32_t)(sinf(angle) * 16383));
            _tonePhase++;
        } else if (_toneType == SpkTone::SWEEP) {
            uint16_t f = 200 + (uint16_t)((_tonePhase / 500) % 2000);
            float angle = 2.0f * M_PI * (_tonePhase % (sr / f)) * f / sr;
            s = _applyVolume((int32_t)(sinf(angle) * 16383));
            _tonePhase++;
        } else if (_toneType == SpkTone::NOISE) {
            s = _applyVolume((int32_t)(((int32_t)esp_random() & 0xFFFF) - 32768) / 4);
        }
        toneBuf[i] = s;
    }
    size_t written = 0;
    i2s_write(SPK_I2S_PORT, toneBuf, sizeof(toneBuf), &written, pdMS_TO_TICKS(5));
}

// ── statusJson ────────────────────────────────────────────────
String SpeakerModule::statusJson() const {
    return String("{\"enabled\":") + (_enabled ? "true" : "false") +
           ",\"playing\":" + (_playing ? "true" : "false") +
           ",\"volume\":" + _cfg.volume +
           ",\"sampleRate\":" + _cfg.sampleRate +
           ",\"tone\":" + (int)_toneType +
           ",\"i2sPort\":1}";
}

// ── gpioJson ──────────────────────────────────────────────────
String SpeakerModule::gpioJson() const {
    return String("{\"bclkPin\":") + _cfg.bclkPin +
           ",\"lrckPin\":"  + _cfg.lrckPin +
           ",\"dinPin\":"   + _cfg.dinPin +
           ",\"enabled\":"  + (_cfg.enabled ? "true" : "false") +
           ",\"volume\":"   + _cfg.volume +
           ",\"sampleRate\":" + _cfg.sampleRate +
           ",\"stereo\":"   + (_cfg.stereo ? "true" : "false") + "}";
}
