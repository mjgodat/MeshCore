#pragma once

// Relay names the visual head has learned from repeater adverts, kept across
// reboots (pure C++, no Arduino; host tested in test/test_relay_name_store.py).
//
// A relay that rarely adverts would otherwise show as hex ("via 7E7662")
// after every reboot until its next advert. The page task loads the store
// from NVS at boot and writes it back only when a name was added or changed,
// at most once per kSaveIntervalMs (counted from boot and from the last
// write), so flash is not worn by repeated adverts.
//
// Bounded: kCapacity entries; when full the least recently learned/heard
// relay is dropped (LRU). Entries are keyed by the first 4 bytes of the
// repeater's public key; a path hash (1..3 bytes) resolves to a name only
// when exactly one entry starts with it (matches() == 1).

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mesh { namespace head {

class RelayNameStore {
 public:
  static const size_t kCapacity = 64;
  static const size_t kKeyLen = 4;
  static const size_t kNameLen = 24;   // with the NUL (NodeInfo::name size)
  static const uint32_t kSaveIntervalMs = 60000;
  // Blob (NVS): magic 'R' 'N' version, count, then count entries of
  // key[4] name[24], oldest first.
  static const size_t kBlobHeader = 4;
  static const size_t kBlobEntry = kKeyLen + kNameLen;
  static const size_t kBlobMax = kBlobHeader + kCapacity * kBlobEntry;

  RelayNameStore() { clear(); }

  // Starts the write throttle (boot): the first write is due no sooner than
  // kSaveIntervalMs after this.
  void begin(uint32_t now_ms) {
    _last_save_ms = now_ms;
  }

  // A repeater advertised `name`. Returns true when the stored data changed
  // (a new relay or a new name); the same name again only refreshes LRU.
  bool learn(const uint8_t* key, const char* name) {
    if (key == nullptr || name == nullptr || name[0] == 0) return false;
    Entry* e = find(key);
    if (e != nullptr) {
      e->seq = ++_seq;
      if (strncmp(e->name, name, kNameLen - 1) == 0) return false;
      copyName(e->name, name);
      _dirty = true;
      return true;
    }
    e = freeSlot();
    memcpy(e->key, key, kKeyLen);
    copyName(e->name, name);
    e->seq = ++_seq;
    _dirty = true;
    return true;
  }

  // Number of stored relays whose key starts with hash[0..size) (size 1..3),
  // not counting `except_key` (a relay already known elsewhere, e.g. the live
  // model's). *name / *key: the last match's name and key; use them only when
  // the result is 1 (a unique match).
  size_t matches(const uint8_t* hash, uint8_t size, const uint8_t* except_key, const char** name,
                 const uint8_t** key = nullptr) const {
    if (hash == nullptr || size < 1 || size > 3) return 0;
    size_t n = 0;
    for (size_t i = 0; i < kCapacity; i++) {
      const Entry& e = _e[i];
      if (e.seq == 0 || memcmp(e.key, hash, size) != 0) continue;
      if (except_key != nullptr && memcmp(e.key, except_key, kKeyLen) == 0) continue;
      if (name != nullptr) *name = e.name;
      if (key != nullptr) *key = e.key;
      n++;
    }
    return n;
  }

  // The stored name of exactly this key; nullptr when unknown.
  const char* nameFor(const uint8_t* key) const {
    const Entry* e = const_cast<RelayNameStore*>(this)->find(key);
    return e != nullptr ? e->name : nullptr;
  }

  size_t count() const {
    size_t n = 0;
    for (size_t i = 0; i < kCapacity; i++) n += _e[i].seq != 0;
    return n;
  }

  // A write is due: something changed and the last write (or boot) was at
  // least kSaveIntervalMs ago (millis wrap safe).
  bool saveDue(uint32_t now_ms) const {
    return _dirty && (uint32_t)(now_ms - _last_save_ms) >= kSaveIntervalMs;
  }
  // A write was attempted at now_ms; written false (NVS failed) keeps the
  // changes for the next interval.
  void markSaved(uint32_t now_ms, bool written = true) {
    if (written) _dirty = false;
    _last_save_ms = now_ms;
  }

  // The blob, oldest entry first. Returns its length; 0 when cap is too small.
  size_t serialize(uint8_t* out, size_t cap) const {
    const size_t n = count();
    const size_t len = kBlobHeader + n * kBlobEntry;
    if (out == nullptr || cap < len) return 0;
    out[0] = 'R'; out[1] = 'N'; out[2] = kVersion; out[3] = (uint8_t)n;
    uint32_t after = 0;   // emit in seq order: the smallest seq above `after`
    for (size_t k = 0; k < n; k++) {
      const Entry* next = nullptr;
      for (size_t i = 0; i < kCapacity; i++) {
        const Entry& e = _e[i];
        if (e.seq > after && (next == nullptr || e.seq < next->seq)) next = &e;
      }
      uint8_t* p = out + kBlobHeader + k * kBlobEntry;
      memcpy(p, next->key, kKeyLen);
      memcpy(p + kKeyLen, next->name, kNameLen);
      after = next->seq;
    }
    return len;
  }

  // Replaces the contents with a blob (boot). A malformed blob loads nothing
  // and returns false; entries with an empty name or a repeated key are
  // dropped, an unterminated name is cut. Loading is not a change (no write).
  bool load(const uint8_t* blob, size_t len) {
    clear();
    if (blob == nullptr || len < kBlobHeader) return false;
    if (blob[0] != 'R' || blob[1] != 'N' || blob[2] != kVersion) return false;
    const size_t n = blob[3];
    if (n > kCapacity || len != kBlobHeader + n * kBlobEntry) return false;
    for (size_t k = 0; k < n; k++) {
      const uint8_t* p = blob + kBlobHeader + k * kBlobEntry;
      char name[kNameLen];
      memcpy(name, p + kKeyLen, kNameLen);
      name[kNameLen - 1] = 0;
      if (name[0] == 0 || find(p) != nullptr) continue;
      Entry* e = freeSlot();
      memcpy(e->key, p, kKeyLen);
      memcpy(e->name, name, kNameLen);
      e->seq = ++_seq;
    }
    _dirty = false;
    return true;
  }

 private:
  static const uint8_t kVersion = 1;
  struct Entry {
    uint8_t key[kKeyLen];
    char name[kNameLen];
    uint32_t seq;   // LRU stamp; 0 = empty slot
  };

  void clear() {
    memset(_e, 0, sizeof(_e));
    _seq = 0;
    _dirty = false;
  }

  Entry* find(const uint8_t* key) {
    if (key == nullptr) return nullptr;
    for (size_t i = 0; i < kCapacity; i++) {
      if (_e[i].seq != 0 && memcmp(_e[i].key, key, kKeyLen) == 0) return &_e[i];
    }
    return nullptr;
  }

  // An empty slot, else the least recently used one (overwritten).
  Entry* freeSlot() {
    Entry* oldest = &_e[0];
    for (size_t i = 0; i < kCapacity; i++) {
      if (_e[i].seq == 0) return &_e[i];
      if (_e[i].seq < oldest->seq) oldest = &_e[i];
    }
    return oldest;
  }

  static void copyName(char* dst, const char* src) {
    const size_t n = strnlen(src, kNameLen - 1);
    memcpy(dst, src, n);
    memset(dst + n, 0, kNameLen - n);
  }

  Entry _e[kCapacity];
  uint32_t _seq;
  uint32_t _last_save_ms = 0;
  bool _dirty;
};

}}  // namespace mesh::head
