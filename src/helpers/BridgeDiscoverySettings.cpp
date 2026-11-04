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
constexpr size_t SecretSize = 16;
// "EBD\1", announce mode, rxlimit u16, udp.secret[16], CRC-32
constexpr size_t ImageSize = 4 + 1 + 2 + SecretSize + 4;
FILESYSTEM* settings_fs = nullptr;
bool healthy = false, writable = false;

struct Values {
  AnnounceMode announce = AnnounceMode::Auto;
  uint16_t rx_limit = kRxLimitDefault;
  char secret[SecretSize] = {};
};
Values current;

uint32_t checksum(const uint8_t* bytes, size_t size) {
  uint32_t crc = 0xffffffffU;
  while (size--) {
    crc ^= *bytes++;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
  }
  return ~crc;
}

void encode(uint8_t image[ImageSize], const Values& v) {
  memcpy(image, "EBD\1", 4);
  image[4] = static_cast<uint8_t>(v.announce);
  image[5] = static_cast<uint8_t>(v.rx_limit);
  image[6] = static_cast<uint8_t>(v.rx_limit >> 8);
  memcpy(image + 7, v.secret, SecretSize);
  const uint32_t crc = checksum(image, 7 + SecretSize);
  for (unsigned i = 0; i < 4; ++i) image[7 + SecretSize + i] = static_cast<uint8_t>(crc >> (8 * i));
}

bool decode(const uint8_t image[ImageSize], Values& out) {
  if (memcmp(image, "EBD\1", 4) || image[4] > 2) return false;
  uint32_t saved = 0;
  for (unsigned i = 0; i < 4; ++i) saved |= uint32_t(image[7 + SecretSize + i]) << (8 * i);
  if (saved != checksum(image, 7 + SecretSize)) return false;
  const uint16_t limit = static_cast<uint16_t>(image[5] | (image[6] << 8));
  if (limit > kRxLimitMax) return false;
  const char* text = reinterpret_cast<const char*>(image + 7);
  const size_t length = strnlen(text, SecretSize);
  if (length == SecretSize) return false;
  for (size_t i = length; i < SecretSize; ++i) if (text[i]) return false;
  Values v;
  v.announce = static_cast<AnnounceMode>(image[4]);
  v.rx_limit = limit;
  memcpy(v.secret, text, length);
  out = v;
  return true;
}

void apply() {
  rxLimiter().configure(current.rx_limit);
  char* override_secret = udpSecretOverride();
  memset(override_secret, 0, SecretSize);
  memcpy(override_secret, current.secret, strnlen(current.secret, SecretSize - 1));
}

void applyDefaults() {
  memset(current.secret, 0, sizeof(current.secret));
  current = Values();
  apply();
}

bool save(const Values& v) {
  if (!settings_fs || !writable || v.rx_limit > kRxLimitMax) return false;
  if (healthy && current.announce == v.announce && current.rx_limit == v.rx_limit
      && !memcmp(current.secret, v.secret, SecretSize)) return true;
  uint8_t image[ImageSize];
  encode(image, v);
  ContactFileTransaction file(settings_fs, Path, filePresence<FILESYSTEM>);
  using Outcome = ContactFileTransaction::Outcome;
  if (!file || file.write(image, sizeof(image)) != sizeof(image)) {
    memset(image, 0, sizeof(image));
    if (file.outcome() == Outcome::Indeterminate) {
      healthy = writable = false;
      applyDefaults();
    }
    return false;
  }
  file.commit();
  if (file.outcome() == Outcome::NotCommitted) { memset(image, 0, sizeof(image)); return false; }
  uint8_t verified[ImageSize];
  const bool ok = file.outcome() == Outcome::Committed
      && storage::readFixedSettingsFile(settings_fs, Path, verified, sizeof(verified))
      && !memcmp(verified, image, sizeof(image));
  memset(verified, 0, sizeof(verified));
  memset(image, 0, sizeof(image));
  if (!ok) {
    healthy = writable = false;
    applyDefaults();
    return false;
  }
  current = v;
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

const char* modeName(AnnounceMode m) {
  return m == AnnounceMode::Off ? "off" : m == AnnounceMode::On ? "on" : "auto";
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
  Values v;
  healthy = read == storage::FixedSettingsRead::Complete && decode(image, v);
  memset(image, 0, sizeof(image));
  if (healthy) {
    current = v;
    apply();
  }
  memset(v.secret, 0, sizeof(v.secret));
}

bool bridgeDiscoverySettingsHealthy() { return healthy; }
AnnounceMode bridgeAnnounceMode() { return current.announce; }
bool bridgeAnnounceActive(bool udp_running) {
  return current.announce == AnnounceMode::On || (current.announce == AnnounceMode::Auto && udp_running);
}
uint16_t bridgeRxLimit() { return current.rx_limit; }

bool handleBridgeDiscoveryCommand(const char* command, char* reply, size_t capacity,
                                  const DiscoveryRuntime* runtime) {
  if (!command || !reply || !capacity) return false;
  const bool get_limit = matches(command, "get bridge.rxlimit");
  const bool set_limit = matches(command, "set bridge.rxlimit");
#if defined(MESH_BRIDGE_DISCOVERY)
  const bool get_announce = matches(command, "get bridge.announce");
  const bool set_announce = matches(command, "set bridge.announce");
  const bool get_peers = matches(command, "get bridge.autopeers");
  const bool get_secret = matches(command, "get udp.secret");
  const bool set_secret = matches(command, "set udp.secret");
#else
  const bool get_announce = false, set_announce = false, get_peers = false, get_secret = false,
             set_secret = false;
  (void)runtime;
#endif
  if (!get_announce && !set_announce && !get_limit && !set_limit && !get_peers && !get_secret && !set_secret)
    return false;

  if (get_peers) {
    if (command[20]) snprintf(reply, capacity, "Error: use get bridge.autopeers");
    else if (runtime && runtime->describe_peers) runtime->describe_peers(runtime->ctx, reply, capacity);
    else snprintf(reply, capacity, "> none");
    return true;
  }
  if (get_secret || set_secret) {
    if (get_secret) {
      if (command[14]) snprintf(reply, capacity, "Error: use get udp.secret");
      else snprintf(reply, capacity, "> %s", current.secret[0] ? "set" : "off (bridge.secret)");
      return true;
    }
    const char* value = argument(command, 14);
    Values v = current;
    memset(v.secret, 0, sizeof(v.secret));
    if (strcmp(value, "off")) {
      const size_t n = strlen(value);
      if (n == 0 || n >= SecretSize) {
        snprintf(reply, capacity, "Error: udp.secret is 1-15 characters, or off (use bridge.secret)");
        return true;
      }
      memcpy(v.secret, value, n);
    }
    const bool ok = save(v);
    memset(v.secret, 0, sizeof(v.secret));
    // Never "unchanged": the reply tells nothing about the old secret.
    snprintf(reply, capacity, ok ? "OK - udp.secret saved; UDP sessions restart" : "Error: udp.secret not saved");
    return true;
  }
  const size_t key_len = get_announce || set_announce ? 19 : 18;
  if (get_announce || get_limit) {
    if (command[key_len]) {
      snprintf(reply, capacity, "Error: use %s", get_announce ? "get bridge.announce" : "get bridge.rxlimit");
    } else if (!healthy) {
      snprintf(reply, capacity, "Error: bridge discovery settings unavailable; defaults in use");
    } else if (get_announce) {
      snprintf(reply, capacity, "> %s", modeName(current.announce));
    } else {
      const RxLimiter& l = rxLimiter();
      if (current.rx_limit == 0)
        snprintf(reply, capacity, "> 0 (no limit) passed=%lu dropped=%lu shared=%lu",
                 (unsigned long)l.passed(), (unsigned long)l.dropped(), (unsigned long)l.shared());
      else
        snprintf(reply, capacity, "> %u/min (burst %u) passed=%lu dropped=%lu shared=%lu", unsigned(current.rx_limit),
                 unsigned(l.burst()), (unsigned long)l.passed(), (unsigned long)l.dropped(),
                 (unsigned long)l.shared());
    }
    return true;
  }
  const char* value = argument(command, key_len);
  Values v = current;
  if (set_announce) {
    if (!strcmp(value, "on")) v.announce = AnnounceMode::On;
    else if (!strcmp(value, "off")) v.announce = AnnounceMode::Off;
    else if (!strcmp(value, "auto")) v.announce = AnnounceMode::Auto;
    else {
      snprintf(reply, capacity, "Error: use set bridge.announce on|off|auto");
      return true;
    }
    const bool unchanged = healthy && current.announce == v.announce;
    if (!save(v)) snprintf(reply, capacity, "Error: bridge.announce not saved");
    else snprintf(reply, capacity, unchanged ? "OK - bridge.announce unchanged" : "OK - bridge.announce saved");
    memset(v.secret, 0, sizeof(v.secret));
    return true;
  }
  if (!parseLimit(value, v.rx_limit)) {
    snprintf(reply, capacity, "Error: use set bridge.rxlimit 0-%u (packets/min, 0 = no limit)",
             unsigned(kRxLimitMax));
    memset(v.secret, 0, sizeof(v.secret));
    return true;
  }
  const bool unchanged = healthy && current.rx_limit == v.rx_limit;
  if (!save(v)) snprintf(reply, capacity, "Error: bridge.rxlimit not saved");
  else snprintf(reply, capacity, unchanged ? "OK - bridge.rxlimit unchanged" : "OK - bridge.rxlimit saved");
  memset(v.secret, 0, sizeof(v.secret));
  return true;
}

}} // namespace mesh::discovery
#endif
