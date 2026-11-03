#pragma once

#if defined(WITH_ESPNOW_BRIDGE) || (defined(MESH_PRIMARY_ESPNOW) && MESH_PRIMARY_ESPNOW)
#define MESH_ESPNOW_REPEAT_SETTINGS 1
#include "IdentityStore.h"
#include <helpers/esp32/EspNowRepeatScheduler.h>
#include <stddef.h>
#include <stdint.h>

namespace mesh { namespace espnow {

// Separate from NodePrefs and the ESP-NOW wire formats, like espnow.peer.
// A missing record keeps repeat=1 (today's behavior). An unreadable or
// malformed record also falls back to repeat=1 and reports an error.
void loadEspNowRepeatSettings(FILESYSTEM* fs);
bool espNowRepeatSettingsHealthy();
bool saveEspNowRepeat(uint8_t count, uint16_t gap_ms);

// get/set espnow.repeat <1-4>, get/set espnow.repeat.gap <0-200>. In builds
// without the ESP-NOW bridge (primary ESP-NOW radio roles) it also answers
// `get espnow.stats` with the radio's repeat copy count. Returns false for
// any other command, leaving `reply` untouched.
bool handleEspNowRepeatCommand(const char* command, char* reply, size_t capacity);

}} // namespace mesh::espnow
#endif
