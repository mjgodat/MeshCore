"""FrameBuffer565 (ui-freenove): RGB565 drawing with anti-aliasing, AA text,
clipping, change tracking and the RGB565 push planner. Host harness (g++,
ASan/UBSan off Windows)."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / 'examples' / 'companion_radio' / 'ui-freenove'

HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "FrameBuffer565.h"
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static const uint16_t kBlack = 0, kWhite = 0xFFFF, kRed = rgb565(255, 0, 0);

int main() {
  std::vector<uint16_t> px(480 * 320), bg(480 * 320);
  FrameBuffer565 fb(px.data());
  // Colour maths.
  CHECK(rgb565(255, 255, 255) == 0xFFFF && rgb565(0, 0, 0) == 0 && kRed == 0xF800);
  CHECK(FrameBuffer565::mix(kBlack, kWhite, 0) == kBlack && FrameBuffer565::mix(kBlack, kWhite, 255) == kWhite);
  const uint16_t half = FrameBuffer565::mix(kBlack, kWhite, 128);
  CHECK(((half >> 11) & 31) == 16 && ((half >> 5) & 63) == 32 && (half & 31) == 16);
  CHECK(FrameBuffer565::mix(kWhite, kBlack, 128) == FrameBuffer565::mix(kBlack, kWhite, 127));
  // Dither: a level between two 565 steps averages to it over a 4x4 tile.
  {
    long sum = 0;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) sum += (FrameBuffer565::dither(0, 0, 255.0f / 31.0f * 0.5f, x, y) & 31);
    CHECK(sum >= 6 && sum <= 10);   // ~half the pixels at 1, the rest 0
    CHECK(FrameBuffer565::dither(255, 255, 255, 3, 3) == 0xFFFF && FrameBuffer565::dither(0, 0, 0, 0, 0) == 0);
  }
  // Clipping and spans.
  fb.clear(kBlack);
  RowSpans sp; sp.reset();
  fb.takePushSpans(sp);
  CHECK(sp.rows() == 320);
  fb.fillRect(-10, -10, 20, 20, kRed);
  CHECK(fb.get(0, 0) == kRed && fb.get(9, 9) == kRed && fb.get(10, 10) == kBlack);
  fb.setClip(100, 100, 10, 10);
  fb.fillRect(0, 0, 480, 320, kWhite);
  CHECK(fb.get(100, 100) == kWhite && fb.get(109, 109) == kWhite && fb.get(110, 110) == kBlack && fb.get(99, 100) == kBlack);
  fb.text(90, 95, "HELLO", kWhite, kFontTitle);
  fb.resetClip();
  fb.line(-100000, -50000, 100000, 50000, kWhite);   // absurd endpoints: safe
  fb.aaLine(-1e6f, 5.0f, 1e6f, 5.0f, 2.0f, kWhite);
  fb.aaRing(-5000.0f, 0.0f, 10.0f, 2.0f, kWhite);
  fb.text(-1000, -1000, "X", kWhite, kFontBig);
  fb.text(470, 310, "WWWWWW", kWhite, kFontBig);
  // eraseTouched restores only what objects drew.
  for (auto& p : bg) p = rgb565(10, 20, 30);
  FrameBuffer565 f2(px.data());
  f2.copyFrom(bg.data());
  f2.aaLine(10.0f, 10.0f, 200.0f, 150.0f, 2.0f, kWhite);
  f2.aaDisc(300.0f, 200.0f, 8.0f, kRed);
  int changed = 0;
  for (size_t i = 0; i < px.size(); i++) changed += px[i] != bg[i];
  CHECK(changed > 300);
  f2.eraseTouched(bg.data());
  changed = 0;
  for (size_t i = 0; i < px.size(); i++) changed += px[i] != bg[i];
  CHECK(changed == 0);
  // AA line: full coverage on its axis, partial at the rim, symmetric.
  f2.copyFrom(bg.data());
  f2.clear(kBlack);
  f2.aaLine(50.0f, 50.0f, 150.0f, 50.0f, 1.0f, kWhite);
  CHECK(f2.get(100, 50) == kWhite && f2.get(100, 48) == kBlack && f2.get(100, 52) == kBlack);
  f2.aaLine(50.0f, 100.5f, 150.0f, 100.5f, 1.0f, kWhite);   // between rows: two half rows
  CHECK(f2.get(100, 100) != kBlack && f2.get(100, 100) != kWhite && f2.get(100, 100) == f2.get(100, 101));
  // AA disc: centre solid, edge partial, outside untouched; symmetric.
  f2.clear(kBlack);
  f2.aaDisc(240.0f, 160.0f, 10.0f, kWhite);
  CHECK(f2.get(240, 160) == kWhite && f2.get(240, 172) == kBlack);
  CHECK(f2.get(230, 160) == f2.get(250, 160) && f2.get(240, 150) == f2.get(240, 170));
  CHECK(f2.get(240, 150) != kWhite && f2.get(240, 150) != kBlack);
  // Arc: only its sector (0..90 = +x to +y, clockwise on screen).
  f2.clear(kBlack);
  f2.aaArc(240.0f, 160.0f, 30.0f, 4.0f, 0.0f, 90.0f, kWhite);
  CHECK(f2.get(240 + 28, 160 + 3) == kWhite || f2.get(240 + 27, 160 + 5) == kWhite);   // lower right
  CHECK(f2.get(240 - 28, 160) == kBlack && f2.get(240, 160 - 28) == kBlack);
  // Round rect: straight edges solid, corner pixel blank, symmetric corners.
  f2.clear(kBlack);
  f2.fillRoundRect(100, 100, 60, 40, 6, kWhite);
  CHECK(f2.get(130, 100) == kWhite && f2.get(100, 120) == kWhite && f2.get(159, 120) == kWhite && f2.get(130, 139) == kWhite);
  CHECK(f2.get(100, 100) == kBlack && f2.get(159, 139) == kBlack);
  CHECK(f2.get(101, 101) == f2.get(158, 138) && f2.get(101, 101) == f2.get(158, 101));
  CHECK(f2.get(99, 120) == kBlack && f2.get(130, 140) == kBlack);
  f2.clear(kBlack);
  f2.strokeRoundRect(100, 100, 60, 40, 6, kWhite);
  CHECK(f2.get(130, 100) == kWhite && f2.get(130, 101) == kBlack && f2.get(130, 120) == kBlack);
  // Text: width, anchors, AA pixels in between.
  CHECK(FrameBuffer565::textWidth("", kFontBody) == 0);
  const int w = FrameBuffer565::textWidth("MESH", kFontTitle);
  CHECK(w > 25 && w < 60);
  CHECK(FrameBuffer565::textWidth("MESH", kFontTitle, 2) == w + 6);
  CHECK(FrameBuffer565::textWidth("0123", kFontDigits) > FrameBuffer565::textWidth("0123", kFontBody));
  CHECK(FrameBuffer565::textWidth("A", kFontDigits) == 0);   // digits-only font
  f2.clear(kBlack);
  const int end = f2.text(10, 10, "MESH", kWhite, kFontTitle);
  CHECK(end == 10 + w);
  int full = 0, part = 0, outside = 0;
  for (int y = 0; y < 40; y++) for (int x = 0; x < 80; x++) {
    const uint16_t c = f2.get(x, y);
    if (((c >> 11) & 31) >= 26) full++; else if (c != kBlack) part++;   // solid (>= 85%) vs edge
    if (c != kBlack && (x < 9 || x > 10 + w + 1 || y < 10 || y > 10 + kFontTitle.ascent + kFontTitle.descent)) outside++;
  }
  if (!(full > 10 && part > 10 && outside == 0)) std::printf("text full=%d part=%d outside=%d\n", full, part, outside);
  CHECK(full > 10 && part > 10 && outside == 0);
  f2.clear(kBlack);
  f2.text(240, 100, "42", kWhite, kFontDigits, FrameBuffer565::kRightTop);
  for (int y = 0; y < 320; y++) for (int x = 241; x < 480; x++) CHECK(f2.get(x, y) == kBlack);
  CHECK(f2.text(240, 100, "42", kWhite, kFontDigits, FrameBuffer565::kCentreMiddle) == 240);
  // The middle dot (0xB7) has a glyph.
  CHECK(FrameBuffer565::textWidth("\xB7", kFontSmall) > 0);
  // Planner: identical redraw sends nothing; one changed pixel sends a
  // small window.
  std::vector<uint16_t> shadow(480 * 320);
  PushPlanner565 pl(shadow.data());
  SpanBands bands;
  f2.clear(kBlack);
  pl.take(f2);
  pl.plan(f2, bands);
  CHECK(bands.count == 1 && spanBandBytes(bands) == 480L * 320 * 2);
  f2.fillRect(0, 0, 480, 320, kBlack);
  pl.take(f2);
  pl.plan(f2, bands);
  pl.take(f2);
  pl.plan(f2, bands);
  CHECK(bands.count == 0);
  f2.pixel(200, 101, kWhite);
  pl.take(f2);
  pl.plan(f2, bands);
  CHECK(bands.count == 1 && bands.x0[0] == 200 && bands.x1[0] == 200 && bands.y0[0] == 100 && bands.n[0] == 4);
  CHECK(shadow[101 * 480 + 200] == kWhite);
  if (!failures) std::printf("PASS framebuffer565\n");
  return failures ? 1 : 0;
}
'''


class FrameBuffer565Test(unittest.TestCase):
    def test_framebuffer565(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            self.skipTest('a host C++17 compiler is required')
        sanitize = [] if os.name == 'nt' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        with tempfile.TemporaryDirectory(prefix='freenove-fb565-') as d:
            work = Path(d)
            cpp = work / 'fb565.cpp'
            cpp.write_text(HARNESS, encoding='ascii')
            exe = work / ('fb565' + ('.exe' if os.name == 'nt' else ''))
            built = subprocess.run([compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize,
                                    '-I', str(UI), str(cpp), str(UI / 'FrameBuffer565.cpp'),
                                    str(UI / 'FrameBuffer332.cpp'), str(UI / 'FbFonts.cpp'), str(UI / 'FbFontsAA.cpp'),
                                    '-o', str(exe)], capture_output=True, text=True, timeout=180)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('PASS framebuffer565', ran.stdout)

    def test_pure_cxx11(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            self.skipTest('a host C++17 compiler is required')
        for src in ('FrameBuffer565.cpp', 'FbFontsAA.cpp'):
            ran = subprocess.run([compiler, '-std=c++11', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
                                  '-I', str(UI), str(UI / src)], capture_output=True, text=True, timeout=60)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertTrue((UI / 'LICENSE.D-DIN-OFL.txt').exists())
        self.assertIn('SIL Open Font License', (UI / 'LICENSE.D-DIN-OFL.txt').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
