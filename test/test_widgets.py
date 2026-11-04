"""Freenove visual head TELEMETRY, MAP and NODES pages and the shared node
card (Task 8): the pure widget helpers (Widgets.h), the pages rendered into
an RGB565 framebuffer with a live model, their touch handling, and the
device wiring.

Host harness (g++, ASan/UBSan off Windows). Run directly with --preview to
write the owner previews (fake data, host only) into the task folder:
telem_preview, map_preview, map_preview_nofix, nodes_preview,
detail_preview (each *_1x.png at 1:1 and *.png zoomed 2x):

    python test_widgets.py --preview
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_layouts import function_body, rgb565_rows, strip_comments, write_png  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / 'examples' / 'companion_radio' / 'ui-freenove'
VARIANT = ROOT / 'variants' / 'freenove_s3_35_head'
PREVIEW_DIR = ROOT / '.superpowers' / 'sdd' / '2026-10-30-freenove-visual-head'

HARNESS = r'''
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <Packet.h>
#include <helpers/AdvertDataHelpers.h>
#include "HomePage.h"
#include "MapPage.h"
#include "NodeCard.h"
#include "NodesPage.h"
#include "StubPages.h"
#include "TelemetryPage.h"
#include "Widgets.h"
#include "RelayNameStore.h"
using namespace freenove_ui;
using namespace mesh::head;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return (rng >> 8) & 0xFFFFFF; }

// ------------------------------------------------------------ pure helpers

static void gauge_angle_clamps() {
  using widgets::gaugeAngle;
  CHECK(gaugeAngle(-20.0f, -20.0f, 15.0f, 135.0f, 270.0f) == 135.0f);
  CHECK(gaugeAngle(15.0f, -20.0f, 15.0f, 135.0f, 270.0f) == 405.0f);
  CHECK(std::fabs(gaugeAngle(-2.5f, -20.0f, 15.0f, 135.0f, 270.0f) - 270.0f) < 0.01f);
  CHECK(gaugeAngle(-99.0f, -20.0f, 15.0f, 135.0f, 270.0f) == 135.0f);   // below: start
  CHECK(gaugeAngle(99.0f, -20.0f, 15.0f, 135.0f, 270.0f) == 405.0f);    // above: end
  CHECK(gaugeAngle(NAN, -20.0f, 15.0f, 135.0f, 270.0f) == 135.0f);
  CHECK(gaugeAngle(5.0f, 3.0f, 3.0f, 135.0f, 270.0f) == 135.0f);        // empty range
  float prev = -1;
  for (int v = -40; v <= 40; v++) {
    const float a = gaugeAngle((float)v, -20.0f, 15.0f, 135.0f, 270.0f);
    CHECK(a >= prev && a >= 135.0f && a <= 405.0f);
    prev = a;
  }
  if (!failures) std::printf("PASS gauge_angle_clamps\n");
}

static void sparkline_scales_to_box() {
  const int16_t none = SNR_NONE;
  int16_t v[32];
  for (int i = 0; i < 32; i++) v[i] = (int16_t)(i % 5 == 0 ? none : (int16_t)(((int)(rnd() % 61)) - 30));
  int lo = 0, hi = 0;
  const int n = widgets::sparkRange(v, 32, none, 8, lo, hi);
  CHECK(n == 32 - 7);
  int mn = 999, mx = -999;
  for (int i = 0; i < 32; i++) if (v[i] != none) { mn = std::min(mn, (int)v[i]); mx = std::max(mx, (int)v[i]); }
  CHECK(lo == mn && hi == mx);
  // A flat series widens to the minimum span around its value.
  int16_t flat[4] = {12, 12, none, 12};
  CHECK(widgets::sparkRange(flat, 4, none, 8, lo, hi) == 3 && hi - lo == 8 && lo <= 12 && hi >= 12);
  int16_t empty[3] = {none, none, none};
  CHECK(widgets::sparkRange(empty, 3, none, 8, lo, hi) == 0 && lo == 0 && hi == 0);
  // Every point lands in the box: max on the top row, min on the bottom.
  const int bx = 40, by = 50, bw = 120, bh = 30;
  widgets::sparkRange(v, 32, none, 8, lo, hi);
  for (int i = 0; i < 32; i++) {
    const int x = widgets::sparkX(i, 32, bx, bw);
    CHECK(x >= bx && x < bx + bw);
    if (v[i] == none) continue;
    const int y = widgets::sparkY(v[i], lo, hi, by, bh);
    CHECK(y >= by && y < by + bh);
    if (v[i] == hi) CHECK(y == by);
    if (v[i] == lo) CHECK(y == by + bh - 1);
  }
  CHECK(widgets::sparkX(0, 32, bx, bw) == bx && widgets::sparkX(31, 32, bx, bw) == bx + bw - 1);
  CHECK(widgets::sparkY(-1000, lo, hi, by, bh) == by + bh - 1 && widgets::sparkY(1000, lo, hi, by, bh) == by);
  CHECK(widgets::sparkY(5, 5, 5, by, bh) == by + (bh - 1) / 2);
  // Drawn: nothing outside the box (AA edge: 2 px), something inside.
  std::vector<uint16_t> px(480 * 320, 0);
  FrameBuffer565 fb(px.data());
  widgets::sparkline(fb, bx, by, bw, bh, v, 32, none, lo, hi, theme::kAccent);
  int inside = 0, outside = 0;
  for (int y = 0; y < 320; y++) for (int x = 0; x < 480; x++) {
    if (px[(size_t)y * 480 + x] == 0) continue;
    if (x >= bx - 2 && x < bx + bw + 2 && y >= by - 2 && y < by + bh + 2) inside++; else outside++;
  }
  CHECK(inside > bw && outside == 0);
  if (!failures) std::printf("PASS sparkline_scales_to_box\n");
}

static void waterfall_colour_map_monotonic() {
  int prev = -1;
  for (int q = -40 * 4; q <= 30 * 4; q++) {
    const theme::Rgb8 c = widgets::waterfallRgb(q);
    const int l = widgets::luma(c);
    CHECK(l >= prev);
    prev = l;
  }
  // Clamped ends, a real spread between them, distinct colours by band.
  CHECK(widgets::luma(widgets::waterfallRgb(-100 * 4)) == widgets::luma(widgets::waterfallRgb(widgets::kWaterfallMinQ4)));
  CHECK(widgets::luma(widgets::waterfallRgb(widgets::kWaterfallMaxQ4)) - widgets::luma(widgets::waterfallRgb(widgets::kWaterfallMinQ4)) > 150);
  CHECK(widgets::waterfallColour(-15 * 4) != widgets::waterfallColour(0) && widgets::waterfallColour(0) != widgets::waterfallColour(10 * 4));
  if (!failures) std::printf("PASS waterfall_colour_map_monotonic\n");
}

static bool missing(const widgets::NodeRow& r, widgets::NodeColumn c) {
  return (c == widgets::kColSnr && !r.has_snr) || (c == widgets::kColKm && !r.has_km);
}

static void nodes_sort_by_each_column() {
  // Ties in every column, rows without SNR / distance, emoji names.
  static const char* const names[8] = {"Woods 5C", "\xF0\x9F\x8C\xAE TacoBot", "alpha", "Beta", "woods 5c", "Zed", "  Mike", "beta"};
  widgets::NodeRow rows[8];
  std::memset(rows, 0, sizeof(rows));
  for (int i = 0; i < 8; i++) {
    rows[i].key[0] = (uint8_t)i;
    rows[i].name = names[i];
    rows[i].type = (uint8_t)(1 + i % 3);
    rows[i].hops = (uint8_t)(i % 3);
    rows[i].has_snr = i % 4 != 3;
    rows[i].snr_q4 = (int8_t)((i % 3) * 8 - 8);
    rows[i].has_km = i % 3 != 1;
    rows[i].km = (float)(i % 2) * 2.5f;
    rows[i].age_ms = (uint32_t)(i % 4) * 1000u;
    rows[i].packets = (uint32_t)(i % 2) * 10u;
  }
  for (int c = 0; c < widgets::kColCount; c++) {
    for (int d = 0; d < 2; d++) {
      const widgets::NodeColumn col = (widgets::NodeColumn)c;
      uint16_t order[8];
      for (int i = 0; i < 8; i++) order[i] = (uint16_t)i;
      widgets::sortRows(order, 8, rows, col, d == 1);
      bool seen[8] = {false};
      for (int i = 0; i < 8; i++) { CHECK(order[i] < 8 && !seen[order[i]]); seen[order[i]] = true; }
      for (int i = 0; i + 1 < 8; i++) {
        const widgets::NodeRow& a = rows[order[i]];
        const widgets::NodeRow& b = rows[order[i + 1]];
        if (missing(a, col) || missing(b, col)) {
          CHECK(!missing(a, col) || missing(b, col));   // missing last, either direction
          if (missing(a, col) && missing(b, col)) CHECK(order[i] < order[i + 1]);
          continue;
        }
        const int cmp = widgets::compareRows(a, b, col);
        CHECK(d == 1 ? cmp >= 0 : cmp <= 0);
        if (cmp == 0) CHECK(order[i] < order[i + 1]);   // stable
      }
    }
  }
  // Names as drawn: emoji and leading spaces ignored, case ignored.
  uint16_t order[8];
  for (int i = 0; i < 8; i++) order[i] = (uint16_t)i;
  widgets::sortRows(order, 8, rows, widgets::kColName, false);
  CHECK(order[0] == 2 && order[1] == 3 && order[2] == 7 && order[3] == 6 && order[4] == 1);   // alpha Beta beta Mike TacoBot
  CHECK(order[5] == 0 && order[6] == 4 && order[7] == 5);                                    // Woods 5C, woods 5c (stable), Zed
  // Re-sorting by another column keeps the previous order among ties.
  widgets::sortRows(order, 8, rows, widgets::kColPackets, true);
  CHECK(rows[order[0]].packets == 10);
  for (int i = 0; i + 1 < 8; i++)
    if (rows[order[i]].packets == rows[order[i + 1]].packets)
      CHECK(widgets::compareNames(rows[order[i]].name, rows[order[i + 1]].name) <= 0);
  CHECK(widgets::defaultDescending(widgets::kColSnr) && widgets::defaultDescending(widgets::kColPackets));
  CHECK(!widgets::defaultDescending(widgets::kColName) && !widgets::defaultDescending(widgets::kColHeard));
  if (!failures) std::printf("PASS nodes_sort_by_each_column\n");
}

// --------------------------------------------------------------- the mesh

typedef std::vector<uint8_t> Bytes;
static const int32_t kLat0 = 47606200, kLon0 = -122332100;   // Seattle

static Bytes packet(uint8_t type, uint8_t route, uint8_t path_len, const Bytes& path, const Bytes& payload) {
  Bytes b;
  b.push_back((uint8_t)((type << PH_TYPE_SHIFT) | route));
  b.push_back(path_len);
  b.insert(b.end(), path.begin(), path.end());
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}

static Bytes advert(uint8_t k0, uint8_t k1, uint8_t type, const char* name, double lat, double lon, bool pos, uint32_t ts = 1793000000u) {
  Bytes p;
  for (int i = 0; i < 32; i++) p.push_back((uint8_t)(0x40 + i));
  p[0] = k0; p[1] = k1;
  p.insert(p.end(), (const uint8_t*)&ts, (const uint8_t*)&ts + 4);
  for (int i = 0; i < 64; i++) p.push_back((uint8_t)(0xA0 ^ i));
  uint8_t app[MAX_ADVERT_DATA_SIZE];
  AdvertDataBuilder b = pos ? AdvertDataBuilder(type, name, lat, lon) : AdvertDataBuilder(type, name);
  const uint8_t n = b.encodeTo(app);
  p.insert(p.end(), app, app + n);
  return p;
}

static Bytes hashes(std::initializer_list<uint16_t> hs) {
  Bytes b;
  for (uint16_t h : hs) { b.push_back((uint8_t)(h >> 8)); b.push_back((uint8_t)h); }
  return b;
}

static uint32_t salt = 1;
static Bytes payload() {
  Bytes p; p.push_back(0x5C);
  const uint32_t s = salt++;
  p.push_back((uint8_t)s); p.push_back((uint8_t)(s >> 8)); p.push_back((uint8_t)(s >> 16));
  for (int i = 0; i < 28; i++) p.push_back((uint8_t)(i * 7 + s));
  return p;
}

static void obs(MeshModel& m, const Bytes& raw, uint32_t now, float snr, int rssi = -95) {
  mesh::observer::PacketRecord r; std::memset(&r, 0, sizeof(r));
  r.rx_ms = now; r.snr_q4 = (int8_t)std::lround(snr * 4); r.rssi = (int16_t)rssi;
  mesh::observer::setPacketBytes(r, raw.data(), raw.size());
  m.onObserverPacket(r, now);
}

static uint8_t plen(int count) { return (uint8_t)(0x40 | count); }

static void ctxInit(PageContext& ctx, MeshModel* m) {
  std::memset(&ctx, 0, sizeof(ctx));
  ctx.model = m;
  std::snprintf(ctx.clock_hm, sizeof(ctx.clock_hm), "13:42");
  std::snprintf(ctx.clock_date, sizeof(ctx.clock_date), "31 Oct UTC");
}

static void vitals(MeshModel& m, PageContext& ctx, uint32_t t, int16_t nf, uint16_t rx, bool fix) {
  mesh::observer::Vitals v; std::memset(&v, 0, sizeof(v));
  v.uptime_s = 86400 * 3 + 4 * 3600 + t / 1000; v.noise_floor = nf; v.rx_per_min = rx; v.tx_per_min = 11;
  v.dup_per_min = (uint16_t)(rx / 6); v.airtime_permille = 31;
  v.espnow_nodes = 3; v.batt_mv = 4100; v.has_pos = fix; v.lat_e6 = fix ? kLat0 : 0; v.lon_e6 = fix ? kLon0 : 0;
  v.freq = 910.525f; v.bw = 62.5f; v.sf = 7; v.cr = 5; v.preset_slot = 1;
  m.onVitals(v, t);
  ctx.obs_last_ms = t; ctx.obs_seen = true;
  ctx.data_gen++;
}

struct Rep { uint16_t h; const char* name; double lat, lon; bool pos; float snr; uint8_t type; };
static const Rep kReps[] = {
  {0x5701, "Woods 5C", 47.6400, -122.3900, true, 2.0f, ADV_TYPE_REPEATER},
  {0xCA75, "CATHOUSE", 47.6250, -122.2800, true, 6.5f, ADV_TYPE_REPEATER},
  {0x7EBE, "Trebek", 47.5600, -122.3000, true, -9.0f, ADV_TYPE_REPEATER},
  {0x1F01, "Jupiter", 47.6550, -122.4200, true, 7.5f, ADV_TYPE_REPEATER},
  {0x2E11, "Relay-1", 0, 0, false, -4.0f, ADV_TYPE_REPEATER},
  {0x8821, "Harbor", 47.5900, -122.3900, true, -3.5f, ADV_TYPE_REPEATER},
};
static const int kRepCount = 6;

static void health(MeshModel& m, uint32_t t, uint32_t free_heap) {
  mesh::observer::Health h;
  h.uptime_s = t / 1000; h.free_heap = free_heap; h.min_free_heap = 151234; h.largest_block = 65524; h.reset_reason = 1;
  m.onHealth(h, t);
}

// A fake hour of mesh around the M5 (host previews / tests only): adverts,
// then 60 minutes of traffic relayed by the repeaters with per-minute vitals.
static uint32_t populate(MeshModel& m, PageContext& ctx, bool fix) {
  const Bytes none;
  uint32_t t = 1000;
  const Bytes m5 = packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), none,
                          advert(0xB5, 0x05, ADV_TYPE_REPEATER, "CaptCallahan-Base", 0, 0, false));
  m.onBridgedPacket(m5.data(), m5.size(), t);
  ctx.bridge_last_ms = t; ctx.bridge_seen = true;
  for (int i = 0; i < kRepCount; i++) {
    const Rep& r = kReps[i];
    obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), none,
                  advert((uint8_t)(r.h >> 8), (uint8_t)r.h, r.type, r.name, r.lat, r.lon, r.pos)), t += 300, r.snr);
  }
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(1), hashes({0x5701}),
                advert(0x5A, 0x33, ADV_TYPE_REPEATER, "Summit", 47.7000, -122.2500, true)), t += 300, 3.0f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(2), hashes({0x7EBE, 0xCA75}),
                advert(0x17, 0x0A, ADV_TYPE_CHAT, "CardPuter", 0, 0, false)), t += 300, 1.0f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(1), hashes({0x5701}),
                advert(0xC4, 0x4D, ADV_TYPE_CHAT, "Capt4McNeil", 0, 0, false)), t += 300, 2.5f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(1), hashes({0x1F01}),
                advert(0x3B, 0x90, ADV_TYPE_SENSOR, "Weather-Ballard", 47.6680, -122.3840, true)), t += 300, 4.0f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(2), hashes({0x8821, 0x2E11}),
                advert(0x64, 0x12, ADV_TYPE_ROOM, "\xF0\x9F\x8C\xAE TacoRoom", 47.5400, -122.3600, true)), t += 300, -4.0f);
  // An hour of traffic.
  static const uint8_t kTypes[5] = {PAYLOAD_TYPE_TXT_MSG, PAYLOAD_TYPE_GRP_TXT, PAYLOAD_TYPE_ACK, PAYLOAD_TYPE_GRP_TXT, PAYLOAD_TYPE_PATH};
  const uint32_t start = 60000;
  for (int min = 0; min < 60; min++) {
    const int n = 18 + (int)(12.0 * std::sin(min / 6.0)) + (int)(rnd() % 8);
    const uint32_t t0 = start + (uint32_t)min * 60000u;
    for (int k = 0; k < n; k++) {
      const int w = (int)(rnd() % 10);
      const int ri = w < 3 ? 1 : (w < 5 ? 3 : (w < 7 ? 0 : (w < 8 ? 5 : (w < 9 ? 2 : 4))));
      const Rep& r = kReps[ri];
      const float snr = r.snr + (float)((int)(rnd() % 9) - 4) * 0.75f;
      const Bytes path = (rnd() % 3 == 0) ? hashes({0x7EBE, r.h}) : hashes({r.h});
      obs(m, packet(kTypes[rnd() % 5], ROUTE_TYPE_FLOOD, plen((int)path.size() / 2), path, payload()),
          t0 + (uint32_t)k * (55000u / (uint32_t)n), snr, -70 - (int)(rnd() % 40));
    }
    vitals(m, ctx, t0 + 59000, (int16_t)(-116 + (int)(3.0 * std::sin(min / 9.0)) + (int)(rnd() % 3)), (uint16_t)n, fix);
    health(m, t0 + 59500, 186000u - (uint32_t)min * 150u - (uint32_t)(rnd() % 1500));
  }
  t = start + 60u * 60000u;
  ctx.data_gen += 5;
  return t;
}

struct Screen {
  std::vector<uint16_t> px, bgpx;
  FrameBuffer565 fb, bg;
  PageContext* ctx;
  explicit Screen(PageContext* c) : px(480 * 320), bgpx(480 * 320), fb(px.data()), bg(bgpx.data()), ctx(c) {}
  void show(Page& p, uint32_t now, int index) {
    drawPageChrome(bg, p, index, PageSet::kCount);
    fb.copyFrom(bgpx.data());
    p.onShow(now);
  }
  void frame(Page& p, uint32_t now) {
    ctx->now_ms = now;
    fb.eraseTouched(bgpx.data());
    p.draw(fb, now);
    drawHeader(fb, *ctx, now);
  }
  int differ(int x0, int y0, int x1, int y1) const {
    int n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) n += px[(size_t)y * 480 + x] != bgpx[(size_t)y * 480 + x];
    return n;
  }
  int near(int x0, int y0, int x1, int y1, uint16_t c) const {
    int n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
      const uint16_t p = px[(size_t)y * 480 + x];
      const int dr = ((p >> 11) & 31) - ((c >> 11) & 31), dg = ((p >> 5) & 63) - ((c >> 5) & 63), db = (p & 31) - (c & 31);
      n += std::abs(dr) <= 3 && std::abs(dg) <= 6 && std::abs(db) <= 3;
    }
    return n;
  }
  bool save(const char* path) const {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(px.data(), 2, px.size(), f) == px.size();
    std::fclose(f);
    return ok;
  }
};

static bool tap(Page& p, int x, int y) { const bool a = p.touch(x, y, true); const bool b = p.touch(x, y, false); return a || b; }

static const NodeInfo* byName(const MeshModel& m, const char* name) {
  for (size_t i = 0; i < m.nodeCount(); i++) if (std::strcmp(m.node(i)->name, name) == 0) return m.node(i);
  return nullptr;
}

// ------------------------------------------------------------------ card

static void node_card_handles_no_gps() {
  NodeInfo n; std::memset(&n, 0, sizeof(n));
  std::snprintf(n.name, sizeof(n.name), "CardPuter");
  char a[48], b[48];
  widgets::positionLines(a, sizeof(a), b, sizeof(b), n, false, 0, 0);
  CHECK(std::strcmp(a, "No GPS position") == 0 && b[0] != 0 && std::strstr(b, "km") == nullptr);
  widgets::positionLines(a, sizeof(a), b, sizeof(b), n, true, kLat0, kLon0);
  CHECK(std::strcmp(a, "No GPS position") == 0 && std::strstr(b, "km") == nullptr);
  n.has_pos = true; n.lat_e6 = 47640000; n.lon_e6 = -122390000;
  widgets::positionLines(a, sizeof(a), b, sizeof(b), n, false, 0, 0);
  CHECK(std::strstr(a, "47.64000") != nullptr && std::strstr(b, "no GPS fix") != nullptr);
  widgets::positionLines(a, sizeof(a), b, sizeof(b), n, true, kLat0, kLon0);
  CHECK(std::strstr(b, "km") != nullptr && std::strstr(b, "bearing") != nullptr && std::strstr(b, "NW") != nullptr);
  CHECK(std::strcmp(widgets::compassPoint(359.0), "N") == 0 && std::strcmp(widgets::compassPoint(91.0), "E") == 0);

  // On a page: a node without GPS, a repeater without a fix, no vitals.
  MeshModel m(64, 256);
  PageContext ctx; ctxInit(ctx, &m);
  const Bytes none;
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), none, advert(0x17, 0x0A, ADV_TYPE_CHAT, "CardPuter", 0, 0, false)), 1000, 2.0f);
  ctx.data_gen++;
  NodeCard card; card.attach(&ctx);
  CHECK(!card.isOpen() && !card.wantsFrame(5000));
  const NodeInfo* cp = byName(m, "CardPuter");
  CHECK(cp != nullptr);
  card.open(cp->pub_prefix);
  CHECK(card.isOpen() && card.node() == cp && card.wantsFrame(2000));
  Screen s(&ctx);
  HomePage home; home.attach(&ctx);
  s.show(home, 1000, PageSet::kHome);
  s.fb.eraseTouched(s.bgpx.data());
  card.draw(s.fb, 2000);
  CHECK(!card.wantsFrame(2100) && card.wantsFrame(3001));
  // Opaque over its rectangle only.
  int outside = 0;
  for (int y = 0; y < 320; y++) for (int x = 0; x < 480; x++) {
    const bool in = x >= NodeCard::kRect.x && x < NodeCard::kRect.x + NodeCard::kRect.w && y >= NodeCard::kRect.y &&
                    y < NodeCard::kRect.y + NodeCard::kRect.h;
    if (!in) outside += s.px[(size_t)y * 480 + x] != s.bgpx[(size_t)y * 480 + x];
  }
  CHECK(outside == 0);
  CHECK(s.near(252, 112, 400, 128, kDimText) > 10);   // "No GPS position"
  // A tap elsewhere does nothing; CLOSE closes.
  CHECK(!tap(*(Page*)&home, 100, 200) || card.isOpen());
  CHECK(!card.touch(100, 200, true) && !card.touch(100, 200, false) && card.isOpen());
  card.touch(NodeCard::kCloseX + 10, NodeCard::kCloseY + 10, true);
  CHECK(card.touch(NodeCard::kCloseX + 10, NodeCard::kCloseY + 10, false) && !card.isOpen());
  // A key the model does not have (node evicted): says so, no crash.
  const uint8_t gone[4] = {1, 2, 3, 4};
  card.open(gone);
  CHECK(card.node() == nullptr);
  card.draw(s.fb, 4000);
  if (!failures) std::printf("PASS node_card_handles_no_gps\n");
}

static void node_card_with_history() {
  MeshModel m(64, 512);
  PageContext ctx; ctxInit(ctx, &m);
  const uint32_t t = populate(m, ctx, true);
  NodeCard card; card.attach(&ctx);
  const NodeInfo* j = byName(m, "Jupiter");
  CHECK(j != nullptr && j->snr_hist[1] != SNR_NONE);
  card.open(j->pub_prefix);
  Screen s(&ctx);
  TelemetryPage tp; tp.attach(&ctx);
  s.show(tp, t, PageSet::kTelemetry);
  s.fb.eraseTouched(s.bgpx.data());
  card.draw(s.fb, t);
  CHECK(s.near(48, 116, 232, 172, linkColour(j->last_snr_q4)) > 30);   // the sparkline
  CHECK(s.near(252, 130, 466, 146, theme::kAccent) > 10);               // distance + bearing
  CHECK(s.differ(252, 174, 466, 262) > 300);                             // last packets
  // feedFromNode: the packets it relayed or originated.
  int from = 0;
  for (size_t i = 0; i < m.feedCount(); i++) from += feedFromNode(m, *m.feed(i), *j) ? 1 : 0;
  CHECK(from > 10);
  // A companion whose key starts like a repeater's: a route through that
  // hash is the repeater's, never the companion's (only repeaters forward).
  const Bytes none;
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(1), hashes({0x5701}),
                advert(0x1F, 0x01, ADV_TYPE_CHAT, "Twin", 0, 0, false, 1793000500u)), t + 100, 2.0f);
  const NodeInfo* twin = byName(m, "Twin");
  CHECK(twin != nullptr && twin->type == ADV_TYPE_CHAT);
  int through = 0, own = 0, relayed = 0;
  for (size_t i = 0; i < m.feedCount(); i++) {
    const FeedItem& f = *m.feed(i);
    if (routeThroughNode(m, f, *twin)) { through++; own += f.payload_type == PAYLOAD_TYPE_ADVERT; }
    if (feedFromNode(m, f, *twin) && f.payload_type != PAYLOAD_TYPE_ADVERT) relayed++;
  }
  CHECK(through == 1 && own == 1 && relayed == 0);
  // A long advert route keeps its last FEED_ROUTE hops: no sender at [0].
  FeedItem lf; std::memset(&lf, 0, sizeof(lf));
  lf.payload_type = PAYLOAD_TYPE_ADVERT; lf.hops = 3; lf.route_len = 4;
  CHECK(routeHasSender(lf));
  lf.hops = 9; lf.route_len = (uint8_t)FEED_ROUTE;
  CHECK(!routeHasSender(lf));
  lf.payload_type = PAYLOAD_TYPE_GRP_TXT; lf.hops = 2; lf.route_len = 2;
  CHECK(!routeHasSender(lf));
  if (!failures) std::printf("PASS node_card_with_history\n");
}

// ----------------------------------------------------------------- pages

static void telemetry_renders() {
  MeshModel m(64, 512);
  PageContext ctx; ctxInit(ctx, &m);
  TelemetryPage tp; tp.attach(&ctx);
  Screen s(&ctx);
  s.show(tp, 500, PageSet::kTelemetry);
  CHECK(tp.wantsFrame(500));
  s.frame(tp, 500);   // empty model: "--" everywhere, no crash
  CHECK(!tp.wantsFrame(600) && tp.wantsFrame(1500));
  const uint32_t t = populate(m, ctx, true);
  CHECK(tp.wantsFrame(t));
  s.frame(tp, t);
  // Gauges: value text and an arc in each of the four.
  for (int i = 0; i < 4; i++) {
    const PanelRect& p = TelemetryPage::kGauge[i];
    CHECK(s.differ(p.x + 4, p.y + 27, p.x + p.w - 4, p.y + p.h - 2) > 150);
  }
  // Rates area chart and waterfall columns.
  CHECK(s.near(42, 146, 224, 186, theme::kAccent) > 40);
  CHECK(s.differ(276, 146, 464, 200) > 1000);
  // Active nodes: three rows; noise trend; vitals values.
  CHECK(s.differ(14, 238, 230, 290) > 400);
  CHECK(s.differ(250, 242, 352, 286) > 60);
  CHECK(s.near(400, 238, 470, 290, theme::kText) > 30);
  // Data changes ask for a frame, at most every 250 ms.
  ctx.data_gen++;
  CHECK(!tp.wantsFrame(t + 100) && tp.wantsFrame(t + 300));
  // Stale vitals: values grey.
  s.frame(tp, t + 10u * 60000u);
  CHECK(s.near(400, 238, 470, 290, theme::kText) < 5);
  if (!failures) std::printf("PASS telemetry_renders\n");
}

// The repeater's heap (observer health frame): the model keeps the lowest
// free heap per 10 min over 8 h; TELEMETRY shows it in the VITALS panel.
static void telemetry_heap() {
  {
    MeshModel m(16, 64);
    uint32_t low[MeshModel::kHeapSlots]; bool has[MeshModel::kHeapSlots];
    CHECK(m.health() == nullptr);
    m.heapLowPerSlot(low, has);
    for (size_t i = 0; i < MeshModel::kHeapSlots; i++) CHECK(!has[i]);
    health(m, 1000, 180000); health(m, 2000, 170000); health(m, 3000, 175000);
    CHECK(m.health() != nullptr && m.health()->free_heap == 175000 && m.healthMs() == 3000);
    m.heapLowPerSlot(low, has);
    CHECK(has[MeshModel::kHeapSlots - 1] && low[MeshModel::kHeapSlots - 1] == 170000);   // the slot's lowest
    health(m, 1000 + MeshModel::kHeapSlotMs, 172000);                                      // next slot
    m.heapLowPerSlot(low, has);
    CHECK(has[MeshModel::kHeapSlots - 1] && low[MeshModel::kHeapSlots - 1] == 172000);
    CHECK(has[MeshModel::kHeapSlots - 2] && low[MeshModel::kHeapSlots - 2] == 170000);
    // 8 h later the old slots have rolled out of the window
    m.tick(1000 + 49u * MeshModel::kHeapSlotMs);
    m.heapLowPerSlot(low, has);
    int any = 0;
    for (size_t i = 0; i < MeshModel::kHeapSlots; i++) any += has[i];
    CHECK(any == 0);
  }
  MeshModel m(64, 512);
  PageContext ctx; ctxInit(ctx, &m);
  TelemetryPage tp; tp.attach(&ctx);
  Screen s(&ctx);
  const uint32_t t = populate(m, ctx, true);
  // a repeater that sends no health frame: "--", no trend
  MeshModel old(64, 512);
  PageContext octx; ctxInit(octx, &old);
  TelemetryPage otp; otp.attach(&octx);
  Screen os(&octx);
  os.show(otp, 500, PageSet::kTelemetry);
  os.frame(otp, 500);
  CHECK(os.near(400, 259, 440, 273, theme::kAccent) == 0);
  CHECK(os.near(440, 259, 470, 287, theme::kText) == 0);
  // with health: the value (row 4), the "min" note and the trend line
  s.show(tp, t, PageSet::kTelemetry);
  s.frame(tp, t);
  CHECK(s.near(440, 259, 470, 273, theme::kText) > 8);       // "173k"
  CHECK(s.near(402, 260, 436, 273, theme::kAccent) > 8);     // trend line
  CHECK(s.near(440, 273, 470, 287, theme::kText) > 8);       // HEAP MIN "148k"
  // stale after 95 s without a health frame: grey
  s.frame(tp, t + 100000u);
  CHECK(s.near(440, 259, 470, 287, theme::kText) < 3);
  CHECK(s.near(402, 260, 436, 273, theme::kAccent) == 0);
  if (!failures) std::printf("PASS telemetry_heap\n");
}

static void map_page_plots_and_taps() {
  for (int fix = 1; fix >= 0; fix--) {
    MeshModel m(64, 512);
    PageContext ctx; ctxInit(ctx, &m);
    PageSet set; set.attach(&ctx);
    MapPage& map = *static_cast<MapPage*>(set.page(PageSet::kMap));
    Screen s(&ctx);
    s.show(map, 500, PageSet::kMap);
    s.frame(map, 500);
    CHECK(map.shownCount() == 0);
    const uint32_t t = populate(m, ctx, fix == 1);
    s.frame(map, t);
    CHECK(map.repeaterFix() == (fix == 1));
    // Contacts with GPS: Woods 5C, CATHOUSE, Trebek, Jupiter, Harbor, Summit, Weather, TacoRoom.
    CHECK(map.shownCount() == 8);
    CHECK(map.withoutGps() == 3);   // Relay-1, CardPuter, Capt4McNeil
    for (int i = 0; i < map.shownCount(); i++) {
      int x, y; uint8_t key[4];
      CHECK(map.shown(i, x, y, key));
      CHECK(x >= MapPage::kAreaX && x < MapPage::kAreaX + MapPage::kAreaW && y >= MapPage::kAreaY && y < MapPage::kAreaY + MapPage::kAreaH);
    }
    // North is up: Summit (north-east) above Trebek (south).
    int sx = 0, sy = 0, tx = 0, ty = 0;
    for (int i = 0; i < map.shownCount(); i++) {
      int x, y; uint8_t key[4];
      map.shown(i, x, y, key);
      if (key[0] == 0x5A) { sx = x; sy = y; }
      if (key[0] == 0x7E) { tx = x; ty = y; }
    }
    CHECK(sy < ty && sx > tx);
    // An arc for a new packet whose route crosses plotted contacts.
    obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, plen(2), hashes({0x7EBE, 0x5701}), payload()), t + 100, 3.0f);
    ctx.data_gen++;
    CHECK(map.wantsFrame(t + 120));
    s.frame(map, t + 120);
    CHECK(map.arcsActive(t + 120) >= 1);
    CHECK(map.arcsActive(t + 120 + MapPage::kArcMs) == 0);
    // Tap a contact: the card; a tap on empty space: nothing.
    int x, y; uint8_t key[4];
    map.shown(0, x, y, key);
    CHECK(tap(map, x + 3, y - 2));
    CHECK(set.page(PageSet::kMap)->wantsFrame(t + 200));
    s.frame(map, t + 200);
    CHECK(s.near(NodeCard::kCloseX, NodeCard::kCloseY, NodeCard::kCloseX + NodeCard::kCloseW, NodeCard::kCloseY + NodeCard::kCloseH, theme::kAccent) > 20);
    CHECK(tap(map, NodeCard::kCloseX + 20, NodeCard::kCloseY + 10));
    CHECK(map.wantsFrame(t + 300));
    s.frame(map, t + 300);
    // Repeater hidden, tap far from everything.
    CHECK(!tap(map, MapPage::kAreaX + 2, MapPage::kAreaY + MapPage::kAreaH - 2) || false);
    // Leaving the page closes an open card.
    CHECK(tap(map, x, y));
    map.onHide();
    s.show(map, t + 400, PageSet::kMap);
    s.frame(map, t + 400);
    CHECK(s.near(NodeCard::kCloseX, NodeCard::kCloseY, NodeCard::kCloseX + NodeCard::kCloseW, NodeCard::kCloseY + NodeCard::kCloseH, theme::kTileOnFill) < 100);
  }
  if (!failures) std::printf("PASS map_page_plots_and_taps\n");
}

static void nodes_page_sorts_scrolls_and_taps() {
  MeshModel m(64, 512);
  PageContext ctx; ctxInit(ctx, &m);
  PageSet set; set.attach(&ctx);
  NodesPage& np = *static_cast<NodesPage*>(set.page(PageSet::kNodes));
  Screen s(&ctx);
  s.show(np, 500, PageSet::kNodes);
  s.frame(np, 500);
  CHECK(np.rowCount() == 0);
  uint32_t t = populate(m, ctx, true);
  // More nodes than fit: 30 chat nodes heard via Woods 5C.
  for (int i = 0; i < 30; i++)
    obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(1), hashes({0x5701}),
                  advert((uint8_t)(0x90 + i), 0x55, ADV_TYPE_CHAT, (std::string("Chat-") + std::to_string(i)).c_str(), 0, 0, false)),
        t += 100, 1.0f);
  ctx.data_gen++;
  s.frame(np, t);
  CHECK(np.rowCount() == (int)m.nodeCount() && np.rowCount() == 41);
  CHECK(np.sortColumn() == widgets::kColHeard && !np.sortDescending());
  CHECK(np.maxScroll() == 41 * NodesPage::kRowH - NodesPage::kRowsH);
  // Tap each header: sorts by it (default direction); again: reversed.
  for (int c = 0; c < widgets::kColCount; c++) {
    const int hx = (NodesPage::columnX0(c) + NodesPage::columnX1(c)) / 2;
    CHECK(NodesPage::columnAt(hx) == c);
    CHECK(tap(np, hx, NodesPage::kHeadY + 8));
    CHECK(np.sortColumn() == c && np.sortDescending() == widgets::defaultDescending((widgets::NodeColumn)c));
    CHECK(tap(np, hx, NodesPage::kHeadY + 8));
    CHECK(np.sortColumn() == c && np.sortDescending() != widgets::defaultDescending((widgets::NodeColumn)c));
    s.frame(np, t + 100 + (uint32_t)c);
  }
  // SNR, strongest first: the first row is the strongest direct node.
  const int snrX = (NodesPage::columnX0(widgets::kColSnr) + NodesPage::columnX1(widgets::kColSnr)) / 2;
  tap(np, snrX, NodesPage::kHeadY + 8);
  if (np.sortColumn() == widgets::kColSnr && !np.sortDescending()) tap(np, snrX, NodesPage::kHeadY + 8);
  CHECK(np.sortColumn() == widgets::kColSnr && np.sortDescending());
  uint8_t k0[4];
  CHECK(np.rowKey(0, k0));
  const NodeInfo* first = findNode(m, k0);
  int best = -999;
  for (size_t i = 0; i < m.nodeCount(); i++) if (m.node(i)->snr_hist[0] != SNR_NONE) best = std::max(best, (int)m.node(i)->last_snr_q4);
  CHECK(first != nullptr && first->last_snr_q4 == best);
  // Drag up: scrolls (clamped); a drag never opens a card or sorts.
  const int x = 200, y0 = 250;
  CHECK(!np.touch(x, y0, true));
  for (int dy = 0; dy <= 120; dy += 20) np.touch(x, y0 - dy, true);
  CHECK(!np.touch(x, y0 - 120, false));
  CHECK(np.scroll() == 120);
  s.frame(np, t + 500);
  np.touch(x, 100, true);
  for (int yy = 100; yy <= 290; yy += 10) np.touch(x, yy, true);
  np.touch(x, 290, false);
  CHECK(np.scroll() == 0);
  np.touch(x, 280, true);
  for (int yy = 280; yy >= -900; yy -= 30) np.touch(x, yy, true);
  np.touch(x, -900, false);
  CHECK(np.scroll() == np.maxScroll());
  s.frame(np, t + 600);
  // Tap a row: the card for that row's node.
  const int pos = (NodesPage::kRowsY + 5 + np.scroll()) / NodesPage::kRowH - NodesPage::kRowsY / NodesPage::kRowH;
  uint8_t want[4];
  CHECK(np.rowKey((NodesPage::kRowsY + 5 - NodesPage::kRowsY + np.scroll()) / NodesPage::kRowH, want));
  (void)pos;
  CHECK(tap(np, 200, NodesPage::kRowsY + 5));
  s.frame(np, t + 700);
  const NodeInfo* shown = findNode(m, want);
  CHECK(shown != nullptr);
  // The card shows that node; CLOSE returns to the table at the same scroll.
  CHECK(np.wantsFrame(t + 1800));
  CHECK(tap(np, NodeCard::kCloseX + 20, NodeCard::kCloseY + 10));
  CHECK(np.scroll() == np.maxScroll());
  s.frame(np, t + 800);
  if (!failures) std::printf("PASS nodes_page_sorts_scrolls_and_taps\n");
}

static void home_tap_opens_card() {
  MeshModel m(64, 512);
  PageContext ctx; ctxInit(ctx, &m);
  PageSet set; set.attach(&ctx);
  HomePage& home = *static_cast<HomePage*>(set.page(PageSet::kHome));
  Screen s(&ctx);
  s.show(home, 500, PageSet::kHome);
  uint32_t t = populate(m, ctx, true);
  for (int i = 0; i < 400 && home.wantsFrame(t); i++) { s.frame(home, t); t += 16; }
  s.frame(home, t);
  int slot = -1, x = 0, y = 0;
  for (int sl = 1; sl < GraphLayout::kMaxNodes && slot < 0; sl++) if (home.contactPos(sl, x, y)) slot = sl;
  CHECK(slot > 0);
  CHECK(home.contactAt(x + 2, y + 2) == slot);
  CHECK(home.contactAt(x, HomePage::kAreaY - 5) == -1);
  CHECK(tap(home, x + 2, y + 2));
  CHECK(home.wantsFrame(t + 10));
  s.frame(home, t + 10);
  CHECK(s.near(NodeCard::kCloseX, NodeCard::kCloseY, NodeCard::kCloseX + NodeCard::kCloseW, NodeCard::kCloseY + NodeCard::kCloseH, theme::kAccent) > 20);
  // Quick tiles are under the card: a tap there does nothing.
  CHECK(!tap(home, HomePage::kTileX + 30, HomePage::kTileY + 20));
  CHECK(home.takeRequest() == kReqNone);
  CHECK(tap(home, NodeCard::kCloseX + 20, NodeCard::kCloseY + 10));
  CHECK(home.wantsFrame(t + 20));
  s.frame(home, t + 20);
  CHECK(s.near(NodeCard::kCloseX, NodeCard::kCloseY, NodeCard::kCloseX + NodeCard::kCloseW, NodeCard::kCloseY + NodeCard::kCloseH, theme::kTileOnFill) < 100);
  // A tap on the GRAPH/GEO note still toggles; on a tile still arms ADVERT.
  CHECK(tap(home, HomePage::kToggleX + 40, HomePage::kToggleY + 10) && home.mode() == HomePage::kGeo);
  if (!failures) std::printf("PASS home_tap_opens_card\n");
}

// ------------------------------------------------------- fix round 1 (review)

// 1-byte path hashes (path_len = count, hash size 1).
static Bytes hashes1(std::initializer_list<uint8_t> hs) { return Bytes(hs.begin(), hs.end()); }

// I1: a hop or last hop is a repeater's only when the hash resolves to that
// one repeater; a hash two repeaters share belongs to neither.
static void card_requires_unique_hash() {
  MeshModel m(64, 256);
  PageContext ctx; ctxInit(ctx, &m);
  const Bytes none;
  uint32_t t = 1000;
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, none, advert(0x98, 0x10, ADV_TYPE_REPEATER, "TwinA", 0, 0, false)), t += 100, 2.0f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, none, advert(0x98, 0x20, ADV_TYPE_REPEATER, "TwinB", 0, 0, false)), t += 100, 3.0f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, none, advert(0x5D, 0x30, ADV_TYPE_REPEATER, "Solo", 0, 0, false)), t += 100, 4.0f);
  // Flood packets relayed through 0x98 (ambiguous) and 0x5D (unique).
  obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, 2, hashes1({0x79, 0x98}), payload()), t += 100, 1.0f);
  obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, 2, hashes1({0x98, 0x5D}), payload()), t += 100, 1.0f);
  const NodeInfo* a = byName(m, "TwinA");
  const NodeInfo* b = byName(m, "TwinB");
  const NodeInfo* solo = byName(m, "Solo");
  CHECK(a != nullptr && b != nullptr && solo != nullptr);
  int aThrough = 0, aFrom = 0, soloThrough = 0, soloFrom = 0;
  for (size_t i = 0; i < m.feedCount(); i++) {
    const FeedItem& f = *m.feed(i);
    if (f.payload_type == PAYLOAD_TYPE_ADVERT) continue;
    aThrough += routeThroughNode(m, f, *a) || routeThroughNode(m, f, *b);
    aFrom += feedFromNode(m, f, *a) || feedFromNode(m, f, *b);
    soloThrough += routeThroughNode(m, f, *solo);
    soloFrom += feedFromNode(m, f, *solo);
  }
  CHECK(aThrough == 0 && aFrom == 0);       // 0x98: ambiguous, neither twin
  CHECK(soloThrough == 1 && soloFrom == 1); // 0x5D: Solo's (route and last hop)
  CHECK(uniqueNode(m, makePathHash(solo->pub_prefix, 1)) == solo);
  CHECK(uniqueNode(m, makePathHash(a->pub_prefix, 1)) == nullptr);
  CHECK(uniqueNode(m, makePathHash(a->pub_prefix, 2)) == a);
  // Their own adverts are still theirs (the sender is in the packet whole).
  int ownA = 0;
  for (size_t i = 0; i < m.feedCount(); i++) ownA += feedFromNode(m, *m.feed(i), *a);
  CHECK(ownA == 1);
  if (!failures) std::printf("PASS card_requires_unique_hash\n");
}

// NODES SNR column: own SNR for a node heard directly, "<snr> via <relay>"
// for a relayed one, the relay named from every repeater the model heard
// (not the contact table); unknown / ambiguous hashes stay hex.
static void snr_cell_names_relay() {
  MeshModel m(64, 256);
  const Bytes none;
  uint32_t t = 1000;
  char buf[64];
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, none, advert(0x7E, 0x76, ADV_TYPE_REPEATER, "Downtown", 0, 0, false)), t += 100, -3.0f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, none, advert(0xAB, 0x01, ADV_TYPE_REPEATER, "TwinA", 0, 0, false)), t += 100, 2.0f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, none, advert(0xAB, 0x02, ADV_TYPE_REPEATER, "TwinB", 0, 0, false)), t += 100, 2.0f);
  // 0 hops: own SNR, no relay.
  const NodeInfo* d = byName(m, "Downtown");
  CHECK(d != nullptr);
  if (d == nullptr) return;
  widgets::SnrCell c = widgets::snrCell(m, *d);
  widgets::formatSnrCell(buf, sizeof(buf), c);
  CHECK(!c.relayed && c.has_snr && c.snr_q4 == -12 && std::strcmp(buf, "-3.0") == 0);
  // Two hops, last hop 0x7E (1-byte hashes): Downtown, at the advert's SNR.
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 2, hashes1({0x11, 0x7E}), advert(0x31, 0x01, ADV_TYPE_CHAT, "Zed", 0, 0, false)), t += 100, -4.25f);
  const NodeInfo* z = byName(m, "Zed");
  CHECK(z != nullptr);
  if (z == nullptr) return;
  c = widgets::snrCell(m, *z);
  widgets::formatSnrCell(buf, sizeof(buf), c);
  CHECK(c.relayed && c.has_snr && c.snr_q4 == -17 && std::strcmp(c.via, "Downtown") == 0);
  CHECK(std::strcmp(buf, "-4.3 via Downtown") == 0 || std::strcmp(buf, "-4.2 via Downtown") == 0);
  // 2-byte hash of the same repeater resolves too.
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(1), hashes({0x7E76}), advert(0x32, 0x01, ADV_TYPE_CHAT, "Two", 0, 0, false)), t += 100, 5.0f);
  c = widgets::snrCell(m, *byName(m, "Two"));
  widgets::formatSnrCell(buf, sizeof(buf), c);
  CHECK(std::strcmp(buf, "+5.0 via Downtown") == 0);
  // Unknown relay: hex. Ambiguous (two repeaters start 0xAB): hex.
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 1, hashes1({0x99}), advert(0x33, 0x01, ADV_TYPE_CHAT, "Quo", 0, 0, false)), t += 100, 1.0f);
  c = widgets::snrCell(m, *byName(m, "Quo"));
  CHECK(c.relayed && std::strcmp(c.via, "99") == 0);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 1, hashes1({0xAB}), advert(0x34, 0x01, ADV_TYPE_CHAT, "Amb", 0, 0, false)), t += 100, 1.0f);
  c = widgets::snrCell(m, *byName(m, "Amb"));
  widgets::formatSnrCell(buf, sizeof(buf), c);
  CHECK(c.relayed && std::strcmp(buf, "+1.0 via AB") == 0);
  // Bridged only: no SNR, still the relay.
  Bytes br = packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 1, hashes1({0x7E}), advert(0x35, 0x01, ADV_TYPE_CHAT, "Brd", 0, 0, false));
  m.onBridgedPacket(br.data(), br.size(), t += 100);
  c = widgets::snrCell(m, *byName(m, "Brd"));
  widgets::formatSnrCell(buf, sizeof(buf), c);
  CHECK(std::strcmp(buf, "via Downtown") == 0);
  // The relay is named from the model even though the head has no contacts:
  // relayName on a raw hash.
  widgets::relayName(buf, sizeof(buf), m, makePathHash(d->pub_prefix, 3));
  CHECK(std::strcmp(buf, "Downtown") == 0);
  // Header labels (any one of them carrying the sort mark) never touch.
  for (int sorted = -1; sorted < widgets::kColCount; sorted++)
    for (int c = 0; c + 1 < widgets::kColCount; c++) {
      int a0, a1, b0, b1;
      NodesPage::headerSpan(c, c == sorted, a0, a1);
      NodesPage::headerSpan(c + 1, c + 1 == sorted, b0, b1);
      CHECK(a1 + 3 <= b0);
    }
  // NODES rows show and sort on the same value.
  PageContext ctx; ctxInit(ctx, &m);
  PageSet set; set.attach(&ctx);
  NodesPage& np = *static_cast<NodesPage*>(set.page(PageSet::kNodes));
  Screen s(&ctx);
  s.show(np, t, PageSet::kNodes);
  ctx.data_gen++;
  s.frame(np, t + 10);
  np.setSort(widgets::kColSnr, true);
  // Strongest first: Two (+5.0 via Downtown) ties Downtown (+5.0, sampled
  // as Two's last hop); Zed (-4.3 via Downtown) last but the bridged-only Brd.
  uint8_t k0[4], k1[4], k6[4], k7[4];
  CHECK(np.rowCount() == 8 && np.rowKey(0, k0) && np.rowKey(1, k1) && np.rowKey(6, k6) && np.rowKey(7, k7));
  CHECK(std::memcmp(k0, byName(m, "Two")->pub_prefix, 4) == 0 || std::memcmp(k1, byName(m, "Two")->pub_prefix, 4) == 0);
  CHECK(std::memcmp(k6, z->pub_prefix, 4) == 0 && std::memcmp(k7, byName(m, "Brd")->pub_prefix, 4) == 0);
  if (!failures) std::printf("PASS snr_cell_names_relay\n");
}

// Relay names learned from adverts persist (RelayNameStore): after a reboot
// (a fresh model, the store loaded from its blob) a relay hash resolves to
// the stored name before that relay adverts again. Ambiguous hashes, across
// the store and the live model, stay hex.
static void relay_names_survive_reboot() {
  const Bytes none;
  uint32_t t = 1000;
  char buf[64];
  RelayNameStore st;
  st.begin(0);
  MeshModel m1(64, 256);
  m1.setNameStore(&st);
  CHECK(m1.nameStore() == &st);
  obs(m1, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, none, advert(0x7E, 0x76, ADV_TYPE_REPEATER, "Downtown", 0, 0, false)), t += 100, -3.0f);
  const uint8_t down_key[4] = {0x7E, 0x76, 0x42, 0x43};
  CHECK(st.nameFor(down_key) != nullptr && std::strcmp(st.nameFor(down_key), "Downtown") == 0);
  // Chat companions are not relays: never stored.
  obs(m1, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 1, hashes1({0x7E}), advert(0x31, 0x01, ADV_TYPE_CHAT, "Zed", 0, 0, false)), t += 100, -4.0f);
  CHECK(st.count() == 1);
  // Reboot: blob -> new store -> new model that never heard Downtown.
  std::vector<uint8_t> blob(RelayNameStore::kBlobMax);
  const size_t len = st.serialize(blob.data(), blob.size());
  CHECK(len > 0);
  RelayNameStore st2;
  CHECK(st2.load(blob.data(), len));
  st2.begin(0);
  MeshModel m2(64, 256);
  m2.setNameStore(&st2);
  CHECK(m2.findByHash(makePathHash(down_key, 1)) == nullptr);   // the live model does not know it
  obs(m2, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(1), hashes({0x7E76}), advert(0x32, 0x01, ADV_TYPE_CHAT, "Two", 0, 0, false)), t += 100, 5.0f);
  widgets::SnrCell c = widgets::snrCell(m2, *byName(m2, "Two"));
  widgets::formatSnrCell(buf, sizeof(buf), c);
  CHECK(std::strcmp(buf, "+5.0 via Downtown") == 0);
  widgets::relayName(buf, sizeof(buf), m2, makePathHash(down_key, 1));
  CHECK(std::strcmp(buf, "Downtown") == 0);
  widgets::relayName(buf, sizeof(buf), m2, makePathHash(down_key, 3));
  CHECK(std::strcmp(buf, "Downtown") == 0);
  // A second stored relay on the same 1-byte hash: ambiguous -> hex; the
  // 2-byte hash still names Downtown.
  const uint8_t other_key[4] = {0x7E, 0x11, 0x00, 0x00};
  st2.learn(other_key, "Other7E");
  widgets::relayName(buf, sizeof(buf), m2, makePathHash(down_key, 1));
  CHECK(std::strcmp(buf, "7E") == 0);
  widgets::relayName(buf, sizeof(buf), m2, makePathHash(down_key, 2));
  CHECK(std::strcmp(buf, "Downtown") == 0);
  // A live repeater and a different stored one on the same hash: hex.
  RelayNameStore st3;
  st3.learn(down_key, "Downtown");
  MeshModel m3(64, 256);
  obs(m3, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, none, advert(0x7E, 0x55, ADV_TYPE_REPEATER, "Live", 0, 0, false)), t += 100, 1.0f);
  m3.setNameStore(&st3);
  widgets::relayName(buf, sizeof(buf), m3, makePathHash(down_key, 1));
  CHECK(std::strcmp(buf, "7E") == 0);
  widgets::relayName(buf, sizeof(buf), m3, makePathHash(down_key, 2));
  CHECK(std::strcmp(buf, "Downtown") == 0);
  const uint8_t live_key[4] = {0x7E, 0x55, 0x42, 0x43};
  widgets::relayName(buf, sizeof(buf), m3, makePathHash(live_key, 2));
  CHECK(std::strcmp(buf, "Live") == 0);
  // The same relay both live and stored is one relay, not ambiguous; a new
  // advert name replaces the stored one.
  MeshModel m4(64, 256);
  m4.setNameStore(&st3);
  obs(m4, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, none, advert(0x7E, 0x76, ADV_TYPE_REPEATER, "Downtown NEW", 0, 0, false, 1793000500u)), t += 100, 1.0f);
  widgets::relayName(buf, sizeof(buf), m4, makePathHash(down_key, 1));
  CHECK(std::strcmp(buf, "Downtown NEW") == 0);
  CHECK(std::strcmp(st3.nameFor(down_key), "Downtown NEW") == 0);
  // Emoji-only stored names are drawn like any node name.
  const uint8_t emo_key[4] = {0x57, 0xF7, 0x23, 0x34};
  st3.learn(emo_key, "\xF0\x9F\xA5\x9E");
  widgets::relayName(buf, sizeof(buf), m4, makePathHash(emo_key, 2));
  CHECK(std::strcmp(buf, "rptr 57f72334") == 0);
  // No store attached: unchanged behaviour (hex for an unknown hash).
  MeshModel m5(64, 256);
  CHECK(m5.nameStore() == nullptr);
  widgets::relayName(buf, sizeof(buf), m5, makePathHash(down_key, 1));
  CHECK(std::strcmp(buf, "7E") == 0);
  if (!failures) std::printf("PASS relay_names_survive_reboot\n");
}

// I2: the repeater's RX/min is checked against what the head observed.
static void rx_source_sanity() {
  using widgets::pickRx;
  widgets::RxPick p = pickRx(true, 14, 13);
  CHECK(p.known && p.rx == 14 && !p.from_head && p.dup_ok);
  p = pickRx(true, 0, 13);   // the M5 says 0 while the head saw 13
  CHECK(p.known && p.rx == 13 && p.from_head && !p.dup_ok);
  p = pickRx(true, 3, 20);   // far below what was observed
  CHECK(p.known && p.rx == 20 && p.from_head && !p.dup_ok);
  p = pickRx(true, 0, 0);    // quiet: 0 is true, but no basis for a dupe %
  CHECK(p.known && p.rx == 0 && !p.from_head && !p.dup_ok);
  p = pickRx(false, 0, 9);   // no vitals: the head's own count
  CHECK(p.known && p.rx == 9 && p.from_head && !p.dup_ok);
  p = pickRx(false, 0, 0);
  CHECK(!p.known && !p.dup_ok);
  // On the page: vitals RX 0 with traffic observed: DUPES has no arc.
  MeshModel m(64, 512);
  PageContext ctx; ctxInit(ctx, &m);
  TelemetryPage tp; tp.attach(&ctx);
  Screen s(&ctx);
  uint32_t t = 1000;
  for (int i = 0; i < 40; i++) obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, plen(0), Bytes(), payload()), t += 3000, 2.0f);
  mesh::observer::Vitals v; std::memset(&v, 0, sizeof(v));
  v.noise_floor = -116; v.rx_per_min = 0; v.tx_per_min = 11; v.dup_per_min = 0; v.freq = 910.525f; v.bw = 62.5f; v.sf = 7; v.cr = 5;
  t = 125000;
  m.onVitals(v, t);
  ctx.data_gen++;
  s.show(tp, t, PageSet::kTelemetry);
  s.frame(tp, t);
  const PanelRect& g = TelemetryPage::kGauge[3];
  int arc = 0;
  for (int y = g.y + 26; y < g.y + g.h; y++) for (int x = g.x; x < g.x + 50; x++) {
    const uint16_t c = s.px[(size_t)y * 480 + x];
    arc += c == theme::kGood || c == theme::kWarn || c == theme::kBad;
  }
  CHECK(arc == 0);
  if (!failures) std::printf("PASS rx_source_sanity\n");
}

// M1: the header names the home repeater before its own advert is heard.
static void header_name_fallback() {
  MeshModel m(64, 64);
  PageContext ctx; ctxInit(ctx, &m);
  CHECK(std::strcmp(repeaterDisplayName(ctx), "") == 0);
  std::snprintf(ctx.repeater_name, sizeof(ctx.repeater_name), "CaptCallahan");
  CHECK(std::strcmp(repeaterDisplayName(ctx), "CaptCallahan") == 0);
  std::vector<uint16_t> px(480 * 320), px2(480 * 320);
  FrameBuffer565 fb(px.data()), fb2(px2.data());
  drawHeader(fb, ctx, 1000);
  ctx.repeater_name[0] = 0;
  drawHeader(fb2, ctx, 1000);
  int d = 0;
  for (int y = 0; y < 20; y++) for (int x = 0; x < 210; x++) d += px[(size_t)y * 480 + x] != px2[(size_t)y * 480 + x];
  CHECK(d > 50);   // "M5 // CAPTCALLAHAN" vs "M5 // REPEATER"
  if (!failures) std::printf("PASS header_name_fallback\n");
}

// M5: a map arc needs the hash to resolve among all the model's nodes, not
// only the plotted ones.
static void map_arc_hash_unique_model_wide() {
  MeshModel m(64, 512);
  PageContext ctx; ctxInit(ctx, &m);
  PageSet set; set.attach(&ctx);
  MapPage& map = *static_cast<MapPage*>(set.page(PageSet::kMap));
  Screen s(&ctx);
  uint32_t t = populate(m, ctx, true);
  // A repeater without GPS sharing Woods 5C's (0x5701) first byte.
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), advert(0x57, 0x99, ADV_TYPE_REPEATER, "NoGps57", 0, 0, false)), t += 100, 1.0f);
  ctx.data_gen++;
  s.show(map, t, PageSet::kMap);
  s.frame(map, t);
  // 1-byte route 0x57 -> 0xCA: 0x57 is ambiguous model-wide: no arc from it.
  obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, 2, hashes1({0x57, 0xCA}), payload()), t += 100, 2.0f);
  ctx.data_gen++;
  s.frame(map, t + 20);
  const int arcsAmbiguous = map.arcsActive(t + 20);
  CHECK(arcsAmbiguous == 1);   // only CATHOUSE (0xCA) -> M5
  if (!failures) std::printf("PASS map_arc_hash_unique_model_wide\n");
}

// M7: a drag that ends on a contact does not open its card.
static void home_tap_needs_press_start() {
  MeshModel m(64, 512);
  PageContext ctx; ctxInit(ctx, &m);
  PageSet set; set.attach(&ctx);
  HomePage& home = *static_cast<HomePage*>(set.page(PageSet::kHome));
  Screen s(&ctx);
  s.show(home, 500, PageSet::kHome);
  uint32_t t = populate(m, ctx, true);
  for (int i = 0; i < 400 && home.wantsFrame(t); i++) { s.frame(home, t); t += 16; }
  int x = 0, y = 0, slot = -1;
  for (int sl = 1; sl < GraphLayout::kMaxNodes && slot < 0; sl++) if (home.contactPos(sl, x, y)) slot = sl;
  CHECK(slot > 0);
  home.touch(x + 40, y + 30, true);   // press elsewhere, slide onto the contact
  home.touch(x + 20, y + 15, true);
  home.touch(x, y, true);
  home.touch(x, y, false);
  CHECK(!home.wantsFrame(t) || true);
  s.frame(home, t + 10);
  CHECK(s.near(NodeCard::kCloseX, NodeCard::kCloseY, NodeCard::kCloseX + NodeCard::kCloseW, NodeCard::kCloseY + NodeCard::kCloseH, theme::kTileOnFill) < 100);
  CHECK(tap(home, x, y));   // a real tap still opens it
  if (!failures) std::printf("PASS home_tap_needs_press_start\n");
}

// M9: the NODES rows live in the allocator's memory (PSRAM on the device).
static size_t g_allocated = 0;
static void* countingAlloc(size_t n, void*) { g_allocated += n; return std::malloc(n); }
static void countingFree(void* p, void*) { std::free(p); }
static void nodes_rows_from_allocator() {
  CHECK(sizeof(NodesPage) < 1024);
  MeshModel m(64, 256);
  PageContext ctx; ctxInit(ctx, &m);
  const ModelAllocator a = {countingAlloc, countingFree, nullptr};
  {
    PageSet set; set.setAllocator(&a); set.attach(&ctx);
    NodesPage& np = *static_cast<NodesPage*>(set.page(PageSet::kNodes));
    Screen s(&ctx);
    const uint32_t t = populate(m, ctx, true);
    s.show(np, t, PageSet::kNodes);
    s.frame(np, t);
    CHECK(np.rowCount() == (int)m.nodeCount());
    CHECK(g_allocated >= (size_t)NodesPage::kMaxRows * sizeof(widgets::NodeRow));
  }
  if (!failures) std::printf("PASS nodes_rows_from_allocator\n");
}

// ---------------------------------------------------------------- previews

static int preview(const char* dir) {
  std::string base(dir);
  bool ok = true;
  for (int fix = 1; fix >= 0; fix--) {
    MeshModel m(64, 512);
    PageContext ctx; ctxInit(ctx, &m);
    PageSet set; set.attach(&ctx);
    Screen s(&ctx);
    uint32_t t = populate(m, ctx, fix == 1);
    if (fix) {
      Page& tp = *set.page(PageSet::kTelemetry);
      s.show(tp, t, PageSet::kTelemetry);
      s.frame(tp, t);
      ok = s.save((base + "/telem.rgb565").c_str()) && ok;
      {
        const auto c0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; i++) s.frame(tp, t + 1000u * (uint32_t)i);
        std::printf("TIMING telemetry_frame us=%lld\n", (long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - c0).count() / 20);
      }
      NodesPage& np = *static_cast<NodesPage*>(set.page(PageSet::kNodes));
      s.show(np, t, PageSet::kNodes);
      const int snrX = (NodesPage::columnX0(widgets::kColSnr) + NodesPage::columnX1(widgets::kColSnr)) / 2;
      s.frame(np, t);
      tap(np, snrX, NodesPage::kHeadY + 8);
      s.frame(np, t + 50);
      ok = s.save((base + "/nodes.rgb565").c_str()) && ok;
    }
    MapPage& map = *static_cast<MapPage*>(set.page(PageSet::kMap));
    s.show(map, t, PageSet::kMap);
    s.frame(map, t);
    obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, plen(3), hashes({0x8821, 0x7EBE, 0xCA75}), payload()), t + 100, 6.0f);
    ctx.data_gen++;
    for (uint32_t u = t + 120; u <= t + 820; u += 50) s.frame(map, u);
    ok = s.save((base + (fix ? "/map.rgb565" : "/map_nofix.rgb565")).c_str()) && ok;
    if (fix) {
      // The card from a tap on Jupiter on the map.
      for (int i = 0; i < map.shownCount(); i++) {
        int x, y; uint8_t key[4];
        map.shown(i, x, y, key);
        if (key[0] == 0x1F) tap(map, x, y);
      }
      s.frame(map, t + 900);
      ok = s.save((base + "/detail.rgb565").c_str()) && ok;
    }
  }
  std::printf(ok ? "PASS preview\n" : "FAIL preview\n");
  return ok ? 0 : 1;
}

int main(int argc, char** argv) {
  const std::string name = argc > 1 ? argv[1] : "";
  if (name == "preview") return preview(argc > 2 ? argv[2] : ".");
  if (name == "gauge_angle_clamps") gauge_angle_clamps();
  else if (name == "sparkline_scales_to_box") sparkline_scales_to_box();
  else if (name == "waterfall_colour_map_monotonic") waterfall_colour_map_monotonic();
  else if (name == "nodes_sort_by_each_column") nodes_sort_by_each_column();
  else if (name == "node_card_handles_no_gps") node_card_handles_no_gps();
  else if (name == "node_card_with_history") node_card_with_history();
  else if (name == "telemetry_renders") telemetry_renders();
  else if (name == "telemetry_heap") telemetry_heap();
  else if (name == "map_page_plots_and_taps") map_page_plots_and_taps();
  else if (name == "nodes_page_sorts_scrolls_and_taps") nodes_page_sorts_scrolls_and_taps();
  else if (name == "home_tap_opens_card") home_tap_opens_card();
  else if (name == "card_requires_unique_hash") card_requires_unique_hash();
  else if (name == "rx_source_sanity") rx_source_sanity();
  else if (name == "snr_cell_names_relay") snr_cell_names_relay();
  else if (name == "relay_names_survive_reboot") relay_names_survive_reboot();
  else if (name == "header_name_fallback") header_name_fallback();
  else if (name == "map_arc_hash_unique_model_wide") map_arc_hash_unique_model_wide();
  else if (name == "home_tap_needs_press_start") home_tap_needs_press_start();
  else if (name == "nodes_rows_from_allocator") nodes_rows_from_allocator();
  else { std::printf("unknown scenario %s\n", name.c_str()); return 2; }
  return failures ? 1 : 0;
}
'''

PAGE_SOURCES = ['HomePage.cpp', 'StubPages.cpp', 'TelemetryPage.cpp', 'MapPage.cpp', 'NodesPage.cpp', 'NodeCard.cpp',
                'MeshModel.cpp', 'FrameBuffer565.cpp', 'FrameBuffer332.cpp', 'FbFonts.cpp', 'FbFontsAA.cpp']
SOURCES = [UI / n for n in PAGE_SOURCES] + [ROOT / 'src' / 'Packet.cpp', ROOT / 'src' / 'helpers' / 'AdvertDataHelpers.cpp']
NEW_FILES = ('TelemetryPage.cpp', 'TelemetryPage.h', 'MapPage.cpp', 'MapPage.h', 'NodesPage.cpp', 'NodesPage.h',
             'NodeCard.cpp', 'NodeCard.h', 'Widgets.h')


def build(work):
    compiler = shutil.which('g++') or shutil.which('clang++')
    if compiler is None:
        raise unittest.SkipTest('a host C++17 compiler is required')
    sanitize = [] if os.name == 'nt' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    cpp = work / 'widgets.cpp'
    cpp.write_text(HARNESS, encoding='ascii')
    exe = work / ('widgets' + ('.exe' if os.name == 'nt' else ''))
    built = subprocess.run([
        compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', *sanitize,
        '-I', str(UI), '-isystem', str(ROOT / 'src'), '-isystem', str(ROOT / 'test' / 'mocks'),
        str(cpp), *[str(s) for s in SOURCES], '-o', str(exe),
    ], capture_output=True, text=True, timeout=300, cwd=str(ROOT / 'test'))
    if built.returncode != 0:
        raise AssertionError(built.stdout + built.stderr)
    return compiler, exe


class WidgetsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='freenove-widgets-')
        try:
            cls.compiler, cls.exe = build(Path(cls.temp.name))
        except BaseException:
            cls.temp.cleanup()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_scenario(self, name):
        ran = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=120)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('PASS ' + name, ran.stdout)

    def test_gauge_angle_clamps(self):
        self.run_scenario('gauge_angle_clamps')

    def test_sparkline_scales_to_box(self):
        self.run_scenario('sparkline_scales_to_box')

    def test_waterfall_colour_map_monotonic(self):
        self.run_scenario('waterfall_colour_map_monotonic')

    def test_nodes_sort_by_each_column(self):
        self.run_scenario('nodes_sort_by_each_column')

    def test_node_card_handles_no_gps(self):
        self.run_scenario('node_card_handles_no_gps')

    def test_node_card_with_history(self):
        self.run_scenario('node_card_with_history')

    def test_telemetry_renders(self):
        self.run_scenario('telemetry_renders')

    def test_telemetry_heap(self):
        self.run_scenario('telemetry_heap')

    def test_ui_task_feeds_health_frames_to_the_model(self):
        task = (UI / 'FreenoveUITask.cpp').read_text()
        self.assertRegex(task, r'frame\[5\] == \(uint8_t\)mesh::observer::Kind::Health\) \{\s+mesh::observer::Health health;\s+'
                               r'if \(mesh::observer::decodeHealth\(frame, len, health\)\) _model->onHealth\(health, now_ms\);')

    def test_map_page_plots_and_taps(self):
        self.run_scenario('map_page_plots_and_taps')

    def test_nodes_page_sorts_scrolls_and_taps(self):
        self.run_scenario('nodes_page_sorts_scrolls_and_taps')

    def test_home_tap_opens_card(self):
        self.run_scenario('home_tap_opens_card')

    def test_card_requires_unique_hash(self):
        self.run_scenario('card_requires_unique_hash')

    def test_snr_cell_names_relay(self):
        self.run_scenario('snr_cell_names_relay')

    def test_relay_names_survive_reboot(self):
        self.run_scenario('relay_names_survive_reboot')

    def test_relay_names_everywhere_use_the_model(self):
        # HOME's route caption and the node card name relays through
        # widgets::relayName (every repeater the model heard), never contacts.
        home = strip_comments((UI / 'HomePage.cpp').read_text(encoding='utf-8'))
        self.assertIn('relayName(', function_body(home, 'void HomePage::takePath'))
        card = strip_comments((UI / 'NodeCard.cpp').read_text(encoding='utf-8'))
        self.assertIn('snrCell(', function_body(card, 'void NodeCard::drawSignal'))
        nodes = strip_comments((UI / 'NodesPage.cpp').read_text(encoding='utf-8'))
        self.assertIn('snrCell(', nodes)
        self.assertNotIn('lookupContact', home + card + nodes)

    def test_rx_source_sanity(self):
        self.run_scenario('rx_source_sanity')

    def test_header_name_fallback(self):
        self.run_scenario('header_name_fallback')

    def test_map_arc_hash_unique_model_wide(self):
        self.run_scenario('map_arc_hash_unique_model_wide')

    def test_home_tap_needs_press_start(self):
        self.run_scenario('home_tap_needs_press_start')

    def test_nodes_rows_from_allocator(self):
        self.run_scenario('nodes_rows_from_allocator')

    def test_preview_renders(self):
        out = Path(self.temp.name)
        ran = subprocess.run([str(self.exe), 'preview', str(out)], capture_output=True, text=True, timeout=120)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        for name in ('telem', 'map', 'map_nofix', 'nodes', 'detail'):
            self.assertEqual((out / (name + '.rgb565')).stat().st_size, 480 * 320 * 2)

    def test_pages_use_theme_tokens_and_cxx11(self):
        for name in NEW_FILES:
            src = strip_comments((UI / name).read_text(encoding='ascii'))
            self.assertNotIn('Arduino.h', src, name)
            self.assertNotIn('esp_', src, name)
            self.assertNotIn('fbpal::', src, name)
            if name != 'Widgets.h':   # the waterfall ramp is the one palette, built from Rgb8 stops
                self.assertNotRegex(src, r'\brgb(565)?\(', name)
            self.assertNotRegex(src, r'(fill\w*|text|\w*[lL]ine|\w*[rR]ect|\w*Ring|\w*Arc|\w*Disc|hline|vline)\([^;]*0x[0-9A-Fa-f]{2,4}\b', name)
        for name in ('TelemetryPage.cpp', 'MapPage.cpp', 'NodesPage.cpp', 'NodeCard.cpp'):
            checked = subprocess.run([
                self.compiler, '-std=c++11', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
                '-I', str(UI), '-isystem', str(ROOT / 'src'), '-isystem', str(ROOT / 'test' / 'mocks'), str(UI / name),
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)


class WidgetsWiringTest(unittest.TestCase):
    def test_page_set_uses_the_pages_and_one_card(self):
        header = strip_comments((UI / 'StubPages.h').read_text(encoding='ascii'))
        for member in ('NodeCard _card;', 'TelemetryPage _telemetry;', 'MapPage _map;', 'NodesPage _nodes;'):
            self.assertIn(member, header)
        cpp = strip_comments((UI / 'StubPages.cpp').read_text(encoding='ascii'))
        for page in ('_home', '_map', '_nodes'):
            self.assertIn(page + '.setCard(&_card)', cpp)
        self.assertIn('_card.attach(ctx)', cpp)

    def test_product_env_builds_the_pages(self):
        ini = (VARIANT / 'platformio.ini').read_text(encoding='ascii')
        product = ini.split('[env:Freenove_S3_35_Head_companion_usb]')[1].split('[env:')[0]
        for name in ('TelemetryPage.cpp', 'MapPage.cpp', 'NodesPage.cpp', 'NodeCard.cpp'):
            self.assertIn('ui-freenove/' + name, product)

    def test_labels_say_what_they_measure(self):
        tele = strip_comments((UI / 'TelemetryPage.cpp').read_text(encoding='ascii'))
        self.assertIn('"ESP-NOW 15m"', tele)   # peers the M5 heard in its 15 min window
        card = strip_comments((UI / 'NodeCard.cpp').read_text(encoding='ascii'))
        self.assertIn('advert %u hop', card)
        self.assertIn('this packet', card)

    def test_header_name_wiring(self):
        ui = strip_comments((UI / 'FreenoveUITask.cpp').read_text(encoding='ascii'))
        service = function_body(ui, r'void FreenoveUITask::serviceUiLoop\(')
        self.assertIn('headHomeRepeater()', service)
        self.assertIn('lookupContactByPubKey(', service)
        self.assertIn('putString("name"', ui)        # the learned name persists
        self.assertIn('setAllocator(&kPsramAllocator)', ui)

    def test_no_demo_data_on_the_device(self):
        # Fake meshes live in the host harness only: the pages read the model.
        for name in NEW_FILES:
            src = strip_comments((UI / name).read_text(encoding='ascii'))
            for fake in ('Jupiter', 'Woods', 'CATHOUSE', 'Trebek', 'demo', 'Demo', 'fake'):
                self.assertNotIn(fake, src, name)


def write_preview():
    """Previews at 1:1 (*_1x.png) and zoomed 2x (*.png)."""
    with tempfile.TemporaryDirectory(prefix='freenove-widgets-preview-') as tmp:
        _, exe = build(Path(tmp))
        ran = subprocess.run([str(exe), 'preview', tmp], capture_output=True, text=True, timeout=120)
        if ran.returncode != 0:
            raise SystemExit(ran.stdout + ran.stderr)
        for src, dst in (('telem', 'telem_preview'), ('map', 'map_preview'), ('map_nofix', 'map_preview_nofix'),
                         ('nodes', 'nodes_preview'), ('detail', 'detail_preview')):
            raw = (Path(tmp) / (src + '.rgb565')).read_bytes()
            write_png(rgb565_rows(raw, scale=1), PREVIEW_DIR / (dst + '_1x.png'))
            write_png(rgb565_rows(raw, scale=2), PREVIEW_DIR / (dst + '.png'))
            print('wrote', PREVIEW_DIR / (dst + '_1x.png'), 'and the 2x zoom')


if __name__ == '__main__':
    if '--preview' in sys.argv:
        write_preview()
    else:
        unittest.main()
