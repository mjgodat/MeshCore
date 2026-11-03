#pragma once

// HMI behaviour of the visual head's HOME page that is not drawing (pure
// C++11, header-only; host-tested by test/test_hmi_logic.py):
//   Odometer        rolling packet counter digits
//   Needle          damped noise-floor needle (moves only to new targets)
//   noiseRating     quiet / moderate / noisy thresholds and the gauge scale
//   rateTrend       packets/min against the previous five minutes
//   QuickControls   tile taps: two-step confirm for actions that transmit
//   ClockTrack      is the head's RTC set this session? (else show uptime)
//   formatClock     "13:42" + "31 Oct UTC", or "T+05:12" + "UPTIME"
//   linkLevel       observer-link icon arcs from data freshness

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace freenove_ui {
namespace hmi {

// ------------------------------------------------------------ odometer

class Odometer {
 public:
  static const uint32_t kRollMs = 400;   // one change rolls this long (60 fps meanwhile)
  static const uint32_t kModulo = 1000000;   // six digits

  Odometer() : _from(0), _to(0), _start(0), _set(false) {}

  // New count: rolls from what is shown now; the first value, a drop
  // (restart, wrap past 999999) and a jump of 1000+ appear without rolling.
  void set(uint32_t value, uint32_t now_ms) {
    const uint32_t v = value % kModulo;
    if (_set && v == _to) return;
    const double cur = shown(now_ms);
    if (!_set || v < _to || v - _to >= 1000) {
      _from = v;
    } else {
      _from = cur;
    }
    _to = v;
    _start = now_ms;
    _set = true;
  }
  // The value on the dials (fractional while rolling). Ease-out.
  double shown(uint32_t now_ms) const {
    if (!_set) return 0.0;
    const uint32_t t = now_ms - _start;
    if (t >= kRollMs || _from == (double)_to) return (double)_to;
    const double f = (double)t / (double)kRollMs;
    const double e = 1.0 - (1.0 - f) * (1.0 - f);
    return _from + ((double)_to - _from) * e;
  }
  bool rolling(uint32_t now_ms) const { return _set && _from != (double)_to && now_ms - _start < kRollMs; }
  uint32_t target() const { return _to; }

  // Digit at `place` (0 = units) of v, and how far it has rolled towards the
  // next digit (0..1). Like a mechanical counter, a digit turns only while
  // every digit below it is passing 9 -> 0.
  static void digit(double v, int place, int& d, float& roll) {
    if (v < 0) v = 0;
    double unit = 1.0;
    for (int i = 0; i < place; i++) unit *= 10.0;
    const double whole = floor(v / unit);
    d = (int)fmod(whole, 10.0);
    const double within = v - whole * unit;   // 0 .. unit
    double r = within - (unit - 1.0);         // the last unit step of this place
    if (r < 0) r = 0;
    if (r > 1) r = 1;
    roll = (float)r;
  }

 private:
  double _from;
  uint32_t _to;
  uint32_t _start;
  bool _set;
};

// ------------------------------------------------------------ needle

// The needle eases toward its target (exponential, kTauMs); only a new
// target moves it. Packets never set targets -- vitals do -- so it never
// swings per packet.
class Needle {
 public:
  static const uint32_t kTauMs = 350;

  Needle() : _pos(0), _target(0), _ms(0), _has(false) {}
  void setTarget(float v, uint32_t now_ms) {
    if (!_has) {   // first reading: no sweep up from nothing
      _pos = v;
      _ms = now_ms;
      _has = true;
    }
    _target = v;
  }
  bool has() const { return _has; }
  float target() const { return _target; }
  // Advance to now_ms and return the position.
  float update(uint32_t now_ms) {
    if (!_has) return 0;
    const uint32_t dt = now_ms - _ms;
    _ms = now_ms;
    if (dt > 0 && dt < 60000) {
      const float k = expf(-(float)dt / (float)kTauMs);
      _pos = _target + (_pos - _target) * k;
    } else if (dt >= 60000) {
      _pos = _target;
    }
    if (fabsf(_pos - _target) < 0.05f) _pos = _target;
    return _pos;
  }
  float position() const { return _pos; }
  bool settling() const { return _has && _pos != _target; }

 private:
  float _pos, _target;
  uint32_t _ms;
  bool _has;
};

// ------------------------------------------------------------ noise floor

// Noise floor ratings (dBm, the repeater's own estimate): an SX1262 at
// 62.5-250 kHz on a quiet site reads about -120..-110; local interference
// (switching supplies, a nearby transmitter) lifts it. quiet <= -108,
// moderate -107..-96, noisy >= -95.
enum NoiseRating { kNoiseQuiet, kNoiseModerate, kNoiseNoisy };
static const int kQuietMaxDbm = -108;
static const int kNoisyMinDbm = -95;
inline NoiseRating noiseRating(int dbm) {
  if (dbm <= kQuietMaxDbm) return kNoiseQuiet;
  if (dbm < kNoisyMinDbm) return kNoiseModerate;
  return kNoiseNoisy;
}
inline const char* noiseRatingName(NoiseRating r) {
  return r == kNoiseQuiet ? "quiet" : (r == kNoiseModerate ? "moderate" : "noisy");
}
// Gauge: -120..-90 dBm over 240 degrees, starting at 150 (lower left,
// clockwise through the top).
static const int kGaugeMinDbm = -120;
static const int kGaugeMaxDbm = -90;
static const int kGaugeStartDeg = 150;
static const int kGaugeSweepDeg = 240;
inline float gaugeAngle(float dbm) {
  float f = (dbm - (float)kGaugeMinDbm) / (float)(kGaugeMaxDbm - kGaugeMinDbm);
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  return (float)kGaugeStartDeg + f * (float)kGaugeSweepDeg;
}

// ------------------------------------------------------------ rate trend

// rates[59] = current (partial) minute, rates[0] = 59 minutes ago
// (MeshModel::ratesPerMinute). The last complete minute against the mean of
// the five before it; false while there is nothing to compare with.
inline bool rateTrend(const uint16_t rates[60], int& percent) {
  uint32_t sum = 0;
  for (int i = 53; i <= 57; i++) sum += rates[i];
  if (sum == 0) return false;
  const float mean = (float)sum / 5.0f;
  const float pct = ((float)rates[58] - mean) * 100.0f / mean;
  percent = (int)lroundf(pct);
  return true;
}

// ------------------------------------------------------------ quick controls

enum QuickTile { kTileAdvert, kTilePreset, kTileObserver, kTileBridge, kTileTrace, kTileAdmin, kTileCount };
enum QuickAction {
  kActNone,
  kActAdvert,         // send the HEAD's own advert (confirmed, rate-limited)
  kActOpenHead,       // HEAD page: repeater admin console
  kActAdminLogin,     // ...console, log in with the saved password
  kActOpenPresets,    // ...the repeater's preset list (its own confirm flow)
  kActOpenSettings,   // ...the repeater settings list (bridge.*, ...)
};

// What a tile does. ADVERT transmits, so it needs a second tap between
// kMinConfirmGapMs and kConfirmMs after the first (the tile shows
// "CONFIRM?"; a tap inside the gap is ignored, so one bouncing press can
// never send), and after a send the tile rests for kAdvertCooldownMs.
// Navigation runs at once. A tap on another tile or outside, or the
// timeout, cancels a pending confirm.
class QuickControls {
 public:
  static const uint32_t kConfirmMs = 3000;
  static const uint32_t kMinConfirmGapMs = 250;
  static const uint32_t kAdvertCooldownMs = 30000;
  static const uint32_t kResultMs = 2000;

  QuickControls()
      : _armed(-1), _armedMs(0), _result(-1), _resultOk(false), _resultMs(0), _firedMs(0), _fired(false) {}

  static bool needsConfirm(int tile) { return tile == kTileAdvert; }
  static QuickAction actionOf(int tile) {
    switch (tile) {
      case kTileAdvert: return kActAdvert;
      case kTileAdmin: return kActAdminLogin;
      case kTilePreset: return kActOpenPresets;
      case kTileObserver:
      case kTileBridge: return kActOpenSettings;
      case kTileTrace: return kActOpenHead;
      default: return kActNone;
    }
  }

  // A tap on `tile` (-1 = outside every tile). Returns the action to run now.
  QuickAction tap(int tile, uint32_t now_ms) {
    if (tile < 0 || tile >= kTileCount) {
      _armed = -1;
      return kActNone;
    }
    if (_armed == tile && confirming(tile, now_ms)) {
      if (now_ms - _armedMs < kMinConfirmGapMs) return kActNone;   // bounce: stay armed
      _armed = -1;
      if (tile == kTileAdvert) {
        _fired = true;
        _firedMs = now_ms;
      }
      return actionOf(tile);
    }
    _armed = -1;
    if (cooldownLeftMs(tile, now_ms) > 0) return kActNone;
    if (needsConfirm(tile)) {
      _armed = tile;
      _armedMs = now_ms;
      return kActNone;
    }
    return actionOf(tile);
  }
  bool confirming(int tile, uint32_t now_ms) const {
    return _armed == tile && now_ms - _armedMs < kConfirmMs;
  }
  // True while a confirm is pending (the page redraws when it times out).
  bool pending(uint32_t now_ms) const { return _armed >= 0 && now_ms - _armedMs < kConfirmMs; }
  void cancel() { _armed = -1; }
  // Time until ADVERT may be sent again (0 = ready).
  uint32_t cooldownLeftMs(int tile, uint32_t now_ms) const {
    if (tile != kTileAdvert || !_fired) return 0;
    const uint32_t since = now_ms - _firedMs;
    return since < kAdvertCooldownMs ? kAdvertCooldownMs - since : 0;
  }

  // The outcome of an action, shown on its tile for kResultMs.
  void setResult(int tile, bool ok, uint32_t now_ms) {
    _result = tile;
    _resultOk = ok;
    _resultMs = now_ms;
  }
  // 1 = ok, 0 = failed, -1 = nothing to show.
  int result(int tile, uint32_t now_ms) const {
    if (_result != tile || now_ms - _resultMs >= kResultMs) return -1;
    return _resultOk ? 1 : 0;
  }

 private:
  int _armed;
  uint32_t _armedMs;
  int _result;
  bool _resultOk;
  uint32_t _resultMs;
  uint32_t _firedMs;
  bool _fired;
};

// ------------------------------------------------------------ clock

// The head has no RTC battery: at boot its clock starts from a saved floor
// or a fixed seed, which is not the time. It counts as set once it has been
// moved (app or mesh clock sync) by more than kJumpS away from free-running
// since the first sample, to a plausible date. Until then the header shows
// the head's uptime instead -- never a made-up date.
class ClockTrack {
 public:
  static const uint32_t kJumpS = 30;
  static const uint32_t kMinPlausible = 1735689600u;   // 2025-01-01 UTC

  ClockTrack() : _epoch0(0), _ms0(0), _has(false), _set(false) {}
  void sample(uint32_t epoch, uint32_t now_ms) {
    if (!_has) {
      _epoch0 = epoch;
      _ms0 = now_ms;
      _has = true;
      return;
    }
    if (_set) return;
    const int64_t ran = (int64_t)((uint32_t)(now_ms - _ms0) / 1000u);
    const int64_t moved = (int64_t)epoch - (int64_t)_epoch0;
    const int64_t jump = moved - ran;
    if ((jump > (int64_t)kJumpS || jump < -(int64_t)kJumpS) && epoch >= kMinPlausible) _set = true;
  }
  bool valid() const { return _set; }

 private:
  uint32_t _epoch0, _ms0;
  bool _has, _set;
};

// Days since 1970-01-01 to civil date (Howard Hinnant's algorithm).
inline void civilFromDays(int32_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = (uint32_t)(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int32_t yy = (int32_t)yoe + era * 400;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = yy + (m <= 2 ? 1 : 0);
}

// valid: "13:42" and "31 Oct UTC"; else "T+05:12" (uptime h:mm; from
// 100 h "T+123h") and "UPTIME".
inline void formatClock(char* hm, size_t hmCap, char* date, size_t dateCap, bool valid, uint32_t epoch,
                        uint32_t uptime_ms) {
  if (valid) {
    static const char* const kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const uint32_t days = epoch / 86400u, sec = epoch % 86400u;
    int y;
    unsigned m, d;
    civilFromDays((int32_t)days, y, m, d);
    snprintf(hm, hmCap, "%02u:%02u", (unsigned)(sec / 3600u), (unsigned)(sec / 60u % 60u));
    snprintf(date, dateCap, "%u %s UTC", d, kMonths[(m - 1) % 12]);
    return;
  }
  const uint32_t mins = uptime_ms / 60000u;
  if (mins < 100u * 60u) snprintf(hm, hmCap, "T+%02u:%02u", (unsigned)(mins / 60u), (unsigned)(mins % 60u));
  else snprintf(hm, hmCap, "T+%uh", (unsigned)(mins / 60u));
  snprintf(date, dateCap, "UPTIME");
}

// ------------------------------------------------------------ link icon

// Arcs lit on the observer-link icon (0..4) from the age of the newest
// observer frame (packet or vitals); 0 when none has arrived.
inline int linkLevel(bool any, uint32_t age_ms) {
  if (!any) return 0;
  if (age_ms < 10000) return 4;
  if (age_ms < 30000) return 3;
  if (age_ms < 60000) return 2;
  if (age_ms < 300000) return 1;
  return 0;
}

}  // namespace hmi
}  // namespace freenove_ui
