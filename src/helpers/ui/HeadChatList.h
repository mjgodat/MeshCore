#pragma once

// Conversation list + thread helpers for a repeater head's chat screen
// (UI_HEAD_CHAT_LIST, HeadChats.h). Pure C++11: works on any message store
// shaped like CompanionMessageHistory (newest(age), count(), Entry with
// channel_idx / origin / message / unread / own_sent / heard_millis), so the
// screen reuses the companion's buffered messages instead of copying them.
//
// Conversations: one per channel (channel_idx >= 0) and one per direct-message
// peer (channel_idx < 0, keyed by the origin's name before " [", the same rule
// as CompanionMessageHistory::sameThread). Rows with messages come first,
// newest activity first; rows without messages follow in the order given
// (configured channels, then saved chat contacts).

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mesh {
namespace ui {
namespace chats {

static const int kDirect = -1;   // ChatRow::channel_idx of a direct-message thread

struct ChatRow {
  int channel_idx;      // >= 0 channel; kDirect = DM with `name`
  char name[34];        // channel name, or the DM peer's (raw) name
  int newest_age;       // store age of the newest message (0 = newest overall); -1 = none
  int count;            // messages in the store for this conversation
  int unread;
};

// Length of the peer name in a DM origin ("Alice [2h]:" -> 5).
inline size_t peerLength(const char* origin) {
  if (origin == nullptr) return 0;
  const char* route = strstr(origin, " [");
  return route == nullptr ? strlen(origin) : (size_t)(route - origin);
}

inline bool samePeer(const char* origin, const char* name) {
  const size_t n = peerLength(origin);
  return n == strlen(name) && memcmp(origin, name, n) == 0;
}

template <class Entry>
bool inThread(const Entry& e, const ChatRow& row) {
  if (row.channel_idx >= 0) return e.channel_idx == row.channel_idx;
  return e.channel_idx < 0 && samePeer(e.origin, row.name);
}

// Sender and body of a stored message. Own posts say "You"; a DM is from its
// peer; a channel post carries "Name: text" (split at the first ": " within
// the first 32 bytes, else no sender).
template <class Entry>
const char* splitSender(const Entry& e, char* sender, size_t cap) {
  if (cap == 0) return e.message;
  sender[0] = 0;
  if (e.own_sent) {
    snprintf(sender, cap, "You");
    if (e.channel_idx >= 0) {
      // A channel post stored as "Name: text" shows only the text.
      const char* colon = strstr(e.message, ": ");
      const size_t n = peerLength(e.origin);
      if (colon != nullptr && (size_t)(colon - e.message) == n && memcmp(e.message, e.origin, n) == 0)
        return colon + 2;
    }
    return e.message;
  }
  if (e.channel_idx < 0) {
    const size_t n = peerLength(e.origin);
    snprintf(sender, cap, "%.*s", (int)n, e.origin);
    return e.message;
  }
  const char* colon = strstr(e.message, ": ");
  if (colon == nullptr || colon == e.message || colon - e.message > 32) return e.message;
  snprintf(sender, cap, "%.*s", (int)(colon - e.message), e.message);
  return colon + 2;
}

namespace detail {
inline void copyName(char* out, size_t cap, const char* s, size_t n) {
  if (cap == 0) return;
  if (n >= cap) n = cap - 1;
  memcpy(out, s, n);
  out[n] = 0;
}
}  // namespace detail

// Builds the conversation rows. `visible(entry)` filters the store (inbox
// mode); channels[i] / channel_names[i] are the configured channels, dm_names
// the saved chat contacts' names. Returns rows written (at most cap).
template <class History, class Visible>
int buildRows(const History& h, Visible visible, const int* channels, const char* const* channel_names, int nch,
              const char* const* dm_names, int ndm, ChatRow* out, int cap) {
  int n = 0;
  // Every conversation that has messages, in newest-first order of activity.
  for (size_t age = 0; age < h.count(); ++age) {
    const typename History::Entry* e = h.newest(age);
    if (e == nullptr || !visible(*e)) continue;
    int found = -1;
    for (int r = 0; r < n && found < 0; ++r)
      if (inThread(*e, out[r])) found = r;
    if (found < 0) {
      if (n >= cap) continue;
      ChatRow& row = out[n++];
      memset(&row, 0, sizeof(row));
      row.channel_idx = e->channel_idx >= 0 ? e->channel_idx : kDirect;
      row.newest_age = (int)age;
      if (e->channel_idx >= 0) {
        const char* name = e->channel_name;
        for (int c = 0; c < nch; ++c)
          if (channels[c] == e->channel_idx && channel_names[c] != nullptr && channel_names[c][0]) name = channel_names[c];
        if (name[0]) detail::copyName(row.name, sizeof(row.name), name, strlen(name));
        else snprintf(row.name, sizeof(row.name), "Channel %d", e->channel_idx);
      } else {
        detail::copyName(row.name, sizeof(row.name), e->origin, peerLength(e->origin));
      }
      found = n - 1;
    }
    out[found].count++;
    if (e->unread) out[found].unread++;
  }
  // Quiet channels, then quiet contacts.
  for (int c = 0; c < nch && n < cap; ++c) {
    bool have = false;
    for (int r = 0; r < n && !have; ++r) have = out[r].channel_idx == channels[c];
    if (have) continue;
    ChatRow& row = out[n++];
    memset(&row, 0, sizeof(row));
    row.channel_idx = channels[c];
    row.newest_age = -1;
    const char* name = channel_names[c] != nullptr ? channel_names[c] : "";
    if (name[0]) detail::copyName(row.name, sizeof(row.name), name, strlen(name));
    else snprintf(row.name, sizeof(row.name), "Channel %d", channels[c]);
  }
  for (int d = 0; d < ndm && n < cap; ++d) {
    if (dm_names[d] == nullptr || dm_names[d][0] == 0) continue;
    bool have = false;
    for (int r = 0; r < n && !have; ++r) have = out[r].channel_idx < 0 && strcmp(out[r].name, dm_names[d]) == 0;
    if (have) continue;
    ChatRow& row = out[n++];
    memset(&row, 0, sizeof(row));
    row.channel_idx = kDirect;
    row.newest_age = -1;
    detail::copyName(row.name, sizeof(row.name), dm_names[d], strlen(dm_names[d]));
  }
  return n;
}

// Word wrap: the length of the next line of `s` that fits `width`
// (measure(p, n) gives the width of the n bytes at p; it is called one
// character at a time and summed), breaking at a space when it can, inside a
// word when it must, always at a newline. *next is where the following line
// starts (the spaces or newline after it consumed); the returned length has
// no trailing spaces and may be 0 (an empty line).
template <class Measure>
size_t nextLine(const char* s, int width, Measure measure, const char** next) {
  const size_t total = strlen(s);
  size_t fit = 0, lastSpace = 0;
  int used = 0;
  bool haveSpace = false;
  while (fit < total && s[fit] != '\n') {
    // Grow by one UTF-8 character.
    size_t step = 1;
    while (fit + step < total && ((unsigned char)s[fit + step] & 0xC0) == 0x80) ++step;
    const int w = measure(s + fit, step);
    if (used + w > width && fit > 0) break;
    used += w;
    if (s[fit] == ' ') { lastSpace = fit; haveSpace = true; }
    fit += step;
  }
  size_t len = fit;
  if (fit < total && s[fit] != '\n' && s[fit] != ' ' && haveSpace && lastSpace > 0) len = lastSpace;
  const char* p = s + len;
  if (*p == '\n') ++p;
  else while (*p == ' ') ++p;
  *next = p;
  while (len > 0 && s[len - 1] == ' ') --len;   // trailing spaces are not drawn
  return len;
}

// Lines `s` takes at `width` (at least 1).
template <class Measure>
int countLines(const char* s, int width, Measure measure) {
  int lines = 0;
  const char* p = s;
  while (*p) {
    const char* next = p;
    nextLine(p, width, measure, &next);
    ++lines;
    if (next == p) break;   // no progress (cannot happen with width >= 1 glyph)
    p = next;
  }
  return lines == 0 ? 1 : lines;
}

inline int clampScroll(int scroll, int content, int view) {
  const int max = content > view ? content - view : 0;
  return scroll < 0 ? 0 : (scroll > max ? max : scroll);
}

}  // namespace chats
}  // namespace ui
}  // namespace mesh
