#include "BridgeDiscoverySettings.h"

#if defined(MESH_BRIDGE_DISCOVERY_SETTINGS)
#include "CheckedSettingsFile.h"
#include "ContactFileTransaction.h"
#include "FilePresence.h"
#include <helpers/bridges/DiscoveryFrame.h>
#include <stdio.h>
#include <string.h>

namespace mesh { namespace discovery {
namespace {
constexpr char Path[] = "/bridge_discovery";
constexpr size_t ImageSize = 4 + 1 + 2 + 4;
FILESYSTEM* settings_fs = nullptr;
bool healthy = false, writable = false;
bool announce = true;
uint16_t rx_limit = kRxLimitDefault;

uint32_t checksum(const uint8_t* bytes, size_t size) {
  uint32_t crc = 0xffffffffU;
  while (size--) {
    crc ^= *bytes++;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
  }
  return ~crc;
}

void encode(uint8_t image[ImageSize], bool on, uint16_t limit) {
  memcpy(image, "EBD\1", 4);
  image[4] = on ? 1 : 0;
  image[5] = static_cast<uint8_t>(limit);
  image[6] = static_cast<uint8_t>(limit >> 8);
  const uint32_t crc = checksum(image, 7);
  for (unsigned i = 0; i < 4; ++i) image[7 + i] = static_cast<uint8_t>(crc >> (8 * i));
}

bool decode(const uint8_t image[ImageSize], bool& on, uint16_t& limit) {
  if (memcmp(image, "EBD\1", 4) || image[4] > 1) return false;
  uint32_t saved = 0;
  for (unsigned i = 0; i < 4; ++i) saved |= uint32_t(image[7 + i]) << (8 * i);
  if (saved != checksum(image, 7)) return false;
  const uint16_t value = static_cast<uint16_t>(image[5] | (image[6] << 8));
  if (value > kRxLimitMax) return false;
  on = image[4] != 0;
  limit = value;
  return true;
}

void apply() { rxLimiter().configure(rx_limit); }

void applyDefaults() {
  announce = true;
  rx_limit = kRxLimitDefault;
  apply();
}

bool save(bool on, uint16_t limit) {
  if (!settings_fs || !writable || limit > kRxLimitMax) return false;
  if (healthy && announce == on && rx_limit == limit) return true;
  uint8_t image[ImageSize];
  encode(image, on, limit);
  ContactFileTransaction file(settings_fs, Path, filePresence<FILESYSTEM>);
  using Outcome = ContactFileTransaction::Outcome;
  if (!file || file.write(image, sizeof(image)) != sizeof(image)) {
    if (file.outcome() == Outcome::Indeterminate) {
      healthy = writable = false;
      applyDefaults();
    }
    return false;
  }
  file.commit();
  if (file.outcome() == Outcome::NotCommitted) return false;
  uint8_t verified[ImageSize];
  if (file.outcome() != Outcome::Committed
      || !storage::readFixedSettingsFile(settings_fs, Path, verified, sizeof(verified))
      || memcmp(verified, image, sizeof(image))) {
    healthy = writable = false;
    applyDefaults();
    return false;
  }
  announce = on;
  rx_limit = limit;
  healthy = true;
  apply();
  return true;
}

bool matches(const char* command, const char* key) {
  const size_t n = strlen(key);
  return !strncmp(command, key, n) && (!command[n] || command[n] == ' ' || command[n] == '\t');
}

const char* argument(const char* command, size_t key_len) {
  const char* value = command + key_len;
  while (*value == ' ' || *value == '\t') ++value;
  return value;
}

bool parseLimit(const char* text, uint16_t& out) {
  if (!text || *text < '0' || *text > '9') return false;
  uint32_t value = 0;
  while (*text >= '0' && *text <= '9') {
    value = value * 10 + uint32_t(*text - '0');
    if (value > kRxLimitMax) return false;
    ++text;
  }
  while (*text == ' ' || *text == '\t') ++text;
  if (*text) return false;
  out = static_cast<uint16_t>(value);
  return true;
}
} // namespace

void loadBridgeDiscoverySettings(FILESYSTEM* fs) {
  settings_fs = fs;
  healthy = writable = false;
  applyDefaults();
  if (!fs || !ContactFileTransaction::recover(fs, Path, filePresence<FILESYSTEM>)) return;
  bool present = false;
  if (!filePresence(fs, Path, present)) return;
  if (!present) {
    healthy = writable = true;
    return;
  }
  uint8_t image[ImageSize];
  const auto read = storage::readFixedSettingsFileResult(fs, Path, image, sizeof(image));
  if (read == storage::FixedSettingsRead::Unreadable) return;
  writable = true;  // a classified malformed record permits an explicit repair
  bool on = true;
  uint16_t limit = kRxLimitDefault;
  healthy = read == storage::FixedSettingsRead::Complete && decode(image, on, limit);
  if (healthy) {
    announce = on;
    rx_limit = limit;
    apply();
  }
}

bool bridgeDiscoverySettingsHealthy() { return healthy; }
bool bridgeAnnounceEnabled() { return announce; }
uint16_t bridgeRxLimit() { return rx_limit; }

bool handleBridgeDiscoveryCommand(const char* command, char* reply, size_t capacity,
                                  const DiscoveryRuntime* runtime) {
  if (!command || !reply || !capacity) return false;
  const bool get_announce = matches(command, "get bridge.announce");
  const bool set_announce = matches(command, "set bridge.announce");
  const bool get_limit = matches(command, "get bridge.rxlimit");
  const bool set_limit = matches(command, "set bridge.rxlimit");
  const bool get_peers = matches(command, "get bridge.autopeers");
  if (!get_announce && !set_announce && !get_limit && !set_limit && !get_peers) return false;

  if (get_peers) {
    if (command[20]) snprintf(reply, capacity, "Error: use get bridge.autopeers");
    else if (runtime && runtime->describe_peers) runtime->describe_peers(runtime->ctx, reply, capacity);
    else snprintf(reply, capacity, "> none");
    return true;
  }
  const size_t key_len = get_announce || set_announce ? 19 : 18;
  if (get_announce || get_limit) {
    if (command[key_len]) {
      snprintf(reply, capacity, "Error: use %s", get_announce ? "get bridge.announce" : "get bridge.rxlimit");
    } else if (!healthy) {
      snprintf(reply, capacity, "Error: bridge discovery settings unavailable; defaults in use");
    } else if (get_announce) {
      snprintf(reply, capacity, "> %s", announce ? "on" : "off");
    } else {
      const RxLimiter& l = rxLimiter();
      if (rx_limit == 0)
        snprintf(reply, capacity, "> 0 (no limit) passed=%lu dropped=%lu",
                 (unsigned long)l.passed(), (unsigned long)l.dropped());
      else
        snprintf(reply, capacity, "> %u/min (burst %u) passed=%lu dropped=%lu", unsigned(rx_limit),
                 unsigned(l.burst()), (unsigned long)l.passed(), (unsigned long)l.dropped());
    }
    return true;
  }
  const char* value = argument(command, key_len);
  if (set_announce) {
    bool on;
    if (!strcmp(value, "on")) on = true;
    else if (!strcmp(value, "off")) on = false;
    else {
      snprintf(reply, capacity, "Error: use set bridge.announce on|off");
      return true;
    }
    const bool unchanged = healthy && announce == on;
    if (!save(on, rx_limit)) snprintf(reply, capacity, "Error: bridge.announce not saved");
    else snprintf(reply, capacity, unchanged ? "OK - bridge.announce unchanged" : "OK - bridge.announce saved");
    return true;
  }
  uint16_t limit = 0;
  if (!parseLimit(value, limit)) {
    snprintf(reply, capacity, "Error: use set bridge.rxlimit 0-%u (packets/min, 0 = no limit)",
             unsigned(kRxLimitMax));
    return true;
  }
  const bool unchanged = healthy && rx_limit == limit;
  if (!save(announce, limit)) snprintf(reply, capacity, "Error: bridge.rxlimit not saved");
  else snprintf(reply, capacity, unchanged ? "OK - bridge.rxlimit unchanged" : "OK - bridge.rxlimit saved");
  return true;
}

}} // namespace mesh::discovery
#endif
