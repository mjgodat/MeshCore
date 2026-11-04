#pragma once

// espnow.format raw|wrapped and espnow.secret on primary ESP-NOW nodes
// (ESPNOWRadio: visual heads, the DevKit relay, Generic_ESPNOW_*).
//
// wrapped is byte-for-byte the LoRa repeater bridge's `bridge.format wrapped`
// (ESPNowWrappedFrame.h): magic, Fletcher-16, XOR with the secret, which must
// equal the bridge's bridge.secret. Receive is strict per format. raw (the
// default, and what a missing or damaged record means) is unchanged.
//
// Saved in /espnow_link, apart from NodePrefs (like espnow.repeat). A change
// applies at the next reboot, so a node can be prepared over the mesh while
// it still talks the old format. The secret is never printed.

#if defined(MESH_PRIMARY_ESPNOW) && MESH_PRIMARY_ESPNOW
#define MESH_ESPNOW_LINK_SETTINGS 1
#include "IdentityStore.h"
#include <stddef.h>
#include <stdint.h>

namespace mesh { namespace espnow {

struct LinkCounters {
  uint32_t foreign = 0;   // frames refused in wrapped mode: not wrapped
  uint32_t checksum = 0;  // frames refused in wrapped mode: wrong secret
};

// Reads the saved record. The first call after boot also activates it for
// this boot (activeLink() in ESPNowWrappedFrame.h); later calls only reload.
void loadEspNowLinkSettings(FILESYSTEM* fs);
bool espNowLinkSettingsHealthy();
bool savedEspNowLinkWrapped();
bool savedEspNowLinkSecret(char out[16]);  // false when none is saved

// get/set espnow.format, get/set espnow.secret. Returns false for any other
// command, leaving `reply` untouched. `counters` (optional) adds the refusal
// counters to `get espnow.format` while wrapped is active.
bool handleEspNowLinkCommand(const char* command, char* reply, size_t capacity,
                             const LinkCounters* counters = nullptr);

}}  // namespace mesh::espnow
#endif
