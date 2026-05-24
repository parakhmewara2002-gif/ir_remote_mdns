#pragma once
// ============================================================
//  speaker_module.h  -  PAM8043 / I2S DAC Speaker Output
//  Uses I2S_NUM_1 (independent from mic I2S_NUM_0)
//  Supports: PCM16 mono/stereo, software volume, tone gen,
//            SD-file playback, BT A2DP sink passthrough
// ============================================================
#include <Arduino.h>
#include <driver/i2s.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#define SPK_I2S_PORT       I2S_NUM_1
#define SPK_SAMPLE_RATE    44100
#define SPK_DMA_BUF_COUNT  8
#define SPK_DMA_BUF_LEN    512
#define SPK_CONFIG_FILE    "/speaker_cfg.json"

// ── Tone/test signal types ────────────────────────────────────
enum class SpkTone : uint8_t {
    NONE    = 0,
    SINE    = 1,   // single frequency sine
    SWEEP   = 2,   // frequency sweep (test)
    NOISE   = 3,   // white noise (test)
};

// ── Persistent config ─────────────────────────────────────────
struct SpeakerConfig {
    uint8_t  bclkPin   = 27;   // Bit clock  → PAM8043 BCK  (26/25 used by Mic I2S)
    uint8_t  lrckPin   = 14;   // Word select → PAM8043 LR / WS
    uint8_t  dinPin    = 22;   // Data out   → PAM8043 DIN
    bool     enabled   = false;
    uint8_t  volume    = 80;   // 0-100 software gain
    uint32_t sampleRate = SPK_SAMPLE_RATE;
    bool     stereo    = false;
};

// ─────────────────────────────────────────────────────────────
class SpeakerModule {
public:
    void   begin();

    // Config
    bool   applyConfig(const SpeakerConfig& cfg);
    void   saveConfig(const SpeakerConfig& cfg);
    SpeakerConfig loadConfig() const;

    // Status
    bool   isEnabled()  const { return _enabled; }
    bool   isPlaying()  const { return _playing; }
    uint8_t volume()    const { return _cfg.volume; }
    String statusJson() const;
    String gpioJson()   const;

    // Play PCM (called by BT, Walkie-Talkie, etc.)
    void   playSamples(const int16_t* samples, size_t count);

    // Software volume (0-100)
    void   setVolume(uint8_t v);

    // Tone generator (for testing PAM8043 wiring)
    void   startTone(SpkTone type, uint16_t freqHz = 1000);
    void   stopTone();
    bool   isToneActive() const { return _toneType != SpkTone::NONE; }

    // Called from main loop — drives tone generator
    void   loop();

private:
    SpeakerConfig _cfg;
    bool     _enabled  = false;
    bool     _playing  = false;
    bool     _i2sInited = false;
    SpkTone  _toneType  = SpkTone::NONE;
    uint16_t _toneFreq  = 1000;
    uint32_t _tonePhase = 0;

    bool _initI2S();
    void _deinitI2S();
    int16_t _applyVolume(int32_t sample) const;
};

extern SpeakerModule speakerModule;
