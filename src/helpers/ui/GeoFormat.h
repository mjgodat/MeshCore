#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace mesh {
namespace ui {

// Format a coordinate given in microdegrees (the LocationProvider unit) as
// degrees/minutes/seconds, e.g. "N47 36'22.1\"" or "W122 19'55.4\"".
// Integer arithmetic only: seconds are rounded to tenths, carrying into
// minutes and degrees, so 59.95" never prints as 60.0".
inline int formatMicrodegreesDms(char* out, size_t size, long microdegrees,
                                 bool is_latitude) {
  const char hemisphere = is_latitude ? (microdegrees < 0 ? 'S' : 'N')
                                      : (microdegrees < 0 ? 'W' : 'E');
  const uint64_t magnitude = microdegrees < 0
      ? (uint64_t)(-(int64_t)microdegrees) : (uint64_t)microdegrees;
  uint32_t degrees = (uint32_t)(magnitude / 1000000ULL);
  const uint64_t fraction = magnitude % 1000000ULL;      // microdegrees
  // 1 degree = 36000 tenths of an arc-second.
  uint32_t tenths = (uint32_t)((fraction * 36000ULL + 500000ULL) / 1000000ULL);
  if (tenths >= 36000) {
    tenths -= 36000;
    degrees++;
  }
  const uint32_t minutes = tenths / 600;
  const uint32_t second_tenths = tenths % 600;
  return snprintf(out, size, "%c%u %02u'%02u.%u\"", hemisphere, (unsigned)degrees,
                  (unsigned)minutes, (unsigned)(second_tenths / 10),
                  (unsigned)(second_tenths % 10));
}

}  // namespace ui
}  // namespace mesh
