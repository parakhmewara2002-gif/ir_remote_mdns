// ============================================================
//  rule_manager.cpp  -  Batch 2: IF-THEN Automation Engine
// ============================================================
#include "rule_manager.h"
#include "audit_manager.h"
#include "scheduler.h"
#include "sd_manager.h"
#include <ctime>

#define SD_DIR_RULES_PRESETS "/rules"

RuleManager ruleMgr;

// ─────────────────────────────────────────────────────────────
RuleManager::RuleManager() : _nextId(1) {}

// ─────────────────────────────────────────────────────────────
void RuleManager::begin() {
    if (!LittleFS.exists(RULES_DIR)) {
        LittleFS.mkdir(RULES_DIR);
    }
    // FIX: pre-allocate cache vector to avoid reallocs as rules are loaded
    _ruleCache.reserve(RULES_MAX);
    // Find max existing ID to set _nextId
    {
        File dir = LittleFS.open(RULES_DIR);
        if (dir && dir.isDirectory()) {
            File f = dir.openNextFile();
            while (f) {
                if (!f.isDirectory()) {
                    String name = String(f.name());
                    name.replace(".json", "");
                    uint32_t id = name.toInt();
                    if (id >= _nextId) _nextId = id + 1;
                }
                f.close();
                f = dir.openNextFile();
            }
        }
        if (dir) dir.close();
    }
    Serial.printf(RULE_TAG " Started - %u rules loaded, nextId=%u\n",
                  (unsigned)ruleCount(), _nextId);
}

// ─────────────────────────────────────────────────────────────
void RuleManager::loop() {
    if (_pending.empty()) return;
    unsigned long now = millis();
    for (auto it = _pending.begin(); it != _pending.end(); ) {
        if (now >= it->fireAt) {
            _executeAction(it->step, it->ruleId);
            it = _pending.erase(it);
        } else {
            ++it;
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  Trigger events
// ─────────────────────────────────────────────────────────────
void RuleManager::triggerRfidScan(const String& uid,
                                   const String& cardName,
                                   bool known) {
    if (!known) {
        _fireTrigger(RuleTrigger::RFID_UNKNOWN, uid);
    }
    // FIX: was calling listRules() here independently - caused two full
    // LittleFS scans per RFID event. Now uses shared cache via _getCachedRules().
    const auto& rules = _getCachedRules();
    for (const auto& r : rules) {
        if (!r.enabled) continue;
        if (r.trigger != RuleTrigger::RFID_SCAN) continue;
        if (r.triggerParam.isEmpty()
            || r.triggerParam.equalsIgnoreCase(uid)
            || r.triggerParam.equalsIgnoreCase(cardName)) {
            RuleEntry mutable_r = r;   // copy for _executeRule (updates firedCount)
            _executeRule(mutable_r);
        }
    }
}

void RuleManager::triggerNfcScan(const String& uid, const String& tagName) {
    _fireTrigger(RuleTrigger::NFC_SCAN, uid);
}

void RuleManager::triggerIrReceived(uint32_t buttonId, const String& protocol) {
    _fireTrigger(RuleTrigger::IR_RECEIVED, String(buttonId));
}

void RuleManager::triggerWifiConnect(const String& ssid) {
    _fireTrigger(RuleTrigger::WIFI_CONNECT, ssid);
}

void RuleManager::triggerWifiDisconnect() {
    _fireTrigger(RuleTrigger::WIFI_DISCONNECT, "");
}

void RuleManager::triggerBoot() {
    _fireTrigger(RuleTrigger::BOOT, "");
}

void RuleManager::triggerManual(uint32_t ruleId) {
    RuleEntry rule;
    if (loadRule(ruleId, rule) && rule.enabled) {
        _executeRule(rule);
    }
}

void RuleManager::triggerAcDetected() {
    _fireTrigger(RuleTrigger::AC_DETECTED, "");
}

void RuleManager::triggerAcLost() {
    _fireTrigger(RuleTrigger::AC_LOST, "");
}

// ─────────────────────────────────────────────────────────────
//  Internal fire - matches trigger type + param
// ─────────────────────────────────────────────────────────────
void RuleManager::_fireTrigger(RuleTrigger trigger, const String& param) {
    // FIX: use cached rules - no LittleFS scan per trigger event
    const auto& rules = _getCachedRules();
    for (const auto& r : rules) {
        if (!r.enabled) continue;
        if (r.trigger != trigger) continue;
        if (!r.triggerParam.isEmpty() &&
            !r.triggerParam.equalsIgnoreCase(param)) continue;
        RuleEntry mutable_r = r;   // copy for _executeRule (writes firedCount)
        _executeRule(mutable_r);
    }
}

// ─────────────────────────────────────────────────────────────
//  Execute all actions of a rule (with delays)
// ─────────────────────────────────────────────────────────────
void RuleManager::_executeRule(RuleEntry& rule) {
    Serial.printf(RULE_TAG " Firing rule #%u: %s\n", rule.id, rule.name.c_str());

    // Update stats
    rule.firedCount++;
    rule.lastFiredAt = _buildTimeStr();
    _saveRule(rule);

    // Audit
    auditMgr.log(AuditSource::RULE, "RULE_FIRED",
                 String("Rule: ") + rule.name + " (id=" + rule.id + ")");

    unsigned long offset = 0;
    for (const auto& step : rule.actions) {
        offset += step.delayMs;
        if (offset == 0) {
            // Execute immediately
            _executeAction(step, rule.id);
        } else {
            // Schedule deferred
            DeferredAction da;
            da.step   = step;
            da.fireAt = millis() + offset;
            da.ruleId = rule.id;
            // FIX: cap pending queue to avoid heap exhaustion under rapid fire
            if (_pending.size() >= MAX_PENDING_ACTIONS) {
                Serial.printf(RULE_TAG " WARNING: pending queue full (%u), dropping oldest\n",
                              (unsigned)_pending.size());
                _pending.erase(_pending.begin());
            }
            _pending.push_back(da);
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  Execute a single action
// ─────────────────────────────────────────────────────────────
void RuleManager::_executeAction(const RuleActionStep& step, uint32_t ruleId) {
    Serial.printf(RULE_TAG " Action: %s param1=%s\n",
                  _actionToStr(step.action).c_str(), step.param1.c_str());

    switch (step.action) {
        case RuleAction::IR_TRANSMIT:
            if (_irCb) _irCb(step.param1.toInt());
            break;

        case RuleAction::MACRO_RUN:
            if (_macroCb) _macroCb(step.param1);
            break;

        case RuleAction::NOTIFY:
            if (_notifyCb) _notifyCb(step.param1, true, true);
            break;

        case RuleAction::BUZZER:
            if (_buzzerCb) {
                uint8_t times = step.param1.toInt();
                if (times < 1) times = 1;
                if (times > 5) times = 5;
                _buzzerCb(times);
            }
            break;

        case RuleAction::LOG:
            auditMgr.log(AuditSource::RULE, "RULE_LOG",
                         String("Rule #") + ruleId + ": " + step.param1);
            break;

        // Feature 11: run a macro from SD card
        case RuleAction::SD_MACRO:
            if (sdMgr.isAvailable()) {
                sdMgr.log(String("Rule #") + ruleId + " queuing SD macro: " + step.param1,
                          SdLogLevel::INFO, "RULE");
                sdMgr.queueMacro(step.param1);
            } else {
                Serial.printf(RULE_TAG " SD_MACRO: SD not available for macro '%s'\n",
                              step.param1.c_str());
            }
            break;

        // Feature 12: write a log entry to SD
        case RuleAction::SD_LOG:
            if (sdMgr.isAvailable()) {
                sdMgr.log(step.param1, SdLogLevel::INFO, "RULE");
            } else {
                Serial.printf(RULE_TAG " SD_LOG: SD not available - msg: %s\n",
                              step.param1.c_str());
            }
            break;

        default:
            break;
    }
}

// ─────────────────────────────────────────────────────────────
//  CRUD
// ─────────────────────────────────────────────────────────────
uint32_t RuleManager::addRule(RuleEntry& entry) {
    if (ruleCount() >= RULES_MAX) {
        Serial.println(RULE_TAG " Max rules reached");
        return 0;
    }
    entry.id = _nextId++;
    _saveRule(entry);
    _cacheValid = false;   // FIX: invalidate cache after write
    Serial.printf(RULE_TAG " Added rule #%u: %s\n", entry.id, entry.name.c_str());
    return entry.id;
}

bool RuleManager::updateRule(const RuleEntry& entry) {
    if (!LittleFS.exists(_rulePath(entry.id))) return false;
    _saveRule(entry);
    _cacheValid = false;   // FIX: invalidate cache after write
    return true;
}

bool RuleManager::deleteRule(uint32_t id) {
    String path = _rulePath(id);
    if (!LittleFS.exists(path)) return false;
    LittleFS.remove(path);
    _cacheValid = false;   // FIX: invalidate cache after write
    Serial.printf(RULE_TAG " Deleted rule #%u\n", id);
    return true;
}

void RuleManager::_deleteRuleFile(uint32_t id) const {
    String path = _rulePath(id);
    if (LittleFS.exists(path)) {
        LittleFS.remove(path);
        Serial.printf(RULE_TAG " _deleteRuleFile: removed %s\n", path.c_str());
    }
}

bool RuleManager::setEnabled(uint32_t id, bool en) {
    RuleEntry rule;
    if (!_loadRule(id, rule)) return false;
    rule.enabled = en;
    _saveRule(rule);
    _cacheValid = false;   // FIX: invalidate cache after write
    return true;
}

// ─────────────────────────────────────────────────────────────
//  List / Load - with RAM cache
// ─────────────────────────────────────────────────────────────
// FIX: listRules() previously performed a full LittleFS directory scan +
// JSON deserialization on EVERY call. Since triggerIrReceived() and
// triggerRfidScan() call listRules() on every IR/RFID event (which can
// fire at 20+ Hz during capture), this caused:
//   - Continuous LittleFS reads (flash wear + 5-20ms latency per trigger)
//   - Repeated heap allocation for std::vector<RuleEntry>
//   - Measurable loop() stalls under concurrent IR + RFID activity
//
// Fix: load once into _ruleCache on first call or after any mutation.
// _cacheValid is set false by add/update/delete/setEnabled.
const std::vector<RuleEntry>& RuleManager::_getCachedRules() const {
    if (_cacheValid) return _ruleCache;

    _ruleCache.clear();
    _ruleCache.reserve(RULES_MAX);

    File dir = LittleFS.open(RULES_DIR);
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                String name = String(f.name());
                if (name.endsWith(".json")) {
                    name.replace(".json", "");
                    uint32_t id = name.toInt();
                    RuleEntry rule;
                    if (_loadRule(id, rule)) _ruleCache.push_back(std::move(rule));
                }
            }
            f.close();
            f = dir.openNextFile();
        }
        dir.close();
    }
    _cacheValid = true;
    Serial.printf(RULE_TAG " Cache rebuilt: %u rules\n", (unsigned)_ruleCache.size());
    return _ruleCache;
}

std::vector<RuleEntry> RuleManager::listRules() const {
    return _getCachedRules();   // returns copy - callers may modify entries safely
}

bool RuleManager::loadRule(uint32_t id, RuleEntry& out) const {
    return _loadRule(id, out);
}

size_t RuleManager::ruleCount() const {
    return _getCachedRules().size();
}

String RuleManager::allRulesToJson() const {
    const auto& rules = _getCachedRules();
    String out = "{\"count\":";
    out += rules.size();
    out += ",\"rules\":[";
    for (size_t i = 0; i < rules.size(); ++i) {
        if (i) out += ",";
        out += rules[i].toJsonString();
    }
    out += "]}";
    return out;
}

// ─────────────────────────────────────────────────────────────
//  Persistence helpers
// ─────────────────────────────────────────────────────────────
void RuleManager::_saveRule(const RuleEntry& rule) const {
    File f = LittleFS.open(_rulePath(rule.id), "w");
    if (!f) return;
    f.print(rule.toJsonString());
    f.close();
}

bool RuleManager::_loadRule(uint32_t id, RuleEntry& out) const {
    String path = _rulePath(id);
    if (!LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    JsonDocument doc;
    bool ok = (deserializeJson(doc, f) == DeserializationError::Ok);
    f.close();
    if (!ok) return false;
    return out.fromJson(doc.as<JsonObjectConst>());
}

String RuleManager::_rulePath(uint32_t id) const {
    return String(RULES_DIR) + "/" + id + ".json";
}

// ─────────────────────────────────────────────────────────────
//  RuleEntry JSON serialisation
// ─────────────────────────────────────────────────────────────
void RuleEntry::toJson(JsonObject obj) const {
    obj["id"]           = id;
    obj["name"]         = name;
    obj["enabled"]      = enabled;
    obj["triggerParam"] = triggerParam;
    obj["firedCount"]   = firedCount;
    obj["lastFiredAt"]  = lastFiredAt;
    // trigger string
    const char* tstr = "MANUAL";
    switch (trigger) {
        case RuleTrigger::RFID_SCAN:       tstr = "RFID_SCAN";       break;
        case RuleTrigger::RFID_UNKNOWN:    tstr = "RFID_UNKNOWN";    break;
        case RuleTrigger::NFC_SCAN:        tstr = "NFC_SCAN";        break;
        case RuleTrigger::IR_RECEIVED:     tstr = "IR_RECEIVED";     break;
        case RuleTrigger::WIFI_CONNECT:    tstr = "WIFI_CONNECT";    break;
        case RuleTrigger::WIFI_DISCONNECT: tstr = "WIFI_DISCONNECT"; break;
        case RuleTrigger::BOOT:            tstr = "BOOT";            break;
        case RuleTrigger::AC_DETECTED:     tstr = "AC_DETECTED";     break;
        case RuleTrigger::AC_LOST:         tstr = "AC_LOST";         break;
        default:                           tstr = "MANUAL";          break;
    }
    obj["trigger"] = tstr;
    JsonArray acts = obj["actions"].to<JsonArray>();
    for (const auto& s : actions) {
        JsonObject a = acts.add<JsonObject>();
        a["delayMs"] = s.delayMs;
        a["param1"]  = s.param1;
        a["param2"]  = s.param2;
        const char* astr = "LOG";
        switch (s.action) {
            case RuleAction::IR_TRANSMIT:     astr = "IR_TRANSMIT";     break;
            case RuleAction::MACRO_RUN:       astr = "MACRO_RUN";       break;
            case RuleAction::NOTIFY:          astr = "NOTIFY";          break;
            case RuleAction::BUZZER:          astr = "BUZZER";          break;
            case RuleAction::SD_MACRO:        astr = "SD_MACRO";        break;
            case RuleAction::SD_LOG:          astr = "SD_LOG";          break;
            default:                          astr = "LOG";             break;
        }
        a["action"] = astr;
    }
}

String RuleEntry::toJsonString() const {
    // Build manually to avoid circular dependency with ruleMgr methods
    String out = "{";
    out += "\"id\":" + String(id) + ",";
    out += "\"name\":\"" + name + "\",";
    out += "\"enabled\":" + String(enabled ? "true" : "false") + ",";
    // Trigger
    const char* tstr = "MANUAL";
    switch (trigger) {
        case RuleTrigger::RFID_SCAN:       tstr = "RFID_SCAN";       break;
        case RuleTrigger::RFID_UNKNOWN:    tstr = "RFID_UNKNOWN";    break;
        case RuleTrigger::NFC_SCAN:        tstr = "NFC_SCAN";        break;
        case RuleTrigger::IR_RECEIVED:     tstr = "IR_RECEIVED";     break;
        case RuleTrigger::WIFI_CONNECT:    tstr = "WIFI_CONNECT";    break;
        case RuleTrigger::WIFI_DISCONNECT: tstr = "WIFI_DISCONNECT"; break;
        case RuleTrigger::BOOT:            tstr = "BOOT";            break;
        case RuleTrigger::AC_DETECTED:     tstr = "AC_DETECTED";     break;
        case RuleTrigger::AC_LOST:         tstr = "AC_LOST";         break;
        case RuleTrigger::MANUAL:          tstr = "MANUAL";          break;
        default:                           tstr = "MANUAL";          break;
    }
    out += "\"trigger\":\"" + String(tstr) + "\",";
    out += "\"triggerParam\":\"" + triggerParam + "\",";
    out += "\"firedCount\":" + String(firedCount) + ",";
    out += "\"lastFiredAt\":\"" + lastFiredAt + "\",";
    out += "\"actions\":[";
    for (size_t i = 0; i < actions.size(); i++) {
        if (i) out += ",";
        const char* astr = "LOG";
        switch (actions[i].action) {
            case RuleAction::IR_TRANSMIT:     astr = "IR_TRANSMIT";     break;
            case RuleAction::MACRO_RUN:       astr = "MACRO_RUN";       break;
            case RuleAction::NOTIFY:          astr = "NOTIFY";          break;
            case RuleAction::BUZZER:          astr = "BUZZER";          break;
            case RuleAction::SD_MACRO:        astr = "SD_MACRO";        break;
            case RuleAction::SD_LOG:          astr = "SD_LOG";          break;
            case RuleAction::LOG:             astr = "LOG";             break;
            default:                          astr = "LOG";             break;
        }
        String p1 = actions[i].param1; p1.replace("\"","\\\"");
        String p2 = actions[i].param2; p2.replace("\"","\\\"");
        out += "{\"action\":\"" + String(astr) + "\","
             + "\"param1\":\"" + p1 + "\","
             + "\"param2\":\"" + p2 + "\","
             + "\"delayMs\":" + String(actions[i].delayMs) + "}";
    }
    out += "]}";
    return out;
}

bool RuleEntry::fromJson(JsonObjectConst obj) {
    id           = obj["id"]           | (uint32_t)0;
    name         = obj["name"]         | (const char*)"";
    enabled      = obj["enabled"]      | true;
    triggerParam = obj["triggerParam"] | (const char*)"";
    firedCount   = obj["firedCount"]   | (uint32_t)0;
    lastFiredAt  = obj["lastFiredAt"]  | (const char*)"";

    // Parse trigger
    String ts = obj["trigger"] | (const char*)"MANUAL";
    ts.toUpperCase();
    if      (ts == "RFID_SCAN")       trigger = RuleTrigger::RFID_SCAN;
    else if (ts == "RFID_UNKNOWN")    trigger = RuleTrigger::RFID_UNKNOWN;
    else if (ts == "NFC_SCAN")        trigger = RuleTrigger::NFC_SCAN;
    else if (ts == "IR_RECEIVED")     trigger = RuleTrigger::IR_RECEIVED;
    else if (ts == "WIFI_CONNECT")    trigger = RuleTrigger::WIFI_CONNECT;
    else if (ts == "WIFI_DISCONNECT") trigger = RuleTrigger::WIFI_DISCONNECT;
    else if (ts == "BOOT")            trigger = RuleTrigger::BOOT;
    else if (ts == "AC_DETECTED")     trigger = RuleTrigger::AC_DETECTED;
    else if (ts == "AC_LOST")         trigger = RuleTrigger::AC_LOST;
    else                              trigger = RuleTrigger::MANUAL;

    // Parse actions
    actions.clear();
    for (JsonObjectConst a : obj["actions"].as<JsonArrayConst>()) {
        RuleActionStep step;
        step.delayMs = a["delayMs"] | (uint32_t)0;
        step.param1  = a["param1"] | (const char*)"";
        step.param2  = a["param2"] | (const char*)"";
        String as    = a["action"] | (const char*)"LOG";
        as.toUpperCase();
        if      (as == "IR_TRANSMIT")     step.action = RuleAction::IR_TRANSMIT;
        else if (as == "MACRO_RUN")       step.action = RuleAction::MACRO_RUN;
        else if (as == "NOTIFY")          step.action = RuleAction::NOTIFY;
        else if (as == "BUZZER")          step.action = RuleAction::BUZZER;
        else if (as == "SD_MACRO")        step.action = RuleAction::SD_MACRO;
        else if (as == "SD_LOG")          step.action = RuleAction::SD_LOG;
        else                              step.action = RuleAction::LOG;
        actions.push_back(step);
    }
    return id > 0 && !name.isEmpty();
}

// ─────────────────────────────────────────────────────────────
//  String conversion helpers
// ─────────────────────────────────────────────────────────────
String RuleManager::_triggerToStr(RuleTrigger t) const {
    switch (t) {
        case RuleTrigger::RFID_SCAN:       return "RFID_SCAN";
        case RuleTrigger::RFID_UNKNOWN:    return "RFID_UNKNOWN";
        case RuleTrigger::NFC_SCAN:        return "NFC_SCAN";
        case RuleTrigger::IR_RECEIVED:     return "IR_RECEIVED";
        case RuleTrigger::WIFI_CONNECT:    return "WIFI_CONNECT";
        case RuleTrigger::WIFI_DISCONNECT: return "WIFI_DISCONNECT";
        case RuleTrigger::BOOT:            return "BOOT";
        case RuleTrigger::AC_DETECTED:     return "AC_DETECTED";
        case RuleTrigger::AC_LOST:         return "AC_LOST";
        case RuleTrigger::MANUAL:          return "MANUAL";
        default:                           return "MANUAL";
    }
}

RuleTrigger RuleManager::_strToTrigger(const String& s) const {
    if (s == "RFID_SCAN")        return RuleTrigger::RFID_SCAN;
    if (s == "RFID_UNKNOWN")     return RuleTrigger::RFID_UNKNOWN;
    if (s == "NFC_SCAN")         return RuleTrigger::NFC_SCAN;
    if (s == "IR_RECEIVED")      return RuleTrigger::IR_RECEIVED;
    if (s == "WIFI_CONNECT")     return RuleTrigger::WIFI_CONNECT;
    if (s == "WIFI_DISCONNECT")  return RuleTrigger::WIFI_DISCONNECT;
    if (s == "BOOT")             return RuleTrigger::BOOT;
    if (s == "AC_DETECTED")      return RuleTrigger::AC_DETECTED;
    if (s == "AC_LOST")          return RuleTrigger::AC_LOST;
    return RuleTrigger::MANUAL;
}

String RuleManager::_actionToStr(RuleAction a) const {
    switch (a) {
        case RuleAction::IR_TRANSMIT:     return "IR_TRANSMIT";
        case RuleAction::MACRO_RUN:       return "MACRO_RUN";
        case RuleAction::NOTIFY:          return "NOTIFY";
        case RuleAction::BUZZER:          return "BUZZER";
        case RuleAction::SD_MACRO:        return "SD_MACRO";
        case RuleAction::SD_LOG:          return "SD_LOG";
        case RuleAction::LOG:             return "LOG";
        default:                          return "LOG";
    }
}

String RuleManager::_buildTimeStr() const {
    time_t now; time(&now);
    if (now > 1000000000UL) {
        struct tm t; localtime_r(&now, &t);
        char buf[24];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year+1900, t.tm_mon+1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
        return String(buf);
    }
    return String("uptime:") + (millis()/1000) + "s";
}

// ─────────────────────────────────────────────────────────────
//  Feature 10: exportToSD / importFromSD
// ─────────────────────────────────────────────────────────────
bool RuleManager::exportToSD(const String& tag) {
    if (!sdMgr.isAvailable()) {
        Serial.println(RULE_TAG " [exportToSD] SD not available");
        return false;
    }

    String dstDir = String(SD_DIR_BACKUPS) + "/" + tag + "/rules";
    sdMgr.makeDir(dstDir);

    // Iterate all rule files in LittleFS /rules/
    File dir = LittleFS.open(RULES_DIR);
    if (!dir || !dir.isDirectory()) {
        Serial.println(RULE_TAG " [exportToSD] Cannot open rules dir");
        return false;
    }

    int exported = 0;
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            String fname = String(f.name());
            if (fname.endsWith(".json")) {
                // Read rule JSON from LittleFS
                File rf = LittleFS.open(String(RULES_DIR) + "/" + fname, "r");
                if (rf) {
                    String content = rf.readString();
                    rf.close();
                    // Write to SD
                    String dstPath = dstDir + "/" + fname;
                    if (sdMgr.safeWriteFile(dstPath, content)) {
                        exported++;
                    } else {
                        Serial.printf(RULE_TAG " [exportToSD] Failed to write %s\n", dstPath.c_str());
                    }
                }
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();

    sdMgr.log(String("Rules exported to SD tag=") + tag + " count=" + exported,
              SdLogLevel::INFO, "RULE");
    Serial.printf(RULE_TAG " [exportToSD] Exported %d rules to tag '%s'\n", exported, tag.c_str());
    return exported > 0;
}

bool RuleManager::importFromSD(const String& tag) {
    if (!sdMgr.isAvailable()) {
        Serial.println(RULE_TAG " [importFromSD] SD not available");
        return false;
    }

    String srcDir = String(SD_DIR_BACKUPS) + "/" + tag + "/rules";
    if (!sdMgr.exists(srcDir)) {
        Serial.printf(RULE_TAG " [importFromSD] Not found: %s\n", srcDir.c_str());
        return false;
    }

    // Ensure LittleFS rules dir exists
    if (!LittleFS.exists(RULES_DIR)) LittleFS.mkdir(RULES_DIR);

    auto entries = sdMgr.listDir(srcDir);
    int imported = 0;
    for (const auto& e : entries) {
        if (e.isDir) continue;
        if (!e.name.endsWith(".json")) continue;
        String content = sdMgr.readTextFile(e.fullPath);
        if (content.isEmpty()) continue;
        String dstPath = String(RULES_DIR) + "/" + e.name;
        File dst = LittleFS.open(dstPath, "w");
        if (dst) {
            dst.print(content);
            dst.close();
            imported++;
        } else {
            Serial.printf("[Rules] open(%s,w) failed\n", dstPath.c_str());
        }
    }

    // Invalidate cache so rules are reloaded
    _cacheValid = false;

    sdMgr.log(String("Rules imported from SD tag=") + tag + " count=" + imported,
              SdLogLevel::INFO, "RULE");
    Serial.printf(RULE_TAG " [importFromSD] Imported %d rules from tag '%s'\n",
                  imported, tag.c_str());
    return imported > 0;
}

// ─────────────────────────────────────────────────────────────
//  Feature 13: saveRulePreset / loadRulePreset / listRulePresets
// ─────────────────────────────────────────────────────────────
bool RuleManager::saveRulePreset(const String& name) {
    if (!sdMgr.isAvailable()) return false;

    // Serialize all current rules to a single JSON object
    const auto& rules = _getCachedRules();
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& r : rules) {
        JsonObject obj = arr.add<JsonObject>();
        r.toJson(obj);
    }
    String json;
    serializeJson(doc, json);

    String sdDir = String(SD_DIR_RULES_PRESETS);
    sdMgr.makeDir(sdDir);
    String dstPath = sdDir + "/" + name + ".json";
    bool ok = sdMgr.safeWriteFile(dstPath, json);
    if (ok) {
        sdMgr.log(String("Rule preset saved: ") + name, SdLogLevel::INFO, "RULE");
        Serial.printf(RULE_TAG " [saveRulePreset] Saved '%s' (%u rules)\n",
                      name.c_str(), (unsigned)rules.size());
    }
    return ok;
}

bool RuleManager::loadRulePreset(const String& name) {
    if (!sdMgr.isAvailable()) return false;
    String srcPath = String(SD_DIR_RULES_PRESETS) + "/" + name + ".json";
    if (!sdMgr.exists(srcPath)) {
        Serial.printf(RULE_TAG " [loadRulePreset] Not found: %s\n", srcPath.c_str());
        return false;
    }
    String json = sdMgr.readTextFile(srcPath);
    if (json.isEmpty()) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err != DeserializationError::Ok || !doc.is<JsonArrayConst>()) {
        Serial.printf(RULE_TAG " [loadRulePreset] Parse error: %s\n", err.c_str());
        return false;
    }

    // Ensure rules dir exists
    if (!LittleFS.exists(RULES_DIR)) LittleFS.mkdir(RULES_DIR);

    int added = 0;
    for (JsonObjectConst obj : doc.as<JsonArrayConst>()) {
        RuleEntry entry;
        if (!entry.fromJson(obj)) continue;
        // Merge: skip if rule with same name already exists
        bool exists = false;
        const auto& cached = _getCachedRules();
        for (const auto& r : cached) {
            if (r.name.equalsIgnoreCase(entry.name)) { exists = true; break; }
        }
        if (!exists) {
            entry.id = 0;  // will be assigned by addRule
            if (addRule(entry) != 0) added++;
        }
    }

    sdMgr.log(String("Rule preset loaded: ") + name + " (+" + added + " rules)",
              SdLogLevel::INFO, "RULE");
    Serial.printf(RULE_TAG " [loadRulePreset] Loaded '%s' (+%d rules)\n", name.c_str(), added);
    return added > 0;
}

std::vector<String> RuleManager::listRulePresets() const {
    std::vector<String> result;
    if (!sdMgr.isAvailable()) return result;
    auto entries = sdMgr.listDir(SD_DIR_RULES_PRESETS);
    for (const auto& e : entries) {
        if (e.isDir) continue;
        String n = e.name;
        if (n.endsWith(".json")) {
            result.push_back(n.substring(0, n.length() - 5));
        }
    }
    return result;
}
