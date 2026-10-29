#pragma once

// Repeater head (UI_REPEATER_HEAD): parsing for the repeater's radio preset
// replies, kept free of Arduino code so it can be host-tested.
//
// `get preset` replies (at most 160 bytes) in one of two forms, optionally
// prefixed by "pending, reverts in <N>m; " while a remote load is waiting to
// be committed:
//   full:       "1*USA-Canada 910.5250244,62.5,7,5\n2 Test 925.5,62.5,9,5\n3 -\n4 -\n5 -"
//               ('*' = active, '-' = empty slot, the tuple may carry a 5th
//               ",preamble" field)
//   names only: "1*Name 2 Name 3 - 4 - 5 - (get preset N)"   (single line)
// The text may be the head terminal log tail: the last "CLI -> from" header
// line is skipped, as are "  ", "  -> " and "> " markers and CRs.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct HeadPresetSlot {
  bool used;
  bool active;
  char name[13];
  char freq[12];  // MHz for display, at most 3 decimals; empty when unknown
};

struct HeadPresetList {
  HeadPresetSlot slot[5];
  bool pending;
  uint8_t pending_minutes;
  bool names_only;
};

namespace head_presets_detail {

inline bool hp_digit(char c) { return c >= '0' && c <= '9'; }
inline bool hp_blank(char c) { return c == ' ' || c == '\t'; }
inline bool hp_eol(char c) { return c == '\n' || c == '\r'; }

// Skips whitespace, line breaks and the terminal's "->" / ">" markers.
inline const char* hp_skipMarkers(const char* p) {
  for (;;) {
    if (hp_blank(*p) || hp_eol(*p) || *p == '>') p++;
    else if (p[0] == '-' && p[1] == '>') p += 2;
    else return p;
  }
}

inline void hp_copyTrimmed(const char* start, const char* end, char* out, size_t size) {
  while (start < end && hp_blank(*start)) start++;
  while (end > start && hp_blank(end[-1])) end--;
  size_t n = (size_t)(end - start);
  if (n >= size) n = size - 1;
  memcpy(out, start, n);
  out[n] = 0;
}

}  // namespace head_presets_detail

namespace hpd = head_presets_detail;

// "910.5250244" -> "910.525", "925.500" -> "925.5"; non-numbers are copied.
inline void headPresetTrimFreq(const char* in, char* out, size_t size) {
  if (size == 0) return;
  out[0] = 0;
  if (!in) return;
  char* end = nullptr;
  const double v = strtod(in, &end);
  if (end == in || *end != 0) {
    snprintf(out, size, "%s", in);
    return;
  }
  snprintf(out, size, "%.3f", v);
  char* dot = strchr(out, '.');
  if (!dot) return;
  char* last = out + strlen(out) - 1;
  while (last > dot && *last == '0') *last-- = 0;
  if (last == dot) *dot = 0;
}

inline bool parseHeadPresetReply(const char* text, HeadPresetList& out) {
  memset(&out, 0, sizeof(out));
  if (!text) return false;

  const char* p = text;
  const char* header = nullptr;
  for (const char* h = strstr(p, "CLI -> from"); h; h = strstr(h + 1, "CLI -> from")) header = h;
  if (header) {
    const char* nl = strchr(header, '\n');
    if (!nl) return false;
    p = nl + 1;
  }
  p = hpd::hp_skipMarkers(p);

  if (strncmp(p, "pending", 7) == 0) {
    const char* semi = strchr(p, ';');
    const char* r = strstr(p, "reverts in ");
    if (!semi || !r || r > semi) return false;
    r += 11;
    if (!hpd::hp_digit(*r)) return false;
    unsigned minutes = 0;
    while (hpd::hp_digit(*r)) {
      minutes = minutes * 10 + (unsigned)(*r - '0');
      if (minutes > 255) minutes = 255;
      r++;
    }
    out.pending = true;
    out.pending_minutes = (uint8_t)minutes;
    p = hpd::hp_skipMarkers(semi + 1);
  }

  // The names-only fallback is one line ending in "(get preset N)".
  const char* eol = p;
  while (*eol && !hpd::hp_eol(*eol)) eol++;
  const char* hint = strstr(p, "(get preset");
  out.names_only = hint && hint < eol;

  for (int k = 1; k <= 5; k++) {
    if (k > 1) p = out.names_only ? p : hpd::hp_skipMarkers(p);
    while (hpd::hp_blank(*p)) p++;
    if (*p != (char)('0' + k)) return false;
    p++;
    HeadPresetSlot& s = out.slot[k - 1];
    if (*p == '*') {
      s.active = true;
      p++;
    } else if (!hpd::hp_blank(*p)) {
      return false;
    }
    while (hpd::hp_blank(*p)) p++;

    const char* start = p;
    const char* end = p;
    while (*end && !hpd::hp_eol(*end)) {
      if (out.names_only && *end == ' ') {
        if (k < 5 && end[1] == (char)('0' + k + 1) && (end[2] == ' ' || end[2] == '*')) break;
        if (strncmp(end, " (get preset", 12) == 0) break;
      }
      end++;
    }
    p = end;
    while (end > start && hpd::hp_blank(end[-1])) end--;
    if (end == start) return false;
    if (end - start == 1 && *start == '-') {
      s.active = false;
      continue;
    }
    s.used = true;

    const char* name_end = end;
    if (!out.names_only) {
      const char* sp = end;
      while (sp > start && !hpd::hp_blank(sp[-1])) sp--;
      const char* comma = (const char*)memchr(sp, ',', (size_t)(end - sp));
      if (sp > start && comma) {
        char raw[24];
        hpd::hp_copyTrimmed(sp, comma, raw, sizeof(raw));
        headPresetTrimFreq(raw, s.freq, sizeof(s.freq));
        name_end = sp;
      }
    }
    hpd::hp_copyTrimmed(start, name_end, s.name, sizeof(s.name));
  }
  return true;
}

// Index (0-4) of the active slot, or -1.
inline int headPresetActiveSlot(const HeadPresetList& list) {
  for (int i = 0; i < 5; i++) {
    if (list.slot[i].used && list.slot[i].active) return i;
  }
  return -1;
}

// `load preset N` succeeded: the reply starts with "OK - loading <name> ...".
inline bool isLoadPresetOk(const char* reply) {
  return reply && strstr(reply, "OK - loading") != nullptr;
}

// A typed repeater command that loads a preset (moves the LoRa radio).
inline bool isLoadPresetCommand(const char* command) {
  if (!command) return false;
  while (hpd::hp_blank(*command)) command++;
  if (strncmp(command, "load", 4) != 0 || !hpd::hp_blank(command[4])) return false;
  command += 4;
  while (hpd::hp_blank(*command)) command++;
  return strncmp(command, "preset", 6) == 0 && (command[6] == 0 || hpd::hp_blank(command[6]));
}
