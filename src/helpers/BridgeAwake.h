#pragma once

// The ESP-NOW bridge's awake rule and the station owner's remembered
// modem-sleep preference. Shared by ESPNowBridge, WiFiStationPower.h, the
// esp_wifi_set_ps wrapper (esp32/EspNowAwakeWrap.cpp) and their host tests.
// Pure, C++11, no Arduino.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mesh { namespace wifi {

// wifi_ps_type_t values: WIFI_PS_NONE = 0, MIN_MODEM = 1, MAX_MODEM = 2.
// While the bridge runs every request becomes "no modem sleep".
inline uint8_t powerSaveWithBridge(uint8_t requested, bool bridge_running) {
  return bridge_running ? 0 : requested;
}

// What the station's owner (MQTT, WebConfig, the UDP station, a CLI
// setting) last asked for, so a bridge that forced the radio awake on a
// shared station can hand that preference back when it stops.
struct BridgeSleepMemory {
  int8_t owner_ps = -1;   // -1: no owner preference known
  bool shared = false;    // the running bridge started on a shared station
};
inline BridgeSleepMemory& bridgeSleepMemory() {
  static BridgeSleepMemory memory;
  return memory;
}
inline void noteOwnerPowerSave(uint8_t requested) {
  bridgeSleepMemory().owner_ps = static_cast<int8_t>(requested);
}
// A bridge starts on a station someone else runs: remember the owner's
// current preference (Arduino's cache); requests made while the bridge runs
// (noteOwnerPowerSave) replace it.
inline void bridgeStartsShared(uint8_t cached_preference) {
  BridgeSleepMemory& m = bridgeSleepMemory();
  m.shared = true;
  m.owner_ps = static_cast<int8_t>(cached_preference);
}
inline void bridgeStartsDedicated() { bridgeSleepMemory().shared = false; }
// The bridge stops: the preference to restore, or -1 (dedicated start, or
// nothing known, or the owner wanted no sleep anyway).
inline int bridgeStopsShared() {
  BridgeSleepMemory& m = bridgeSleepMemory();
  const bool shared = m.shared;
  m.shared = false;
  return shared && m.owner_ps > 0 ? m.owner_ps : -1;
}

// Back-off for a refused "stay awake" (e.g. Bluetooth requires modem sleep):
// 500 ms, doubling, capped at 10 s; 0 after a success.
static const uint32_t kAwakeRetryFirstMs = 500;
static const uint32_t kAwakeRetryMaxMs = 10000;
inline uint32_t nextAwakeBackoff(uint32_t previous_ms) {
  if (previous_ms == 0) return kAwakeRetryFirstMs;
  return previous_ms >= kAwakeRetryMaxMs / 2 ? kAwakeRetryMaxMs : previous_ms * 2;
}

// `wifi.powersave` replies while the ESP-NOW bridge runs: the saved setting
// differs from what the radio does.
inline void annotatePowerSaveReply(char* reply, size_t capacity, bool bridge_running, bool is_set) {
  if (!reply || !capacity || !bridge_running) return;
  if (is_set) {
    if (strncmp(reply, "OK", 2) != 0) return;
    snprintf(reply, capacity, "OK - saved (radio kept awake while the ESP-NOW bridge runs)");
    return;
  }
  if (reply[0] != '>' || strstr(reply, "none")) return;
  const size_t used = strlen(reply);
  if (used + 1 < capacity)
    snprintf(reply + used, capacity - used, " (saved; effective none while the ESP-NOW bridge runs)");
}

}}  // namespace mesh::wifi
