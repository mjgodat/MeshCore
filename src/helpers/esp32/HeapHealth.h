#pragma once

// The repeater's heap for the observer health frame (ObserverFrame.h kind 3)
// and `stats-core`: free now, the lowest free since boot, the largest free
// block (fragmentation), all over the 8-bit-capable heap that malloc uses.

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <stdio.h>
#include <string.h>

#include <helpers/bridges/ObserverFrame.h>

namespace mesh {
namespace esp32 {

inline mesh::observer::Health heapHealth(uint32_t uptime_s = 0) {
  mesh::observer::Health h;
  h.uptime_s = uptime_s;
  h.free_heap = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_8BIT));
  h.min_free_heap = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  h.largest_block = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  const int reason = static_cast<int>(esp_reset_reason());
  h.reset_reason = reason >= 0 && reason < 256 ? static_cast<uint8_t>(reason) : 0;
  return h;
}

// Appends ,"heap_free":N,"heap_min":N,"heap_max_block":N to a stats-core
// JSON object in `reply` (cap bytes); left unchanged when it would not fit.
inline void appendHeapStats(char* reply, size_t cap) {
  const size_t len = strlen(reply);
  if (len == 0 || reply[len - 1] != '}') return;
  const mesh::observer::Health h = heapHealth();
  char extra[96];
  const int n = snprintf(extra, sizeof(extra), ",\"heap_free\":%lu,\"heap_min\":%lu,\"heap_max_block\":%lu}",
                         static_cast<unsigned long>(h.free_heap), static_cast<unsigned long>(h.min_free_heap),
                         static_cast<unsigned long>(h.largest_block));
  if (n <= 0 || len - 1 + static_cast<size_t>(n) >= cap) return;
  memcpy(reply + len - 1, extra, static_cast<size_t>(n) + 1);
}

}  // namespace esp32
}  // namespace mesh
