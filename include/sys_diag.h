#pragma once

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_log.h"

struct SysDiagSnapshot {
    size_t sramFree;
    size_t sramLargest;
    size_t sramMin;
    uint8_t sramFragPct;
    size_t psramFree;
    size_t psramLargest;
    size_t psramMin;
    uint8_t psramFragPct;
    uint32_t stackHighWaterBytes;
    bool heapIntegrityOk;
};

namespace SysDiag {

/**
 * @brief Captures heap fragmentation, minimum high-water marks, stack headroom, and optional canary integrity.
 */
inline SysDiagSnapshot capture(bool checkIntegrity = false) {
    multi_heap_info_t sram = {};
    multi_heap_info_t psram = {};
    heap_caps_get_info(&sram, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    SysDiagSnapshot snap = {};
    snap.sramFree = sram.total_free_bytes;
    snap.sramLargest = sram.largest_free_block;
    snap.sramMin = sram.minimum_free_bytes;
    snap.sramFragPct = (sram.total_free_bytes > 0)
                           ? static_cast<uint8_t>(100U - (static_cast<uint64_t>(sram.largest_free_block) * 100ULL) /
                                                             sram.total_free_bytes)
                           : 0U;

    snap.psramFree = psram.total_free_bytes;
    snap.psramLargest = psram.largest_free_block;
    snap.psramMin = psram.minimum_free_bytes;
    snap.psramFragPct = (psram.total_free_bytes > 0)
                            ? static_cast<uint8_t>(100U - (static_cast<uint64_t>(psram.largest_free_block) * 100ULL) /
                                                              psram.total_free_bytes)
                            : 0U;

    snap.stackHighWaterBytes = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) * sizeof(StackType_t);
    snap.heapIntegrityOk = checkIntegrity ? heap_caps_check_integrity_all(false) : true;
    return snap;
}

/**
 * @brief Logs formatted internal SRAM / PSRAM fragmentation and FreeRTOS stack telemetry.
 */
inline void logReport(const char* tag, const char* phase, float fps = -1.0f, bool checkIntegrity = false) {
    SysDiagSnapshot s = capture(checkIntegrity);
    if (fps >= 0.0f) {
        ESP_LOGI(
            tag,
            "[%s] FPS:%.1f | SRAM:%uB (MaxBlk:%uB Frag:%u%% Min:%uB) | PSRAM:%uKB (MaxBlk:%uKB Frag:%u%% Min:%uKB) | "
            "StkMin:%uB | HeapOK:%d",
            phase, fps, (unsigned)s.sramFree, (unsigned)s.sramLargest, (unsigned)s.sramFragPct, (unsigned)s.sramMin,
            (unsigned)(s.psramFree / 1024U), (unsigned)(s.psramLargest / 1024U), (unsigned)s.psramFragPct,
            (unsigned)(s.psramMin / 1024U), (unsigned)s.stackHighWaterBytes, s.heapIntegrityOk ? 1 : 0);
    } else {
        ESP_LOGI(tag,
                 "[%s] SRAM:%uB (MaxBlk:%uB Frag:%u%% Min:%uB) | PSRAM:%uKB (MaxBlk:%uKB Frag:%u%% Min:%uKB) | "
                 "StkMin:%uB | HeapOK:%d",
                 phase, (unsigned)s.sramFree, (unsigned)s.sramLargest, (unsigned)s.sramFragPct, (unsigned)s.sramMin,
                 (unsigned)(s.psramFree / 1024U), (unsigned)(s.psramLargest / 1024U), (unsigned)s.psramFragPct,
                 (unsigned)(s.psramMin / 1024U), (unsigned)s.stackHighWaterBytes, s.heapIntegrityOk ? 1 : 0);
    }
}

}  // namespace SysDiag
