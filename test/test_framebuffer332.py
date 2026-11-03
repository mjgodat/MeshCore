"""Host tests for the Freenove head RGB332 framebuffer engine (FrameBuffer332).

Pure C++: FrameBuffer332.cpp + FbFonts.cpp compiled against a harness.
Sanitizers (ASan/UBSan) run on non-Windows hosts (WSL is the authority for
primitives_clip_safely); on Windows the guard-byte check still runs.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / 'examples' / 'companion_radio' / 'ui-freenove'

HARNESS = r'''
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>
#include "FrameBuffer332.h"
#include "FbFonts.h"
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static const int W = 480, H = 320;

static void allPrimitives(FrameBuffer332& fb, int a, int b, int c, int d) {
  fb.pixel(a, b, 0x11);
  fb.hline(a, b, c, 0x12);
  fb.vline(a, b, d, 0x13);
  fb.fillRect(a, b, c, d, 0x14);
  fb.rect(a, b, c, d, 0x15);
  fb.line(a, b, c, d, 0x16);
  fb.dashedLine(a, b, c, d, 0x17, 3, 2, a);
  fb.dashedLine(a, b, c, d, 0x17, 0, 0, b);       // degenerate pattern
  fb.dashedLine(a, b, c, d, 0x17, -4, -9, c);
  fb.circle(a, b, c, 0x18);
  fb.fillCircle(a, b, c, 0x19);
  fb.circle(a, b, d, 0x18);
  fb.fillCircle(a, b, d, 0x19);
  fb.arc(a, b, c, a, b, d, 0x1A);
  fb.arc(a, b, d, 0, 359, c, 0x1A);
  fb.text(a, b, "Clip! ~\x7f\x01\xff", 0x1B, kFont6x8Id);
  fb.text(a, b, "Clip! ~", 0x1B, kFont8x16Id);
  fb.text(a, b, "x", 0x1B, 7);                     // unknown font id
  fb.glowDot(a, b, fbpal::kCyan);
  (void)fb.get(a, b);
}

static int scenario_clip() {
  const int v[] = {INT_MIN, INT_MIN + 1, -1000000, -4097, -7, -1, 0, 1, 3, 239, 319, 320,
                   479, 480, 481, 4097, 1000000, INT_MAX - 1, INT_MAX};
  const int n = (int)(sizeof(v) / sizeof(v[0]));
  // Exact-size heap buffer: ASan flags any write outside it.
  uint8_t* exact = new uint8_t[W * H];
  FrameBuffer332 fb(exact, W, H);
  fb.clear(0);
  // Guarded buffer: catches out-of-bounds writes without ASan (MinGW).
  const int G = 64 * 1024;
  std::vector<uint8_t> mem(G + W * H + G, 0xA5);
  FrameBuffer332 gb(&mem[G], W, H);
  gb.clear(0);
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++)
      for (int k = 0; k < n; k += 3)
        for (int l = 0; l < n; l += 4) {
          allPrimitives(fb, v[i], v[j], v[k], v[l]);
          allPrimitives(gb, v[i], v[j], v[k], v[l]);
        }
  bool guards = true;
  for (int i = 0; i < G; i++)
    if (mem[i] != 0xA5 || mem[G + W * H + i] != 0xA5) guards = false;
  CHECK(guards);
  // On-screen primitives still draw.
  gb.clear(0);
  gb.line(-100, 160, 1000, 160, 0x16);
  CHECK(gb.get(0, 160) == 0x16 && gb.get(479, 160) == 0x16);
  gb.fillCircle(240, 160, 10, 0x19);
  CHECK(gb.get(240, 160) == 0x19 && gb.get(250, 160) == 0x19 && gb.get(251, 160) != 0x19);
  gb.circle(100, 100, 5, 0x18);
  CHECK(gb.get(105, 100) == 0x18 && gb.get(100, 95) == 0x18 && gb.get(100, 100) == 0);
  gb.clear(0);
  gb.arc(240, 160, 50, 0, 90, 4, 0x1A);           // east -> south (clockwise)
  CHECK(gb.get(289, 160) == 0x1A && gb.get(240, 209) == 0x1A);
  CHECK(gb.get(240, 111) == 0 && gb.get(191, 160) == 0 && gb.get(240, 160) == 0);
  CHECK(gb.get(275, 195) == 0x1A);                // 45 deg, radius ~49.5
  gb.clear(0);
  gb.arc(240, 160, 50, 270, 0, 2, 0x1A);          // north -> east
  CHECK(gb.get(240, 111) == 0x1A && gb.get(289, 160) == 0x1A && gb.get(240, 209) == 0);
  gb.clear(0);
  gb.arc(240, 160, 50, 300, 60, 2, 0x1A);         // a1 < a0 wraps through 0
  CHECK(gb.get(289, 160) == 0x1A && gb.get(191, 160) == 0);
  delete[] exact;
  if (failures) return 1;
  std::printf("PASS clip\n");
  return 0;
}

static int scenario_lut() {
  uint16_t a[256], b[256];
  buildLut565(a, false);
  buildLut565(b, true);
  for (int i = 0; i < 256; i++) std::printf("%04x %04x\n", a[i], b[i]);
  std::printf("PASS lut\n");
  return 0;
}

static int scenario_diff() {
  std::vector<uint8_t> px(W * H, 0);
  FrameBuffer332 fb(px.data(), W, H);
  uint32_t last[kMaxFbRows] = {};
  DirtyRows d;
  diffRows(fb, last, d);                          // prime
  diffRows(fb, last, d);
  CHECK(d.count == 0);                            // identical frame
  fb.pixel(37, 100, 0x3B);
  diffRows(fb, last, d);
  CHECK(d.count == 1 && d.y0[0] == 100 && d.n[0] == 1);
  diffRows(fb, last, d);
  CHECK(d.count == 0);
  // Every x position of a row is seen (no blind words), first and last row too.
  for (int y : {0, 100, 319}) {
    for (int x = 0; x < W; x++) {
      const uint8_t old = fb.get(x, y);
      fb.pixel(x, y, (uint8_t)(old ^ 0x01));
      diffRows(fb, last, d);
      if (!(d.count == 1 && d.y0[0] == y && d.n[0] == 1)) { CHECK(false); std::printf("x=%d y=%d\n", x, y); break; }
    }
  }
  // Adjacent changed rows coalesce; a gap splits.
  fb.hline(0, 10, 5, 0x1C); fb.hline(0, 11, 5, 0x1C); fb.hline(0, 12, 5, 0x1C);
  fb.pixel(9, 14, 0x1C);
  diffRows(fb, last, d);
  CHECK(d.count == 2 && d.y0[0] == 10 && d.n[0] == 3 && d.y0[1] == 14 && d.n[1] == 1);
  // Moving a pixel within a row is a change even if the row's bytes sum the same.
  fb.clear(0); diffRows(fb, last, d);
  fb.pixel(5, 50, 0x80); fb.pixel(6, 50, 0x01); diffRows(fb, last, d);
  fb.pixel(5, 50, 0x01); fb.pixel(6, 50, 0x80); diffRows(fb, last, d);
  CHECK(d.count == 1 && d.y0[0] == 50);
  // Full change: one run covering everything.
  fb.clear(0x55);
  diffRows(fb, last, d);
  CHECK(d.count == 1 && d.y0[0] == 0 && d.n[0] == H);
  // Alternating rows: 160 runs, all recorded.
  for (int y = 0; y < H; y += 2) fb.hline(0, y, W, 0x56);
  diffRows(fb, last, d);
  CHECK(d.count == 160 && d.y0[159] == 318);

  // planBands: align to 4, merge small gaps, clamp.
  DirtyRows r, b;
  r.count = 1; r.y0[0] = 100; r.n[0] = 1;
  planBands(r, H, kBandAlign, kBandMergeGap, b);
  CHECK(b.count == 1 && b.y0[0] == 100 && b.n[0] == 4);
  r.y0[0] = 103;
  planBands(r, H, kBandAlign, kBandMergeGap, b);
  CHECK(b.count == 1 && b.y0[0] == 100 && b.n[0] == 4);
  r.y0[0] = 103; r.n[0] = 2;                      // straddles a boundary
  planBands(r, H, kBandAlign, kBandMergeGap, b);
  CHECK(b.count == 1 && b.y0[0] == 100 && b.n[0] == 8);
  r.y0[0] = 319; r.n[0] = 1;
  planBands(r, H, kBandAlign, kBandMergeGap, b);
  CHECK(b.count == 1 && b.y0[0] == 316 && b.n[0] == 4);
  r.count = 2; r.y0[0] = 10; r.n[0] = 1; r.y0[1] = 18; r.n[1] = 1;   // gap 4 -> merge
  planBands(r, H, kBandAlign, kBandMergeGap, b);
  CHECK(b.count == 1 && b.y0[0] == 8 && b.n[0] == 12);
  r.y0[1] = 30;                                                      // gap 16 -> split
  planBands(r, H, kBandAlign, kBandMergeGap, b);
  CHECK(b.count == 2 && b.y0[0] == 8 && b.n[0] == 4 && b.y0[1] == 28 && b.n[1] == 4);
  planBands(r, H, kBandAlign, 0, b);
  CHECK(b.count == 2);
  r.y0[1] = 12;                                                      // same band
  planBands(r, H, kBandAlign, 0, b);
  CHECK(b.count == 1 && b.y0[0] == 8 && b.n[0] == 8);
  r.count = 0;
  planBands(r, H, kBandAlign, kBandMergeGap, b);
  CHECK(b.count == 0 && bandBytes(b, W) == 0);
  r.count = 1; r.y0[0] = 0; r.n[0] = H;
  planBands(r, H, kBandAlign, kBandMergeGap, b);
  CHECK(b.count == 1 && b.y0[0] == 0 && b.n[0] == H && bandBytes(b, W) == 307200);
  if (failures) return 1;
  std::printf("PASS diff\n");
  return 0;
}

// Lit pixel positions along a line on a clean buffer.
static std::vector<int> litX(FrameBuffer332& fb, int y) {
  std::vector<int> v;
  for (int x = 0; x < W; x++) if (fb.get(x, y)) v.push_back(x);
  return v;
}

static int scenario_dash() {
  std::vector<uint8_t> px(W * H, 0);
  FrameBuffer332 fb(px.data(), W, H);
  for (int phase = -9; phase < 12; phase++) {
    // Horizontal, left to right: phase + 1 moves the dashes one pixel right.
    fb.clear(0); fb.dashedLine(10, 50, 200, 50, 1, 4, 3, phase);
    std::vector<uint8_t> a(W), b(W);
    for (int x = 0; x < W; x++) a[x] = fb.get(x, 50);
    fb.clear(0); fb.dashedLine(10, 50, 200, 50, 1, 4, 3, phase + 1);
    for (int x = 0; x < W; x++) b[x] = fb.get(x, 50);
    for (int x = 11; x <= 200; x++) CHECK(b[x] == a[x - 1]);
    int lit = 0; for (int x = 10; x <= 200; x++) lit += a[x] ? 1 : 0;
    CHECK(lit >= 191 * 4 / 7 - 4 && lit <= 191 * 4 / 7 + 4);
    // Right to left: dashes move toward (x1, y1), i.e. left.
    fb.clear(0); fb.dashedLine(200, 60, 10, 60, 1, 4, 3, phase);
    for (int x = 0; x < W; x++) a[x] = fb.get(x, 60);
    fb.clear(0); fb.dashedLine(200, 60, 10, 60, 1, 4, 3, phase + 1);
    for (int x = 0; x < W; x++) b[x] = fb.get(x, 60);
    for (int x = 10; x < 200; x++) CHECK(b[x] == a[x + 1]);
    // Vertical and diagonal: shift by one step along the major axis.
    fb.clear(0); fb.dashedLine(70, 10, 70, 300, 1, 5, 5, phase);
    std::vector<uint8_t> c(H), e(H);
    for (int y = 0; y < H; y++) c[y] = fb.get(70, y);
    fb.clear(0); fb.dashedLine(70, 10, 70, 300, 1, 5, 5, phase + 1);
    for (int y = 0; y < H; y++) e[y] = fb.get(70, y);
    for (int y = 11; y <= 300; y++) CHECK(e[y] == c[y - 1]);
    fb.clear(0); fb.dashedLine(20, 20, 220, 220, 1, 3, 3, phase);
    std::vector<uint8_t> da(201), db(201);
    for (int i = 0; i <= 200; i++) da[i] = fb.get(20 + i, 20 + i);
    fb.clear(0); fb.dashedLine(20, 20, 220, 220, 1, 3, 3, phase + 1);
    for (int i = 0; i <= 200; i++) db[i] = fb.get(20 + i, 20 + i);
    for (int i = 1; i <= 200; i++) CHECK(db[i] == da[i - 1]);
    // Clipping keeps the pattern anchored at the real start point.
    fb.clear(0); fb.dashedLine(-37, 80, 300, 80, 1, 4, 3, phase);
    std::vector<int> clipped = litX(fb, 80);
    fb.clear(0); fb.dashedLine(0, 80, 300, 80, 1, 4, 3, phase - 37);
    CHECK(litX(fb, 80) == clipped);
  }
  // A plain line is a dashed line that is always on.
  fb.clear(0); fb.line(3, 7, 400, 290, 1);
  std::vector<uint8_t> solid(px);
  fb.clear(0); fb.dashedLine(3, 7, 400, 290, 1, 1, 0, 0);
  CHECK(px == solid);
  if (failures) return 1;
  std::printf("PASS dash\n");
  return 0;
}

static int lum(uint8_t c) { return ((c >> 5) & 7) * 3 * 36 + ((c >> 2) & 7) * 6 * 36 + (c & 3) * 1 * 85; }

static int scenario_glow() {
  // Palette constants are the mockup colours on the 332 cube.
  CHECK(fbpal::kCyan == FrameBuffer332::rgb(0x22, 0xe3, 0xff));
  CHECK(fbpal::kSky == FrameBuffer332::rgb(0x5f, 0xd3, 0xff));
  CHECK(fbpal::kIce == FrameBuffer332::rgb(0x8f, 0xe9, 0xff));
  CHECK(fbpal::kGreen == FrameBuffer332::rgb(0x9d, 0xff, 0xb0));
  CHECK(fbpal::kAmber == FrameBuffer332::rgb(0xff, 0xd1, 0x66));
  CHECK(fbpal::kMagenta == FrameBuffer332::rgb(0xff, 0x5f, 0xa2));
  CHECK(fbpal::kViolet == FrameBuffer332::rgb(0xb1, 0x8c, 0xff));
  CHECK(fbpal::kGrid == FrameBuffer332::rgb(0x12, 0x32, 0x4a));
  CHECK(fbpal::kNavy == FrameBuffer332::rgb(0x0b, 0x10, 0x30));
  CHECK(fbpal::kBlack == FrameBuffer332::rgb(0x03, 0x04, 0x0a));
  CHECK(FrameBuffer332::rgb(255, 255, 255) == 0xFF && FrameBuffer332::rgb(0, 0, 0) == 0x00);
  CHECK(FrameBuffer332::rgb(255, 0, 0) == 0xE0 && FrameBuffer332::rgb(0, 255, 0) == 0x1C && FrameBuffer332::rgb(0, 0, 255) == 0x03);

  const uint8_t neon[] = {fbpal::kCyan, fbpal::kSky, fbpal::kIce, fbpal::kGreen, fbpal::kAmber,
                          fbpal::kMagenta, fbpal::kViolet, fbpal::kWhite};
  std::vector<uint8_t> px(W * H, 0);
  FrameBuffer332 fb(px.data(), W, H);
  for (uint8_t c : neon) {
    const uint8_t r0 = FrameBuffer332::glowRamp(c, 0);
    const uint8_t r1 = FrameBuffer332::glowRamp(c, 1);
    const uint8_t r2 = FrameBuffer332::glowRamp(c, 2);
    CHECK(r0 == c);
    CHECK(r1 != 0 && r2 != 0);                       // never fades to black
    CHECK(lum(c) > lum(r1) && lum(r1) > lum(r2));    // strictly dimmer outward
    for (int sh : {5, 2, 0}) {                       // each channel non-increasing
      const int m = sh == 0 ? 3 : 7;
      CHECK(((r1 >> sh) & m) <= ((c >> sh) & m));
      CHECK(((r2 >> sh) & m) <= ((r1 >> sh) & m));
    }
    // The dominant channel of the colour stays lit in both rings (hue kept).
    fb.clear(0);
    fb.glowDot(100, 100, c);
    std::set<uint8_t> seen;
    for (int y = 80; y <= 120; y++) for (int x = 80; x <= 120; x++) if (fb.get(x, y)) seen.insert(fb.get(x, y));
    CHECK(seen.size() == 3 && seen.count(c) && seen.count(r1) && seen.count(r2));
    CHECK(fb.get(100, 100) == c);
    CHECK(fb.get(100 + FrameBuffer332::kGlowCore, 100) == c);
    CHECK(fb.get(100 + FrameBuffer332::kGlowMid, 100) == r1);
    CHECK(fb.get(100 + FrameBuffer332::kGlowOuter, 100) == r2);
    CHECK(fb.get(100 + FrameBuffer332::kGlowOuter + 1, 100) == 0);
    for (int dy = -8; dy <= 8; dy++) for (int dx = -8; dx <= 8; dx++) {   // symmetric
      const uint8_t p = fb.get(100 + dx, 100 + dy);
      CHECK(p == fb.get(100 - dx, 100 + dy) && p == fb.get(100 + dy, 100 + dx));
    }
  }
  // Overlapping glows never darken each other.
  fb.clear(0);
  fb.glowDot(200, 200, fbpal::kCyan);
  fb.glowDot(204, 200, fbpal::kMagenta);
  CHECK(fb.get(200, 200) == FrameBuffer332::maxBlend(fbpal::kCyan, FrameBuffer332::glowRamp(fbpal::kMagenta, 1)));
  CHECK(fb.get(204, 200) == fbpal::kMagenta);
  if (failures) return 1;
  std::printf("PASS glow\n");
  return 0;
}

static int scenario_text() {
  std::vector<uint8_t> px(W * H, 0);
  FrameBuffer332 fb(px.data(), W, H);
  CHECK(FrameBuffer332::textWidth("HUD 12", kFont6x8Id) == 36);
  CHECK(FrameBuffer332::textWidth("HUD 12", kFont8x16Id) == 48);
  CHECK(fb.text(10, 20, "A", 0x3B, kFont8x16Id) == 18);
  const uint8_t* g = &kFont8x16[('A' - kFontFirstChar) * 16];
  bool same = true;
  for (int r = 0; r < 16; r++) for (int col = 0; col < 8; col++)
    if (((g[r] & (0x80 >> col)) != 0) != (fb.get(10 + col, 20 + r) == 0x3B)) same = false;
  CHECK(same);
  fb.clear(0);
  CHECK(fb.text(0, 0, "Mq", 0x9E, kFont6x8Id) == 12);
  const uint8_t* m = &kFont6x8[('M' - kFontFirstChar) * 8];
  same = true;
  for (int r = 0; r < 8; r++) for (int col = 0; col < 6; col++)
    if (((m[r] & (0x80 >> col)) != 0) != (fb.get(col, r) == 0x9E)) same = false;
  CHECK(same);
  // Glyphs are not blank (font data present) except space.
  for (int ch = kFontFirstChar + 1; ch <= kFontLastChar; ch++) {
    int a = 0, b = 0;
    for (int r = 0; r < 8; r++) a |= kFont6x8[(ch - kFontFirstChar) * 8 + r];
    for (int r = 0; r < 16; r++) b |= kFont8x16[(ch - kFontFirstChar) * 16 + r];
    CHECK(a != 0 && b != 0);
    CHECK((a & 0x03) == 0);                          // 6x8 glyphs stay in 6 columns
  }
  if (failures) return 1;
  std::printf("PASS text\n");
  return 0;
}

// Copy the planned windows from fb to the simulated glass.
static void applyBands(const FrameBuffer332& fb, const SpanBands& b, std::vector<uint8_t>& glass) {
  for (int i = 0; i < b.count; i++)
    for (int y = b.y0[i]; y < b.y0[i] + b.n[i]; y++)
      for (int x = b.x0[i]; x <= b.x1[i]; x++) glass[y * W + x] = fb.get(x, y);
}

static bool bandsWellFormed(const SpanBands& b) {
  int prevEnd = 0;
  for (int i = 0; i < b.count; i++) {
    if (b.y0[i] % kBandAlign || b.n[i] <= 0 || b.n[i] % kBandAlign) return false;
    if (b.y0[i] < prevEnd || b.y0[i] + b.n[i] > H) return false;
    if (b.x0[i] < 0 || b.x0[i] > b.x1[i] || b.x1[i] >= W) return false;
    prevEnd = b.y0[i] + b.n[i];
  }
  return true;
}

// Tracked spans equal the rows the primitive drew: a row is marked iff it has
// a lit pixel, every lit pixel lies in its row's span, and (tight) the span
// ends on lit pixels.
static bool spansMatch(const FrameBuffer332& fb, const RowSpans& s, bool tight) {
  for (int y = 0; y < H; y++) {
    int first = -1, last = -1;
    for (int x = 0; x < W; x++) if (fb.get(x, y)) { if (first < 0) first = x; last = x; }
    if ((first >= 0) != s.has(y)) { std::printf("row %d lit=%d marked=%d\n", y, first >= 0, s.has(y)); return false; }
    if (first < 0) continue;
    if (first < s.x0[y] || last > s.x1[y]) { std::printf("row %d pixels %d..%d span %d..%d\n", y, first, last, s.x0[y], s.x1[y]); return false; }
    if (tight && (first != s.x0[y] || last != s.x1[y])) { std::printf("row %d loose %d..%d vs %d..%d\n", y, first, last, s.x0[y], s.x1[y]); return false; }
  }
  return true;
}

static bool sameSpans(const RowSpans& a, const RowSpans& b) {
  for (int y = 0; y < kMaxFbRows; y++)
    if (a.has(y) != b.has(y) || (a.has(y) && (a.x0[y] != b.x0[y] || a.x1[y] != b.x1[y]))) return false;
  return true;
}

// pixels() is read-only; writers go through rawPixels() (marks everything).
static_assert(std::is_same<decltype(std::declval<FrameBuffer332&>().pixels()), const uint8_t*>::value,
              "pixels() must be read-only");

static int scenario_track() {
  std::vector<uint8_t> px(W * H, 0);
  FrameBuffer332 fb(px.data(), W, H);
  RowSpans sink;
  sink.reset();
  struct Case { const char* name; bool tight; void (*draw)(FrameBuffer332&); };
  const Case cases[] = {
    {"pixel", true, [](FrameBuffer332& f) { f.pixel(5, 7, 1); f.pixel(479, 319, 2); f.pixel(-1, 3, 3); }},
    {"hline", true, [](FrameBuffer332& f) { f.hline(-20, 10, 50, 1); f.hline(470, 11, 50, 1); }},
    {"vline", true, [](FrameBuffer332& f) { f.vline(33, -5, 40, 1); }},
    {"fillRect", true, [](FrameBuffer332& f) { f.fillRect(100, 300, 50, 50, 1); }},
    {"rect", true, [](FrameBuffer332& f) { f.rect(10, 10, 30, 20, 1); }},
    {"line", true, [](FrameBuffer332& f) { f.line(-50, 300, 600, -20, 1); f.line(3, 3, 3, 3, 2); }},
    {"dashed", true, [](FrameBuffer332& f) { f.dashedLine(20, 300, 460, 40, 1, 6, 4, 3); f.dashedLine(200, 0, 201, 319, 1, 2, 9, 0); }},
    {"circle", true, [](FrameBuffer332& f) { f.circle(240, 160, 100, 1); f.circle(0, 0, 30, 1); }},
    {"fillCircle", true, [](FrameBuffer332& f) { f.fillCircle(470, 310, 25, 1); }},
    {"arc", true, [](FrameBuffer332& f) { f.arc(240, 160, 90, 200, 340, 3, 1); f.arc(100, 100, 40, 300, 60, 5, 1); }},
    {"text", false, [](FrameBuffer332& f) { f.text(-3, 2, "Hi _|", 1, kFont6x8Id); f.text(400, 310, "Wg", 1, kFont8x16Id); }},
    {"glow", true, [](FrameBuffer332& f) { f.glowDot(100, 100, fbpal::kCyan); f.glowDot(2, 317, fbpal::kWhite); f.glowDot(104, 102, fbpal::kViolet); }},
  };
  for (const Case& c : cases) {
    fb.clear(0);
    fb.takePushSpans(sink);                       // start with nothing tracked
    CHECK(fb.pushSpans().rows() == 0 && fb.objectSpans().rows() == 0);
    c.draw(fb);
    const bool ok = spansMatch(fb, fb.pushSpans(), c.tight) && sameSpans(fb.pushSpans(), fb.objectSpans());
    if (!ok) std::printf("case %s\n", c.name);
    CHECK(ok);
    // Handing the frame to the pusher resets the push spans only.
    RowSpans taken; taken.reset();
    const RowSpans objects = fb.objectSpans();
    fb.takePushSpans(taken);
    CHECK(sameSpans(taken, objects));
    CHECK(fb.pushSpans().rows() == 0);
    CHECK(sameSpans(fb.objectSpans(), objects));
    // Erase restores the background exactly there and marks it for the push.
    std::vector<uint8_t> bg(W * H);
    for (int i = 0; i < W * H; i++) bg[i] = (uint8_t)(i * 7 + 3);
    std::vector<uint8_t> before(px);
    fb.eraseTouched(bg.data());
    bool restored = true;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
      const bool inSpan = objects.has(y) && x >= objects.x0[y] && x <= objects.x1[y];
      if (px[y * W + x] != (inSpan ? bg[y * W + x] : before[y * W + x])) restored = false;
    }
    CHECK(restored);
    CHECK(sameSpans(fb.pushSpans(), objects));
    CHECK(fb.objectSpans().rows() == 0);
    fb.eraseTouched(nullptr, 9);                  // nothing left to erase
    CHECK(sameSpans(fb.pushSpans(), objects));
  }
  // Whole-frame writers mark everything; clear/copy reset the object spans.
  fb.takePushSpans(sink);
  fb.glowDot(50, 50, 1);
  fb.clear(3);
  CHECK(fb.pushSpans().rows() == H && fb.objectSpans().rows() == 0);
  CHECK(fb.pushSpans().x0[0] == 0 && fb.pushSpans().x1[H - 1] == W - 1);
  fb.takePushSpans(sink);
  std::vector<uint8_t> src(W * H, 4);
  fb.copyFrom(src.data());
  CHECK(px == src && fb.pushSpans().rows() == H && fb.objectSpans().rows() == 0);
  fb.takePushSpans(sink);
  fb.markAll();
  CHECK(fb.pushSpans().rows() == H);
  fb.takePushSpans(sink);
  CHECK(fb.rawPixels() == px.data() && fb.pushSpans().rows() == H);
  if (failures) return 1;
  std::printf("PASS track\n");
  return 0;
}

static uint32_t rng_state = 1;
static int rnd(int n) { rng_state = rng_state * 1664525u + 1013904223u; return (int)((rng_state >> 8) % (uint32_t)n); }

// Double-buffered page with a background, erase-and-redraw objects, idle
// stretches, full-screen changes, raw writes + markAll and an invalidate:
// after every planned push the simulated glass must equal the frame.
static int scenario_planner() {
  std::vector<uint8_t> bg(W * H), A(W * H), B(W * H), glass(W * H, 0xEE);
  FrameBuffer332 bgf(bg.data(), W, H);
  bgf.clear(fbpal::kBlack);
  for (int x = 40; x < W; x += 40) bgf.vline(x, 0, H, fbpal::kNavy);
  bgf.circle(240, 160, 120, fbpal::kGrid);
  FrameBuffer332 fa(A.data(), W, H), fbb(B.data(), W, H);
  fa.copyFrom(bg.data());
  fbb.copyFrom(bg.data());
  std::vector<uint8_t> shadow(W * H, 0x55);
  PushPlanner pl(shadow.data(), W, H);
  SpanBands bands;
  PushPlanner::Stats st;
  int ox[6] = {10, 100, 200, 300, 400, 470}, oy[6] = {5, 50, 100, 150, 200, 315};
  int idleQuiet = 0;
  for (int frame = 0; frame < 600; frame++) {
    FrameBuffer332& cur = (frame & 1) ? fbb : fa;
    const bool idle = frame >= 300 && frame < 340;
    if (!idle) {
      if (frame % 97 == 50) {
        cur.clear((uint8_t)rnd(256));
      } else if (frame % 97 == 52 || frame % 97 == 53) {
        cur.copyFrom(bg.data());
      } else if (frame % 61 == 7) {
        uint8_t* raw = cur.rawPixels();             // marks the whole frame
        for (int i = 0; i < 500; i++) raw[rnd(W * H)] = (uint8_t)rnd(256);
      } else {
        cur.eraseTouched(bg.data());
        for (int k = 0; k < 6; k++) {
          ox[k] += rnd(9) - 4; oy[k] += rnd(9) - 4;
          cur.glowDot(ox[k], oy[k], (uint8_t)(0x20 + k * 37));
        }
        cur.dashedLine(ox[0], oy[0], ox[1], oy[1], fbpal::kCyan, 5, 3, frame);
        cur.text(ox[2], oy[2], "NODE", fbpal::kAmber, kFont6x8Id);
        if (frame % 5 == 0) cur.fillCircle(ox[3], oy[3], 15, fbpal::kMagenta);
      }
    } else if (frame == 300 || frame == 301) {
      // Settle: both buffers show the same scene before the idle stretch.
      cur.copyFrom(bg.data());
    }
    if (frame == 450) pl.invalidate();
    pl.take(cur);
    pl.plan(cur, bands, &st);
    CHECK(bandsWellFormed(bands));
    applyBands(cur, bands, glass);
    const bool same = std::memcmp(glass.data(), cur.pixels(), W * H) == 0;
    if (!same) std::printf("frame %d: glass differs\n", frame);
    CHECK(same);
    if (idle && frame >= 304) { CHECK(bands.count == 0); idleQuiet++; }
    if (failures > 5) break;
  }
  CHECK(idleQuiet > 30);
  // Dropped submit (FramePusher timed out waiting for the previous push):
  // the sequence A, B, A (dropped), B, A, B ... breaks the A/B alternation
  // the spans rely on; FramePusher then invalidates, and the glass must
  // still match every planned frame.
  {
    const bool dropAt[] = {false, false, true, false, false, false, false, true, true, false, false, false};
    const int nSteps = (int)(sizeof(dropAt) / sizeof(dropAt[0]));
    for (int step = 0; step < nSteps; step++) {
      FrameBuffer332& cur = (step & 1) ? fbb : fa;
      cur.eraseTouched(bg.data());
      for (int k = 0; k < 6; k++) {
        ox[k] += rnd(21) - 10; oy[k] += rnd(21) - 10;
        cur.glowDot(ox[k], oy[k], (uint8_t)(0x40 + k * 31));
      }
      cur.dashedLine(ox[0], oy[0], ox[4], oy[4], fbpal::kGreen, 4, 4, step);
      if (dropAt[step]) {
        pl.invalidate();                          // what FramePusher::submit does
        continue;                                 // spans stay in the frame
      }
      pl.take(cur);
      pl.plan(cur, bands, &st);
      CHECK(bandsWellFormed(bands));
      applyBands(cur, bands, glass);
      const bool same = std::memcmp(glass.data(), cur.pixels(), W * H) == 0;
      if (!same) std::printf("drop step %d: glass differs\n", step);
      CHECK(same);
    }
  }
  // Single-buffered use: identical redraw sends nothing.
  FrameBuffer332 one(A.data(), W, H);
  one.copyFrom(bg.data());
  std::vector<uint8_t> shadow1(W * H);
  PushPlanner p1(shadow1.data(), W, H);
  p1.take(one); p1.plan(one, bands);
  CHECK(bands.count == 1 && spanBandBytes(bands) == (long)W * H * 2);
  p1.take(one); p1.plan(one, bands, &st);         // settles: nothing changed
  CHECK(bands.count == 0 && st.candidateRows == H && st.skippedRows == H);
  one.glowDot(100, 100, fbpal::kGreen);
  p1.take(one); p1.plan(one, bands);
  CHECK(bands.count == 1 && bands.y0[0] == 92 && bands.n[0] == 16 && bands.x0[0] == 94 && bands.x1[0] == 106);
  one.eraseTouched(bg.data()); one.glowDot(100, 100, fbpal::kGreen);
  p1.take(one); p1.plan(one, bands, &st);
  CHECK(bands.count == 0 && st.candidateRows == 13 && st.skippedRows == 13);
  p1.take(one); p1.plan(one, bands, &st);
  CHECK(bands.count == 0 && st.candidateRows == 0);
  // Windows are trimmed to the columns that really changed.
  one.pixel(300, 200, 0x77);
  one.pixel(310, 201, 0x77);
  p1.take(one); p1.plan(one, bands, &st);
  CHECK(bands.count == 1 && bands.y0[0] == 200 && bands.n[0] == 4 && bands.x0[0] == 300 && bands.x1[0] == 310);
  CHECK(shadow1[200 * W + 300] == 0x77 && shadow1[201 * W + 310] == 0x77);
  // planSpanBands merges near blocks and keeps far ones apart.
  RowSpans r; r.reset();
  r.add(10, 100, 110); r.add(13, 100, 110);
  planSpanBands(r, H, kBandAlign, kWindowCostBytes, bands);
  CHECK(bands.count == 1 && bands.y0[0] == 8 && bands.n[0] == 8 && bands.x0[0] == 100 && bands.x1[0] == 110);
  r.add(300, 0, 479);
  planSpanBands(r, H, kBandAlign, kWindowCostBytes, bands);
  CHECK(bands.count == 2 && bands.y0[1] == 300 && bands.n[1] == 4);
  if (failures) return 1;
  std::printf("PASS planner\n");
  return 0;
}

// Host-measured push volume and per-frame work for typical changes (planner
// path: tracked spans + hash backstop + x-trimmed windows).
struct Work { long render; long hashed; long pushed; long windows; int frames; };

static void account(Work& w, const PushPlanner::Stats& st, const SpanBands& b, long renderBytes) {
  w.render += renderBytes; w.hashed += st.comparedBytes; w.pushed += spanBandBytes(b); w.windows += b.count; w.frames++;
}

static long spanBytes(const RowSpans& s) {
  long t = 0;
  for (int y = 0; y < kMaxFbRows; y++) if (s.has(y)) t += s.x1[y] - s.x0[y] + 1;
  return t;
}

static void report(const char* name, const Work& w) {
  const int n = w.frames ? w.frames : 1;
  std::printf("VOL %s bytes=%ld windows=%.2f compared_bytes=%ld render_bytes=%ld\n", name, w.pushed / n,
              (double)w.windows / n, w.hashed / n, w.render / n);
}

typedef void (*DrawFn)(FrameBuffer332&, int);

struct Rig {
  std::vector<uint8_t> bg, A, B, shadow;
  FrameBuffer332 fa, fbb;
  PushPlanner pl;
  SpanBands b;
  PushPlanner::Stats st;
  Rig() : bg(W * H, 0), A(W * H), B(W * H), shadow(W * H), fa(A.data(), W, H), fbb(B.data(), W, H),
          pl(shadow.data(), W, H) {
    FrameBuffer332 bgf(bg.data(), W, H);
    for (int x = 40; x < W; x += 40) bgf.vline(x, 24, H - 24, fbpal::kNavy);
    for (int y = 40; y < H; y += 40) bgf.hline(0, y, W, fbpal::kNavy);
    bgf.circle(240, 172, 120, fbpal::kGrid);
  }
  void start() {
    fa.copyFrom(bg.data()); fbb.copyFrom(bg.data()); pl.invalidate();
    pl.take(fa); pl.plan(fa, b);
  }
  // Erase + draw into the next buffer, take, plan (double-buffered).
  void step(int i, Work& w, DrawFn draw) {
    FrameBuffer332& f = (i & 1) ? fbb : fa;
    const long erased = spanBytes(f.objectSpans());
    f.eraseTouched(bg.data());
    draw(f, i);
    const long drawn = spanBytes(f.objectSpans());
    pl.take(f); pl.plan(f, b, &st);
    if (i > 2) account(w, st, b, erased + drawn);
  }
};

static void drawDemoAnim(FrameBuffer332& f, int i) {
  int px[8], py[8];
  for (int k = 0; k < 8; k++) {
    px[k] = 20 + (i * (3 + k) + k * 53) % 440;
    py[k] = 40 + (i * (2 + k) + k * 37) % 260;
    f.glowDot(px[k], py[k], (uint8_t)(0x3B + k * 29));
  }
  f.dashedLine(px[0], py[0], px[1], py[1], fbpal::kCyan, 6, 4, i);
  f.text(8, 4, "FREENOVE HEAD", fbpal::kCyan, kFont8x16Id);
  f.text(6, 310, (i / 30) & 1 ? "anim 31 FPS 12.0 KB/F" : "anim 30 FPS 11.9 KB/F", fbpal::kSky, kFont6x8Id);
}

static int scenario_volumes() {
  Rig rig;
  Work w;
  rig.start(); w = Work{0, 0, 0, 0, 0};
  for (int i = 0; i < 40; i++) rig.step(i, w, [](FrameBuffer332& f, int i) { f.pixel(240, 100, (uint8_t)(i * 3 + 1)); });
  report("one_pixel", w);

  rig.start(); w = Work{0, 0, 0, 0, 0};
  for (int i = 0; i < 60; i++) rig.step(i, w, [](FrameBuffer332& f, int i) { f.fillCircle(100 + 2 * i, 80 + 2 * i, 10, fbpal::kCyan); });
  report("dot20_moving", w);

  rig.start(); w = Work{0, 0, 0, 0, 0};
  for (int i = 0; i < 60; i++) rig.step(i, w, [](FrameBuffer332& f, int i) { f.glowDot(50 + i, 157, fbpal::kGreen); });
  report("glow_moving_h", w);

  // The demo's anim page: 8 bouncing glow dots, a marching dashed link
  // between two of them, HUD text.
  rig.start(); w = Work{0, 0, 0, 0, 0};
  for (int i = 0; i < 120; i++) rig.step(i, w, drawDemoAnim);
  report("demo_anim", w);

  // Fixed diagonal marching line across most of the screen.
  rig.start(); w = Work{0, 0, 0, 0, 0};
  for (int i = 0; i < 60; i++) rig.step(i, w, [](FrameBuffer332& f, int i) { f.dashedLine(20, 300, 460, 40, fbpal::kCyan, 6, 4, i); });
  report("diag_line", w);

  // Idle page: the same objects redrawn every frame.
  rig.start(); w = Work{0, 0, 0, 0, 0};
  for (int i = 0; i < 60; i++) rig.step(i, w, [](FrameBuffer332& f, int) {
    for (int k = 0; k < 8; k++) f.glowDot(30 + k * 50, 60 + k * 25, fbpal::kGreen);
    f.text(6, 310, "idle", fbpal::kSky, kFont6x8Id);
  });
  report("idle_redraw", w);

  rig.start(); w = Work{0, 0, 0, 0, 0};
  for (int i = 0; i < 20; i++) rig.step(i, w, [](FrameBuffer332&, int) {});
  report("idle", w);

  rig.start(); w = Work{0, 0, 0, 0, 0};
  for (int i = 0; i < 20; i++) {
    FrameBuffer332& f = (i & 1) ? rig.fbb : rig.fa;
    f.clear((uint8_t)(i * 37 + 1));
    rig.pl.take(f); rig.pl.plan(f, rig.b, &rig.st);
    if (i > 2) account(w, rig.st, rig.b, (long)W * H);
  }
  report("full_change", w);
  std::printf("PASS volumes\n");
  return 0;
}

// Scaled timing model: host time of the demo's anim frame (render, plan)
// relative to the host time of one full-frame row hash, which took 8.9 ms on
// the device. Printed only (host timing is noisy); no assertions.
static int scenario_budget() {
  Rig rig;
  Work w = Work{0, 0, 0, 0, 0};
  rig.start();
  uint32_t last[kMaxFbRows] = {};
  DirtyRows d;
  const int reps = 400;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < reps; i++) diffRows(rig.fa, last, d);
  auto t1 = std::chrono::steady_clock::now();
  const double fullHash = std::chrono::duration<double, std::micro>(t1 - t0).count() / reps;
  double render = 0, plan = 0;
  for (int i = 0; i < reps; i++) {
    FrameBuffer332& f = (i & 1) ? rig.fbb : rig.fa;
    auto a = std::chrono::steady_clock::now();
    f.eraseTouched(rig.bg.data());
    drawDemoAnim(f, i);
    auto b = std::chrono::steady_clock::now();
    rig.pl.take(f); rig.pl.plan(f, rig.b, &rig.st);
    auto c = std::chrono::steady_clock::now();
    render += std::chrono::duration<double, std::micro>(b - a).count();
    plan += std::chrono::duration<double, std::micro>(c - b).count();
    account(w, rig.st, rig.b, 0);
  }
  render /= reps; plan /= reps;
  std::printf("BUDGET host_full_hash_us=%.1f host_render_us=%.1f host_plan_us=%.1f "
              "render_vs_fullhash=%.3f plan_vs_fullhash=%.3f compared_bytes=%ld push_bytes=%ld windows=%.2f\n",
              fullHash, render, plan, render / fullHash, plan / fullHash, w.hashed / w.frames,
              w.pushed / w.frames, (double)w.windows / w.frames);
  std::printf("PASS budget\n");
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const char* s = argv[1];
  if (!std::strcmp(s, "clip")) return scenario_clip();
  if (!std::strcmp(s, "lut")) return scenario_lut();
  if (!std::strcmp(s, "diff")) return scenario_diff();
  if (!std::strcmp(s, "dash")) return scenario_dash();
  if (!std::strcmp(s, "glow")) return scenario_glow();
  if (!std::strcmp(s, "text")) return scenario_text();
  if (!std::strcmp(s, "volumes")) return scenario_volumes();
  if (!std::strcmp(s, "track")) return scenario_track();
  if (!std::strcmp(s, "planner")) return scenario_planner();
  if (!std::strcmp(s, "budget")) return scenario_budget();
  return 2;
}
'''


def ref565(c):
    r3, g3, b2 = (c >> 5) & 7, (c >> 2) & 7, c & 3
    r5 = (r3 << 2) | (r3 >> 1)
    g6 = (g3 << 3) | g3
    b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1)
    return (r5 << 11) | (g6 << 5) | b5


class FrameBuffer332Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            raise unittest.SkipTest('a host C++17 compiler is required')
        cls.compiler = compiler
        sanitize = [] if os.name == 'nt' else [
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        cls.temp = tempfile.TemporaryDirectory(prefix='fb332-')
        work = Path(cls.temp.name)
        cpp = work / 'fb.cpp'
        cpp.write_text(HARNESS, encoding='ascii')
        cls.exe = work / ('fb' + ('.exe' if os.name == 'nt' else ''))
        built = subprocess.run([
            compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize,
            '-I', str(UI), str(cpp), str(UI / 'FrameBuffer332.cpp'), str(UI / 'FbFonts.cpp'),
            '-o', str(cls.exe),
        ], capture_output=True, text=True, timeout=180, cwd=str(ROOT / 'test'))
        if built.returncode != 0:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_scenario(self, name, timeout=120):
        ran = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=timeout)
        self.assertEqual(ran.returncode, 0, ran.stdout[-4000:] + ran.stderr[-4000:])
        self.assertIn('PASS ' + name, ran.stdout)
        return ran.stdout

    def test_lut_matches_formula(self):
        out = self.run_scenario('lut')
        rows = [line.split() for line in out.splitlines() if not line.startswith('PASS')]
        self.assertEqual(len(rows), 256)
        for c, (plain, swapped) in enumerate(rows):
            ref = ref565(c)
            self.assertEqual(int(plain, 16), ref, 'entry %d' % c)
            self.assertEqual(int(swapped, 16), ((ref & 0xFF) << 8) | (ref >> 8), 'entry %d' % c)
        self.assertEqual(int(rows[0][0], 16), 0x0000)
        self.assertEqual(int(rows[255][0], 16), 0xFFFF)

    def test_primitives_clip_safely(self):
        self.run_scenario('clip', timeout=300)

    def test_push_skips_unchanged_rows(self):
        self.run_scenario('diff')

    def test_dashed_line_phase_animates(self):
        self.run_scenario('dash')

    def test_glow_ramp_colours_in_palette(self):
        self.run_scenario('glow')

    def test_text_fonts(self):
        self.run_scenario('text')

    def test_push_volumes(self):
        out = self.run_scenario('volumes')
        vol = {}
        for line in out.splitlines():
            if line.startswith('VOL '):
                parts = line.split()
                vol[parts[1]] = {k: float(v) for k, v in (p.split('=') for p in parts[2:])}
        print('\n' + '\n'.join(l for l in out.splitlines() if l.startswith('VOL ')))
        self.assertEqual(vol['one_pixel']['bytes'], 4 * 1 * 2)       # 4-row block, 1 column
        self.assertLessEqual(vol['one_pixel']['compared_bytes'], 4)
        self.assertEqual(vol['full_change']['bytes'], 480 * 320 * 2)
        self.assertEqual(vol['idle']['bytes'], 0)
        self.assertEqual(vol['idle']['compared_bytes'], 0)
        self.assertEqual(vol['idle_redraw']['bytes'], 0)             # hash backstop
        self.assertLess(vol['dot20_moving']['bytes'], 32 * 32 * 2)   # x-trimmed window
        self.assertLess(vol['glow_moving_h']['bytes'], 16 * 32 * 2)
        self.assertLess(vol['diag_line']['bytes'], 480 * 320 * 2 // 6)
        self.assertLess(vol['demo_anim']['bytes'], 480 * 320 * 2 // 4)
        self.assertLess(vol['demo_anim']['render_bytes'], 480 * 320 // 8)   # erase + draw only

    def test_dirty_rows_tracking(self):
        self.run_scenario('track')

    def test_planner_keeps_glass_in_sync(self):
        self.run_scenario('planner', timeout=300)

    def test_budget_model(self):
        out = self.run_scenario('budget')
        print('\n' + '\n'.join(l for l in out.splitlines() if l.startswith('BUDGET')))

    def test_engine_is_pure_cxx11(self):
        for name in ('FrameBuffer332.cpp', 'FrameBuffer332.h', 'FbFonts.cpp', 'FbFonts.h'):
            source = (UI / name).read_text(encoding='utf-8')
            self.assertNotIn('Arduino.h', source)
            self.assertNotIn('esp_', source)
        for name in ('FrameBuffer332.cpp', 'FbFonts.cpp'):
            checked = subprocess.run([
                self.compiler, '-std=c++11', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
                '-I', str(UI), str(UI / name),
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)


if __name__ == '__main__':
    unittest.main()
