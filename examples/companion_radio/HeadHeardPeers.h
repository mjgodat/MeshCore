#pragma once

// Repeater head (UI_REPEATER_HEAD): chat companions heard advertising that
// are NOT contacts. Pure logic (no Arduino), host tested in
// test/test_head_contact_policy.py.
//
// Heads no longer add chat companions from their adverts (HeadContactPolicy.h).
// A DM can only be decrypted with the sender's full public key, so the head
// remembers the last few companions it heard (key, name, advert time) here,
// in RAM only. MyMesh offers them to BaseChatMesh as "extra peers": a plain
// DM from one is shown and acknowledged without a contact being created. A
// contact is saved only when the user sends that companion a DM
// (MyMesh::headSaveHeardPeer), and the entry then leaves this cache.
//
// Bounded (N entries). When full, the least recently heard companion that
// never sent a DM goes first; DM senders (the user may still reply) go only
// when every entry is one, oldest first.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace head_policy {

template <size_t N>
class HeardPeers {
 public:
  static const size_t kKeyLen = 32;    // PUB_KEY_SIZE
  static const size_t kNameLen = 32;   // ContactInfo::name, with the NUL

  struct Entry {
    uint8_t key[kKeyLen];
    char name[kNameLen];
    uint32_t advert_ts;   // sender's advert timestamp (replay guard)
    uint32_t seq;         // LRU stamp; 0 = empty slot
    bool dm;              // has sent this head a DM
  };

  HeardPeers() { memset(_e, 0, sizeof(_e)); }

  // A (signature-checked) chat advert from a companion that is not a
  // contact. An advert no newer than the one already held is ignored (same
  // rule as contacts). Returns true when stored or updated.
  bool learn(const uint8_t* key, const char* name, uint32_t advert_ts) {
    if (key == nullptr || name == nullptr || name[0] == 0) return false;
    Entry* e = findMut(key, kKeyLen);
    if (e != nullptr) {
      if (advert_ts <= e->advert_ts) return false;
    } else {
      e = victim();
      memset(e, 0, sizeof(*e));
      memcpy(e->key, key, kKeyLen);
    }
    const size_t n = strnlen(name, kNameLen - 1);
    memcpy(e->name, name, n);
    memset(e->name + n, 0, kNameLen - n);
    e->advert_ts = advert_ts;
    e->seq = ++_seq;
    return true;
  }

  // The companion sent a DM (it was decrypted with this entry's key).
  bool markDm(const uint8_t* key) {
    Entry* e = findMut(key, kKeyLen);
    if (e == nullptr) return false;
    e->dm = true;
    e->seq = ++_seq;
    return true;
  }

  // Saved as a contact (or otherwise no longer needed).
  bool remove(const uint8_t* key) {
    Entry* e = findMut(key, kKeyLen);
    if (e == nullptr) return false;
    memset(e, 0, sizeof(*e));
    return true;
  }

  // The entry whose key starts with key[0..len) (1..32 bytes); the first one.
  const Entry* find(const uint8_t* key, size_t len) const {
    return const_cast<HeardPeers*>(this)->findMut(key, len);
  }

  // Slot i (0..capacity()-1); null when empty. Slots are stable until the
  // entry is evicted or removed.
  const Entry* at(size_t i) const { return i < N && _e[i].seq != 0 ? &_e[i] : nullptr; }
  size_t capacity() const { return N; }
  size_t count() const {
    size_t c = 0;
    for (size_t i = 0; i < N; i++) c += _e[i].seq != 0;
    return c;
  }

 private:
  Entry* findMut(const uint8_t* key, size_t len) {
    if (key == nullptr || len == 0 || len > kKeyLen) return nullptr;
    for (size_t i = 0; i < N; i++) {
      if (_e[i].seq != 0 && memcmp(_e[i].key, key, len) == 0) return &_e[i];
    }
    return nullptr;
  }

  // An empty slot, else the oldest non-DM entry, else the oldest entry.
  Entry* victim() {
    Entry* plain = nullptr;
    Entry* any = nullptr;
    for (size_t i = 0; i < N; i++) {
      Entry& e = _e[i];
      if (e.seq == 0) return &e;
      if (!e.dm && (plain == nullptr || e.seq < plain->seq)) plain = &e;
      if (any == nullptr || e.seq < any->seq) any = &e;
    }
    return plain != nullptr ? plain : any;
  }

  Entry _e[N];
  uint32_t _seq = 0;
};

}  // namespace head_policy
