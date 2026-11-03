"""Freenove visual head: the always-visible labelled tab bar (navigation,
hit test, highlight, HEAD-page touch routing) and the repeater radio display
(HUD label, ESP-NOW-only heads never showing the placeholder LoRa radio).

Host harness (g++, ASan/UBSan off Windows) plus source contracts for the
device glue in FreenoveDisplay, FreenoveUITask and the companion UITask.
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / 'examples' / 'companion_radio' / 'ui-freenove'
UI_NEW = ROOT / 'examples' / 'companion_radio' / 'ui-new'
VARIANT = ROOT / 'variants' / 'freenove_s3_35_head'

HARNESS = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "PageNav.h"
#include "RadioLabel.h"
#include "StubPages.h"
#include "HeadRadio.h"
using namespace freenove_ui;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

struct Step { bool down; int x, y; uint32_t t; };
static NavEvent run(PageNav& nav, const std::vector<Step>& steps, bool passThrough, std::vector<NavEvent>* all = nullptr) {
  NavEvent last; last.kind = NavEvent::None; last.x = last.y = -1;
  for (const Step& s : steps) {
    NavEvent e = nav.update(s.down, s.x, s.y, s.t, passThrough);
    if (all) all->push_back(e);
    if (e.kind != NavEvent::None) last = e;
  }
  return last;
}
static std::vector<Step> tap(int x, int y, uint32_t t) {
  return std::vector<Step>{{true, x, y, t}, {true, x + 1, y, t + 40}, {false, -1, -1, t + 130}};
}
static std::vector<Step> drag(int x0, int y0, int x1, int y1, uint32_t t0, uint32_t ms) {
  std::vector<Step> s;
  const int n = 8;
  for (int i = 0; i <= n; i++) s.push_back({true, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, t0 + ms * i / n});
  s.push_back({false, -1, -1, t0 + ms + 90});
  return s;
}

static void tabbar_hit_test() {
  PageNav nav(PageSet::kCount);
  CHECK(kTabBarH == 24);
  CHECK(nav.barY() == 296);
  for (int i = 0; i < PageSet::kCount; i++) {
    const int x0 = nav.tabX(i), x1 = nav.tabX(i + 1);
    CHECK(x1 - x0 >= 96);   // >= 96 x 24 px tap targets
    for (int y = nav.barY(); y < 320; y += 5) {
      CHECK(nav.tabAt(x0, y) == i);
      CHECK(nav.tabAt(x1 - 1, y) == i);
      CHECK(nav.tabAt((x0 + x1) / 2, y) == i);
    }
  }
  CHECK(nav.tabX(PageSet::kCount) == 480);
  CHECK(nav.tabAt(479, 319) == PageSet::kCount - 1);
  // Outside the bar: none.
  CHECK(nav.tabAt(100, 295) == -1);
  CHECK(nav.tabAt(100, 0) == -1);
  CHECK(nav.tabAt(100, 320) == -1);
  CHECK(nav.tabAt(-1, 300) == -1);
  CHECK(nav.tabAt(480, 300) == -1);
  // Tapping each tab switches to its page, from any page, on visual and
  // pass-through (HEAD) pages alike; the tap is never forwarded.
  uint32_t t = 0;
  for (int from = 0; from < PageSet::kCount; from++) {
    for (int to = 0; to < PageSet::kCount; to++) {
      for (int pass = 0; pass < 2; pass++) {
        nav.setCurrent(from);
        std::vector<NavEvent> evs;
        const NavEvent e = run(nav, tap(nav.tabX(to) + 30, 308, t += 1000), pass != 0, &evs);
        CHECK(nav.current() == to);
        CHECK(e.kind == (to == from ? NavEvent::None : NavEvent::PageChanged));
        for (const NavEvent& ev : evs) CHECK(ev.kind != NavEvent::Tap && ev.kind != NavEvent::Press);
      }
    }
  }
  // A press that slips onto another tab before release cancels.
  nav.setCurrent(0);
  std::vector<Step> slip = {{true, nav.tabX(1) + 90, 300, 90000}, {true, nav.tabX(2) + 4, 300, 90030},
                            {false, -1, -1, 90120}};
  CHECK(run(nav, slip, false).kind == NavEvent::None && nav.current() == 0);
}

static void tabbar_current_highlight() {
  std::vector<uint16_t> px(480 * 320);
  FrameBuffer565 fb(px.data());
  PageNav nav(PageSet::kCount);
  const char* labels[] = {"HOME", "TELEMETRY", "MAP", "NODES", "HEAD"};
  for (int i = 0; i < PageSet::kCount; i++) CHECK(std::strcmp(tabLabel(i), labels[i]) == 0);
  CHECK(std::strcmp(tabLabel(PageSet::kCount), "") == 0);
  const uint16_t kSentinel = 0xF81F;
  for (int cur = 0; cur < PageSet::kCount; cur++) {
    fb.clear(kSentinel);   // the bar must not touch the area above it
    drawTabBar(fb, cur, PageSet::kCount);
    for (int y = 0; y < nav.barY(); y++)
      for (int x = 0; x < 480; x++) CHECK(px[y * 480 + x] == kSentinel);
    for (int y = nav.barY(); y < 320; y++)
      for (int x = 0; x < 480; x++) CHECK(px[y * 480 + x] != kSentinel);   // own backdrop strip
    for (int i = 0; i < PageSet::kCount; i++) {
      // Tile interior next to its left edge (no glyphs there): teal for the
      // current tab only, navy for the others.
      const uint16_t fill = px[(nav.barY() + 10) * 480 + nav.tabX(i) + 8];
      CHECK(fill == (i == cur ? theme::kTabActiveFill : theme::kTabFill));
      int active = 0, plain = 0, text = 0;
      for (int y = nav.barY(); y < 320; y++) {
        for (int x = nav.tabX(i); x < nav.tabX(i + 1); x++) {
          const uint16_t c = px[y * 480 + x];
          active += c == theme::kTabActiveOutline;
          plain += c == theme::kTabOutline;
          if (x > nav.tabX(i) + 20 && x < nav.tabX(i + 1) - 20 && y > nav.barY() + 3 && y < nav.barY() + 17)
            text += c != fill;
        }
      }
      if (i == cur) CHECK(active > 100 && plain == 0);   // cyan edge (+ cyan label)
      else CHECK(plain > 100 && active == 0);
      CHECK(text > 15);   // the label
    }
  }
  // Page chrome = backdrop + page background + the tab bar for that page.
  PageSet set;
  fb.clear(kSentinel);
  drawPageChrome(fb, *set.page(PageSet::kMap), PageSet::kMap, PageSet::kCount);
  CHECK(px[(nav.barY() + 10) * 480 + nav.tabX(PageSet::kMap) + 8] == theme::kTabActiveFill);
  CHECK(px[(nav.barY() + 10) * 480 + nav.tabX(PageSet::kHome) + 8] == theme::kTabFill);
  for (uint16_t c : px) CHECK(c != kSentinel);   // the backdrop covers everything
  // Header (dynamic layer): only the header rows.
  mesh::head::MeshModel model(4, 4);
  PageContext ctx; std::memset(&ctx, 0, sizeof(ctx)); ctx.model = &model;
  std::snprintf(ctx.clock_hm, sizeof(ctx.clock_hm), "T+00:05");
  std::snprintf(ctx.clock_date, sizeof(ctx.clock_date), "UPTIME");
  fb.clear(kSentinel);
  drawHeader(fb, ctx, 1000);
  int inside = 0, below = 0;
  for (int y = 0; y < 320; y++) for (int x = 0; x < 480; x++)
    if (px[y * 480 + x] != kSentinel) { inside += y < kHeaderH; below += y >= kHeaderH; }
  CHECK(inside > 300 && below == 0);
}

static void swipe_still_navigates() {
  PageNav nav(PageSet::kCount);
  // Visual pages: swipe above the bar, left = next, right = previous.
  CHECK(run(nav, drag(300, 150, 180, 150, 0, 200), false).kind == NavEvent::PageChanged && nav.current() == 1);
  CHECK(run(nav, drag(150, 150, 300, 150, 1000, 200), false).kind == NavEvent::PageChanged && nav.current() == 0);
  CHECK(run(nav, drag(150, 150, 300, 150, 2000, 200), false).kind == NavEvent::None && nav.current() == 0);
  // A swipe that starts in the bar also turns the page...
  CHECK(run(nav, drag(300, 308, 150, 308, 3000, 200), false).kind == NavEvent::PageChanged && nav.current() == 1);
  // ...and is the swipe shortcut on HEAD (pass-through) as well.
  nav.setCurrent(PageSet::kHead);
  CHECK(run(nav, drag(150, 308, 330, 308, 4000, 200), true).kind == NavEvent::PageChanged
        && nav.current() == PageSet::kNodes);
  nav.setCurrent(PageSet::kHead);
  // Above the bar on HEAD a horizontal drag is the companion's, not a page turn.
  std::vector<NavEvent> evs;
  run(nav, drag(330, 150, 100, 150, 5000, 150), true, &evs);
  CHECK(nav.current() == PageSet::kHead);
  CHECK(evs.front().kind == NavEvent::Press && evs.back().kind == NavEvent::Release);
}

static void head_touch_routing() {
  PageNav nav(PageSet::kCount, PageSet::kHead);
  CHECK(320 - kTabBarH == 296);
  // y < 296: the companion gets the whole gesture.
  for (int y : {0, 100, 200, 295}) {
    std::vector<NavEvent> evs;
    run(nav, tap(240, y, 1000u + y * 10), true, &evs);
    CHECK(evs.front().kind == NavEvent::Press && evs.front().y == y);
    CHECK(evs.back().kind == NavEvent::Release);
    CHECK(nav.current() == PageSet::kHead);
  }
  // ...even when the finger slides onto the bar afterwards.
  std::vector<NavEvent> evs;
  run(nav, drag(240, 280, 240, 310, 9000, 100), true, &evs);
  CHECK(evs.front().kind == NavEvent::Press && evs.back().kind == NavEvent::Release);
  CHECK(nav.current() == PageSet::kHead);
  // y >= 296: the navigator; nothing reaches the companion.
  for (int y : {296, 300, 319}) {
    evs.clear();
    nav.setCurrent(PageSet::kHead);
    run(nav, tap(nav.tabX(PageSet::kTelemetry) + 20, y, 20000u + y * 10), true, &evs);
    for (const NavEvent& e : evs) CHECK(e.kind != NavEvent::Press && e.kind != NavEvent::Move
                                        && e.kind != NavEvent::Release && e.kind != NavEvent::Tap);
    CHECK(nav.current() == PageSet::kTelemetry);
  }
  // Tapping HEAD's own tab is a no-op.
  nav.setCurrent(PageSet::kHead);
  CHECK(run(nav, tap(nav.tabX(PageSet::kHead) + 20, 310, 30000), true).kind == NavEvent::None);
}

static void radio_label_format() {
  char out[48];
  mesh::observer::Vitals v; std::memset(&v, 0, sizeof(v));
  v.freq = 910.5250244f; v.bw = 62.5f; v.sf = 7; v.cr = 5;
  CHECK(formatRepeaterRadio(out, sizeof(out), &v));
  CHECK(std::strcmp(out, "M5 910.525 MHZ SF7 BW62.5") == 0);
  v.freq = 869.618f; v.bw = 250.0f; v.sf = 8;
  CHECK(formatRepeaterRadio(out, sizeof(out), &v) && std::strcmp(out, "M5 869.618 MHZ SF8 BW250") == 0);
  v.freq = 433.0f; v.bw = 125.0f; v.sf = 12;
  CHECK(formatRepeaterRadio(out, sizeof(out), &v) && std::strcmp(out, "M5 433.000 MHZ SF12 BW125") == 0);
  // No vitals (or vitals without a sane radio): never a made-up frequency.
  CHECK(!formatRepeaterRadio(out, sizeof(out), nullptr) && std::strcmp(out, "NO REPEATER DATA") == 0);
  v.freq = 0.0f;
  CHECK(!formatRepeaterRadio(out, sizeof(out), &v) && std::strcmp(out, "NO REPEATER DATA") == 0);
  v.freq = NAN;
  CHECK(!formatRepeaterRadio(out, sizeof(out), &v) && std::strcmp(out, "NO REPEATER DATA") == 0);
  v.freq = 910.525f; v.sf = 0;
  CHECK(!formatRepeaterRadio(out, sizeof(out), &v));
  // Truncates safely.
  char tiny[6];
  v.sf = 7;
  formatRepeaterRadio(tiny, sizeof(tiny), &v);
  CHECK(std::strlen(tiny) == 5);
  // Header parts: frequency and "SF7 . BW62.5" (0xB7 middle dot).
  char freq[32], detail[40];
  v.freq = 910.5250244f; v.bw = 62.5f; v.sf = 7;
  CHECK(formatRadioParts(freq, sizeof(freq), detail, sizeof(detail), &v));
  CHECK(std::strcmp(freq, "910.525") == 0 && std::strcmp(detail, "SF7 \xB7 BW62.5") == 0);
  CHECK(!formatRadioParts(freq, sizeof(freq), detail, sizeof(detail), nullptr) && freq[0] == 0 && detail[0] == 0);
  // Header placement: the frequency right-aligned at x 300, in the header rows.
  std::vector<uint16_t> px(480 * 320, 0);
  FrameBuffer565 fb(px.data());
  mesh::head::MeshModel model(4, 4);
  PageContext ctx; std::memset(&ctx, 0, sizeof(ctx)); ctx.model = &model;
  model.onVitals(v, 1000);
  drawHeader(fb, ctx, 2000);
  int accent = 0, below = 0;
  for (int y = 0; y < 320; y++)
    for (int x = 0; x < 480; x++) {
      const uint16_t c = px[y * 480 + x];
      if (c == theme::kAccent) { accent++; below += y >= kHeaderH || (x > 330 && x < 400); }
    }
  CHECK(accent > 40 && below == 0);

  // Aged on the head clock: live for 60 s after the vitals, then dimmed
  // "(STALE)"; wrap-safe across the 32-bit millis() rollover.
  CHECK(formatRepeaterRadio(out, sizeof(out), &v, 1000, 1000) == kRadioLive);
  CHECK(std::strcmp(out, "M5 910.525 MHZ SF7 BW62.5") == 0);
  CHECK(formatRepeaterRadio(out, sizeof(out), &v, 1000, 1000 + kRadioStaleMs - 1) == kRadioLive);
  CHECK(formatRepeaterRadio(out, sizeof(out), &v, 1000, 1000 + kRadioStaleMs) == kRadioStale);
  CHECK(std::strcmp(out, "M5 910.525 MHZ SF7 BW62.5 (STALE)") == 0);
  const uint32_t nearWrap = 0xFFFFFFFFu - 5000u;
  CHECK(formatRepeaterRadio(out, sizeof(out), &v, nearWrap, nearWrap + 30000u) == kRadioLive);
  CHECK(formatRepeaterRadio(out, sizeof(out), &v, nearWrap, nearWrap + 61000u) == kRadioStale);
  CHECK(formatRepeaterRadio(out, sizeof(out), nullptr, 0, 999999) == kRadioNone);
  CHECK(std::strcmp(out, "NO REPEATER DATA") == 0);
  // Stale never overflows a short buffer.
  CHECK(formatRepeaterRadio(tiny, sizeof(tiny), &v, 0, 999999) == kRadioStale && std::strlen(tiny) == 5);
  // Colours: live = the caller's, stale = dimmed, none = amber.
  CHECK(radioColour(kRadioLive, theme::kText) == theme::kText);
  CHECK(radioColour(kRadioStale, theme::kText) == kDimText && kDimText != theme::kText);
  CHECK(radioColour(kRadioNone, theme::kText) == theme::kWarn);
  std::fill(px.begin(), px.end(), 0);
  drawHeader(fb, ctx, 1000 + kRadioStaleMs + 5);
  int dim = 0, bright = 0;
  for (int y = 0; y < kHeaderH; y++) for (int x = 236; x < 330; x++) {
    dim += px[y * 480 + x] == kDimText;
    bright += px[y * 480 + x] == theme::kAccent;
  }
  CHECK(dim > 30 && bright == 0);
  // HOME's noise reading goes stale on the same clock.
  HomePage home; home.attach(&ctx);
  auto numberDim = [&]() {
    int n = 0;
    for (int y = 60; y < 92; y++) for (int x = 310; x < 380; x++) n += px[y * 480 + x] == kDimText;
    return n;
  };
  std::fill(px.begin(), px.end(), 0);
  home.onShow(2000); home.draw(fb, 2000);
  const int liveDim = numberDim();
  std::fill(px.begin(), px.end(), 0);
  home.draw(fb, 1000 + kRadioStaleMs + 5);
  const int staleDim = numberDim();
  CHECK(liveDim == 0 && staleDim > 20);
}

static int sourceCalls = 0;
static bool sourceKnown = false;
static bool testSource(mesh::ui::RepeaterRadio& out) {
  sourceCalls++;
  if (!sourceKnown) return false;
  out.freq = 910.5250244f; out.bw = 62.5f; out.sf = 7; out.cr = 5;
  return true;
}

static void espnow_head_never_shows_placeholder() {
  char l1[48], l2[48];
  // CYD head: no source registered -> "ESP-NOW", no frequency at all.
  mesh::ui::setRepeaterRadioSource(nullptr);
  mesh::ui::formatEspNowHeadRadio(l1, sizeof(l1), l2, sizeof(l2));
  CHECK(std::strcmp(l1, "Radio: ESP-NOW") == 0 && std::strcmp(l2, "no repeater data") == 0);
  CHECK(std::strpbrk(l1, "0123456789") == nullptr && std::strpbrk(l2, "0123456789") == nullptr);
  // Freenove head before vitals: same.
  mesh::ui::setRepeaterRadioSource(testSource);
  sourceKnown = false;
  mesh::ui::formatEspNowHeadRadio(l1, sizeof(l1), l2, sizeof(l2));
  CHECK(sourceCalls == 1);
  CHECK(std::strcmp(l1, "Radio: ESP-NOW") == 0 && std::strcmp(l2, "no repeater data") == 0);
  // ...and once the repeater's radio is known, that radio.
  sourceKnown = true;
  mesh::ui::formatEspNowHeadRadio(l1, sizeof(l1), l2, sizeof(l2));
  CHECK(std::strcmp(l1, "Repeater 910.525 MHz") == 0);
  CHECK(std::strcmp(l2, "SF7 BW62.5 CR5") == 0);
  mesh::ui::setRepeaterRadioSource(nullptr);
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  std::string name = argv[1];
  if (name == "tabbar_hit_test") tabbar_hit_test();
  else if (name == "tabbar_current_highlight") tabbar_current_highlight();
  else if (name == "swipe_still_navigates") swipe_still_navigates();
  else if (name == "head_touch_routing") head_touch_routing();
  else if (name == "radio_label_format") radio_label_format();
  else if (name == "espnow_head_never_shows_placeholder") espnow_head_never_shows_placeholder();
  else { std::printf("unknown scenario %s\n", argv[1]); return 2; }
  if (failures) return 1;
  std::printf("PASS %s\n", argv[1]);
  return 0;
}
'''


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def function_body(text, signature_regex):
    m = re.search(signature_regex, text)
    if m is None:
        raise AssertionError('function not found: ' + signature_regex)
    start = text.index('{', m.end())
    depth = 0
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise AssertionError('unbalanced body')


def espnow_branches(lines):
    """Yield (line_no, line, frames) where frames is the #if stack as
    [(condition, in_else)]."""
    stack = []
    for no, line in enumerate(lines, 1):
        d = line.strip()
        if re.match(r'#\s*if', d):
            stack.append([d, False])
        elif re.match(r'#\s*el(se|if)', d) and stack:
            stack[-1][1] = True
            if d.startswith('#elif'):
                stack[-1][0] = d
        elif re.match(r'#\s*endif', d) and stack:
            stack.pop()
        yield no, line, [tuple(f) for f in stack]


class TabBarTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            raise unittest.SkipTest('a host C++17 compiler is required')
        sanitize = [] if os.name == 'nt' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        cls.temp = tempfile.TemporaryDirectory(prefix='freenove-tabbar-')
        work = Path(cls.temp.name)
        cpp = work / 'tabbar.cpp'
        cpp.write_text(HARNESS, encoding='ascii')
        cls.exe = work / ('tabbar' + ('.exe' if os.name == 'nt' else ''))
        sources = [UI / 'StubPages.cpp', UI / 'TelemetryPage.cpp', UI / 'MapPage.cpp', UI / 'NodesPage.cpp', UI / 'NodeCard.cpp', UI / 'HomePage.cpp', UI / 'MeshModel.cpp', UI / 'FrameBuffer332.cpp', UI / 'FbFonts.cpp',
                   UI / 'FrameBuffer565.cpp', UI / 'FbFontsAA.cpp',
                   ROOT / 'src' / 'Packet.cpp', ROOT / 'src' / 'helpers' / 'AdvertDataHelpers.cpp']
        built = subprocess.run([
            compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', *sanitize,
            '-I', str(UI), '-I', str(UI_NEW), '-isystem', str(ROOT / 'src'), '-isystem', str(ROOT / 'test' / 'mocks'),
            str(cpp), *[str(s) for s in sources], '-o', str(cls.exe),
        ], capture_output=True, text=True, timeout=300, cwd=str(ROOT / 'test'))
        if built.returncode != 0:
            cls.temp.cleanup()
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_scenario(self, name):
        ran = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=60)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('PASS ' + name, ran.stdout)

    def test_tabbar_hit_test(self):
        self.run_scenario('tabbar_hit_test')

    def test_tabbar_current_highlight(self):
        self.run_scenario('tabbar_current_highlight')

    def test_swipe_still_navigates(self):
        self.run_scenario('swipe_still_navigates')

    def test_head_touch_routing(self):
        self.run_scenario('head_touch_routing')
        # Device side: the companion's LGFX device is the 480x296 area above
        # the bar, and its touches map from that area alone.
        header = strip_comments((VARIANT / 'FreenoveDisplay.h').read_text(encoding='ascii'))
        self.assertRegex(header, r'kCompanionH\s*=\s*kLandscapeH\s*-\s*freenove_ui::kTabBarH')
        src = strip_comments((VARIANT / 'FreenoveDisplay.cpp').read_text(encoding='ascii'))
        self.assertIn('LGFXDisplay(freenove::kLandscapeW, freenove::kCompanionH, _dev)', src)
        self.assertIn('cfg.panel_height = kCompanionH;', src)
        self.assertNotIn('kLandscapeH', function_body(src, r'void Panel_Fb332::_fill_rect_inner\('))
        touch = function_body(src, r'bool FreenoveDisplay::getTouch\(')
        self.assertRegex(touch, r'panelTouchToLogical\([^;]*freenove::kCompanionH')
        # The HEAD page's overlay is the tab bar (no page dots, no edge strips).
        ui = strip_comments((UI / 'FreenoveUITask.cpp').read_text(encoding='ascii'))
        overlay = function_body(ui, r'void FreenoveUITask::drawHeadOverlay\(')
        self.assertIn('drawTabBar(', overlay)
        nav = strip_comments((UI / 'PageNav.h').read_text(encoding='ascii'))
        for gone in ('edgePx', 'dotAt', 'dotsY', 'drawPageDots'):
            self.assertNotIn(gone, nav + ui)

    def test_radio_label_format(self):
        self.run_scenario('radio_label_format')
        ui = strip_comments((UI / 'FreenoveUITask.cpp').read_text(encoding='ascii'))
        # The HUD label is re-evaluated on time as well as on new data.
        self.assertIn('vitalsMs()', function_body(ui, r'void FreenoveUITask::updateRadio\('))
        render = function_body(ui, r'void FreenoveUITask::renderFrame\(')
        self.assertIn('drawHeader(', render)
        # HOME's noise reading and rate age on the same clock as the header.
        home = strip_comments((UI / 'HomePage.cpp').read_text(encoding='ascii'))
        self.assertIn('kRadioStaleMs', function_body(home, r'void HomePage::drawNoise\('))
        self.assertIn('kRadioStaleMs', function_body(home, r'void HomePage::drawPackets\('))

    def test_espnow_head_never_shows_placeholder(self):
        self.run_scenario('espnow_head_never_shows_placeholder')
        raw = (UI_NEW / 'UITask.cpp').read_bytes()
        self.assertNotIn(b'\n', raw.replace(b'\r\n', b''), 'UITask.cpp must stay CRLF')
        lines = raw.decode('ascii').split('\r\n')
        own_radio = re.compile(r'MHz|FQ:|F:%06|B:%03|BW: %03')
        espnow = 'MESH_PRIMARY_ESPNOW'
        checked = 0
        helper_calls = 0
        for no, line, frames in espnow_branches(lines):
            code = line.split('//')[0]
            if 'formatEspNowHeadRadio(' in code:
                helper_calls += 1
                self.assertTrue(any(espnow in c and not e for c, e in frames),
                                'line %d: helper outside the MESH_PRIMARY_ESPNOW branch' % no)
            if own_radio.search(code) and ('freq' in code or 'bw' in code):
                checked += 1
                self.assertTrue(any(espnow in c and e for c, e in frames),
                                'line %d shows the head\'s own LoRa radio on ESP-NOW heads: %s' % (no, line.strip()))
        self.assertGreaterEqual(checked, 4, 'radio lines not found (pattern drift?)')
        self.assertEqual(helper_calls, 2, 'pocket and classic radio pages')
        # HeadRadio.h is included for every ESP-NOW-only build, not only
        # UI_REPEATER_HEAD ones (SenseCAP Indicator ESP-NOW has no head UI).
        include = [f for n, l, f in espnow_branches(lines) if '#include "HeadRadio.h"' in l]
        self.assertEqual(len(include), 1)
        self.assertTrue(any(espnow in c and not e for c, e in include[0]))
        self.assertFalse(any('UI_REPEATER_HEAD' in c for c, e in include[0]))
        # The Freenove head registers the observer-vitals source.
        ui = strip_comments((UI / 'FreenoveUITask.cpp').read_text(encoding='ascii'))
        self.assertIn('setRepeaterRadioSource(companionRepeaterRadio)', ui)
        self.assertIn('portENTER_CRITICAL(&s_radioMux)', function_body(ui, r'bool companionRepeaterRadio\('))


class CarryInContractTest(unittest.TestCase):
    """Task 6 review carry-ins."""

    def test_dropped_submit_forces_next_frame(self):
        ui = strip_comments((UI / 'FreenoveUITask.cpp').read_text(encoding='ascii'))
        render = function_body(ui, r'void FreenoveUITask::renderFrame\(')
        self.assertRegex(render, r'if\s*\(\s*!_display\.submitPage\(fb\)\s*\)\s*_forceFrame\s*=\s*true;')
        # ...and nothing after it clears the flag again.
        self.assertNotIn('_forceFrame = false', render[render.index('submitPage('):])

    def test_companion_skips_push_wait_when_not_owner(self):
        src = strip_comments((VARIANT / 'FreenoveDisplay.cpp').read_text(encoding='ascii'))
        for fn in ('endFrame', 'startFrame'):
            body = function_body(src, r'void FreenoveDisplay::%s\(' % fn)
            self.assertRegex(body, r'if\s*\(\s*panelOwner\(\)\s*==\s*PanelOwner::Companion\s*\)\s*waitForPush\(\);')


if __name__ == '__main__':
    unittest.main()
