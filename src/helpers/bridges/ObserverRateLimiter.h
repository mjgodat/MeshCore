#pragma once

// Observer feed admission and buffering (repeater -> visual head).
// Pure, header-only, no Arduino dependencies; owned by the main loop only.
//
// Policy:
// - Packet records pass a token bucket (default 20/s, burst 40).
// - Vitals bypass the bucket.
// - While mesh traffic is queued on the bridge, new packet records are
//   dropped (mesh frames always win). Vitals still queue and wait.
// - The outbox is small. When full, the oldest packet record is evicted
//   (the oldest vitals frame only when nothing else remains).

#include "ObserverFrame.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mesh { namespace observer {

constexpr uint16_t kPacketRatePerSecond = 20;
constexpr uint16_t kPacketBurst = 40;
constexpr uint8_t kOutboxDepth = 4;
constexpr uint32_t kVitalsIntervalMs = 5000;

// Integer token bucket in milli-tokens; wrap-safe on a 32-bit millisecond clock.
class TokenBucket {
public:
  explicit TokenBucket(uint16_t rate_per_s = kPacketRatePerSecond, uint16_t burst = kPacketBurst)
      : _rate(rate_per_s), _cap(uint32_t(burst) * 1000U) {}

  bool take(uint32_t now_ms) {
    refill(now_ms);
    if (_milli < 1000U) return false;
    _milli -= 1000U;
    return true;
  }
  void reset() { _started = false; }

private:
  void refill(uint32_t now_ms) {
    if (!_started) { _started = true; _last = now_ms; _milli = _cap; return; }
    uint32_t elapsed = now_ms - _last;
    _last = now_ms;
    if (_rate == 0) return;
    const uint32_t full_after = _cap / _rate + 1;  // ms to refill from empty
    if (elapsed > full_after) elapsed = full_after;
    const uint32_t added = elapsed * _rate;         // ms * tokens/s = milli-tokens
    _milli = (_cap - _milli <= added) ? _cap : _milli + added;
  }
  uint16_t _rate;
  uint32_t _cap;
  uint32_t _milli = 0, _last = 0;
  bool _started = false;
};

struct OutboxStats {
  uint32_t queued = 0, sent_ok = 0, sent_fail = 0, timeouts = 0;
  uint32_t rate_drops = 0, preempt_drops = 0, overflow_drops = 0;
};

enum class Offer : uint8_t { Queued, RateLimited, Preempted, Invalid };

class Outbox {
public:
  Offer offer(const uint8_t* frame, size_t len, uint32_t now_ms, bool mesh_pending) {
    if (!isObserverFrame(frame, len) || len > MAX_PACKET_FRAME) return Offer::Invalid;
    const bool vitals = frame[5] == (uint8_t)Kind::Vitals;
    if (!vitals) {
      if (mesh_pending) { ++_stats.preempt_drops; return Offer::Preempted; }
      if (!_bucket.take(now_ms)) { ++_stats.rate_drops; return Offer::RateLimited; }
    }
    if (_count == kOutboxDepth) {
      uint8_t victim = 0;
      while (victim < _count && _slots[victim].vitals) ++victim;
      remove(victim < _count ? victim : 0);
      ++_stats.overflow_drops;
    }
    Slot& slot = _slots[_count++];
    slot.len = (uint8_t)len;
    slot.vitals = vitals;
    memcpy(slot.data, frame, len);
    ++_stats.queued;
    return Offer::Queued;
  }

  bool empty() const { return _count == 0; }
  uint8_t size() const { return _count; }

  // Moves the oldest frame to `out`; returns its length, or 0 when empty.
  size_t pop(uint8_t* out, size_t cap) {
    if (_count == 0 || out == nullptr || cap < _slots[0].len) return 0;
    const size_t len = _slots[0].len;
    memcpy(out, _slots[0].data, len);
    remove(0);
    return len;
  }

  void clear() { _count = 0; }
  void noteSent(bool ok) { ++(ok ? _stats.sent_ok : _stats.sent_fail); }
  void noteTimeout() { ++_stats.timeouts; }
  const OutboxStats& stats() const { return _stats; }

private:
  struct Slot { uint8_t len; bool vitals; uint8_t data[MAX_PACKET_FRAME]; };
  void remove(uint8_t index) {
    for (uint8_t i = index; i + 1 < _count; ++i) _slots[i] = _slots[i + 1];
    --_count;
  }
  Slot _slots[kOutboxDepth];
  uint8_t _count = 0;
  TokenBucket _bucket;
  OutboxStats _stats;
};

// Events per minute over a measured window; saturates at 0xFFFF.
inline uint16_t perMinute(uint32_t delta, uint32_t elapsed_ms) {
  if (elapsed_ms == 0) return 0;
  const uint64_t rate = (uint64_t)delta * 60000U / elapsed_ms;
  return rate > 0xFFFF ? 0xFFFF : (uint16_t)rate;
}

// Busy time per mille over a measured window; saturates at 1000.
inline uint16_t permille(uint32_t busy_ms, uint32_t elapsed_ms) {
  if (elapsed_ms == 0) return 0;
  const uint64_t value = (uint64_t)busy_ms * 1000U / elapsed_ms;
  return value > 1000 ? 1000 : (uint16_t)value;
}

// The repeater's counters at one vitals tick (all free-running).
struct VitalsCounters { uint32_t at_ms, rx, tx, dup, airtime_ms; };
struct VitalsRates { uint16_t rx_per_min, tx_per_min, dup_per_min, airtime_permille; };

// Rates over the last minute of vitals ticks rather than one tick: a
// kVitalsIntervalMs window holds 0 or 1 packet on a quiet mesh, so
// per-interval rates read 0 or 12/min in turn. Keeps the last kSlots
// samples; rates are taken against the oldest (one minute back once full).
class VitalsWindow {
public:
  static const int kSlots = (int)(60000U / kVitalsIntervalMs);

  void reset() { _n = 0; _head = 0; }
  bool started() const { return _n > 0; }

  // Adds a sample and writes the rates since the oldest kept one. False
  // (out untouched) for the first sample after reset() or after a counter
  // went backwards (stats reset): the window starts over from it.
  bool sample(const VitalsCounters& c, VitalsRates& out) {
    bool ok = _n > 0;
    if (ok) {
      const VitalsCounters& o = _s[(_head + kSlots - _n) % kSlots];
      const VitalsCounters& last = _s[(_head + kSlots - 1) % kSlots];
      if (c.rx < last.rx || c.tx < last.tx || c.dup < last.dup || c.airtime_ms < last.airtime_ms) {
        reset();
        ok = false;
      } else {
        const uint32_t elapsed = c.at_ms - o.at_ms;
        out.rx_per_min = perMinute(c.rx - o.rx, elapsed);
        out.tx_per_min = perMinute(c.tx - o.tx, elapsed);
        out.dup_per_min = perMinute(c.dup - o.dup, elapsed);
        out.airtime_permille = permille(c.airtime_ms - o.airtime_ms, elapsed);
      }
    }
    _s[_head] = c;
    _head = (_head + 1) % kSlots;
    if (_n < kSlots) _n++;
    return ok;
  }

private:
  VitalsCounters _s[kSlots] = {};
  int _n = 0;
  int _head = 0;
};


// SNR in dB to quarter-dB steps, clamped to int8.
inline int8_t snrQuarterDb(float snr) {
  const float q = snr * 4.0f;
  if (!(q > -128.0f)) return -128;  // also maps NaN to the floor
  if (q > 127.0f) return 127;
  return (int8_t)(q < 0 ? q - 0.5f : q + 0.5f);
}

inline int16_t clampRssi(float rssi) {
  if (!(rssi > -32768.0f)) return -32768;
  if (rssi > 32767.0f) return 32767;
  return (int16_t)(rssi < 0 ? rssi - 0.5f : rssi + 0.5f);
}

}}  // namespace mesh::observer
