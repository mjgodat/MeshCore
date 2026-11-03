#pragma once

// Non-blocking WiFi station join for a node that also runs ESP-NOW, used by
// the repeater's UDP bridge and the visual heads' UDP link. Pure, header-only,
// C++11; the ESP32 calls sit behind a Driver (esp32/StationJoinDriver.h), so
// test/test_station_join.py runs it on a simulated clock.
//
// Rules:
// - Never blocks: every step is one cheap driver call per poll(). The scan
//   for the access point is asynchronous and restricted to the ESP-NOW
//   channel (it does not leave that channel), the join is WiFi.begin()'s
//   asynchronous start.
// - ESP-NOW never leaves its channel. A join is attempted only for an access
//   point found on the ESP-NOW channel (pinned BSSID and channel). While
//   connected, every poll checks the station's channel; if the access point
//   moved (router auto-channel, CSA), the station is disconnected at once,
//   the ESP-NOW channel restored, and the reason reported (ap-moved). It
//   rejoins only when a later scan finds the access point back on the
//   ESP-NOW channel.
// - Failed attempts back off: 15 s, 30 s, 60 s, then every 2 min; a
//   successful join resets the back-off.
// - Optional diagnosis (heads): when the access point is not on the ESP-NOW
//   channel, one all-channel scan at most every `diagnose_interval_ms`
//   reports where it is. Repeaters leave it at 0 (never), so the bridge
//   never goes deaf for a scan.
// - With no ESP-NOW channel to protect (espNowChannel() == 0) the join is a
//   plain WiFi.begin() without a scan.

#include <stddef.h>
#include <stdint.h>

namespace mesh { namespace wifi {

enum class JoinState : uint8_t { Idle, Scanning, Joining, Connected, Diagnosing };
enum class JoinReason : uint8_t {
  None,           // nothing wrong (or not tried yet)
  NoCredentials,  // no network configured
  Busy,           // another WiFi owner (setup AP, OTA) holds the radio
  NotOnChannel,   // network not found on the ESP-NOW channel
  ApMoved,        // was joined; the access point left the ESP-NOW channel
  JoinTimeout,    // found but did not associate / get an address in time
  ScanFailed
};

inline const char* joinReasonName(JoinReason r) {
  switch (r) {
    case JoinReason::NoCredentials: return "no-credentials";
    case JoinReason::Busy: return "busy";
    case JoinReason::NotOnChannel: return "ap-not-on-espnow-channel";
    case JoinReason::ApMoved: return "ap-moved-off-espnow-channel";
    case JoinReason::JoinTimeout: return "join-timeout";
    case JoinReason::ScanFailed: return "scan-failed";
    default: return "none";
  }
}

// Driver contract (all calls must return promptly):
//   uint8_t espNowChannel()          channel to protect, 0 = none
//   bool mutationAllowed()           false while another owner holds WiFi
//   bool startScan(uint8_t ch, const char* ssid)   asynchronous; ch 0 = all
//   int  scanResult()                -1 running, < -1 failed, >= 0 count
//   bool pick(const char* ssid, uint8_t ch, uint8_t bssid[6], uint8_t& strongest)
//                                    best BSSID of ssid on ch (ch 0 = any);
//                                    strongest = channel of the strongest match
//   void scanDelete()
//   bool begin(const char* ssid, const char* pw, uint8_t ch, const uint8_t* bssid)
//   bool linkUp()                    associated with an address
//   uint8_t linkChannel()
//   void disconnect()                drop the association, keep ESP-NOW
//   void restoreChannel()            put the radio back on the ESP-NOW channel
template <class Driver>
class StationJoin {
public:
  static const uint32_t kFirstBackoffMs = 15000;
  static const uint32_t kMaxBackoffMs = 120000;
  static const uint32_t kScanTimeoutMs = 5000;
  static const uint32_t kDiagnoseTimeoutMs = 8000;
  static const uint32_t kJoinTimeoutMs = 20000;
  static const uint32_t kBusyRetryMs = 1000;

  explicit StationJoin(uint32_t diagnose_interval_ms = 0) : _diagnose_interval(diagnose_interval_ms) {}

  void reset(uint32_t now) {
    _state = JoinState::Idle; _reason = JoinReason::None;
    _retry_at = now; _backoff = kFirstBackoffMs; _ap_channel = 0;
  }

  // Leaves the radio as it is (an association belongs to whoever made it);
  // only an own scan is cancelled.
  void abandon(Driver& d) {
    if (_state == JoinState::Scanning || _state == JoinState::Diagnosing) { d.scanDelete(); d.restoreChannel(); }
    if (_state != JoinState::Connected) _state = JoinState::Idle;
  }

  // Drops this node's association too (transport switched off).
  void stop(Driver& d) {
    if (_state == JoinState::Scanning || _state == JoinState::Diagnosing) d.scanDelete();
    if (_state != JoinState::Idle) { d.disconnect(); d.restoreChannel(); }
    _state = JoinState::Idle;
  }

  // True when poll() will need the credentials on its next call (they can be
  // loaded lazily, e.g. from NVS, only then).
  bool needsCredentials(uint32_t now) const { return _state == JoinState::Idle && due(now, _retry_at); }

  void poll(Driver& d, uint32_t now, const char* ssid, const char* password) {
    const uint8_t channel = d.espNowChannel();
    switch (_state) {
      case JoinState::Idle: {
        if (d.linkUp()) {   // joined by another owner, or a disconnect still settling
          const uint8_t at = d.linkChannel();
          if (channel && at != channel) {   // never stay off the ESP-NOW channel
            if (at) _ap_channel = at;
            d.disconnect();
            d.restoreChannel();
            if (_reason == JoinReason::None) _reason = JoinReason::ApMoved;
            return;
          }
          _state = JoinState::Connected;
          checkConnected(d, now, channel);
          return;
        }
        if (!due(now, _retry_at)) return;
        if (!ssid || !ssid[0]) { _reason = JoinReason::NoCredentials; fail(now); return; }
        if (!d.mutationAllowed()) { _reason = JoinReason::Busy; _retry_at = now + kBusyRetryMs; return; }
        if (!channel) {
          if (!d.begin(ssid, password, 0, nullptr)) { _reason = JoinReason::ScanFailed; fail(now); return; }
          _state = JoinState::Joining; _deadline = now + kJoinTimeoutMs;
          return;
        }
        if (!d.startScan(channel, ssid)) { _reason = JoinReason::ScanFailed; fail(now); return; }
        _state = JoinState::Scanning; _deadline = now + kScanTimeoutMs;
        return;
      }
      case JoinState::Scanning: {
        const int n = d.scanResult();
        if (n < 0) {
          if (n == -1 && !due(now, _deadline)) return;   // still running
          d.scanDelete(); d.restoreChannel(); _reason = JoinReason::ScanFailed; _state = JoinState::Idle; fail(now);
          return;
        }
        uint8_t bssid[6] = {0, 0, 0, 0, 0, 0};
        uint8_t strongest = 0;
        const bool found = n > 0 && d.pick(ssid, channel, bssid, strongest);
        d.scanDelete();
        if (!found) {
          d.restoreChannel();
          _reason = JoinReason::NotOnChannel;
          _state = JoinState::Idle;
          if (_diagnose_interval && (!_diagnosed_at || now - _diagnosed_at >= _diagnose_interval)
              && d.startScan(0, ssid)) {
            _diagnosed_at = now | 1U;
            _state = JoinState::Diagnosing; _deadline = now + kDiagnoseTimeoutMs;
            return;
          }
          fail(now);
          return;
        }
        if (!ssid || !d.begin(ssid, password, channel, bssid)) {
          d.restoreChannel(); _reason = JoinReason::ScanFailed; _state = JoinState::Idle; fail(now);
          return;
        }
        _state = JoinState::Joining; _deadline = now + kJoinTimeoutMs;
        return;
      }
      case JoinState::Diagnosing: {
        const int n = d.scanResult();
        if (n == -1 && !due(now, _deadline)) return;
        uint8_t bssid[6]; uint8_t strongest = 0;
        if (n > 0) d.pick(ssid, 0, bssid, strongest);
        if (strongest) _ap_channel = strongest;
        d.scanDelete();
        d.restoreChannel();
        _state = JoinState::Idle;
        fail(now);
        return;
      }
      case JoinState::Joining:
        if (d.linkUp()) { _state = JoinState::Connected; checkConnected(d, now, channel); return; }
        if (due(now, _deadline)) {
          d.disconnect(); d.restoreChannel();
          _reason = JoinReason::JoinTimeout; _state = JoinState::Idle; fail(now);
        }
        return;
      case JoinState::Connected:
        if (!d.linkUp()) {   // lost: try again soon, from the start of the back-off
          _state = JoinState::Idle; _retry_at = now + kFirstBackoffMs; _backoff = kFirstBackoffMs;
          return;
        }
        checkConnected(d, now, channel);
        return;
    }
  }

  JoinState state() const { return _state; }
  bool connected() const { return _state == JoinState::Connected; }
  JoinReason reason() const { return _reason; }
  // Channel the access point was last seen on (0 = not known).
  uint8_t apChannel() const { return _ap_channel; }
  uint32_t backoffMs() const { return _backoff; }
  uint32_t retryAt() const { return _retry_at; }

private:
  static bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

  void fail(uint32_t now) {
    _retry_at = now + _backoff;
    _backoff = _backoff >= kMaxBackoffMs / 2 ? kMaxBackoffMs : _backoff * 2;
  }

  void checkConnected(Driver& d, uint32_t now, uint8_t channel) {
    const uint8_t at = d.linkChannel();
    if (at) _ap_channel = at;
    if (channel && at != channel) {
      // Never follow the access point off the ESP-NOW channel.
      d.disconnect();
      d.restoreChannel();
      _reason = JoinReason::ApMoved;
      _state = JoinState::Idle;
      fail(now);
      return;
    }
    _reason = JoinReason::None;
    _backoff = kFirstBackoffMs;
  }

  JoinState _state = JoinState::Idle;
  JoinReason _reason = JoinReason::None;
  uint32_t _retry_at = 0, _deadline = 0, _backoff = kFirstBackoffMs;
  uint32_t _diagnose_interval, _diagnosed_at = 0;
  uint8_t _ap_channel = 0;
};

}}  // namespace mesh::wifi
