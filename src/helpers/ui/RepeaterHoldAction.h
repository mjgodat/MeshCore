#pragma once

#include <stdint.h>
#include <stdio.h>

#include "RadioProfileDisplayPage.h"

namespace mesh { namespace ui {

// Page-dependent long-press ("hold") actions for repeater screens.
//
// Opt-in per board with REPEATER_UI_HOLD_ACTIONS=1. Boards without that flag
// keep long press = power off. With it, a hold never powers the board off:
// the home page sends this repeater's flood advert (the CLI `advert` path) and
// every other page only reports that it has no hold action. A hold never
// changes radio settings.

enum class RepeaterHoldAction : uint8_t {
  None,
  SendAdvert,
};

enum class RepeaterHoldOutcome : uint8_t {
  WakeOnly,          // the screen was off: the hold only woke it
  NoAction,          // current page has no hold action
  AdvertSent,        // advert requested
  AdvertRateLimited, // advert refused, wait_secs says how long to wait
};

// Repeated holds must not spam the mesh with flood adverts.
constexpr uint32_t REPEATER_HOLD_ADVERT_MIN_INTERVAL_MILLIS = 30000;
// How long hold feedback replaces the hint line.
constexpr uint32_t REPEATER_HOLD_FEEDBACK_MILLIS = 3000;
// Same delay the CLI `advert` command uses before the advert goes out.
constexpr int REPEATER_HOLD_ADVERT_DELAY_MILLIS = 1500;

// Only the first page (R1 radio details: name, frequency, battery) is "home".
// The R2 profile page and the system status pages have no hold action.
inline RepeaterHoldAction repeaterHoldActionForPage(bool dual_radio_enabled,
                                                    uint8_t status_page_count,
                                                    uint8_t selected_page) {
  return radioProfileManualPageIndex(dual_radio_enabled, status_page_count,
                                     selected_page) == 0
      ? RepeaterHoldAction::SendAdvert
      : RepeaterHoldAction::None;
}

inline const char* repeaterHoldHint(RepeaterHoldAction action) {
  return action == RepeaterHoldAction::SendAdvert ? "hold: send advert" : nullptr;
}

// Wrap-safe (32-bit millis) minimum spacing between hold adverts.
class RepeaterHoldAdvertLimiter {
  uint32_t _last_millis = 0;
  bool _sent = false;

public:
  uint32_t remainingMillis(uint32_t now) const {
    if (!_sent) return 0;
    const uint32_t elapsed = now - _last_millis;
    return elapsed >= REPEATER_HOLD_ADVERT_MIN_INTERVAL_MILLIS
        ? 0 : REPEATER_HOLD_ADVERT_MIN_INTERVAL_MILLIS - elapsed;
  }

  // Returns true (and starts a new interval) when an advert may go out now.
  bool tryConsume(uint32_t now) {
    if (remainingMillis(now) != 0) return false;
    _sent = true;
    _last_millis = now;
    return true;
  }
};

// Decide and perform one hold. send_advert is invoked at most once.
template <typename SendAdvert>
RepeaterHoldOutcome handleRepeaterHold(bool display_was_on, RepeaterHoldAction action,
                                       RepeaterHoldAdvertLimiter& limiter, uint32_t now,
                                       SendAdvert send_advert, uint32_t* wait_secs) {
  if (wait_secs != nullptr) *wait_secs = 0;
  // A hold on a dark screen acts on a page the user cannot see: only wake.
  if (!display_was_on) return RepeaterHoldOutcome::WakeOnly;
  if (action != RepeaterHoldAction::SendAdvert) return RepeaterHoldOutcome::NoAction;
  const uint32_t remaining = limiter.remainingMillis(now);
  if (remaining != 0 || !limiter.tryConsume(now)) {
    if (wait_secs != nullptr) *wait_secs = (remaining + 999) / 1000;
    return RepeaterHoldOutcome::AdvertRateLimited;
  }
  send_advert();
  return RepeaterHoldOutcome::AdvertSent;
}

// Short e-ink feedback text; empty for WakeOnly.
inline void formatRepeaterHoldFeedback(char* dest, size_t dest_size,
                                       RepeaterHoldOutcome outcome, uint32_t wait_secs) {
  if (dest == nullptr || dest_size == 0) return;
  switch (outcome) {
    case RepeaterHoldOutcome::AdvertSent:
      snprintf(dest, dest_size, "Advert sent");
      break;
    case RepeaterHoldOutcome::AdvertRateLimited:
      snprintf(dest, dest_size, "Advert wait %lus", (unsigned long)wait_secs);
      break;
    case RepeaterHoldOutcome::NoAction:
      snprintf(dest, dest_size, "No hold action");
      break;
    case RepeaterHoldOutcome::WakeOnly:
    default:
      dest[0] = 0;
      break;
  }
}

}}  // namespace mesh::ui
