#pragma once

// Force-directed layout for the HOME topology view (pure, header-only,
// C++11; host-tested by test/test_layouts.py).
//
// Slot 0 is the repeater, fixed at the centre of the area. Other slots are
// nodes keyed by the first 4 bytes of their public key. Each step applies
//   - repulsion between every pair (k^2 / d, Fruchterman-Reingold),
//   - a spring along every link (rest length ~k),
//   - a weak radial pull toward a ring by hop count (more hops = farther
//     out), so nodes without known links still read as a topology,
// and moves each node by at most its own temperature, which cools
// geometrically. A new node starts hot (kHotPx) and settles in ~120 steps;
// existing nodes are only warmed slightly (kWarmPx), so adding a node never
// reshuffles the picture (total drift <= kWarmPx / (1 - kCooling) ~ 10 px).
// Positions are clamped to the area minus kMarginPx: dots never leave it.

#include <math.h>
#include <stdint.h>

namespace freenove_ui {

class GraphLayout {
 public:
  static const int kMaxNodes = 32;     // slot 0 = the repeater
  static const int kMaxLinks = 96;
  static constexpr float kHotPx = 12.0f;      // a new node's first step limit
  static constexpr float kWarmPx = 0.3f;      // others when a node is added
  static constexpr float kCooling = 0.97f;    // temperature factor per step
  static constexpr float kSettledPx = 0.25f;  // below this nothing visibly moves
  static constexpr float kMarginPx = 8.0f;

  explicit GraphLayout(float w = 340.0f, float h = 270.0f) { reset(w, h); }

  void reset(float w, float h) {
    _w = w > 2 * kMarginPx + 1 ? w : 2 * kMarginPx + 1;
    _h = h > 2 * kMarginPx + 1 ? h : 2 * kMarginPx + 1;
    for (int i = 0; i < kMaxNodes; i++) {
      _used[i] = false; _key[i] = 0; _hops[i] = 0; _x[i] = _y[i] = 0; _temp[i] = 0;
    }
    _used[0] = true;
    _x[0] = _w / 2; _y[0] = _h / 2;
    _links = 0;
  }

  float width() const { return _w; }
  float height() const { return _h; }
  float x(int i) const { return valid(i) ? _x[i] : 0.0f; }
  float y(int i) const { return valid(i) ? _y[i] : 0.0f; }
  uint32_t key(int i) const { return valid(i) ? _key[i] : 0; }
  bool used(int i) const { return valid(i); }
  // Nodes besides the repeater.
  int count() const {
    int n = 0;
    for (int i = 1; i < kMaxNodes; i++) n += _used[i] ? 1 : 0;
    return n;
  }

  int find(uint32_t key) const {
    for (int i = 1; i < kMaxNodes; i++) if (_used[i] && _key[i] == key) return i;
    return -1;
  }

  // Adds a node (or returns its slot) near `anchor` (a slot) or, without
  // one, on its hop ring at an angle derived from the key. -1 when full.
  int add(uint32_t key, int hops, int anchor = -1) {
    int s = find(key);
    if (s >= 0) { _hops[s] = clampHops(hops); return s; }
    for (int i = 1; i < kMaxNodes; i++) {
      if (_used[i]) continue;
      s = i;
      break;
    }
    if (s < 0) return -1;
    _used[s] = true;
    _key[s] = key;
    _hops[s] = clampHops(hops);
    const float a = (float)(mix(key) % 3600u) * (3.14159265f / 1800.0f);
    if (valid(anchor)) {
      _x[s] = _x[anchor] + 28.0f * cosf(a);
      _y[s] = _y[anchor] + 28.0f * sinf(a);
    } else {
      const float r = ringRadius(_hops[s]);
      _x[s] = _w / 2 + r * cosf(a);
      _y[s] = _h / 2 + r * sinf(a);
    }
    clampPos(s);
    _temp[s] = kHotPx;
    for (int i = 1; i < kMaxNodes; i++) if (_used[i] && i != s && _temp[i] < kWarmPx) _temp[i] = kWarmPx;
    return s;
  }

  void remove(uint32_t key) {
    const int s = find(key);
    if (s < 0) return;
    _used[s] = false;
    int w = 0;
    for (int i = 0; i < _links; i++) {
      if (_la[i] == s || _lb[i] == s) continue;
      _la[w] = _la[i]; _lb[w] = _lb[i]; w++;
    }
    _links = w;
  }

  void clearLinks() { _links = 0; }
  int linkCount() const { return _links; }
  int linkA(int i) const { return _la[i]; }
  int linkB(int i) const { return _lb[i]; }
  bool linked(int a, int b) const {
    for (int i = 0; i < _links; i++)
      if ((_la[i] == a && _lb[i] == b) || (_la[i] == b && _lb[i] == a)) return true;
    return false;
  }
  // Undirected, deduplicated. False when invalid or full.
  bool link(int a, int b) {
    if (!valid(a) || !valid(b) || a == b) return false;
    if (linked(a, b)) return true;
    if (_links >= kMaxLinks) return false;
    _la[_links] = (uint8_t)a; _lb[_links] = (uint8_t)b; _links++;
    return true;
  }

  // Let a node move again (e.g. it gained a link): up to px per step, cooling.
  void warm(int i, float px) {
    if (valid(i) && i != 0 && _temp[i] < px) _temp[i] = px;
  }

  // Nodes still moving visibly.
  bool hot() const {
    for (int i = 1; i < kMaxNodes; i++) if (_used[i] && _temp[i] > kSettledPx) return true;
    return false;
  }

  // One iteration; returns the largest movement (px).
  float step() {
    const int n = count() + 1;
    const float area = _w * _h;
    float k = sqrtf(area / (float)n) * 0.42f;
    if (k < 24.0f) k = 24.0f;
    if (k > 64.0f) k = 64.0f;
    const float k2 = k * k;
    float fx[kMaxNodes], fy[kMaxNodes];
    for (int i = 0; i < kMaxNodes; i++) { fx[i] = 0; fy[i] = 0; }

    for (int i = 1; i < kMaxNodes; i++) {
      if (!_used[i]) continue;
      for (int j = 0; j < kMaxNodes; j++) {
        if (j == i || !_used[j]) continue;
        float dx = _x[i] - _x[j], dy = _y[i] - _y[j];
        float d2 = dx * dx + dy * dy;
        if (d2 < 0.01f) {   // coincident: a fixed, slot-dependent nudge
          dx = (float)((i * 7 + j * 3) % 5) - 2.0f + 0.5f;
          dy = (float)((i * 3 + j * 7) % 5) - 2.0f + 0.5f;
          d2 = dx * dx + dy * dy;
        }
        if (d2 < 4.0f) d2 = 4.0f;
        const float f = k2 / d2;   // (k^2 / d) along the unit vector
        fx[i] += dx * f;
        fy[i] += dy * f;
      }
      // Radial pull toward the hop ring, plus a little gravity.
      const float cx = _x[i] - _w / 2, cy = _y[i] - _h / 2;
      const float r = sqrtf(cx * cx + cy * cy);
      if (r > 0.5f) {
        const float pull = (ringRadius(_hops[i]) - r) * 0.08f;
        fx[i] += cx / r * pull;
        fy[i] += cy / r * pull;
      }
      fx[i] -= cx * 0.02f;
      fy[i] -= cy * 0.02f;
    }
    for (int l = 0; l < _links; l++) {
      const int a = _la[l], b = _lb[l];
      const float dx = _x[b] - _x[a], dy = _y[b] - _y[a];
      const float d = sqrtf(dx * dx + dy * dy);
      if (d < 0.01f) continue;
      const float f = (d - k) * 0.6f / d;
      fx[a] += dx * f; fy[a] += dy * f;
      fx[b] -= dx * f; fy[b] -= dy * f;
    }

    float moved = 0;
    for (int i = 1; i < kMaxNodes; i++) {
      if (!_used[i]) continue;
      const float f = sqrtf(fx[i] * fx[i] + fy[i] * fy[i]);
      float mx = 0, my = 0;
      if (f > 0.0001f) {
        const float s = (f < _temp[i] ? f : _temp[i]) / f;
        mx = fx[i] * s; my = fy[i] * s;
      }
      const float ox = _x[i], oy = _y[i];
      _x[i] += mx; _y[i] += my;
      clampPos(i);
      const float ddx = _x[i] - ox, ddy = _y[i] - oy;
      const float m = sqrtf(ddx * ddx + ddy * ddy);
      if (m > moved) moved = m;
      _temp[i] *= kCooling;
    }
    return moved;
  }

 private:
  bool valid(int i) const { return i >= 0 && i < kMaxNodes && _used[i]; }
  static int clampHops(int h) { return h < 0 ? 0 : (h > 3 ? 3 : h); }
  // Ring radius for a hop count: 0 hops = inner ring, 3+ = near the edge.
  float ringRadius(int hops) const {
    const float half = (_w < _h ? _w : _h) / 2 - kMarginPx;
    return half * (float)(hops + 1) / 4.6f;
  }
  static uint32_t mix(uint32_t v) {
    v ^= v >> 16; v *= 0x7feb352dU; v ^= v >> 15; v *= 0x846ca68bU; v ^= v >> 16;
    return v;
  }
  void clampPos(int i) {
    if (_x[i] < kMarginPx) _x[i] = kMarginPx;
    if (_x[i] > _w - kMarginPx) _x[i] = _w - kMarginPx;
    if (_y[i] < kMarginPx) _y[i] = kMarginPx;
    if (_y[i] > _h - kMarginPx) _y[i] = _h - kMarginPx;
  }

  float _w, _h;
  bool _used[kMaxNodes];
  uint32_t _key[kMaxNodes];
  uint8_t _hops[kMaxNodes];
  float _x[kMaxNodes], _y[kMaxNodes], _temp[kMaxNodes];
  uint8_t _la[kMaxLinks], _lb[kMaxLinks];
  int _links;
};

}  // namespace freenove_ui
