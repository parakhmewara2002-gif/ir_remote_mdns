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
// FIX: 4096 is insufficient when hw_poll runs NFC MIFARE block reads.
// _readMifareBlocks() iterates 16 blocks, each calling mifareclassic_AuthenticateBlock()
// + mifareclassic_ReadDataBlock() via Adafruit_PN532 I2C stack (~800 bytes per call).
// Stack overflow in hw_poll = silent heap corruption. Raised to 6144.
#define TASK_HW_POLL_STACK   6144    // hw_poll: NFC/RFID/SubGHz/NRF24

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
