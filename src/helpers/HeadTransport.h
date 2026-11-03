#pragma once

// Visual head transport selection: ESP-NOW (default), UDP over WiFi, both, or
// auto (ESP-NOW with fail-over to UDP). Pure, header-only, C++11, no
// Arduino; host-tested by test/test_head_transport.py. The ESP32 side is
// src/helpers/esp32/HeadLinkRadio.* (HEAD_UDP_TRANSPORT builds).
//
//   espnow  mesh packets and the observer feed over ESP-NOW only (as before;
//           WiFi is never joined, nothing below runs)
//   udp     both over UDP to the repeater's UDP bridge; ESP-NOW is not used
//   both    receive on both (observer frames de-duplicated), send on both
//   auto    receive on both; send on ESP-NOW while the repeater's ESP-NOW
//           vitals keep arriving; after `transport.failover` seconds (30)
//           without them, send on UDP; back to ESP-NOW after 3 vitals in a
//           row and at least 30 s on UDP. Hysteresis both ways: after any
//           switch back to ESP-NOW (including a lost UDP session) it stays
//           on ESP-NOW at least 30 s before failing over again.
//
// Settings live in the head's NVS (namespace "headlink"); the WiFi password
// and the shared secret are never printed (`get udp` shows "set").
//
// Channel rule (ObserverTransport.h): the station joins an access point
// only on the head's ESP-NOW channel. both/auto are refused while the
// access point is known to be on another channel.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ObserverTransport.h"
#include "UDPBridgePrefs.h"

namespace mesh { namespace headlink {

enum class Mode : uint8_t { EspNow = 0, Udp = 1, Both = 2, Auto = 3 };

inline const char* modeName(Mode m) {
  switch (m) {
    case Mode::Udp: return "udp";
    case Mode::Both: return "both";
    case Mode::Auto: return "auto";
    default: return "espnow";
  }
}

inline bool parseMode(const char* text, Mode& out) {
  if (!text) return false;
  for (uint8_t i = 0; i < 4; ++i) {
    if (!strcmp(text, modeName((Mode)i))) { out = (Mode)i; return true; }
  }
  return false;
}

inline bool usesWiFi(Mode m) { return m != Mode::EspNow; }
inline bool needsSharedChannel(Mode m) { return m == Mode::Both || m == Mode::Auto; }

constexpr uint16_t kDefaultPort = 4404;
constexpr uint16_t kDefaultFailoverS = 30, kMinFailoverS = 10, kMaxFailoverS = 600;
constexpr size_t kSsidMax = 32, kPasswordMax = 63, kSecretMax = 15;  // secret = repeater bridge.secret

struct Config {
  Mode mode = Mode::EspNow;
  char ssid[kSsidMax + 1] = {};
  char password[kPasswordMax + 1] = {};
  char secret[kSecretMax + 1] = {};
  mesh::bridge::udp::Peer repeater;          // the repeater's IP and udp.port
  uint16_t local_port = kDefaultPort;        // this head listens here
  uint16_t failover_s = kDefaultFailoverS;
};

// Everything UDP needs: network, repeater address and the shared secret.
inline bool udpConfigured(const Config& c) {
  return c.ssid[0] && c.secret[0] && c.repeater.enabled() && c.local_port;
}

// ---- NVS image ---------------------------------------------------------------
// "HLT1", mode, ssid[33], password[64], secret[16], repeater ip[4] port[2],
// local port[2], failover[2], CRC-32 (little-endian) of everything before.

constexpr size_t kImageSize = 4 + 1 + 33 + 64 + 16 + 4 + 2 + 2 + 2 + 4;

inline uint32_t imageChecksum(const uint8_t* p, size_t n) {
  return mesh::observer::transportChecksum(p, n);
}

inline bool terminated(const char* s, size_t cap) { return memchr(s, 0, cap) != nullptr; }

inline bool validConfig(const Config& c) {
  if ((uint8_t)c.mode > 3 || !c.local_port) return false;
  if (!terminated(c.ssid, sizeof(c.ssid)) || !terminated(c.password, sizeof(c.password))
      || !terminated(c.secret, sizeof(c.secret))) return false;
  if (c.failover_s < kMinFailoverS || c.failover_s > kMaxFailoverS) return false;
  if (c.repeater.enabled() && !mesh::bridge::udp::validAddress(c.repeater.ip)) return false;
  if (!c.repeater.enabled()) for (uint8_t o : c.repeater.ip) if (o) return false;
  return true;
}

inline void encodeConfig(uint8_t image[kImageSize], const Config& c) {
  memset(image, 0, kImageSize);
  size_t i = 0;
  memcpy(image, "HLT1", 4); i = 4;
  image[i++] = (uint8_t)c.mode;
  memcpy(image + i, c.ssid, 33); i += 33;
  memcpy(image + i, c.password, 64); i += 64;
  memcpy(image + i, c.secret, 16); i += 16;
  memcpy(image + i, c.repeater.ip, 4); i += 4;
  image[i++] = (uint8_t)(c.repeater.port >> 8); image[i++] = (uint8_t)c.repeater.port;
  image[i++] = (uint8_t)(c.local_port >> 8); image[i++] = (uint8_t)c.local_port;
  image[i++] = (uint8_t)(c.failover_s >> 8); image[i++] = (uint8_t)c.failover_s;
  const uint32_t crc = imageChecksum(image, i);
  for (unsigned b = 0; b < 4; ++b) image[i++] = (uint8_t)(crc >> (8 * b));
}

inline bool decodeConfig(const uint8_t* image, size_t size, Config& out) {
  if (!image || size != kImageSize || memcmp(image, "HLT1", 4)) return false;
  uint32_t saved = 0;
  for (unsigned b = 0; b < 4; ++b) saved |= (uint32_t)image[kImageSize - 4 + b] << (8 * b);
  if (saved != imageChecksum(image, kImageSize - 4)) return false;
  Config c;
  size_t i = 4;
  c.mode = (Mode)image[i++];
  memcpy(c.ssid, image + i, 33); i += 33;
  memcpy(c.password, image + i, 64); i += 64;
  memcpy(c.secret, image + i, 16); i += 16;
  memcpy(c.repeater.ip, image + i, 4); i += 4;
  c.repeater.port = (uint16_t)((image[i] << 8) | image[i + 1]); i += 2;
  c.local_port = (uint16_t)((image[i] << 8) | image[i + 1]); i += 2;
  c.failover_s = (uint16_t)((image[i] << 8) | image[i + 1]);
  if (!validConfig(c)) return false;
  out = c;
  return true;
}

// ---- auto: fail-over / fail-back -------------------------------------------

class AutoSelector {
public:
  static const uint8_t kFailbackVitals = 3;       // ESP-NOW vitals in a row
  static const uint32_t kStreakGapMs = 12000;     // a longer gap restarts the count
  static const uint32_t kMinUdpDwellMs = 30000;   // least time on UDP before fail-back
  static const uint32_t kMinEspNowDwellMs = 30000; // least time on ESP-NOW before fail-over

  void begin(uint32_t now) {
    _on_udp = false; _last_espnow = now; _streak = 0; _seen = false; _since = now; _switched = false;
  }
  void noteEspNowVitals(uint32_t now) {
    _streak = (_seen && now - _last_espnow <= kStreakGapMs && _streak < 255) ? (uint8_t)(_streak + 1) : 1;
    _seen = true;
    _last_espnow = now;
  }
  // Returns true when the active transport changed.
  bool update(uint32_t now, uint32_t failover_ms, bool udp_ready) {
    if (!_on_udp) {
      if (!udp_ready || now - _last_espnow < failover_ms) return false;
      if (_switched && now - _since < kMinEspNowDwellMs) return false;   // no flapping
      _on_udp = true; _since = now; _streak = 0; ++_failovers; _switched = true;
      return true;
    }
    if (now - _last_espnow > kStreakGapMs) _streak = 0;
    const bool settled = _streak >= kFailbackVitals && now - _since >= kMinUdpDwellMs;
    if (!settled && udp_ready) return false;
    _on_udp = false; _since = now; ++_failbacks;   // UDP lost: ESP-NOW is all there is
    return true;
  }
  bool onUdp() const { return _on_udp; }
  bool seenEspNow() const { return _seen; }
  uint32_t lastEspNowMs() const { return _last_espnow; }
  uint32_t failovers() const { return _failovers; }
  uint32_t failbacks() const { return _failbacks; }

private:
  bool _on_udp = false, _seen = false, _switched = false;
  uint8_t _streak = 0;
  uint32_t _last_espnow = 0, _since = 0, _failovers = 0, _failbacks = 0;
};

// ---- observer frames arriving on both transports ---------------------------
// The same record sent over ESP-NOW and UDP is byte-identical, and every
// record carries the repeater's receive time (packets) or uptime (vitals), so
// a hash of the whole frame is the de-duplication key. No wire change: older
// repeaters and heads are unaffected. The window holds the last 64 frames:
// at the repeater's limit (20 records/s, bursts of 40) that is over 3 s of
// skew between the two copies, far more than WiFi and ESP-NOW latency differ.
// No sequence numbers are involved, so nothing wraps.

class FrameDeduper {
public:
  static const uint8_t kDepth = 64;
  static uint32_t key(const uint8_t* f, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ f[i]) * 16777619u;
    return h ^ (uint32_t)n;
  }
  // True when the frame was seen among the last kDepth frames; otherwise
  // remembers it and returns false.
  bool duplicate(const uint8_t* f, size_t n) {
    if (!f || !n) return false;
    const uint32_t k = key(f, n);
    for (uint8_t i = 0; i < _count; ++i) if (_keys[i] == k) { ++_dropped; return true; }
    _keys[_next] = k;
    _next = (uint8_t)((_next + 1) % kDepth);
    if (_count < kDepth) ++_count;
    return false;
  }
  void reset() { _count = _next = 0; }
  uint32_t dropped() const { return _dropped; }

private:
  uint32_t _keys[kDepth] = {};
  uint8_t _count = 0, _next = 0;
  uint32_t _dropped = 0;
};

// ---- status and CLI --------------------------------------------------------

enum class LinkState : uint8_t {
  Off,            // mode espnow
  NotConfigured,  // udp.* incomplete
  Joining,        // looking for / joining the access point
  WrongChannel,   // access point found on another channel (not joined)
  Connected,      // station up, waiting for the repeater's handshake
  Ready,          // authenticated session with the repeater
  NoMemory
};

inline const char* linkStateName(LinkState s) {
  switch (s) {
    case LinkState::NotConfigured: return "not-configured";
    case LinkState::Joining: return "joining";
    case LinkState::WrongChannel: return "wrong-channel";
    case LinkState::Connected: return "handshake";
    case LinkState::Ready: return "ready";
    case LinkState::NoMemory: return "no-memory";
    default: return "off";
  }
}

struct Status {
  LinkState udp = LinkState::Off;
  bool sending_udp = false;          // auto: currently on UDP
  uint8_t espnow_channel = 0;        // this head's ESP-NOW channel
  uint8_t ap_channel = 0;            // access point channel, 0 = not known
  uint8_t local_ip[4] = {};
  uint32_t espnow_vitals_age_s = UINT32_MAX;   // UINT32_MAX = none yet
  uint32_t udp_vitals_age_s = UINT32_MAX;
};

// Which transport carries this head's outgoing mesh packets right now.
inline const char* activeName(const Config& c, const Status& s) {
  switch (c.mode) {
    case Mode::Udp: return "udp";
    case Mode::Both: return "both";
    case Mode::Auto: return s.sending_udp ? "udp" : "espnow";
    default: return "espnow";
  }
}

inline void formatAge(char* out, size_t cap, uint32_t age_s) {
  if (age_s == UINT32_MAX) snprintf(out, cap, "-");
  else snprintf(out, cap, "%lus", (unsigned long)age_s);
}

// `get transport`: "> auto via espnow; espnow ch 6 vitals 3s; udp ready 192.168.1.60 vitals 4s"
inline void formatStatus(const Config& c, const Status& s, char* out, size_t cap) {
  char ev[12], uv[12];
  formatAge(ev, sizeof(ev), s.espnow_vitals_age_s);
  formatAge(uv, sizeof(uv), s.udp_vitals_age_s);
  if (c.mode == Mode::EspNow) {
    snprintf(out, cap, "> espnow; ch %u vitals %s", (unsigned)s.espnow_channel, ev);
    return;
  }
  int n = snprintf(out, cap, "> %s via %s; espnow ch %u vitals %s; udp %s", modeName(c.mode),
                   activeName(c, s), (unsigned)s.espnow_channel, ev, linkStateName(s.udp));
  if (n < 0 || (size_t)n >= cap) return;
  if (s.udp == LinkState::WrongChannel) {
    snprintf(out + n, cap - n, " (AP on ch %u)", (unsigned)s.ap_channel);
  } else if (s.udp == LinkState::Connected || s.udp == LinkState::Ready) {
    snprintf(out + n, cap - n, " %u.%u.%u.%u:%u vitals %s", s.local_ip[0], s.local_ip[1],
             s.local_ip[2], s.local_ip[3], (unsigned)c.local_port, uv);
  }
}

// Masked: the password and the secret are only ever "set" / "-".
inline void formatConfig(const Config& c, char* out, size_t cap) {
  char peer[24];
  mesh::bridge::udp::formatPeer(c.repeater, peer, sizeof(peer));
  snprintf(out, cap, "> ssid=%s password=%s repeater=%s port=%u secret=%s failover=%us",
           c.ssid[0] ? c.ssid : "-", c.password[0] ? "set" : "-", peer, (unsigned)c.local_port,
           c.secret[0] ? "set" : "-", (unsigned)c.failover_s);
}

enum class Result : uint8_t { NotMine, Handled, Changed };

namespace detail {
inline const char* valueAfter(const char* command, const char* key) {
  const size_t n = strlen(key);
  if (strncmp(command, key, n)) return nullptr;
  if (command[n] == 0) return command + n;
  if (command[n] != ' ' && command[n] != '\t') return nullptr;
  const char* v = command + n;
  while (*v == ' ' || *v == '\t') ++v;
  return v;
}
inline bool parseNumber(const char* text, uint32_t max, uint32_t& out) {
  if (!text || !*text) return false;
  uint32_t v = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    v = v * 10 + (uint32_t)(*text - '0');
    if (v > max) return false;
  }
  out = v;
  return true;
}
}  // namespace detail

// The head's transport commands (USB terminal and the on-screen settings use
// the same text). `status` is used by `get transport`. Changed = save the
// config and apply it.
inline Result handleCommand(Config& c, const char* command, char* reply, size_t cap,
                            const Status& status) {
  using detail::valueAfter;
  if (!command || !reply || !cap) return Result::NotMine;
  const char* v;
  if ((v = valueAfter(command, "get transport.failover"))) {
    if (*v) snprintf(reply, cap, "Error: use get transport.failover");
    else snprintf(reply, cap, "> %u", (unsigned)c.failover_s);
    return Result::Handled;
  }
  if ((v = valueAfter(command, "set transport.failover"))) {
    uint32_t s = 0;
    if (!detail::parseNumber(v, kMaxFailoverS, s) || s < kMinFailoverS) {
      snprintf(reply, cap, "Error: transport.failover is %u-%u seconds", (unsigned)kMinFailoverS,
               (unsigned)kMaxFailoverS);
      return Result::Handled;
    }
    c.failover_s = (uint16_t)s;
    snprintf(reply, cap, "OK - transport.failover %us", (unsigned)s);
    return Result::Changed;
  }
  if ((v = valueAfter(command, "get transport"))) {
    if (*v) snprintf(reply, cap, "Error: use get transport");
    else formatStatus(c, status, reply, cap);
    return Result::Handled;
  }
  if ((v = valueAfter(command, "set transport"))) {
    Mode m;
    if (!parseMode(v, m)) {
      snprintf(reply, cap, "Error: use set transport espnow|udp|both|auto");
      return Result::Handled;
    }
    if (needsSharedChannel(m) && mesh::observer::checkChannel(status.espnow_channel, status.ap_channel)
                                     == mesh::observer::ChannelCheck::Mismatch) {
      const int n = snprintf(reply, cap, "Error: %s refused. ", modeName(m));
      if (n > 0 && (size_t)n < cap)
        mesh::observer::formatChannelMismatch(reply + n, cap - (size_t)n, status.espnow_channel,
                                              status.ap_channel);
      return Result::Handled;
    }
    c.mode = m;
    snprintf(reply, cap, "OK - transport %s%s", modeName(m),
             usesWiFi(m) && !udpConfigured(c) ? "; set udp.ssid, udp.peer and udp.secret" : "");
    return Result::Changed;
  }
  if ((v = valueAfter(command, "get udp"))) {
    if (*v) snprintf(reply, cap, "Error: use get udp");
    else formatConfig(c, reply, cap);
    return Result::Handled;
  }
  if ((v = valueAfter(command, "set udp.ssid"))) {
    const size_t n = strlen(v);
    if (n == 0 || n > kSsidMax) { snprintf(reply, cap, "Error: udp.ssid is 1-32 characters"); return Result::Handled; }
    memcpy(c.ssid, v, n + 1);
    snprintf(reply, cap, "OK - udp.ssid saved");
    return Result::Changed;
  }
  if ((v = valueAfter(command, "set udp.password"))) {
    const size_t n = strlen(v);   // empty = open network
    if (n > kPasswordMax || (n > 0 && n < 8)) {
      snprintf(reply, cap, "Error: udp.password is 8-63 characters (empty for an open network)");
      return Result::Handled;
    }
    memcpy(c.password, v, n + 1);
    snprintf(reply, cap, "OK - udp.password %s", n ? "saved" : "cleared");
    return Result::Changed;
  }
  if ((v = valueAfter(command, "set udp.secret"))) {
    const size_t n = strlen(v);
    if (n == 0 || n > kSecretMax) {
      snprintf(reply, cap, "Error: udp.secret is the repeater's bridge.secret, 1-15 characters");
      return Result::Handled;
    }
    memcpy(c.secret, v, n + 1);
    snprintf(reply, cap, "OK - udp.secret saved");
    return Result::Changed;
  }
  if ((v = valueAfter(command, "set udp.peer"))) {
    mesh::bridge::udp::Peer p;
    if (!mesh::bridge::udp::parsePeer(v, p)) {
      snprintf(reply, cap, "Error: use set udp.peer <repeater IPv4:port|off>");
      return Result::Handled;
    }
    c.repeater = p;
    snprintf(reply, cap, "OK - udp.peer saved");
    return Result::Changed;
  }
  if ((v = valueAfter(command, "set udp.port"))) {
    uint16_t port = 0;
    if (!mesh::bridge::udp::parsePort(v, port)) {
      snprintf(reply, cap, "Error: udp.port is 1-65535");
      return Result::Handled;
    }
    c.local_port = port;
    snprintf(reply, cap, "OK - udp.port %u saved", (unsigned)port);
    return Result::Changed;
  }
  static const char* const kGets[] = {"get udp.ssid", "get udp.peer", "get udp.port", "get udp.password",
                                      "get udp.secret"};
  for (const char* key : kGets) {
    if (!(v = valueAfter(command, key)) || *v) continue;
    char peer[24];
    switch (key[8]) {
      case 's': snprintf(reply, cap, "> %s", key[9] == 's' ? (c.ssid[0] ? c.ssid : "-")
                                                          : (c.secret[0] ? "set" : "-")); break;
      case 'p':
        if (key[9] == 'e') { mesh::bridge::udp::formatPeer(c.repeater, peer, sizeof(peer)); snprintf(reply, cap, "> %s", peer); }
        else if (key[9] == 'o') snprintf(reply, cap, "> %u", (unsigned)c.local_port);
        else snprintf(reply, cap, "> %s", c.password[0] ? "set" : "-");
        break;
      default: break;
    }
    return Result::Handled;
  }
  return Result::NotMine;
}

}}  // namespace mesh::headlink
