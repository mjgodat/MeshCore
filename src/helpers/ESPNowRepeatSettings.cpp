#include "ESPNowRepeatSettings.h"

#if defined(MESH_ESPNOW_REPEAT_SETTINGS)
#include "CheckedSettingsFile.h"
#include "ContactFileTransaction.h"
#include "FilePresence.h"
#include <stdio.h>
#include <string.h>

namespace mesh { namespace espnow {
namespace {
constexpr char Path[] = "/espnow_repeat";
constexpr size_t ImageSize = 11;
FILESYSTEM* settings_fs = nullptr;
bool healthy = false, writable = false;

uint32_t checksum(const uint8_t* bytes, size_t size) {
  uint32_t crc = 0xffffffffU;
  while (size--) {
    crc ^= *bytes++;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
  }
  return ~crc;
}

void encode(uint8_t image[ImageSize], uint8_t count, uint16_t gap_ms) {
  memcpy(image, "ENR\1", 4);
  image[4] = count;
  image[5] = static_cast<uint8_t>(gap_ms);
  image[6] = static_cast<uint8_t>(gap_ms >> 8);
  const uint32_t crc = checksum(image, 7);
  for (unsigned i = 0; i < 4; ++i) image[7 + i] = static_cast<uint8_t>(crc >> (8 * i));
}

bool decode(const uint8_t image[ImageSize], uint8_t& count, uint16_t& gap_ms) {
  if (memcmp(image, "ENR\1", 4)) return false;
  uint32_t saved = 0;
  for (unsigned i = 0; i < 4; ++i) saved |= uint32_t(image[7 + i]) << (8 * i);
  if (saved != checksum(image, 7)) return false;
  const uint16_t gap = static_cast<uint16_t>(image[5] | (image[6] << 8));
  if (!isValidRepeatCount(image[4]) || !isValidRepeatGap(gap)) return false;
  count = image[4];
  gap_ms = gap;
  return true;
}

void applyDefaults() {
  espNowRepeatConfig().count = kRepeatCountDefault;
  espNowRepeatConfig().gap_ms = kRepeatGapDefaultMs;
}

bool matches(const char* command, const char* key, size_t key_len) {
  return !strncmp(command, key, key_len)
      && (!command[key_len] || command[key_len] == ' ' || command[key_len] == '\t');
}

const char* argument(const char* command, size_t key_len) {
  const char* value = command + key_len;
  while (*value == ' ' || *value == '\t') ++value;
  return value;
}

void formatError(char* reply, size_t capacity, const char* message) {
  snprintf(reply, capacity, "Error: %s", message);
}
} // namespace

void loadEspNowRepeatSettings(FILESYSTEM* fs) {
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
  writable = true; // A classified malformed record permits an explicit repair.
  uint8_t count = kRepeatCountDefault;
  uint16_t gap_ms = kRepeatGapDefaultMs;
  healthy = read == storage::FixedSettingsRead::Complete && decode(image, count, gap_ms);
  if (healthy) {
    espNowRepeatConfig().count = count;
    espNowRepeatConfig().gap_ms = gap_ms;
  }
}

bool espNowRepeatSettingsHealthy() { return healthy; }

bool saveEspNowRepeat(uint8_t count, uint16_t gap_ms) {
  if (!settings_fs || !writable || !isValidRepeatCount(count) || !isValidRepeatGap(gap_ms))
    return false;
  EspNowRepeatConfig& config = espNowRepeatConfig();
  if (healthy && config.count == count && config.gap_ms == gap_ms) return true;
  uint8_t image[ImageSize];
  encode(image, count, gap_ms);
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
  if (file.outcome() != Outcome::Committed ||
      !storage::readFixedSettingsFile(settings_fs, Path, verified, sizeof(verified)) ||
      memcmp(verified, image, sizeof(image))) {
    healthy = writable = false;
    applyDefaults();
    return false;
  }
  config.count = count;
  config.gap_ms = gap_ms;
  healthy = true;
  return true;
}

bool handleEspNowRepeatCommand(const char* command, char* reply, size_t capacity) {
  if (!command || !reply || !capacity) return false;
  static constexpr char GetGap[] = "get espnow.repeat.gap", SetGap[] = "set espnow.repeat.gap";
  static constexpr char GetCount[] = "get espnow.repeat", SetCount[] = "set espnow.repeat";
  const size_t gap_len = sizeof(GetGap) - 1, count_len = sizeof(GetCount) - 1;
  const bool get_gap = matches(command, GetGap, gap_len);
  const bool set_gap = matches(command, SetGap, gap_len);
  const bool get_count = !get_gap && matches(command, GetCount, count_len);
  const bool set_count = !set_gap && matches(command, SetCount, count_len);
#if !defined(WITH_ESPNOW_BRIDGE)
  if (!strcmp(command, "get espnow.stats")) {
    snprintf(reply, capacity, "> repeat=%lu",
             static_cast<unsigned long>(espNowRepeatConfig().radio_copies));
    return true;
  }
#endif
  if (!get_gap && !set_gap && !get_count && !set_count) return false;
  const EspNowRepeatConfig& config = espNowRepeatConfig();
  if (get_gap || get_count) {
    if (command[get_gap ? gap_len : count_len])
      formatError(reply, capacity, get_gap ? "use get espnow.repeat.gap" : "use get espnow.repeat");
    else if (!healthy)
      formatError(reply, capacity, "ESP-NOW repeat settings unavailable; repeat off");
    else snprintf(reply, capacity, "> %u", get_gap ? unsigned(config.gap_ms) : unsigned(config.count));
    return true;
  }
  uint32_t value = 0;
  const char* text = argument(command, set_gap ? gap_len : count_len);
  if (set_gap ? !parseRepeatDecimal(text, kRepeatGapMaxMs, value)
              : !parseRepeatDecimal(text, kRepeatCountMax, value) || !isValidRepeatCount(value)) {
    formatError(reply, capacity, set_gap ? "use set espnow.repeat.gap 0-200" : "use set espnow.repeat 1-4");
    return true;
  }
  const uint8_t count = set_gap ? config.count : static_cast<uint8_t>(value);
  const uint16_t gap_ms = set_gap ? static_cast<uint16_t>(value) : config.gap_ms;
  const bool unchanged = healthy && config.count == count && config.gap_ms == gap_ms;
  if (!saveEspNowRepeat(count, gap_ms)) {
    formatError(reply, capacity, healthy ? "ESP-NOW repeat not saved"
        : "ESP-NOW repeat not saved; repeat off until reload");
    return true;
  }
  snprintf(reply, capacity, "OK - %s %s", set_gap ? "espnow.repeat.gap" : "espnow.repeat",
           unchanged ? "unchanged" : "saved");
  return true;
}

}} // namespace mesh::espnow
#endif
