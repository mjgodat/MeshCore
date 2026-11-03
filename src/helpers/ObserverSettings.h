#pragma once

#if defined(WITH_ESPNOW_BRIDGE)
#define MESH_ESPNOW_OBSERVER_SETTINGS 1
#include "IdentityStore.h"
#include <stddef.h>
#include <stdint.h>

namespace mesh { namespace espnow {

// espnow.observer: opt-in repeater -> visual head feed (per-packet LoRa
// records and periodic vitals) over the ESP-NOW bridge. Stored apart from
// NodePrefs, like espnow.peer and espnow.repeat. Missing, unreadable or
// malformed records all mean off.
void loadEspNowObserverSettings(FILESYSTEM* fs);
bool espNowObserverSettingsHealthy();
bool espNowObserverEnabled();
bool saveEspNowObserver(bool enabled);

// get/set espnow.observer on|off. Returns false for any other command,
// leaving `reply` untouched.
bool handleEspNowObserverCommand(const char* command, char* reply, size_t capacity);

}} // namespace mesh::espnow
#endif
