#pragma once

// GxEPD2 BUSY waits (GxEPDDisplay's busy callback). Pure C++11, host-tested
// (test/test_eink_busy_watch.py).
//
// GxEPD2 busy-waits on the panel's BUSY line, up to 10 s per wait, and a
// refresh can make several waits. A healthy full refresh takes 2-3 s. With a
// stuck BUSY line (a hardware fault) each refresh would hold the main loop
// for 30 s or more, which the ESP32 repeater's loop watchdog would turn into
// a reset at every refresh. Instead the callback feeds that watchdog, this
// class notices a wait longer than kStuckMs, and GxEPDDisplay then skips
// refreshes for kPauseMs (logged once per stuck event): the node stays up
// and keeps repeating; only its screen goes stale.

#include <stdint.h>

namespace mesh {
namespace ui {

class EinkBusyWatch {
public:
  static const uint32_t kStuckMs = 5000;            // a healthy wait is under 3 s
  static const uint32_t kPauseMs = 10UL * 60000UL;  // no refresh this long after one
  static const uint32_t kNewWaitGapMs = 100;        // callbacks come about every 1 ms

  // From the busy callback, repeatedly while BUSY is asserted.
  void onBusy(uint32_t now_ms) {
    if (!_in_wait || now_ms - _last_ms > kNewWaitGapMs) {
      _in_wait = true;
      _wait_start = now_ms;
      _flagged = false;
    }
    _last_ms = now_ms;
    if (!_flagged && now_ms - _wait_start >= kStuckMs) {
      _flagged = true;
      ++_stuck;
      _event = true;
      _paused = true;
      _paused_until = now_ms + kPauseMs;
    }
  }

  // False while refreshes are paused after a stuck wait.
  bool refreshAllowed(uint32_t now_ms) {
    if (_paused && static_cast<int32_t>(now_ms - _paused_until) < 0) return false;
    _paused = false;
    return true;
  }

  // True once per stuck event (for one log line).
  bool takeStuckEvent() {
    const bool e = _event;
    _event = false;
    return e;
  }
  uint32_t stuckCount() const { return _stuck; }

private:
  uint32_t _wait_start = 0, _last_ms = 0, _paused_until = 0, _stuck = 0;
  bool _in_wait = false, _flagged = false, _paused = false, _event = false;
};

}  // namespace ui
}  // namespace mesh
