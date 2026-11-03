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

constexpr char TransportPath[] = "/observer_transport";
bool transport_healthy = false, transport_writable = false;
mesh::observer::Transport transport = mesh::observer::Transport::EspNow;

// Writes and verifies one fixed record. False when not saved; `lost` is set
// when the file may now be in an unknown state.
bool writeRecord(const char* path, const uint8_t* image, size_t size, bool& lost) {
  lost = false;
  ContactFileTransaction file(settings_fs, path, filePresence<FILESYSTEM>);
  using Outcome = ContactFileTransaction::Outcome;
  if (!file || file.write(image, size) != size) {
    lost = file.outcome() == Outcome::Indeterminate;
    return false;
  }
  file.commit();
  if (file.outcome() == Outcome::NotCommitted) return false;
  uint8_t verified[16];
  if (size > sizeof(verified) || file.outcome() != Outcome::Committed ||
      !storage::readFixedSettingsFile(settings_fs, path, verified, size) ||
      memcmp(verified, image, size)) {
    lost = true;
    return false;
  }
  return true;
}

void loadTransport(FILESYSTEM* fs) {
  transport_healthy = transport_writable = false;
  transport = mesh::observer::Transport::EspNow;
  if (!fs || !ContactFileTransaction::recover(fs, TransportPath, filePresence<FILESYSTEM>)) return;
  bool present = false;
  if (!filePresence(fs, TransportPath, present)) return;
  if (!present) { transport_healthy = transport_writable = true; return; }
  uint8_t image[mesh::observer::kTransportImageSize];
  const auto read = storage::readFixedSettingsFileResult(fs, TransportPath, image, sizeof(image));
  if (read == storage::FixedSettingsRead::Unreadable) return;
  transport_writable = true;  // a classified malformed record permits a repair
  mesh::observer::Transport saved = mesh::observer::Transport::EspNow;
  transport_healthy = read == storage::FixedSettingsRead::Complete
      && mesh::observer::decodeTransport(image, sizeof(image), saved);
  if (transport_healthy) transport = saved;
}
} // namespace

void loadEspNowObserverSettings(FILESYSTEM* fs) {
  settings_fs = fs;
  loadTransport(fs);  // its own record, independent of /espnow_observer
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

mesh::observer::Transport observerTransport() { return transport; }

bool saveObserverTransport(mesh::observer::Transport value) {
  if (!settings_fs || !transport_writable) return false;
  if (transport_healthy && transport == value) return true;
  uint8_t image[mesh::observer::kTransportImageSize];
  mesh::observer::encodeTransport(image, value);
  bool lost = false;
  if (!writeRecord(TransportPath, image, sizeof(image), lost)) {
    if (lost) {
      transport_healthy = transport_writable = false;
      transport = mesh::observer::Transport::EspNow;
    }
    return false;
  }
  transport = value;
  transport_healthy = true;
  return true;
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

namespace {
bool handleTransportCommand(const char* command, char* reply, size_t capacity,
                            const ObserverRuntime* runtime) {
  static constexpr char Get[] = "get observer.transport", Set[] = "set observer.transport";
  constexpr size_t key_len = sizeof(Get) - 1;
  const bool get = matches(command, Get, key_len), set = matches(command, Set, key_len);
  if (!get && !set) return false;
  using namespace mesh::observer;
  if (get) {
    if (command[key_len]) { snprintf(reply, capacity, "Error: use get observer.transport"); return true; }
    char detail[112] = "";
    if (runtime && runtime->describe) runtime->describe(runtime->ctx, detail, sizeof(detail));
    snprintf(reply, capacity, "> %s%s%s%s", transportName(transport),
             transport_healthy ? "" : " (settings unavailable)", detail[0] ? " " : "", detail);
    return true;
  }
  const char* value = command + key_len;
  while (*value == ' ' || *value == '\t') ++value;
  Transport wanted;
  if (!parseTransport(value, wanted)) {
    snprintf(reply, capacity, "Error: use set observer.transport espnow|udp|both");
    return true;
  }
  if (usesUdp(wanted) && !(runtime && runtime->udp_available)) {
    snprintf(reply, capacity, "Error: no UDP bridge in this firmware");
    return true;
  }
  if (runtime && !checkTransportChange(wanted, runtime->espnow_channel, runtime->ap_channel,
                                       reply, capacity)) return true;
  const bool unchanged = transport_healthy && transport == wanted;
  if (!saveObserverTransport(wanted)) {
    snprintf(reply, capacity, "Error: observer.transport not saved");
    return true;
  }
  snprintf(reply, capacity, "OK - observer.transport %s", unchanged ? "unchanged" : "saved");
  return true;
}
} // namespace

bool handleEspNowObserverCommand(const char* command, char* reply, size_t capacity,
                                 const ObserverRuntime* runtime) {
  if (!command || !reply || !capacity) return false;
  if (handleTransportCommand(command, reply, capacity, runtime)) return true;
  // `observer` is the transport-neutral name of `espnow.observer` (one flag).
  static constexpr char GetOld[] = "get espnow.observer", SetOld[] = "set espnow.observer";
  static constexpr char GetNew[] = "get observer", SetNew[] = "set observer";
  const char* key = nullptr;
  size_t key_len = 0;
  bool get = false;
  if (matches(command, GetOld, sizeof(GetOld) - 1)) { key = "espnow.observer"; key_len = sizeof(GetOld) - 1; get = true; }
  else if (matches(command, SetOld, sizeof(SetOld) - 1)) { key = "espnow.observer"; key_len = sizeof(SetOld) - 1; }
  else if (matches(command, GetNew, sizeof(GetNew) - 1)) { key = "observer"; key_len = sizeof(GetNew) - 1; get = true; }
  else if (matches(command, SetNew, sizeof(SetNew) - 1)) { key = "observer"; key_len = sizeof(SetNew) - 1; }
  else return false;
  if (get) {
    if (command[key_len]) snprintf(reply, capacity, "Error: use get %s", key);
    else if (!healthy) snprintf(reply, capacity, "Error: ESP-NOW observer settings unavailable; observer off");
    else snprintf(reply, capacity, "> %s", enabled ? "on" : "off");
    return true;
  }
  const char* value = command + key_len;
  while (*value == ' ' || *value == '\t') ++value;
  const bool on = !strcmp(value, "on"), off = !strcmp(value, "off");
  if (!on && !off) {
    snprintf(reply, capacity, "Error: use set %s on|off", key);
    return true;
  }
  const bool unchanged = healthy && enabled == on;
  if (!saveEspNowObserver(on)) {
    snprintf(reply, capacity, healthy ? "Error: ESP-NOW observer not saved"
        : "Error: ESP-NOW observer not saved; observer off until reload");
    return true;
  }
  snprintf(reply, capacity, "OK - %s %s", key, unchanged ? "unchanged" : "saved");
  return true;
}

}} // namespace mesh::espnow
#endif
