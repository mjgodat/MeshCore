#include "ESPNowLinkSettings.h"

#if defined(MESH_ESPNOW_LINK_SETTINGS)
#include "CheckedSettingsFile.h"
#include "ContactFileTransaction.h"
#include "FilePresence.h"
#include <helpers/bridges/ESPNowWrappedFrame.h>
#include <stdio.h>
#include <string.h>

namespace mesh { namespace espnow {
namespace {
constexpr char Path[] = "/espnow_link";
constexpr size_t SecretSize = mesh::bridge::WRAPPED_SECRET_SIZE;
constexpr size_t ImageSize = 4 + 1 + SecretSize + 4;
FILESYSTEM* settings_fs = nullptr;
bool healthy = false, writable = false, activated = false;
bool saved_wrapped = false;
char saved_secret[SecretSize] = {};

uint32_t checksum(const uint8_t* bytes, size_t size) {
  uint32_t crc = 0xffffffffU;
  while (size--) {
    crc ^= *bytes++;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
  }
  return ~crc;
}

void encode(uint8_t image[ImageSize], bool wrapped, const char secret[SecretSize]) {
  memcpy(image, "ENL\1", 4);
  image[4] = wrapped ? 1 : 0;
  memcpy(image + 5, secret, SecretSize);
  const uint32_t crc = checksum(image, 5 + SecretSize);
  for (unsigned i = 0; i < 4; ++i) image[5 + SecretSize + i] = static_cast<uint8_t>(crc >> (8 * i));
}

bool decode(const uint8_t image[ImageSize], bool& wrapped, char secret[SecretSize]) {
  if (memcmp(image, "ENL\1", 4) || image[4] > 1) return false;
  uint32_t saved = 0;
  for (unsigned i = 0; i < 4; ++i) saved |= uint32_t(image[5 + SecretSize + i]) << (8 * i);
  if (saved != checksum(image, 5 + SecretSize)) return false;
  const char* text = reinterpret_cast<const char*>(image + 5);
  const size_t length = strnlen(text, SecretSize);
  if (length == SecretSize) return false;                    // unterminated
  for (size_t i = length; i < SecretSize; ++i) if (text[i]) return false;  // NUL padded
  if (image[4] && length == 0) return false;                 // wrapped needs a secret
  wrapped = image[4] != 0;
  memset(secret, 0, SecretSize);
  memcpy(secret, text, length);
  return true;
}

void applyDefaults() {
  saved_wrapped = false;
  memset(saved_secret, 0, sizeof(saved_secret));
}

bool save(bool wrapped, const char* secret) {
  if (!settings_fs || !writable) return false;
  char padded[SecretSize] = {};
  memcpy(padded, secret, strnlen(secret, SecretSize - 1));
  if (healthy && saved_wrapped == wrapped && !memcmp(saved_secret, padded, SecretSize)) return true;
  uint8_t image[ImageSize];
  encode(image, wrapped, padded);
  ContactFileTransaction file(settings_fs, Path, filePresence<FILESYSTEM>);
  using Outcome = ContactFileTransaction::Outcome;
  const bool written = file && file.write(image, sizeof(image)) == sizeof(image);
  if (written) file.commit();
  uint8_t verified[ImageSize] = {};
  const bool ok = written && file.outcome() == Outcome::Committed
      && storage::readFixedSettingsFile(settings_fs, Path, verified, sizeof(verified))
      && !memcmp(verified, image, sizeof(image));
  memset(verified, 0, sizeof(verified));
  memset(image, 0, sizeof(image));
  if (!ok) {
    // NotCommitted leaves the previous record in place; anything else may not.
    if (!written ? file.outcome() == Outcome::Indeterminate : file.outcome() != Outcome::NotCommitted) {
      healthy = writable = false;
      applyDefaults();
    }
    memset(padded, 0, sizeof(padded));
    return false;
  }
  saved_wrapped = wrapped;
  memcpy(saved_secret, padded, SecretSize);
  memset(padded, 0, sizeof(padded));
  healthy = true;
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

const char* formatName(bool wrapped) { return wrapped ? "wrapped" : "raw"; }
} // namespace

void loadEspNowLinkSettings(FILESYSTEM* fs) {
  settings_fs = fs;
  healthy = writable = false;
  applyDefaults();
  if (fs && ContactFileTransaction::recover(fs, Path, filePresence<FILESYSTEM>)) {
    bool present = false;
    if (filePresence(fs, Path, present)) {
      if (!present) {
        healthy = writable = true;
      } else {
        uint8_t image[ImageSize];
        const auto read = storage::readFixedSettingsFileResult(fs, Path, image, sizeof(image));
        if (read != storage::FixedSettingsRead::Unreadable) {
          writable = true;  // a classified malformed record permits an explicit repair
          bool wrapped = false;
          char secret[SecretSize] = {};
          healthy = read == storage::FixedSettingsRead::Complete && decode(image, wrapped, secret);
          if (healthy) {
            saved_wrapped = wrapped;
            memcpy(saved_secret, secret, SecretSize);
          }
          memset(secret, 0, sizeof(secret));
        }
        memset(image, 0, sizeof(image));
      }
    }
  }
  // The format of this boot: decided once, by the first load.
  if (!activated) {
    activated = true;
    activateLink(saved_wrapped, saved_secret);
  }
}

bool espNowLinkSettingsHealthy() { return healthy; }
bool savedEspNowLinkWrapped() { return saved_wrapped; }
bool savedEspNowLinkSecret(char out[16]) {
  memcpy(out, saved_secret, SecretSize);
  return saved_secret[0] != 0;
}

bool handleEspNowLinkCommand(const char* command, char* reply, size_t capacity,
                             const LinkCounters* counters) {
  if (!command || !reply || !capacity) return false;
  const bool get_format = matches(command, "get espnow.format");
  const bool set_format = matches(command, "set espnow.format");
  const bool get_secret = matches(command, "get espnow.secret");
  const bool set_secret = matches(command, "set espnow.secret");
  if (!get_format && !set_format && !get_secret && !set_secret) return false;
  const size_t key_len = 17;  // every key above
  const bool active_wrapped = activeLink().wrapped.load(std::memory_order_acquire);

  if (get_format || get_secret) {
    if (command[key_len]) {
      snprintf(reply, capacity, "Error: use %s", get_format ? "get espnow.format" : "get espnow.secret");
    } else if (!healthy) {
      snprintf(reply, capacity, "Error: ESP-NOW link settings unavailable; %s until reboot",
               formatName(active_wrapped));
    } else if (get_secret) {
      snprintf(reply, capacity, "> %s", saved_secret[0] ? "set" : "not set");
    } else if (saved_wrapped != active_wrapped) {
      snprintf(reply, capacity, "> %s (saved; %s until reboot)", formatName(saved_wrapped),
               formatName(active_wrapped));
    } else if (active_wrapped && counters) {
      snprintf(reply, capacity, "> wrapped (refused: not wrapped %lu, wrong secret %lu)",
               (unsigned long)counters->foreign, (unsigned long)counters->checksum);
    } else {
      snprintf(reply, capacity, "> %s", formatName(active_wrapped));
    }
    return true;
  }

  const char* value = argument(command, key_len);
  if (set_format) {
    bool wrapped = false;
    if (!strcmp(value, "wrapped")) wrapped = true;
    else if (strcmp(value, "raw")) {
      snprintf(reply, capacity, "Error: use set espnow.format raw|wrapped");
      return true;
    }
    if (wrapped && !saved_secret[0]) {
      snprintf(reply, capacity, "Error: set espnow.secret first (the bridge's bridge.secret)");
      return true;
    }
    const bool unchanged = healthy && saved_wrapped == wrapped;
    if (!save(wrapped, saved_secret)) {
      snprintf(reply, capacity, "Error: espnow.format not saved");
      return true;
    }
    snprintf(reply, capacity, unchanged ? "OK - espnow.format unchanged"
        : wrapped == active_wrapped ? "OK - espnow.format saved"
        : "OK - espnow.format saved; reboot to apply");
    return true;
  }

  // set espnow.secret <1-15 characters> | off
  const size_t length = strlen(value);
  if (!strcmp(value, "off")) {
    if (saved_wrapped) {
      snprintf(reply, capacity, "Error: set espnow.format raw first");
      return true;
    }
    value = "";
  } else if (length == 0 || length >= SecretSize) {
    snprintf(reply, capacity, "Error: secret must be 1-%u characters", (unsigned)(SecretSize - 1));
    return true;
  }
  const bool unchanged = healthy && !strncmp(saved_secret, value, SecretSize);
  if (!save(saved_wrapped, value)) {
    snprintf(reply, capacity, "Error: espnow.secret not saved");
    return true;
  }
  snprintf(reply, capacity, unchanged ? "OK - espnow.secret unchanged"
      : active_wrapped || saved_wrapped ? "OK - espnow.secret saved; reboot to apply"
      : "OK - espnow.secret saved");
  return true;
}

}} // namespace mesh::espnow
#endif
