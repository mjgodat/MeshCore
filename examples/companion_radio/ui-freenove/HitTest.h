#pragma once

// Tap targets for the visual head's pages (pure C++11, host-tested by
// test/test_observer_demux.py). A page records what it drew that can be
// tapped (node dots, rows) while drawing, then resolves a Tap from PageNav:
// the nearest target centre within max(r, kMinTouchRadius) wins, so small
// dots still get a finger-sized target and neighbours split the difference.

#include <stddef.h>
#include <stdint.h>

namespace freenove_ui {

static const int kMinTouchRadius = 14;   // px, ~6 mm on the 3.5" panel

struct HitTarget {
  int16_t x, y, r;
  uint16_t id;
};

template <size_t N>
class HitList {
 public:
  HitList() : _n(0) {}
  void clear() { _n = 0; }
  size_t size() const { return _n; }
  // False when full (the target is not recorded).
  bool add(int x, int y, int r, uint16_t id) {
    if (_n >= N) return false;
    HitTarget& t = _t[_n++];
    t.x = (int16_t)x; t.y = (int16_t)y; t.r = (int16_t)(r < 0 ? 0 : r); t.id = id;
    return true;
  }
  // Id of the nearest target under (x, y), -1 for none.
  int find(int x, int y) const {
    int best = -1;
    long bestD = 0;
    for (size_t i = 0; i < _n; i++) {
      const long dx = x - _t[i].x, dy = y - _t[i].y;
      const long r = _t[i].r > kMinTouchRadius ? _t[i].r : kMinTouchRadius;
      const long d = dx * dx + dy * dy;
      if (d > r * r) continue;
      if (best < 0 || d < bestD) { best = _t[i].id; bestD = d; }
    }
    return best;
  }

 private:
  HitTarget _t[N];
  size_t _n;
};

}  // namespace freenove_ui
