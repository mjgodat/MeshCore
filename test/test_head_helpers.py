"""Host tests for repeater-head helpers: DMS coordinate text, the debounced
GPS slide-switch watcher, and the on-screen touch keyboard's key logic.

Each case compiles the real header with a small harness; no Arduino code.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

GEO_HARNESS = r'''
#include <helpers/ui/GeoFormat.h>
#include <cstdio>
#include <cstring>
static int failures = 0;
static void expect(long micro, bool lat, const char* want) {
  char out[24];
  mesh::ui::formatMicrodegreesDms(out, sizeof(out), micro, lat);
  if (std::strcmp(out, want) != 0) {
    std::printf("FAIL %ld %d: got [%s] want [%s]\n", micro, (int)lat, out, want);
    failures++;
  }
}
int main() {
  expect(47606209, true, "N47 36'22.4\"");
  expect(-122332069, false, "W122 19'55.4\"");
  expect(0, true, "N0 00'00.0\"");
  expect(0, false, "E0 00'00.0\"");
  expect(-500000, true, "S0 30'00.0\"");
  expect(47999987, true, "N48 00'00.0\"");      // 59.953" rounds up and carries
  expect(47999986, true, "N47 59'59.9\"");      // 59.9496" stays
  expect(90000000, true, "N90 00'00.0\"");
  expect(-90000000, true, "S90 00'00.0\"");
  expect(180000000, false, "E180 00'00.0\"");
  expect(-180000000, false, "W180 00'00.0\"");
  char tiny[6];
  volatile size_t tiny_size = sizeof(tiny);  // runtime size: truncation is the point
  const int n = mesh::ui::formatMicrodegreesDms(tiny, tiny_size, 47606209, true);
  if (n != 12 || std::strlen(tiny) != 5) { std::printf("FAIL truncation %d [%s]\n", n, tiny); failures++; }
  if (failures) return 1;
  std::printf("PASS geo\n");
  return 0;
}
'''

SWITCH_HARNESS = r'''
#include <helpers/sensors/GpsHardwareSwitch.h>
#include <cstdio>
using C = mesh::GpsHardwareSwitch::Change;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)
int main() {
  {  // boot position is reported once, whichever way the switch sits
    mesh::GpsHardwareSwitch s;
    CHECK(s.poll(true, 0) == C::On);
    CHECK(s.poll(true, 10) == C::None);
    CHECK(s.isOn());
  }
  {
    mesh::GpsHardwareSwitch s;
    CHECK(s.poll(false, 5) == C::Off);
    CHECK(!s.isOn());
  }
  {  // a flip must hold for the debounce time, then reports exactly once
    mesh::GpsHardwareSwitch s;
    s.poll(true, 0);
    CHECK(s.poll(false, 1000) == C::None);
    CHECK(s.poll(false, 1099) == C::None);
    CHECK(s.poll(false, 1100) == C::Off);
    CHECK(s.poll(false, 1500) == C::None);
    CHECK(!s.isOn());
  }
  {  // contact bounce restarts the timer and never reports
    mesh::GpsHardwareSwitch s;
    s.poll(true, 0);
    CHECK(s.poll(false, 1000) == C::None);
    CHECK(s.poll(true, 1050) == C::None);
    CHECK(s.poll(false, 1090) == C::None);
    CHECK(s.poll(false, 1150) == C::None);
    CHECK(s.poll(false, 1190) == C::Off);
  }
  {  // a bounce back to the stable state before the deadline reports nothing
    mesh::GpsHardwareSwitch s;
    s.poll(true, 0);
    CHECK(s.poll(false, 1000) == C::None);
    CHECK(s.poll(true, 1050) == C::None);
    CHECK(s.poll(true, 2000) == C::None);
    CHECK(s.isOn());
  }
  {  // millis() wrap
    mesh::GpsHardwareSwitch s;
    s.poll(false, 0xFFFFFF00u);
    CHECK(s.poll(true, 0xFFFFFFF0u) == C::None);
    CHECK(s.poll(true, 0x00000050u) == C::None);  // 96 ms across the wrap
    CHECK(s.poll(true, 0x00000060u) == C::On);    // 112 ms
  }
  if (failures) return 1;
  std::printf("PASS switch\n");
  return 0;
}
'''

KEYBOARD_HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../examples/companion_radio/ui-new/TouchKeyboard.h"
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)
using R = TouchKeyboard::Result;
// 160 wide, keys from y=0, 22 tall: rows at y 5 / 27 / 49 / 71.
static R tap(TouchKeyboard& k, int x, int y) { return k.touch(x, y, 160, 0, 22); }
int main() {
  TouchKeyboard k;
  k.reset("", 3);
  CHECK(tap(k, 5, 5) == R::Changed);           // q
  CHECK(std::strcmp(k.text(), "q") == 0);
  CHECK(tap(k, 2, 49) == R::Changed);          // shift (left of z row)
  CHECK(tap(k, 21, 5) == R::Changed);          // w -> W, shift releases
  CHECK(tap(k, 37, 5) == R::Changed);          // e
  CHECK(std::strcmp(k.text(), "qWe") == 0);
  CHECK(tap(k, 53, 5) == R::Changed);          // r ignored: max length 3
  CHECK(std::strcmp(k.text(), "qWe") == 0);
  CHECK(tap(k, 150, 49) == R::Changed);        // delete
  CHECK(std::strcmp(k.text(), "qW") == 0);
  CHECK(tap(k, 40, 71) == R::Changed);         // space
  CHECK(std::strcmp(k.text(), "qW ") == 0);
  CHECK(tap(k, 100, 71) == R::Cancel);
  CHECK(tap(k, 150, 71) == R::Submit);
  CHECK(tap(k, 80, 200) == R::None);           // below the keyboard
  k.reset("", 10);
  CHECK(tap(k, 5, 71) == R::Changed);          // 123
  CHECK(tap(k, 5, 5) == R::Changed);           // 1
  CHECK(tap(k, 2, 49) == R::Changed);          // shift does not alter symbols
  CHECK(tap(k, 21, 5) == R::Changed);          // 2
  CHECK(std::strcmp(k.text(), "12") == 0);
  k.reset("abcdefghijklmnop", 5);              // initial text is clamped
  CHECK(std::strcmp(k.text(), "abcde") == 0);
  CHECK(k.length() == 5);
  if (failures) return 1;
  std::printf("PASS keyboard\n");
  return 0;
}
'''


NODES_HARNESS = r'''
#include <helpers/bridges/ESPNowNodeTable.h>
#include <cstdio>
#include <cstring>
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)
static void mac(uint8_t* m, uint8_t last) { const uint8_t base[6] = {0x8C,0x94,0xDF,0x4D,0x21,0}; std::memcpy(m, base, 6); m[5] = last; }
int main() {
  using T = mesh::bridge::ESPNowNodeTable;
  T t;
  uint8_t a[6], b[6], c[6];
  mac(a, 0x9C); mac(b, 0x24); mac(c, 0x01);
  CHECK(t.activeCount(0) == 0);
  t.note(a, 1000); t.note(a, 2000); t.note(b, 3000);
  CHECK(t.activeCount(4000) == 2);                       // same MAC counted once
  char out[160];
  t.format(out, sizeof(out), 5000);
  CHECK(std::strcmp(out, "2 heard 15m: 4D2124 2s/1 4D219C 3s/2") == 0);  // newest first
  if (failures) std::printf("got [%s]\n", out);
  CHECK(t.activeCount(2000 + T::kWindowMillis) == 2);    // a still inside the window
  CHECK(t.activeCount(2001 + T::kWindowMillis) == 1);    // a aged out, b remains
  T full;
  uint8_t m[6];
  for (int i = 0; i < T::kCapacity; i++) { mac(m, (uint8_t)i); full.note(m, 100 + i); }
  CHECK(full.activeCount(200) == T::kCapacity);
  mac(m, 0xEE); full.note(m, 300);                       // evicts the oldest (i = 0)
  CHECK(full.activeCount(400) == T::kCapacity);
  mac(m, 0x00); full.note(m, 500);                       // re-adds 0 as new, evicting i = 1
  CHECK(full.activeCount(600) == T::kCapacity);
  char tiny[20];
  full.format(tiny, sizeof(tiny), 600);
  CHECK(std::strlen(tiny) < sizeof(tiny));
  CHECK(std::strncmp(tiny, "16 heard 15m:", 13) == 0);
  T wrap;                                                // millis() wrap
  wrap.note(a, 0xFFFFFF00u);
  CHECK(wrap.activeCount(0x00000100u) == 1);
  if (failures) return 1;
  std::printf("PASS nodes\n");
  return 0;
}
'''


PRESETS_HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../examples/companion_radio/ui-new/HeadPresets.h"
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)
#define STREQ(a, b) CHECK(std::strcmp((a), (b)) == 0)
int main() {
  HeadPresetList l;
  // full list, as the raw reply text
  CHECK(parseHeadPresetReply("1*USA-Canada 910.5250244,62.5,7,5\n2 Test 925.5,62.5,9,5\n3 -\n4 -\n5 -", l));
  CHECK(!l.pending && !l.names_only);
  CHECK(l.slot[0].used && l.slot[0].active);
  STREQ(l.slot[0].name, "USA-Canada");
  STREQ(l.slot[0].freq, "910.525");
  CHECK(l.slot[1].used && !l.slot[1].active);
  STREQ(l.slot[1].name, "Test");
  STREQ(l.slot[1].freq, "925.5");
  for (int i = 2; i < 5; i++) { CHECK(!l.slot[i].used && !l.slot[i].active); STREQ(l.slot[i].name, ""); STREQ(l.slot[i].freq, ""); }
  CHECK(headPresetActiveSlot(l) == 0);

  // pending prefix
  CHECK(parseHeadPresetReply("pending, reverts in 9m; 1 USA-Canada 910.5250244,62.5,7,5\n2*Test 925.5,62.5,9,5\n3 -\n4 -\n5 -", l));
  CHECK(l.pending && l.pending_minutes == 9);
  CHECK(!l.slot[0].active && l.slot[1].active);
  STREQ(l.slot[0].name, "USA-Canada");
  STREQ(l.slot[1].freq, "925.5");
  CHECK(headPresetActiveSlot(l) == 1);

  // names-only fallback (single line)
  CHECK(parseHeadPresetReply("1*USA-Canada 2 Test 3 - 4 Long Name 5 - (get preset N)", l));
  CHECK(l.names_only && !l.pending);
  CHECK(l.slot[0].used && l.slot[0].active);
  STREQ(l.slot[0].name, "USA-Canada");
  STREQ(l.slot[0].freq, "");
  CHECK(l.slot[1].used && !l.slot[1].active);
  STREQ(l.slot[1].name, "Test");
  CHECK(!l.slot[2].used);
  CHECK(l.slot[3].used);
  STREQ(l.slot[3].name, "Long Name");
  CHECK(!l.slot[4].used);

  // pending + names-only
  CHECK(parseHeadPresetReply("pending, reverts in 10m; 1 A 2*B 3 - 4 - 5 - (get preset N)", l));
  CHECK(l.pending && l.pending_minutes == 10 && l.names_only);
  CHECK(l.slot[1].active);
  STREQ(l.slot[1].name, "B");

  // 5th tuple field (preamble), names longer than the slot are truncated
  CHECK(parseHeadPresetReply("1 -\n2 -\n3 EU-Narrow-Long-Name 869.6179999,62.5,8,8,16\n4 -\n5*X 915,250,11,5", l));
  STREQ(l.slot[2].name, "EU-Narrow-Lo");
  STREQ(l.slot[2].freq, "869.618");
  STREQ(l.slot[4].freq, "915");
  CHECK(l.slot[4].active);

  // all empty
  CHECK(parseHeadPresetReply("1 -\n2 -\n3 -\n4 -\n5 -", l));
  for (int i = 0; i < 5; i++) CHECK(!l.slot[i].used);
  CHECK(headPresetActiveSlot(l) == -1);

  // as captured by the head terminal log: header, "  " / "  -> " / "> " prefixes, CRLF, prompt after
  CHECK(parseHeadPresetReply(
      "\n> cmd get preset\n\n(direct) CLI -> from CaptCallahan-Base (round trip 412 ms)\n"
      "  1*USA-Canada 910.5250244,62.5,7,5\n2 Test 925.5,62.5,9,5\n3 -\n4 -\n5 -\n> ", l));
  CHECK(l.slot[0].active);
  STREQ(l.slot[1].name, "Test");
  CHECK(!l.slot[4].used);
  CHECK(parseHeadPresetReply("  -> pending, reverts in 3m; 1*A 910.5,62.5,7,5\r\n2 -\r\n3 -\r\n4 -\r\n5 B 920.125,62.5,7,5\r\n> ", l));
  CHECK(l.pending && l.pending_minutes == 3);
  STREQ(l.slot[0].name, "A");
  STREQ(l.slot[0].freq, "910.5");
  STREQ(l.slot[4].name, "B");
  STREQ(l.slot[4].freq, "920.125");
  CHECK(parseHeadPresetReply("> 1*A 910.5,62.5,7,5\n2 -\n3 -\n4 -\n5 -", l));

  // garbage and other replies
  CHECK(!parseHeadPresetReply("", l));
  CHECK(!parseHeadPresetReply(nullptr, l));
  CHECK(!parseHeadPresetReply("Error: unknown command", l));
  CHECK(!parseHeadPresetReply("OK - loading Test ...", l));
  CHECK(!parseHeadPresetReply("> cmd get preset\n", l));
  CHECK(!parseHeadPresetReply("1*A 910.5,62.5,7,5\n2 -\n3 -", l));        // truncated list
  CHECK(!parseHeadPresetReply("2 A 910.5,62.5,7,5\n1 -\n3 -\n4 -\n5 -", l));  // out of order
  CHECK(!parseHeadPresetReply("pending, reverts in Xm; garbage", l));

  // load replies
  CHECK(isLoadPresetOk("OK - loading Test ..."));
  CHECK(isLoadPresetOk("\n> cmd load preset 2\n(direct) CLI -> from R\n  OK - loading Test (reverts in 10m)\n> "));
  CHECK(!isLoadPresetOk("Error: preset 3 is empty"));
  CHECK(!isLoadPresetOk("> cmd load preset 2\n"));
  CHECK(!isLoadPresetOk(nullptr));

  // hazard matcher for typed commands
  CHECK(isLoadPresetCommand("load preset 2"));
  CHECK(isLoadPresetCommand("  load   preset 2"));
  CHECK(!isLoadPresetCommand("get preset"));
  CHECK(!isLoadPresetCommand("loadpreset 2"));
  CHECK(!isLoadPresetCommand(nullptr));

  // display frequency trimming
  char f[12];
  headPresetTrimFreq("910.5250244", f, sizeof(f)); STREQ(f, "910.525");
  headPresetTrimFreq("925.5", f, sizeof(f)); STREQ(f, "925.5");
  headPresetTrimFreq("869.6179999", f, sizeof(f)); STREQ(f, "869.618");
  headPresetTrimFreq("915.000", f, sizeof(f)); STREQ(f, "915");
  headPresetTrimFreq("abc", f, sizeof(f)); STREQ(f, "abc");
  if (failures) return 1;
  std::printf("PASS presets\n");
  return 0;
}
'''


class HeadHelpersTest(unittest.TestCase):
    def build_and_run(self, name, source, expected, extra_flags=()):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            self.skipTest('a host C++17 compiler is required')
        sanitize = [] if os.name == 'nt' else [
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        with tempfile.TemporaryDirectory(prefix='head-helpers-') as directory:
            work = Path(directory)
            cpp = work / (name + '.cpp')
            cpp.write_text(source, encoding='ascii')
            binary = work / (name + ('.exe' if os.name == 'nt' else ''))
            built = subprocess.run([
                compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize,
                *extra_flags,
                '-I', str(ROOT / 'src'), '-I', str(ROOT / 'test'), str(cpp), '-o', str(binary),
            ], capture_output=True, text=True, timeout=120, cwd=str(ROOT / 'test'))
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn(expected, ran.stdout)

    def test_dms_formatting(self):
        self.build_and_run('geo', GEO_HARNESS, 'PASS geo')

    def test_espnow_node_table(self):
        self.build_and_run('nodes', NODES_HARNESS, 'PASS nodes')

    def test_gps_switch_debounce(self):
        self.build_and_run('switch', SWITCH_HARNESS, 'PASS switch')

    def test_touch_keyboard_keys(self):
        # DisplayDriver.h (shared, pre-existing) has an unused parameter.
        self.build_and_run('keyboard', KEYBOARD_HARNESS.replace(
            '"../examples/companion_radio/ui-new/TouchKeyboard.h"',
            '"' + (ROOT / 'examples/companion_radio/ui-new/TouchKeyboard.h').as_posix() + '"'),
            'PASS keyboard', ('-Wno-unused-parameter',))

    def test_preset_reply_parsing(self):
        self.build_and_run('presets', PRESETS_HARNESS.replace(
            '"../examples/companion_radio/ui-new/HeadPresets.h"',
            '"' + (ROOT / 'examples/companion_radio/ui-new/HeadPresets.h').as_posix() + '"'),
            'PASS presets')


if __name__ == '__main__':
    unittest.main()
