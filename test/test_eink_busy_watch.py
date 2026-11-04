"""A stuck e-ink BUSY line must not reset the ESP32 repeater (review M2).

GxEPD2 waits up to 10 s per BUSY wait and a refresh makes several; with the
30 s loop watchdog a stuck line would reset the M5 at every refresh.

- EinkBusyWatch.h: detects a wait longer than 5 s, pauses refreshes for
  10 min (one event per stuck wait), healthy waits never pause.
- GxEPDDisplay: installs a busy callback that feeds the loop watchdog on
  ESP32 (each wait stays bounded by GxEPD2's own timeout), skips refreshes
  while paused without taking the frame as shown, and logs the event.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

CHECKS = r'''
#include <cassert>
#include <cstdio>
#include "helpers/ui/EinkBusyWatch.h"
using mesh::ui::EinkBusyWatch;
int main() {
  EinkBusyWatch w;
  // healthy full refresh: three waits of 2.5 s, gaps between them
  uint32_t t = 1000;
  for (int wait = 0; wait < 3; ++wait) {
    for (int i = 0; i < 2500; ++i) w.onBusy(t++);
    t += 300;
  }
  assert(w.stuckCount() == 0 && !w.takeStuckEvent() && w.refreshAllowed(t));
  // a stuck line: GxEPD2 waits 10 s per wait, three waits in a refresh
  for (int wait = 0; wait < 3; ++wait) {
    for (int i = 0; i < 10000; ++i) w.onBusy(t++);
    t += 5;   // the next wait follows at once
  }
  // consecutive callbacks less than 100 ms apart are one wait
  assert(w.stuckCount() == 1 && w.takeStuckEvent() && !w.takeStuckEvent());
  assert(!w.refreshAllowed(t) && !w.refreshAllowed(t + EinkBusyWatch::kPauseMs - 30001));
  // separate waits (a gap over 100 ms) each count once
  t += 1000;
  const uint32_t t0 = t;
  for (int i = 0; i <= 5000; ++i) w.onBusy(t++);   // flagged at t0 + 5000
  assert(w.stuckCount() == 2 && w.takeStuckEvent());
  const uint32_t resume = t0 + 5000 + EinkBusyWatch::kPauseMs;
  assert(!w.refreshAllowed(resume - 1) && w.refreshAllowed(resume) && w.refreshAllowed(resume + 1));
  // millis wrap
  EinkBusyWatch z; uint32_t u = 0xFFFFF000u;
  for (int i = 0; i < 6000; ++i) z.onBusy(u++);
  assert(z.stuckCount() == 1 && !z.refreshAllowed(u) && z.refreshAllowed(u + EinkBusyWatch::kPauseMs));
  printf("PASS eink\n");
  return 0;
}
'''


def text(path):
    return (ROOT / path).read_text().replace("\r\n", "\n")


class EinkBusyWatchTests(unittest.TestCase):
    def test_stuck_waits_pause_refreshes(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("a host C++ compiler is required")
        sanitize = [] if os.name == "nt" else ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        with tempfile.TemporaryDirectory(prefix="eink-busy-") as tmp:
            folder = Path(tmp)
            (folder / "t.cpp").write_text(CHECKS)
            built = subprocess.run([compiler, "-std=c++11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *sanitize,
                                    "-I", str(ROOT / "src"), str(folder / "t.cpp"), "-o", str(folder / "t")],
                                   capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(folder / "t")], capture_output=True, text=True, timeout=60)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("PASS eink", ran.stdout)

    def test_display_feeds_and_pauses(self):
        source = text("src/helpers/ui/GxEPDDisplay.cpp")
        callback = source[source.index("void GxEPDDisplay::onPanelBusy(const void* self) {"):]
        callback = callback[:callback.index("\n}\n")]
        self.assertIn("_busy_watch.onBusy(millis());", callback)
        self.assertIn("mesh::esp32::feedLoopWatchdog();", callback)
        header = text("src/helpers/ui/GxEPDDisplay.h")
        self.assertRegex(header, r"#if defined\(ESP32\) && !UI_POCKET_GUI\s+#define GXEPD_BUSY_WATCH 1")
        self.assertIn("delay(1);", callback)   # GxEPD2's own pacing without a callback
        begin = source[source.index("bool GxEPDDisplay::begin() {"):]
        self.assertLess(begin.index("display.epd2.setBusyCallback(onPanelBusy, this);"), begin.index("display.init("))
        end = source[source.index("void GxEPDDisplay::endFrame() {"):]
        end = end[end.index("#else"):end.index("last_display_crc_value = crc;")]
        self.assertLess(end.index("if (!_busy_watch.refreshAllowed(millis())) return;"), end.index("display.display(true);"))
        self.assertIn('if (_busy_watch.takeStuckEvent())', end)
        self.assertRegex(header, r"#if GXEPD_BUSY_WATCH\s+mesh::ui::EinkBusyWatch _busy_watch;")


if __name__ == "__main__":
    unittest.main()
