"""Freenove visual head HOME page: graph and geo layouts, the page itself
(rendered into an RGB565 framebuffer with a live model, HMI theme) and the
device glue that feeds it (bridged packets).

Host harness (g++, ASan/UBSan off Windows). Run directly with --preview to
write the owner preview images (HOME graph mid-animation, GEO, busy, a tile
in CONFIRM?, another tab, and HOME next to the approved mockup) into the
task folder (needs Pillow):

    python test_layouts.py --preview
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

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
#include "GeoLayout.h"
#include "GraphLayout.h"
#include "HomePage.h"
#include "RadioLabel.h"
#include "StubPages.h"
#include "HmiLogic.h"
using namespace freenove_ui;
using namespace mesh::head;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return (rng >> 8) & 0xFFFFFF; }

// ------------------------------------------------------------- graph layout

static void build_random(GraphLayout& g, int nodes, uint32_t seed) {
  rng = seed;
  for (int i = 0; i < nodes; i++) {
    const int anchor = i > 2 && rnd() % 2 ? 1 + (int)(rnd() % (uint32_t)i) : -1;
    const int s = g.add(0x10000000u + rnd(), (int)(rnd() % 4), anchor);
    CHECK(s > 0);
  }
  for (int l = 0; l < nodes; l++) {
    const int a = (int)(rnd() % (uint32_t)(nodes + 1)), b = (int)(rnd() % (uint32_t)(nodes + 1));
    if (a != b) g.link(a, b);
  }
}

static bool inside(const GraphLayout& g, int s, float w, float h) {
  return g.x(s) >= 0 && g.x(s) < w && g.y(s) >= 0 && g.y(s) < h;
}

// The brief's 340x270 and the page's real topology area.
static const float kSizes[2][2] = {{340, 270}, {(float)(HomePage::kAreaW - HomePage::kGraphRightMargin), (float)HomePage::kAreaH}};

static void graph_layout_converges_and_stays_on_screen() {
  for (int size = 0; size < 2; size++)
  for (int nodes = 20; nodes <= 31; nodes += 11)
  for (uint32_t seed = 1; seed <= 8; seed++) {
    const float W = kSizes[size][0], H = kSizes[size][1];
    GraphLayout g(W, H);
    build_random(g, nodes, seed * 7919u);
    CHECK(g.count() == nodes);
    float moved = 1e9f;
    for (int i = 0; i < 200; i++) {
      moved = g.step();
      for (int s = 0; s < GraphLayout::kMaxNodes; s++) if (g.used(s)) CHECK(inside(g, s, W, H));
    }
    CHECK(moved < 0.5f);
    CHECK(!g.hot());
    // The repeater never moves; nodes do not pile up on each other.
    CHECK(g.x(0) == W / 2 && g.y(0) == H / 2);
    float minD = 1e9f;
    for (int a = 0; a < GraphLayout::kMaxNodes; a++) for (int b = a + 1; b < GraphLayout::kMaxNodes; b++) {
      if (!g.used(a) || !g.used(b)) continue;
      const float dx = g.x(a) - g.x(b), dy = g.y(a) - g.y(b);
      minD = std::fmin(minD, std::sqrt(dx * dx + dy * dy));
    }
    if (!(minD > (nodes > 20 ? 3.0f : 6.0f))) std::printf("minD %.2f size %d nodes %d seed %u\n", minD, size, nodes, seed);
    CHECK(minD > (nodes > 20 ? 3.0f : 6.0f));
  }
  // A single node settles on screen, clear of the repeater.
  {
    GraphLayout g((float)HomePage::kAreaW, (float)HomePage::kAreaH);
    const int s = g.add(0x12345678u, 0);
    CHECK(s > 0 && g.count() == 1);
    g.link(s, 0);
    float moved = 1e9f;
    for (int i = 0; i < 200; i++) moved = g.step();
    CHECK(moved < 0.5f && inside(g, s, (float)HomePage::kAreaW, (float)HomePage::kAreaH));
    const float dx = g.x(s) - g.x(0), dy = g.y(s) - g.y(0);
    CHECK(std::sqrt(dx * dx + dy * dy) > 15.0f);
  }
  // Full table, duplicates, removal.
  GraphLayout g(340, 270);
  for (int i = 0; i < GraphLayout::kMaxNodes - 1; i++) CHECK(g.add(100u + (uint32_t)i, 1) > 0);
  CHECK(g.add(9999u, 1) == -1);
  CHECK(g.add(100u, 2) == g.find(100u));
  CHECK(g.link(g.find(100u), 0) && g.link(0, g.find(100u)) && g.linkCount() == 1);
  g.remove(100u);
  CHECK(g.find(100u) == -1 && g.linkCount() == 0 && g.count() == GraphLayout::kMaxNodes - 2);
  CHECK(g.add(9999u, 1) > 0);
  if (!failures) std::printf("PASS graph_layout_converges_and_stays_on_screen\n");
}

static void graph_layout_stable_when_node_added() {
  for (int size = 0; size < 2; size++)
  for (uint32_t seed = 1; seed <= 8; seed++) {
    const float W = kSizes[size][0], H = kSizes[size][1];
    GraphLayout g(W, H);
    build_random(g, 20, seed * 104729u);
    for (int i = 0; i < 200; i++) g.step();
    float x[GraphLayout::kMaxNodes], y[GraphLayout::kMaxNodes];
    for (int s = 0; s < GraphLayout::kMaxNodes; s++) { x[s] = g.x(s); y[s] = g.y(s); }
    const int anchor = 1 + (int)(rnd() % 20u);
    const int n = g.add(0xABCDEF01u + seed, 2, anchor);
    CHECK(n > 0);
    g.link(n, anchor);
    float moved = 0;
    for (int i = 0; i < 200; i++) moved = g.step();
    CHECK(moved < 0.5f);
    CHECK(inside(g, n, W, H));
    float worst = 0;
    for (int s = 0; s < GraphLayout::kMaxNodes; s++) {
      if (!g.used(s) || s == n) continue;
      const float dx = g.x(s) - x[s], dy = g.y(s) - y[s];
      worst = std::fmax(worst, std::sqrt(dx * dx + dy * dy));
    }
    CHECK(worst < 20.0f);
  }
  if (!failures) std::printf("PASS graph_layout_stable_when_node_added\n");
}

// --------------------------------------------------------------- geo layout

static const int32_t kLat0 = 47606200, kLon0 = -122332100;   // Seattle

static void geo_projection_fits_all_nodes() {
  rng = 4242;
  std::vector<GeoPoint> pts;
  for (int i = 0; i < 25; i++) {
    GeoPoint p;
    p.lat_e6 = kLat0 + (int32_t)(rnd() % 600000u) - 300000;
    p.lon_e6 = kLon0 + (int32_t)(rnd() % 800000u) - 400000;
    pts.push_back(p);
  }
  GeoPoint far; far.lat_e6 = kLat0 + 9000000; far.lon_e6 = kLon0;   // ~1000 km: a bad fix
  pts.push_back(far);
  GeoLayout geo(340, 270, 14);
  geo.fit(kLat0, kLon0, pts.data(), pts.size());
  int x, y;
  CHECK(geo.project(kLat0, kLon0, x, y) && x == 170 && y == 135);
  double use = 0;
  for (size_t i = 0; i + 1 < pts.size(); i++) {
    CHECK(geo.project(pts[i].lat_e6, pts[i].lon_e6, x, y));
    CHECK(x >= 14 && x <= 340 - 14 && y >= 14 && y <= 270 - 14);
    use = std::fmax(use, std::fmax(std::fabs(x - 170) / (170.0 - 14), std::fabs(y - 135) / (135.0 - 14)));
  }
  CHECK(use > 0.97);   // zoomed to fit: the farthest node reaches the margin
  CHECK(!geo.project(far.lat_e6, far.lon_e6, x, y));
  // North is up, east is right.
  GeoPoint ne[1] = {{kLat0 + 100000, kLon0 + 100000}};
  CHECK(geo.project(ne[0].lat_e6, ne[0].lon_e6, x, y) && x > 170 && y < 135);
  // Distances are right to ~1 % (11.1 km north).
  CHECK(std::fabs(geo.distanceKm(kLat0 + 100000, kLon0) - 11.12) < 0.1);
  CHECK(GeoLayout::niceKm(7.3) == 5.0 && GeoLayout::niceKm(3.1) == 2.0 && GeoLayout::niceKm(0.13) == 0.1);
  if (!failures) std::printf("PASS geo_projection_fits_all_nodes\n");
}

static void geo_projection_single_node() {
  {
    // The page's real area: one node lands on its fitted edge.
    GeoLayout real(HomePage::kAreaW, HomePage::kAreaH, 14);
    GeoPoint one[1] = {{kLat0 - 30000, kLon0 - 10000}};
    real.fit(kLat0, kLon0, one, 1);
    int rx, ry;
    CHECK(real.project(one[0].lat_e6, one[0].lon_e6, rx, ry));
    CHECK(rx < real.cx() && ry > real.cy() && ry == HomePage::kAreaH - 14 - (HomePage::kAreaH % 2 ? 1 : 0));
  }
  GeoLayout geo(340, 270, 14);
  int x, y;
  // Repeater only: default zoom, centre at the centre.
  geo.fit(kLat0, kLon0, nullptr, 0);
  CHECK(geo.pxPerKm() == GeoLayout::kDefaultPxPerKm);
  CHECK(geo.project(kLat0, kLon0, x, y) && x == 170 && y == 135);
  // One node north-east: on the fitted edge, in the right quadrant.
  GeoPoint p[1] = {{kLat0 + 20000, kLon0 + 50000}};
  geo.fit(kLat0, kLon0, p, 1);
  CHECK(geo.project(p[0].lat_e6, p[0].lon_e6, x, y));
  CHECK(x > 170 && y < 135);
  CHECK(x >= 14 && x <= 326 && y >= 14 && y <= 256);
  CHECK(x == 326 || y == 14);
  // One node exactly at the repeater: no zoom to infinity.
  GeoPoint same[1] = {{kLat0, kLon0}};
  geo.fit(kLat0, kLon0, same, 1);
  CHECK(geo.pxPerKm() == GeoLayout::kDefaultPxPerKm);
  CHECK(geo.project(kLat0, kLon0, x, y) && x == 170 && y == 135);
  // Antimeridian: a node just across it is a short hop east.
  GeoPoint across[1] = {{0, -179990000}};
  geo.fit(0, 179990000, across, 1);
  CHECK(geo.project(across[0].lat_e6, across[0].lon_e6, x, y) && x > 170 && y == 135);
  if (!failures) std::printf("PASS geo_projection_single_node\n");
}

// --------------------------------------------------------------- HOME page

typedef std::vector<uint8_t> Bytes;

static Bytes packet(uint8_t type, uint8_t route, uint8_t path_len, const Bytes& path, const Bytes& payload) {
  Bytes b;
  b.push_back((uint8_t)((type << PH_TYPE_SHIFT) | route));
  b.push_back(path_len);
  b.insert(b.end(), path.begin(), path.end());
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}

static Bytes advert(uint8_t k0, uint8_t k1, uint8_t type, const char* name, double lat, double lon, bool pos) {
  Bytes p;
  for (int i = 0; i < 32; i++) p.push_back((uint8_t)(0x40 + i));
  p[0] = k0; p[1] = k1;
  const uint32_t ts = 1793000000u;
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

static Bytes payload(uint8_t salt) { Bytes p; p.push_back(0x5C); p.push_back(salt); for (int i = 0; i < 30; i++) p.push_back((uint8_t)(i * 7 + salt)); return p; }

static void obs(MeshModel& m, const Bytes& raw, uint32_t now, float snr) {
  mesh::observer::PacketRecord r; std::memset(&r, 0, sizeof(r));
  r.rx_ms = now; r.snr_q4 = (int8_t)std::lround(snr * 4); r.rssi = -90;
  mesh::observer::setPacketBytes(r, raw.data(), raw.size());
  m.onObserverPacket(r, now);
}

// 2-byte path hashes (the M5's path.hash.mode 2): path_len = (2-1)<<6 | count.
static uint8_t plen(int count) { return (uint8_t)(0x40 | count); }

static void ctxInit(PageContext& ctx, MeshModel* m) {
  std::memset(&ctx, 0, sizeof(ctx));
  ctx.model = m;
  std::snprintf(ctx.clock_hm, sizeof(ctx.clock_hm), "T+00:00");
  std::snprintf(ctx.clock_date, sizeof(ctx.clock_date), "UPTIME");
}

static void vitals(MeshModel& m, PageContext& ctx, uint32_t t, int16_t nf, uint16_t rx) {
  mesh::observer::Vitals v; std::memset(&v, 0, sizeof(v));
  v.uptime_s = 86400 * 3; v.noise_floor = nf; v.rx_per_min = rx; v.tx_per_min = 11; v.airtime_permille = 31;
  v.espnow_nodes = 3; v.batt_mv = 4100; v.has_pos = true; v.lat_e6 = kLat0; v.lon_e6 = kLon0;
  v.freq = 910.525f; v.bw = 62.5f; v.sf = 7; v.cr = 5; v.preset_slot = 1;
  m.onVitals(v, t);
  ctx.obs_last_ms = t; ctx.obs_seen = true;
  ctx.data_gen++;
}

// The mockup's mesh around the M5 in Seattle.
static void populate(MeshModel& m, PageContext& ctx, uint32_t t) {
  const Bytes none;
  // The M5's own advert over the bridge (0 hops): names the header.
  const Bytes m5 = packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), none,
                          advert(0xB5, 0x05, ADV_TYPE_REPEATER, "CaptCallahan-Base", 0, 0, false));
  m.onBridgedPacket(m5.data(), m5.size(), t);
  ctx.bridge_last_ms = t; ctx.bridge_seen = true;
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), none, advert(0x57, 0x01, ADV_TYPE_REPEATER, "Woods 5C", 47.6400, -122.3900, true)), t += 400, 2.0f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), none, advert(0xCA, 0x75, ADV_TYPE_REPEATER, "CATHOUSE", 47.6250, -122.2800, true)), t += 400, 1.5f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), none, advert(0x7E, 0xBE, ADV_TYPE_REPEATER, "Trebek", 47.5600, -122.3000, true)), t += 400, -9.0f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), none, advert(0x1F, 0x01, ADV_TYPE_REPEATER, "Jupiter", 47.6550, -122.4200, true)), t += 400, 6.5f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), none, advert(0x2E, 0x11, ADV_TYPE_REPEATER, "Relay-1", 0, 0, false)), t += 400, -4.0f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(1), hashes({0x5701}), advert(0x5A, 0x33, ADV_TYPE_REPEATER, "Summit", 47.7000, -122.2500, true)), t += 400, 3.0f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(2), hashes({0x7EBE, 0xCA75}), advert(0x17, 0x0A, ADV_TYPE_CHAT, "CardPuter", 0, 0, false)), t += 400, 1.0f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(1), hashes({0x5701}), advert(0xC4, 0x4D, ADV_TYPE_CHAT, "Capt4McNeil", 0, 0, false)), t += 400, 2.5f);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(1), hashes({0x2E11}), advert(0x88, 0x21, ADV_TYPE_REPEATER, "Harbor", 47.5900, -122.3900, true)), t += 400, -3.5f);
  obs(m, packet(PAYLOAD_TYPE_ACK, ROUTE_TYPE_FLOOD, plen(1), hashes({0xCA75}), Bytes(4, 0x42)), t += 400, 7.7f);
  obs(m, packet(PAYLOAD_TYPE_TRACE, ROUTE_TYPE_DIRECT, plen(0), none, payload(3)), t += 400, 4.0f);
  obs(m, packet(PAYLOAD_TYPE_TXT_MSG, ROUTE_TYPE_FLOOD, plen(2), hashes({0x5701, 0x1F01}), payload(4)), t += 400, 6.0f);
  const Bytes bridged = packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, plen(1), hashes({0x2E11}), payload(5));
  m.onBridgedPacket(bridged.data(), bridged.size(), t += 400);
  obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, plen(2), hashes({0x7EBE, 0xCA75}), payload(6)), t += 400, -2.5f);
  vitals(m, ctx, t += 100, -116, 42);
  ctx.data_gen += 20;
}

struct Screen {
  std::vector<uint16_t> px, bgpx;
  FrameBuffer565 fb, bg;
  const PageContext* ctx;
  explicit Screen(const PageContext* c) : px(480 * 320), bgpx(480 * 320), fb(px.data()), bg(bgpx.data()), ctx(c) {}
  void show(Page& p, uint32_t now, int index = 0) {
    drawPageChrome(bg, p, index, PageSet::kCount);
    fb.copyFrom(bgpx.data());
    p.onShow(now);
  }
  void frame(Page& p, const MeshModel& m, uint32_t now) {
    (void)m;
    const_cast<PageContext*>(ctx)->now_ms = now;
    fb.eraseTouched(bgpx.data());
    p.draw(fb, now);
    drawHeader(fb, *ctx, now);
  }
  int count(int x0, int y0, int x1, int y1, uint16_t c) const {
    int n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) n += px[(size_t)y * 480 + x] == c;
    return n;
  }
  // Pixels within ~22 % of c per channel (the solid part of AA text).
  int near(int x0, int y0, int x1, int y1, uint16_t c) const {
    int n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
      const uint16_t p = px[(size_t)y * 480 + x];
      const int dr = ((p >> 11) & 31) - ((c >> 11) & 31), dg = ((p >> 5) & 63) - ((c >> 5) & 63), db = (p & 31) - (c & 31);
      n += std::abs(dr) <= 7 && std::abs(dg) <= 14 && std::abs(db) <= 7;
    }
    return n;
  }
  int differ(int y0, int y1) const {
    int n = 0;
    for (size_t i = (size_t)y0 * 480; i < (size_t)y1 * 480; i++) n += px[i] != bgpx[i];
    return n;
  }
  int differRect(int x0, int y0, int x1, int y1) const {
    int n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) n += px[(size_t)y * 480 + x] != bgpx[(size_t)y * 480 + x];
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

static void link_quality_colours() {
  CHECK(linkColour(20) == theme::kSigStrong && linkColour(127) == theme::kSigStrong);
  CHECK(linkColour(19) == theme::kSigGood && linkColour(0) == theme::kSigGood);
  CHECK(linkColour(-1) == theme::kSigWeak && linkColour(-28) == theme::kSigWeak);
  CHECK(linkColour(-29) == theme::kSigPoor && linkColour(-128) == theme::kSigPoor);
  // Four distinct signal colours, none of them the stale grey.
  const uint16_t sig[4] = {theme::kSigStrong, theme::kSigGood, theme::kSigWeak, theme::kSigPoor};
  for (int i = 0; i < 4; i++) {
    CHECK(sig[i] != theme::kStale && sig[i] != theme::kBg && sig[i] != theme::kPanelFill);
    for (int j = i + 1; j < 4; j++) CHECK(sig[i] != sig[j]);
  }
  if (!failures) std::printf("PASS link_quality_colours\n");
}

static void model_path_and_feed_sequence() {
  MeshModel m(16, 8);
  CHECK(m.pathSeq() == 0 && m.feedSeq() == 0 && m.lastPathItem() == nullptr);
  const Bytes none;
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), none, advert(0x57, 0x01, ADV_TYPE_REPEATER, "Woods", 0, 0, false)), 100, 2);
  CHECK(m.pathSeq() == 1 && m.feedSeq() == 1 && m.lastPathItem() == m.feed(0));
  obs(m, packet(PAYLOAD_TYPE_TXT_MSG, ROUTE_TYPE_DIRECT, plen(0), none, payload(1)), 200, 2);
  CHECK(m.pathSeq() == 1 && m.feedSeq() == 2 && m.lastPathItem() == m.feed(1));
  obs(m, packet(PAYLOAD_TYPE_TXT_MSG, ROUTE_TYPE_DIRECT, plen(0), none, payload(1)), 300, 2);
  CHECK(m.pathSeq() == 1 && m.feedSeq() == 2);
  const Bytes f = packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, plen(1), hashes({0x5701}), payload(2));
  m.onBridgedPacket(f.data(), f.size(), 400);
  CHECK(m.pathSeq() == 2 && m.feedSeq() == 3 && m.lastPathItem() == m.feed(0) && !m.feed(0)->observed);
  for (uint8_t i = 10; i < 18; i++) obs(m, packet(PAYLOAD_TYPE_TXT_MSG, ROUTE_TYPE_DIRECT, plen(0), none, payload(i)), 500 + i, 1);
  CHECK(m.pathSeq() == 2 && m.lastPathItem() == nullptr);
  if (!failures) std::printf("PASS model_path_and_feed_sequence\n");
}

static const int kArea[4] = {HomePage::kAreaX, HomePage::kAreaY, HomePage::kAreaX + HomePage::kAreaW,
                             HomePage::kAreaY + HomePage::kAreaH};

static void home_page_renders_and_animates() {
  MeshModel model(64, 64);
  PageContext ctx; ctxInit(ctx, &model);
  PageSet set;
  set.attach(&ctx);
  HomePage* home = static_cast<HomePage*>(set.page(PageSet::kHome));
  CHECK(std::strcmp(home->title(), "HOME") == 0);
  Screen s(&ctx);
  // Empty model: draws, idles, never touches the tab bar.
  s.show(*home, 1000);
  CHECK(home->wantsFrame(1000));
  s.frame(*home, model, 1000);
  CHECK(s.differ(0, 296) > 50);
  CHECK(s.differ(296, 320) == 0);
  CHECK(s.near(250, 0, 370, 34, theme::kWarn) > 15);   // "NO REPEATER DATA"
  CHECK(!home->wantsFrame(1100));
  CHECK(home->wantsFrame(2000));   // once a second
  // The mesh arrives: nodes glide in, then everything settles.
  populate(model, ctx, 2000);
  CHECK(home->wantsFrame(9000));
  uint32_t t = 9000;
  int frames = 0;
  while (home->wantsFrame(t) && frames < 600) { s.frame(*home, model, t); t += 16; frames++; }
  CHECK(frames < 600);   // motion ends
  CHECK(!home->animating(t));
  CHECK(home->graph().count() == 9);
  CHECK(s.differ(296, 320) == 0);
  // Contacts in their link colours (CATHOUSE -2.5 amber, Trebek -9 violet).
  CHECK(s.count(kArea[0], kArea[1], kArea[2], kArea[3], theme::kSigWeak) > 4);
  CHECK(s.count(kArea[0], kArea[1], kArea[2], kArea[3], theme::kSigPoor) > 4);
  CHECK(s.count(kArea[0], kArea[1], kArea[2], kArea[3], theme::kBlip) == 0);   // no packet in flight
  const int restRoute = s.count(kArea[0], kArea[1], kArea[2], kArea[3], theme::kRoute);
  // Header: repeater name, frequency, MHz, clock.
  CHECK(s.near(8, 4, 200, 17, theme::kText) > 40);      // "M5 // CAPTCALLAHAN-BASE"
  CHECK(s.near(200, 2, 301, 26, theme::kAccent) > 40);  // 910.525
  CHECK(s.near(400, 2, 475, 20, theme::kText) > 20);    // clock
  // Noise floor reading, quiet (green), needle at its target.
  CHECK(s.near(310, 72, 390, 108, theme::kText) > 40);
  CHECK(s.near(310, 112, 370, 128, theme::kGood) > 6);
  CHECK(home->needle().position() == -116.0f);
  // Odometer shows the packet count; rate in cyan.
  CHECK(home->odometer().target() == model.feedSeq());
  CHECK(s.near(HomePage::kOdoX, HomePage::kOdoY, HomePage::kOdoX + 6 * HomePage::kOdoPitch, HomePage::kOdoY + HomePage::kOdoH, theme::kText) > 60);
  CHECK(s.near(310, 198, 340, 222, theme::kAccent) > 20);
  // Quick tiles: OBSERVER and BRIDGE on (green), PRESET slot 1 (lit).
  for (int i = hmi::kTileObserver; i <= hmi::kTileBridge; i++) {
    const int x = HomePage::kTileX + i * HomePage::kTilePitch;
    CHECK(s.count(x, HomePage::kTileY, x + HomePage::kTileW, HomePage::kTileY + HomePage::kTileH, theme::kTileOnFill) > 500);
    CHECK(s.near(x, HomePage::kTileY + 20, x + HomePage::kTileW, HomePage::kTileY + 34, theme::kGood) > 4);
  }

  // A new packet with a path: the route animates for ~2 s, then rests.
  const uint32_t t0 = t + 50;
  obs(model, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, plen(2), hashes({0x1F01, 0x5701}), payload(9)), t0, 6.5f);
  ctx.data_gen++;
  CHECK(home->wantsFrame(t0));
  s.frame(*home, model, t0);
  CHECK(home->animating(t0 + 100));
  s.frame(*home, model, t0 + 900);
  CHECK(s.count(kArea[0], kArea[1], kArea[2], kArea[3], theme::kBlip) > 4);    // the travelling blip
  CHECK(s.count(kArea[0], kArea[1], kArea[2], kArea[3], theme::kRoute) > restRoute + 20);   // the lit route
  CHECK(s.near(kArea[0], kArea[3] - HomePage::kCaptionH, kArea[2], kArea[3], theme::kAccent) > 10);   // caption while in flight
  CHECK(s.differ(296, 320) == 0);
  t = t0 + 900;
  frames = 0;
  while (home->wantsFrame(t) && frames < 600) { s.frame(*home, model, t); t += 16; frames++; }
  CHECK(t - t0 < HomePage::kPathAnimMs + HomePage::kPingMs);   // motion is the packet's, not ambient
  CHECK(s.count(kArea[0], kArea[1], kArea[2], kArea[3], theme::kBlip) == 0);
  CHECK(!home->wantsFrame(t + 100));
  CHECK(home->wantsFrame(t + 1000));

  // GRAPH | GEO toggle: tap the header note; taps elsewhere do nothing.
  CHECK(!home->touch(200, 150, true) && !home->touch(200, 150, false));
  CHECK(home->mode() == HomePage::kGraph);
  const int tx = HomePage::kToggleX + HomePage::kToggleW - 20, ty = HomePage::kToggleY + HomePage::kToggleH / 2;
  std::vector<uint16_t> noteGraph(s.px);
  CHECK(!home->touch(tx, ty, true));
  CHECK(home->touch(tx, ty, false));
  CHECK(home->mode() == HomePage::kGeo);
  s.frame(*home, model, t + 1100);
  {
    int changed = 0;
    for (int y = 40; y < 56; y++) for (int x = 230; x < 296; x++) changed += noteGraph[(size_t)y * 480 + x] != s.px[(size_t)y * 480 + x];
    CHECK(changed > 10);   // "Live - Geo"
  }
  CHECK(s.differ(296, 320) == 0);
  CHECK(s.count(kArea[0], kArea[1], kArea[2], kArea[3], theme::kSigPoor) > 4);
  CHECK(!home->animating(t + 1100));   // the graph settles off screen at once
  // GEO with a fix: the sweep asks for frames at <= 20 fps, and moves.
  CHECK(home->sweeping());
  CHECK(!home->wantsFrame(t + 1100 + HomePage::kSweepFrameMs - 1));
  CHECK(home->wantsFrame(t + 1100 + HomePage::kSweepFrameMs));
  {
    std::vector<uint16_t> before = s.px;
    s.frame(*home, model, t + 1100 + 1500);
    int moved = 0;
    for (size_t i = 0; i < before.size(); i++) moved += before[i] != s.px[i];
    CHECK(moved > 50);
  }
  // A press that slides off the toggle does not switch.
  CHECK(!home->touch(tx, ty, true) && !home->touch(100, 200, false));
  CHECK(home->mode() == HomePage::kGeo);
  CHECK(!home->touch(tx, ty, true) && home->touch(tx, ty, false));
  CHECK(home->mode() == HomePage::kGraph);

  // Hidden while packets arrive: shown again without replaying them.
  home->onHide();
  obs(model, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, plen(1), hashes({0xCA75}), payload(10)), t + 5000, 1.0f);
  ctx.data_gen++;
  s.show(*home, t + 6000);
  s.frame(*home, model, t + 6000);
  CHECK(!home->animating(t + 6001 + hmi::Odometer::kRollMs));
  if (!failures) std::printf("PASS home_page_renders_and_animates\n");
}

static int countShown(const HomePage& h) { return h.graph().count(); }

// The repeater (M5) and the head itself are never nodes.
static void self_nodes_filtered() {
  MeshModel model(64, 64);
  uint8_t headKey[32];
  for (int i = 0; i < 32; i++) headKey[i] = (uint8_t)(0x40 + i);
  headKey[0] = 0xEE; headKey[1] = 0x01;
  model.setHeadKey(headKey);
  PageContext ctx; ctxInit(ctx, &model);
  HomePage home; home.attach(&ctx);
  Screen s(&ctx);
  s.show(home, 1000);
  populate(model, ctx, 2000);
  const size_t before = model.nodeCount();
  uint32_t t = 9000;
  for (int i = 0; i < 600 && home.wantsFrame(t); i++) { s.frame(home, model, t); t += 16; }
  const int shownBefore = countShown(home);
  // The M5's own advert again (bridge, 0 hops) and its echoes through
  // neighbours (observer, 1 hop).
  const Bytes m5adv = advert(0xB5, 0x05, ADV_TYPE_REPEATER, "CaptCallahan-Base", 0, 0, false);
  const Bytes bridged = packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), Bytes(), m5adv);
  model.onBridgedPacket(bridged.data(), bridged.size(), t);
  obs(model, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(1), hashes({0x5701}), m5adv), t + 300, 2.0f);
  uint8_t rk[4];
  CHECK(model.repeaterKey(rk) && rk[0] == 0xB5 && rk[1] == 0x05);
  CHECK(std::strcmp(model.repeaterName(), "CaptCallahan-Base") == 0);
  // The head's own advert echoed back (observer, 2 hops) and bridged.
  const Bytes headAdv = advert(0xEE, 0x01, ADV_TYPE_CHAT, "FreenoveHead", 0, 0, false);
  obs(model, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(2), hashes({0xCA75, 0x5701}), headAdv), t + 400, 1.0f);
  const Bytes headB = packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), Bytes(), headAdv);
  model.onBridgedPacket(headB.data(), headB.size(), t + 500);
  ctx.data_gen += 4;
  CHECK(model.nodeCount() == before);
  t += 600;
  for (int i = 0; i < 600 && home.wantsFrame(t); i++) { s.frame(home, model, t); t += 16; }
  CHECK(countShown(home) == shownBefore);
  CHECK(model.feed(0) && std::strncmp(model.feed(0)->label, "ADV FreenoveHead", 16) == 0);
  for (size_t i = 0; i < model.nodeCount(); i++) {
    CHECK(!(model.node(i)->pub_prefix[0] == 0xB5 && model.node(i)->pub_prefix[1] == 0x05));
    CHECK(!(model.node(i)->pub_prefix[0] == 0xEE && model.node(i)->pub_prefix[1] == 0x01));
  }
  // A route through the M5's hash maps it to the centre, not to a node.
  obs(model, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, plen(2), hashes({0xB505, 0x5701}), payload(40)), t, 3.0f);
  ctx.data_gen++;
  s.frame(home, model, t);
  CHECK(home.pathLength() >= 2 && home.pathSlot(0) == 0);
  // GEO: nothing sits on the centre (the M5 at the vitals position, learnt
  // from its GPS even without the bridge).
  MeshModel m2(64, 64);
  PageContext c2; ctxInit(c2, &m2);
  populate(m2, c2, 2000);
  obs(m2, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(1), hashes({0x5701}),
                 advert(0xB6, 0x06, ADV_TYPE_REPEATER, "Base", 47.6062, -122.3321, true)), 9000, 1.0f);
  CHECK(m2.repeaterKey(rk) && rk[0] == 0xB6);
  CHECK(std::strcmp(m2.repeaterName(), "Base") == 0);
  for (size_t i = 0; i < m2.nodeCount(); i++) CHECK(std::strcmp(m2.node(i)->name, "Base") != 0);
  // Another bridged-only 0-hop repeater after the M5 is known: a node, and the
  // M5's key does not flap (no real node is deleted).
  const size_t n1 = model.nodeCount();
  const Bytes other = packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), Bytes(),
                             advert(0xC9, 0x09, ADV_TYPE_REPEATER, "EspRelay", 0, 0, false));
  model.onBridgedPacket(other.data(), other.size(), t + 1000);
  CHECK(model.repeaterKey(rk) && rk[0] == 0xB5 && rk[1] == 0x05);
  CHECK(std::strcmp(model.repeaterName(), "CaptCallahan-Base") == 0);
  CHECK(model.nodeCount() == n1 + 1);
  model.onBridgedPacket(bridged.data(), bridged.size(), t + 1100);   // M5 again (a repeat)
  CHECK(model.repeaterKey(rk) && rk[0] == 0xB5 && model.nodeCount() == n1 + 1);
  // A 0-hop advert the observer itself heard is a neighbour, never the M5.
  MeshModel m3(16, 16);
  const Bytes nb = packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), Bytes(), advert(0xB7, 0x07, ADV_TYPE_REPEATER, "Nb", 0, 0, false));
  obs(m3, nb, 100, 4.0f);
  CHECK(!m3.repeaterKey(rk) && m3.nodeCount() == 1 && m3.repeaterName()[0] == 0);
  if (!failures) std::printf("PASS self_nodes_filtered\n");
}

static Bytes numberedAdvert(int i, bool pos) {
  char name[16];
  std::snprintf(name, sizeof(name), "N%03d", i);
  return packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), Bytes(),
                advert((uint8_t)(0x10 + i / 200), (uint8_t)(i % 200 + 1), ADV_TYPE_REPEATER, name,
                       47.6 + (i % 13) * 0.01, -122.33 + (i % 7) * 0.012, pos));
}

// More nodes than fit: the shown set is stable; stale nodes make room; a
// slot reused for another node never inherits the old trace or ping.
static void many_nodes_stay_put(int total) {
  MeshModel model((size_t)total, 512);
  PageContext ctx; ctxInit(ctx, &model);
  HomePage home; home.attach(&ctx);
  Screen s(&ctx);
  s.show(home, 1000);
  uint32_t t = 2000;
  for (int i = 0; i < total; i++) obs(model, numberedAdvert(i, true), t += 50, (float)(i % 20) - 9.0f);
  ctx.data_gen++;
  for (int i = 0; i < 600 && home.wantsFrame(t); i++) { s.frame(home, model, t); t += 16; }
  CHECK(countShown(home) == HomePage::kMaxShown);
  uint32_t keys[GraphLayout::kMaxNodes];
  for (int k = 0; k < GraphLayout::kMaxNodes; k++) keys[k] = home.graph().key(k);
  int pathNode = -1;
  for (int k = 1; k < GraphLayout::kMaxNodes && pathNode < 0; k++) if (home.graph().used(k)) pathNode = k;
  const uint32_t pk = home.graph().key(pathNode);
  obs(model, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, plen(1), hashes({(uint16_t)(pk >> 16)}), payload(77)), t, 2.0f);
  ctx.data_gen++;
  s.frame(home, model, t);
  CHECK(home.pathSlot(0) == pathNode);
  for (int i = 0; i < 40; i++) {
    t += 7000;
    obs(model, numberedAdvert(i, true), t, 1.0f);   // waiting nodes heard again
    ctx.data_gen++;
    s.frame(home, model, t);
    CHECK(!home.graph().hot());
  }
  for (int k = 0; k < GraphLayout::kMaxNodes; k++) CHECK(home.graph().key(k) == keys[k]);
  int plen0 = home.pathLength();
  uint32_t pkeys[16];
  for (int i = 0; i < plen0 && i < 16; i++) pkeys[i] = home.pathSlot(i) > 0 ? home.graph().key(home.pathSlot(i)) : 0;
  t += HomePage::kKeepMs;
  model.tick(t);
  ctx.data_gen++;
  s.frame(home, model, t + 20);
  int changed = 0;
  for (int k = 1; k < GraphLayout::kMaxNodes; k++) changed += home.graph().key(k) != keys[k];
  CHECK(changed > 0 && countShown(home) == HomePage::kMaxShown);
  CHECK(home.pathLength() == plen0);
  for (int i = 0; i < home.pathLength() && i < 16; i++) {
    const int sl = home.pathSlot(i);
    CHECK(sl <= 0 || home.graph().key(sl) == pkeys[i]);
  }
  CHECK(s.differ(296, 320) == 0);
  home.onHide();
  HomePage fresh; fresh.attach(&ctx);
  Screen s2(&ctx);
  s2.show(fresh, t + 100);
  const auto t0 = std::chrono::steady_clock::now();
  s2.frame(fresh, model, t + 100);
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
  std::printf("TIMING first_sync nodes=%d feed=%u us=%lld\n", total, (unsigned)model.feedCount(), (long long)us);
  if (!failures) std::printf("PASS many_nodes_%d\n", total);
}

// Unknown-last-hop packets ping the M5 at most once per kCentrePingGapMs.
static void centre_ping_rate_limited() {
  MeshModel model(16, 64);
  PageContext ctx; ctxInit(ctx, &model);
  HomePage home; home.attach(&ctx);
  Screen s(&ctx);
  s.show(home, 1000);
  s.frame(home, model, 1000);
  uint32_t t = 2000;
  int animated = 0, total = 0;
  for (int i = 0; i < 60; i++) {   // a direct-routed packet every 400 ms for 24 s
    obs(model, packet(PAYLOAD_TYPE_TXT_MSG, ROUTE_TYPE_DIRECT, plen(0), Bytes(), payload((uint8_t)i)), t, 1.0f);
    ctx.data_gen++;
    for (int f = 0; f < 25; f++, t += 16) {
      if (home.wantsFrame(t)) s.frame(home, model, t);
      animated += home.pinging(t) ? 1 : 0;
      total++;
    }
  }
  const double share = (double)animated / total;
  std::printf("INFO centre ping animating share=%.2f\n", share);
  // 1.2 s ring per 2 s (+ the odometer's roll per packet), never continuous.
  CHECK(share > 0.4 && share < 0.95);
  if (!failures) std::printf("PASS centre_ping_rate_limited\n");
}

// GEO without a repeater fix: centred on the nodes, no M5, no rings.
static void geo_without_repeater_fix() {
  MeshModel model(64, 64);
  PageContext ctx; ctxInit(ctx, &model);
  HomePage home; home.attach(&ctx);
  Screen s(&ctx);
  s.show(home, 1000);
  obs(model, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), Bytes(), advert(0x51, 0x01, ADV_TYPE_REPEATER, "Alpha", 47.64, -122.39, true)), 2000, 2.0f);
  obs(model, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), Bytes(), advert(0x52, 0x02, ADV_TYPE_REPEATER, "Bravo", 47.60, -122.28, true)), 2100, 3.0f);
  obs(model, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), Bytes(), advert(0x53, 0x03, ADV_TYPE_REPEATER, "Charlie", 47.56, -122.33, true)), 2200, 1.0f);
  mesh::observer::Vitals v; std::memset(&v, 0, sizeof(v));
  v.freq = 910.525f; v.bw = 62.5f; v.sf = 7; v.rx_per_min = 3; v.noise_floor = -110;
  model.onVitals(v, 8000);
  ctx.data_gen++;
  home.setMode(HomePage::kGeo);
  s.frame(home, model, 9000);
  s.frame(home, model, 9016);
  CHECK(s.count(kArea[0], kArea[1], kArea[2], kArea[3], theme::kRepeater) == 0);   // no M5 ring or label
  CHECK(s.count(kArea[0], kArea[1], kArea[2], kArea[3], theme::kGeoRing) == 0);    // no range rings
  CHECK(s.count(kArea[0], kArea[1], kArea[2], kArea[3], theme::kTextDim) == 0);    // no compass ticks or km labels
  CHECK(s.count(kArea[0], kArea[1], kArea[2], kArea[3], theme::kSigGood) > 4);     // contacts still placed
  CHECK(s.near(kArea[0], kArea[1], kArea[0] + 120, kArea[1] + 14, kDimText) > 10); // "No repeater GPS fix"
  CHECK(!home.sweeping() && !home.wantsFrame(9016 + 100));                         // no sweep without a centre
  CHECK(s.differ(296, 320) == 0);
  if (!failures) std::printf("PASS geo_without_repeater_fix\n");
}

// Links nobody has heard for an hour stop being drawn.
static void links_expire() {
  MeshModel model(64, 64);
  PageContext ctx; ctxInit(ctx, &model);
  HomePage home; home.attach(&ctx);
  Screen s(&ctx);
  s.show(home, 1000);
  populate(model, ctx, 2000);
  s.frame(home, model, 9000);
  CHECK(home.graph().linkCount() > 3);
  s.frame(home, model, 9000 + HomePage::kLinkTtlMs + 60000);
  CHECK(home.graph().linkCount() == 0);
  if (!failures) std::printf("PASS links_expire\n");
}

// A node the M5 hears directly links to the centre even without a route
// that says so; the link goes after kLinkTtlMs without hearing it.
static void direct_neighbour_links_centre() {
  MeshModel model(64, 64);
  PageContext ctx; ctxInit(ctx, &model);
  HomePage home; home.attach(&ctx);
  Screen s(&ctx);
  s.show(home, 1000);
  s.frame(home, model, 1000);
  obs(model, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), Bytes(), advert(0x57, 0x01, ADV_TYPE_REPEATER, "Woods", 0, 0, false)), 1500, 2.0f);
  obs(model, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(2), hashes({0x6101, 0x5701}), advert(0x61, 0x01, ADV_TYPE_REPEATER, "Far", 0, 0, false)), 1600, 1.0f);
  obs(model, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(0), Bytes(), advert(0x61, 0x01, ADV_TYPE_REPEATER, "Far", 0, 0, false)), 1700, 4.0f);
  ctx.data_gen += 3;
  s.frame(home, model, 2000);
  const int woods = home.graph().find(0x57014243u);
  const int far = home.graph().find(0x61014243u);
  CHECK(woods > 0 && far > 0);
  CHECK(home.graph().linked(woods, 0));
  CHECK(home.graph().linked(far, 0));
  s.frame(home, model, 1700 + HomePage::kLinkTtlMs + 1000);
  CHECK(!home.graph().linked(woods, 0) && !home.graph().linked(far, 0));
  if (!failures) std::printf("PASS direct_neighbour_links_centre\n");
}

// Every placed label: clear of its own ring by >= 3 px, of every other
// contact's ring by >= kForeignGap - 1, and inside the topology area.
static int checkLabels(const HomePage& h) {
  int placed = 0;
  for (int s = 1; s < GraphLayout::kMaxNodes; s++) {
    int x, y, w, hh;
    if (!h.labelRect(s, x, y, w, hh)) continue;
    placed++;
    CHECK(x >= HomePage::kAreaX && y >= HomePage::kAreaY && x + w <= HomePage::kAreaX + HomePage::kAreaW &&
          y + hh <= HomePage::kAreaY + HomePage::kAreaH);
    for (int o = 1; o < GraphLayout::kMaxNodes; o++) {
      int ox, oy;
      if (!h.graph().used(o) || !h.contactPos(o, ox, oy)) continue;
      const int r = HomePage::kNodeHalf + (o == s ? 3 : HomePage::kForeignGap - 1);
      const bool hit = x < ox + r + 1 && x + w > ox - r && y < oy + r + 1 && y + hh > oy - r;
      if (hit) std::printf("label of slot %d at %d,%d %dx%d hits ring %d at %d,%d\n", s, x, y, w, hh, o, ox, oy);
      CHECK(!hit);
    }
  }
  return placed;
}

static void label_names_and_placement() {
  char out[24];
  labelName(out, sizeof(out), "CardPuter", 8);
  CHECK(std::strcmp(out, "CardPut~") == 0);
  labelName(out, sizeof(out), "Woods 5C", 8);
  CHECK(std::strcmp(out, "Woods 5C") == 0);
  labelName(out, sizeof(out), "Jupiter", 12);
  CHECK(std::strcmp(out, "Jupiter") == 0);
  labelName(out, sizeof(out), "abcdefghijklmnop", 12);
  CHECK(std::strcmp(out, "abcdefghijk~") == 0);
  labelName(out, 4, "abcdefgh", 8);   // tiny buffer: still terminated
  CHECK(std::strlen(out) <= 3);
  MeshModel model(64, 64);
  PageContext ctx; ctxInit(ctx, &model);
  HomePage home; home.attach(&ctx);
  Screen s(&ctx);
  s.show(home, 1000);
  populate(model, ctx, 2000);
  uint32_t t = 9000;
  for (int i = 0; i < 600 && home.wantsFrame(t); i++) { s.frame(home, model, t); t += 16; }
  const int graphPlaced = checkLabels(home);
  CHECK(graphPlaced == home.graph().count());   // 9 contacts, all labelled
  home.setMode(HomePage::kGeo);
  s.frame(home, model, t + 100);
  const int geoPlaced = checkLabels(home);
  CHECK(geoPlaced >= 5);                         // whole names only (14 px): 5 of the 6 with GPS
  home.setMode(HomePage::kGraph);
  s.frame(home, model, t + 200);
  for (int i = 0; i < 31; i++) {
    const Bytes path = hashes({(uint16_t)(i % 3 == 0 ? 0x5701 : (i % 3 == 1 ? 0xCA75 : 0x2E11))});
    obs(model, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(1), path,
                      advert((uint8_t)(0x30 + i), 0x44, ADV_TYPE_CHAT, (std::string("Node-") + std::to_string(i)).c_str(),
                             47.55 + (i % 9) * 0.02, -122.45 + (i % 11) * 0.025, i % 2 == 0)),
        t + 300 + (uint32_t)i, 1.0f);
  }
  ctx.data_gen++;
  t += 1000;
  for (int i = 0; i < 600 && home.wantsFrame(t); i++) { s.frame(home, model, t); t += 16; }
  const int busyGraph = checkLabels(home);
  home.setMode(HomePage::kGeo);
  s.frame(home, model, t + 100);
  const int busyGeo = checkLabels(home);
  std::printf("INFO labels placed: graph %d/%d, geo %d, busy graph %d/%d, busy geo %d\n", graphPlaced,
              9, geoPlaced, busyGraph, home.graph().count(), busyGeo);
  CHECK(busyGraph >= 12);   // whole 14 px names: fewer fit than 7b's cut 8x16 names
  if (!failures) std::printf("PASS label_names_and_placement\n");
}

// Caption packet types have their own colours: never a contact (signal),
// route or blip colour; ADV/ACK amber stays.
static void feed_colours_distinct() {
  const uint16_t forbidden[] = {theme::kBlip, theme::kRoute, theme::kRouteRest, theme::kSigStrong, theme::kSigGood,
                                theme::kSigPoor, theme::kStale};
  const uint8_t types[] = {PAYLOAD_TYPE_GRP_TXT, PAYLOAD_TYPE_GRP_DATA, PAYLOAD_TYPE_TXT_MSG, PAYLOAD_TYPE_ADVERT,
                           PAYLOAD_TYPE_ACK, PAYLOAD_TYPE_TRACE, PAYLOAD_TYPE_REQ};
  for (uint8_t t : types)
    for (uint16_t f : forbidden) CHECK(feedTypeColour(t) != f);
  CHECK(feedTypeColour(PAYLOAD_TYPE_GRP_TXT) != theme::kSigWeak && feedTypeColour(PAYLOAD_TYPE_TXT_MSG) != theme::kSigWeak);
  CHECK(feedTypeColour(PAYLOAD_TYPE_GRP_TXT) != feedTypeColour(PAYLOAD_TYPE_TXT_MSG));
  CHECK(feedTypeColour(PAYLOAD_TYPE_ADVERT) == theme::kFeedAdv && feedTypeColour(PAYLOAD_TYPE_ACK) == theme::kFeedAdv);
  CHECK(theme::kFeedEsp == theme::kEsp && theme::kBlip == 0xFFFF);
  if (!failures) std::printf("PASS feed_colours_distinct\n");
}

// The needle moves only to new vitals: packets never swing it.
static void needle_only_on_vitals() {
  MeshModel model(64, 64);
  PageContext ctx; ctxInit(ctx, &model);
  HomePage home; home.attach(&ctx);
  Screen s(&ctx);
  s.show(home, 1000);
  populate(model, ctx, 2000);
  uint32_t t = 9000;
  for (int i = 0; i < 600 && home.wantsFrame(t); i++) { s.frame(home, model, t); t += 16; }
  CHECK(home.needle().position() == -116.0f && !home.needle().settling());
  // A burst of packets (strong and weak): the needle stays put.
  std::vector<uint16_t> gauge;
  for (int i = 0; i < 20; i++) {
    obs(model, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, plen(1), hashes({0x5701}), payload((uint8_t)(50 + i))), t, (float)(i % 2 ? 12 : -15));
    ctx.data_gen++;
    s.frame(home, model, t);
    CHECK(home.needle().position() == -116.0f && home.needle().target() == -116.0f);
    t += 100;
  }
  // New vitals: it eases there (no jump), then rests.
  vitals(model, ctx, t, -100, 50);
  s.frame(home, model, t);
  s.frame(home, model, t + 16);
  const float p1 = home.needle().position();
  CHECK(p1 > -116.0f && p1 < -110.0f && home.needle().settling() && home.wantsFrame(t + 32));
  int frames = 0;
  t += 32;
  while (home.wantsFrame(t) && frames < 600) { s.frame(home, model, t); t += 16; frames++; }
  CHECK(home.needle().position() == -100.0f && frames < 300);
  CHECK(s.near(310, 112, 380, 128, theme::kWarn) > 6);   // "moderate"
  if (!failures) std::printf("PASS needle_only_on_vitals\n");
}

// The packet odometer rolls when the count changes, then rests.
static void odometer_rolls_on_page() {
  MeshModel model(64, 64);
  PageContext ctx; ctxInit(ctx, &model);
  HomePage home; home.attach(&ctx);
  Screen s(&ctx);
  s.show(home, 1000);
  populate(model, ctx, 2000);
  uint32_t t = 9000;
  for (int i = 0; i < 600 && home.wantsFrame(t); i++) { s.frame(home, model, t); t += 16; }
  const uint32_t before = model.feedSeq();
  obs(model, packet(PAYLOAD_TYPE_TXT_MSG, ROUTE_TYPE_DIRECT, plen(0), Bytes(), payload(99)), t, 1.0f);
  ctx.data_gen++;
  s.frame(home, model, t);
  CHECK(model.feedSeq() == before + 1 && home.odometer().target() == before + 1);
  CHECK(home.odometer().rolling(t + 100) && home.wantsFrame(t + 100));
  std::vector<uint16_t> a = s.px;
  s.frame(home, model, t + hmi::Odometer::kRollMs / 2);
  const int x0 = HomePage::kOdoX + 5 * HomePage::kOdoPitch;
  int moved = 0;
  for (int y = HomePage::kOdoY; y < HomePage::kOdoY + HomePage::kOdoH; y++)
    for (int x = x0; x < x0 + HomePage::kOdoW; x++) moved += a[(size_t)y * 480 + x] != s.px[(size_t)y * 480 + x];
  CHECK(moved > 10);   // the units digit is mid-roll
  s.frame(home, model, t + hmi::Odometer::kRollMs + 20);
  CHECK(!home.odometer().rolling(t + hmi::Odometer::kRollMs + 20));
  if (!failures) std::printf("PASS odometer_rolls_on_page\n");
}

static int tileCentreX(int i) { return HomePage::kTileX + i * HomePage::kTilePitch + HomePage::kTileW / 2; }
static const int kTileCY = HomePage::kTileY + HomePage::kTileH / 2;
static bool tap(HomePage& h, int x, int y) { const bool a = h.touch(x, y, true); const bool b = h.touch(x, y, false); return a || b; }

// Quick controls: ADVERT needs a confirming second tap; nothing is sent on
// the first tap, on a timeout or after a cancel. Navigation tiles act at once.
static void quick_controls_confirm() {
  MeshModel model(64, 64);
  PageContext ctx; ctxInit(ctx, &model);
  HomePage home; home.attach(&ctx);
  Screen s(&ctx);
  s.show(home, 1000);
  populate(model, ctx, 2000);
  uint32_t t = 9000;
  for (int i = 0; i < 600 && home.wantsFrame(t); i++) { s.frame(home, model, t); t += 16; }
  CHECK(HomePage::tileAt(tileCentreX(0), kTileCY) == hmi::kTileAdvert);
  CHECK(HomePage::tileAt(tileCentreX(5), kTileCY) == hmi::kTileAdmin);
  CHECK(HomePage::tileAt(478, kTileCY) == -1 && HomePage::tileAt(tileCentreX(2), 200) == -1);
  const int ax = HomePage::kTileX;
  // First tap: CONFIRM? (amber edge), no request.
  ctx.now_ms = t;
  CHECK(tap(home, tileCentreX(0), kTileCY));
  CHECK(home.takeRequest() == kReqNone);
  CHECK(home.wantsFrame(t));
  s.frame(home, model, t);
  CHECK(s.count(ax, HomePage::kTileY, ax + HomePage::kTileW, HomePage::kTileY + HomePage::kTileH, theme::kWarn) > 40);
  // Timeout: the tile reverts by itself (a frame is asked for), no request.
  t += hmi::QuickControls::kConfirmMs;
  CHECK(home.wantsFrame(t));
  s.frame(home, model, t);
  CHECK(s.count(ax, HomePage::kTileY, ax + HomePage::kTileW, HomePage::kTileY + HomePage::kTileH, theme::kWarn) == 0);
  CHECK(home.takeRequest() == kReqNone);
  // Tap, tap: sent once; the outcome shows on the tile.
  ctx.now_ms = t + 100;
  tap(home, tileCentreX(0), kTileCY);
  CHECK(home.takeRequest() == kReqNone);
  ctx.now_ms = t + 900;
  tap(home, tileCentreX(0), kTileCY);
  CHECK(home.takeRequest() == kReqAdvert);
  CHECK(home.takeRequest() == kReqNone);
  home.requestDone(kReqAdvert, true, t + 950);
  s.frame(home, model, t + 950);
  CHECK(s.near(ax, HomePage::kTileY + 20, ax + HomePage::kTileW, HomePage::kTileY + 34, theme::kGood) > 4);   // SENT
  // Cancel: a tap elsewhere, then the second tap only arms again.
  ctx.now_ms = t + 4000;
  tap(home, tileCentreX(0), kTileCY);
  ctx.now_ms = t + 4100;
  tap(home, 150, 150);
  ctx.now_ms = t + 4200;
  tap(home, tileCentreX(0), kTileCY);
  CHECK(home.takeRequest() == kReqNone);
  // A press that slides to another tile does nothing.
  ctx.now_ms = t + 9000;
  CHECK(!home.touch(tileCentreX(1), kTileCY, true));
  home.touch(tileCentreX(2), kTileCY, false);
  CHECK(home.takeRequest() == kReqNone);
  // Navigation tiles: at once.
  tap(home, tileCentreX(hmi::kTilePreset), kTileCY);
  CHECK(home.takeRequest() == kReqOpenPresets);
  tap(home, tileCentreX(hmi::kTileObserver), kTileCY);
  CHECK(home.takeRequest() == kReqOpenSettings);
  tap(home, tileCentreX(hmi::kTileBridge), kTileCY);
  CHECK(home.takeRequest() == kReqOpenSettings);
  tap(home, tileCentreX(hmi::kTileTrace), kTileCY);
  CHECK(home.takeRequest() == kReqOpenHeadAdmin);
  tap(home, tileCentreX(hmi::kTileAdmin), kTileCY);
  CHECK(home.takeRequest() == kReqAdminLogin);
  if (!failures) std::printf("PASS quick_controls_confirm\n");
}

// Quick tile sub-labels: the cached preset name (cut with "~" to fit, never
// "slot N" while a name is known), and ADMIN reflecting the login.
static void tile_labels() {
  char out[24];
  const int room = HomePage::kTileW - 6;
  HomePage::presetLabel(out, sizeof(out), "USA-Canada", 1, room);
  CHECK(std::strncmp(out, "USA", 3) == 0);
  CHECK(FrameBuffer565::textWidth(out, kFontSmall) <= room);
  HomePage::presetLabel(out, sizeof(out), "Test", 2, room);
  CHECK(std::strcmp(out, "Test") == 0);
  HomePage::presetLabel(out, sizeof(out), "", 3, room);
  CHECK(std::strcmp(out, "slot 3") == 0);
  HomePage::presetLabel(out, sizeof(out), "", 0, room);
  CHECK(std::strcmp(out, "custom") == 0);
  CHECK(std::strcmp(HomePage::adminLabel(false), "login") == 0);
  CHECK(std::strcmp(HomePage::adminLabel(true), "console") == 0);
  // Names the fonts cannot draw: emoji dropped, emoji-only falls back.
  char hdr[48];
  headerName(hdr, sizeof(hdr), "\xF0\x9F\xA5\x9E", kHeaderNameMaxPx);
  CHECK(std::strcmp(hdr, "M5 // REPEATER") == 0);
  headerName(hdr, sizeof(hdr), "Base \xF0\x9F\x93\xA1", kHeaderNameMaxPx);
  CHECK(std::strcmp(hdr, "M5 // BASE") == 0);
  if (!failures) std::printf("PASS tile_labels\n");
}

// Header: name cut to fit, stale and missing radio, uptime clock.
static void header_states() {
  char name[48];
  const AAFont* f = &headerName(name, sizeof(name), "Base", kHeaderNameMaxPx);
  CHECK(std::strcmp(name, "M5 // BASE") == 0 && f == &kFontName);
  f = &headerName(name, sizeof(name), "", kHeaderNameMaxPx);
  CHECK(std::strcmp(name, "M5 // REPEATER") == 0);
  // Too wide: smaller font, then the name alone, then whole words; never cut mid-word.
  f = &headerName(name, sizeof(name), "CaptCallahan-Base", kHeaderNameMaxPx);
  CHECK(FrameBuffer565::textWidth(name, *f) <= kHeaderNameMaxPx && std::strchr(name, '~') == nullptr);
  CHECK(std::strstr(name, "CAPTCALLAHAN") != nullptr);
  f = &headerName(name, sizeof(name), "An extremely long repeater name here", 120);
  CHECK(FrameBuffer565::textWidth(name, *f) <= 120 && std::strchr(name, '~') == nullptr);
  CHECK(std::strncmp("AN EXTREMELY LONG REPEATER NAME HERE", name, std::strlen(name)) == 0);
  f = &headerName(name, sizeof(name), "Supercalifragilisticexpialidocious", 120);
  CHECK(std::strcmp(name, "M5") == 0);
  MeshModel model(16, 16);
  PageContext ctx; ctxInit(ctx, &model);
  std::vector<uint16_t> px(480 * 320);
  FrameBuffer565 fb(px.data());
  fb.clear(theme::kBg);
  drawHeader(fb, ctx, 1000);   // no data at all
  int warn = 0;
  for (int y = 0; y < 34; y++) for (int x = 0; x < 480; x++) {
    const uint16_t p = px[(size_t)y * 480 + x];
    warn += ((p >> 11) & 31) >= 24 && ((p >> 5) & 63) >= 30 && (p & 31) <= 8;   // amber-ish
  }
  CHECK(warn > 10);
  vitals(model, ctx, 2000, -116, 42);
  fb.clear(theme::kBg);
  drawHeader(fb, ctx, 2000 + kRadioStaleMs + 1);   // stale: the frequency greys
  int accent = 0, dim = 0;
  for (int y = 0; y < 30; y++) for (int x = 200; x < 302; x++) { accent += px[(size_t)y * 480 + x] == theme::kAccent; dim += px[(size_t)y * 480 + x] == kDimText; }
  CHECK(accent == 0 && dim > 20);
  if (!failures) std::printf("PASS header_states\n");
}

// Owner previews (raw RGB565 frames): HOME graph mid-animation, GEO, busy,
// a tile in CONFIRM?, and another tab.
static int preview(const char* dir) {
  MeshModel model(64, 64);
  PageContext ctx; ctxInit(ctx, &model);
  std::snprintf(ctx.clock_hm, sizeof(ctx.clock_hm), "13:42");
  std::snprintf(ctx.clock_date, sizeof(ctx.clock_date), "31 Oct UTC");
  HomePage home;
  home.attach(&ctx);
  Screen s(&ctx);
  s.show(home, 1000);
  s.frame(home, model, 1000);
  populate(model, ctx, 2000);
  // Six minutes of traffic history for the rate trend (+13 %).
  static const int kPerMin[6] = {40, 38, 45, 41, 39, 46};
  uint32_t t = 60000;
  for (int m = 0; m < 6; m++) {
    for (int k = 0; k < kPerMin[m]; k++) {
      Bytes p = payload((uint8_t)k);
      p[2] = (uint8_t)m; p[3] = (uint8_t)k; p[4] = 0xEE;
      obs(model, packet(PAYLOAD_TYPE_TXT_MSG, ROUTE_TYPE_DIRECT, plen(0), Bytes(), p), t + (uint32_t)k * 1000u, 1.0f);
    }
    t += 60000;
  }
  t += 1000;
  vitals(model, ctx, t, -116, 42);
  for (int i = 0; i < 600 && home.wantsFrame(t); i++) { s.frame(home, model, t); t += 16; }
  obs(model, packet(PAYLOAD_TYPE_ACK, ROUTE_TYPE_FLOOD, plen(1), hashes({0xCA75}), Bytes(4, 0x77)), t + 600, 1.5f);
  ctx.data_gen++;
  ctx.obs_last_ms = t + 600;
  s.frame(home, model, t + 600);
  const uint32_t t0 = t + 700;
  obs(model, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, plen(2), hashes({0x1F01, 0x5701}), payload(9)), t0, 6.5f);
  ctx.data_gen++;
  ctx.obs_last_ms = t0;
  for (uint32_t u = t0; u <= t0 + 1100; u += 16) s.frame(home, model, u);
  std::string base(dir);
  bool ok = s.save((base + "/home_graph.rgb565").c_str());
  // CONFIRM? on ADVERT.
  ctx.now_ms = t0 + 1110;
  tap(home, tileCentreX(0), kTileCY);
  s.frame(home, model, t0 + 1116);
  ok = s.save((base + "/home_confirm.rgb565").c_str()) && ok;
  home.touch(10, 100, true);
  home.touch(10, 100, false);
  home.touch(HomePage::kToggleX + 60, HomePage::kToggleY + 10, true);
  home.touch(HomePage::kToggleX + 60, HomePage::kToggleY + 10, false);
  for (uint32_t u = t0 + 1132; u <= t0 + 2300; u += 16) s.frame(home, model, u);
  ok = s.save((base + "/home_geo.rgb565").c_str()) && ok;
  {
    // Busy: ~40 nodes in the model (31 shown), a route in flight.
    MeshModel bm(64, 128);
    PageContext bc; ctxInit(bc, &bm);
    std::snprintf(bc.clock_hm, sizeof(bc.clock_hm), "13:42");
    std::snprintf(bc.clock_date, sizeof(bc.clock_date), "31 Oct UTC");
    HomePage bh; bh.attach(&bc);
    Screen bs(&bc);
    bs.show(bh, 1000);
    populate(bm, bc, 2000);
    uint32_t bt = 8000;
    for (int i = 0; i < 31; i++) {
      const Bytes path = hashes({(uint16_t)(i % 3 == 0 ? 0x5701 : (i % 3 == 1 ? 0xCA75 : 0x2E11))});
      obs(bm, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, plen(1), path,
                     advert((uint8_t)(0x30 + i), 0x44, i % 4 ? ADV_TYPE_CHAT : ADV_TYPE_REPEATER,
                            (std::string("Node-") + std::to_string(i)).c_str(),
                            47.55 + (i % 9) * 0.02, -122.45 + (i % 11) * 0.025, i % 2 == 0)),
          bt += 200, (float)(i % 17) - 8.0f);
    }
    bc.data_gen++;
    bc.obs_last_ms = bt;
    for (int i = 0; i < 600 && bh.wantsFrame(bt); i++) { bs.frame(bh, bm, bt); bt += 16; }
    obs(bm, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, plen(2), hashes({0x1F01, 0x5701}), payload(9)), bt, 6.5f);
    bc.data_gen++;
    for (uint32_t u = bt; u <= bt + 900; u += 16) bs.frame(bh, bm, u);
    ok = bs.save((base + "/home_busy.rgb565").c_str()) && ok;
  }
  {
    PageSet set;
    set.attach(&ctx);
    Page* tel = set.page(PageSet::kTelemetry);
    Screen ts(&ctx);
    ts.show(*tel, 1000, PageSet::kTelemetry);
    ts.frame(*tel, model, t0 + 1200);
    ok = ts.save((base + "/home_tab.rgb565").c_str()) && ok;
  }
  // Render timing (host): one full HOME frame.
  {
    const auto c0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; i++) s.frame(home, model, t0 + 3000 + (uint32_t)i * 16);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - c0).count();
    std::printf("TIMING home_frame us=%lld\n", (long long)(us / 20));
  }
  std::printf(ok ? "PASS preview\n" : "FAIL preview\n");
  return ok ? 0 : 1;
}

int main(int argc, char** argv) {
  const std::string name = argc > 1 ? argv[1] : "";
  if (name == "preview") return preview(argc > 2 ? argv[2] : ".");
  if (name == "graph_layout_converges_and_stays_on_screen") graph_layout_converges_and_stays_on_screen();
  else if (name == "graph_layout_stable_when_node_added") graph_layout_stable_when_node_added();
  else if (name == "geo_projection_fits_all_nodes") geo_projection_fits_all_nodes();
  else if (name == "geo_projection_single_node") geo_projection_single_node();
  else if (name == "link_quality_colours") link_quality_colours();
  else if (name == "model_path_and_feed_sequence") model_path_and_feed_sequence();
  else if (name == "home_page_renders_and_animates") home_page_renders_and_animates();
  else if (name == "self_nodes_filtered") self_nodes_filtered();
  else if (name == "label_names_and_placement") label_names_and_placement();
  else if (name == "feed_colours_distinct") feed_colours_distinct();
  else if (name == "direct_neighbour_links_centre") direct_neighbour_links_centre();
  else if (name == "many_nodes_64") many_nodes_stay_put(64);
  else if (name == "many_nodes_256") many_nodes_stay_put(256);
  else if (name == "centre_ping_rate_limited") centre_ping_rate_limited();
  else if (name == "geo_without_repeater_fix") geo_without_repeater_fix();
  else if (name == "links_expire") links_expire();
  else if (name == "needle_only_on_vitals") needle_only_on_vitals();
  else if (name == "odometer_rolls_on_page") odometer_rolls_on_page();
  else if (name == "quick_controls_confirm") quick_controls_confirm();
  else if (name == "header_states") header_states();
  else if (name == "tile_labels") tile_labels();
  else { std::printf("unknown scenario %s\n", name.c_str()); return 2; }
  return failures ? 1 : 0;
}
'''

SOURCES = [UI / 'HomePage.cpp', UI / 'StubPages.cpp', UI / 'TelemetryPage.cpp', UI / 'MapPage.cpp', UI / 'NodesPage.cpp', UI / 'NodeCard.cpp', UI / 'MeshModel.cpp', UI / 'FrameBuffer565.cpp',
           UI / 'FrameBuffer332.cpp', UI / 'FbFonts.cpp', UI / 'FbFontsAA.cpp', ROOT / 'src' / 'Packet.cpp',
           ROOT / 'src' / 'helpers' / 'AdvertDataHelpers.cpp']


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def function_body(text, signature_regex):
    m = re.search(signature_regex, text)
    if m is None:
        raise AssertionError('function not found: ' + signature_regex)
    start = text.index('{', m.end())
    depth = 0
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise AssertionError('unbalanced body')


def build(work):
    compiler = shutil.which('g++') or shutil.which('clang++')
    if compiler is None:
        raise unittest.SkipTest('a host C++17 compiler is required')
    sanitize = [] if os.name == 'nt' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    cpp = work / 'layouts.cpp'
    cpp.write_text(HARNESS, encoding='ascii')
    exe = work / ('layouts' + ('.exe' if os.name == 'nt' else ''))
    built = subprocess.run([
        compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', *sanitize,
        '-I', str(UI), '-isystem', str(ROOT / 'src'), '-isystem', str(ROOT / 'test' / 'mocks'),
        str(cpp), *[str(s) for s in SOURCES], '-o', str(exe),
    ], capture_output=True, text=True, timeout=300, cwd=str(ROOT / 'test'))
    if built.returncode != 0:
        raise AssertionError(built.stdout + built.stderr)
    return compiler, exe


def rgb565_rows(raw, w=480, h=320, scale=2):
    """RGB565 little-endian frame -> list of RGB byte rows, scaled up."""
    rows = []
    for y in range(h):
        line = bytearray()
        for x in range(w):
            v = raw[2 * (y * w + x)] | (raw[2 * (y * w + x) + 1] << 8)
            r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
            line += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))) * scale
        for _ in range(scale):
            rows.append(bytes(line))
    return rows


def write_png(rows, path):
    h = len(rows)
    w = len(rows[0]) // 3

    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(b''.join(b'\x00' + r for r in rows), 9)) + chunk(b'IEND', b'')
    Path(path).write_bytes(png)


class LayoutsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='freenove-layouts-')
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

    def test_graph_layout_converges_and_stays_on_screen(self):
        self.run_scenario('graph_layout_converges_and_stays_on_screen')

    def test_graph_layout_stable_when_node_added(self):
        self.run_scenario('graph_layout_stable_when_node_added')

    def test_geo_projection_fits_all_nodes(self):
        self.run_scenario('geo_projection_fits_all_nodes')

    def test_geo_projection_single_node(self):
        self.run_scenario('geo_projection_single_node')

    def test_link_quality_colours(self):
        self.run_scenario('link_quality_colours')

    def test_model_path_and_feed_sequence(self):
        self.run_scenario('model_path_and_feed_sequence')

    def test_home_page_renders_and_animates(self):
        self.run_scenario('home_page_renders_and_animates')

    def test_self_nodes_filtered(self):
        self.run_scenario('self_nodes_filtered')

    def test_label_names_and_placement(self):
        self.run_scenario('label_names_and_placement')

    def test_feed_colours_distinct(self):
        self.run_scenario('feed_colours_distinct')

    def test_direct_neighbour_links_centre(self):
        self.run_scenario('direct_neighbour_links_centre')

    def test_many_nodes_64(self):
        self.run_scenario('many_nodes_64')

    def test_many_nodes_256(self):
        self.run_scenario('many_nodes_256')

    def test_centre_ping_rate_limited(self):
        self.run_scenario('centre_ping_rate_limited')

    def test_geo_without_repeater_fix(self):
        self.run_scenario('geo_without_repeater_fix')

    def test_links_expire(self):
        self.run_scenario('links_expire')

    def test_needle_only_on_vitals(self):
        self.run_scenario('needle_only_on_vitals')

    def test_odometer_rolls_on_page(self):
        self.run_scenario('odometer_rolls_on_page')

    def test_quick_controls_confirm(self):
        self.run_scenario('quick_controls_confirm')

    def test_header_states(self):
        self.run_scenario('header_states')

    def test_tile_labels(self):
        self.run_scenario('tile_labels')

    def test_preview_renders(self):
        out = Path(self.temp.name)
        ran = subprocess.run([str(self.exe), 'preview', str(out)], capture_output=True, text=True, timeout=120)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        for name in ('home_graph', 'home_geo', 'home_busy', 'home_confirm', 'home_tab'):
            self.assertEqual((out / (name + '.rgb565')).stat().st_size, 480 * 320 * 2)

    def test_pages_use_theme_tokens_only(self):
        # Colours come from Theme.h only: no raw palette constants or rgb()
        # in page code, HUD/tab bar code or the framework.
        for name in ('HomePage.cpp', 'HomePage.h', 'StubPages.cpp', 'StubPages.h', 'FreenoveUITask.cpp'):
            src = strip_comments((UI / name).read_text(encoding='ascii'))
            self.assertNotIn('fbpal::', src, name)
            self.assertNotRegex(src, r'\brgb(565)?\(', name)
            self.assertNotRegex(src, r'(fill\w*|text|\w*[lL]ine|\w*[rR]ect|\w*Ring|\w*Arc|\w*Disc|hline|vline)\([^;]*0x[0-9A-Fa-f]{2,4}\b', name)
        theme = (UI / 'Theme.h').read_text(encoding='ascii')
        for token in ('kBg', 'kPanelFill', 'kPanelEdge', 'kDivider', 'kText', 'kTextDim', 'kAccent', 'kStale',
                      'kSigStrong', 'kSigGood', 'kSigWeak', 'kSigPoor', 'kTabFill', 'kTabActiveFill', 'kTileOnFill',
                      'kOdoFill', 'kGaugeTrack'):
            self.assertRegex(theme, r'constexpr uint16_t %s\b' % token)
        # Fonts: OFL only (no Bahnschrift), licence next to the generated data.
        fonts = (UI / 'FbFontsAA.cpp').read_text(encoding='ascii')
        self.assertIn('D-DIN', fonts)
        self.assertNotIn('ahnschrift', fonts)
        self.assertTrue((UI / 'LICENSE.D-DIN-OFL.txt').exists())
        self.assertTrue((UI / 'tools' / 'gen_aa_fonts.py').exists())

    def test_layouts_are_pure_cxx11(self):
        # The head firmware may build as gnu++11; no Arduino in the page.
        for name in ('GraphLayout.h', 'GeoLayout.h', 'HomePage.h', 'HomePage.cpp'):
            src = (UI / name).read_text(encoding='ascii')
            self.assertNotIn('Arduino.h', src)
            self.assertNotIn('esp_', src)
        checked = subprocess.run([
            self.compiler, '-std=c++11', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
            '-I', str(UI), '-isystem', str(ROOT / 'src'), '-isystem', str(ROOT / 'test' / 'mocks'),
            str(UI / 'HomePage.cpp'),
        ], capture_output=True, text=True, timeout=60)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)


class HomeWiringTest(unittest.TestCase):
    """Device glue the host harness cannot run."""

    def test_page_set_uses_home_page(self):
        header = strip_comments((UI / 'StubPages.h').read_text(encoding='ascii'))
        self.assertIn('HomePage _home;', header)
        self.assertNotIn('HomeStubPage', header)

    def test_product_env_builds_home_page(self):
        ini = (VARIANT / 'platformio.ini').read_text(encoding='ascii')
        product = ini.split('[env:Freenove_S3_35_Head_companion_usb]')[1].split('[env:')[0]
        self.assertIn('ui-freenove/HomePage.cpp', product)
        self.assertIn('ui-freenove/FrameBuffer565.cpp', ini)
        self.assertIn('ui-freenove/FbFontsAA.cpp', ini)

    def test_bridged_packets_reach_the_model(self):
        # Companion loop: every MeshCore packet the head receives (ESP-NOW
        # bridge) is handed to the visual head, after the route-policy check
        # and before parsing, without blocking.
        raw = (ROOT / 'examples' / 'companion_radio' / 'MyMesh.cpp').read_bytes()
        self.assertNotIn(b'\n', raw.replace(b'\r\n', b''), 'MyMesh.cpp must stay CRLF')
        mymesh = strip_comments(raw.decode('ascii'))
        body = function_body(mymesh, r'void MyMesh::logRxRaw\(')
        self.assertIn('freenoveHeadReceived(raw, len)', body)
        ui = strip_comments((UI / 'FreenoveUITask.cpp').read_text(encoding='ascii'))
        hook = function_body(ui, r'void freenoveHeadReceived\(')
        self.assertIn('portENTER_CRITICAL', hook)
        self.assertIn('_bridged.push(', hook)
        self.assertNotIn('_model', hook)
        self.assertIn('s_head.load(std::memory_order_acquire)', hook)
        self.assertIn('std::atomic<FreenoveUITask*> s_head', ui)
        begin = function_body(ui, r'bool FreenoveUITask::begin\(')
        self.assertIn('setHeadKey(self_pub_key)', begin)
        main = (ROOT / 'examples' / 'companion_radio' / 'main.cpp').read_bytes()
        self.assertNotIn(b'\n', main.replace(b'\r\n', b''), 'main.cpp must stay CRLF')
        self.assertIn(b'freenove_pages.begin(the_mesh.self_id.pub_key)', main)
        drain = function_body(ui, r'void FreenoveUITask::drainObserver\(')
        self.assertRegex(drain, r'onBridgedPacket\(\s*\w+\s*,\s*\w+\s*,\s*now_ms\s*\)')
        self.assertIn('_bridged.pop(', drain)


    def test_quick_controls_reach_the_companion(self):
        # HOME's requests run on the companion's loop (the objects are its),
        # through the head's existing paths: the_mesh.advert(), the admin
        # console, its saved-password login. Never a password in code.
        ui = strip_comments((UI / 'FreenoveUITask.cpp').read_text(encoding='ascii'))
        service = function_body(ui, r'void FreenoveUITask::serviceUiLoop\(')
        self.assertIn('the_mesh.advert()', service)
        self.assertIn('ui_task.showAdmin()', service)
        self.assertIn('ui_task.showAdminLogin()', service)
        self.assertIn('ui_task.showAdminPresets()', service)   # PRESET: the preset list
        self.assertIn('ui_task.showAdminSettings()', service)  # OBSERVER / BRIDGE: settings
        self.assertIn('headPresetListSink() = onPresetList', ui)
        self.assertNotIn('login ', service)
        handle = function_body(ui, r'void FreenoveUITask::handleRequest\(')
        self.assertIn('compare_exchange_strong', handle)
        self.assertIn('showPage(freenove_ui::PageSet::kHead', handle)
        main = (ROOT / 'examples' / 'companion_radio' / 'main.cpp').read_bytes()
        self.assertIn(b'freenove_pages.serviceUiLoop();', main)
        admin_raw = (ROOT / 'examples' / 'companion_radio' / 'ui-new' / 'HeadAdmin.h').read_bytes()
        self.assertNotIn(b'\n', admin_raw.replace(b'\r\n', b''), 'HeadAdmin.h must stay CRLF')
        admin = strip_comments(admin_raw.decode('ascii'))
        # ADMIN opens the password entry (prefilled, masked): never a
        # silent re-send of a saved password the owner cannot change.
        self.assertIn('openLoginEntry()', function_body(admin, r'void loginSaved\('))
        uitask = strip_comments((ROOT / 'examples' / 'companion_radio' / 'ui-new' / 'UITask.cpp').read_bytes().decode('ascii'))
        self.assertIn('loginSaved()', function_body(uitask, r'void UITask::showAdminLogin\('))
        self.assertIn('openPresetsScreen()', function_body(uitask, r'void UITask::showAdminPresets\('))
        self.assertIn('openPresets()', function_body(admin, r'void openPresetsScreen\('))
        # Only ADVERT transmits from HOME, and it needs the confirm step.
        hmi = (UI / 'HmiLogic.h').read_text(encoding='ascii')
        self.assertIn('static bool needsConfirm(int tile) { return tile == kTileAdvert; }', hmi)

def write_preview():
    """Previews at true 1:1 (480x320, *_1x.png) and a 2x nearest-neighbour
    zoom (*.png), plus home_vs_mock_1x.png: the approved mockup rendered at
    480x320 next to the firmware's HOME, both 1:1 and both zoomed 2x."""
    with tempfile.TemporaryDirectory(prefix='freenove-preview-') as tmp:
        _, exe = build(Path(tmp))
        ran = subprocess.run([str(exe), 'preview', tmp], capture_output=True, text=True, timeout=120)
        if ran.returncode != 0:
            raise SystemExit(ran.stdout + ran.stderr)
        print(ran.stdout.strip())
        for src, dst in (('home_graph', 'home_preview'), ('home_geo', 'home_preview_geo'),
                         ('home_busy', 'home_preview_busy'), ('home_confirm', 'home_preview_confirm'),
                         ('home_tab', 'home_preview_tab')):
            raw = (Path(tmp) / (src + '.rgb565')).read_bytes()
            write_png(rgb565_rows(raw, scale=1), PREVIEW_DIR / (dst + '_1x.png'))
            write_png(rgb565_rows(raw, scale=2), PREVIEW_DIR / (dst + '.png'))
            print('wrote', PREVIEW_DIR / (dst + '_1x.png'), 'and the 2x zoom')
    from PIL import Image, ImageDraw
    mock = Image.open(PREVIEW_DIR / 'mockups' / 'mock_glass3_1x.png').convert('RGB')
    ours = Image.open(PREVIEW_DIR / 'home_preview_1x.png').convert('RGB')
    both = Image.new('RGB', (480 * 2 + 8, 320 * 3 + 24), (0, 0, 0))
    both.paste(mock, (0, 16))
    both.paste(ours, (488, 16))
    both.paste(mock.resize((480, 320 * 2 // 2), Image.NEAREST), (0, 16))
    zoom_m = mock.crop((0, 0, 240, 160)).resize((480, 320), Image.NEAREST)
    zoom_o = ours.crop((0, 0, 240, 160)).resize((480, 320), Image.NEAREST)
    both.paste(zoom_m, (0, 344))
    both.paste(zoom_o, (488, 344))
    zoom_m = mock.crop((240, 160, 480, 320)).resize((480, 320), Image.NEAREST)
    zoom_o = ours.crop((240, 160, 480, 320)).resize((480, 320), Image.NEAREST)
    both.paste(zoom_m, (0, 664))
    both.paste(zoom_o, (488, 664))
    d = ImageDraw.Draw(both)
    d.text((4, 2), 'mockup at 480x320 (1:1, then 2x crops)', fill=(200, 200, 200))
    d.text((492, 2), 'firmware HOME (1:1, then 2x crops)', fill=(200, 200, 200))
    both.save(PREVIEW_DIR / 'home_vs_mock_1x.png')
    print('wrote', PREVIEW_DIR / 'home_vs_mock_1x.png')


if __name__ == '__main__':
    if '--preview' in sys.argv:
        write_preview()
    else:
        unittest.main()
