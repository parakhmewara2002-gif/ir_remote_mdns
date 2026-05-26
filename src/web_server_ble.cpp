// ============================================================
//  web_server_ble.cpp  —  BLE REST API routes v1.0
//
//  GET  /api/ble/status          — role, advertising, hid status
//  GET  /api/ble/config          — saved config
//  POST /api/ble/config          — save config { enabled, role, name }
//  POST /api/ble/hid/key         — { keyCode, modifier } send keyboard key
//  POST /api/ble/hid/media       — { usage: 0-15 } media control
//  POST /api/ble/hid/gamepad     — { buttons, axisX, axisY, axisRX, axisRY }
//  GET  /api/ble/bonds           — list bonded devices
//  POST /api/ble/bonds/add       — { address, name } bond a device
//  POST /api/ble/bonds/remove    — { address }
//  POST /api/ble/bonds/clear     — wipe all bonds
// ============================================================
#include "web_server.h"
#include "bluetooth_module.h"
#include "auth_manager.h"
#include <ArduinoJson.h>
#include <AsyncJson.h>

extern BluetoothModule btModule;
extern AuthManager authMgr;  // AUTH FIX: gate every BLE endpoint

// ── Local helpers ─────────────────────────────────────────────
static void _bleSendJson(AsyncWebServerRequest* req, int code, const String& json) {
    AsyncWebServerResponse* r = req->beginResponse(code, "application/json", json);
    r->addHeader("Access-Control-Allow-Origin", "*");
    r->addHeader("Cache-Control", "no-cache");
    req->send(r);
}

// Reuse body accumulation pattern from other batch files
static String* _bleGetBuf(AsyncWebServerRequest* req) {
    if (!req->_tempObject) {
        req->_tempObject = new String();
        req->onDisconnect([req]() {
            if (req->_tempObject) {
                delete reinterpret_cast<String*>(req->_tempObject);
                req->_tempObject = nullptr;
            }
        });
    }
    return reinterpret_cast<String*>(req->_tempObject);
}
static void _bleFreeBuf(AsyncWebServerRequest* req) {
    if (req->_tempObject) {
        delete reinterpret_cast<String*>(req->_tempObject);
        req->_tempObject = nullptr;
    }
}

#define BLE_POST(path, handler) \
    _server.on(path, HTTP_POST, \
        [](AsyncWebServerRequest* req){}, \
        nullptr, \
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t i, size_t t) { \
            /* AUTH FIX: gate every BLE state-changing POST. Previously
               unauthenticated, exposing /api/ble/connect, /hid/key,
               /bonds/clear, etc. to any LAN attacker. Done at first
               chunk so we don't buffer body for unauthorised callers. */ \
            if (i == 0 && !authMgr.checkAuth(req)) { return; } \
            if (_bleGetBuf(req)->length() + l > 4096) { \
                _bleFreeBuf(req); \
                _bleSendJson(req, 413, "{\"error\":\"Request too large\"}"); return; \
            } \
            _bleGetBuf(req)->concat((char*)d, l); \
            bool done = (t > 0) ? (i + l >= t) : (i == 0); \
            if (done) { \
                String body = *_bleGetBuf(req); \
                _bleFreeBuf(req); \
                handler(req, body); \
            } \
        })

// ── setupBluetoothRoutes() ────────────────────────────────────
void WebUI::setupBluetoothRoutes() {

    // ── GET /api/ble/status ───────────────────────────────────
    _server.on("/api/ble/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            String j = btModule.statusJson();
            AsyncWebServerResponse* r = req->beginResponse(200, "application/json", j);
            r->addHeader("Access-Control-Allow-Origin", "*");
            r->addHeader("Cache-Control", "no-cache");
            req->send(r);
        });

    // ── GET /api/ble/config ───────────────────────────────────
    _server.on("/api/ble/config", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            BleConfig cfg = btModule.getConfig();
            JsonDocument doc;
            doc["enabled"]     = cfg.enabled;
            doc["role"]        = (uint8_t)cfg.role;
            doc["roleStr"]     = btModule.roleString();
            doc["name"]        = cfg.deviceName;
            doc["autoConnect"] = cfg.autoConnect;
            doc["lastAddr"]    = cfg.lastAddress;
            String j; serializeJson(doc, j);
            AsyncWebServerResponse* r = req->beginResponse(200, "application/json", j);
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        });

    // ── POST /api/ble/config ──────────────────────────────────
    BLE_POST("/api/ble/config",
        ([this](AsyncWebServerRequest* req, const String& body) {
            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                _bleSendJson(req, 400, "{\"error\":\"Bad JSON\"}"); return;
            }
            BleConfig cfg = btModule.getConfig();
            if (doc["enabled"].is<bool>())     cfg.enabled     = doc["enabled"];
            if (doc["role"].is<int>())         cfg.role        = (BleRole)doc["role"].as<int>();
            if (doc["name"].is<const char*>()) cfg.deviceName  = doc["name"].as<String>();
            if (doc["autoConnect"].is<bool>()) cfg.autoConnect = doc["autoConnect"];
            btModule.saveConfig(cfg);
            if (cfg.enabled) btModule.applyRole(cfg.role);
            _bleSendJson(req, 200, "{\"ok\":true}");
        }));

    // ── POST /api/ble/hid/key ─────────────────────────────────
    BLE_POST("/api/ble/hid/key",
        ([this](AsyncWebServerRequest* req, const String& body) {
            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                _bleSendJson(req, 400, "{\"error\":\"Bad JSON\"}"); return;
            }
            uint8_t keyCode  = doc["keyCode"]  | 0;
            uint8_t modifier = doc["modifier"] | 0;
            btModule.hidSendKey(keyCode, modifier);
            _bleSendJson(req, 200, "{\"ok\":true}");
        }));

    // ── POST /api/ble/hid/media ───────────────────────────────
    BLE_POST("/api/ble/hid/media",
        ([this](AsyncWebServerRequest* req, const String& body) {
            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                _bleSendJson(req, 400, "{\"error\":\"Bad JSON\"}"); return;
            }
            uint8_t usage = doc["usage"] | 3;  // default: play/pause
            btModule.hidSendMedia(usage);
            _bleSendJson(req, 200, "{\"ok\":true}");
        }));

    // ── POST /api/ble/hid/gamepad ─────────────────────────────
    BLE_POST("/api/ble/hid/gamepad",
        ([this](AsyncWebServerRequest* req, const String& body) {
            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                _bleSendJson(req, 400, "{\"error\":\"Bad JSON\"}"); return;
            }
            GamepadState gs;
            gs.buttons = doc["buttons"] | 0;
            gs.axisX   = doc["axisX"]   | 0;
            gs.axisY   = doc["axisY"]   | 0;
            gs.axisRX  = doc["axisRX"]  | 0;
            gs.axisRY  = doc["axisRY"]  | 0;
            btModule.hidSendGamepad(gs);
            _bleSendJson(req, 200, "{\"ok\":true}");
        }));

    // ── OPTIONS preflight (CORS) ──────────────────────────────
    _server.on("/api/ble/*", HTTP_OPTIONS,
        [](AsyncWebServerRequest* req) {
            AsyncWebServerResponse* r = req->beginResponse(204);
            r->addHeader("Access-Control-Allow-Origin", "*");
            r->addHeader("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
            r->addHeader("Access-Control-Allow-Headers", "Content-Type");
            req->send(r);
        });

    // ── FEATURE #8: Bonding routes ────────────────────────────

    // GET /api/ble/bonds  — list bonded devices
    _server.on("/api/ble/bonds", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            _bleSendJson(req, 200, btModule.bondsJson());
        });

    // POST /api/ble/bonds/add  — { address, name } bond a device
    BLE_POST("/api/ble/bonds/add",
        ([this](AsyncWebServerRequest* req, const String& body) {
            JsonDocument doc;
            if (deserializeJson(doc, body) || !doc["address"].is<const char*>()) {
                _bleSendJson(req, 400, "{\"error\":\"Missing address\"}"); return;
            }
            String addr = doc["address"].as<String>();
            String name = doc["name"]    | addr;
            bool ok = btModule.bondDevice(addr, name);
            _bleSendJson(req, ok ? 200 : 507, ok
                ? "{\"ok\":true}"
                : "{\"ok\":false,\"error\":\"Bond list full\"}");
        }));

    // POST /api/ble/bonds/remove  — { address }
    BLE_POST("/api/ble/bonds/remove",
        ([this](AsyncWebServerRequest* req, const String& body) {
            JsonDocument doc;
            if (deserializeJson(doc, body) || !doc["address"].is<const char*>()) {
                _bleSendJson(req, 400, "{\"error\":\"Missing address\"}"); return;
            }
            bool ok = btModule.removeBond(doc["address"].as<String>());
            _bleSendJson(req, 200, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Not found\"}");
        }));

    // POST /api/ble/bonds/clear  — wipe all bonds
    _server.on("/api/ble/bonds/clear", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX: wipes pairings
            btModule.clearAllBonds();
            _bleSendJson(req, 200, "{\"ok\":true}");
        });
}
