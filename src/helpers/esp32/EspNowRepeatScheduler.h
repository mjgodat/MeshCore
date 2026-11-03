#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mesh {
namespace espnow {

// Optional time-spaced redundant transmission of outgoing ESP-NOW frames.
// `count` is the total number of copies (1 = feature off, today's behavior);
// `gap_ms` separates the start of one copy from the start of the next.
// Receivers already drop duplicates (MeshCore seen table, bridge seen table,
// raw fragment reassembler), so copies are harmless to them.
constexpr uint8_t kRepeatCountMin = 1;
constexpr uint8_t kRepeatCountMax = 4;
constexpr uint8_t kRepeatCountDefault = 1;
constexpr uint16_t kRepeatGapMaxMs = 200;
constexpr uint16_t kRepeatGapDefaultMs = 30;

inline bool isValidRepeatCount(uint32_t count) {
  return count >= kRepeatCountMin && count <= kRepeatCountMax;
}
inline bool isValidRepeatGap(uint32_t gap_ms) { return gap_ms <= kRepeatGapMaxMs; }

// Strict unsigned decimal: digits only, no sign/whitespace/trailing text.
// `value` is untouched on failure.
inline bool parseRepeatDecimal(const char* text, uint32_t max, uint32_t& value) {
  if (text == nullptr || *text == 0) return false;
  uint32_t parsed = 0;
  for (size_t i = 0; text[i]; ++i) {
    if (i >= 3 || text[i] < '0' || text[i] > '9') return false;
    parsed = parsed * 10 + static_cast<uint32_t>(text[i] - '0');
  }
  if (parsed > max) return false;
  value = parsed;
  return true;
}

// OTA firmware frames are paced by Dispatcher's OTA quiet time and must never
// be multiplied. Matches PAYLOAD_TYPE_OTA (header bits 2..5 of byte 0).
constexpr uint8_t kRepeatOtaPayloadType = 0x0C;
inline bool isRepeatablePacket(const uint8_t* packet, size_t length) {
  return packet != nullptr && length > 0
      && ((packet[0] >> 2) & 0x0F) != kRepeatOtaPayloadType;
}

// Live settings shared by the primary ESP-NOW radio and the ESP-NOW bridge.
// The persistence module (ESPNowRepeatSettings) writes it after a verified
// load/save; senders read it when a new frame starts.
struct EspNowRepeatConfig {
  uint8_t count = kRepeatCountDefault;
  uint16_t gap_ms = kRepeatGapDefaultMs;
  uint32_t radio_copies = 0;  // copies started by the primary ESP-NOW radio
  uint32_t radio_copy_timeouts = 0;  // copies whose send callback never arrived
};

// A copy frame whose send callback has not arrived within this bound is
// abandoned, so a lost callback can never hold Dispatcher's TX indefinitely.
// Matches the order of Dispatcher's own per-packet deadline (airtime x 1.5).
constexpr uint32_t kRepeatCopyCallbackTimeoutMs = 50;
inline EspNowRepeatConfig& espNowRepeatConfig() {
  static EspNowRepeatConfig config;
  return config;
}

// Pure, bounded scheduler for one logical frame's extra copies. It never
// queues: a new frame replaces (cancels) any copies still owed for the
// previous one. Callers own the bytes and the radio; this only answers
// "may a copy start now?".
class EspNowRepeatScheduler {
 public:
  // A new original frame started transmitting at `now`.
  void begin(uint32_t now, uint8_t count, uint16_t gap_ms) {
    remaining_ = isValidRepeatCount(count) ? static_cast<uint8_t>(count - 1) : 0;
    gap_ms_ = gap_ms > kRepeatGapMaxMs ? kRepeatGapMaxMs : gap_ms;
    last_start_ = now;
    in_flight_ = true;
  }

  // The most recently started transmission (original or copy) completed.
  void finished() { in_flight_ = false; }

  // The caller gave up on the current transmission without completing it.
  void abortIfUnfinished() {
    if (in_flight_) cancel();
  }

  void cancel() {
    remaining_ = 0;
    in_flight_ = false;
  }

  bool pending() const { return remaining_ != 0; }
  uint8_t remaining() const { return remaining_; }

  // `idle`: the radio/send path has no outstanding send of its own.
  bool due(uint32_t now, bool idle) const {
    return remaining_ != 0 && !in_flight_ && idle
        && static_cast<uint32_t>(now - last_start_) >= gap_ms_;
  }

  void copyStarted(uint32_t now) {
    if (remaining_ != 0) --remaining_;
    last_start_ = now;
    in_flight_ = true;
  }

 private:
  uint32_t last_start_ = 0;
  uint16_t gap_ms_ = 0;
  uint8_t remaining_ = 0;
  bool in_flight_ = false;
};

}  // namespace espnow
}  // namespace mesh
