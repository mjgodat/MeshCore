#pragma once

// Repeater settings for the bridges (helpers/bridges/DiscoveryFrame.h has the
// frames, the auto-paired head table and the rate limiter):
//
//   bridge.rxlimit <0-6000>      every ESP-NOW / UDP bridge build. Packets per
//                                minute admitted from the bridges; default 0
//                                (no limit), so an upgrade changes nothing.
//   bridge.announce on|off|auto  UDP bridge builds only (MESH_BRIDGE_DISCOVERY).
//                                auto (default): announce only while the UDP
//                                bridge runs (pairing needs it).
//   udp.secret <1-15>|off        UDP bridge builds only. Keys the UDP sessions
//                                and discovery instead of bridge.secret, so the
//                                ESP-NOW wrapped XOR key (recoverable from the
//                                air) is not also the LAN key. off = use
//                                bridge.secret (compatible default). Never
//                                printed.
//   get bridge.autopeers         UDP bridge builds only.
//
// Saved in /bridge_discovery apart from NodePrefs (like espnow.repeat). A
// missing record means the defaults above; a damaged record falls back to the
// same defaults and is reported until saved again.

#if defined(WITH_ESPNOW_BRIDGE) || defined(WITH_UDP_BRIDGE)
#define MESH_BRIDGE_DISCOVERY_SETTINGS 1
#if defined(WITH_UDP_BRIDGE)
#define MESH_BRIDGE_DISCOVERY 1
#endif
#include "IdentityStore.h"
#include <stddef.h>
#include <stdint.h>

namespace mesh { namespace discovery {

enum class AnnounceMode : uint8_t { Off = 0, On = 1, Auto = 2 };

void loadBridgeDiscoverySettings(FILESYSTEM* fs);
bool bridgeDiscoverySettingsHealthy();
AnnounceMode bridgeAnnounceMode();
// Should the repeater announce now? On, or Auto while the UDP bridge runs.
bool bridgeAnnounceActive(bool udp_running);
uint16_t bridgeRxLimit();   // packets per minute, 0 = no limit

// What the CLI needs from the running repeater.
struct DiscoveryRuntime {
  // One line describing the auto-paired heads (may be null).
  void (*describe_peers)(void* ctx, char* out, size_t capacity) = nullptr;
  void* ctx = nullptr;
};

// get/set bridge.rxlimit; with the UDP bridge also get/set bridge.announce,
// get/set udp.secret, get bridge.autopeers. Returns false for any other
// command, leaving `reply` untouched.
bool handleBridgeDiscoveryCommand(const char* command, char* reply, size_t capacity,
                                  const DiscoveryRuntime* runtime = nullptr);

}}  // namespace mesh::discovery
#endif
