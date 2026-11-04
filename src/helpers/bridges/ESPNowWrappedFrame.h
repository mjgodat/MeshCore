#pragma once

// The ESP-NOW "wrapped" frame, shared by the LoRa repeater's ESP-NOW bridge
// (ESPNowBridge, bridge.format wrapped) and primary ESP-NOW nodes
// (ESPNOWRadio, espnow.format wrapped). One codec, so both sides produce and
// accept byte-identical frames. Pure, header-only, C++11, no Arduino.
//
//   [0..1]  magic 0xC0 0x3E (BridgeBase::BRIDGE_PACKET_MAGIC, big-endian)
//   [2..3]  Fletcher-16 of the plaintext payload, big-endian  } XORed with
//   [4.. ]  payload, at most 246 bytes                          } the secret
//
// The XOR key is the secret's bytes (up to the first NUL), repeating, starting
// at the checksum's first byte. It isolates networks; it is not encryption.
// A receiver with another secret fails the checksum (1 in 65536 passes; the
// payload must then still parse).
//
// The payload is one serialized MeshCore packet, or, on links that carry
// them, one of our own frames (observer, discovery), recognised after
// unwrapping by their own magic. Receive is strict per format: a node in
// wrapped mode never accepts a raw frame and vice versa.

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <atomic>

namespace mesh { namespace bridge {

static constexpr uint8_t WRAPPED_MAGIC0 = 0xC0, WRAPPED_MAGIC1 = 0x3E;
static constexpr size_t WRAPPED_HEADER = 4;            // magic + checksum
static constexpr size_t WRAPPED_MAX_FRAME = 250;       // one ESP-NOW frame
static constexpr size_t WRAPPED_MAX_PAYLOAD = WRAPPED_MAX_FRAME - WRAPPED_HEADER;
static constexpr size_t WRAPPED_SECRET_SIZE = 16;      // 1-15 characters + NUL

inline uint16_t wrappedFletcher16(const uint8_t* data, size_t len) {
  uint8_t sum1 = 0, sum2 = 0;
  for (size_t i = 0; i < len; i++) {
    sum1 = (uint8_t)((sum1 + data[i]) % 255);
    sum2 = (uint8_t)((sum2 + sum1) % 255);
  }
  return (uint16_t)((sum2 << 8) | sum1);
}

// Length of a usable secret in a fixed buffer: 1..cap-1 characters before a
// NUL. 0 for an empty or unterminated buffer (never usable).
inline size_t wrappedSecretLength(const char* secret, size_t cap) {
  if (secret == nullptr || cap == 0) return 0;
  size_t n = 0;
  while (n < cap && secret[n] != 0) n++;
  return (n == 0 || n == cap) ? 0 : n;
}

inline void wrappedXor(uint8_t* data, size_t len, const char* secret, size_t key_len) {
  for (size_t i = 0; i < len; i++) data[i] ^= (uint8_t)secret[i % key_len];
}

inline bool isWrappedFrame(const uint8_t* frame, size_t len) {
  return frame != nullptr && len >= WRAPPED_HEADER && frame[0] == WRAPPED_MAGIC0
      && frame[1] == WRAPPED_MAGIC1;
}

// Wraps `payload` into `out`. Returns the frame length, or 0 when the payload
// is empty or over 246 bytes, `out` is too small, or the secret is unusable.
// `payload` and `out` may not overlap.
inline size_t wrapFrame(const uint8_t* payload, size_t len, const char* secret, size_t secret_cap,
                        uint8_t* out, size_t cap) {
  const size_t key_len = wrappedSecretLength(secret, secret_cap);
  if (payload == nullptr || out == nullptr || len == 0 || len > WRAPPED_MAX_PAYLOAD
      || cap < WRAPPED_HEADER + len || key_len == 0) return 0;
  out[0] = WRAPPED_MAGIC0;
  out[1] = WRAPPED_MAGIC1;
  const uint16_t checksum = wrappedFletcher16(payload, len);
  out[2] = (uint8_t)(checksum >> 8);
  out[3] = (uint8_t)checksum;
  memcpy(out + WRAPPED_HEADER, payload, len);
  wrappedXor(out + 2, len + 2, secret, key_len);
  return WRAPPED_HEADER + len;
}

enum class Unwrap : uint8_t { Ok, TooShort, Foreign, NoSecret, Checksum };

// Unwraps `frame` into `out` (the plaintext payload, `len` bytes). Strict:
// a frame shorter than the header (or with an empty payload) is TooShort, one
// without the magic Foreign; a wrong secret normally Checksum.
inline Unwrap unwrapFrame(const uint8_t* frame, size_t len, const char* secret, size_t secret_cap,
                          uint8_t* out, size_t cap, size_t& out_len) {
  out_len = 0;
  if (frame == nullptr || len < WRAPPED_HEADER) return Unwrap::TooShort;
  if (frame[0] != WRAPPED_MAGIC0 || frame[1] != WRAPPED_MAGIC1) return Unwrap::Foreign;
  if (len == WRAPPED_HEADER || len > WRAPPED_MAX_FRAME) return Unwrap::TooShort;
  const size_t key_len = wrappedSecretLength(secret, secret_cap);
  if (key_len == 0) return Unwrap::NoSecret;
  const size_t payload_len = len - WRAPPED_HEADER;
  if (out == nullptr || cap < payload_len) return Unwrap::TooShort;
  uint8_t sum[2] = {frame[2], frame[3]};
  wrappedXor(sum, 2, secret, key_len);
  memcpy(out, frame + WRAPPED_HEADER, payload_len);
  // The payload's key stream starts two bytes in (after the checksum).
  for (size_t i = 0; i < payload_len; i++) out[i] ^= (uint8_t)secret[(i + 2) % key_len];
  const uint16_t received = (uint16_t)((sum[0] << 8) | sum[1]);
  if (received != wrappedFletcher16(out, payload_len)) {
    memset(out, 0, payload_len);
    return Unwrap::Checksum;
  }
  out_len = payload_len;
  return Unwrap::Ok;
}

}}  // namespace mesh::bridge

namespace mesh { namespace espnow {

// The ESP-NOW wire format a primary ESP-NOW node (ESPNOWRadio) uses for this
// boot. Set once by the espnow.format/espnow.secret loader at boot (a saved
// change applies at the next reboot); raw until then. Read from the WiFi
// receive callback: the secret is written before `wrapped` is published.
struct ActiveLink {
  std::atomic<bool> wrapped{false};
  char secret[mesh::bridge::WRAPPED_SECRET_SIZE] = {};
};

inline ActiveLink& activeLink() {
  static ActiveLink link;
  return link;
}

// Applies a format for this boot. Wrapped needs a usable secret; otherwise
// the link stays raw and false is returned.
inline bool activateLink(bool wrapped, const char* secret) {
  ActiveLink& link = activeLink();
  if (!wrapped) {
    link.wrapped.store(false, std::memory_order_release);
    return true;
  }
  const size_t n = mesh::bridge::wrappedSecretLength(secret, mesh::bridge::WRAPPED_SECRET_SIZE);
  if (n == 0) return false;
  link.wrapped.store(false, std::memory_order_release);
  memset(link.secret, 0, sizeof(link.secret));
  memcpy(link.secret, secret, n);
  link.wrapped.store(true, std::memory_order_release);
  return true;
}

}}  // namespace mesh::espnow
