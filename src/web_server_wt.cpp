// ============================================================
//  web_server_wt.cpp  -  Walkie-Talkie (half/full duplex)
//  Phone mic  → WS binary → ESP32 I2S speaker (DAC output)
//  ESP32 mic  → WS binary → Phone AudioContext playback
//  Protocol: first byte = channel tag
//    0x01 = ESP32→Phone PCM16 mono 16kHz
//    0x02 = Phone→ESP32 PCM16 mono 16kHz
//    0x10 = control JSON (null-terminated or length-prefixed)
// ============================================================
#include "web_server.h"
#include "mic_module.h"
#include "speaker_module.h"
#include "auth_manager.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

extern AuthManager  authMgr;
extern WebUI        webUI;

// ── WS endpoint ──────────────────────────────────────────────
#define WT_WS_PATH    "/wt"
#define WT_CHUNK      512          // PCM bytes per frame (256 samples @ 16kHz)
#define WT_SAMPLE_HZ  16000

// ── State ─────────────────────────────────────────────────────
static AsyncWebSocket  _wtWs(WT_WS_PATH);

// half-duplex: only one side talks at a time
// full-duplex: both sides stream simultaneously (echo may occur)
static bool     _wtFullDuplex = false;
static bool     _wtEnabled    = false;
static bool     _wtPhoneTalking = false;   // phone is pushing audio
static bool     _wtEspTalking   = false;   // ESP is streaming to phone

// ── ESP→Phone stream task ─────────────────────────────────────
static TaskHandle_t _wtTask = nullptr;
static bool         _wtTaskStop = false;

static void wtEspToPhoneTask(void*) {
    uint8_t* frame = (uint8_t*)malloc(WT_CHUNK + 1);
    if (!frame) { _wtTask = nullptr; vTaskDelete(NULL); return; }
    frame[0] = 0x01;   // channel tag: ESP32→Phone
    while (!_wtTaskStop) {
        // only stream when someone is connected and ESP side is active
        if (_wtWs.count() > 0 && _wtEnabled &&
            (_wtFullDuplex || !_wtPhoneTalking)) {
            size_t n = micModule.readChunk(frame + 1, WT_CHUNK);
            if (n > 0) {
                // Use makeBuffer+binaryAll(buffer*) — the shared buffer variant
                // is reference-counted and queued safely from non-loop contexts.
                AsyncWebSocketMessageBuffer* msg = _wtWs.makeBuffer(frame, n + 1);
                if (msg) _wtWs.binaryAll(msg);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(16));   // ~62 fps ≈ 16kHz / 256 smp
    }
    free(frame);
    _wtTask = nullptr;
    vTaskDelete(NULL);
}

// ── WS event handler ─────────────────────────────────────────
static void onWtEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len)
{
    (void)srv;
    switch (type) {
        case WS_EVT_CONNECT:
            srv->cleanupClients(4);
            client->text("{\"event\":\"wt_ready\",\"sampleRate\":" +
                         String(WT_SAMPLE_HZ) + ",\"fullDuplex\":" +
                         (_wtFullDuplex ? "true" : "false") + "}");
            break;

        case WS_EVT_DISCONNECT:
            _wtPhoneTalking = false;
            if (srv->count() == 0) {
                _wtEnabled = false;
                // stop ESP stream task when last client leaves
                _wtTaskStop = true;
            }
            break;

        case WS_EVT_DATA: {
            AwsFrameInfo* fi = (AwsFrameInfo*)arg;
            if (!fi->final || fi->index != 0 || fi->len != len) break;

            if (fi->opcode == WS_TEXT) {
                // Control message
                char buf[64] = {0};
                memcpy(buf, data, len < 63 ? len : 63);
                // {"cmd":"start"} / {"cmd":"stop"} / {"cmd":"ptt","v":true}
                // {"cmd":"duplex","full":true}
                JsonDocument doc;
                if (!deserializeJson(doc, buf)) {
                    const char* cmd = doc["cmd"] | "";
                    if (strcmp(cmd, "start") == 0) {
                        _wtEnabled = true;
                        // start ESP→Phone task if not running
                        if (!_wtTask) {
                            _wtTaskStop = false;
                            micModule.startStream();
                            xTaskCreatePinnedToCore(
                                wtEspToPhoneTask, "wt_esp", 6144, nullptr, 1,
                                &_wtTask, 0);
                        }
                        client->text("{\"event\":\"wt_started\"}");
                    } else if (strcmp(cmd, "stop") == 0) {
                        _wtEnabled = false;
                        _wtTaskStop = true;
                        micModule.stopStream();
                        client->text("{\"event\":\"wt_stopped\"}");
                    } else if (strcmp(cmd, "ptt") == 0) {
                        bool v = doc["v"] | false;
                        _wtPhoneTalking = v;
                        // in half-duplex: pause ESP stream while phone talks
                        if (!_wtFullDuplex) {
                            _wtEspTalking = !v;
                        }
                        client->text(String("{\"event\":\"ptt\",\"v\":") +
                                     (v ? "true" : "false") + "}");
                    } else if (strcmp(cmd, "duplex") == 0) {
                        _wtFullDuplex = doc["full"] | false;
                        client->text(String("{\"event\":\"duplex\",\"full\":") +
                                     (_wtFullDuplex ? "true" : "false") + "}");
                    }
                }
            } else if (fi->opcode == WS_BINARY) {
                // Phone→ESP32: PCM16 audio, play on I2S speaker
                // First byte is channel tag (0x02), rest is PCM data
                if (len < 2) break;
                uint8_t tag = data[0];
                if (tag == 0x02) {
                    const int16_t* pcm = reinterpret_cast<const int16_t*>(data + 1);
                    size_t samples = (len - 1) / sizeof(int16_t);
                    if (samples > 0) {
                        // Route to speaker module (PAM8043) if enabled, else mic I2S
                        if (speakerModule.isEnabled())
                            speakerModule.playSamples(pcm, samples);
                        else
                            micModule.playSamples(pcm, samples);
                    }
                }
            }
            break;
        }
        case WS_EVT_ERROR:
            break;
        default: break;
    }
}

// ── Route setup (called from WebUI::begin) ────────────────────
void WebUI::setupWalkieTalkieRoutes() {
    _wtWs.onEvent(onWtEvent);
    _server.addHandler(&_wtWs);

    // GET /api/wt/status
    _server.on("/api/wt/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        String j = "{\"enabled\":" + String(_wtEnabled ? "true" : "false") +
                   ",\"fullDuplex\":" + (_wtFullDuplex ? "true" : "false") +
                   ",\"phoneTalking\":" + (_wtPhoneTalking ? "true" : "false") +
                   ",\"clients\":" + String(_wtWs.count()) +
                   ",\"sampleRate\":" + String(WT_SAMPLE_HZ) + "}";
        sendJson(req, 200, j);
    });

    // POST /api/wt/config  { "fullDuplex": bool }
    _server.on("/api/wt/config", HTTP_POST,
        [](AsyncWebServerRequest* req){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
           size_t /*index*/, size_t /*total*/) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc;
            if (!deserializeJson(doc, data, len)) {
                if (!doc["fullDuplex"].isNull())
                    _wtFullDuplex = doc["fullDuplex"] | false;
            }
            sendJson(req, 200,
                String("{\"ok\":true,\"fullDuplex\":") +
                (_wtFullDuplex ? "true" : "false") + "}");
        });
}
