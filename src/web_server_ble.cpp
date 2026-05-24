// ============================================================
//  web_server_ble.cpp  —  BLE REST API routes v1.0
//
//  GET  /api/ble/status          — role, connected, scanning, watch data
//  GET  /api/ble/config          — saved config
//  POST /api/ble/config          — save config { enabled, role, name, autoConnect }
//  POST /api/ble/scan/start      — start BLE scan
//  POST /api/ble/scan/stop       — stop scan
//  GET  /api/ble/scan/results    — list of discovered devices
//  POST /api/ble/connect         — { address } connect to BLE device
//  POST /api/ble/disconnect      — disconnect current device
//  GET  /api/ble/watch           — live smartwatch data
//  POST /api/ble/hid/key         — { keyCode, modifier } send keyboard key
//  POST /api/ble/hid/media       — { usage: 0-15 } media control
//  POST /api/ble/hid/gamepad     — { buttons, axisX, axisY, axisRX, axisRY }
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
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX: leaks lastAddress
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

    // ── POST /api/ble/scan/start ──────────────────────────────
    _server.on("/api/ble/scan/start", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            uint8_t dur = 10;
            if (req->hasParam("duration"))
                dur = (uint8_t)req->getParam("duration")->value().toInt();
            btModule.startScan(dur);
            _bleSendJson(req, 200, "{\"ok\":true,\"scanning\":true}");
        });

    // ── POST /api/ble/scan/stop ───────────────────────────────
    _server.on("/api/ble/scan/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            btModule.stopScan();
            _bleSendJson(req, 200, "{\"ok\":true,\"scanning\":false}");
        });

    // ── GET /api/ble/scan/results ─────────────────────────────
    _server.on("/api/ble/scan/results", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX: nearby device list
            const auto& results = btModule.scanResults();
            JsonDocument doc;
            JsonArray arr = doc["devices"].to<JsonArray>();
            for (const auto& d : results) {
                JsonObject o = arr.add<JsonObject>();
                o["address"]      = d.address;
                o["name"]         = d.name;
                o["rssi"]         = d.rssi;
                o["connectable"]  = d.connectable;
                o["isWatch"]      = d.isWatch;
                o["isPhone"]      = d.isPhone;
                o["isESP32"]      = d.isESP32;
                o["hasHeartRate"] = d.hasHeartRate;
                o["hasBattery"]   = d.hasBattery;
                o["services"]     = d.rawServices;
            }
            doc["count"]    = results.size();
            doc["scanning"] = btModule.isScanning();
            String j; serializeJson(doc, j);
            AsyncWebServerResponse* r = req->beginResponse(200, "application/json", j);
            r->addHeader("Access-Control-Allow-Origin", "*");
            r->addHeader("Cache-Control", "no-cache");
            req->send(r);
        });

    // ── POST /api/ble/connect ─────────────────────────────────
    BLE_POST("/api/ble/connect",
        ([this](AsyncWebServerRequest* req, const String& body) {
            JsonDocument doc;
            if (deserializeJson(doc, body) || !doc["address"].is<const char*>()) {
                _bleSendJson(req, 400, "{\"error\":\"Missing address\"}"); return;
            }
            String addr = doc["address"].as<String>();
            bool ok = btModule.connect(addr);
            if (ok)
                _bleSendJson(req, 200, "{\"ok\":true,\"connected\":true}");
            else
                _bleSendJson(req, 503, "{\"ok\":false,\"error\":\"Connection failed\"}");
        }));

    // ── POST /api/ble/disconnect ──────────────────────────────
    _server.on("/api/ble/disconnect", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            btModule.disconnect();
            _bleSendJson(req, 200, "{\"ok\":true,\"connected\":false}");
        });

    // ── GET /api/ble/watch ────────────────────────────────────
    _server.on("/api/ble/watch", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX: HR/SpO2/steps PII
            const WatchData& w = btModule.watchData();
            JsonDocument doc;
            doc["connected"]   = w.connected;
            doc["address"]     = w.address;
            doc["name"]        = w.name;
            doc["heartRate"]   = w.heartRate;
            doc["spo2"]        = w.spo2;
            doc["steps"]       = w.steps;
            doc["battery"]     = w.battery;
            doc["temperature"] = w.temperature;
            doc["manufacturer"]= w.manufacturer;
            doc["model"]       = w.model;
            doc["firmwareRev"] = w.firmwareRev;
            doc["hrNotifying"] = w.hrNotifying;
            doc["lastUpdate"]  = w.lastUpdate;
            String j; serializeJson(doc, j);
            AsyncWebServerResponse* r = req->beginResponse(200, "application/json", j);
            r->addHeader("Access-Control-Allow-Origin", "*");
            r->addHeader("Cache-Control", "no-cache");
            req->send(r);
        });

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

    // ── FEATURE #12: Multi-connect routes ────────────────────

    // GET /api/ble/multi  — slot status
    _server.on("/api/ble/multi", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            _bleSendJson(req, 200, btModule.multiStatusJson());
        });

    // POST /api/ble/multi/connect  — { address, slot? }
    BLE_POST("/api/ble/multi/connect",
        ([this](AsyncWebServerRequest* req, const String& body) {
            JsonDocument doc;
            if (deserializeJson(doc, body) || !doc["address"].is<const char*>()) {
                _bleSendJson(req, 400, "{\"error\":\"Missing address\"}"); return;
            }
            String  addr = doc["address"].as<String>();
            uint8_t slot = doc["slot"]   | 0xFF;
            bool ok = btModule.connectMulti(addr, slot);
            if (ok)
                _bleSendJson(req, 200, "{\"ok\":true}");
            else
                _bleSendJson(req, 503, "{\"ok\":false,\"error\":\"Connect failed or not bonded\"}");
        }));

    // POST /api/ble/multi/disconnect  — { slot }
    BLE_POST("/api/ble/multi/disconnect",
        ([this](AsyncWebServerRequest* req, const String& body) {
            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                _bleSendJson(req, 400, "{\"error\":\"Bad JSON\"}"); return;
            }
            uint8_t slot = doc["slot"] | 0;
            btModule.disconnectSlot(slot);
            _bleSendJson(req, 200, "{\"ok\":true}");
        }));

    // ── FEATURE #13: Time Sync routes ────────────────────────

    // GET /api/ble/timesync  — sync status
    _server.on("/api/ble/timesync", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            _bleSendJson(req, 200, btModule.timeSyncJson());
        });

    // POST /api/ble/timesync/sync  — trigger sync from connected phone
    _server.on("/api/ble/timesync/sync", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            bool ok = btModule.syncTimeFromPhone();
            if (ok)
                _bleSendJson(req, 200, "{\"ok\":true,\"message\":\"Time synced from phone via CTS\"}");
            else
                _bleSendJson(req, 503,
                    "{\"ok\":false,\"error\":\"No CTS found — ensure phone is connected in Central mode\"}");
        });

    // ── FEATURE #P: BLE Proxy routes ─────────────────────────
    //
    //  GET  /api/ble/proxy          — proxy status + watch data
    //  GET  /api/ble/proxy/config   — saved proxy config
    //  POST /api/ble/proxy/config   — save config
    //  POST /api/ble/proxy/start    — start proxy (uses saved config)
    //  POST /api/ble/proxy/stop     — stop proxy
    //
    //  Proxy flow:
    //    1. Save config with watch MAC + spoof name
    //    2. POST /start  → ESP32 connects watch, watch drops phone
    //    3. ESP32 advertises as fake watch
    //    4. Phone reconnects to ESP32 (thinks it's the watch)
    //    5. All HR/Battery/Temp forwarded watch→ESP32→phone
    // ─────────────────────────────────────────────────────────

    // GET /api/ble/proxy
    _server.on("/api/ble/proxy", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            _bleSendJson(req, 200, btModule.proxyStatusJson());
        });

    // GET /api/ble/proxy/config
    _server.on("/api/ble/proxy/config", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            ProxyConfig cfg = btModule.proxyLoadConfig();
            JsonDocument doc;
            doc["watchAddress"] = cfg.watchAddress;
            doc["spoofName"]    = cfg.spoofName;
            doc["forwardHR"]    = cfg.forwardHR;
            doc["forwardBatt"]  = cfg.forwardBatt;
            doc["forwardTemp"]  = cfg.forwardTemp;
            doc["autoStart"]    = cfg.autoStart;
            String j; serializeJson(doc, j);
            _bleSendJson(req, 200, j);
        });

    // POST /api/ble/proxy/config
    BLE_POST("/api/ble/proxy/config",
        ([this](AsyncWebServerRequest* req, const String& body) {
            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                _bleSendJson(req, 400, "{\"error\":\"Bad JSON\"}"); return;
            }
            ProxyConfig cfg = btModule.proxyLoadConfig();
            if (doc["watchAddress"].is<const char*>())
                cfg.watchAddress = doc["watchAddress"].as<String>();
            if (doc["spoofName"].is<const char*>())
                cfg.spoofName    = doc["spoofName"].as<String>();
            if (doc["forwardHR"].is<bool>())
                cfg.forwardHR    = doc["forwardHR"];
            if (doc["forwardBatt"].is<bool>())
                cfg.forwardBatt  = doc["forwardBatt"];
            if (doc["forwardTemp"].is<bool>())
                cfg.forwardTemp  = doc["forwardTemp"];
            if (doc["autoStart"].is<bool>())
                cfg.autoStart    = doc["autoStart"];
            btModule.proxySaveConfig(cfg);
            _bleSendJson(req, 200, "{\"ok\":true}");
        }));

    // POST /api/ble/proxy/start
    _server.on("/api/ble/proxy/start", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            ProxyConfig cfg = btModule.proxyLoadConfig();
            if (cfg.watchAddress.isEmpty()) {
                _bleSendJson(req, 400,
                    "{\"ok\":false,\"error\":\"No watch address saved — POST /api/ble/proxy/config first\"}");
                return;
            }
            bool ok = btModule.proxyStart(cfg);
            if (ok)
                _bleSendJson(req, 200,
                    "{\"ok\":true,\"message\":\"Proxy starting — watch will drop phone connection\"}");
            else
                _bleSendJson(req, 503,
                    "{\"ok\":false,\"error\":\"Watch connection failed — is it nearby and powered?\"}");
        });

    // POST /api/ble/proxy/stop
    _server.on("/api/ble/proxy/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            btModule.proxyStop();
            _bleSendJson(req, 200, "{\"ok\":true,\"message\":\"Proxy stopped\"}");
        });
}
