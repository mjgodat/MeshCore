"""Freenove visual head HMI behaviour (ui-freenove/HmiLogic.h): odometer
rolling, needle damping, noise ratings, rate trend, quick-control confirm,
clock fallback and the link icon. Host harness (g++, ASan/UBSan off
Windows)."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / 'examples' / 'companion_radio' / 'ui-freenove'

HARNESS = r'''
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "HmiLogic.h"
using namespace freenove_ui::hmi;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static void odometer_rolls() {
  // Digits and their roll like a mechanical counter.
  int d; float r;
  Odometer::digit(12847.0, 0, d, r); CHECK(d == 7 && r == 0.0f);
  Odometer::digit(12847.0, 1, d, r); CHECK(d == 4 && r == 0.0f);
  Odometer::digit(12847.0, 4, d, r); CHECK(d == 1 && r == 0.0f);
  Odometer::digit(12847.0, 5, d, r); CHECK(d == 0 && r == 0.0f);
  Odometer::digit(12847.25, 0, d, r); CHECK(d == 7 && std::fabs(r - 0.25f) < 1e-4f);
  Odometer::digit(12847.25, 1, d, r); CHECK(d == 4 && r == 0.0f);         // units not at 9: tens still
  Odometer::digit(12849.5, 1, d, r); CHECK(d == 4 && std::fabs(r - 0.5f) < 1e-4f);   // 49 -> 50 turns tens
  Odometer::digit(12899.5, 2, d, r); CHECK(d == 8 && std::fabs(r - 0.5f) < 1e-4f);   // 899 -> 900
  Odometer::digit(12899.5, 3, d, r); CHECK(d == 2 && r == 0.0f);
  // First value: shown at once; a change rolls over kRollMs, monotonic.
  Odometer o;
  CHECK(!o.rolling(0));
  o.set(12847, 1000);
  CHECK(o.shown(1000) == 12847.0 && !o.rolling(1000));
  o.set(12850, 2000);
  CHECK(o.rolling(2000) && o.shown(2000) == 12847.0);
  double last = 12847.0;
  int steps = 0;
  for (uint32_t t = 2000; t <= 2000 + Odometer::kRollMs; t += 16) {
    const double v = o.shown(t);
    CHECK(v >= last - 1e-9 && v <= 12850.0);
    if (v > last) steps++;
    last = v;
  }
  CHECK(steps > 20);                                   // smooth, not a jump
  CHECK(o.shown(2000 + Odometer::kRollMs) == 12850.0 && !o.rolling(2000 + Odometer::kRollMs));
  // A new count mid-roll continues from what is shown (no jump back).
  o.set(12860, 3000);
  const double mid = o.shown(3100);
  o.set(12861, 3100);
  CHECK(std::fabs(o.shown(3100) - mid) < 1e-9);
  // Restart / wrap / huge jump: no roll.
  o.set(5, 5000);
  CHECK(o.shown(5000) == 5.0 && !o.rolling(5000));
  o.set(999999 + 3, 6000);   // modulo six digits
  CHECK(o.target() == 2);
  o.set(50000, 7000);
  CHECK(!o.rolling(7000));
  if (!failures) std::printf("PASS odometer_rolls\n");
}

static void needle_damped() {
  Needle n;
  CHECK(!n.has() && n.update(0) == 0.0f);
  n.setTarget(-116.0f, 1000);
  CHECK(n.update(1000) == -116.0f && !n.settling());   // first reading: no sweep
  // Without a new target it never moves.
  for (uint32_t t = 1000; t < 4000; t += 16) CHECK(n.update(t) == -116.0f);
  n.setTarget(-100.0f, 4000);
  const float p1 = n.update(4016);
  CHECK(p1 > -116.0f && p1 < -113.0f);                 // eases, does not jump
  CHECK(n.settling());
  float last = p1;
  uint32_t t = 4016;
  for (; t < 8000 && n.settling(); t += 16) {
    const float p = n.update(t);
    CHECK(p >= last && p <= -100.0f);
    last = p;
  }
  CHECK(!n.settling() && n.position() == -100.0f && t < 4000 + 3000);
  if (!failures) std::printf("PASS needle_damped\n");
}

static void noise_thresholds() {
  CHECK(noiseRating(-125) == kNoiseQuiet && noiseRating(-108) == kNoiseQuiet);
  CHECK(noiseRating(-107) == kNoiseModerate && noiseRating(-96) == kNoiseModerate);
  CHECK(noiseRating(-95) == kNoiseNoisy && noiseRating(-60) == kNoiseNoisy);
  CHECK(std::strcmp(noiseRatingName(kNoiseQuiet), "quiet") == 0);
  CHECK(std::strcmp(noiseRatingName(kNoiseModerate), "moderate") == 0);
  CHECK(std::strcmp(noiseRatingName(kNoiseNoisy), "noisy") == 0);
  CHECK(gaugeAngle(-130) == 150.0f && gaugeAngle(-120) == 150.0f);
  CHECK(gaugeAngle(-90) == 390.0f && gaugeAngle(-50) == 390.0f);
  CHECK(std::fabs(gaugeAngle(-105) - 270.0f) < 1e-3f);   // straight up at mid scale
  if (!failures) std::printf("PASS noise_thresholds\n");
}

static void rate_trend() {
  uint16_t r[60];
  std::memset(r, 0, sizeof(r));
  int pct = 0;
  CHECK(!rateTrend(r, pct));
  for (int i = 53; i <= 57; i++) r[i] = 40;
  r[58] = 45;
  r[59] = 3;   // current minute: partial, ignored
  CHECK(rateTrend(r, pct) && pct == 13);
  r[58] = 30;
  CHECK(rateTrend(r, pct) && pct == -25);
  if (!failures) std::printf("PASS rate_trend\n");
}

static void quick_confirm() {
  QuickControls q;
  // ADVERT: first tap arms, nothing is sent.
  CHECK(q.tap(kTileAdvert, 1000) == kActNone);
  CHECK(q.confirming(kTileAdvert, 1000) && q.pending(1000));
  // A second tap inside the debounce gap is ignored (stays armed).
  CHECK(q.tap(kTileAdvert, 1000) == kActNone && q.confirming(kTileAdvert, 1000));
  CHECK(q.tap(kTileAdvert, 1000 + QuickControls::kMinConfirmGapMs - 1) == kActNone);
  CHECK(q.confirming(kTileAdvert, 1300));
  // Second tap after the gap, within the window: sends once.
  CHECK(q.tap(kTileAdvert, 2500) == kActAdvert);
  CHECK(!q.confirming(kTileAdvert, 2500) && !q.pending(2500));
  // Cooldown: 30 s with no arming at all.
  CHECK(q.cooldownLeftMs(kTileAdvert, 2500) == QuickControls::kAdvertCooldownMs);
  CHECK(q.tap(kTileAdvert, 3000) == kActNone && !q.pending(3000));
  CHECK(q.tap(kTileAdvert, 3500) == kActNone);
  CHECK(q.cooldownLeftMs(kTileAdvert, 2500 + QuickControls::kAdvertCooldownMs) == 0);
  uint32_t t = 2500 + QuickControls::kAdvertCooldownMs;
  // Timeout: the next tap arms again, never sends.
  CHECK(q.tap(kTileAdvert, t) == kActNone);
  CHECK(!q.confirming(kTileAdvert, t + QuickControls::kConfirmMs));
  CHECK(q.tap(kTileAdvert, t + QuickControls::kConfirmMs) == kActNone);   // re-armed only
  CHECK(q.confirming(kTileAdvert, t + QuickControls::kConfirmMs));
  // Cancel by tapping elsewhere (outside every tile, or another tile).
  t += 10000;
  CHECK(q.tap(-1, t) == kActNone && !q.pending(t));
  CHECK(q.tap(kTileAdvert, t + 1000) == kActNone);
  CHECK(q.tap(kTilePreset, t + 1400) == kActOpenPresets && !q.pending(t + 1400));
  CHECK(q.tap(kTileAdvert, t + 1800) == kActNone);   // armed again, not sent
  q.cancel();
  CHECK(!q.pending(t + 1800) && q.tap(kTileAdvert, t + 2400) == kActNone);
  // Navigation tiles act at once, to the relevant HEAD screen.
  QuickControls n;
  CHECK(n.tap(kTileObserver, 0) == kActOpenSettings && n.tap(kTileBridge, 0) == kActOpenSettings);
  CHECK(n.tap(kTileTrace, 0) == kActOpenHead && n.tap(kTileAdmin, 0) == kActAdminLogin);
  CHECK(n.tap(kTilePreset, 0) == kActOpenPresets);
  CHECK(!QuickControls::needsConfirm(kTileAdmin) && QuickControls::needsConfirm(kTileAdvert));
  CHECK(n.cooldownLeftMs(kTilePreset, 0) == 0);
  // Results show for kResultMs.
  n.setResult(kTileAdvert, true, 100);
  CHECK(n.result(kTileAdvert, 100) == 1 && n.result(kTileAdmin, 100) == -1);
  CHECK(n.result(kTileAdvert, 100 + QuickControls::kResultMs) == -1);
  if (!failures) std::printf("PASS quick_confirm\n");
}

static void clock_fallback() {
  char hm[16], date[24];
  // Never set: uptime, never a date.
  ClockTrack c;
  c.sample(1715770351u, 1000);   // the RTC's boot seed
  c.sample(1715770351u + 600, 601000);
  CHECK(!c.valid());
  formatClock(hm, sizeof(hm), date, sizeof(date), c.valid(), 1715770951u, 312000);
  CHECK(std::strcmp(hm, "T+00:05") == 0 && std::strcmp(date, "UPTIME") == 0);
  formatClock(hm, sizeof(hm), date, sizeof(date), false, 0, 101u * 3600u * 1000u);
  CHECK(std::strcmp(hm, "T+101h") == 0);
  // Synced (jumped forward) to a plausible date: the time.
  c.sample(1793454120u, 602000);   // 2026-10-31 13:42 UTC (a Saturday)
  CHECK(c.valid());
  formatClock(hm, sizeof(hm), date, sizeof(date), c.valid(), 1793454120u, 602000);
  CHECK(std::strcmp(hm, "13:42") == 0);
  CHECK(std::strcmp(date, "31 Oct UTC") == 0);
  // A jump to an implausible date does not count.
  ClockTrack b;
  b.sample(100, 0);
  b.sample(100000, 1000);
  CHECK(!b.valid());
  // Drift of a few seconds does not count either.
  ClockTrack d;
  d.sample(1793454120u, 0);
  d.sample(1793454120u + 3600 + 5, 3600000);
  CHECK(!d.valid());
  if (!failures) std::printf("PASS clock_fallback\n");
}

static void link_icon() {
  CHECK(linkLevel(false, 0) == 0);
  CHECK(linkLevel(true, 0) == 4 && linkLevel(true, 9999) == 4);
  CHECK(linkLevel(true, 10000) == 3 && linkLevel(true, 30000) == 2);
  CHECK(linkLevel(true, 60000) == 1 && linkLevel(true, 300000) == 0);
  if (!failures) std::printf("PASS link_icon\n");
}

int main(int argc, char** argv) {
  const char* n = argc > 1 ? argv[1] : "";
  if (!std::strcmp(n, "odometer_rolls")) odometer_rolls();
  else if (!std::strcmp(n, "needle_damped")) needle_damped();
  else if (!std::strcmp(n, "noise_thresholds")) noise_thresholds();
  else if (!std::strcmp(n, "rate_trend")) rate_trend();
  else if (!std::strcmp(n, "quick_confirm")) quick_confirm();
  else if (!std::strcmp(n, "clock_fallback")) clock_fallback();
  else if (!std::strcmp(n, "link_icon")) link_icon();
  else { std::printf("unknown %s\n", n); return 2; }
  return failures ? 1 : 0;
}
'''


class HmiLogicTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            raise unittest.SkipTest('a host C++17 compiler is required')
        cls.temp = tempfile.TemporaryDirectory(prefix='freenove-hmi-')
        work = Path(cls.temp.name)
        sanitize = [] if os.name == 'nt' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        cpp = work / 'hmi.cpp'
        cpp.write_text(HARNESS, encoding='ascii')
        cls.exe = work / ('hmi' + ('.exe' if os.name == 'nt' else ''))
        built = subprocess.run([compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize,
                                '-I', str(UI), str(cpp), '-o', str(cls.exe)],
                               capture_output=True, text=True, timeout=120)
        if built.returncode != 0:
            cls.temp.cleanup()
            raise AssertionError(built.stdout + built.stderr)
        # Also C++11 (the head may build as gnu++11).
        tu = work / 'hmi11.cpp'
        tu.write_text('#include "HmiLogic.h"\n', encoding='ascii')
        c11 = subprocess.run([compiler, '-std=c++11', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
                              '-I', str(UI), str(tu)], capture_output=True, text=True, timeout=60)
        if c11.returncode != 0:
            cls.temp.cleanup()
            raise AssertionError(c11.stdout + c11.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, name):
        ran = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=60)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('PASS ' + name, ran.stdout)

    def test_odometer_rolls(self):
        self.run_case('odometer_rolls')

    def test_needle_damped(self):
        self.run_case('needle_damped')

    def test_noise_thresholds(self):
        self.run_case('noise_thresholds')

    def test_rate_trend(self):
        self.run_case('rate_trend')

    def test_quick_confirm(self):
        self.run_case('quick_confirm')

    def test_clock_fallback(self):
        self.run_case('clock_fallback')

    def test_link_icon(self):
        self.run_case('link_icon')


if __name__ == '__main__':
    unittest.main()
