#pragma once

// bridge.announce on|off and bridge.rxlimit <pkts/min> on repeaters with the
// ESP-NOW and/or UDP bridge (helpers/bridges/DiscoveryFrame.h has the frames,
// the auto-paired head table and the rate limiter).
//
// Saved in /bridge_discovery apart from NodePrefs (like espnow.repeat). A
// missing record means announce on and rxlimit 120/min; a damaged record
// falls back to the same defaults and is reported until saved again.

#if defined(WITH_ESPNOW_BRIDGE) || defined(WITH_UDP_BRIDGE)
#define MESH_BRIDGE_DISCOVERY_SETTINGS 1
#include "IdentityStore.h"
#include <stddef.h>
#include <stdint.h>

namespace mesh { namespace discovery {

void loadBridgeDiscoverySettings(FILESYSTEM* fs);
bool bridgeDiscoverySettingsHealthy();
bool bridgeAnnounceEnabled();
uint16_t bridgeRxLimit();   // packets per minute, 0 = no limit

// What the CLI needs from the running repeater.
struct DiscoveryRuntime {
  // One line describing the auto-paired heads (may be null).
  void (*describe_peers)(void* ctx, char* out, size_t capacity) = nullptr;
  void* ctx = nullptr;
};

// get/set bridge.announce, get/set bridge.rxlimit, get bridge.autopeers.
// Returns false for any other command, leaving `reply` untouched.
bool handleBridgeDiscoveryCommand(const char* command, char* reply, size_t capacity,
                                  const DiscoveryRuntime* runtime = nullptr);

}}  // namespace mesh::discovery
#endif
