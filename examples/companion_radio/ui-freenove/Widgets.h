#pragma once

// Shared widgets of the Freenove visual head's TELEMETRY / MAP / NODES pages
// and the node detail card (pure C++11, header-only; host-tested by
// test/test_widgets.py).
//
//   math     gaugeAngle (value -> arc angle, clamped), sparkline scaling
//            (sparkRange / sparkX / sparkY), the waterfall colour map
//            (SNR -> a ramp whose brightness rises with SNR), the NODES
//            table's stable sort and the card's text helpers (age, km,
//            bearing, position lines)
//   drawing  ring gauge, sparkline (line + filled area), area chart with
//            axes, section label: the HMI look of HOME (Theme.h tokens,
//            D-DIN AA text >= 14 px)

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../HeadContactPolicy.h"
#include "FrameBuffer565.h"
#include "GeoMath.h"
#include "MeshModel.h"
#include "RelayNameStore.h"
#include "Theme.h"

namespace freenove_ui {
namespace widgets {

// ------------------------------------------------------------------ math

// Arc angle (degrees, 0 = +x, clockwise on screen) of `v` on a gauge from
// lo (at start_deg) to hi (at start_deg + sweep_deg). Out of range and NaN
// clamp to the ends.
inline float gaugeAngle(float v, float lo, float hi, float start_deg, float sweep_deg) {
  if (!(hi > lo)) return start_deg;
  float f = (v - lo) / (hi - lo);
  if (!(f >= 0.0f)) f = 0.0f;   // also NaN
  if (f > 1.0f) f = 1.0f;
  return start_deg + f * sweep_deg;
}

// Value range of a sparkline: the data's min..max (entries equal to `none`
// are gaps), widened symmetrically to at least min_span. Returns the
// number of values; lo == hi == 0 when there are none.
inline int sparkRange(const int16_t* v, int n, int16_t none, int min_span, int& lo, int& hi) {
  int count = 0;
  lo = hi = 0;
  for (int i = 0; i < n; i++) {
    if (v[i] == none) continue;
    if (count == 0 || v[i] < lo) lo = v[i];
    if (count == 0 || v[i] > hi) hi = v[i];
    count++;
  }
  if (count == 0) return 0;
  if (min_span < 1) min_span = 1;
  if (hi - lo < min_span) {
    const int grow = min_span - (hi - lo);
    lo -= grow / 2;
    hi = lo + min_span;
  }
  return count;
}

// Column of sample i of n across a box x..x+w-1 (oldest left).
inline int sparkX(int i, int n, int x, int w) {
  if (n <= 1 || w <= 1) return x + (w > 0 ? w - 1 : 0);
  return x + (int)lroundf((float)i * (float)(w - 1) / (float)(n - 1));
}

// Row of value v in a box y..y+h-1 for the range lo..hi (hi at the top),
// clamped into the box.
inline int sparkY(int v, int lo, int hi, int y, int h) {
  if (h <= 1) return y;
  if (hi <= lo) return y + (h - 1) / 2;
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return y + (h - 1) - (int)lroundf((float)(v - lo) * (float)(h - 1) / (float)(hi - lo));
}

// SNR waterfall colour map, quarter-dB in: a ramp from deep navy (weak)
// through blue, cyan and green to yellow and white (strong). Brightness
// (Rec.601 luma) never falls as SNR rises.
static const int kWaterfallMinQ4 = -20 * 4;
static const int kWaterfallMaxQ4 = 12 * 4;
inline theme::Rgb8 waterfallRgb(int snr_q4) {
  static const theme::Rgb8 kStops[6] = {
      {20, 40, 90}, {40, 90, 200}, {40, 200, 230}, {80, 230, 120}, {240, 230, 80}, {255, 255, 230}};
  if (snr_q4 < kWaterfallMinQ4) snr_q4 = kWaterfallMinQ4;
  if (snr_q4 > kWaterfallMaxQ4) snr_q4 = kWaterfallMaxQ4;
  const int span = kWaterfallMaxQ4 - kWaterfallMinQ4;
  const int pos = (snr_q4 - kWaterfallMinQ4) * 5 * 256 / span;   // 0 .. 5*256
  int seg = pos / 256;
  if (seg > 4) seg = 4;
  const int t = pos - seg * 256;
  const theme::Rgb8 a = kStops[seg], b = kStops[seg + 1];
  theme::Rgb8 out;
  out.r = (uint8_t)(a.r + ((int)b.r - (int)a.r) * t / 256);
  out.g = (uint8_t)(a.g + ((int)b.g - (int)a.g) * t / 256);
  out.b = (uint8_t)(a.b + ((int)b.b - (int)a.b) * t / 256);
  return out;
}
inline uint16_t waterfallColour(int snr_q4) {
  const theme::Rgb8 c = waterfallRgb(snr_q4);
  return rgb565(c.r, c.g, c.b);
}
inline int luma(const theme::Rgb8& c) { return (299 * c.r + 587 * c.g + 114 * c.b) / 1000; }

// ------------------------------------------------------------ RX source

// Which RX/min to show: the repeater's (vitals) unless it contradicts what
// the head itself observed in the last complete minute (the observer feed
// carries every packet the repeater receives): 0 while the head saw
// traffic, or under half of it. Then the head's count, labelled as such.
// A duplicate % needs a trustworthy non-zero repeater RX.
struct RxPick {
  bool known;       // some RX figure exists
  unsigned rx;
  bool from_head;   // the head's observed count, not the repeater's
  bool dup_ok;      // dup / rx is meaningful
};
inline RxPick pickRx(bool has_vitals, unsigned vitals_rx, unsigned head_rx) {
  RxPick p;
  p.known = true;
  p.rx = vitals_rx;
  p.from_head = false;
  p.dup_ok = false;
  const bool contradicts = (vitals_rx == 0 && head_rx > 0) || (head_rx >= 4 && vitals_rx * 2 < head_rx);
  if (!has_vitals || contradicts) {
    p.rx = head_rx;
    p.from_head = true;
    p.known = has_vitals || head_rx > 0;
    return p;
  }
  p.dup_ok = vitals_rx > 0;
  return p;
}

// The pick for the model now: the repeater's vitals RX against the head's
// observed count of the last complete minute.
inline RxPick rxFor(const mesh::head::MeshModel& m) {
  uint16_t rates[60];
  m.ratesPerMinute(rates);
  const mesh::observer::Vitals* v = m.vitals();
  return pickRx(v != nullptr, v != nullptr ? v->rx_per_min : 0u, rates[58]);
}

// ------------------------------------------------------------ NODES table

enum NodeColumn { kColName, kColType, kColHeard, kColSnr, kColHops, kColKm, kColPackets, kColCount };

// One table row: the sort keys of a model node (taken when the table is
// built; the name points into the model and is valid until it next changes).
struct NodeRow {
  uint8_t key[4];       // NodeInfo::pub_prefix
  const char* name;     // drawable name (see rowName)
  uint8_t type;
  uint8_t hops;
  bool has_snr;         // an SNR to show and sort on (see snrCell)
  int8_t snr_q4;
  bool has_km;          // both the node and the repeater have a position
  float km;
  uint32_t age_ms;      // since last heard
  uint32_t packets;
};

// Names compared as drawn (emoji and other non-ASCII bytes dropped, then
// leading spaces), ignoring case.
inline int compareNames(const char* p, const char* q) {
  p = p != nullptr ? p : "";
  q = q != nullptr ? q : "";
  bool sp = false, sq = false;   // past the leading spaces
  for (;;) {
    while (*p && ((unsigned char)*p >= 0x80 || (!sp && *p == ' '))) p++;
    while (*q && ((unsigned char)*q >= 0x80 || (!sq && *q == ' '))) q++;
    sp = sq = true;
    int x = (unsigned char)*p, y = (unsigned char)*q;
    if (x >= 'a' && x <= 'z') x -= 32;
    if (y >= 'a' && y <= 'z') y -= 32;
    if (x != y) return x < y ? -1 : 1;
    if (x == 0) return 0;
    p++;
    q++;
  }
}

// Natural ascending order of a column (name A..Z ignoring case, type,
// heard most recently first, SNR low..high, hops, km, packets). Rows
// without the value (no SNR, no distance) are "greater" than any row with
// it, so they sort last ascending.
inline int compareRows(const NodeRow& a, const NodeRow& b, NodeColumn c) {
  switch (c) {
    case kColName: return compareNames(a.name, b.name);
    case kColType: return a.type == b.type ? 0 : (a.type < b.type ? -1 : 1);
    case kColHeard: return a.age_ms == b.age_ms ? 0 : (a.age_ms < b.age_ms ? -1 : 1);
    case kColSnr:
      if (a.has_snr != b.has_snr) return a.has_snr ? -1 : 1;
      if (!a.has_snr) return 0;
      return a.snr_q4 == b.snr_q4 ? 0 : (a.snr_q4 < b.snr_q4 ? -1 : 1);
    case kColHops: return a.hops == b.hops ? 0 : (a.hops < b.hops ? -1 : 1);
    case kColKm:
      if (a.has_km != b.has_km) return a.has_km ? -1 : 1;
      if (!a.has_km) return 0;
      return a.km == b.km ? 0 : (a.km < b.km ? -1 : 1);
    case kColPackets: return a.packets == b.packets ? 0 : (a.packets < b.packets ? -1 : 1);
    default: return 0;
  }
}

// The direction a column sorts in when first tapped: strongest SNR, most
// packets first; the rest ascending.
inline bool defaultDescending(NodeColumn c) { return c == kColSnr || c == kColPackets; }

// Stable sort of the row indices in `order` (n entries) by column c.
// Descending reverses the column's order, but rows without the value stay
// last and equal rows keep their previous order either way.
inline void sortRows(uint16_t* order, int n, const NodeRow* rows, NodeColumn c, bool descending) {
  for (int i = 1; i < n; i++) {   // insertion sort: stable, n <= 256
    const uint16_t v = order[i];
    int j = i;
    while (j > 0) {
      const NodeRow& a = rows[order[j - 1]];
      const NodeRow& b = rows[v];
      int cmp = compareRows(a, b, c);
      if (descending) {
        const bool aMissing = (c == kColSnr && !a.has_snr) || (c == kColKm && !a.has_km);
        const bool bMissing = (c == kColSnr && !b.has_snr) || (c == kColKm && !b.has_km);
        cmp = aMissing != bMissing ? cmp : -cmp;
      }
      if (cmp <= 0) break;
      order[j] = order[j - 1];
      j--;
    }
    order[j] = v;
  }
}

// ------------------------------------------------------------- text helpers

// "+6.5" / "-7.2" from quarter-dB, one decimal, no floats.
inline void formatSnr(char* out, size_t cap, int snr_q4) {
  const int a = snr_q4 < 0 ? -snr_q4 : snr_q4;
  const int tenths = (a * 10 + 2) / 4;
  snprintf(out, cap, "%c%d.%d", snr_q4 < 0 ? '-' : '+', tenths / 10, tenths % 10);
}

// "12s", "5m", "3h", "2d" since last heard.
inline void formatAge(char* out, size_t cap, uint32_t ms) {
  const uint32_t s = ms / 1000u;
  if (s < 60) snprintf(out, cap, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(out, cap, "%lum", (unsigned long)(s / 60));
  else if (s < 86400) snprintf(out, cap, "%luh", (unsigned long)(s / 3600));
  else snprintf(out, cap, "%lud", (unsigned long)(s / 86400));
}

// "850m", "4.1", "37" (km; one decimal below 10 km).
inline void formatKm(char* out, size_t cap, double km, bool unit) {
  if (km < 1.0) snprintf(out, cap, "%dm", (int)(km * 1000.0 + 0.5));
  else if (km < 10.0) snprintf(out, cap, unit ? "%.1fkm" : "%.1f", km);
  else snprintf(out, cap, unit ? "%dkm" : "%d", (int)(km + 0.5));
}

// Compass point of a bearing: N, NE, E, ...
inline const char* compassPoint(double deg) {
  static const char* const kPts[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int i = (int)floor((fmod(deg, 360.0) + 360.0 + 22.5) / 45.0) % 8;
  if (i < 0) i += 8;
  return kPts[i];
}

// "RPT", "CHAT", "ROOM", "SENS", "?".
inline const char* typeShort(uint8_t type) {
  switch (type) {
    case head_policy::kTypeChat: return "CHAT";
    case head_policy::kTypeRepeater: return "RPT";
    case head_policy::kTypeRoom: return "ROOM";
    case head_policy::kTypeSensor: return "SENS";
    default: return "?";
  }
}
inline const char* typeLong(uint8_t type) {
  switch (type) {
    case head_policy::kTypeChat: return "Companion";
    case head_policy::kTypeRepeater: return "Repeater";
    case head_policy::kTypeRoom: return "Room";
    case head_policy::kTypeSensor: return "Sensor";
    default: return "Node";
  }
}

// The drawable name of a node (emoji dropped; empty -> "rptr 57f72334").
inline void nodeName(char* out, size_t cap, const mesh::head::NodeInfo& n) {
  head_policy::displayName(n.name, n.pub_prefix, n.type, out, cap);
}

// The name of the node a relay hash stands for. The hash must stand for one
// relay among every relay the head knows of: the observing repeater, the
// repeaters the model has heard this boot and the relay names kept across
// reboots (MeshModel::nameStore). Exactly one -> its name (the observing
// repeater: its advertised name, else its stored name, else "M5"); none or
// several (ambiguous) -> the hash in hex. Not limited to the head's contacts.
inline void relayName(char* out, size_t cap, const mesh::head::MeshModel& m, const mesh::head::PathHash& h) {
  if (cap == 0) return;
  out[0] = 0;
  if (!h.valid()) return;
  const mesh::head::RelayNameStore* store = m.nameStore();
  uint8_t rep[4];
  const bool is_rep = m.repeaterKey(rep) && h.matchesKey(rep);
  uint8_t live[4];
  const size_t live_n = m.repeatersMatching(h, live);
  // The observing repeater never sits in the node table, so it and a live
  // match are two relays.
  const size_t known = (is_rep ? 1 : 0) + live_n;
  const uint8_t* known_key = is_rep ? rep : (live_n == 1 ? live : nullptr);
  const char* stored = nullptr;
  const uint8_t* stored_key = nullptr;
  const size_t others = store != nullptr ? store->matches(h.bytes, h.size, known_key, &stored, &stored_key) : 0;
  if (known + others == 1) {
    if (is_rep) {
      const char* own = m.repeaterName();
      if (own[0] == 0 && store != nullptr && store->nameFor(rep) != nullptr) own = store->nameFor(rep);
      snprintf(out, cap, "%s", own[0] ? own : "M5");
      return;
    }
    if (live_n == 1) {
      const mesh::head::NodeInfo* n = m.findByHash(h);
      if (n != nullptr) {
        nodeName(out, cap, *n);
        return;
      }
    } else if (stored != nullptr) {
      head_policy::displayName(stored, stored_key, head_policy::kTypeRepeater, out, cap);
      return;
    }
  }
  size_t used = 0;
  for (uint8_t i = 0; i < h.size && used + 3 <= cap; i++) used += (size_t)snprintf(out + used, cap - used, "%02X", h.bytes[i]);
}

// What the NODES SNR column (and the node card) says about a node: a node
// whose latest advert came straight to the M5 shows its own last-hop SNR; a
// relayed one shows the SNR the M5 heard its advert at plus the relay
// ("-4.2 via Downtown"). A node with no advert path known falls back to its
// own SNR, else "relayed".
struct SnrCell {
  bool has_snr;
  int8_t snr_q4;
  bool relayed;
  char via[28];         // relay name or hex ("" = unknown)
};

inline SnrCell snrCell(const mesh::head::MeshModel& m, const mesh::head::NodeInfo& n) {
  SnrCell c;
  memset(&c, 0, sizeof(c));
  const bool own = n.snr_hist[0] != mesh::head::SNR_NONE;
  if (n.hops > 0 && n.advert_via.valid()) {
    c.relayed = true;
    c.has_snr = n.advert_via_snr_q4 != mesh::head::SNR_NONE;
    c.snr_q4 = c.has_snr ? n.advert_via_snr_q4 : 0;
    relayName(c.via, sizeof(c.via), m, n.advert_via);
  } else if (own) {
    c.has_snr = true;
    c.snr_q4 = n.last_snr_q4;
  } else {
    c.relayed = n.hops > 0;
  }
  return c;
}

// "-4.2 via Downtown", "+6.5", "via 7E", "relayed", "--".
inline void formatSnrCell(char* out, size_t cap, const SnrCell& c) {
  if (cap == 0) return;
  char snr[12] = "";
  if (c.has_snr) formatSnr(snr, sizeof(snr), c.snr_q4);
  if (c.relayed && c.via[0]) snprintf(out, cap, "%s%svia %s", snr, snr[0] ? " " : "", c.via);
  else if (snr[0]) snprintf(out, cap, "%s", snr);
  else snprintf(out, cap, "%s", c.relayed ? "relayed" : "--");
}

// `s` in at most max_px of font f: whole when it fits, else cut and
// marked with a trailing "~" (as HOME's labels).
inline void fitText(char* out, size_t cap, const char* s, const AAFont& f, int max_px) {
  if (cap == 0) return;
  size_t n = 0;
  while (s[n] && n + 1 < cap) { out[n] = s[n]; n++; }
  out[n] = 0;
  if (FrameBuffer565::textWidth(out, f) <= max_px) return;
  while (n > 1) {
    out[n - 1] = '~';
    out[n] = 0;
    if (FrameBuffer565::textWidth(out, f) <= max_px) return;
    n--;
  }
}

// Two lines of the card's POSITION section. Node without GPS: "No GPS
// position" / its type of fix is unknown. Node with GPS: its coordinates,
// then the distance and bearing from the repeater, or why there is none.
inline void positionLines(char* l1, size_t c1, char* l2, size_t c2, const mesh::head::NodeInfo& n,
                          bool rep_fix, int32_t rep_lat, int32_t rep_lon) {
  if (c1) l1[0] = 0;
  if (c2) l2[0] = 0;
  if (!n.has_pos || !mesh::head::geoValid(n.lat_e6, n.lon_e6)) {
    snprintf(l1, c1, "No GPS position");
    snprintf(l2, c2, "not in its adverts");
    return;
  }
  snprintf(l1, c1, "%.5f, %.5f", (double)n.lat_e6 / 1e6, (double)n.lon_e6 / 1e6);
  if (!rep_fix) {
    snprintf(l2, c2, "repeater: no GPS fix");
    return;
  }
  const double km = mesh::head::distanceKm(rep_lat, rep_lon, n.lat_e6, n.lon_e6);
  const double brg = mesh::head::bearingDeg(rep_lat, rep_lon, n.lat_e6, n.lon_e6);
  char d[16];
  formatKm(d, sizeof(d), km, true);
  snprintf(l2, c2, "%s  bearing %d %s", d, (int)(brg + 0.5) % 360, compassPoint(brg));
}

// ---------------------------------------------------------------- drawing

// Ring gauge track over sweep_deg from start_deg (static: the page
// background).
inline void ringGaugeTrack(FrameBuffer565& fb, float cx, float cy, float r, float thick, float start_deg,
                           float sweep_deg) {
  fb.aaArc(cx, cy, r, thick, start_deg, start_deg + sweep_deg, theme::kGaugeTrack);
}

// The value's arc over the track in `c` with a white tip (nothing when
// !has).
inline void ringGauge(FrameBuffer565& fb, float cx, float cy, float r, float thick, float start_deg,
                      float sweep_deg, bool has, float v, float lo, float hi, uint16_t c) {
  if (!has) return;
  const float a = gaugeAngle(v, lo, hi, start_deg, sweep_deg);
  if (a > start_deg + 0.5f) fb.aaArc(cx, cy, r, thick, start_deg, a, c);
  const float rad = a * 3.14159265f / 180.0f;
  fb.aaDisc(cx + cosf(rad) * (r - thick * 0.5f), cy + sinf(rad) * (r - thick * 0.5f), thick * 0.5f + 0.8f, theme::kText);
}

// Sparkline of v[0..n) (oldest first; `none` = gap) in the box: a filled
// area under a 1.5 px line in `c`; lo..hi as from sparkRange.
inline void sparkline(FrameBuffer565& fb, int x, int y, int w, int h, const int16_t* v, int n, int16_t none, int lo,
                      int hi, uint16_t c, unsigned fill_alpha = 60) {
  int px = -1, py = -1;
  for (int i = 0; i < n; i++) {
    if (v[i] == none) { px = -1; continue; }
    const int sx = sparkX(i, n, x, w), sy = sparkY(v[i], lo, hi, y, h);
    if (fill_alpha > 0) {
      const int x0 = px >= 0 ? px + 1 : sx;
      for (int cx = x0; cx <= sx; cx++) {
        const int cy = px >= 0 && sx != px ? py + (sy - py) * (cx - px) / (sx - px) : sy;
        fb.fillRectAlpha(cx, cy, 1, y + h - cy, c, fill_alpha);
      }
    }
    if (px >= 0) fb.aaLine((float)px, (float)py, (float)sx, (float)sy, 1.5f, c);
    else fb.aaDisc((float)sx, (float)sy, 1.2f, c);
    px = sx;
    py = sy;
  }
}

// Section label inside a panel: 14 px caps in the accent colour.
inline void sectionLabel(FrameBuffer565& fb, int x, int y, const char* s) {
  fb.text(x, y, s, theme::kAccent, kFontSmall);
}

}  // namespace widgets
}  // namespace freenove_ui
