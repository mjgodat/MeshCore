#pragma once

// Loop-task watchdog for ESP32 repeaters (MESH_LOOP_WATCHDOG_SECS).
//
// Arduino-ESP32 2.0.x does not watch loopTask: its task WDT (5 s, panic)
// watches only the idle task of core 0, and loop() runs on core 1. A loop
// stuck in a driver, a callback or a busy wait was therefore never reset.
// beginLoopWatchdog() sets the task WDT to `timeout_s` (IDF 4.4 re-applies
// the timeout to an already started WDT; this also relaxes core 0's idle
// check from 5 s to the same value) and subscribes loopTask through
// Arduino's enableLoopWDT(), whose loop wrapper feeds it once per loop().
// A hang then ends in a "Task watchdog" panic + reset, which `powerlog`
// reports (ResetHistory.h).
//
// ESP-IDF 5 (Arduino-ESP32 3.x, the pioarduino ESP32-C6 / RC32 envs) takes
// a config struct instead: the same timeout and panic flag, the idle-task
// mask the SDK was built with (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPUx),
// applied with esp_task_wdt_reconfigure() when the SDK already started the
// TWDT, else esp_task_wdt_init(). The IDF 4 call is unchanged.
//
// Work that legitimately blocks the loop for longer (a firmware download,
// an OTA apply, a filesystem format) runs inside a LoopWatchdogPause.
// Sleep is capped at half the timeout (loopWatchdogSleepCapMs).

#include <Arduino.h>
#include <esp_idf_version.h>
#include <esp_task_wdt.h>
#include <stdint.h>

#ifndef MESH_LOOP_WATCHDOG_SECS
#define MESH_LOOP_WATCHDOG_SECS 0   // off unless the build enables it
#endif

namespace mesh {
namespace esp32 {

inline bool& loopWatchdogActive() {
  static bool active = false;
  return active;
}

// Sets the task WDT timeout (seconds) with panic, starting it if needed.
inline esp_err_t applyTaskWatchdogTimeout(uint32_t timeout_s) {
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t config = {};
  config.timeout_ms = timeout_s * 1000UL;
  config.idle_core_mask = 0;
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
  config.idle_core_mask |= 1U << 0;
#endif
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
  config.idle_core_mask |= 1U << 1;
#endif
  config.trigger_panic = true;
  esp_err_t err = esp_task_wdt_reconfigure(&config);
  if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_init(&config);   // TWDT not started by the SDK
  return err;
#else
  return esp_task_wdt_init(timeout_s, true);   // IDF 4.4: updates a running TWDT
#endif
}

// From setup() (it runs on loopTask), last.
inline bool beginLoopWatchdog(uint32_t timeout_s) {
  if (timeout_s == 0 || loopWatchdogActive()) return loopWatchdogActive();
  if (applyTaskWatchdogTimeout(timeout_s) != ESP_OK) return false;
  enableLoopWDT();
  loopWatchdogActive() = esp_task_wdt_status(nullptr) == ESP_OK;
  return loopWatchdogActive();
}

inline void feedLoopWatchdog() {
  if (loopWatchdogActive()) esp_task_wdt_reset();
}

// The longest single sleep while the watchdog runs (half its timeout).
inline uint32_t loopWatchdogSleepCapMs() {
  return loopWatchdogActive() && MESH_LOOP_WATCHDOG_SECS > 0
      ? static_cast<uint32_t>(MESH_LOOP_WATCHDOG_SECS) * 500UL : UINT32_MAX;
}

// Unsubscribes the calling task for its scope when it is watched; no-op
// on other tasks or when the watchdog is off.
class LoopWatchdogPause {
public:
  LoopWatchdogPause() : _paused(esp_task_wdt_status(nullptr) == ESP_OK && esp_task_wdt_delete(nullptr) == ESP_OK) {}
  ~LoopWatchdogPause() {
    if (_paused) esp_task_wdt_add(nullptr);
  }
  LoopWatchdogPause(const LoopWatchdogPause&) = delete;
  LoopWatchdogPause& operator=(const LoopWatchdogPause&) = delete;

private:
  bool _paused;
};

}  // namespace esp32
}  // namespace mesh
