#pragma once

// Repeater <-> head discovery and pairing frames, plus the repeater's bounded
// table of auto-paired UDP heads and its bridged-ingress rate limit.
// Pure, header-only, C++11, no Arduino (host tested in
// test/test_bridge_discovery.py).
//
// Announce (repeater -> anyone, every 5 s on ESP-NOW, every 10 s on UDP):
// who the repeater is and how to reach it on the other transport. Reply
// (head -> repeater, over UDP, to the announce's address): the head's own
// UDP address, echoing the announce's boot id and counter. Both carry an
// 8-byte tag, the first bytes of HMAC-SHA256 keyed with the shared secret
// (the repeater's bridge.secret = the head's udp.secret / espnow.secret)
// over "MeshCore discovery v1\0" and every byte before the tag. Frames whose
// tag does not verify are ignored. Never carried: WiFi credentials, secrets.
//
// Layout (little-endian, like ObserverFrame.h): MAGIC[4] VERSION KIND body TAG[8]
//   MAGIC B0 42 44 43. Like the observer magic, B0 is a MeshCore header byte
//   today's MeshCore rejects (version bits 0b10), so a node that does not
//   know these frames drops them; the four bytes plus the exact length make
//   an accidental match negligible.
//   Announce body (39): boot u32, counter u32, pubkey prefix[6], name[16]
//     (NUL padded), espnow channel u8 (0 = bridge not running), espnow
//     format u8 (0 raw, 1 wrapped), caps u8, udp ip[4], udp port u16
//     (0 = no UDP).
//   Reply body (37): boot u32, counter u32 (both echoed), pubkey prefix[6],
//     name[16], flags u8 (bit 0: wants the observer feed), udp ip[4],
//     udp port u16.
//
// On ESP-NOW the frame travels like any other frame of the link: raw, or
// wrapped (ESPNowWrappedFrame.h) when the link is wrapped.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mesh { namespace discovery {

constexpr uint8_t MAGIC0 = 0xB0, MAGIC1 = 0x42, MAGIC2 = 0x44, MAGIC3 = 0x43;
constexpr uint8_t VERSION = 1;
enum class Kind : uint8_t { Announce = 1, Reply = 2 };

constexpr size_t HEADER_SIZE = 6;
constexpr size_t TAG_SIZE = 8;
constexpr size_t PREFIX_SIZE = 6;
constexpr size_t NAME_SIZE = 16;
constexpr size_t ANNOUNCE_FRAME = HEADER_SIZE + 4 + 4 + PREFIX_SIZE + NAME_SIZE + 1 + 1 + 1 + 4 + 2 + TAG_SIZE;
constexpr size_t REPLY_FRAME = HEADER_SIZE + 4 + 4 + PREFIX_SIZE + NAME_SIZE + 1 + 4 + 2 + TAG_SIZE;
constexpr size_t MAX_FRAME = ANNOUNCE_FRAME;
static_assert(ANNOUNCE_FRAME == 53 && REPLY_FRAME == 51, "discovery frame sizes");

constexpr uint32_t kAnnounceEspNowMs = 5000;   // ESP-NOW announce period
constexpr uint32_t kAnnounceUdpMs = 10000;     // UDP announce period
constexpr uint32_t kReplyWindow = 12;          // a reply may echo one of the last 12 counters (~60 s)
constexpr uint32_t kReplyEveryMs = 30000;      // a head re-replies at most this often

// Capability bits in an announce.
constexpr uint8_t CAP_ESPNOW = 0x01;       // ESP-NOW bridge running
constexpr uint8_t CAP_UDP = 0x02;          // UDP bridge ready (ip/port valid)
constexpr uint8_t CAP_OBSERVER = 0x04;     // observer feed on
constexpr uint8_t CAP_OBS_ESPNOW = 0x08;   // ... over ESP-NOW
constexpr uint8_t CAP_OBS_UDP = 0x10;      // ... over UDP
constexpr uint8_t CAP_AUTOPAIR = 0x20;     // accepts replies (UDP auto-pairing)
constexpr uint8_t REPLY_WANTS_OBSERVER = 0x01;

// HMAC-SHA256 over (domain || data) keyed with the secret; the platform
// supplies it (rweather SHA256 on the devices). False when no key.
typedef bool (*Mac)(void* ctx, const uint8_t* data, size_t len, uint8_t digest[32]);
static const uint8_t kDomain[] = "MeshCore discovery v1";   // hashed with its NUL

struct Announce {
  uint32_t boot = 0, counter = 0;
  uint8_t prefix[PREFIX_SIZE] = {};
  char name[NAME_SIZE + 1] = {};
  uint8_t espnow_channel = 0, espnow_format = 0, caps = 0;
  uint8_t ip[4] = {};
  uint16_t port = 0;
};

struct Reply {
  uint32_t boot = 0, counter = 0;
  uint8_t prefix[PREFIX_SIZE] = {};
  char name[NAME_SIZE + 1] = {};
  uint8_t flags = 0;
  uint8_t ip[4] = {};
  uint16_t port = 0;
};

namespace detail {
struct W {
  uint8_t* p; size_t i;
  void u8(uint8_t v) { p[i++] = v; }
  void u16(uint16_t v) { u8(v & 0xFF); u8(v >> 8); }
  void u32(uint32_t v) { u16(v & 0xFFFF); u16((uint16_t)(v >> 16)); }
  void bytes(const void* b, size_t n) { memcpy(p + i, b, n); i += n; }
  void name(const char* s) {
    size_t n = 0;
    while (n < NAME_SIZE && s[n]) n++;
    memset(p + i, 0, NAME_SIZE); memcpy(p + i, s, n); i += NAME_SIZE;
  }
  void head(Kind k) { u8(MAGIC0); u8(MAGIC1); u8(MAGIC2); u8(MAGIC3); u8(VERSION); u8((uint8_t)k); }
};
struct R {
  const uint8_t* p; size_t i;
  uint8_t u8() { return p[i++]; }
  uint16_t u16() { uint16_t a = u8(); return (uint16_t)(a | (u8() << 8)); }
  uint32_t u32() { uint32_t a = u16(); return a | ((uint32_t)u16() << 16); }
  void bytes(void* b, size_t n) { memcpy(b, p + i, n); i += n; }
  bool name(char* out) {
    memcpy(out, p + i, NAME_SIZE); out[NAME_SIZE] = 0;
    // NUL padded, printable ASCII only
    size_t n = strlen(out);
    for (size_t k = n; k < NAME_SIZE; ++k) if (p[i + k]) return false;
    for (size_t k = 0; k < n; ++k) if ((uint8_t)out[k] < 0x20 || (uint8_t)out[k] > 0x7E) out[k] = '?';
    i += NAME_SIZE;
    return true;
  }
};
inline bool tag(Mac mac, void* ctx, const uint8_t* data, size_t len, uint8_t out[TAG_SIZE]) {
  uint8_t digest[32];
  if (mac == nullptr || !mac(ctx, data, len, digest)) return false;
  memcpy(out, digest, TAG_SIZE);
  memset(digest, 0, sizeof(digest));
  return true;
}
inline bool verify(Mac mac, void* ctx, const uint8_t* frame, size_t body_end) {
  uint8_t expected[TAG_SIZE];
  if (!tag(mac, ctx, frame, body_end, expected)) return false;
  uint8_t diff = 0;
  for (size_t k = 0; k < TAG_SIZE; ++k) diff |= (uint8_t)(expected[k] ^ frame[body_end + k]);
  return diff == 0;
}
}  // namespace detail

inline bool isDiscoveryFrame(const uint8_t* buf, size_t len) {
  if (buf == nullptr || len < HEADER_SIZE) return false;
  if (buf[0] != MAGIC0 || buf[1] != MAGIC1 || buf[2] != MAGIC2 || buf[3] != MAGIC3 || buf[4] != VERSION) return false;
  if (buf[5] == (uint8_t)Kind::Announce) return len == ANNOUNCE_FRAME;
  if (buf[5] == (uint8_t)Kind::Reply) return len == REPLY_FRAME;
  return false;
}
inline bool isAnnounce(const uint8_t* buf, size_t len) {
  return isDiscoveryFrame(buf, len) && buf[5] == (uint8_t)Kind::Announce;
}
inline bool isReply(const uint8_t* buf, size_t len) {
  return isDiscoveryFrame(buf, len) && buf[5] == (uint8_t)Kind::Reply;
}

inline size_t encodeAnnounce(const Announce& a, Mac mac, void* ctx, uint8_t* out, size_t cap) {
  if (out == nullptr || cap < ANNOUNCE_FRAME || a.boot == 0 || a.espnow_format > 1) return 0;
  detail::W w{out, 0};
  w.head(Kind::Announce);
  w.u32(a.boot); w.u32(a.counter); w.bytes(a.prefix, PREFIX_SIZE); w.name(a.name);
  w.u8(a.espnow_channel); w.u8(a.espnow_format); w.u8(a.caps);
  w.bytes(a.ip, 4); w.u16(a.port);
  if (!detail::tag(mac, ctx, out, w.i, out + w.i)) return 0;
  return w.i + TAG_SIZE;
}

inline size_t encodeReply(const Reply& r, Mac mac, void* ctx, uint8_t* out, size_t cap) {
  if (out == nullptr || cap < REPLY_FRAME || r.boot == 0) return 0;
  detail::W w{out, 0};
  w.head(Kind::Reply);
  w.u32(r.boot); w.u32(r.counter); w.bytes(r.prefix, PREFIX_SIZE); w.name(r.name);
  w.u8(r.flags); w.bytes(r.ip, 4); w.u16(r.port);
  if (!detail::tag(mac, ctx, out, w.i, out + w.i)) return 0;
  return w.i + TAG_SIZE;
}

// Decoders verify the tag first; `out` is untouched on failure.
inline bool decodeAnnounce(const uint8_t* buf, size_t len, Mac mac, void* ctx, Announce& out) {
  if (!isAnnounce(buf, len) || !detail::verify(mac, ctx, buf, len - TAG_SIZE)) return false;
  detail::R r{buf, HEADER_SIZE};
  Announce a;
  a.boot = r.u32(); a.counter = r.u32(); r.bytes(a.prefix, PREFIX_SIZE);
  if (!r.name(a.name)) return false;
  a.espnow_channel = r.u8(); a.espnow_format = r.u8(); a.caps = r.u8();
  r.bytes(a.ip, 4); a.port = r.u16();
  if (a.boot == 0 || a.espnow_format > 1 || a.espnow_channel > 13) return false;
  out = a;
  return true;
}

inline bool decodeReply(const uint8_t* buf, size_t len, Mac mac, void* ctx, Reply& out) {
  if (!isReply(buf, len) || !detail::verify(mac, ctx, buf, len - TAG_SIZE)) return false;
  detail::R r{buf, HEADER_SIZE};
  Reply p;
  p.boot = r.u32(); p.counter = r.u32(); r.bytes(p.prefix, PREFIX_SIZE);
  if (!r.name(p.name)) return false;
  p.flags = r.u8(); r.bytes(p.ip, 4); p.port = r.u16();
  if (p.boot == 0) return false;
  out = p;
  return true;
}

// A usable unicast IPv4 host address (not 0.x, 127.x, multicast, broadcast).
inline bool usableAddress(const uint8_t ip[4]) {
  return ip[0] != 0 && ip[0] != 127 && ip[0] < 224
      && !(ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255);
}

// The repeater's freshness rule: a reply must echo this boot and one of the
// last kReplyWindow announce counters.
inline bool freshReply(const Reply& r, uint32_t boot, uint32_t counter) {
  return r.boot == boot && r.counter <= counter && counter - r.counter < kReplyWindow;
}

inline bool due(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }

// Repeater side: heads auto-paired over UDP. Bounded slots, keyed by the
// head's pubkey prefix; a slot lives kAutoPeerTtlMs after the head's last
// verified reply. New heads (not refreshes) are rate limited. The caller
// checks the manual udp.peer.N list first (manual wins, nothing is added).
constexpr uint8_t kAutoPeerSlots = 4;
constexpr uint32_t kAutoPeerTtlMs = 300000;     // 5 min without a reply
constexpr uint8_t kAutoPeerNewPerMinute = 4;

struct AutoPeer {
  bool used = false;
  uint8_t prefix[PREFIX_SIZE] = {};
  uint8_t ip[4] = {};
  uint16_t port = 0;
  uint32_t until = 0;
  bool wants_observer = false;
};

enum class PairResult : uint8_t { Added, Refreshed, Moved, Full, RateLimited, Invalid };

class AutoPeers {
public:
  // Records a verified, fresh reply. Moved: same head, new address (the
  // caller re-points the UDP peer). Full / RateLimited: nothing changed.
  PairResult offer(const Reply& r, uint32_t now) {
    if (!usableAddress(r.ip) || r.port == 0) return PairResult::Invalid;
    for (uint8_t i = 0; i < kAutoPeerSlots; ++i) {
      AutoPeer& p = _slots[i];
      if (!p.used || memcmp(p.prefix, r.prefix, PREFIX_SIZE)) continue;
      const bool moved = memcmp(p.ip, r.ip, 4) || p.port != r.port;
      memcpy(p.ip, r.ip, 4); p.port = r.port;
      p.until = now + kAutoPeerTtlMs;
      p.wants_observer = (r.flags & REPLY_WANTS_OBSERVER) != 0;
      _last = i;
      return moved ? PairResult::Moved : PairResult::Refreshed;
    }
    uint8_t free_slot = kAutoPeerSlots;
    for (uint8_t i = 0; i < kAutoPeerSlots && free_slot == kAutoPeerSlots; ++i) if (!_slots[i].used) free_slot = i;
    if (free_slot == kAutoPeerSlots) { ++_full; return PairResult::Full; }
    if (!takeToken(now)) { ++_limited; return PairResult::RateLimited; }
    AutoPeer& p = _slots[free_slot];
    p = AutoPeer();
    p.used = true;
    memcpy(p.prefix, r.prefix, PREFIX_SIZE); memcpy(p.ip, r.ip, 4); p.port = r.port;
    p.until = now + kAutoPeerTtlMs;
    p.wants_observer = (r.flags & REPLY_WANTS_OBSERVER) != 0;
    _last = free_slot;
    ++_added;
    return PairResult::Added;
  }
  // Frees slots whose time is up; returns them as a bit mask (slot i = bit i).
  uint8_t expire(uint32_t now) {
    uint8_t mask = 0;
    for (uint8_t i = 0; i < kAutoPeerSlots; ++i)
      if (_slots[i].used && due(now, _slots[i].until)) { _slots[i].used = false; mask |= (uint8_t)(1U << i); }
    return mask;
  }
  void clear() { for (auto& p : _slots) p.used = false; }
  const AutoPeer& slot(uint8_t i) const { return _slots[i]; }
  uint8_t last() const { return _last; }
  uint8_t count() const { uint8_t n = 0; for (const auto& p : _slots) n += p.used ? 1 : 0; return n; }
  uint32_t added() const { return _added; }
  uint32_t full() const { return _full; }
  uint32_t limited() const { return _limited; }

private:
  bool takeToken(uint32_t now) {
    // kAutoPeerNewPerMinute tokens, one back every 60/N s.
    const uint32_t every = 60000U / kAutoPeerNewPerMinute;
    if (!_started) { _started = true; _tokens = kAutoPeerNewPerMinute; _at = now; }
    while (_tokens < kAutoPeerNewPerMinute && (uint32_t)(now - _at) >= every) { ++_tokens; _at += every; }
    if (_tokens == kAutoPeerNewPerMinute) _at = now;
    if (_tokens == 0) return false;
    --_tokens;
    return true;
  }
  AutoPeer _slots[kAutoPeerSlots];
  uint8_t _last = 0, _tokens = 0;
  uint32_t _at = 0, _added = 0, _full = 0, _limited = 0;
  bool _started = false;
};

// bridge.rxlimit: mesh packets per minute admitted from the ESP-NOW and UDP
// bridges into the repeater (and so towards its LoRa airtime); 0 = no limit.
// One shared token bucket, burst = a quarter minute's worth (at least 1).
constexpr uint16_t kRxLimitDefault = 120;
constexpr uint16_t kRxLimitMax = 6000;

class RxLimiter {
public:
  void configure(uint16_t per_minute) {
    if (per_minute > kRxLimitMax) per_minute = kRxLimitMax;
    if (per_minute != _rate) { _rate = per_minute; _started = false; }
  }
  uint16_t rate() const { return _rate; }
  uint16_t burst() const { return _rate == 0 ? 0 : (uint16_t)(_rate / 4 ? _rate / 4 : 1); }
  // True when the packet may pass; counts passes and drops.
  bool admit(uint32_t now) {
    if (_rate == 0) { ++_passed; return true; }
    const uint32_t cap = (uint32_t)burst() * 60000U;   // milli-token-minutes
    if (!_started) { _started = true; _last = now; _level = cap; }
    uint32_t elapsed = now - _last;
    _last = now;
    if (elapsed > 3600000U) elapsed = 3600000U;
    const uint64_t added = (uint64_t)elapsed * _rate;   // ms * pkts/min
    _level = (uint64_t)_level + added >= cap ? cap : (uint32_t)(_level + added);
    if (_level < 60000U) { ++_dropped; return false; }
    _level -= 60000U;
    ++_passed;
    return true;
  }
  uint32_t passed() const { return _passed; }
  uint32_t dropped() const { return _dropped; }
  void resetCounters() { _passed = _dropped = 0; }

private:
  uint16_t _rate = kRxLimitDefault;
  bool _started = false;
  uint32_t _last = 0, _level = 0, _passed = 0, _dropped = 0;
};

inline RxLimiter& rxLimiter() {
  static RxLimiter limiter;
  return limiter;
}

}}  // namespace mesh::discovery

namespace mesh { namespace discovery {
// Bridges hand received discovery frames to the role (main loop). `ip` is
// the UDP source address (null over ESP-NOW), `port` its source port.
typedef void (*FrameSink)(void* ctx, const uint8_t* frame, size_t len, const uint8_t* ip, uint16_t port);
}}  // namespace mesh::discovery
