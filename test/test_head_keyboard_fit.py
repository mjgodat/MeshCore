"""On-screen keyboard (ui-new/TouchKeyboard.h) on every head screen that
hosts it: all four rows (with the 123 and action keys) fit on the screen,
keys are finger sized, and nothing is touchable that is not drawn.

Screens (logical companion px, 2x zoom): the CYD visual head's HEAD page
(160 x 105: 320 x 210 above the 30-row tab bar), the plain CYD head
(160 x 120) and the Freenove head (240 x 148). Hosts: the admin entry
(password / command / setting value; keyboard from y 40, at most 20 rows)
and the compose screen (from y 32, at most 22 rows).

For each case a recording DisplayDriver captures the key rectangles draw()
produced; then every logical pixel of the screen is tapped on a fresh
keyboard: a tap that does anything must land inside a drawn key, and every
drawn key must answer a tap at its centre. The hosts are checked to size
the rows with keyHeight() and to hit-test with the drawn geometry.
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI_NEW = ROOT / 'examples' / 'companion_radio' / 'ui-new'

HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "TouchKeyboard.h"
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

ColorVal UIColor::window_bkg = 0, UIColor::title_bkg = 1, UIColor::title_txt = 2, UIColor::primary_txt = 3,
         UIColor::secondary_txt = 4, UIColor::warning_txt = 5, UIColor::popup_bkg = 6, UIColor::popup_txt = 7,
         UIColor::corp_blue = 8;

struct Rect { int x, y, w, h; };

class Recorder : public DisplayDriver {
 public:
  Recorder(int w, int h) : DisplayDriver(w, h) {}
  std::vector<Rect> rects;
  bool isOn() override { return true; }
  void turnOn() override {}
  void turnOff() override {}
  void clear() override {}
  void startFrame(ColorVal) override {}
  void setTextSize(int) override {}
  void setColor(ColorVal) override {}
  void setCursor(int, int) override {}
  void print(const char*) override {}
  void fillRect(int, int, int, int) override {}
  void drawRect(int x, int y, int w, int h) override { rects.push_back({x, y, w, h}); }
  void drawXbm(int, int, const uint8_t*, int, int) override {}
  uint16_t getTextWidth(const char* s) override { return (uint16_t)(6 * std::strlen(s)); }
  void endFrame() override {}
};

using R = TouchKeyboard::Result;

// Drawn key rects grow by the 1 px inset drawKey uses (keys are drawn at
// x + 1, y + 1, w - 2, h - 2): the touch cell is the rect plus that inset.
static bool inDrawnKey(const std::vector<Rect>& keys, int x, int y) {
  for (const Rect& r : keys)
    if (x >= r.x - 1 && x < r.x + r.w + 1 && y >= r.y - 1 && y < r.y + r.h + 1) return true;
  return false;
}

static void check(const char* name, int w, int h, int top, int max_h, int min_key_h, int expect_key_h) {
  const int key_h = TouchKeyboard::keyHeight(top, h, max_h);
  if (expect_key_h > 0) CHECK(key_h == expect_key_h);
  CHECK(key_h >= min_key_h);                          // finger sized (logical px; 2x on the glass)
  CHECK(top + TouchKeyboard::kRows * key_h <= h);     // all four rows on the screen
  for (int page = 0; page < 3; page++) {   // abc, ABC, 123
    Recorder d(w, h);
    TouchKeyboard k;
    k.reset("", 100);
    // Reach the mode: the mode key (third row, left) cycles abc ABC 123.
    k.draw(d, top, key_h, "OK");
    for (int i = 0; i < page; i++) k.touch(4, top + 2 * key_h + key_h / 2);
    static const char* const kModes[3] = {"abc", "ABC", "123"};
    CHECK(std::strcmp(k.modeLabel(), kModes[page]) == 0);
    d.rects.clear();
    k.draw(d, top, key_h, "OK");
    CHECK(k.drawnRows() == TouchKeyboard::kRows);
    // Drawn: the mode key (third row, left), space (bottom left) and the
    // action key (bottom right).
    bool modeKey = false, bottomLeft = false, bottomRight = false;
    for (const Rect& r : d.rects) {
      CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= w && r.y + r.h <= h);   // never off the screen
      if (r.y > top + 2 * key_h - 1 && r.y < top + 3 * key_h && r.x <= 1) modeKey = true;
      if (r.y > top + 3 * key_h - 1 && r.x <= 1) bottomLeft = true;
      if (r.y > top + 3 * key_h - 1 && r.x + r.w >= w - 2) bottomRight = true;
    }
    CHECK(modeKey && bottomLeft && bottomRight);
    // Every logical pixel: a tap that does something is on a drawn key.
    int stray = 0;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        TouchKeyboard t = k;
        if (t.touch(x, y) != R::None && !inDrawnKey(d.rects, x, y)) stray++;
      }
    if (stray) std::printf("%s page %d: %d touchable pixels outside drawn keys\n", name, page, stray);
    CHECK(stray == 0);
    // Every drawn key answers at its centre.
    for (const Rect& r : d.rects) {
      TouchKeyboard t = k;
      CHECK(t.touch(r.x + r.w / 2, r.y + r.h / 2) != R::None);
    }
  }
  // Nothing is touchable before the first draw, or below the drawn rows.
  TouchKeyboard fresh;
  CHECK(fresh.touch(5, top + 2) == R::None);
  // A screen too short for four rows: the rows that do not fit are neither
  // drawn nor touchable.
  Recorder small(w, top + 2 * key_h + key_h / 2);
  TouchKeyboard s;
  s.draw(small, top, key_h, "OK");
  CHECK(s.drawnRows() == 2);
  for (const Rect& r : small.rects) CHECK(r.y + r.h <= small.height());
  CHECK(s.touch(4, top + 3 * key_h + 2) == R::None);
  CHECK(s.touch(4, top + 2 * key_h + 2) == R::None);
}

int main() {
  // CYD visual head, HEAD page: 160 x 105.
  check("cyd-hmi admin entry", 160, 105, 40, 20, 16, 16);
  check("cyd-hmi compose", 160, 105, 32, 22, 16, 18);
  // Plain CYD head (160 x 120) and the Freenove head (240 x 148): the key
  // heights the screens always had.
  check("cyd admin entry", 160, 120, 40, 20, 16, 20);
  check("cyd compose", 160, 120, 32, 22, 16, 22);
  check("freenove admin entry", 240, 148, 40, 20, 16, 20);
  check("freenove compose", 240, 148, 32, 22, 16, 22);
  if (failures) return 1;
  std::printf("PASS keyboard_fit\n");
  return 0;
}
'''


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


class KeyboardFitTest(unittest.TestCase):
    def test_keyboard_fits_and_only_drawn_keys_touch(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            self.skipTest('a host C++17 compiler is required')
        sanitize = [] if os.name == 'nt' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        with tempfile.TemporaryDirectory(prefix='kb-fit-') as tmp:
            cpp = Path(tmp) / 'kb.cpp'
            cpp.write_text(HARNESS, encoding='ascii')
            exe = Path(tmp) / ('kb' + ('.exe' if os.name == 'nt' else ''))
            built = subprocess.run([compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-unused-parameter', *sanitize, '-I', str(UI_NEW), '-I', str(ROOT / 'src'),
                                    '-I', str(ROOT / 'test'), str(cpp), '-o', str(exe)],
                                   capture_output=True, text=True, timeout=180)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('PASS keyboard_fit', ran.stdout)

    def test_hosts_size_rows_and_hit_test_the_drawn_keys(self):
        admin = strip_comments((UI_NEW / 'HeadAdmin.h').read_text(encoding='ascii'))
        compose = strip_comments((UI_NEW / 'HeadCompose.h').read_text(encoding='ascii'))
        for name, src in (('HeadAdmin.h', admin), ('HeadCompose.h', compose)):
            self.assertIn('TouchKeyboard::keyHeight(', src, name)
            self.assertRegex(src, r'_keyboard\.touch\(\s*x\s*,\s*y\s*\)', name)
            self.assertNotRegex(src, r'_keyboard\.touch\([^)]*,[^)]*,[^)]*,', name)   # no separate geometry
        # The admin entry's keyboard starts at y 40 (kHeaderH + 10 + kTextH - 2)
        # with at most kKeyH - 1 rows; compose at kHeaderH + kTextH, kKeyH.
        self.assertIn('kb_top = top + 10 + kTextH - 2', admin)
        self.assertIn('static constexpr int kHeaderH = 18;', admin)
        self.assertIn('static constexpr int kTextH = 14;', admin)
        self.assertIn('static constexpr int kKeyH = 22;', admin)


if __name__ == '__main__':
    unittest.main()
