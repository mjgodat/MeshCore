#pragma once

// The ESP-NOW bridge's awake rule, shared by the esp_wifi_set_ps wrapper
// (esp32/EspNowAwakeWrap.cpp) and its host tests. Pure, C++11.

#include <stdint.h>

namespace mesh { namespace wifi {

// wifi_ps_type_t values: WIFI_PS_NONE = 0, MIN_MODEM = 1, MAX_MODEM = 2.
// While the bridge runs every request becomes "no modem sleep".
inline uint8_t powerSaveWithBridge(uint8_t requested, bool bridge_running) {
  return bridge_running ? 0 : requested;
}

}}  // namespace mesh::wifi
