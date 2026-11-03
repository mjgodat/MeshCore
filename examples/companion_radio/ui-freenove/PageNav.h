#pragma once

// Page navigation for the Freenove visual head (pure C++11, host-tested by
// test/test_observer_demux.py and test/test_freenove_tabbar.py).
//
// PageNav turns raw touch samples (down, x, y, time) into page changes and
// the touch events a page receives:
//   - tab bar: a labelled bar (kTabBarH px) along the bottom of every page,
//     one equal-width tab per page. A tap on a tab jumps to that page (never
//     forwarded); a horizontal swipe that starts in the bar turns the page
//     (the only swipe on pass-through pages);
//   - horizontal swipe above the bar: left = next page, right = previous (no
//     wrap). A swipe turns the page when it travels swipeMinPx, or flickMinPx
//     at flickMinPxPerS or faster (speed from press to the last moving
//     sample, so the touch driver's release hold-over does not slow it down);
//   - tap (press + release without moving slopPx): Tap at the press point;
//   - vertical drag: Press / Move / Release forwarded to the page.
// Pass-through pages (the HEAD page, where the companion UI owns everything
// above the bar) receive every gesture that starts above the bar as
// Press / Move / Release; gestures that start in the bar belong to PageNav.
//
// TouchRelay hands touches to a consumer that polls (the companion UI reads
// DisplayDriver::getTouch every 25 ms): a press that began and ended between
// two polls is still reported once, so short taps are never lost.

#include <stdint.h>
#include <stdlib.h>

#include "HmiProfile.h"

namespace freenove_ui {

// Height of the tab bar at the bottom of the screen (24 on the 480x320
// Freenove, 30 on the 320x240 CYD); the HEAD page's companion UI gets the
// rows above it.
static const int kTabBarH = profile::kTabBarH;

struct NavConfig {
  int width, height;
  int slopPx;           // movement before a press becomes a drag/swipe
  int swipeMinPx;       // horizontal travel that always turns the page
  int flickMinPx;       // shorter travel turns it when fast enough...
  int flickMinPxPerS;   // ...at this speed
  int barH;             // tab bar height (bottom of the screen)
};

inline NavConfig defaultNavConfig() {
  NavConfig c;
  c.width = profile::kScreenW; c.height = profile::kScreenH;
#if HMI_CYD
  // Resistive touch jitters more; the screen is narrower.
  c.slopPx = 14;
  c.swipeMinPx = 48;
  c.flickMinPx = 24;
#else
  c.slopPx = 12;
  c.swipeMinPx = 60;
  c.flickMinPx = 24;
#endif
  c.flickMinPxPerS = 400;
  c.barH = kTabBarH;
  return c;
}

struct NavEvent {
  enum Kind { None, PageChanged, Press, Move, Release, Tap };
  Kind kind;
  int x, y;
};

class PageNav {
 public:
  explicit PageNav(int count, int first = 0, const NavConfig& cfg = defaultNavConfig())
      : _cfg(cfg), _count(count > 0 ? count : 1), _cur(0), _mode(Idle), _tab0(-1),
        _x0(0), _y0(0), _lx(0), _ly(0), _t0(0), _tMove(0) {
    setCurrent(first);
  }

  int count() const { return _count; }
  int current() const { return _cur; }
  const NavConfig& config() const { return _cfg; }
  void setCurrent(int i) { _cur = i < 0 ? 0 : (i >= _count ? _count - 1 : i); }

  // Ignore the rest of the gesture in progress (e.g. the touch that woke
  // the screen).
  void swallowGesture() { if (_mode != Idle) _mode = Swallow; }

  // Tab bar geometry: rows [barY(), height), tab i spans [tabX(i), tabX(i+1)).
  int barY() const { return _cfg.height - _cfg.barH; }
  int tabX(int i) const { return i * _cfg.width / _count; }
  // Index of the tab under (x, y), -1 when outside the bar.
  int tabAt(int x, int y) const {
    if (y < barY() || y >= _cfg.height || x < 0 || x >= _cfg.width) return -1;
    const int i = x * _count / _cfg.width;
    return i < _count ? i : _count - 1;
  }

  // passThrough: the page takes every gesture that starts above the bar.
  NavEvent update(bool down, int x, int y, uint32_t now_ms, bool passThrough) {
    if (!down) return release();
    if (_mode == Idle) return press(x, y, now_ms, passThrough);
    const int dx = x - _x0, dy = y - _y0;
    if (x != _lx || y != _ly) _tMove = now_ms;
    _lx = x; _ly = y;
    switch (_mode) {
      case Pending:
        if (abs(dx) >= _cfg.slopPx && abs(dx) > abs(dy)) {
          _mode = Swipe;
        } else if (abs(dy) >= _cfg.slopPx) {
          _mode = Drag;
          return event(NavEvent::Press, _x0, _y0);
        }
        return none();
      case Tabs:
        if (abs(dx) >= _cfg.slopPx && abs(dx) > abs(dy)) _mode = Swipe;
        return none();
      case Drag:
      case Pass:
        return event(NavEvent::Move, x, y);
      default:
        return none();
    }
  }

 private:
  enum Mode { Idle, Pending, Swipe, Drag, Tabs, Pass, Swallow };

  static NavEvent event(NavEvent::Kind k, int x, int y) { NavEvent e; e.kind = k; e.x = x; e.y = y; return e; }
  static NavEvent none() { return event(NavEvent::None, -1, -1); }

  NavEvent press(int x, int y, uint32_t now_ms, bool passThrough) {
    _x0 = _lx = x; _y0 = _ly = y; _t0 = _tMove = now_ms;
    _tab0 = tabAt(x, y);
    if (_tab0 >= 0) { _mode = Tabs; return none(); }
    if (passThrough) { _mode = Pass; return event(NavEvent::Press, x, y); }
    _mode = Pending;
    return none();
  }

  NavEvent release() {
    const Mode m = _mode;
    _mode = Idle;
    switch (m) {
      case Pending: return event(NavEvent::Tap, _x0, _y0);
      case Drag:
      case Pass: return event(NavEvent::Release, _lx, _ly);
      case Tabs: {
        // Released on the tab it was pressed on (a slip off it cancels).
        const int i = tabAt(_lx, _ly);
        if (i < 0 || i != _tab0 || i == _cur) return none();
        _cur = i;
        return event(NavEvent::PageChanged, _lx, _ly);
      }
      case Swipe: {
        const int dx = _lx - _x0;
        const int dist = abs(dx);
        uint32_t dt = _tMove - _t0;
        if (dt == 0) dt = 1;
        const bool far = dist >= _cfg.swipeMinPx;
        const bool flick = dist >= _cfg.flickMinPx
            && (uint32_t)dist * 1000u >= (uint32_t)_cfg.flickMinPxPerS * dt;
        if (!far && !flick) return none();
        const int next = _cur + (dx < 0 ? 1 : -1);
        if (next < 0 || next >= _count) return none();
        _cur = next;
        return event(NavEvent::PageChanged, _lx, _ly);
      }
      default: return none();
    }
  }

  NavConfig _cfg;
  int _count, _cur;
  Mode _mode;
  int _tab0;
  int _x0, _y0, _lx, _ly;
  uint32_t _t0, _tMove;
};

// Resistive touch (the CYD's XPT2046) drops samples mid-press and bounces
// on contact. TouchDebounce turns raw samples into a stable state: a press
// counts after `press` consecutive down samples, a release after `release`
// consecutive up samples; a dropped sample inside a press is bridged (the
// press stays down at its last position). press = release = 1 is the raw
// state (the Freenove's capacitive panel).
class TouchDebounce {
 public:
  explicit TouchDebounce(int press = 1, int release = 1)
      : _press(press < 1 ? 1 : press), _release(release < 1 ? 1 : release), _down(false), _run(0), _x(-1), _y(-1) {}
  // One raw sample; returns the debounced state and its position in x, y
  // (the last down sample while down; -1, -1 when up).
  bool update(bool raw, int& x, int& y) {
    if (raw == _down) {
      _run = 0;
    } else if (++_run >= (raw ? _press : _release)) {
      _down = raw;
      _run = 0;
    }
    if (raw && _down) { _x = x; _y = y; }
    if (_down) { x = _x; y = _y; }
    else { x = y = -1; }
    return _down;
  }
  bool down() const { return _down; }

 private:
  int _press, _release;
  bool _down;
  int _run;
  int _x, _y;
};

class TouchRelay {
 public:
  TouchRelay() : _down(false), _seen(true), _x(-1), _y(-1) {}
  void set(bool down, int x, int y) {
    if (down) {
      if (!_down) _seen = false;
      _down = true; _x = x; _y = y;
    } else {
      _down = false;
    }
  }
  // Down (with the last position) while pressed, and once more for a press
  // no read has seen yet.
  bool read(int& x, int& y) {
    if (!_down && _seen) return false;
    _seen = true;
    x = _x; y = _y;
    return true;
  }
  void reset() { _down = false; _seen = true; }

 private:
  bool _down, _seen;
  int _x, _y;
};

}  // namespace freenove_ui
