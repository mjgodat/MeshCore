#pragma once

// A visual head's side of repeater discovery (helpers/bridges/DiscoveryFrame.h).
// Pure, header-only, C++11, no Arduino; host tested in
// test/test_head_discovery.py. The ESP32 glue is HeadLinkRadio.
//
// - A verified announce (heard over ESP-NOW or UDP) tells the head where the
//   repeater's UDP bridge is. While `udp.peer` is off, that address is the
//   head's repeater: nothing to type. A manual udp.peer always wins.
// - The learned address sticks to one repeater (pubkey prefix): another
//   repeater's announces do not move it until the current one has been
//   silent for kSwitchAfterMs.
// - Replays are ignored: within a boot the counter must grow, and a new boot
//   id is taken only after the current boot has been silent kNewBootAfterMs
//   (a reboot), so a recorded old announce cannot move the head back.
// - The head answers an announce with its own UDP address (Reply), so the
//   repeater adds it as an expiring UDP peer: at most every kReplyEveryMs,
//   sooner when the repeater rebooted or the head's address changed, but
//   never more than once per kReplyMinGapMs.
// - The first repeater announced is offered as the home repeater once (the
//   head keeps its own home logic: a home it already has is not replaced).

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "UDPBridgePrefs.h"
#include "bridges/DiscoveryFrame.h"

namespace mesh { namespace headlink {

class HeadDiscovery {
public:
  static const uint32_t kSwitchAfterMs = 120000;   // another repeater after 2 min of silence
  static const uint32_t kNewBootAfterMs = 15000;   // a new boot id after 15 s of silence
  static const uint32_t kReplyMinGapMs = 1000;     // at most one reply a second

  enum Note : uint8_t { Ignored, Kept, PeerChanged };

  // A verified announce. Ignored: another repeater, a replay or a stale boot
  // (nothing learned, no reply). PeerChanged: the learned address moved.
  Note noteAnnounce(const mesh::discovery::Announce& a, bool via_espnow, uint32_t now) {
    const bool same = _seen && !memcmp(_last.prefix, a.prefix, sizeof(a.prefix));
    if (_seen && !same && now - _at < kSwitchAfterMs) { ++_ignored; return Ignored; }
    if (same && a.boot == _last.boot && a.counter <= _last.counter) { ++_replays; return Ignored; }
    if (same && a.boot != _last.boot && now - _at < kNewBootAfterMs) { ++_replays; return Ignored; }
    if (!_home_offered) { memcpy(_home, a.prefix, sizeof(_home)); _home_offered = _home_pending = true; }
    _last = a;
    _seen = true;
    _at = now;
    _via_espnow = via_espnow;
    mesh::bridge::udp::Peer peer;
    if ((a.caps & mesh::discovery::CAP_UDP) && a.port && mesh::discovery::usableAddress(a.ip)) {
      memcpy(peer.ip, a.ip, 4);
      peer.port = a.port;
    }
    const bool changed = !mesh::bridge::udp::equalPeer(peer, _peer);
    _peer = peer;
    return changed ? PeerChanged : Kept;
  }

  // The address learned from announces (port 0 = none).
  const mesh::bridge::udp::Peer& learnedPeer() const { return _peer; }

  // The repeater a head should talk to: the manual udp.peer, else the learned one.
  static mesh::bridge::udp::Peer effectivePeer(const mesh::bridge::udp::Peer& manual,
                                               const mesh::bridge::udp::Peer& learned) {
    return manual.enabled() ? manual : learned;
  }

  // Should the head answer the latest announce now, from local_ip:local_port?
  bool shouldReply(uint32_t now, const uint8_t local_ip[4], uint16_t local_port) const {
    if (!_seen || !(_last.caps & mesh::discovery::CAP_AUTOPAIR) || !_last.port || !local_port
        || !mesh::discovery::usableAddress(local_ip)) return false;
    if (!_replied) return true;
    if (now - _replied_at < kReplyMinGapMs) return false;
    if (_replied_boot != _last.boot || memcmp(_replied_ip, local_ip, 4) || _replied_port != local_port) return true;
    return now - _replied_at >= mesh::discovery::kReplyEveryMs;
  }
  void noteReplied(uint32_t now, const uint8_t local_ip[4], uint16_t local_port) {
    _replied = true;
    _replied_at = now;
    _replied_boot = _last.boot;
    memcpy(_replied_ip, local_ip, 4);
    _replied_port = local_port;
    ++_replies;
  }

  // The reply to the latest announce (the caller signs and sends it).
  mesh::discovery::Reply reply(const uint8_t self_prefix[6], const char* name, const uint8_t local_ip[4],
                               uint16_t local_port) const {
    mesh::discovery::Reply r;
    r.boot = _last.boot;
    r.counter = _last.counter;
    memcpy(r.prefix, self_prefix, sizeof(r.prefix));
    size_t n = 0;
    while (name && n < mesh::discovery::NAME_SIZE && name[n]) { r.name[n] = name[n]; ++n; }
    r.flags = 0;   // reserved
    memcpy(r.ip, local_ip, 4);
    r.port = local_port;
    return r;
  }

  // The first announced repeater, offered once as the home repeater.
  bool takeHome(uint8_t out[6]) {
    if (!_home_pending) return false;
    _home_pending = false;
    memcpy(out, _home, sizeof(_home));
    return true;
  }

  bool seen() const { return _seen; }
  const mesh::discovery::Announce& last() const { return _last; }
  uint32_t lastAt() const { return _at; }
  bool lastViaEspNow() const { return _via_espnow; }
  uint32_t replies() const { return _replies; }
  uint32_t ignored() const { return _ignored; }
  uint32_t replays() const { return _replays; }

private:
  mesh::discovery::Announce _last;
  mesh::bridge::udp::Peer _peer;
  bool _seen = false, _via_espnow = false, _replied = false;
  bool _home_offered = false, _home_pending = false;
  uint8_t _home[6] = {};
  uint32_t _at = 0, _replied_at = 0, _replied_boot = 0, _replies = 0, _ignored = 0, _replays = 0;
  uint8_t _replied_ip[4] = {};
  uint16_t _replied_port = 0;
};

}}  // namespace mesh::headlink
