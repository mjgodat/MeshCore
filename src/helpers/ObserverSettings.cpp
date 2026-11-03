#include "ObserverSettings.h"

#if defined(MESH_ESPNOW_OBSERVER_SETTINGS)
#include "CheckedSettingsFile.h"
#include "ContactFileTransaction.h"
#include "FilePresence.h"
#include <stdio.h>
#include <string.h>

namespace mesh { namespace espnow {
namespace {
constexpr char Path[] = "/espnow_observer";
constexpr size_t ImageSize = 9;
FILESYSTEM* settings_fs = nullptr;
bool healthy = false, writable = false, enabled = false;

uint32_t checksum(const uint8_t* bytes, size_t size) {
  uint32_t crc = 0xffffffffU;
  while (size--) {
    crc ^= *bytes++;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
  }
  return ~crc;
}

void encode(uint8_t image[ImageSize], bool on) {
  memcpy(image, "ENO\1", 4);
  image[4] = on ? 1 : 0;
  const uint32_t crc = checksum(image, 5);
  for (unsigned i = 0; i < 4; ++i) image[5 + i] = static_cast<uint8_t>(crc >> (8 * i));
}

bool decode(const uint8_t image[ImageSize], bool& on) {
  if (memcmp(image, "ENO\1", 4) || image[4] > 1) return false;
  uint32_t saved = 0;
  for (unsigned i = 0; i < 4; ++i) saved |= uint32_t(image[5 + i]) << (8 * i);
  if (saved != checksum(image, 5)) return false;
  on = image[4] == 1;
  return true;
}

bool matches(const char* command, const char* key, size_t key_len) {
  return !strncmp(command, key, key_len)
      && (!command[key_len] || command[key_len] == ' ' || command[key_len] == '\t');
}
} // namespace

void loadEspNowObserverSettings(FILESYSTEM* fs) {
  settings_fs = fs;
  healthy = writable = enabled = false;
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
  writable = true; // A classified malformed record permits an explicit repair.
  bool on = false;
  healthy = read == storage::FixedSettingsRead::Complete && decode(image, on);
  enabled = healthy && on;
}

bool espNowObserverSettingsHealthy() { return healthy; }
bool espNowObserverEnabled() { return enabled; }

bool saveEspNowObserver(bool on) {
  if (!settings_fs || !writable) return false;
  if (healthy && enabled == on) return true;
  uint8_t image[ImageSize];
  encode(image, on);
  ContactFileTransaction file(settings_fs, Path, filePresence<FILESYSTEM>);
  using Outcome = ContactFileTransaction::Outcome;
  if (!file || file.write(image, sizeof(image)) != sizeof(image)) {
    if (file.outcome() == Outcome::Indeterminate) healthy = writable = enabled = false;
    return false;
  }
  file.commit();
  if (file.outcome() == Outcome::NotCommitted) return false;
  uint8_t verified[ImageSize];
  if (file.outcome() != Outcome::Committed ||
      !storage::readFixedSettingsFile(settings_fs, Path, verified, sizeof(verified)) ||
      memcmp(verified, image, sizeof(image))) {
    healthy = writable = enabled = false;
    return false;
  }
  enabled = on;
  healthy = true;
  return true;
}

bool handleEspNowObserverCommand(const char* command, char* reply, size_t capacity) {
  if (!command || !reply || !capacity) return false;
  static constexpr char Get[] = "get espnow.observer", Set[] = "set espnow.observer";
  constexpr size_t key_len = sizeof(Get) - 1;
  const bool get = matches(command, Get, key_len), set = matches(command, Set, key_len);
  if (!get && !set) return false;
  if (get) {
    if (command[key_len]) snprintf(reply, capacity, "Error: use get espnow.observer");
    else if (!healthy) snprintf(reply, capacity, "Error: ESP-NOW observer settings unavailable; observer off");
    else snprintf(reply, capacity, "> %s", enabled ? "on" : "off");
    return true;
  }
  const char* value = command + key_len;
  while (*value == ' ' || *value == '\t') ++value;
  const bool on = !strcmp(value, "on"), off = !strcmp(value, "off");
  if (!on && !off) {
    snprintf(reply, capacity, "Error: use set espnow.observer on|off");
    return true;
  }
  const bool unchanged = healthy && enabled == on;
  if (!saveEspNowObserver(on)) {
    snprintf(reply, capacity, healthy ? "Error: ESP-NOW observer not saved"
        : "Error: ESP-NOW observer not saved; observer off until reload");
    return true;
  }
  snprintf(reply, capacity, "OK - espnow.observer %s", unchanged ? "unchanged" : "saved");
  return true;
}

}} // namespace mesh::espnow
#endif
