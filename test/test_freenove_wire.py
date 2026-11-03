"""Freenove panel wire format: the RGB565 page path (transposeBand565) must
put exactly the bytes on the wire that the RGB332 path (LUT +
transposeBand332, the path the 7b builds used) puts there for the same
colour, with the same byte order and no inversion. Host harness (g++,
ASan/UBSan off Windows)."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / 'examples' / 'companion_radio' / 'ui-freenove'
VARIANT = ROOT / 'variants' / 'freenove_s3_35_head'

HARNESS = r'''
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include "FreenoveGeometry.h"
#include "FrameBuffer332.h"
#include "Theme.h"
using namespace freenove;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

// Bytes in bounce-buffer (wire) order for a band of one colour, each path.
static std::vector<uint8_t> wire332(uint8_t c, const uint16_t lut[256], bool flip) {
  std::vector<uint8_t> fb(kLandscapeW * 8, c);
  std::vector<uint16_t> out(8 * 16);
  transposeBand332(fb.data(), 8, 0, 16, flip, lut, out.data());
  std::vector<uint8_t> b(out.size() * 2);
  std::memcpy(b.data(), out.data(), b.size());   // little-endian host = ESP32-S3
  return b;
}
static std::vector<uint8_t> wire565(uint16_t v, bool flip) {
  std::vector<uint16_t> fb(kLandscapeW * 8, v);
  std::vector<uint16_t> out(8 * 16);
  transposeBand565(fb.data(), 8, 0, 16, flip, out.data());
  std::vector<uint8_t> b(out.size() * 2);
  std::memcpy(b.data(), out.data(), b.size());
  return b;
}

int main() {
  uint16_t lut[256];
  buildLut565(lut, true);   // FramePusher's table: wire order
  // Every RGB332 colour: identical wire bytes through both paths.
  for (int f = 0; f < 2; f++)
    for (int c = 0; c < 256; c++)
      CHECK(wire332((uint8_t)c, lut, f != 0) == wire565(rgb332To565((uint8_t)c), f != 0));
  // Byte order: big-endian RGB565 on the wire (high byte first), no inversion.
  struct { const char* name; uint16_t v; uint8_t hi, lo; } cases[] = {
    {"black", 0x0000, 0x00, 0x00},
    {"white", 0xFFFF, 0xFF, 0xFF},
    {"red", 0xF800, 0xF8, 0x00},
    {"blue", 0x001F, 0x00, 0x1F},
    {"navy bg", freenove_ui::theme::kBg, (uint8_t)(freenove_ui::theme::kBg >> 8), (uint8_t)freenove_ui::theme::kBg},
    {"panel", freenove_ui::theme::kPanelFill, (uint8_t)(freenove_ui::theme::kPanelFill >> 8), (uint8_t)freenove_ui::theme::kPanelFill},
    {"cyan", freenove_ui::theme::kAccent, (uint8_t)(freenove_ui::theme::kAccent >> 8), (uint8_t)freenove_ui::theme::kAccent},
    {"green", freenove_ui::theme::kGood, (uint8_t)(freenove_ui::theme::kGood >> 8), (uint8_t)freenove_ui::theme::kGood},
    {"amber", freenove_ui::theme::kWarn, (uint8_t)(freenove_ui::theme::kWarn >> 8), (uint8_t)freenove_ui::theme::kWarn},
  };
  for (auto& k : cases) {
    const std::vector<uint8_t> b = wire565(k.v, false);
    bool ok = true;
    for (size_t i = 0; i < b.size(); i += 2) ok = ok && b[i] == k.hi && b[i + 1] == k.lo;
    if (!ok) std::printf("FAIL %s: got %02X %02X want %02X %02X\n", k.name, b[0], b[1], k.hi, k.lo);
    CHECK(ok);
  }
  // Theme colours: the 565 path's bytes equal the 332 path's for the
  // nearest RGB332 colour within RGB332 quantisation (per channel, in 565
  // levels: red/blue half a 332 step, green half a step).
  const uint16_t theme[] = {freenove_ui::theme::kBg, freenove_ui::theme::kPanelFill, freenove_ui::theme::kAccent,
                            freenove_ui::theme::kGood, freenove_ui::theme::kWarn, freenove_ui::theme::kText,
                            0x0000, 0xFFFF};
  for (uint16_t v : theme) {
    const uint8_t r = (uint8_t)(((v >> 11) & 31) * 255 / 31), g = (uint8_t)(((v >> 5) & 63) * 255 / 63),
                  b = (uint8_t)((v & 31) * 255 / 31);
    const uint8_t c = FrameBuffer332::rgb(r, g, b);
    const std::vector<uint8_t> w8 = wire332(c, lut, false), w16 = wire565(v, false);
    const uint16_t a = (uint16_t)((w8[0] << 8) | w8[1]), z = (uint16_t)((w16[0] << 8) | w16[1]);
    const int dr = (int)((a >> 11) & 31) - (int)((z >> 11) & 31);
    const int dg = (int)((a >> 5) & 63) - (int)((z >> 5) & 63);
    const int db = (int)(a & 31) - (int)(z & 31);
    CHECK(dr >= -3 && dr <= 3 && dg >= -5 && dg <= 5 && db >= -6 && db <= 6);
    // An inverted path would land near ~v: far away for every channel.
    CHECK(!(std::abs(dr) > 20 && std::abs(dg) > 40));
  }
  if (!failures) std::printf("PASS wire\n");
  return failures ? 1 : 0;
}
'''


class FreenoveWireTest(unittest.TestCase):
    def test_565_path_matches_332_path_on_the_wire(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            self.skipTest('a host C++17 compiler is required')
        sanitize = [] if os.name == 'nt' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        with tempfile.TemporaryDirectory(prefix='freenove-wire-') as d:
            cpp = Path(d) / 'wire.cpp'
            cpp.write_text(HARNESS, encoding='ascii')
            exe = Path(d) / ('wire' + ('.exe' if os.name == 'nt' else ''))
            built = subprocess.run([compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize,
                                    '-I', str(UI), '-I', str(VARIANT), '-I', str(ROOT / 'src'),
                                    str(cpp), str(UI / 'FrameBuffer332.cpp'), str(UI / 'FbFonts.cpp'),
                                    '-o', str(exe)], capture_output=True, text=True, timeout=180)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('PASS wire', ran.stdout)

    def test_inversion_is_one_panel_setting_for_both_paths(self):
        # The pixel paths never invert; the panel's inversion is one init
        # command (INVON by default, INVOFF with FREENOVE_PANEL_INVERT=0),
        # the same for RGB332 and RGB565 frames.
        src = (VARIANT / 'St77922Panel.cpp').read_text(encoding='ascii')
        geo = (VARIANT / 'FreenoveGeometry.h').read_text(encoding='ascii')
        for body in (geo[geo.index('inline void transposeBand565'):geo.index('// RGB332 -> RGB565')],
                     geo[geo.index('inline void transposeBand332'):geo.index('inline void transposeBand565')]):
            self.assertNotIn('~', body)
            self.assertNotIn('0xFFFF', body)
        self.assertIn('FREENOVE_PANEL_INVERT', src)


if __name__ == '__main__':
    unittest.main()
