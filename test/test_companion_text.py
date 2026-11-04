"""Freenove HEAD page: the companion UI's text through the D-DIN AA renderer
(ui-freenove/CompanionText.h, used by FreenoveDisplay). Host harness (g++,
ASan/UBSan off Windows) that also renders the real TouchKeyboard and a
replica of HeadAdmin's settings list through a DisplayDriver that behaves
like the Freenove adapter (boxes zoomed 2x, text AA at full resolution).

    python test_companion_text.py --preview   # head_keyboard_preview*.png, head_list_preview*.png
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / 'examples' / 'companion_radio' / 'ui-freenove'
UI_NEW = ROOT / 'examples' / 'companion_radio' / 'ui-new'
VARIANT = ROOT / 'variants' / 'freenove_s3_35_head'
PREVIEW_DIR = ROOT / '.superpowers' / 'sdd' / '2026-10-30-freenove-visual-head'

HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <helpers/ui/DisplayDriver.h>
#include "TouchKeyboard.h"
#include "CompanionText.h"
using namespace freenove_ui;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

// The companion palette (ColorTheme.h, day).
ColorVal UIColor::window_bkg = rgb565(10, 12, 16);
ColorVal UIColor::title_bkg = rgb565(18, 38, 74);
ColorVal UIColor::title_txt = rgb565(232, 238, 246);
ColorVal UIColor::primary_txt = rgb565(232, 238, 246);
ColorVal UIColor::secondary_txt = rgb565(122, 134, 150);
ColorVal UIColor::warning_txt = rgb565(248, 176, 72);
ColorVal UIColor::popup_bkg = rgb565(20, 48, 70);
ColorVal UIColor::popup_txt = rgb565(232, 238, 246);
ColorVal UIColor::corp_blue = rgb565(64, 176, 240);

// Logical 240x148, zoomed 2x to the 480x296 companion area, like the
// Freenove adapter: boxes in the (zoomed) sprite, text recorded and drawn
// AA over the frame at the end.
class HostDisplay : public DisplayDriver {
 public:
  HostDisplay() : DisplayDriver(240, 148), px(480 * 320), fb(px.data()) {}
  std::vector<uint16_t> px;
  FrameBuffer565 fb;
  CompanionText text;
  uint16_t colour = 0xFFFF;
  int size = 1, cx = 0, cy = 0;
  bool isOn() override { return true; }
  void turnOn() override {}
  void turnOff() override {}
  void clear() override { fb.clear(UIColor::window_bkg); }
  void startFrame(ColorVal bkg) override { fb.clear(bkg); text.clear(); }
  void setTextSize(int sz) override { size = sz < 1 ? 1 : sz; }
  void setColor(ColorVal c) override { colour = c; }
  void setCursor(int x, int y) override { cx = x; cy = y; }
  void print(const char* s) override { cx = text.add(cx, cy, size, colour, s); }
  void fillRect(int x, int y, int w, int h) override { fb.fillRect(x * 2, y * 2, w * 2, h * 2, colour); text.occlude(x, y, w, h); }
  void drawRect(int x, int y, int w, int h) override {
    fb.fillRect(x * 2, y * 2, w * 2, 2, colour); fb.fillRect(x * 2, (y + h - 1) * 2, w * 2, 2, colour);
    fb.fillRect(x * 2, y * 2, 2, h * 2, colour); fb.fillRect((x + w - 1) * 2, y * 2, 2, h * 2, colour);
  }
  void drawXbm(int, int, const uint8_t*, int, int) override {}
  uint16_t getTextWidth(const char* s) override { return (uint16_t)CompanionText::logicalWidth(s, size); }
  int textLineHeight() override { return CompanionText::lineHeight(size); }
  void endFrame() override { text.draw(fb, 296); }
  bool save(const char* path) const {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(px.data(), 2, px.size(), f) == px.size();
    std::fclose(f);
    return ok;
  }
};

// HeadAdmin::drawButton, verbatim (HeadAdmin.h needs the companion runtime).
static void drawButton(DisplayDriver& d, int x, int y, int w, int h, const char* label, bool on, bool enabled = true) {
  d.setColor(on ? UIColor::corp_blue : enabled ? UIColor::title_bkg : UIColor::window_bkg);
  d.fillRect(x + 1, y + 1, w - 2, h - 2);
  d.setColor(UIColor::secondary_txt);
  d.drawRect(x, y, w, h);
  d.setColor(enabled ? UIColor::title_txt : UIColor::secondary_txt);
  d.drawTextEllipsized(x + 3, y + (h - 8) / 2, w - 6, label);
}

static void metrics() {
  CHECK(&CompanionText::font(1) == &kFontSmall && &CompanionText::font(2) == &kFontText20 &&
        &CompanionText::font(3) == &kFontText28 && &CompanionText::font(7) == &kFontText28);
  CHECK(kFontSmall.size == 14 && kFontText20.size == 20 && kFontText28.size == 28);
  // Logical widths round up; a 14 px "M" fits the old 6 px cell.
  const int m = CompanionText::logicalWidth("M", 1);
  CHECK(m >= 4 && m <= 6);
  CHECK(CompanionText::logicalWidth("", 1) == 0);
  CHECK(CompanionText::logicalWidth("Settings", 2) > CompanionText::logicalWidth("Settings", 1));
  // The old line spacing rule is kept.
  CHECK(CompanionText::lineHeight(1) == 10 && CompanionText::lineHeight(2) == 24 && CompanionText::lineHeight(3) == 36);
  // Lines sit centred in the old 8 x size cell.
  for (int s = 1; s <= 3; s++) {
    const AAFont& f = CompanionText::font(s);
    const int top = CompanionText::physicalTop(10, s);
    const int mid = top + (f.ascent + f.descent) / 2, cellMid = 10 * 2 + 8 * s;
    CHECK(mid >= cellMid - 1 && mid <= cellMid + 1);
  }
  // print() continues on the line; hashing sees text-only changes; a later
  // fill hides the text under it.
  CompanionText t;
  const int x = t.add(10, 20, 1, 0xFFFF, "abc");
  CHECK(x == 10 + CompanionText::logicalWidth("abc", 1));
  const uint32_t h1 = t.hash();
  CompanionText u;
  u.add(10, 20, 1, 0xFFFF, "abd");
  CHECK(u.hash() != h1);
  t.add(10, 60, 2, 0xFFFF, "kept");
  t.occlude(0, 15, 100, 10);
  CHECK(t.count() == 2 && t.visible() == 1 && t.hash() != h1);
  // Bounded: overflow drops text, never writes past the pool.
  CompanionText big;
  for (int i = 0; i < 400; i++) big.add(0, 0, 1, 0, "0123456789012345678901234567890123456789");
  CHECK(big.count() <= CompanionText::kMaxOps);
  if (!failures) std::printf("PASS metrics\n");
}

static void keyboard_fits() {
  HostDisplay d;
  TouchKeyboard kb;
  kb.reset("hello mesh");
  d.startFrame(UIColor::window_bkg);
  d.setTextSize(1);
  kb.drawText(d, 30);
  kb.draw(d, 46, 21, "OK");
  d.endFrame();
  // The key labels are drawn (AA glyph pixels in the keyboard rows).
  int textPx = 0;
  for (int y = 46 * 2; y < (46 + 4 * 21) * 2 && y < 296; y++)
    for (int x = 0; x < 480; x++) textPx += ((d.fb.get(x, y) >> 11) & 31) >= 24;   // near-white glyph pixels
  CHECK(textPx > 150);
  CHECK(d.text.count() >= 30);
  if (!failures) std::printf("PASS keyboard_fits\n");
}

static int preview(const char* dir) {
  bool ok = true;
  {
    HostDisplay d;
    TouchKeyboard kb;
    kb.reset("CaptCallahan-Base");
    d.startFrame(UIColor::window_bkg);
    d.setTextSize(1);
    // HeadAdmin's header buttons and the entry title, as renderEntry draws them.
    drawButton(d, 0, 0, 96, 18, "Rptr:CaptCallahan-Base", false);
    drawButton(d, 96, 0, 240 - 96, 18, "Logged in", true);
    d.setColor(UIColor::secondary_txt);
    d.setCursor(2, 20);
    d.print("set name");
    d.setColor(UIColor::primary_txt);
    kb.drawText(d, 30);
    d.setColor(UIColor::secondary_txt);
    d.drawRect(0, 28, 240, 12);
    kb.draw(d, 40, 21, "OK");
    d.endFrame();
    ok = d.save((std::string(dir) + "/head_keyboard.rgb565").c_str()) && ok;
  }
  {
    HostDisplay d;
    d.startFrame(UIColor::window_bkg);
    d.setTextSize(1);
    drawButton(d, 0, 0, 96, 18, "Rptr:CaptCallahan-Base", false);
    drawButton(d, 96, 0, 240 - 96, 18, "Logged in", true);
    static const char* const keys[] = {"bridge.enabled", "bridge.format", "bridge.source", "bridge.channel",
                                       "Forget password", "name", "radio"};
    const int top = 18, row_h = (148 - top) / 8;
    for (int r = 0; r < 7; r++) drawButton(d, 0, top + r * row_h, 240, row_h, keys[r], false);
    const int y = top + 7 * row_h, w = 240 / 3;
    drawButton(d, 0, y, w, row_h, "Up", false);
    drawButton(d, w, y, w, row_h, "Down", false);
    drawButton(d, 2 * w, y, 240 - 2 * w, row_h, "Back", false);
    d.endFrame();
    ok = d.save((std::string(dir) + "/head_list.rgb565").c_str()) && ok;
  }
  std::printf(ok ? "PASS preview\n" : "FAIL preview\n");
  return ok ? 0 : 1;
}

int main(int argc, char** argv) {
  const std::string n = argc > 1 ? argv[1] : "";
  if (n == "preview") return preview(argc > 2 ? argv[2] : ".");
  if (n == "metrics") metrics();
  else if (n == "keyboard_fits") keyboard_fits();
  else { std::printf("unknown %s\n", n.c_str()); return 2; }
  return failures ? 1 : 0;
}
'''


def build(work):
    compiler = shutil.which('g++') or shutil.which('clang++')
    if compiler is None:
        raise unittest.SkipTest('a host C++17 compiler is required')
    sanitize = [] if os.name == 'nt' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    cpp = work / 'ctext.cpp'
    cpp.write_text(HARNESS, encoding='ascii')
    exe = work / ('ctext' + ('.exe' if os.name == 'nt' else ''))
    built = subprocess.run([compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                            '-DTOUCH_KEYBOARD_PC_LAYOUT=1',  # the Freenove head's keyboard
                            *sanitize, '-I', str(UI), '-I', str(UI_NEW), '-isystem', str(ROOT / 'src'),
                            '-isystem', str(ROOT / 'test' / 'mocks'), str(cpp), str(UI / 'FrameBuffer565.cpp'),
                            str(UI / 'FrameBuffer332.cpp'), str(UI / 'FbFonts.cpp'), str(UI / 'FbFontsAA.cpp'),
                            '-o', str(exe)], capture_output=True, text=True, timeout=180)
    if built.returncode != 0:
        raise AssertionError(built.stdout + built.stderr)
    return exe


class CompanionTextTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='freenove-ctext-')
        try:
            cls.exe = build(Path(cls.temp.name))
        except BaseException:
            cls.temp.cleanup()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, name):
        ran = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=60)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('PASS ' + name, ran.stdout)

    def test_metrics(self):
        self.run_case('metrics')

    def test_keyboard_fits(self):
        self.run_case('keyboard_fits')

    def test_preview_renders(self):
        ran = subprocess.run([str(self.exe), 'preview', self.temp.name], capture_output=True, text=True, timeout=60)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_freenove_display_routes_text(self):
        # Freenove only: the adapter records print() into CompanionText and
        # draws it over the companion frame; LGFXDisplay itself is unchanged.
        src = (VARIANT / 'FreenoveDisplay.cpp').read_text(encoding='ascii')
        hdr = (VARIANT / 'FreenoveDisplay.h').read_text(encoding='ascii')
        self.assertIn('FREENOVE_AA_COMPANION_TEXT 1', hdr)
        self.assertIn('_text.add(', src)
        self.assertIn('_shownText.draw(_companionView', src)
        self.assertIn('_text.occlude(', src)
        self.assertIn('textHash == _shownTextHash', src)
        lgfx = (ROOT / 'src' / 'helpers' / 'ui' / 'LGFXDisplay.cpp').read_text(encoding='utf-8')
        self.assertNotIn('CompanionText', lgfx)


def write_preview():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from test_layouts import rgb565_rows, write_png
    with tempfile.TemporaryDirectory(prefix='freenove-ctext-preview-') as tmp:
        exe = build(Path(tmp))
        ran = subprocess.run([str(exe), 'preview', tmp], capture_output=True, text=True, timeout=60)
        if ran.returncode != 0:
            raise SystemExit(ran.stdout + ran.stderr)
        for src, dst in (('head_keyboard', 'head_keyboard_preview'), ('head_list', 'head_list_preview')):
            raw = (Path(tmp) / (src + '.rgb565')).read_bytes()
            write_png(rgb565_rows(raw, scale=1), PREVIEW_DIR / (dst + '_1x.png'))
            write_png(rgb565_rows(raw, scale=2), PREVIEW_DIR / (dst + '.png'))
            print('wrote', PREVIEW_DIR / (dst + '_1x.png'), 'and the 2x zoom')


if __name__ == '__main__':
    if '--preview' in sys.argv:
        write_preview()
    else:
        unittest.main()
