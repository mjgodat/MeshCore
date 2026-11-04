"""Ghost gesture hints (ui-new/GestureHints.h) on the repeater heads:
visibility per position, the brightness step, carousel tiles equal to the
touch zones the carousel really uses (TouchDebugOverlay.h's rule, which the
touch tests check against TouchInput), and the wiring (the carousel draws
its tiles; NODES draws up / down tiles that are its hit targets: covered by
test_strip_renderer's nodes_hints scenario in both screen profiles).
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
#include <cstdio>
#include "GestureHints.h"
#include <helpers/ui/TouchDebugOverlay.h>
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
using namespace gesture_hints;
int main() {
  // Hidden without more content; bright for kBrightMs after a change.
  CHECK(level(false, 1000, 1000) == kHidden);
  CHECK(level(true, 1000, 1000) == kBright);
  CHECK(level(true, 1000 + kBrightMs - 1, 1000) == kBright);
  CHECK(level(true, 1000 + kBrightMs, 1000) == kGhost);
  CHECK(level(true, 5, 0xFFFFFFF0u) == kBright);   // wraps
  CHECK(settleMs(1000, 1000) == kBrightMs && settleMs(1000 + kBrightMs, 1000) == 0);
  // List tiles at the ends of the column.
  Rect up, dn;
  listTiles(300, 12, 78, 206, 18, up, dn);
  CHECK(up.x == 300 && up.y == 78 && up.w == 12 && up.h == 18);
  CHECK(dn.y == 206 - 18 && dn.h == 18 && dn.contains(305, 200) && !dn.contains(305, 206));
  // Carousel tiles = the Previous / Next touch zones below the title bar,
  // for the CYD HEAD page (160 x 105) and the Freenove (240 x 148).
  const int sizes[2][2] = {{160, 105}, {240, 148}};
  for (int s = 0; s < 2; s++) {
    const int W = sizes[s][0], H = sizes[s][1], top = 20;
    Rect l, r;
    carouselZones(W, H, 50, top, l, r);
    const auto areas = mesh::ui::makeTouchDebugAreas(W, H, 50, false, nullptr, nullptr);
    CHECK(areas.count == 3);
    const auto& prev = areas.areas[0];
    const auto& next = areas.areas[2];
    CHECK(prev.action == mesh::ui::TouchAction::Previous && next.action == mesh::ui::TouchAction::Next);
    CHECK(l.x == prev.x && l.w == prev.width && l.y == top && l.y + l.h == prev.y + prev.height);
    CHECK(r.x == next.x && r.w == next.width && r.y == top && r.y + r.h == next.y + next.height);
  }
  if (failures) return 1;
  std::printf("PASS gesture_hints\n");
  return 0;
}
'''


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


class GestureHintsTest(unittest.TestCase):
    def test_levels_and_geometry(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            self.skipTest('a host C++17 compiler is required')
        sanitize = [] if os.name == 'nt' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        with tempfile.TemporaryDirectory(prefix='hints-') as tmp:
            cpp = Path(tmp) / 'h.cpp'
            cpp.write_text(HARNESS, encoding='ascii')
            exe = Path(tmp) / ('h' + ('.exe' if os.name == 'nt' else ''))
            built = subprocess.run([compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-unused-parameter', *sanitize, '-I', str(UI_NEW), '-I', str(ROOT / 'src'),
                                    '-I', str(ROOT / 'test'), str(cpp), '-o', str(exe)],
                                   capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('PASS gesture_hints', ran.stdout)

    def test_carousel_draws_its_zones(self):
        ui = strip_comments((UI_NEW / 'UITask.cpp').read_text(encoding='utf-8'))
        self.assertIn('gesture_hints::carouselZones(display.width(), display.height(), TOUCH_CENTER_ZONE_PERCENT', ui)
        # The column's inner edge is drawn (a 1 px divider), with the chevron.
        self.assertIn('display.drawRect(i == 0 ? r.x + r.w - 1 : r.x, r.y, 1, r.h)', ui)
        self.assertIn('_hintMs = millis();', ui)   # page turn brightens


if __name__ == '__main__':
    unittest.main()
