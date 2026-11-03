#pragma once

// The repeater's radio as the visual head shows it (pure C++11, host-tested
// by test/test_freenove_tabbar.py). The settings come only from the M5's
// observer vitals: an ESP-NOW-only head has no LoRa radio of its own, so its
// placeholder LoRa preferences are never shown as the mesh's frequency.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <helpers/bridges/ObserverFrame.h>

namespace freenove_ui {

static const char* const kNoRepeaterData = "NO REPEATER DATA";
// Vitals older than this (head clock) are shown dimmed with " (STALE)".
static const uint32_t kRadioStaleMs = 60000;

enum RadioState { kRadioNone, kRadioLive, kRadioStale };

// Vitals carry a usable radio (decoded floats can be anything).
inline bool vitalsHaveRadio(const mesh::observer::Vitals* v) {
  return v != nullptr && isfinite(v->freq) && v->freq > 0.0f && v->freq < 10000.0f
      && isfinite(v->bw) && v->bw > 0.0f && v->bw < 10000.0f && v->sf > 0;
}

// "62.5" / "250": bandwidth in kHz, one decimal only when it has one.
inline void formatBandwidth(char* out, size_t cap, float bw_khz) {
  const long tenths = (long)(bw_khz * 10.0f + 0.5f);
  if (tenths % 10 == 0) snprintf(out, cap, "%ld", tenths / 10);
  else snprintf(out, cap, "%ld.%ld", tenths / 10, tenths % 10);
}

// "M5 910.525 MHZ SF7 BW62.5" (all caps like the rest of the HUD; frequency
// like the CLI's `get radio`, rounded to 3 decimals) or "NO REPEATER DATA". True when the vitals had a radio.
inline bool formatRepeaterRadio(char* out, size_t cap, const mesh::observer::Vitals* v) {
  if (cap == 0) return false;
  if (!vitalsHaveRadio(v)) {
    snprintf(out, cap, "%s", kNoRepeaterData);
    return false;
  }
  char bw[24];   // any long fits (no truncation warning)
  formatBandwidth(bw, sizeof(bw), v->bw);
  snprintf(out, cap, "M5 %.3f MHZ SF%u BW%s", (double)v->freq, (unsigned)v->sf, bw);
  return true;
}

// The HMI header's two parts: "910.525" (MHz, 3 decimals) and
// "SF7 \xB7 BW62.5" (0xB7 = the fonts' middle dot). False, both empty,
// without a usable radio.
inline bool formatRadioParts(char* freq, size_t fcap, char* detail, size_t dcap,
                             const mesh::observer::Vitals* v) {
  if (fcap > 0) freq[0] = 0;
  if (dcap > 0) detail[0] = 0;
  if (!vitalsHaveRadio(v)) return false;
  char bw[24];
  formatBandwidth(bw, sizeof(bw), v->bw);
  if (fcap > 0) snprintf(freq, fcap, "%.3f", (double)v->freq);
  if (dcap > 0) snprintf(detail, dcap, "SF%u \xB7 BW%s", (unsigned)v->sf, bw);
  return true;
}

// Same, aged on the head clock: vitals_ms is when the vitals arrived
// (MeshModel::vitalsMs()); wrap-safe. Stale appends " (STALE)".
inline RadioState formatRepeaterRadio(char* out, size_t cap, const mesh::observer::Vitals* v,
                                      uint32_t vitals_ms, uint32_t now_ms) {
  if (!formatRepeaterRadio(out, cap, v)) return kRadioNone;
  if ((uint32_t)(now_ms - vitals_ms) < kRadioStaleMs) return kRadioLive;
  // Append " (STALE)" as far as it fits (always terminated).
  static const char kSuffix[] = " (STALE)";
  size_t len = 0;
  while (len + 1 < cap && out[len]) len++;
  for (size_t i = 0; kSuffix[i] && len + 1 < cap; i++) out[len++] = kSuffix[i];
  out[len] = 0;
  return kRadioStale;
}

}  // namespace freenove_ui
