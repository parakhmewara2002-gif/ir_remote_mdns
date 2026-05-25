#pragma once
// ============================================================
//  task_manager.h  -  Centralized FreeRTOS Task Architecture
//
//  Core 1 (Realtime/Loop):
//    Arduino loop() (priority 1)
//    ir_tx     (already created by IRTransmitter, priority 5)
//    hw_poll   (NFC + RFID + SubGHz + NRF24, priority 2)
// ============================================================
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ── Task stack sizes ──────────────────────────────────────────
// hw_poll stack: NFC MIFARE reads need 6144 when PN532 is connected.
// When NFC hardware is absent (not detected), loop() returns immediately —
// 3072 is sufficient. If NFC is later connected, raise back to 6144.
#define TASK_HW_POLL_STACK   3072    // was 6144 — safe when NFC absent

// ── Task priorities ───────────────────────────────────────────
#define TASK_HW_POLL_PRIO    2

class TaskManager {
public:
    // Call once from setup() after all modules are initialized
    static void begin();

private:
    static void _hwPollTask(void* param);
};

extern TaskManager taskMgr;
