#pragma once

// The radio an ESP-NOW-only head shows on its Network/Radio screens (pure
// C++11, host-tested by test/test_freenove_tabbar.py).
//
// A MESH_PRIMARY_ESPNOW head has no LoRa radio: its LoRa preferences
// (freq/bw/sf/cr) are a placeholder (e.g. 869.6 MHz) and must never be shown
// as the mesh's frequency. The head shows the repeater's radio instead when a
// source knows it (the Freenove visual head registers one fed by the M5's
// observer vitals), else just "ESP-NOW".

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace mesh {
namespace ui {

struct RepeaterRadio {
  float freq;   // MHz
  float bw;     // kHz
  uint8_t sf;
  uint8_t cr;
};

// Fills `out` and returns true when the repeater's radio is known. Called
// from the companion UI loop; must be cheap and thread-safe.
typedef bool (*RepeaterRadioSource)(RepeaterRadio& out);

inline RepeaterRadioSource& repeaterRadioSourceSlot() {
  static RepeaterRadioSource source = nullptr;
  return source;
}
// Register once at start-up, before the UI loop reads it.
inline void setRepeaterRadioSource(RepeaterRadioSource source) { repeaterRadioSourceSlot() = source; }

inline bool repeaterRadio(RepeaterRadio& out) {
  const RepeaterRadioSource source = repeaterRadioSourceSlot();
  return source != nullptr && source(out);
}

// Two lines for the head's radio screen:
//   known : "Repeater 910.525 MHz" / "SF7 BW62.5 CR5"
//   else  : "Radio: ESP-NOW"       / "no repeater data"
inline void formatEspNowHeadRadio(char* line1, size_t cap1, char* line2, size_t cap2,
                                  const RepeaterRadio* rpt) {
  if (rpt == nullptr) {
    if (cap1) snprintf(line1, cap1, "Radio: ESP-NOW");
    if (cap2) snprintf(line2, cap2, "no repeater data");
    return;
  }
  const long tenths = (long)(rpt->bw * 10.0f + 0.5f);
  char bw[24];   // any long fits (no truncation warning)
  if (tenths % 10 == 0) snprintf(bw, sizeof(bw), "%ld", tenths / 10);
  else snprintf(bw, sizeof(bw), "%ld.%ld", tenths / 10, tenths % 10);
  if (cap1) snprintf(line1, cap1, "Repeater %.3f MHz", (double)rpt->freq);
  if (cap2) {
    if (rpt->cr) snprintf(line2, cap2, "SF%u BW%s CR%u", (unsigned)rpt->sf, bw, (unsigned)rpt->cr);
    else snprintf(line2, cap2, "SF%u BW%s", (unsigned)rpt->sf, bw);
  }
}

// Same, from the registered source.
inline void formatEspNowHeadRadio(char* line1, size_t cap1, char* line2, size_t cap2) {
  RepeaterRadio rpt;
  formatEspNowHeadRadio(line1, cap1, line2, cap2, repeaterRadio(rpt) ? &rpt : nullptr);
}

}  // namespace ui
}  // namespace mesh
