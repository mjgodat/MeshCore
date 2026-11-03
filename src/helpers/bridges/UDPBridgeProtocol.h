#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mesh { namespace bridge { namespace udp {

// A LAN transport, independent of the selected ESP-NOW raw/wrapped format.
// Every field and payload is authenticated. HMAC does not encrypt contents.
static constexpr size_t HEADER_SIZE = 28;
static constexpr size_t TAG_SIZE = 16;
static constexpr size_t MAX_PAYLOAD = 255;
static constexpr size_t MAX_FRAME = HEADER_SIZE + MAX_PAYLOAD + TAG_SIZE;
// Observer (6) carries one ObserverFrame.h frame, repeater -> visual head,
// and only to a peer whose Subscribe (7, payload 1 byte: 1 on / 0 off) is
// current. Older bridges reject both kinds as malformed, so a head never
// receives observer frames from a repeater that cannot send them, and a
// repeater never sends them to a peer that did not ask.
enum class Kind : uint8_t { Hello = 1, Challenge = 2, Proof = 3, Mesh = 4, Probe = 5,
                            Observer = 6, Subscribe = 7 };
using Authenticate = bool (*)(void*, const uint8_t*, size_t, uint8_t digest[32]);
struct Frame {
  Kind kind = Kind::Hello;
  uint64_t sender = 0, recipient = 0;
  uint32_t sequence = 0;
  const uint8_t* payload = nullptr;
  uint16_t length = 0;
};
inline void put32(uint8_t* out, uint32_t v) {
  out[0] = v >> 24; out[1] = v >> 16; out[2] = v >> 8; out[3] = v;
}
inline uint32_t get32(const uint8_t* in) {
  return (uint32_t(in[0]) << 24) | (uint32_t(in[1]) << 16)
      | (uint32_t(in[2]) << 8) | in[3];
}
inline void put64(uint8_t* out, uint64_t v) { put32(out, v >> 32); put32(out + 4, v); }
inline uint64_t get64(const uint8_t* in) { return (uint64_t(get32(in)) << 32) | get32(in + 4); }
inline bool validShape(const Frame& f) {
  if (!f.sender || !f.sequence || f.length > MAX_PAYLOAD || (f.length && !f.payload)) return false;
  switch (f.kind) {
    case Kind::Hello: return !f.recipient && f.length == 0;
    case Kind::Challenge: case Kind::Proof: return f.recipient && f.length == 8 && get64(f.payload);
    case Kind::Mesh: return f.recipient && f.length >= 2;
    case Kind::Probe: return f.recipient && f.length == 4;
    case Kind::Observer: return f.recipient && f.length >= 6;
    case Kind::Subscribe: return f.recipient && f.length == 1 && f.payload[0] <= 1;
    default: return false;
  }
}
inline size_t encode(uint8_t* out, size_t capacity, const Frame& f,
                     Authenticate auth, void* context) {
  const size_t size = HEADER_SIZE + f.length + TAG_SIZE;
  if (!out || !auth || capacity < size || !validShape(f)) return 0;
  memcpy(out, "MCUD", 4); out[4] = 1; out[5] = uint8_t(f.kind);
  out[6] = f.length >> 8; out[7] = f.length;
  put64(out + 8, f.sender); put64(out + 16, f.recipient); put32(out + 24, f.sequence);
  if (f.length) memcpy(out + HEADER_SIZE, f.payload, f.length);
  uint8_t digest[32];
  if (!auth(context, out, HEADER_SIZE + f.length, digest)) return 0;
  memcpy(out + HEADER_SIZE + f.length, digest, TAG_SIZE);
  memset(digest, 0, sizeof(digest));
  return size;
}
enum class Decode { Ok, Malformed, Authentication };
inline Decode decode(const uint8_t* data, size_t size, Frame& result,
                     Authenticate auth, void* context) {
  if (!data || !auth || size < HEADER_SIZE + TAG_SIZE || size > MAX_FRAME
      || memcmp(data, "MCUD", 4) || data[4] != 1) return Decode::Malformed;
  Frame f;
  f.kind = Kind(data[5]); f.length = (uint16_t(data[6]) << 8) | data[7];
  f.sender = get64(data + 8); f.recipient = get64(data + 16); f.sequence = get32(data + 24);
  f.payload = data + HEADER_SIZE;
  if (size != HEADER_SIZE + f.length + TAG_SIZE || !validShape(f)) return Decode::Malformed;
  uint8_t digest[32];
  if (!auth(context, data, HEADER_SIZE + f.length, digest)) return Decode::Authentication;
  uint8_t different = 0;
  for (size_t i = 0; i < TAG_SIZE; ++i) different |= digest[i] ^ data[HEADER_SIZE + f.length + i];
  memset(digest, 0, sizeof(digest));
  if (different) return Decode::Authentication;
  result = f;
  return Decode::Ok;
}
inline bool due(uint32_t now, uint32_t deadline) { return int32_t(now - deadline) >= 0; }
inline void increment(uint32_t& value) { if (value != UINT32_MAX) ++value; }

// Each configured endpoint has its own authenticated receive session. A Hello
// never replaces a live session. Only proof of a fresh local challenge can do
// so, making recorded old sessions unable to flip the receiver backwards.
struct Session {
  uint64_t remote = 0, candidate = 0, challenge = 0;
  uint32_t received = 0, challenge_deadline = 0, next_hello = 0, challenge_sent_at = 0;

  bool request(uint64_t sender, uint64_t nonce, uint32_t now) {
    if (!sender || !nonce || (candidate && !due(now, challenge_deadline))) return false;
    candidate = sender; challenge = nonce; challenge_deadline = now + 5000; challenge_sent_at = now;
    return true;
  }
  bool prove(uint64_t sender, uint64_t nonce, uint32_t now) {
    if (!candidate || sender != candidate || nonce != challenge || due(now, challenge_deadline)) return false;
    if (remote != sender) { remote = sender; received = 0; }
    candidate = challenge = 0;
    return true;
  }
  bool admit(uint64_t sender, uint64_t recipient, uint64_t local, uint32_t sequence) {
    if (!remote || sender != remote || recipient != local || !sequence || sequence <= received) return false;
    received = sequence;
    return true;
  }
};

} } } // namespace mesh::bridge::udp
