// ============================================================
//  task_manager.cpp  -  FreeRTOS Task Architecture v1.1
//
//  hw_poll (Core 1, priority 2):
//    Polls hardware modules on SPI/I2C bus:
//      - NFC (I2C)
//      - RFID (SPI)
//      - SubGHz (SPI)
//      - NRF24 (SPI)
//    Removes 4 SPI polling calls from loop().
//    Runs at 20ms tick - matches hardware polling needs.
// ============================================================
#include "task_manager.h"
#include "nfc_module.h"
#include "rfid_module.h"
#include "subghz_module.h"
#include "nrf24_module.h"

TaskManager taskMgr;

// ── begin ─────────────────────────────────────────────────────
void TaskManager::begin() {
    xTaskCreatePinnedToCore(
        _hwPollTask, "hw_poll",
        TASK_HW_POLL_STACK, nullptr,
        TASK_HW_POLL_PRIO, nullptr,
        1   // Core 1 - SPI bus access alongside loop()
    );

    Serial.println("[TASK] hw_poll task -> Core 1");
}

// ── _hwPollTask ───────────────────────────────────────────────
// Runs on Core 1, priority 2 (below ir_tx=5, above loop=1).
// Polls hardware modules at 20ms intervals.
void TaskManager::_hwPollTask(void* param) {
    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(20));
        nfcModule.loop();
        rfidModule.loop();
        subGhzModule.loop();
        nrf24Module.loop();
    }
}
