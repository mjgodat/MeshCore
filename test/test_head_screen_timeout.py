"""Repeater heads' screen timeout (ui-new/HeadScreenTimeout.h): the choices
30 s / 1 min / 2 min / 5 min / Never, the commands that apply and persist a
choice (DisplayPowerSettings "set display[.usb].mode|timeout"), the one-time
migration of the old 15 s default to 2 min, and the wiring: both heads'
envs default to 2 min, the carousel's Screen page cycles the choice.
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
#include <cstring>
#include "HeadScreenTimeout.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
using namespace head_screen;
using mesh::ui::DisplayMode;
using mesh::ui::DisplayPowerProfile;
int main() {
  CHECK(kDefaultSeconds == 120);
  char l[16];
  const char* labels[5] = {"30 s", "1 min", "2 min", "5 min", "Never"};
  for (int i = 0; i < kChoices; i++) { label(l, sizeof(l), i); CHECK(std::strcmp(l, labels[i]) == 0); }
  // Cycle: 30 s -> 1 min -> 2 min -> 5 min -> Never -> 30 s.
  int c = 0;
  for (int i = 1; i <= kChoices; i++) { c = nextChoice(c); CHECK(c == i % kChoices); }
  // A profile's choice.
  DisplayPowerProfile p; p.mode = DisplayMode::Button; p.seconds = 120;
  CHECK(choiceOf(p) == 2);
  p.seconds = 15; CHECK(choiceOf(p) == 0);
  p.seconds = 200; CHECK(choiceOf(p) == 3);
  p.seconds = 3600; CHECK(choiceOf(p) == 3);
  p.mode = DisplayMode::On; CHECK(choiceOf(p) == 4);
  // Commands: both profiles, Never = mode on, a timeout = button + seconds.
  char cmd[2][40];
  CHECK(commands(2, false, cmd) == 2);
  CHECK(std::strcmp(cmd[0], "set display.mode button") == 0 && std::strcmp(cmd[1], "set display.timeout 120") == 0);
  CHECK(commands(0, true, cmd) == 2);
  CHECK(std::strcmp(cmd[0], "set display.usb.mode button") == 0 && std::strcmp(cmd[1], "set display.usb.timeout 30") == 0);
  CHECK(commands(4, true, cmd) == 1 && std::strcmp(cmd[0], "set display.usb.mode on") == 0);
  // A choice applied to a profile.
  p.mode = DisplayMode::Button; p.seconds = 15;
  DisplayPowerProfile q = applied(p, 4);
  CHECK(q.mode == DisplayMode::On && q.seconds == 15);
  q = applied(q, 0);
  CHECK(q.mode == DisplayMode::Button && q.seconds == 30);
  p.mode = DisplayMode::Automatic; q = applied(p, 3);
  CHECK(q.mode == DisplayMode::Button && q.seconds == 300);
  // Migration: only a timed profile shorter than 2 min.
  p.mode = DisplayMode::Button; p.seconds = 15; CHECK(needsMigration(p));
  p.seconds = 120; CHECK(!needsMigration(p));
  p.seconds = 300; CHECK(!needsMigration(p));
  p.seconds = 15; p.mode = DisplayMode::On; CHECK(!needsMigration(p));
  p.mode = DisplayMode::Off; CHECK(!needsMigration(p));
  p.mode = DisplayMode::Automatic; CHECK(needsMigration(p));
  if (failures) return 1;
  std::printf("PASS screen_timeout\n");
  return 0;
}
'''


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


class ScreenTimeoutTest(unittest.TestCase):
    def test_choices_commands_migration(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            self.skipTest('a host C++17 compiler is required')
        sanitize = [] if os.name == 'nt' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        with tempfile.TemporaryDirectory(prefix='screen-timeout-') as tmp:
            cpp = Path(tmp) / 't.cpp'
            cpp.write_text(HARNESS, encoding='ascii')
            exe = Path(tmp) / ('t' + ('.exe' if os.name == 'nt' else ''))
            built = subprocess.run([compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize,
                                    '-I', str(UI_NEW), '-I', str(ROOT / 'src'), str(cpp), '-o', str(exe)],
                                   capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('PASS screen_timeout', ran.stdout)

    def test_wiring(self):
        # Both heads default to 2 min.
        for ini in (ROOT / 'variants' / 'cyd_espnow_head' / 'platformio.ini',
                    ROOT / 'variants' / 'freenove_s3_35_head' / 'platformio.ini'):
            self.assertRegex(ini.read_text(encoding='utf-8'), r'-D HEAD_DISPLAY_TIMEOUT_S=120', ini.parent.name)
        # The default reaches both display-settings loads.
        for f in (ROOT / 'examples' / 'companion_radio' / 'main.cpp', ROOT / 'examples' / 'companion_radio' / 'MyMesh.cpp'):
            src = f.read_bytes().decode('utf-8', 'replace')
            self.assertIn(', HEAD_DISPLAY_TIMEOUT_S', src, f.name)
        # The carousel's Screen page cycles and applies the choice.
        ui = strip_comments((UI_NEW / 'UITask.cpp').read_text(encoding='utf-8'))
        self.assertIn('HomePage::SCREEN', ui)
        self.assertIn('head_screen::nextChoice(', ui)
        # One save for both profiles (a write per command blocked the UI loop
        # for seconds on the Freenove).
        self.assertIn('head_screen::applied(', ui)
        self.assertIn('setDisplayPowerProfiles(', ui)
        # One-time migration on the visual heads (NVS flag).
        task = strip_comments((ROOT / 'examples' / 'companion_radio' / 'ui-freenove' / 'FreenoveUITask.cpp').read_text(encoding='utf-8'))
        self.assertIn('head_screen::needsMigration(', task)


if __name__ == '__main__':
    unittest.main()
