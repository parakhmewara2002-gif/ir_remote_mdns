// ============================================================
//  web_server_a2dp.cpp  —  BT A2DP Sink API routes
// ============================================================
#include "web_server.h"
#include "bt_a2dp.h"
#include "auth_manager.h"
#include <ArduinoJson.h>

extern AuthManager authMgr;

void WebUI::setupA2dpRoutes() {

    // GET /api/a2dp/status
    _server.on("/api/a2dp/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            sendJson(req, 200, btA2dp.statusJson());
        });

    // POST /api/a2dp/enable  { "en": true/false }
    _server.on("/api/a2dp/enable", HTTP_POST,
        [](AsyncWebServerRequest* req){}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc;
            deserializeJson(doc, data, len);
            bool en = doc["en"] | false;
            bool ok = en ? btA2dp.enable() : (btA2dp.disable(), true);
            sendJson(req, ok?200:500,
                String("{\"ok\":") + (ok?"true":"false") +
                ",\"enabled\":" + (btA2dp.isEnabled()?"true":"false") + "}");
        });

    // POST /api/a2dp/play
    _server.on("/api/a2dp/play", HTTP_POST,
        [](AsyncWebServerRequest* req){}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t, size_t) {
            if (!authMgr.checkAuth(req)) return;
            btA2dp.play();
            sendJson(req, 200, "{\"ok\":true}");
        });

    // POST /api/a2dp/pause
    _server.on("/api/a2dp/pause", HTTP_POST,
        [](AsyncWebServerRequest* req){}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t, size_t) {
            if (!authMgr.checkAuth(req)) return;
            btA2dp.pause();
            sendJson(req, 200, "{\"ok\":true}");
        });

    // POST /api/a2dp/next
    _server.on("/api/a2dp/next", HTTP_POST,
        [](AsyncWebServerRequest* req){}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t, size_t) {
            if (!authMgr.checkAuth(req)) return;
            btA2dp.next();
            sendJson(req, 200, "{\"ok\":true}");
        });

    // POST /api/a2dp/prev
    _server.on("/api/a2dp/prev", HTTP_POST,
        [](AsyncWebServerRequest* req){}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t, size_t) {
            if (!authMgr.checkAuth(req)) return;
            btA2dp.prev();
            sendJson(req, 200, "{\"ok\":true}");
        });

    // POST /api/a2dp/volume  { "v": 0-100 }
    _server.on("/api/a2dp/volume", HTTP_POST,
        [](AsyncWebServerRequest* req){}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc;
            deserializeJson(doc, data, len);
            uint8_t v = doc["v"] | (uint8_t)80;
            btA2dp.setVolume(v);
            sendJson(req, 200, String("{\"ok\":true,\"volume\":") + v + "}");
        });

    // POST /api/a2dp/config  { "deviceName": "...", "enabled": bool, "autoConnect": bool }
    _server.on("/api/a2dp/config", HTTP_POST,
        [](AsyncWebServerRequest* req){}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                sendJson(req, 400, "{\"error\":\"bad json\"}"); return;
            }
            BtA2dpConfig cfg = btA2dp.loadConfig();
            if (!doc["deviceName"].isNull()) cfg.deviceName = doc["deviceName"].as<String>();
            if (!doc["enabled"].isNull())    cfg.enabled    = doc["enabled"]    | false;
            if (!doc["autoConnect"].isNull())cfg.autoConnect= doc["autoConnect"]| false;
            if (!doc["volume"].isNull())     cfg.volume     = doc["volume"]     | (uint8_t)80;
            btA2dp.applyConfig(cfg);
            sendJson(req, 200, "{\"ok\":true}");
        });
}
