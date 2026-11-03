#pragma once

#if defined(WITH_ESPNOW_BRIDGE)
#define MESH_ESPNOW_OBSERVER_SETTINGS 1
#include "IdentityStore.h"
#include "ObserverTransport.h"
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

// observer.transport (espnow|udp|both, default espnow): where the feed goes.
// Stored in its own record (/observer_transport); missing or malformed means
// espnow, so an upgraded repeater behaves exactly as before.
mesh::observer::Transport observerTransport();
bool saveObserverTransport(mesh::observer::Transport transport);

// What the CLI needs from the running repeater for observer.transport.
struct ObserverRuntime {
  bool udp_available = false;    // the UDP bridge is compiled in
  uint8_t espnow_channel = 0;    // ESP-NOW bridge channel
  uint8_t ap_channel = 0;        // joined access point's channel, 0 = not joined
  // One line after "> <transport>": per-transport state (may be null).
  void (*describe)(void* ctx, char* out, size_t capacity) = nullptr;
  void* ctx = nullptr;
};

// get/set espnow.observer on|off, its transport-neutral alias
// get/set observer on|off, and get/set observer.transport. Returns false for
// any other command, leaving `reply` untouched. `runtime` may be null (then
// udp/both are refused as unavailable).
bool handleEspNowObserverCommand(const char* command, char* reply, size_t capacity,
                                 const ObserverRuntime* runtime = nullptr);

}} // namespace mesh::espnow
#endif
