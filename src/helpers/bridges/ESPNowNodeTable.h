#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mesh {
namespace bridge {

// ESP-NOW stations this bridge hears directly (by source MAC): heads,
// ESP-NOW relays and any other ESP-NOW MeshCore node in radio range.
// Nodes reachable only through a relay are not heard directly and do not
// appear here. Not thread-safe: the caller serialises access.
class ESPNowNodeTable {
 public:
  static constexpr uint8_t kCapacity = 16;
  static constexpr uint32_t kWindowMillis = 15UL * 60UL * 1000UL;

  void note(const uint8_t mac[6], uint32_t now) {
    Entry* slot = nullptr;
    Entry* oldest = &_entries[0];
    for (Entry& e : _entries) {
      if (e.used && memcmp(e.mac, mac, 6) == 0) { slot = &e; break; }
      if (!e.used) { if (!slot) slot = &e; continue; }
      if (oldest->used && now - e.last_ms > now - oldest->last_ms) oldest = &e;
    }
    if (slot == nullptr) slot = oldest;  // full: replace the least recently heard
    if (!slot->used || memcmp(slot->mac, mac, 6) != 0) {
      memcpy(slot->mac, mac, 6);
      slot->frames = 0;
      slot->used = true;
    }
    slot->last_ms = now;
    if (slot->frames < UINT16_MAX) slot->frames++;
  }

  uint8_t activeCount(uint32_t now, uint32_t window = kWindowMillis) const {
    uint8_t n = 0;
    for (const Entry& e : _entries) n += isActive(e, now, window);
    return n;
  }

  // "2 heard 15m: 4D219C 3s/52 F80424 40s/7" (last 3 MAC bytes, age, frames),
  // newest first, truncated to fit `size`.
  void format(char* out, size_t size, uint32_t now, uint32_t window = kWindowMillis) const {
    if (size == 0) return;
    size_t len = (size_t)snprintf(out, size, "%u heard %lum:", (unsigned)activeCount(now, window),
                                  (unsigned long)(window / 60000UL));
    bool printed[kCapacity] = {};
    for (uint8_t k = 0; k < kCapacity && len < size; k++) {
      int best = -1;
      for (uint8_t i = 0; i < kCapacity; i++) {
        if (printed[i] || !isActive(_entries[i], now, window)) continue;
        if (best < 0 || now - _entries[i].last_ms < now - _entries[best].last_ms) best = i;
      }
      if (best < 0) break;
      printed[best] = true;
      const Entry& e = _entries[best];
      const int n = snprintf(out + len, size - len, " %02X%02X%02X %lus/%u", e.mac[3], e.mac[4],
                             e.mac[5], (unsigned long)((now - e.last_ms) / 1000UL),
                             (unsigned)e.frames);
      if (n < 0 || (size_t)n >= size - len) { out[len] = 0; break; }
      len += (size_t)n;
    }
  }

  void clear() { memset(_entries, 0, sizeof(_entries)); }

 private:
  struct Entry {
    uint8_t mac[6];
    uint16_t frames;
    uint32_t last_ms;
    bool used;
  };

  static bool isActive(const Entry& e, uint32_t now, uint32_t window) {
    return e.used && now - e.last_ms <= window;
  }

  Entry _entries[kCapacity] = {};
};

}  // namespace bridge
}  // namespace mesh
