#pragma once

// Which transport carries the observer feed (repeater -> visual head), and
// the ESP-NOW / WiFi channel rule shared by the repeater and the heads.
// Pure, header-only, C++11, no Arduino; host-tested by
// test/test_observer_transport.py.
//
// Repeater: `set observer.transport espnow|udp|both` (default espnow).
//   espnow  observer frames go out over the ESP-NOW bridge (as before)
//   udp     observer frames go to subscribed UDP bridge peers only
//   both    every admitted frame goes out on both (one shared rate limit)
// `observer on|off` (alias of the older `espnow.observer`) still switches the
// feed itself; the transport only chooses where it goes.
//
// Channel rule. An ESP32 has one 2.4 GHz radio: ESP-NOW and a WiFi station
// always share one channel. ESP-NOW stays on its configured channel (the
// repeater's bridge.channel, a head's espnow.channel); a station only joins
// an access point on that same channel (WiFiStationPolicy.h scans that one
// channel). So a setting that needs ESP-NOW and WiFi at the same time
// (repeater `both`, head `both` / `auto`) is refused when the access point
// is known to be on another channel, and the reply says how to fix it. The
// ESP-NOW channel is never moved behind the other ESP-NOW nodes' backs.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mesh { namespace observer {

enum class Transport : uint8_t { EspNow = 0, Udp = 1, Both = 2 };

inline const char* transportName(Transport t) {
  switch (t) {
    case Transport::Udp: return "udp";
    case Transport::Both: return "both";
    default: return "espnow";
  }
}

inline bool parseTransport(const char* text, Transport& out) {
  if (!text) return false;
  if (!strcmp(text, "espnow")) { out = Transport::EspNow; return true; }
  if (!strcmp(text, "udp")) { out = Transport::Udp; return true; }
  if (!strcmp(text, "both")) { out = Transport::Both; return true; }
  return false;
}

inline bool usesEspNow(Transport t) { return t == Transport::EspNow || t == Transport::Both; }
inline bool usesUdp(Transport t) { return t == Transport::Udp || t == Transport::Both; }

// ---- channel rule ----------------------------------------------------------

enum class ChannelCheck : uint8_t { Ok, Unknown, Mismatch };

// ap_channel 0 = not known yet (WiFi not joined and no scan result).
inline ChannelCheck checkChannel(uint8_t espnow_channel, uint8_t ap_channel) {
  if (ap_channel == 0 || espnow_channel == 0) return ChannelCheck::Unknown;
  return ap_channel == espnow_channel ? ChannelCheck::Ok : ChannelCheck::Mismatch;
}

// The one warning text, used by the CLI replies and the head's screen.
inline void formatChannelMismatch(char* out, size_t cap, uint8_t espnow_channel, uint8_t ap_channel) {
  if (!out || !cap) return;
  snprintf(out, cap, "WiFi AP on ch %u, ESP-NOW on ch %u: set the router to ch %u or "
           "ch %u on all ESP-NOW nodes (bridge.channel/espnow.channel)",
           (unsigned)ap_channel, (unsigned)espnow_channel, (unsigned)espnow_channel,
           (unsigned)ap_channel);
}

// ---- saved setting (/observer_transport) -------------------------------------
// "OBT" 0x01, transport, CRC-32 (little-endian) of the first five bytes. A
// missing, short or corrupt record means espnow (the default).

constexpr size_t kTransportImageSize = 9;

inline uint32_t transportChecksum(const uint8_t* bytes, size_t size) {
  uint32_t crc = 0xffffffffU;
  while (size--) {
    crc ^= *bytes++;
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
  }
  return ~crc;
}

inline void encodeTransport(uint8_t image[kTransportImageSize], Transport t) {
  memcpy(image, "OBT\1", 4);
  image[4] = (uint8_t)t;
  const uint32_t crc = transportChecksum(image, 5);
  for (unsigned i = 0; i < 4; ++i) image[5 + i] = (uint8_t)(crc >> (8 * i));
}

inline bool decodeTransport(const uint8_t* image, size_t size, Transport& out) {
  if (!image || size != kTransportImageSize || memcmp(image, "OBT\1", 4) || image[4] > 2) return false;
  uint32_t saved = 0;
  for (unsigned i = 0; i < 4; ++i) saved |= (uint32_t)image[5 + i] << (8 * i);
  if (saved != transportChecksum(image, 5)) return false;
  out = (Transport)image[4];
  return true;
}

// `set observer.transport <value>` on the repeater: false with `reply` set
// when refused. `both` is refused while the station is joined (or its AP was
// seen) on a channel other than the ESP-NOW bridge channel; `udp` alone is
// accepted with a note, because the ESP-NOW feed is then not used.
inline bool checkTransportChange(Transport wanted, uint8_t espnow_channel, uint8_t ap_channel,
                                 char* reply, size_t cap) {
  if (wanted == Transport::Both
      && checkChannel(espnow_channel, ap_channel) == ChannelCheck::Mismatch) {
    const int used = snprintf(reply, cap, "Error: both refused. ");
    if (used > 0 && (size_t)used < cap)
      formatChannelMismatch(reply + used, cap - (size_t)used, espnow_channel, ap_channel);
    return false;
  }
  return true;
}

}}  // namespace mesh::observer
