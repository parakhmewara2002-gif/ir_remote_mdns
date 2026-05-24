// ============================================================
//  web_server_spk.cpp  -  PAM8043 Speaker API routes
// ============================================================
#include "web_server.h"
#include "speaker_module.h"
#include "auth_manager.h"
#include "gpio_config.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

extern AuthManager authMgr;

// Macro reuse from web_server.cpp
#define SPK_POST(path, handler) \
    _server.on(path, HTTP_POST, \
        [](AsyncWebServerRequest* req){}, nullptr, \
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, \
           size_t /*idx*/, size_t /*total*/) { handler })

void WebUI::setupSpeakerRoutes() {

    // GET /api/speaker/status
    _server.on("/api/speaker/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            sendJson(req, 200, speakerModule.statusJson());
        });

    // GET /api/speaker/gpio  — current pin config
    _server.on("/api/speaker/gpio", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            sendJson(req, 200, speakerModule.gpioJson());
        });

    // POST /api/speaker/gpio  — save + apply config
    _server.on("/api/speaker/gpio", HTTP_POST,
        [](AsyncWebServerRequest* req){}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
           size_t, size_t) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                sendJson(req, 400, "{\"error\":\"bad json\"}");
                return;
            }
            SpeakerConfig cfg;
            cfg.bclkPin    = doc["bclkPin"]    | (uint8_t)26;
            cfg.lrckPin    = doc["lrckPin"]    | (uint8_t)25;
            cfg.dinPin     = doc["dinPin"]     | (uint8_t)22;
            cfg.enabled    = doc["enabled"]    | false;
            cfg.volume     = doc["volume"]     | (uint8_t)80;
            cfg.sampleRate = doc["sampleRate"] | (uint32_t)44100;
            cfg.stereo     = doc["stereo"]     | false;

            // Pin conflict check — reject forbidden pins
            auto chk = [](uint8_t pin) -> bool {
                for (uint8_t i = 0; i < FORBIDDEN_COUNT; i++)
                    if (FORBIDDEN_PINS[i] == pin) return false;
                for (uint8_t i = 0; i < INPUT_ONLY_COUNT; i++)
                    if (INPUT_ONLY_PINS[i] == pin) return false;
                return true;
            };
            if (cfg.enabled && (!chk(cfg.bclkPin) || !chk(cfg.lrckPin) || !chk(cfg.dinPin))) {
                sendJson(req, 409,
                    "{\"error\":\"Pin conflict: forbidden/input-only GPIO selected\"}");
                return;
            }
            speakerModule.saveConfig(cfg);
            bool ok = speakerModule.applyConfig(cfg);
            sendJson(req, ok ? 200 : 500,
                String("{\"ok\":") + (ok?"true":"false") +
                ",\"enabled\":" + (cfg.enabled?"true":"false") + "}");
        });

    // POST /api/speaker/volume  { "v": 0-100 }
    _server.on("/api/speaker/volume", HTTP_POST,
        [](AsyncWebServerRequest* req){}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc;
            deserializeJson(doc, data, len);
            uint8_t v = doc["v"] | (uint8_t)80;
            speakerModule.setVolume(v);
            sendJson(req, 200, String("{\"ok\":true,\"volume\":") + v + "}");
        });

    // POST /api/speaker/tone  { "type": 0-3, "freq": 1000 }
    _server.on("/api/speaker/tone", HTTP_POST,
        [](AsyncWebServerRequest* req){}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc;
            deserializeJson(doc, data, len);
            uint8_t  t = doc["type"] | (uint8_t)0;
            uint16_t f = doc["freq"] | (uint16_t)1000;
            if (t == 0) speakerModule.stopTone();
            else        speakerModule.startTone((SpkTone)t, f);
            sendJson(req, 200, String("{\"ok\":true,\"tone\":") + t + "}");
        });

    // POST /api/speaker/stop  — stop tone / silence
    _server.on("/api/speaker/stop", HTTP_POST,
        [](AsyncWebServerRequest* req){}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!authMgr.checkAuth(req)) return;
            speakerModule.stopTone();
            sendJson(req, 200, "{\"ok\":true}");
        });

    // GET /api/speaker/pins  — ESP32 pin info for UI dropdowns
    _server.on("/api/speaker/pins", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            // Return safe output-capable GPIO list with recommended I2S pins
            String j = "[";
            struct PinInfo { uint8_t gpio; const char* note; uint8_t quality; };
            static const PinInfo pins[] = {
                {22, "I2S recommended",    3},
                {25, "I2S/DAC1 best",      3},
                {26, "I2S/DAC2 best",      3},
                {27, "Good",               2},
                {33, "Good",               2},
                {32, "Good",               2},
                {17, "Good",               2},
                {16, "Good",               2},
                {13, "OK",                 1},
                {21, "OK (I2C SDA)",       1},
                {19, "OK",                 1},
                {18, "OK",                 1},
                {23, "OK",                 1},
                {4,  "OK (boot careful)",  1},
            };
            bool first = true;
            for (auto& p : pins) {
                if (!first) j += ",";
                j += "{\"gpio\":" + String(p.gpio) +
                     ",\"note\":\"" + p.note +
                     "\",\"quality\":" + p.quality + "}";
                first = false;
            }
            j += "]";
            sendJson(req, 200, j);
        });
}
