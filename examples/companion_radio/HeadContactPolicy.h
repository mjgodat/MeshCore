#pragma once

// Repeater head (UI_REPEATER_HEAD): which contacts a head keeps, and how its
// admin console orders repeaters. Pure logic (no Arduino), host tested in
// test/test_head_contact_policy.py.
//
// A head reaches the mesh only through its own repeater's ESP-NOW bridge,
// which forwards what the repeater transmits. The repeater's own adverts
// therefore arrive with an empty path (0 hops); everything the repeater
// relays carries at least its own hash. That makes the "home repeater" the
// repeater whose advert arrives at 0 hops.
//
// Every relayed advert would otherwise be added (the table filled up with
// ~55 Puget Sound nodes, then again with 20+ chat companions the owner never
// talked to), so a head auto-adds only its home repeater (always, evicting
// an old heard repeater when full). Nothing else is added from an advert:
//   - chat companions are remembered in RAM (HeadHeardPeers.h) so their DMs
//     are still decrypted, shown and answered; one becomes a contact only
//     when the user sends it a DM (MyMesh::headSaveHeardPeer);
//   - repeaters, rooms and sensors other than home are not added at all.
// Explicit imports (the companion app's add-contact) are unaffected.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace head_policy {

// Same values as ADV_TYPE_* (helpers/AdvertDataHelpers.h).
constexpr uint8_t kTypeChat = 1;
constexpr uint8_t kTypeRepeater = 2;
constexpr uint8_t kTypeRoom = 3;
constexpr uint8_t kTypeSensor = 4;

constexpr size_t kHomePrefixLen = 6;

// An advert from the head's own repeater: a repeater heard at 0 hops (not a
// local contact import).
inline bool isHomeAdvert(uint8_t type, uint8_t path_hash_count, bool local_import) {
  return !local_import && type == kTypeRepeater && path_hash_count == 0;
}

// Auto-add a newly heard contact? Only the home repeater's own advert; the
// companion's auto-add settings do not apply on a head.
inline bool shouldAutoAdd(uint8_t type, bool home_advert) {
  return home_advert && type == kTypeRepeater;
}

// Eviction rank when the table is full: 0 never, 1 only if nothing better,
// 2 preferred (heard repeaters, rooms, sensors). The oldest of the best
// rank is evicted.
inline int evictRank(uint8_t type, bool favourite, bool is_home) {
  if (favourite || is_home) return 0;
  if (type == kTypeRepeater || type == kTypeRoom || type == kTypeSensor) return 2;
  return 1;
}

// "Clear heard contacts": removes heard repeaters, rooms and sensors; keeps
// the home repeater, favourites and chat contacts.
inline bool clearRemoves(uint8_t type, bool favourite, bool is_home) {
  return evictRank(type, favourite, is_home) == 2;
}

inline bool prefixMatches(const uint8_t* key, const uint8_t* home, size_t home_len) {
  return home != nullptr && home_len > 0 && memcmp(key, home, home_len) == 0;
}

// Admin console repeater list: home first, then most recently heard
// (lastmod, the head's clock), ties by table order. Sorts `idx` (contact
// indexes) in place; n is small (<= the contact table), insertion sort.
struct RepeaterKey {
  bool home;
  uint32_t heard;
};
template <typename KeyFn>
inline void sortRepeaters(uint16_t* idx, int n, KeyFn key) {
  auto before = [&](uint16_t a, uint16_t b) {
    const RepeaterKey ka = key(a), kb = key(b);
    if (ka.home != kb.home) return ka.home;
    if (ka.heard != kb.heard) return ka.heard > kb.heard;
    return a < b;
  };
  for (int i = 1; i < n; i++) {
    const uint16_t v = idx[i];
    int j = i - 1;
    while (j >= 0 && before(v, idx[j])) {
      idx[j + 1] = idx[j];
      j--;
    }
    idx[j + 1] = v;
  }
}

// First visible row of a scrolled list so `selected` is on screen.
inline int scrollToShow(int selected, int scroll, int rows, int count) {
  if (count <= rows) return 0;
  if (selected < scroll) scroll = selected;
  if (selected >= scroll + rows) scroll = selected - rows + 1;
  if (scroll > count - rows) scroll = count - rows;
  return scroll < 0 ? 0 : scroll;
}

// A contact name as the head's fonts can draw it (ASCII only; D-DIN has no
// emoji): non-ASCII characters are dropped and the spaces around them
// trimmed. A name with nothing readable left (an emoji only) becomes
// its type and 4-byte key prefix, e.g. "rptr 57f72334".
inline void displayName(const char* name, const uint8_t* key, uint8_t type, char* out, size_t size) {
  if (size == 0) return;
  size_t o = 0;
  bool readable = false;
  for (const unsigned char* p = (const unsigned char*)(name ? name : ""); *p && o + 1 < size; p++) {
    if (*p >= 0x80) continue;   // UTF-8 lead and continuation bytes
    if (*p == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
    if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) readable = true;
    out[o++] = (char)*p;
  }
  while (o > 0 && out[o - 1] == ' ') o--;
  out[o] = 0;
  if (readable) return;
  const char* kind = type == kTypeRepeater ? "rptr" : type == kTypeRoom ? "room"
      : type == kTypeSensor ? "sensor" : "node";
  if (key) snprintf(out, size, "%s %02x%02x%02x%02x", kind, key[0], key[1], key[2], key[3]);
  else snprintf(out, size, "%s", kind);
}

// Login reply in the terminal log since the login was sent.
enum class LoginReply : uint8_t { Pending, Accepted, Failed };
inline LoginReply classifyLoginReply(const char* tail) {
  if (strstr(tail, "accepted")) return LoginReply::Accepted;
  if (strstr(tail, "rejected") || strstr(tail, "ERROR:")) return LoginReply::Failed;
  return LoginReply::Pending;
}

}  // namespace head_policy
