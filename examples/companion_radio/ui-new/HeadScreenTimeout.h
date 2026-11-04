#pragma once

// Screen timeout choices of the repeater heads (UI_REPEATER_HEAD: CYD and
// Freenove), pure C++11, host-tested by test/test_head_screen_timeout.py.
//
// The heads are mostly on USB power next to the repeater: the screen stays
// on for kDefaultSeconds (2 min) after a touch, and the companion home
// carousel's "Screen" page cycles 30 s -> 1 min -> 2 min -> 5 min -> Never.
// The choice is the companion's display power profile (display.timeout /
// display.mode, both the battery and the USB profile), saved by
// DisplayPowerSettings. "Never" is mode on; picking a timeout again goes
// back to the button mode (a touch wakes the screen). Night mode is separate
// and unaffected. A touch on a dark screen only wakes it.

#include <stdint.h>
#include <stdio.h>

#include <helpers/ui/DisplayPowerPolicy.h>

namespace head_screen {

static const uint16_t kDefaultSeconds = 120;
static const int kChoices = 5;
// 0 = Never.
static const uint16_t kSeconds[kChoices] = {30, 60, 120, 300, 0};

// The choice a profile corresponds to: Never for mode on, else the nearest
// timeout at or above it (a value set over the CLI lands on a choice).
inline int choiceOf(const mesh::ui::DisplayPowerProfile& p) {
  if (p.mode == mesh::ui::DisplayMode::On) return kChoices - 1;
  for (int i = 0; i < kChoices - 1; i++)
    if (p.seconds <= kSeconds[i]) return i;
  return kChoices - 2;
}

inline int nextChoice(int choice) { return choice < 0 || choice >= kChoices - 1 ? 0 : choice + 1; }

// "30 s", "1 min", "2 min", "5 min", "Never".
inline void label(char* out, size_t cap, int choice) {
  if (choice < 0 || choice >= kChoices) choice = 2;
  const uint16_t s = kSeconds[choice];
  if (s == 0) snprintf(out, cap, "Never");
  else if (s < 60) snprintf(out, cap, "%u s", (unsigned)s);
  else snprintf(out, cap, "%u min", (unsigned)(s / 60));
}

// The two "set" commands (DisplayPowerSettings) that apply a choice to both
// profiles: a mode, then (when timed) the timeout; usb == true for the USB
// profile. Returns the number of commands (2).
inline int commands(int choice, bool usb, char (*out)[40]) {
  const uint16_t s = kSeconds[choice < 0 || choice >= kChoices ? 2 : choice];
  const char* scope = usb ? "display.usb" : "display";
  snprintf(out[0], 40, "set %s.mode %s", scope, s == 0 ? "on" : "button");
  if (s == 0) {
    out[1][0] = 0;
    return 1;
  }
  snprintf(out[1], 40, "set %s.timeout %u", scope, (unsigned)s);
  return 2;
}

// A profile with `choice` applied: Never = mode on (the timeout kept);
// a timeout = button mode (from on / automatic too) and that timeout.
inline mesh::ui::DisplayPowerProfile applied(const mesh::ui::DisplayPowerProfile& p, int choice) {
  mesh::ui::DisplayPowerProfile out = p;
  const uint16_t s = kSeconds[choice < 0 || choice >= kChoices ? 2 : choice];
  if (s == 0) {
    out.mode = mesh::ui::DisplayMode::On;
  } else {
    if (out.mode != mesh::ui::DisplayMode::Off) out.mode = mesh::ui::DisplayMode::Button;
    out.seconds = s;
  }
  return out;
}

// One-time move of an older head's short timeout (the companion default of
// 15 s) to kDefaultSeconds: only a timed profile below it, never one the
// owner set to on / off / a longer time.
inline bool needsMigration(const mesh::ui::DisplayPowerProfile& p) {
  return (p.mode == mesh::ui::DisplayMode::Button || p.mode == mesh::ui::DisplayMode::Automatic) &&
         p.seconds < kDefaultSeconds;
}

}  // namespace head_screen
