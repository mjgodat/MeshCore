#pragma once

// Observer wire protocol: repeater -> visual head over ESP-NOW.
// Pure codec, header-only, no Arduino dependencies.
//
// Layout (little-endian, fixed): MAGIC[4] VERSION KIND, then the body.
//
// Collision analysis. The ESP-NOW receive path also carries MeshCore raw
// packets, raw-mode fragments (FE 4D 43 46) and WiChat frames. A MeshCore raw
// packet begins with header byte [ver:2 | type:4 | route:2]. Today only
// PAYLOAD_VER_1 (0b00) is valid (Packet.h, Packet.cpp: deserialize rejects
// ver > 0), so valid first bytes are 0x00..0x3F and today's MeshCore rejects any
// first byte >= 0x40. The magic starts with 0xB0 (ver bits 0b10, rejected today)
// and 0xFE (MCF/MCD) is not used. Future MeshCore versions might use those bits,
// so a one-byte marker is not enough: the 4-byte magic B0 42 53 4F plus an
// exact frame-length check (isObserverFrame) keeps an accidental match
// negligible. WiChat frames carry their magic at offsets 16/24, never at 0.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mesh { namespace observer {

constexpr uint8_t MAGIC0 = 0xB0, MAGIC1 = 0x42, MAGIC2 = 0x53, MAGIC3 = 0x4F;
constexpr uint8_t VERSION = 1;
enum class Kind : uint8_t { Packet = 1, Vitals = 2, Health = 3 };
enum class Disposition : uint8_t { Unknown = 0, Forwarded = 1, Duplicate = 2, Dropped = 3 };

// One ESP-NOW frame (250) minus the 15-byte packet header. MeshCore packets are
// up to 255 bytes (MAX_TRANS_UNIT); longer ones carry their first MAX_RAW bytes
// with `truncated` set (bit 7 of the disposition byte on the wire).
constexpr size_t MAX_RAW = 235;
constexpr uint8_t DISP_TRUNCATED = 0x80;
struct PacketRecord { uint32_t rx_ms; int8_t snr_q4; int16_t rssi; Disposition disp; bool truncated;
                      uint8_t len; uint8_t raw[MAX_RAW]; };
struct Vitals { uint32_t uptime_s; int16_t noise_floor; uint16_t rx_per_min, tx_per_min, dup_per_min;
                uint16_t airtime_permille; uint8_t espnow_nodes; uint16_t batt_mv;
                int32_t lat_e6, lon_e6; bool has_pos; float freq; float bw; uint8_t sf, cr; uint8_t preset_slot; };

constexpr size_t HEADER_SIZE = 6;
constexpr size_t PACKET_FIXED = HEADER_SIZE + 4 + 1 + 2 + 1 + 1;   // + raw[len]
constexpr size_t MAX_PACKET_FRAME = PACKET_FIXED + MAX_RAW;
constexpr size_t VITALS_FRAME = HEADER_SIZE + 4 + 2 + 2 + 2 + 2 + 2 + 1 + 2 + 4 + 4 + 1 + 4 + 4 + 1 + 1 + 1;
static_assert(MAX_PACKET_FRAME <= 250 && VITALS_FRAME <= 250, "must fit one ESP-NOW frame");
// In espnow.format/bridge.format wrapped every frame is wrapped (4 more
// bytes), so a record carries at most MAX_RAW_WRAPPED raw bytes there.
constexpr size_t MAX_WRAPPED_FRAME = 246;
constexpr size_t MAX_RAW_WRAPPED = MAX_WRAPPED_FRAME - PACKET_FIXED;
static_assert(MAX_RAW_WRAPPED == 231 && VITALS_FRAME <= MAX_WRAPPED_FRAME, "wrapped observer frames");

// Health (kind 3): the repeater's heap and last reset reason, sent next to
// the vitals (MyMesh: with the first vitals after a start, then every
// kHealthEveryVitals-th). A separate kind, not a longer vitals frame:
// deployed heads check the vitals length exactly and would drop all vitals.
// Those heads ignore kind 3 (not an observer frame for them; MeshCore then
// rejects its first byte, see the collision analysis above). Newer heads
// treat a missing health frame as "unknown".
// Forward compatible: the body starts with its own version byte, and a
// decoder accepts any length from HEALTH_FRAME_V1 up to MAX_WRAPPED_FRAME,
// ignoring bytes it does not know; a later version only appends fields.
constexpr uint8_t HEALTH_BODY_VERSION = 1;
// uptime_s makes every frame unique: a head on `transport both` drops
// byte-identical observer frames as the other transport's copy
// (HeadTransport.h FrameDeduper), and the heap often stays the same.
// Reset reason: the ESP-IDF esp_reset_reason_t value (0 = unknown); the
// numbers are stable in IDF 4.4 and 5.x (resetReasonName below).
struct Health { uint32_t uptime_s, free_heap, min_free_heap, largest_block; uint8_t reset_reason; };
constexpr size_t HEALTH_FRAME_V1 = HEADER_SIZE + 1 + 4 + 4 + 4 + 4 + 1;
static_assert(HEALTH_FRAME_V1 <= MAX_WRAPPED_FRAME, "health fits a wrapped frame");

namespace detail {
struct W {
  uint8_t* p; size_t i;  // no default member init: C++11 aggregate (ESP32 gnu++11)
  void u8(uint8_t v) { p[i++] = v; }
  void u16(uint16_t v) { u8(v & 0xFF); u8(v >> 8); }
  void u32(uint32_t v) { u16(v & 0xFFFF); u16((uint16_t)(v >> 16)); }
  void f32(float f) { uint32_t v; memcpy(&v, &f, 4); u32(v); }
  void head(Kind k) { u8(MAGIC0); u8(MAGIC1); u8(MAGIC2); u8(MAGIC3); u8(VERSION); u8((uint8_t)k); }
};
struct R {
  const uint8_t* p; size_t i;
  uint8_t u8() { return p[i++]; }
  uint16_t u16() { uint16_t a = u8(); return (uint16_t)(a | (u8() << 8)); }
  uint32_t u32() { uint32_t a = u16(); return a | ((uint32_t)u16() << 16); }
  float f32() { uint32_t v = u32(); float f; memcpy(&f, &v, 4); return f; }
};
}  // namespace detail

// Magic + version + kind + exact length implied by kind (packet: header-consistent
// 15 + len; vitals: fixed size; health: at least its version-1 size).
inline bool isObserverFrame(const uint8_t* buf, size_t len) {
  if (buf == nullptr || len < HEADER_SIZE) return false;
  if (buf[0] != MAGIC0 || buf[1] != MAGIC1 || buf[2] != MAGIC2 || buf[3] != MAGIC3 || buf[4] != VERSION) return false;
  if (buf[5] == (uint8_t)Kind::Vitals) return len == VITALS_FRAME;
  if (buf[5] == (uint8_t)Kind::Health) return len >= HEALTH_FRAME_V1 && len <= MAX_WRAPPED_FRAME;
  if (buf[5] == (uint8_t)Kind::Packet) return len >= PACKET_FIXED && buf[PACKET_FIXED - 1] <= MAX_RAW && len == PACKET_FIXED + buf[PACKET_FIXED - 1];
  return false;
}

// Copies a received packet into `r`: exact up to MAX_RAW bytes, otherwise the
// first MAX_RAW bytes with `truncated` set. Returns false for an empty packet.
inline bool setPacketBytes(PacketRecord& r, const uint8_t* raw, size_t len) {
  if (raw == nullptr || len == 0) return false;
  r.truncated = len > MAX_RAW;
  r.len = (uint8_t)(r.truncated ? MAX_RAW : len);
  memcpy(r.raw, raw, r.len);
  return true;
}

inline size_t encodePacket(const PacketRecord& r, uint8_t* out, size_t cap) {
  if (out == nullptr || r.len > MAX_RAW || (r.truncated && r.len != MAX_RAW)) return 0;
  size_t need = PACKET_FIXED + r.len;
  if (cap < need) return 0;
  detail::W w{out, 0};
  w.head(Kind::Packet);
  w.u32(r.rx_ms); w.u8((uint8_t)r.snr_q4); w.u16((uint16_t)r.rssi);
  if ((uint8_t)r.disp > (uint8_t)Disposition::Dropped) return 0;
  w.u8((uint8_t)((uint8_t)r.disp | (r.truncated ? DISP_TRUNCATED : 0))); w.u8(r.len);
  memcpy(out + w.i, r.raw, r.len); w.i += r.len;
  return w.i;
}

inline size_t encodeVitals(const Vitals& v, uint8_t* out, size_t cap) {
  if (out == nullptr || cap < VITALS_FRAME) return 0;
  detail::W w{out, 0};
  w.head(Kind::Vitals);
  w.u32(v.uptime_s); w.u16((uint16_t)v.noise_floor);
  w.u16(v.rx_per_min); w.u16(v.tx_per_min); w.u16(v.dup_per_min);
  w.u16(v.airtime_permille); w.u8(v.espnow_nodes); w.u16(v.batt_mv);
  w.u32((uint32_t)v.lat_e6); w.u32((uint32_t)v.lon_e6); w.u8(v.has_pos ? 1 : 0);
  w.f32(v.freq); w.f32(v.bw); w.u8(v.sf); w.u8(v.cr); w.u8(v.preset_slot);
  return w.i;
}

// Status frames (vitals, health) bypass the packet-record rate limit.
inline bool isStatusFrame(const uint8_t* buf, size_t len) {
  return isObserverFrame(buf, len) && (buf[5] == (uint8_t)Kind::Vitals || buf[5] == (uint8_t)Kind::Health);
}

inline size_t encodeHealth(const Health& h, uint8_t* out, size_t cap) {
  if (out == nullptr || cap < HEALTH_FRAME_V1) return 0;
  detail::W w{out, 0};
  w.head(Kind::Health);
  w.u8(HEALTH_BODY_VERSION);
  w.u32(h.uptime_s); w.u32(h.free_heap); w.u32(h.min_free_heap); w.u32(h.largest_block); w.u8(h.reset_reason);
  return w.i;
}

// Accepts body version 1 and later (fields appended after the version-1
// ones are ignored); version 0 is invalid.
inline bool decodeHealth(const uint8_t* buf, size_t len, Health& out) {
  if (!isObserverFrame(buf, len) || buf[5] != (uint8_t)Kind::Health) return false;
  detail::R r{buf, HEADER_SIZE};
  if (r.u8() == 0) return false;
  Health h;
  h.uptime_s = r.u32(); h.free_heap = r.u32(); h.min_free_heap = r.u32(); h.largest_block = r.u32(); h.reset_reason = r.u8();
  out = h;
  return true;
}

// Short name of an esp_reset_reason_t value (Health::reset_reason).
inline const char* resetReasonName(uint8_t reason) {
  static const char* const kNames[] = {"unknown", "power-on", "ext reset", "software", "panic", "int wdt",
                                       "task wdt", "wdt", "deep sleep", "brownout", "sdio"};
  return reason < sizeof(kNames) / sizeof(kNames[0]) ? kNames[reason] : "other";
}

inline bool decodePacket(const uint8_t* buf, size_t len, PacketRecord& out) {
  if (!isObserverFrame(buf, len) || buf[5] != (uint8_t)Kind::Packet) return false;
  detail::R r{buf, HEADER_SIZE};
  uint32_t rx = r.u32(); int8_t snr = (int8_t)r.u8(); int16_t rssi = (int16_t)r.u16();
  uint8_t disp = r.u8(), n = r.u8();
  const bool truncated = (disp & DISP_TRUNCATED) != 0;
  disp &= (uint8_t)~DISP_TRUNCATED;
  // A truncated record carries MAX_RAW bytes, or MAX_RAW_WRAPPED when it was
  // cut to fit a wrapped ESP-NOW frame (fitWrappedPacketFrame).
  if (disp > (uint8_t)Disposition::Dropped || n > MAX_RAW
      || (truncated && n != MAX_RAW && n != MAX_RAW_WRAPPED)) return false;
  out.rx_ms = rx; out.snr_q4 = snr; out.rssi = rssi; out.disp = (Disposition)disp;
  out.truncated = truncated; out.len = n;
  memcpy(out.raw, buf + r.i, n);
  if (n < MAX_RAW) memset(out.raw + n, 0, MAX_RAW - n);
  return true;
}

// Shrinks an encoded frame in place to fit a wrapped ESP-NOW frame (246
// bytes for the observer frame, ESPNowWrappedFrame.h): a longer packet record
// keeps its first MAX_RAW_WRAPPED raw bytes and is marked truncated. Returns
// the new length (unchanged when it already fits), 0 for a non-frame.
inline size_t fitWrappedPacketFrame(uint8_t* frame, size_t len) {
  if (!isObserverFrame(frame, len)) return 0;
  if (len <= MAX_WRAPPED_FRAME) return len;
  if (frame[5] != (uint8_t)Kind::Packet) return 0;
  frame[PACKET_FIXED - 2] |= DISP_TRUNCATED;
  frame[PACKET_FIXED - 1] = (uint8_t)MAX_RAW_WRAPPED;
  return MAX_WRAPPED_FRAME;
}

inline bool decodeVitals(const uint8_t* buf, size_t len, Vitals& out) {
  // Strict length (like decodePacket): isObserverFrame enforces it.
  if (!isObserverFrame(buf, len) || buf[5] != (uint8_t)Kind::Vitals) return false;
  detail::R r{buf, HEADER_SIZE};
  Vitals v;
  v.uptime_s = r.u32(); v.noise_floor = (int16_t)r.u16();
  v.rx_per_min = r.u16(); v.tx_per_min = r.u16(); v.dup_per_min = r.u16();
  v.airtime_permille = r.u16(); v.espnow_nodes = r.u8(); v.batt_mv = r.u16();
  v.lat_e6 = (int32_t)r.u32(); v.lon_e6 = (int32_t)r.u32();
  uint8_t hp = r.u8();
  if (hp > 1) return false;
  v.has_pos = hp != 0;
  v.freq = r.f32(); v.bw = r.f32(); v.sf = r.u8(); v.cr = r.u8(); v.preset_slot = r.u8();
  out = v;
  return true;
}

}}  // namespace mesh::observer
