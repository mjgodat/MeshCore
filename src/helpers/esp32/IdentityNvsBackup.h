#pragma once

// Keeps a copy of the node identity in ESP32 NVS (COMPANION_IDENTITY_NVS_BACKUP).
// For boards run under an app launcher (e.g. bmorcelli Launcher on the
// Cardputer): installing another app replaces the data partition that holds
// /_main.id, but NVS is left alone, so restoring from this copy keeps the
// node's key — and therefore its contacts' trust — across app switches.

#include <Identity.h>
#include <Preferences.h>
#include <string.h>

namespace mesh {
namespace esp32 {

static constexpr const char* kIdentityBackupNamespace = "mcid";
static constexpr const char* kIdentityBackupKey = "main";
static constexpr size_t kIdentityBackupSize = PRV_KEY_SIZE + PUB_KEY_SIZE;

// True when a complete backup was read into `identity`.
inline bool loadIdentityBackup(LocalIdentity& identity) {
  uint8_t data[kIdentityBackupSize];
  Preferences prefs;
  if (!prefs.begin(kIdentityBackupNamespace, true)) return false;
  const size_t n = prefs.getBytes(kIdentityBackupKey, data, sizeof(data));
  prefs.end();
  if (n != sizeof(data)) return false;
  identity.readFrom(data, n);
  memset(data, 0, sizeof(data));
  return true;
}

// Write the backup only when it differs, so normal boots never wear flash.
inline bool saveIdentityBackup(const LocalIdentity& identity) {
  uint8_t data[kIdentityBackupSize];
  uint8_t stored[kIdentityBackupSize];
  if (identity.writeTo(data, sizeof(data)) != sizeof(data)) return false;
  Preferences prefs;
  if (!prefs.begin(kIdentityBackupNamespace, false)) return false;
  const bool same = prefs.getBytes(kIdentityBackupKey, stored, sizeof(stored)) == sizeof(stored)
      && memcmp(stored, data, sizeof(data)) == 0;
  const bool ok = same || prefs.putBytes(kIdentityBackupKey, data, sizeof(data)) == sizeof(data);
  prefs.end();
  memset(data, 0, sizeof(data));
  memset(stored, 0, sizeof(stored));
  return ok;
}

}  // namespace esp32
}  // namespace mesh
