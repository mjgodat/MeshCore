import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "FreenoveGeometry.h"
using namespace freenove;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static bool samePoint(Point p, int x, int y) { return p.x == x && p.y == y; }

int main() {
  std::printf("section landscape_to_portrait_corners\n");
  {
    CHECK(kLandscapeW == 480 && kLandscapeH == 320 && kPanelW == 320 && kPanelH == 480);
    // Default orientation: landscape = panel rotated 90 degrees clockwise
    // (landscape top edge runs down the panel's right-hand column).
    CHECK(samePoint(toPanel(0, 0, false), 319, 0));
    CHECK(samePoint(toPanel(479, 0, false), 319, 479));
    CHECK(samePoint(toPanel(0, 319, false), 0, 0));
    CHECK(samePoint(toPanel(479, 319, false), 0, 479));
    // Flipped (FREENOVE_LANDSCAPE_FLIP): rotated 180 degrees from the default.
    CHECK(samePoint(toPanel(0, 0, true), 0, 479));
    CHECK(samePoint(toPanel(479, 0, true), 0, 0));
    CHECK(samePoint(toPanel(0, 319, true), 319, 479));
    CHECK(samePoint(toPanel(479, 319, true), 319, 0));
    // Bijection: every landscape pixel lands on a distinct in-range panel pixel
    // and maps back.
    for (int f = 0; f < 2; f++) {
      std::vector<uint8_t> seen(kPanelW * kPanelH, 0);
      for (int ly = 0; ly < kLandscapeH; ly++)
        for (int lx = 0; lx < kLandscapeW; lx++) {
          Point p = toPanel(lx, ly, f != 0);
          CHECK(p.x >= 0 && p.x < kPanelW && p.y >= 0 && p.y < kPanelH);
          if (p.x < 0 || p.x >= kPanelW || p.y < 0 || p.y >= kPanelH) continue;
          CHECK(seen[p.y * kPanelW + p.x] == 0);
          seen[p.y * kPanelW + p.x] = 1;
          Point back = panelToLandscape(p.x, p.y, f != 0);
          CHECK(back.x == lx && back.y == ly);
        }
    }
  }
  std::printf("section touch_coords_landscape\n");
  {
    int x = -1, y = -1;
    // Touch reports panel (portrait) coordinates; corners land on the
    // landscape corners that share the panel corner.
    CHECK(touchToLandscape(319, 0, 320, 480, false, 0, x, y) && x == 0 && y == 0);
    CHECK(touchToLandscape(319, 479, 320, 480, false, 0, x, y) && x == 479 && y == 0);
    CHECK(touchToLandscape(0, 0, 320, 480, false, 0, x, y) && x == 0 && y == 319);
    CHECK(touchToLandscape(0, 479, 320, 480, false, 0, x, y) && x == 479 && y == 319);
    CHECK(touchToLandscape(160, 240, 320, 480, false, 0, x, y) && x == 240 && y == 159);
    // Flip applies to touch the same way it applies to pixels.
    CHECK(touchToLandscape(0, 479, 320, 480, true, 0, x, y) && x == 0 && y == 0);
    CHECK(touchToLandscape(319, 0, 320, 480, true, 0, x, y) && x == 479 && y == 319);
    // Every panel pixel round-trips through toPanel.
    for (int py = 0; py < kPanelH; py += 7)
      for (int px = 0; px < kPanelW; px += 5)
        for (int f = 0; f < 2; f++) {
          CHECK(touchToLandscape(px, py, kPanelW, kPanelH, f != 0, 0, x, y));
          Point p = toPanel(x, y, f != 0);
          CHECK(p.x == px && p.y == py);
        }
    // Reported resolution other than the panel's scales; 0 means "unknown, use
    // the panel's"; out-of-range readings clamp to the screen.
    CHECK(touchToLandscape(639, 959, 640, 960, false, 0, x, y) && x == 479 && y == 0);
    CHECK(touchToLandscape(319, 0, 0, 0, false, 0, x, y) && x == 0 && y == 0);
    CHECK(touchToLandscape(400, 600, 320, 480, false, 0, x, y) && x == 479 && y == 0);
    CHECK(!touchToLandscape(-1, 10, 320, 480, false, 0, x, y));
    CHECK(!touchToLandscape(10, -1, 320, 480, false, 0, x, y));
    // Mirror bits act in panel space before the rotation (bit0 = X, bit1 = Y).
    CHECK(touchToLandscape(0, 0, 320, 480, false, kTouchMirrorX, x, y) && x == 0 && y == 0);
    CHECK(touchToLandscape(319, 479, 320, 480, false, kTouchMirrorY, x, y) && x == 0 && y == 0);
    CHECK(touchToLandscape(0, 479, 320, 480, false, kTouchMirrorX | kTouchMirrorY, x, y) && x == 0 && y == 0);
  }
  std::printf("section row_band_alignment\n");
  {
    int y0 = -1, n = -1;
    CHECK(alignRows(0, 320, y0, n) && y0 == 0 && n == 320);
    CHECK(alignRows(5, 1, y0, n) && y0 == 4 && n == 4);
    CHECK(alignRows(3, 2, y0, n) && y0 == 0 && n == 8);
    CHECK(alignRows(317, 10, y0, n) && y0 == 316 && n == 4);   // clamped to the screen
    CHECK(alignRows(-3, 6, y0, n) && y0 == 0 && n == 4);
    CHECK(!alignRows(0, 0, y0, n));
    CHECK(!alignRows(320, 4, y0, n));
    CHECK(!alignRows(-10, 5, y0, n));
    // ST77922 needs the column window to start on a multiple of 4 and span a
    // multiple of 4 (x_start % 4 == 0, (x_end + 1) % 4 == 0).
    for (int f = 0; f < 2; f++)
      for (int a = 0; a < kLandscapeH; a += 4)
        for (int b = 4; a + b <= kLandscapeH; b += 4) {
          ColumnWindow w = bandColumns(a, b, f != 0);
          CHECK(w.c0 % 4 == 0 && (w.c1 + 1) % 4 == 0 && w.c1 - w.c0 + 1 == b);
          CHECK(w.c0 >= 0 && w.c1 < kPanelW);
        }
    ColumnWindow w = bandColumns(0, 4, false);
    CHECK(w.c0 == 316 && w.c1 == 319);
    w = bandColumns(0, 4, true);
    CHECK(w.c0 == 0 && w.c1 == 3);
  }
  std::printf("section transpose_band\n");
  {
    // Fill a landscape frame with a unique value per pixel, transpose a band,
    // and check every output pixel against toPanel.
    std::vector<uint16_t> frame(kLandscapeW * kLandscapeH);
    for (int i = 0; i < kLandscapeW * kLandscapeH; i++) frame[i] = (uint16_t)(i * 2654435761u >> 16);
    for (int f = 0; f < 2; f++)
      for (int sw = 0; sw < 2; sw++) {
        const int y0 = 12, rows = 8;
        ColumnWindow w = bandColumns(y0, rows, f != 0);
        const int py0 = 37, npy = 50;
        std::vector<uint16_t> out(rows * npy + 1, 0xBEEF);
        transposeBand(&frame[y0 * kLandscapeW], rows, py0, npy, f != 0, sw != 0, out.data());
        CHECK(out[rows * npy] == 0xBEEF);  // no overrun
        for (int r = 0; r < npy; r++)
          for (int c = 0; c < rows; c++) {
            Point l = panelToLandscape(w.c0 + c, py0 + r, f != 0);
            uint16_t want = frame[l.y * kLandscapeW + l.x];
            if (sw) want = swap16(want);
            CHECK(out[r * rows + c] == want);
          }
      }
    CHECK(swap16(0x1234) == 0x3412);
  }
  std::printf("section test_pattern_layout\n");
  {
    // 8 vertical bars, 60 px each: white yellow cyan green magenta red blue black.
    const uint16_t bars[8] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000};
    for (int b = 0; b < 8; b++) {
      CHECK(testPatternColor(b * 60 + 30, 160) == bars[b]);
      CHECK(testPatternColor(b * 60, 160) == bars[b]);
      CHECK(testPatternColor(b * 60 + 59, 160) == bars[b]);
    }
    // Corner markers (24x24, 1 px dark-grey border): TL orange, TR teal,
    // BL purple, BR grey -- none equal to the bar beneath them.
    CHECK(testPatternColor(5, 5) == kMarkerTL);
    CHECK(testPatternColor(474, 5) == kMarkerTR);
    CHECK(testPatternColor(5, 314) == kMarkerBL);
    CHECK(testPatternColor(474, 314) == kMarkerBR);
    CHECK(testPatternColor(0, 0) == kMarkerBorder);
    CHECK(testPatternColor(479, 319) == kMarkerBorder);
    CHECK(testPatternColor(24, 5) == bars[0]);
    const uint16_t markers[4] = {kMarkerTL, kMarkerTR, kMarkerBL, kMarkerBR};
    for (int i = 0; i < 4; i++) {
      for (int b = 0; b < 8; b++) CHECK(markers[i] != bars[b]);
      for (int j = i + 1; j < 4; j++) CHECK(markers[i] != markers[j]);
    }
  }
  std::printf("section touch_hold_over\n");
  {
    // The controller only flags a *new* report (0x0010 bit 3); a held finger
    // has gaps between reports. Stay down until kTouchHoldMs pass without one.
    CHECK(kTouchHoldMs >= 80 && kTouchHoldMs <= 100);
    TouchHold h;
    int x = -1, y = -1;
    CHECK(!h.update(false, 0, 0, 1000, x, y));            // idle: up
    CHECK(h.update(true, 10, 20, 1000, x, y) && x == 10 && y == 20);
    CHECK(h.update(false, 0, 0, 1030, x, y) && x == 10 && y == 20);  // gap: still down
    CHECK(h.update(true, 12, 22, 1060, x, y) && x == 12 && y == 22); // moves
    CHECK(h.update(false, 0, 0, 1060 + kTouchHoldMs - 1, x, y) && x == 12 && y == 22);
    CHECK(!h.update(false, 0, 0, 1060 + kTouchHoldMs, x, y));        // released
    CHECK(!h.update(false, 0, 0, 1060 + kTouchHoldMs + 5, x, y));    // stays up
    // A long hold with reports every 50 ms never flickers up.
    TouchHold g;
    for (uint32_t t = 0; t < 5000; t += 10) {
      const bool report = (t % 50) == 0;
      CHECK(g.update(report, 100, 100, t, x, y));
    }
    // millis() wrap-around does not end a hold early or freeze one.
    TouchHold w;
    CHECK(w.update(true, 1, 2, 0xFFFFFFF0u, x, y));
    CHECK(w.update(false, 0, 0, 0x00000010u, x, y));                 // 32 ms later
    CHECK(!w.update(false, 0, 0, 0xFFFFFFF0u + kTouchHoldMs, x, y));
    CHECK(!w.down());
  }
  std::printf("section touch_warmup_and_retry\n");
  {
    // No polls for kTouchWarmupMs after reset; probes/resolution reads retry
    // every kTouchRetryMs until they succeed.
    CHECK(kTouchWarmupMs >= 900 && kTouchWarmupMs <= 1200);
    CHECK(!touchWarmedUp(5000, 5000));
    CHECK(!touchWarmedUp(5000 + kTouchWarmupMs - 1, 5000));
    CHECK(touchWarmedUp(5000 + kTouchWarmupMs, 5000));
    CHECK(!touchWarmedUp(10, 0xFFFFFF00u));                           // wrap: 266 ms
    CHECK(touchWarmedUp(0xFFFFFF00u + kTouchWarmupMs, 0xFFFFFF00u));
    TouchRetry r;
    CHECK(r.due(100));                    // first attempt immediately
    r.attempted(100);
    CHECK(!r.due(100 + kTouchRetryMs - 1));
    CHECK(r.due(100 + kTouchRetryMs));
  }
  std::printf("section rgb332_to_565\n");
  {
    CHECK(rgb332To565(0x00) == 0x0000);
    CHECK(rgb332To565(0xFF) == 0xFFFF);
    CHECK(rgb332To565(0xE0) == 0xF800);
    CHECK(rgb332To565(0x1C) == 0x07E0);
    CHECK(rgb332To565(0x03) == 0x001F);
  }
  std::printf("section transpose_band_332\n");
  {
    // Straight from the RGB332 framebuffer through the LUT == expand to
    // RGB565 first, then transposeBand().
    uint16_t lut[256];
    for (int c = 0; c < 256; c++) lut[c] = swap16(rgb332To565((uint8_t)c));
    uint32_t seed = 12345;
    std::vector<uint8_t> fb(kLandscapeW * kLandscapeH);
    for (auto& p : fb) { seed = seed * 1103515245u + 12345u; p = (uint8_t)(seed >> 16); }
    const int bands[][2] = {{0, 4}, {100, 4}, {96, 12}, {316, 4}, {0, 320}, {200, 32}};
    for (const auto& b : bands) {
      const int y0 = b[0], rows = b[1];
      std::vector<uint16_t> wide(rows * kLandscapeW);
      for (int i = 0; i < rows * kLandscapeW; i++) wide[i] = rgb332To565(fb[y0 * kLandscapeW + i]);
      for (int flip = 0; flip < 2; flip++) {
        const int spans[][2] = {{0, 15}, {0, 480}, {465, 15}, {123, 77}};
        for (const auto& s : spans) {
          const int py0 = s[0], npy = s[1];
          std::vector<uint16_t> want(npy * rows + 1, 0xBEEF), got(npy * rows + 1, 0xBEEF);
          transposeBand(wide.data(), rows, py0, npy, flip != 0, true, want.data());
          transposeBand332(&fb[y0 * kLandscapeW], rows, py0, npy, flip != 0, lut, got.data());
          CHECK(got == want);
          CHECK(got[npy * rows] == 0xBEEF);           // writes stay in npy * rows
        }
      }
    }
  }
  if (failures) return 1;
  std::printf("PASS freenove geometry\n");
  return 0;
}
'''


class FreenoveGeometryTest(unittest.TestCase):
    def test_freenove_geometry(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            self.skipTest('a host C++17 compiler is required')
        sanitize = [] if os.name == 'nt' else [
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        with tempfile.TemporaryDirectory(prefix='freenove-geometry-') as directory:
            work = Path(directory)
            cpp = work / 'geometry.cpp'
            cpp.write_text(HARNESS, encoding='ascii')
            binary = work / ('geometry' + ('.exe' if os.name == 'nt' else ''))
            built = subprocess.run([
                compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize,
                '-I', str(ROOT / 'src'), '-I', str(ROOT / 'test'),
                '-I', str(ROOT / 'variants' / 'freenove_s3_35_head'),
                str(cpp), '-o', str(binary),
            ], capture_output=True, text=True, timeout=120, cwd=str(ROOT / 'test'))
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('PASS freenove geometry', ran.stdout)


if __name__ == '__main__':
    unittest.main()
