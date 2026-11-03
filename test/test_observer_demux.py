"""Freenove visual head: observer demux on the ESP-NOW receive path and the
page framework's pure logic (swipe/dot navigation, touch relay, hit tests).

Host harness (g++, ASan/UBSan off Windows) plus source contracts for the
device glue in ESPNOWRadio.cpp, FreenoveDisplay and FreenoveUITask.
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
VARIANT = ROOT / 'variants' / 'freenove_s3_35_head'

HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "helpers/bridges/ObserverDemux.h"
#include "PageNav.h"
#include "HitTest.h"
using namespace mesh::observer;
using namespace freenove_ui;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

typedef std::vector<uint8_t> Bytes;

struct Sink { int calls = 0; std::vector<Bytes> frames; };
static void onObserver(const uint8_t* d, size_t n, void* ctx) {
  Sink* s = static_cast<Sink*>(ctx);
  s->calls++;
  s->frames.push_back(Bytes(d, d + n));
}

// The receive path as OnDataRecv runs it: the demux first, then the MeshCore
// receive queue (byte copies) for everything it does not take.
static void receive(const Bytes& f, FrameHandler h, void* ctx, std::vector<Bytes>& meshQueue) {
  if (!f.empty() && demuxObserverFrame(f.data(), f.size(), h, ctx)) return;
  meshQueue.push_back(f);
}

static Bytes packetFrame(uint8_t len, uint8_t seed) {
  PacketRecord p; std::memset(&p, 0, sizeof(p));
  p.rx_ms = 1234u + seed; p.snr_q4 = (int8_t)(seed - 20); p.rssi = -100; p.disp = Disposition::Forwarded;
  p.len = len;
  for (int i = 0; i < len; i++) p.raw[i] = (uint8_t)(i * 3 + seed);
  uint8_t buf[256];
  size_t n = encodePacket(p, buf, sizeof(buf));
  return Bytes(buf, buf + n);
}
static Bytes vitalsFrame() {
  Vitals v; std::memset(&v, 0, sizeof(v));
  v.uptime_s = 99; v.noise_floor = -117; v.rx_per_min = 4; v.batt_mv = 4000; v.freq = 910.525f; v.bw = 62.5f; v.sf = 7; v.cr = 5;
  uint8_t buf[256];
  size_t n = encodeVitals(v, buf, sizeof(buf));
  return Bytes(buf, buf + n);
}

// Frames the head's MeshCore/WiChat parsers must keep receiving untouched.
static std::vector<Bytes> meshCorpus() {
  std::vector<Bytes> c;
  uint32_t rng = 0x12345678u;
  for (int first = 0; first < 256; first++) {
    const size_t lens[] = {1, 2, 15, 16, 41, 64, 120, 184, 235, 249, 250};
    for (size_t len : lens) {
      Bytes f(len);
      for (size_t i = 0; i < len; i++) { rng = rng * 1103515245u + 12345u; f[i] = (uint8_t)(rng >> 16); }
      f[0] = (uint8_t)first;
      c.push_back(f);
    }
  }
  // Raw-mode fragments (FE 4D 43 46 ...) and WiChat-like frames (magic at 16/24).
  for (int k = 0; k < 8; k++) {
    Bytes frag(40 + k * 20, (uint8_t)k); frag[0] = 0xFE; frag[1] = 0x4D; frag[2] = 0x43; frag[3] = 0x46;
    c.push_back(frag);
    Bytes wichat(60 + k, 0x11); wichat[16] = 0xB0; wichat[17] = 0x42; wichat[24] = 0x53;
    c.push_back(wichat);
  }
  // Near misses of the observer header: right magic, wrong version / kind / length.
  Bytes pkt = packetFrame(30, 7), vit = vitalsFrame();
  Bytes badVersion = pkt; badVersion[4] = 2; c.push_back(badVersion);
  Bytes badKind = pkt; badKind[5] = 9; c.push_back(badKind);
  Bytes longer = pkt; longer.push_back(0); c.push_back(longer);
  Bytes shorter = pkt; shorter.pop_back(); c.push_back(shorter);
  Bytes vitLong = vit; vitLong.push_back(0); c.push_back(vitLong);
  Bytes vitShort = vit; vitShort.pop_back(); c.push_back(vitShort);
  Bytes headerOnly(pkt.begin(), pkt.begin() + 6); c.push_back(headerOnly);
  Bytes magicOnly(pkt.begin(), pkt.begin() + 4); c.push_back(magicOnly);
  return c;
}

static int observer_frames_bypass_meshcore_parser() {
  Sink sink; std::vector<Bytes> mesh;
  std::vector<Bytes> sent;
  const uint8_t lens[] = {1, 30, 184, 235};
  for (uint8_t len : lens) sent.push_back(packetFrame(len, len));
  sent.push_back(vitalsFrame());
  for (const Bytes& f : sent) {
    CHECK(isObserverFrame(f.data(), f.size()));
    receive(f, onObserver, &sink, mesh);
  }
  CHECK(mesh.empty());                       // MeshCore never saw one
  CHECK(sink.calls == (int)sent.size());
  CHECK(sink.frames == sent);                // handed over byte for byte, in order

  // The UI-side queue: bounded, drop-oldest, whole frames.
  FrameRing<4> ring;
  CHECK(ring.count() == 0 && ring.dropped() == 0);
  uint8_t out[FrameRing<4>::MAX_FRAME]; size_t n = 0;
  CHECK(!ring.pop(out, sizeof(out), n));
  for (int i = 0; i < 6; i++) CHECK(ring.push(packetFrame((uint8_t)(10 + i), (uint8_t)i).data(), packetFrame((uint8_t)(10 + i), (uint8_t)i).size()));
  CHECK(ring.count() == 4 && ring.dropped() == 2 && ring.pushed() == 6);
  for (int i = 2; i < 6; i++) {
    Bytes want = packetFrame((uint8_t)(10 + i), (uint8_t)i);
    CHECK(ring.pop(out, sizeof(out), n));
    CHECK(n == want.size() && std::memcmp(out, want.data(), n) == 0);
  }
  CHECK(!ring.pop(out, sizeof(out), n) && ring.count() == 0);
  // Invalid sizes are refused, not truncated.
  Bytes big(251, 1);
  CHECK(!ring.push(big.data(), big.size()));
  CHECK(!ring.push(big.data(), 0));
  CHECK(!ring.push(nullptr, 10));
  CHECK(ring.count() == 0 && ring.dropped() == 2 && ring.pushed() == 6);
  // A max-size frame round-trips; a too-small output buffer drops it (counted).
  Bytes full = packetFrame(235, 3);
  CHECK(full.size() == 250 && ring.push(full.data(), full.size()));
  CHECK(ring.pop(out, sizeof(out), n) && n == 250 && std::memcmp(out, full.data(), 250) == 0);
  CHECK(ring.push(full.data(), full.size()));
  uint8_t small[16];
  CHECK(!ring.pop(small, sizeof(small), n) && ring.count() == 0 && ring.dropped() == 3);
  // Wrap-around keeps order over many cycles.
  for (int i = 0; i < 50; i++) {
    Bytes a = packetFrame((uint8_t)(1 + i % 200), (uint8_t)i);
    CHECK(ring.push(a.data(), a.size()));
    CHECK(ring.pop(out, sizeof(out), n) && n == a.size() && std::memcmp(out, a.data(), n) == 0);
  }
  return 0;
}

static int meshcore_frames_unchanged_with_observer_registered() {
  const std::vector<Bytes> corpus = meshCorpus();
  Sink sink;
  std::vector<Bytes> withHandler, without;
  for (const Bytes& f : corpus) {
    receive(f, onObserver, &sink, withHandler);
    receive(f, nullptr, nullptr, without);
  }
  CHECK(sink.calls == 0);
  CHECK(withHandler == corpus);   // every frame, same order, byte for byte
  CHECK(without == corpus);
  // Without a registered handler even observer frames stay on the old path
  // (builds without a visual head behave exactly as before).
  std::vector<Bytes> plain;
  Bytes pkt = packetFrame(50, 1), vit = vitalsFrame();
  receive(pkt, nullptr, nullptr, plain);
  receive(vit, nullptr, nullptr, plain);
  CHECK(plain.size() == 2 && plain[0] == pkt && plain[1] == vit);
  CHECK(!demuxObserverFrame(nullptr, 0, onObserver, &sink) && sink.calls == 0);
  return 0;
}

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
// A straight drag from (x0,y0) to (x1,y1) over ms, then release (90 ms hold-over: same point).
static std::vector<Step> drag(int x0, int y0, int x1, int y1, uint32_t t0, uint32_t ms) {
  std::vector<Step> s;
  const int n = 8;
  for (int i = 0; i <= n; i++) s.push_back({true, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, t0 + ms * i / n});
  s.push_back({false, -1, -1, t0 + ms + 90});
  return s;
}

static int page_swipe_navigation() {
  PageNav nav(5);
  CHECK(nav.count() == 5 && nav.current() == 0);
  // Swipe left = next page, right = previous; no wrap at the ends.
  CHECK(run(nav, drag(300, 150, 180, 150, 0, 200), false).kind == NavEvent::PageChanged && nav.current() == 1);
  CHECK(run(nav, drag(300, 150, 180, 150, 1000, 200), false).kind == NavEvent::PageChanged && nav.current() == 2);
  CHECK(run(nav, drag(150, 150, 300, 150, 2000, 200), false).kind == NavEvent::PageChanged && nav.current() == 1);
  CHECK(run(nav, drag(150, 150, 300, 150, 3000, 200), false).kind == NavEvent::PageChanged && nav.current() == 0);
  CHECK(run(nav, drag(150, 150, 300, 150, 4000, 200), false).kind == NavEvent::None && nav.current() == 0);
  nav.setCurrent(4);
  CHECK(run(nav, drag(300, 150, 150, 150, 5000, 200), false).kind == NavEvent::None && nav.current() == 4);
  nav.setCurrent(1);
  // Velocity: a short fast flick turns the page, the same distance dragged slowly does not.
  CHECK(run(nav, drag(300, 150, 260, 150, 6000, 50), false).kind == NavEvent::PageChanged && nav.current() == 2);
  CHECK(run(nav, drag(300, 150, 260, 150, 7000, 900), false).kind == NavEvent::None && nav.current() == 2);
  // A long slow drag past the distance threshold still turns the page.
  CHECK(run(nav, drag(400, 150, 300, 150, 8000, 1500), false).kind == NavEvent::PageChanged && nav.current() == 3);
  // Vertical and mostly-vertical drags go to the page (press/move/release), never turn it.
  std::vector<NavEvent> evs;
  run(nav, drag(200, 50, 210, 250, 9000, 300), false, &evs);
  CHECK(nav.current() == 3);
  bool sawPress = false, sawMove = false, sawRelease = false, sawPage = false;
  for (const NavEvent& e : evs) {
    sawPress |= e.kind == NavEvent::Press; sawMove |= e.kind == NavEvent::Move;
    sawRelease |= e.kind == NavEvent::Release; sawPage |= e.kind == NavEvent::PageChanged;
  }
  CHECK(sawPress && sawMove && sawRelease && !sawPage);
  CHECK(evs.back().kind == NavEvent::Release && evs.back().x == 210 && evs.back().y == 250);
  // A tap is forwarded as a tap at the press point.
  std::vector<Step> tap = {{true, 100, 120, 10000}, {true, 102, 121, 10040}, {false, -1, -1, 10130}};
  NavEvent t = run(nav, tap, false);
  CHECK(t.kind == NavEvent::Tap && t.x == 100 && t.y == 120 && nav.current() == 3);
  // Tab bar (bottom kTabBarH rows): a tap on a tab jumps to that page and is
  // not forwarded (full coverage in test_freenove_tabbar.py).
  const NavConfig& cfg = nav.config();
  for (int i = 0; i < 5; i++) {
    std::vector<Step> tab = {{true, nav.tabX(i) + 10, nav.barY() + 8, 11000u + i * 500}, {false, -1, -1, 11100u + i * 500}};
    std::vector<NavEvent> de;
    run(nav, tab, false, &de);
    CHECK(nav.current() == i);
    for (const NavEvent& e : de) CHECK(e.kind != NavEvent::Tap && e.kind != NavEvent::Press);
  }
  // Re-tapping the current tab is not a page change.
  nav.setCurrent(2);
  std::vector<Step> same = {{true, nav.tabX(2) + 40, cfg.height - 4, 14000}, {false, -1, -1, 14080}};
  CHECK(run(nav, same, false).kind == NavEvent::None);

  // Pass-through pages (HEAD = the companion UI): everything above the bar
  // passes through, horizontal drags included (no edge strips any more).
  nav.setCurrent(4);
  evs.clear();
  run(nav, drag(300, 160, 100, 160, 15000, 150), true, &evs);
  CHECK(nav.current() == 4);
  CHECK(evs.front().kind == NavEvent::Press && evs.front().x == 300 && evs.front().y == 160);
  CHECK(evs.back().kind == NavEvent::Release);
  int moves = 0; for (const NavEvent& e : evs) moves += e.kind == NavEvent::Move;
  CHECK(moves >= 7);
  evs.clear();
  run(nav, drag(4, 160, 200, 160, 17000, 200), true, &evs);
  CHECK(nav.current() == 4 && evs.front().kind == NavEvent::Press && evs.back().kind == NavEvent::Release);
  // A tap at the very edge is the page's.
  NavEvent et = run(nav, std::vector<Step>{{true, 5, 100, 18000}, {false, -1, -1, 18090}}, true);
  CHECK(et.kind == NavEvent::Release && et.x == 5 && nav.current() == 4);

  // swallowGesture(): the rest of the gesture does nothing (screen-wake touch).
  nav.setCurrent(1);
  evs.clear();
  std::vector<Step> sw = drag(300, 150, 100, 150, 19000, 200);
  for (size_t i = 0; i < sw.size(); i++) {
    evs.push_back(nav.update(sw[i].down, sw[i].x, sw[i].y, sw[i].t, false));
    if (i == 0) nav.swallowGesture();
  }
  for (const NavEvent& e : evs) CHECK(e.kind == NavEvent::None);
  CHECK(nav.current() == 1);
  // ...and the next gesture works normally.
  CHECK(run(nav, drag(300, 150, 150, 150, 20000, 150), false).kind == NavEvent::PageChanged && nav.current() == 2);

  // TouchRelay (HEAD page -> companion UI polled every 25 ms): a press that
  // started and ended between two polls is still reported once.
  TouchRelay relay; int x = 0, y = 0;
  CHECK(!relay.read(x, y));
  relay.set(true, 40, 50); relay.set(false, -1, -1);
  CHECK(relay.read(x, y) && x == 40 && y == 50);
  CHECK(!relay.read(x, y));
  relay.set(true, 60, 70);
  CHECK(relay.read(x, y) && x == 60 && y == 70);
  relay.set(true, 61, 72);
  CHECK(relay.read(x, y) && x == 61 && y == 72);
  relay.set(false, -1, -1);
  CHECK(!relay.read(x, y));
  return 0;
}

static int node_tap_opens_detail() {
  PageNav nav(5);
  HitList<8> hits;
  CHECK(hits.add(100, 100, 6, 11));
  CHECK(hits.add(112, 100, 6, 12));
  CHECK(hits.add(300, 200, 3, 13));
  CHECK(hits.size() == 3);
  // Tap near two nodes: the nearest centre wins.
  NavEvent t = run(nav, std::vector<Step>{{true, 110, 101, 0}, {false, -1, -1, 100}}, false);
  CHECK(t.kind == NavEvent::Tap && hits.find(t.x, t.y) == 12);
  t = run(nav, std::vector<Step>{{true, 101, 99, 1000}, {false, -1, -1, 1100}}, false);
  CHECK(t.kind == NavEvent::Tap && hits.find(t.x, t.y) == 11);
  // Small dots still get a finger-sized target.
  CHECK(hits.find(300 + kMinTouchRadius - 1, 200) == 13);
  CHECK(hits.find(300, 200 + kMinTouchRadius + 1) == -1);
  // Empty space: no node.
  CHECK(hits.find(200, 50) == -1);
  // A swipe that starts on a node turns the page and opens nothing.
  std::vector<NavEvent> evs;
  run(nav, drag(300, 200, 150, 200, 2000, 150), false, &evs);
  for (const NavEvent& e : evs) CHECK(e.kind != NavEvent::Tap && e.kind != NavEvent::Press);
  CHECK(nav.current() == 1);
  // Capacity is bounded; clear() empties.
  HitList<2> small;
  CHECK(small.add(1, 1, 1, 1) && small.add(2, 2, 1, 2) && !small.add(3, 3, 1, 3));
  CHECK(small.find(3, 3) == 2);
  small.clear();
  CHECK(small.size() == 0 && small.find(1, 1) == -1);
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  std::string name = argv[1];
  if (name == "observer_frames_bypass_meshcore_parser") observer_frames_bypass_meshcore_parser();
  else if (name == "meshcore_frames_unchanged_with_observer_registered") meshcore_frames_unchanged_with_observer_registered();
  else if (name == "page_swipe_navigation") page_swipe_navigation();
  else if (name == "node_tap_opens_detail") node_tap_opens_detail();
  else { std::printf("unknown scenario %s\n", argv[1]); return 2; }
  if (failures) return 1;
  std::printf("PASS %s\n", argv[1]);
  return 0;
}
'''

# Renders every page of the framework into a framebuffer with a live model
# (ASan off Windows): the stubs must not crash and HOME must show the model.
PAGES_HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Packet.h"
#include "StubPages.h"
using namespace freenove_ui;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static int differs(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
  int n = 0; for (size_t i = 0; i < a.size(); i++) n += a[i] != b[i]; return n;
}

int main() {
  mesh::head::MeshModel model(16, 32);
  PageContext ctx; std::memset(&ctx, 0, sizeof(ctx));
  ctx.model = &model;
  PageSet set;
  set.attach(&ctx);
  const char* want[] = {"HOME", "TELEMETRY", "MAP", "NODES", "HEAD"};
  CHECK(PageSet::kCount == 5);
  for (int i = 0; i < PageSet::kCount; i++) {
    CHECK(set.page(i) != nullptr && std::strcmp(set.page(i)->title(), want[i]) == 0);
    CHECK(set.page(i)->external() == (i == PageSet::kHead));
  }
  CHECK(set.page(5) == nullptr && set.page(-1) == nullptr);

  std::vector<uint16_t> px(480 * 320), bgpx(480 * 320);
  FrameBuffer565 fb(px.data()), bg(bgpx.data());
  for (int i = 0; i < PageSet::kCount; i++) {
    Page* p = set.page(i);
    if (p->external()) continue;
    bg.clear(0);
    drawPageChrome(bg, *p, i, PageSet::kCount);
    fb.copyFrom(bgpx.data());
    p->onShow(1000);
    p->draw(fb, 1000);
    drawHeader(fb, ctx, 1000);
    CHECK(differs(px, std::vector<uint16_t>(480 * 320, 0)) > 100);   // backdrop, panels, tab bar, header
  }
  // HOME: idle until data arrives or a second passes; then shows the model.
  Page* home = set.page(PageSet::kHome);
  drawPageChrome(bg, *home, 0, PageSet::kCount);
  fb.copyFrom(bgpx.data());
  home->onShow(5000);
  home->draw(fb, 5000);
  std::vector<uint16_t> before = px;
  CHECK(!home->wantsFrame(5100));
  // One observed 0-hop advert-like packet (garbage payload is fine for the model).
  mesh::observer::PacketRecord rec; std::memset(&rec, 0, sizeof(rec));
  rec.snr_q4 = 26; rec.rssi = -90; rec.len = 20;
  rec.raw[0] = (uint8_t)((PAYLOAD_TYPE_GRP_TXT << 2) | ROUTE_TYPE_FLOOD);
  rec.raw[1] = 1; rec.raw[2] = 0x3A;   // one hop, 1-byte hash 3A
  for (int i = 3; i < 20; i++) rec.raw[i] = (uint8_t)i;
  model.onObserverPacket(rec, 5200);
  ctx.data_gen++;
  ctx.obs_frames = 1;
  CHECK(home->wantsFrame(5200));
  fb.eraseTouched(bgpx.data());
  home->draw(fb, 5200);
  CHECK(differs(px, before) > 0);
  // The packet pings the repeater (its last hop is unknown) and rolls the
  // packet counter: frames while they move, then idle until the
  // once-a-second refresh.
  uint32_t t = 5200;
  int frames = 0;
  while (home->wantsFrame(t) && frames < 400) { fb.eraseTouched(bgpx.data()); home->draw(fb, t); t += 17; frames++; }
  CHECK(frames > 10 && t - 5200 < 2000);
  CHECK(!home->wantsFrame(t + 100));
  CHECK(home->wantsFrame(t + 1000));    // ages tick once a second
  // The HEAD page (companion UI) never asks the framework for frames.
  CHECK(!set.page(PageSet::kHead)->wantsFrame(99999));
  // Text helpers
  CHECK(std::strcmp(payloadTypeName(PAYLOAD_TYPE_ADVERT), "ADV") == 0);
  CHECK(std::strcmp(payloadTypeName(PAYLOAD_TYPE_GRP_TXT), "GRP") == 0);
  CHECK(std::strcmp(payloadTypeName(0x0E), "?") == 0);
  if (failures) return 1;
  std::printf("PASS pages\n");
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


class ObserverDemuxTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            raise unittest.SkipTest('a host C++17 compiler is required')
        cls.compiler = compiler
        cls.sanitize = [] if os.name == 'nt' else [
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        cls.temp = tempfile.TemporaryDirectory(prefix='observer-demux-')
        work = Path(cls.temp.name)
        cls.exe = cls._build(work, 'demux', HARNESS, [])
        cls.pages_exe = cls._build(work, 'pages', PAGES_HARNESS, [
            UI / 'StubPages.cpp', UI / 'TelemetryPage.cpp', UI / 'MapPage.cpp', UI / 'NodesPage.cpp', UI / 'NodeCard.cpp', UI / 'HomePage.cpp', UI / 'MeshModel.cpp', UI / 'FrameBuffer332.cpp',
            UI / 'FbFonts.cpp', UI / 'FrameBuffer565.cpp', UI / 'FbFontsAA.cpp', ROOT / 'src' / 'Packet.cpp',
            ROOT / 'src' / 'helpers' / 'AdvertDataHelpers.cpp'])

    @classmethod
    def _build(cls, work, name, source, sources):
        cpp = work / (name + '.cpp')
        cpp.write_text(source, encoding='ascii')
        binary = work / (name + ('.exe' if os.name == 'nt' else ''))
        built = subprocess.run([
            cls.compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', *cls.sanitize,
            '-I', str(UI), '-isystem', str(ROOT / 'src'), '-isystem', str(ROOT / 'test' / 'mocks'),
            str(cpp), *[str(s) for s in sources], '-o', str(binary),
        ], capture_output=True, text=True, timeout=300, cwd=str(ROOT / 'test'))
        if built.returncode != 0:
            raise AssertionError(built.stdout + built.stderr)
        return binary

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_scenario(self, name):
        ran = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=60)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('PASS ' + name, ran.stdout)

    def test_observer_frames_bypass_meshcore_parser(self):
        self.run_scenario('observer_frames_bypass_meshcore_parser')

    def test_meshcore_frames_unchanged_with_observer_registered(self):
        self.run_scenario('meshcore_frames_unchanged_with_observer_registered')

    def test_page_swipe_navigation(self):
        self.run_scenario('page_swipe_navigation')

    def test_node_tap_opens_detail(self):
        self.run_scenario('node_tap_opens_detail')

    def test_pages_render_with_live_model(self):
        ran = subprocess.run([str(self.pages_exe)], capture_output=True, text=True, timeout=60)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('PASS pages', ran.stdout)


class ReceivePathContractTest(unittest.TestCase):
    """The device glue the host harness cannot run."""

    def test_on_data_recv_demuxes_before_meshcore_queue(self):
        raw = (ROOT / 'src' / 'helpers' / 'esp32' / 'ESPNOWRadio.cpp').read_bytes()
        self.assertNotIn(b'\n', raw.replace(b'\r\n', b''), 'ESPNOWRadio.cpp must stay CRLF')
        src = strip_comments(raw.decode('ascii'))
        self.assertIn('#include <helpers/bridges/ObserverDemux.h>', src)
        # Two signatures (Arduino 2/3) share one body: start at the last one.
        starts = [m.start() for m in re.finditer(r'static void OnDataRecv\(', src)]
        self.assertTrue(starts)
        body = function_body(src[starts[-1]:], r'static void OnDataRecv\(')
        accept = body.index('accepting_packets.load()')
        demux = body.index('demuxObserverFrame(')
        queue = body.index('rx_frames[')
        self.assertLess(accept, demux)
        self.assertLess(demux, queue, 'observer frames must be taken before the MeshCore queue')
        # The demux branch returns: observer frames never reach the queue.
        branch = body[demux:queue]
        self.assertRegex(branch, r'\)\s*\)\s*\{?\s*return\s*;')
        # Handler and context are published with release/acquire ordering.
        self.assertIn('observer_handler.load(std::memory_order_acquire)', body)
        setter = function_body(src, r'void ESPNOWRadio::setObserverHandler\(')
        self.assertLess(setter.index('observer_ctx.store'), setter.index('observer_handler.store'))

    def test_header_declares_handler_api(self):
        raw = (ROOT / 'src' / 'helpers' / 'esp32' / 'ESPNOWRadio.h').read_bytes()
        self.assertNotIn(b'\n', raw.replace(b'\r\n', b''), 'ESPNOWRadio.h must stay CRLF')
        self.assertRegex(raw.decode('ascii'),
                         r'static void setObserverHandler\(mesh::observer::FrameHandler handler,\s*void\* ctx\);')

    def test_ui_task_feeds_model_with_head_clock(self):
        src = strip_comments((UI / 'FreenoveUITask.cpp').read_text(encoding='ascii'))
        header = strip_comments((UI / 'FreenoveUITask.h').read_text(encoding='ascii'))
        # Producer side (WiFi task): only a bounded copy under the ring lock.
        handler = function_body(src, r'void FreenoveUITask::onObserverFrame\(')
        self.assertIn('portENTER_CRITICAL', handler)
        self.assertIn('_ring.push(', handler)
        self.assertNotIn('_model', handler)
        # Consumer: decoded records go to the model with the head's millis, not rx_ms.
        drain = function_body(src, r'void FreenoveUITask::drainObserver\(')
        self.assertRegex(drain, r'onObserverPacket\(\s*\w+\s*,\s*now_ms\s*\)')
        self.assertRegex(drain, r'onVitals\(\s*\w+\s*,\s*now_ms\s*\)')
        self.assertNotIn('rx_ms', drain)
        self.assertIn('setObserverHandler(', src)
        # Frame cap: a fixed period of at least 1000/60 ms, on core 1.
        period = re.search(r'kFramePeriodMs\s*=\s*(\d+)', header)
        self.assertIsNotNone(period)
        self.assertGreaterEqual(int(period.group(1)) * 60, 1000)
        self.assertRegex(src, r'xTaskCreatePinnedToCore\([^;]*,\s*1\s*\)')

    def test_page_order_and_head_placeholder(self):
        src = (UI / 'StubPages.h').read_text(encoding='ascii')
        order = re.search(r'enum\s*\{\s*kHome\s*,\s*kTelemetry\s*,\s*kMap\s*,\s*kNodes\s*,\s*kHead\s*,\s*kCount\s*\}', src)
        self.assertIsNotNone(order, 'page order HOME, TELEMETRY, MAP, NODES, HEAD')

    def test_display_arbitrates_panel_owner(self):
        src = strip_comments((VARIANT / 'FreenoveDisplay.cpp').read_text(encoding='ascii'))
        submit = function_body(src, r'bool FreenoveDisplay::submitPage\(')
        self.assertIn('PanelOwner::Pages', submit)
        self.assertIn('lockPanel()', submit)
        companion = function_body(src, r'void FreenoveDisplay::pushCompanionFrame\(')
        self.assertIn('lockPanel()', companion)
        self.assertIn('PanelOwner::Companion', companion)
        end = function_body(src, r'void FreenoveDisplay::endFrame\(')
        self.assertIn('pushCompanionFrame()', end)
        self.assertNotIn('pushFramebuffer()', end)
        owner = function_body(src, r'void FreenoveDisplay::setPanelOwner\(')
        self.assertIn('lockPanel()', owner)
        self.assertIn('_pusher.invalidate()', owner)
        touch = function_body(src, r'bool FreenoveDisplay::getTouch\(')
        self.assertIn('_relay.read(', touch)

    def test_product_env_builds_visual_head(self):
        ini = (VARIANT / 'platformio.ini').read_text(encoding='ascii')
        product = ini.split('[env:Freenove_S3_35_Head_companion_usb]')[1].split('[env:')[0]
        self.assertIn('-D FREENOVE_VISUAL_HEAD=1', product)
        for name in ('FreenoveUITask.cpp', 'StubPages.cpp', 'MeshModel.cpp'):
            self.assertIn(name, product)


if __name__ == '__main__':
    unittest.main()
