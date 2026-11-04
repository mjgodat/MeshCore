#pragma once

// The key behind discovery tags (DiscoveryFrame.h). The shared secret is only
// 1-15 characters and an announce goes out every few seconds, so a recorded
// frame is an offline guessing oracle. The tag key is therefore derived once
// per secret with PBKDF2-HMAC-SHA256 (kKeyIterations, salt kKeySalt, one
// 32-byte block) and cached; each tag is then one HMAC with that key over
// "MeshCore discovery v1\0" || frame. Deriving costs some 100 ms on an ESP32
// and happens at the first frame after boot or after a secret change, never
// per frame.
//
// Header-only, C++11. Uses the rweather <SHA256.h> API (the host tests use
// the real SHA-256 in test/mocks/SHA256.h).

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <SHA256.h>

#include "DiscoveryFrame.h"

namespace mesh { namespace discovery {

constexpr uint32_t kKeyIterations = 2048;
constexpr size_t KEY_SIZE = 32;
static const char kKeySalt[] = "MeshCore discovery key v1";   // without its NUL

inline void hmacSha256(const uint8_t* key, size_t key_len, const uint8_t* a, size_t a_len,
                       const uint8_t* b, size_t b_len, uint8_t out[32]) {
  SHA256 sha;
  sha.resetHMAC(key, key_len);
  if (a_len) sha.update(a, a_len);
  if (b_len) sha.update(b, b_len);
  sha.finalizeHMAC(key, key_len, out, 32);
}

// PBKDF2-HMAC-SHA256, first (and only) 32-byte block.
inline bool deriveKey(const char* secret, size_t secret_len, uint8_t out[KEY_SIZE]) {
  if (!secret || !secret_len) return false;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(secret);
  const uint8_t block[4] = {0, 0, 0, 1};
  uint8_t salt[sizeof(kKeySalt) - 1 + 4];
  memcpy(salt, kKeySalt, sizeof(kKeySalt) - 1);
  memcpy(salt + sizeof(kKeySalt) - 1, block, 4);
  uint8_t u[32];
  hmacSha256(p, secret_len, salt, sizeof(salt), nullptr, 0, u);
  memcpy(out, u, KEY_SIZE);
  for (uint32_t i = 1; i < kKeyIterations; ++i) {
    hmacSha256(p, secret_len, u, sizeof(u), nullptr, 0, u);
    for (size_t k = 0; k < KEY_SIZE; ++k) out[k] ^= u[k];
  }
  memset(u, 0, sizeof(u));
  return true;
}

// One derived key, re-derived only when the secret changes.
class KeyCache {
public:
  // The key for `secret` (1-15 characters), or null when it is unusable.
  const uint8_t* get(const char* secret) {
    const size_t n = secret ? strnlen(secret, sizeof(_secret)) : 0;
    if (n == 0 || n >= sizeof(_secret)) return nullptr;
    if (_valid && strlen(_secret) == n && !memcmp(_secret, secret, n)) return _key;
    clear();
    if (!deriveKey(secret, n, _key)) return nullptr;
    memcpy(_secret, secret, n);
    _valid = true;
    ++_derivations;
    return _key;
  }
  void clear() {
    memset(_secret, 0, sizeof(_secret));
    memset(_key, 0, sizeof(_key));
    _valid = false;
  }
  uint32_t derivations() const { return _derivations; }

private:
  char _secret[16] = {};
  uint8_t _key[KEY_SIZE] = {};
  bool _valid = false;
  uint32_t _derivations = 0;
};

// A Mac (DiscoveryFrame.h) whose ctx is a derived 32-byte key.
inline bool keyedMac(void* ctx, const uint8_t* data, size_t len, uint8_t digest[32]) {
  if (!ctx) return false;
  hmacSha256(static_cast<const uint8_t*>(ctx), KEY_SIZE, kDomain, sizeof(kDomain), data, len, digest);
  return true;
}

}}  // namespace mesh::discovery
