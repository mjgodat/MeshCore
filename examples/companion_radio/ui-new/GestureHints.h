#pragma once

// "Ghost" gesture hints for the repeater heads (CYD and Freenove), pure
// C++11, host-tested by test/test_gesture_hints.py.
//
// A faint chevron at the edge where more content is (left / right on the
// companion home carousel, up / down on a scrollable list) tells a new user
// that the screen swipes or scrolls. It is shown only when there is more in
// that direction, is bright for kBrightMs after the screen appears or moves,
// then settles to a ghost level (no animation beyond that one step). Every
// chevron is drawn on a visible tile that is exactly its tap target and does
// what the swipe / scroll does.

#include <stdint.h>

namespace gesture_hints {

static const uint32_t kBrightMs = 1500;

enum Level { kHidden, kGhost, kBright };

// The level of a chevron: hidden without more content that way, bright for
// kBrightMs after `changed_ms` (screen shown, page turned, list scrolled).
inline Level level(bool more, uint32_t now_ms, uint32_t changed_ms) {
  if (!more) return kHidden;
  return (uint32_t)(now_ms - changed_ms) < kBrightMs ? kBright : kGhost;
}

// Milliseconds until a bright chevron settles (0 when it already has): the
// screen's next refresh, so the step happens on time.
inline uint32_t settleMs(uint32_t now_ms, uint32_t changed_ms) {
  const uint32_t age = now_ms - changed_ms;
  return age < kBrightMs ? kBrightMs - age : 0;
}

struct Rect {
  int x, y, w, h;
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// The home carousel's side tiles: the Previous / Next touch zones
// (TouchInput, TOUCH_CENTER_ZONE_PERCENT; TouchDebugOverlay.h has the same
// rule) from `top` down. Above `top` (the title bar) the zones stay as they
// were: the title bar is a drawn control of its own.
inline void carouselZones(int width, int height, int center_zone_percent, int top, Rect& left, Rect& right) {
  const int side = width * ((100 - center_zone_percent) / 2) / 100;
  left = {0, top, side, height - top};
  right = {width - side, top, side, height - top};
}

// A list's up / down tiles in a column at x (w wide) between top and bottom
// (exclusive): `tile` rows each, at the column's ends.
inline void listTiles(int x, int w, int top, int bottom, int tile, Rect& up, Rect& down) {
  up = {x, top, w, tile};
  down = {x, bottom - tile, w, tile};
}

}  // namespace gesture_hints
