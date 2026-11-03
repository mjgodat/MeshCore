"""Strip rendering for the CYD visual head (HMI_PROFILE_CYD): FrameBuffer565
band mode, StripRenderer (band hashes, pushes) and the pages drawn band by
band (drawFrame replayed per band) against the same pages drawn into one
full frame, frame after frame, in both screen profiles.

Host harness (g++, ASan/UBSan off Windows). Run directly with --preview to
write the CYD page previews (320x240, host renders with fake data) to
.superpowers/cyd/previews/ (cyd_*_1x.png and 2x zooms):

    python test_strip_renderer.py --preview
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_widgets  # noqa: E402
from test_layouts import rgb565_rows, write_png  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / 'examples' / 'companion_radio' / 'ui-freenove'
PREVIEW_DIR = ROOT / '.superpowers' / 'cyd' / 'previews'

_W = test_widgets.HARNESS
# The widgets harness's includes and its fake mesh (packets, adverts,
# populate()): reused as they are.
PRELUDE = (_W[:_W.index('// ------------------------------------------------------------ pure helpers')] +
           _W[_W.index('// --------------------------------------------------------------- the mesh'):
              _W.index('struct Screen {')])

HARNESS = PRELUDE + r'''
#include "PageNav.h"
#include "StripRenderer.h"

static const int W = profile::kScreenW, H = profile::kScreenH;

// ------------------------------------------------------------ band mode

static void scene(FrameBuffer565& fb, int variant) {
  static const PanelRect panels[2] = {{6, 30, 120, 90}, {140, 60, 150, 100}};
  drawBackdrop(fb, panels, 2);
  drawPanel(fb, panels[0], "MESH TOPOLOGY", "GRAPH");
  drawPanel(fb, panels[1], "NOISE", "dBm");
  fb.aaLine(10.5f, 200.0f, (float)W - 20, 12.0f, 2.0f, theme::kAccent);
  fb.aaLine(-50.0f, (float)H + 40, (float)W + 30, -20.0f, 1.0f, theme::kSigWeak, 120);
  fb.aaRing(160.0f, 120.0f, 40.0f, 3.0f, theme::kSigStrong);
  fb.aaArc(220.0f, 150.0f, 33.0f, 5.0f, 150.0f, 390.0f, theme::kGaugeTrack);
  fb.aaDisc(60.0f + (float)variant, 180.0f, 9.5f, theme::kText);
  fb.fillRoundRect(20, 140, 90, 41, 6, theme::kTileOnFill);
  fb.strokeRoundRect(20, 140, 90, 41, 6, theme::kAccent);
  fb.fillRectAlpha(0, 37, W, 13, theme::kPanelFill, 200);
  fb.dashedLine(3, 3, W - 3, H - 3, theme::kDivider, 2, 4, 1);
  fb.text(12, 44, "Capt4McNeil 1.2km", theme::kText, kFontSmall);
  fb.text(W - 4, 90, "-116", theme::kText, kFontBig, FrameBuffer565::kRightTop);
  fb.text(W / 2, H / 2, "CONFIRM?", theme::kWarn, kFontTile, FrameBuffer565::kCentreMiddle);
  fb.setClip(30, 100, 70, 35);
  fb.text(25, 95, "clipped text", theme::kSigPoor, kFontName);
  fb.aaDisc(40.0f, 120.0f, 20.0f, theme::kBad, 160);
  fb.resetClip();
  fb.hline(-5, H - 1, W + 10, theme::kBad);
  fb.vline(W - 1, -5, H + 10, theme::kGood);
  fb.rect(0, 0, W, H, theme::kAccentDim);
}

struct Collect {
  std::vector<uint16_t> px;
  int pushes;
  Collect() : px((size_t)W * H, 0xDEAD), pushes(0) {}
};
static void collect(const uint16_t* px, int w, int y0, int rows, void* p) {
  Collect* c = static_cast<Collect*>(p);
  std::memcpy(&c->px[(size_t)y0 * w], px, (size_t)w * rows * 2);
  c->pushes++;
}
static int g_variant = 0;
static void drawScene(FrameBuffer565& fb, int band, void* p) { (void)band; (void)p; scene(fb, g_variant); }

static void band_primitives() {
  std::vector<uint16_t> full((size_t)W * H);
  FrameBuffer565 ffb(full.data(), W, H);
  scene(ffb, 0);
  static const int kRows[5] = {1, 7, 40, 64, H};
  for (int k = 0; k < 5; k++) {
    std::vector<uint16_t> strip((size_t)W * kRows[k]);
    StripRenderer sr(strip.data(), W, H, kRows[k]);
    Collect c;
    sr.render(drawScene, nullptr, collect, &c);
    CHECK(c.pushes == sr.bands());
    size_t diff = 0;
    for (size_t i = 0; i < full.size(); i++) diff += c.px[i] != full[i];
    if (diff) std::printf("band rows %d: %zu pixels differ\n", kRows[k], diff);
    CHECK(diff == 0);
  }
  // Band mode clips: nothing outside the band is addressed (ASan), get()
  // outside the band is 0, the clip never leaves the band.
  std::vector<uint16_t> strip((size_t)W * 10, 0x1234);
  FrameBuffer565 b(strip.data(), W, H, 100, 10);
  CHECK(b.bandY0() == 100 && b.bandRows() == 10 && b.clipY0() == 100 && b.clipY1() == 109);
  b.setClip(0, 0, W, 50);
  CHECK(b.clipY0() > b.clipY1());
  b.fillRect(0, 0, W, 50, theme::kBad);
  for (uint16_t v : strip) CHECK(v == 0x1234);
  b.setClip(0, 105, W, 500);
  CHECK(b.clipY0() == 105 && b.clipY1() == 109);
  b.resetClip();
  b.clear(theme::kText);
  CHECK(b.get(5, 100) == theme::kText && b.get(5, 109) == theme::kText && b.get(5, 99) == 0 && b.get(5, 110) == 0);
  b.setBand(H - 10, 10);
  CHECK(b.clipY0() == H - 10 && b.clipY1() == H - 1);
  b.setBand(H - 5, 10);   // clamped to the frame
  CHECK(b.bandRows() == 5 && b.clipY1() == H - 1);
  if (!failures) std::printf("PASS band_primitives\n");
}

static void strip_renderer_pushes() {
  std::vector<uint16_t> strip((size_t)W * 40);
  StripRenderer sr(strip.data(), W, H, 40);
  CHECK(sr.bands() == (H + 39) / 40);
  Collect c;
  g_variant = 0;
  CHECK(sr.render(drawScene, nullptr, collect, &c) == sr.bands());   // unknown glass: everything
  CHECK(sr.render(drawScene, nullptr, collect, &c) == 0);            // idle: nothing
  g_variant = 30;                                                    // the disc at y 180 moves
  CHECK(sr.render(drawScene, nullptr, collect, &c) == 1);
  CHECK(sr.render(drawScene, nullptr, collect, &c) == 0);
  sr.invalidate();
  CHECK(sr.render(drawScene, nullptr, collect, &c) == sr.bands());
  CHECK(sr.render(drawScene, nullptr, collect, &c, true) == sr.bands());   // forced (screenshot)
  // More bands than hashes: those are always pushed.
  StripRenderer tiny(strip.data(), W, H, 1);
  CHECK(tiny.bands() == H && tiny.bandRows() == 1);
  CHECK(tiny.render(drawScene, nullptr, collect, &c) == H);
  CHECK(tiny.render(drawScene, nullptr, collect, &c) == H - StripRenderer::kMaxBands);
  CHECK(StripRenderer::hashPixels(strip.data(), 3) != StripRenderer::hashPixels(strip.data() + 1, 3) ||
        strip[0] == strip[1]);
  if (!failures) std::printf("PASS strip_renderer_pushes\n");
}

// ------------------------------------------------------------ pages

struct Stack {
  MeshModel model;
  PageContext ctx;
  PageSet set;
  uint32_t t;
  explicit Stack(bool fix) : model(64, 256), t(0) {
    ctxInit(ctx, &model);
    set.attach(&ctx);
    rng = 12345;
    salt = 1;
    t = populate(model, ctx, fix);
  }
};

struct StripArgs { Stack* s; int index; uint32_t now; };
static void drawPageBand(FrameBuffer565& fb, int band, void* p) {
  StripArgs* a = static_cast<StripArgs*>(p);
  a->s->ctx.band = band;
  drawFrame(fb, *a->s->set.page(a->index), a->s->ctx, a->index, PageSet::kCount, a->now);
  a->s->ctx.band = 0;
}

// The same page in two identical stacks, one drawn into a full frame, the
// other band by band: every frame must match.
static size_t compareFrame(Stack& a, Stack& b, int index, uint32_t now, StripRenderer& sr, Collect& c,
                           std::vector<uint16_t>& full) {
  a.ctx.now_ms = b.ctx.now_ms = now;
  FrameBuffer565 fb(full.data(), W, H);
  drawFrame(fb, *a.set.page(index), a.ctx, index, PageSet::kCount, now);
  StripArgs args = {&b, index, now};
  sr.render(drawPageBand, &args, collect, &c, true);
  size_t diff = 0;
  for (size_t i = 0; i < full.size(); i++) diff += c.px[i] != full[i];
  return diff;
}

static void bothTap(Stack& a, Stack& b, int index, int x, int y) {
  a.set.page(index)->touch(x, y, true); a.set.page(index)->touch(x, y, false);
  b.set.page(index)->touch(x, y, true); b.set.page(index)->touch(x, y, false);
}

static void bothPacket(Stack& a, Stack& b, uint32_t now, int k) {
  const uint16_t hops[3] = {0x5701, 0xCA75, 0x1F01};
  for (Stack* s : {&a, &b}) {
    salt = 900 + (uint32_t)k;
    obs(s->model, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, plen(2), hashes({0x7EBE, hops[k % 3]}), payload()), now,
        (float)(k % 9) - 3.0f);
    s->ctx.data_gen++;
    s->ctx.obs_last_ms = now;
  }
}

static void pages_strip_equals_full() {
  Stack a(true), b(true);
  std::vector<uint16_t> strip((size_t)W * 40), full((size_t)W * H);
  StripRenderer sr(strip.data(), W, H, 40);
  Collect c;
  static const int kPages[4] = {PageSet::kHome, PageSet::kTelemetry, PageSet::kMap, PageSet::kNodes};
  uint32_t now = a.t;
  for (int pi = 0; pi < 4; pi++) {
    const int index = kPages[pi];
    a.set.page(index)->onShow(now);
    b.set.page(index)->onShow(now);
    for (int f = 0; f < 40; f++) {
      now += 60;
      if (f % 7 == 3) bothPacket(a, b, now, f);
      if (index == PageSet::kHome && f == 20) bothTap(a, b, index, HomePage::kToggleX + 20, HomePage::kToggleY + 8);
      if (index == PageSet::kNodes && f == 10) bothTap(a, b, index, NodesPage::columnX0(widgets::kColSnr) + 4, NodesPage::kHeadY + 6);
      if (index == PageSet::kHome && f == 30) {
        bothTap(a, b, index, HomePage::kToggleX + 20, HomePage::kToggleY + 8);   // back to GRAPH
        HomePage* h = static_cast<HomePage*>(a.set.page(index));
        int x = 0, y = 0;
        if (h->contactPos(1, x, y)) bothTap(a, b, index, x, y);                   // the node card
      }
      const size_t diff = compareFrame(a, b, index, now, sr, c, full);
      if (diff) std::printf("page %d frame %d: %zu pixels differ\n", index, f, diff);
      CHECK(diff == 0);
      if (diff) break;
    }
    a.set.page(index)->onHide();
    b.set.page(index)->onHide();
  }
  if (!failures) std::printf("PASS pages_strip_equals_full\n");
}

static void cyd_layout() {
#if HMI_CYD
  CHECK(W == 320 && H == 240);
  // Four tiles: ADVERT PRESET BRIDGE ADMIN.
  const int cy = HomePage::kTileY + HomePage::kTileH / 2;
  CHECK(HomePage::tileAt(HomePage::kTileX + 10, cy) == hmi::kTileAdvert);
  CHECK(HomePage::tileAt(HomePage::kTileX + HomePage::kTilePitch + 10, cy) == hmi::kTilePreset);
  CHECK(HomePage::tileAt(HomePage::kTileX + 2 * HomePage::kTilePitch + 10, cy) == hmi::kTileBridge);
  CHECK(HomePage::tileAt(HomePage::kTileX + 3 * HomePage::kTilePitch + 10, cy) == hmi::kTileAdmin);
  CHECK(HomePage::kTileX + 3 * HomePage::kTilePitch + HomePage::kTileW <= W);
  CHECK(HomePage::kTileY + HomePage::kTileH <= H - kTabBarH);
  // Tabs are finger sized.
  PageNav nav(PageSet::kCount);
  CHECK(nav.barY() == H - kTabBarH && kTabBarH >= 28 && nav.tabX(1) - nav.tabX(0) >= 60);
  CHECK(nav.tabAt(5, H - 5) == 0 && nav.tabAt(W - 5, H - 5) == PageSet::kCount - 1);
  // Every panel inside the content area.
  for (int index : {PageSet::kHome, PageSet::kTelemetry, PageSet::kMap, PageSet::kNodes}) {
    PageSet set;
    const PanelRect* p = nullptr;
    const int n = set.page(index)->panels(p);
    for (int i = 0; i < n; i++) {
      CHECK(p[i].x >= 2 && p[i].y >= kHeaderH && p[i].x + p[i].w <= W - 2 && p[i].y + p[i].h <= H - kTabBarH);
    }
  }
#endif
  if (!failures) std::printf("PASS cyd_layout\n");
}

// Every touch target is a drawn control at the same place: HOME tiles,
// the tab bar, the node card's CLOSE, the NODES column headers.
static void hit_targets_drawn() {
  Stack s(true);
  std::vector<uint16_t> full((size_t)W * H);
  FrameBuffer565 fb(full.data(), W, H);
  const uint32_t now = s.t;
  auto px = [&](int x, int y) { return full[(size_t)y * W + x]; };
  // HOME: every pixel that hits a tile lies on that tile as drawn (+ the
  // 2 px slop), the tile is on the screen and filled with a tile colour.
  HomePage& home = *static_cast<HomePage*>(s.set.page(PageSet::kHome));
  home.onShow(now);
  drawFrame(fb, home, s.ctx, PageSet::kHome, PageSet::kCount, now);
  bool seen[hmi::kTileCount] = {false};
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      const int t = HomePage::tileAt(x, y);
      if (t < 0) continue;
      int pos = -1;
      for (int i = 0; i < HomePage::kTilesShown; i++) if (HomePage::tileId(i) == t) pos = i;
      CHECK(pos >= 0);
      if (pos < 0) continue;
      seen[t] = true;
      const int tx = HomePage::kTileX + pos * HomePage::kTilePitch;
      CHECK(x >= tx - 2 && x < tx + HomePage::kTileW + 2 && y >= HomePage::kTileY - 2 &&
            y < HomePage::kTileY + HomePage::kTileH + 2);
    }
  for (int i = 0; i < HomePage::kTilesShown; i++) {
    const int t = HomePage::tileId(i);
    CHECK(seen[t]);
    const int tx = HomePage::kTileX + i * HomePage::kTilePitch;
    CHECK(tx >= 0 && tx + HomePage::kTileW <= W && HomePage::kTileY + HomePage::kTileH <= H - kTabBarH);
    const uint16_t c = px(tx + 3, HomePage::kTileY + HomePage::kTileH / 2);
    CHECK(c == theme::kTileOnFill || c == theme::kTileOffFill);
  }
  // Tiles not shown on this screen are never hit.
  for (int t = 0; t < hmi::kTileCount; t++) {
    bool shownTile = false;
    for (int i = 0; i < HomePage::kTilesShown; i++) shownTile |= HomePage::tileId(i) == t;
    if (!shownTile) CHECK(!seen[t]);
  }
  // Tab bar: every tab hit lies on its drawn tab tile (inset 3 px).
  PageNav nav(PageSet::kCount);
  for (int i = 0; i < PageSet::kCount; i++) {
    const int cx = (nav.tabX(i) + nav.tabX(i + 1)) / 2, cy = nav.barY() + 6;
    CHECK(nav.tabAt(cx, cy) == i);
    const uint16_t c = px(nav.tabX(i) + 4, cy);
    CHECK(c == theme::kTabFill || c == theme::kTabActiveFill);
  }
  // Node card: CLOSE drawn where it is hit.
  int x = 0, y = 0;
  if (home.contactPos(1, x, y)) { home.touch(x, y, true); home.touch(x, y, false); }
  drawFrame(fb, home, s.ctx, PageSet::kHome, PageSet::kCount, now + 20);
  CHECK(NodeCard::kCloseX >= 0 && NodeCard::kCloseX + NodeCard::kCloseW <= W && NodeCard::kCloseY + NodeCard::kCloseH <= H);
  CHECK(px(NodeCard::kCloseX + 3, NodeCard::kCloseY + NodeCard::kCloseH / 2) == theme::kTileOnFill);
  // NODES: a header hit is a column that is shown (has width).
  for (int hx = 0; hx < W; hx++) {
    const int c = NodesPage::columnAt(hx);
    if (c >= 0) CHECK(NodesPage::columnX1(c) > NodesPage::columnX0(c) || c == widgets::kColCount - 1);
  }
  if (!failures) std::printf("PASS hit_targets_drawn\n");
}

// ------------------------------------------------------------ previews

static bool save(const std::vector<uint16_t>& px, const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(px.data(), 2, px.size(), f) == px.size();
  std::fclose(f);
  return ok;
}

static int preview(const char* dir) {
  std::string base(dir);
  bool ok = true;
  Stack s(true);
  std::vector<uint16_t> strip((size_t)W * 40);
  StripRenderer sr(strip.data(), W, H, 40);
  Collect c;
  uint32_t now = s.t;
  auto shot = [&](int index, const char* name) {
    StripArgs args = {&s, index, now};
    s.ctx.now_ms = now;
    sr.render(drawPageBand, &args, collect, &c, true);
    ok = save(c.px, base + "/" + name + ".rgb565") && ok;
  };
  Page* home = s.set.page(PageSet::kHome);
  home->onShow(now);
  shot(PageSet::kHome, "cyd_home");
  bothPacket(s, s, now += 100, 1);
  now += 200;
  shot(PageSet::kHome, "cyd_home_route");
  home->touch(HomePage::kToggleX + 20, HomePage::kToggleY + 8, true);
  home->touch(HomePage::kToggleX + 20, HomePage::kToggleY + 8, false);
  now += 2000;
  shot(PageSet::kHome, "cyd_home_geo");
  home->touch(HomePage::kToggleX + 20, HomePage::kToggleY + 8, true);
  home->touch(HomePage::kToggleX + 20, HomePage::kToggleY + 8, false);
  const int cy = HomePage::kTileY + HomePage::kTileH / 2;
  home->touch(HomePage::kTileX + 10, cy, true);
  home->touch(HomePage::kTileX + 10, cy, false);
  now += 50;
  shot(PageSet::kHome, "cyd_home_confirm");
  now += 4000;
  {
    int x = 0, y = 0;
    if (static_cast<HomePage*>(home)->contactPos(1, x, y)) { home->touch(x, y, true); home->touch(x, y, false); }
  }
  now += 50;
  shot(PageSet::kHome, "cyd_home_card");
  home->onHide();
  for (int index : {PageSet::kTelemetry, PageSet::kMap, PageSet::kNodes}) {
    s.set.page(index)->onShow(now);
    now += 100;
    shot(index, index == PageSet::kTelemetry ? "cyd_telem" : (index == PageSet::kMap ? "cyd_map" : "cyd_nodes"));
    s.set.page(index)->onHide();
  }
  // Render timing (host): one HOME frame band by band.
  {
    home->onShow(now);
    const auto c0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; i++) { now += 100; StripArgs args = {&s, PageSet::kHome, now}; sr.render(drawPageBand, &args, collect, &c, true); }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - c0).count();
    std::printf("TIMING cyd_home_strip_frame us=%lld\n", (long long)(us / 20));
  }
  std::printf(ok ? "PASS preview\n" : "FAIL preview\n");
  return ok ? 0 : 1;
}

int main(int argc, char** argv) {
  const std::string name = argc > 1 ? argv[1] : "";
  if (name == "preview") return preview(argc > 2 ? argv[2] : ".");
  if (name == "band_primitives") band_primitives();
  else if (name == "strip_renderer_pushes") strip_renderer_pushes();
  else if (name == "pages_strip_equals_full") pages_strip_equals_full();
  else if (name == "cyd_layout") cyd_layout();
  else if (name == "hit_targets_drawn") hit_targets_drawn();
  else { std::printf("unknown scenario %s\n", name.c_str()); return 2; }
  return failures ? 1 : 0;
}
'''

SOURCES = test_widgets.SOURCES
SCENARIOS = ('band_primitives', 'strip_renderer_pushes', 'pages_strip_equals_full', 'cyd_layout',
             'hit_targets_drawn')


def build(work, cyd):
    compiler = shutil.which('g++') or shutil.which('clang++')
    if compiler is None:
        raise unittest.SkipTest('a host C++17 compiler is required')
    sanitize = [] if os.name == 'nt' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    cpp = work / 'strips.cpp'
    cpp.write_text(HARNESS, encoding='ascii')
    exe = work / (('strips_cyd' if cyd else 'strips_fn') + ('.exe' if os.name == 'nt' else ''))
    built = subprocess.run([
        compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', *sanitize,
        *(['-DHMI_PROFILE_CYD=1'] if cyd else []),
        '-I', str(UI), '-isystem', str(ROOT / 'src'), '-isystem', str(ROOT / 'test' / 'mocks'),
        str(cpp), *[str(s) for s in SOURCES], '-o', str(exe),
    ], capture_output=True, text=True, timeout=600, cwd=str(ROOT / 'test'))
    if built.returncode != 0:
        raise AssertionError(built.stdout + built.stderr)
    return exe


class StripRendererTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='cyd-strips-')
        try:
            cls.cyd = build(Path(cls.temp.name), True)
            cls.fn = build(Path(cls.temp.name), False)
        except BaseException:
            cls.temp.cleanup()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_scenario(self, exe, name):
        ran = subprocess.run([str(exe), name], capture_output=True, text=True, timeout=300)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('PASS ' + name, ran.stdout)

    def test_cyd_profile(self):
        for name in SCENARIOS:
            with self.subTest(name=name):
                self.run_scenario(self.cyd, name)

    def test_freenove_profile(self):
        # The same band replay holds for the 480x320 layouts too.
        for name in SCENARIOS:
            with self.subTest(name=name):
                self.run_scenario(self.fn, name)

    def test_cyd_preview_renders(self):
        out = Path(self.temp.name)
        ran = subprocess.run([str(self.cyd), 'preview', str(out)], capture_output=True, text=True, timeout=300)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        for name in ('cyd_home', 'cyd_home_geo', 'cyd_telem', 'cyd_map', 'cyd_nodes', 'cyd_home_card'):
            self.assertEqual((out / (name + '.rgb565')).stat().st_size, 320 * 240 * 2)


def write_preview():
    PREVIEW_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='cyd-preview-') as tmp:
        exe = build(Path(tmp), True)
        ran = subprocess.run([str(exe), 'preview', tmp], capture_output=True, text=True, timeout=300)
        if ran.returncode != 0:
            raise SystemExit(ran.stdout + ran.stderr)
        print(ran.stdout.strip())
        for f in sorted(Path(tmp).glob('*.rgb565')):
            raw = f.read_bytes()
            write_png(rgb565_rows(raw, 320, 240, 1), PREVIEW_DIR / (f.stem + '_1x.png'))
            write_png(rgb565_rows(raw, 320, 240, 2), PREVIEW_DIR / (f.stem + '.png'))
            print('wrote', PREVIEW_DIR / (f.stem + '_1x.png'))


if __name__ == '__main__':
    if '--preview' in sys.argv:
        write_preview()
    else:
        unittest.main()
