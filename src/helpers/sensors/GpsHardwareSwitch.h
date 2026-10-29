#pragma once

#include <stdint.h>

namespace mesh {

// Debounced watcher for a physical GPS on/off switch that the firmware must
// obey (the switch is sensed, not a power cut). The first poll reports the
// switch's position so it wins over any saved setting at boot; afterwards
// only a change that stays stable for kDebounceMillis is reported.
class GpsHardwareSwitch {
 public:
  enum class Change : int8_t { None = 0, On = 1, Off = -1 };

  static constexpr uint32_t kDebounceMillis = 100;

  Change poll(bool raw_on, uint32_t now) {
    if (!_started) {
      _started = true;
      _stable = raw_on;
      _candidate = raw_on;
      _candidate_since = now;
      return raw_on ? Change::On : Change::Off;
    }
    if (raw_on != _candidate) {
      _candidate = raw_on;
      _candidate_since = now;
      return Change::None;
    }
    if (_candidate == _stable || now - _candidate_since < kDebounceMillis) {
      return Change::None;
    }
    _stable = _candidate;
    return _stable ? Change::On : Change::Off;
  }

  bool isOn() const { return _stable; }

 private:
  bool _started = false;
  bool _stable = false;
  bool _candidate = false;
  uint32_t _candidate_since = 0;
};

}  // namespace mesh
