"""Freenove HEAD page in the HMI style (Task 9): ui-freenove/CompanionSkin.h,
used only by FreenoveDisplay. The companion screens (HeadAdmin, HeadCompose,
TouchKeyboard, the home carousel) keep drawing through DisplayDriver
fillRect/drawRect with UIColor roles; the Freenove adapter records those
calls and renders them as HMI panels, tiles, rows and keys (Theme.h) over the
navy gradient backdrop. Other boards (CYD, LGFXDisplay) are untouched.

    python test_companion_skin.py --preview   # head_*_hmi*.png (1:1 and 2x)
"""

import os
import re
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
#include "CompanionSkin.h"
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

static CompanionPalette palette() {
  return CompanionPalette{UIColor::window_bkg, UIColor::title_bkg, UIColor::popup_bkg, UIColor::corp_blue,
                          UIColor::title_txt, UIColor::primary_txt, UIColor::secondary_txt,
                          UIColor::warning_txt, UIColor::popup_txt};
}

// Behaves like FreenoveDisplay with the skin on: fills/rects recorded into
// CompanionSkin, text into CompanionText; the frame is the backdrop, then
// the skin, then the text (there is no sprite content on the host).
class HostDisplay : public DisplayDriver {
 public:
  HostDisplay() : DisplayDriver(240, 148), px(480 * 320), back(480 * 320), fb(px.data()) {
    CompanionSkin::fillBackdrop(back.data(), 480, 296, 320);
  }
  std::vector<uint16_t> px, back;
  FrameBuffer565 fb;
  CompanionText text;
  CompanionSkin skin;
  uint16_t colour = 0xFFFF;
  int size = 1, cx = 0, cy = 0;
  bool isOn() override { return true; }
  void turnOn() override {}
  void turnOff() override {}
  void clear() override {}
  void startFrame(ColorVal) override { text.clear(); skin.clear(); }
  void setTextSize(int sz) override { size = sz < 1 ? 1 : sz; }
  void setColor(ColorVal c) override { colour = c; }
  void setCursor(int x, int y) override { cx = x; cy = y; }
  void print(const char* s) override {
    cx = text.add(cx, cy, size, skinTextColour(colour, palette()), s, skin.textPad(cx, cy, palette()));
  }
  void fillRect(int x, int y, int w, int h) override { skin.fill(x, y, w, h, colour); text.occlude(x, y, w, h); }
  void drawRect(int x, int y, int w, int h) override { skin.rect(x, y, w, h, colour); }
  void drawXbm(int, int, const uint8_t*, int, int) override {}
  uint16_t getTextWidth(const char* s) override { return (uint16_t)CompanionText::logicalWidth(s, size); }
  int textLineHeight() override { return CompanionText::lineHeight(size); }
  void endFrame() override {
    std::memcpy(px.data(), back.data(), 480 * 296 * 2);
    skin.draw(fb, 296, back.data(), palette());
    text.draw(fb, 296);
  }
  bool save(const char* path) const {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(px.data(), 2, px.size(), f) == px.size();
    std::fclose(f);
    return ok;
  }
};

// HeadAdmin::drawButton and HeadCompose::drawHeaderButton, verbatim
// (HeadAdmin.h / HeadCompose.h need the companion runtime).
static void drawButton(DisplayDriver& d, int x, int y, int w, int h, const char* label, bool on, bool enabled = true) {
  d.setColor(on ? UIColor::corp_blue : enabled ? UIColor::title_bkg : UIColor::window_bkg);
  d.fillRect(x + 1, y + 1, w - 2, h - 2);
  d.setColor(UIColor::secondary_txt);
  d.drawRect(x, y, w, h);
  d.setColor(enabled ? UIColor::title_txt : UIColor::secondary_txt);
  d.drawTextEllipsized(x + 3, y + (h - 8) / 2, w - 6, label);
}
static void drawHeaderButton(DisplayDriver& d, int x, int w, const char* label) {
  d.setColor(UIColor::title_bkg);
  d.fillRect(x + 1, 1, w - 2, 18 - 2);
  d.setColor(UIColor::secondary_txt);
  d.drawRect(x, 0, w, 18);
  d.setColor(UIColor::title_txt);
  d.drawTextEllipsized(x + 3, (18 - 8) / 2, w - 6, label);
}

static const SkinShape* only(const CompanionSkin& s, std::vector<SkinShape>& out) {
  out.assign(CompanionSkin::kMaxOps, SkinShape());
  const int n = s.resolve(out.data(), 240, palette());
  out.resize(n);
  return n == 1 ? &out[0] : nullptr;
}

static void roles() {
  const CompanionPalette p = palette();
  CHECK(skinRole(UIColor::window_bkg, p) == SkinRole::Window);
  CHECK(skinRole(UIColor::title_bkg, p) == SkinRole::Title);
  CHECK(skinRole(UIColor::popup_bkg, p) == SkinRole::Popup);
  CHECK(skinRole(UIColor::corp_blue, p) == SkinRole::Accent);
  CHECK(skinRole(UIColor::primary_txt, p) == SkinRole::Text);
  CHECK(skinRole(UIColor::secondary_txt, p) == SkinRole::Dim);
  CHECK(skinRole(UIColor::warning_txt, p) == SkinRole::Warning);
  CHECK(skinRole(rgb565(255, 0, 255), p) == SkinRole::Other);
  // Text: theme tokens only; an unknown colour passes through.
  CHECK(skinTextColour(UIColor::primary_txt, p) == theme::kText);
  CHECK(skinTextColour(UIColor::title_txt, p) == theme::kText);
  CHECK(skinTextColour(UIColor::secondary_txt, p) == theme::kTextDim);
  CHECK(skinTextColour(UIColor::warning_txt, p) == theme::kWarn);
  CHECK(skinTextColour(UIColor::corp_blue, p) == theme::kAccent);
  CHECK(skinTextColour(UIColor::window_bkg, p) == theme::kText);   // text on a lit fill
  CHECK(skinTextColour(rgb565(255, 0, 255), p) == rgb565(255, 0, 255));
  if (!failures) std::printf("PASS roles\n");
}

static void classify() {
  std::vector<SkinShape> v;
  const SkinShape* s;
  // HeadAdmin button: fill inset by 1 + enclosing rect -> one tile.
  { HostDisplay d; drawButton(d, 0, 130, 80, 18, "Up", false);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::Button && !s->lit && s->enabled);
    CHECK(s && s->x == 0 && s->y == 130 && s->w == 80 && s->h == 18); }
  { HostDisplay d; drawButton(d, 96, 0, 144, 18, "Logged in", true);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::Button && s->lit); }
  { HostDisplay d; drawButton(d, 0, 130, 80, 18, "Up", false, false);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::Button && !s->enabled); }
  // Full-width list rows (settings, picker, presets) -> rows.
  { HostDisplay d; drawButton(d, 0, 34, 240, 16, "bridge.enabled", false);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::Row && !s->lit && s->enabled); }
  { HostDisplay d; drawButton(d, 0, 18, 240, 16, "HOME CaptCallahan", true);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::Row && s->lit); }
  { HostDisplay d; drawButton(d, 0, 98, 240, 16, "3 -", false, false);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::Row && !s->enabled); }
  // A tall full-width button (preset Confirm, Back) stays a button.
  { HostDisplay d; drawButton(d, 0, 127, 240, 21, "Back", false);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::Button); }
  // Keyboard keys: outline only; the action key (corp_blue) is lit.
  { HostDisplay d; d.setColor(UIColor::secondary_txt); d.drawRect(1, 41, 22, 19);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::Key && !s->lit); }
  { HostDisplay d; d.setColor(UIColor::corp_blue); d.drawRect(181, 104, 58, 19);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::Key && s->lit); }
  // Full-width outline: the text entry field.
  { HostDisplay d; d.setColor(UIColor::secondary_txt); d.drawRect(0, 28, 240, 12);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::Field); }
  // Carousel header bar: a full-width title fill at the top -> header panel.
  { HostDisplay d; d.setColor(UIColor::title_bkg); d.fillRect(0, 0, 240, 11);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::Header); }
  // Popup box: popup fill + its border.
  { HostDisplay d; d.setColor(UIColor::popup_bkg); d.fillRect(10, 40, 220, 40);
    d.setColor(UIColor::popup_txt); d.drawRect(10, 40, 220, 40);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::Popup); }
  // Small marks keep their exact geometry: page dots, battery, dividers.
  { HostDisplay d; d.setColor(UIColor::corp_blue); d.fillRect(4, 13, 4, 4);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::Plain && s->colour == theme::kAccent); }
  { HostDisplay d; d.setColor(UIColor::title_txt); d.drawRect(200, 2, 18, 6);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::PlainStroke && s->colour == theme::kText); }
  { HostDisplay d; d.setColor(UIColor::corp_blue); d.drawRect(0, 120, 240, 1);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::Line); }
  // A window-colour fill clears (to the backdrop).
  { HostDisplay d; d.setColor(UIColor::window_bkg); d.fillRect(0, 0, 240, 148);
    s = only(d.skin, v); CHECK(s && s->kind == SkinKind::Erase); }
  // A rect that does not enclose the previous fill is its own shape.
  { HostDisplay d; d.setColor(UIColor::title_bkg); d.fillRect(0, 0, 50, 20);
    d.setColor(UIColor::secondary_txt); d.drawRect(100, 50, 50, 20);
    v.assign(CompanionSkin::kMaxOps, SkinShape());
    CHECK(d.skin.resolve(v.data(), 240, palette()) == 2); }
  // Tile labels (drawn at x + 3) get padding; centred key labels do not.
  { HostDisplay d; drawButton(d, 0, 130, 80, 18, "", false);
    CHECK(d.skin.textPad(3, 135, palette()) == CompanionSkin::kTextPad);
    CHECK(d.skin.textPad(30, 135, palette()) == 0);
    CHECK(d.skin.textPad(3, 100, palette()) == 0); }
  { HostDisplay d; d.setColor(UIColor::secondary_txt); d.drawRect(1, 41, 22, 19);
    CHECK(d.skin.textPad(3, 45, palette()) == 0); }
  { HostDisplay d; d.setColor(UIColor::title_bkg); d.fillRect(0, 0, 240, 11);
    CHECK(d.skin.textPad(0, 2, palette()) == CompanionSkin::kTextPad);
    CHECK(d.skin.textPad(0, 40, palette()) == CompanionSkin::kBodyPad);   // carousel body text at x 0
    CHECK(d.skin.textPad(60, 40, palette()) == 0); }
  { HostDisplay d; CHECK(d.skin.textPad(0, 40, palette()) == 0); }   // no carousel: no body pad
  if (!failures) std::printf("PASS classify\n");
}

static uint16_t at(const HostDisplay& d, int x, int y) { return d.fb.get(x, y); }

static void render() {
  // A tile: cyan-edged, rounded (corner pixel is backdrop), tile fill inside.
  HostDisplay d;
  d.startFrame(UIColor::window_bkg);
  drawButton(d, 0, 130, 80, 18, "", false);
  drawButton(d, 80, 130, 80, 18, "", true);
  d.endFrame();
  CHECK(at(d, 80, 130 * 2 + 18) == theme::kTileOffFill);
  CHECK(at(d, 240, 130 * 2 + 18) == theme::kTileOnFill);
  // Corner: rounded, so the very corner pixel is the backdrop.
  CHECK(at(d, 2, 130 * 2 + 2) == d.back[(130 * 2 + 2) * 480 + 2]);
  // Lit edge is the accent; tiles are separated by a backdrop gap.
  bool accentEdge = false;
  for (int y = 130 * 2; y < 130 * 2 + 36; y++) accentEdge |= at(d, 162, y) == theme::kAccent || at(d, 161, y) == theme::kAccent;
  CHECK(accentEdge);
  CHECK(at(d, 159, 130 * 2 + 18) == d.back[(130 * 2 + 18) * 480 + 159]);
  // No legacy colours survive on the glass.
  int legacy = 0;
  for (int y = 0; y < 296; y++)
    for (int x = 0; x < 480; x++) {
      const uint16_t c = at(d, x, y);
      legacy += c == UIColor::title_bkg || c == UIColor::corp_blue || c == UIColor::secondary_txt;
    }
  CHECK(legacy == 0);
  // The hash sees a selection move.
  HostDisplay e;
  e.startFrame(UIColor::window_bkg);
  drawButton(e, 0, 130, 80, 18, "", true);
  drawButton(e, 80, 130, 80, 18, "", false);
  CHECK(e.skin.hash() != d.skin.hash());
  // Bounded.
  CompanionSkin big;
  for (int i = 0; i < 1000; i++) big.fill(0, 0, 10, 10, UIColor::title_bkg);
  CHECK(big.count() == CompanionSkin::kMaxOps);
  // Sprite overlay: background pixels take the composed frame, others the LUT.
  uint16_t dst[4] = {1, 2, 3, 4};
  const uint8_t src[4] = {7, 9, 7, 200};
  uint16_t lut[256];
  for (int i = 0; i < 256; i++) lut[i] = (uint16_t)(1000 + i);
  CompanionSkin::overlaySprite(dst, src, 4, 7, lut);
  CHECK(dst[0] == 1 && dst[1] == 1009 && dst[2] == 3 && dst[3] == 1200);
  // The home carousel's body sits in a panel; a clear inside it keeps the panel.
  HostDisplay c;
  c.startFrame(UIColor::window_bkg);
  c.setColor(UIColor::title_bkg); c.fillRect(0, 0, 240, 11);
  c.setColor(UIColor::window_bkg); c.fillRect(20, 60, 100, 30);
  c.endFrame();
  CHECK(at(c, 240, 100) == theme::kPanelFill);
  CHECK(at(c, 100, 150) == theme::kPanelFill);   // inside the clear
  CHECK(at(c, 240, 28) == c.back[28 * 480 + 240]);   // the page-dot strip is backdrop
  if (!failures) std::printf("PASS render\n");
}

// ---- previews: the HEAD screens as HeadAdmin / HeadCompose draw them ----

static void header(DisplayDriver& d, const char* rptr, bool logged) {
  drawButton(d, 0, 0, 96, 18, rptr, false);
  drawButton(d, 96, 0, 240 - 96, 18, logged ? "Logged in" : "Login", logged);
}

static void list(DisplayDriver& d, const char* const* rows, int n, int lit, const char* back, bool up) {
  const int top = 18, row_h = (148 - top) / 8;
  for (int r = 0; r < 7; r++) drawButton(d, 0, top + r * row_h, 240, row_h, r < n ? rows[r] : "", r == lit, r < n);
  const int y = top + 7 * row_h, w = 240 / 3;
  drawButton(d, 0, y, w, row_h, "Up", false, up);
  drawButton(d, w, y, w, row_h, "Down", false);
  drawButton(d, 2 * w, y, 240 - 2 * w, row_h, back, false);
}

static bool save(HostDisplay& d, const char* dir, const char* name) {
  d.endFrame();
  return d.save((std::string(dir) + "/" + name + ".rgb565").c_str());
}

static int preview(const char* dir) {
  bool ok = true;
  { // Admin console (logged in, Ver output).
    HostDisplay d; d.startFrame(UIColor::window_bkg); d.setTextSize(1);
    header(d, "Rptr:CaptCallahan", true);
    d.setColor(UIColor::primary_txt);
    const char* out[] = {"> ver", "v1.17.1-dev-7a54642e (Build: 30-Oct-2026)", "> get radio", "910.5250244,62.5,7,5"};
    for (int i = 0; i < 4; i++) { d.setCursor(2, 22 + i * 10); d.print(out[i]); }
    const char* labels[] = {"Ver", "Nbrs", "Clock", "Radio", "Cmd", "Settings", "Advert", "Presets", "Back"};
    for (int i = 0; i < 9; i++) {
      const int x0 = (i % 3) * 240 / 3, x1 = (i % 3 + 1) * 240 / 3;
      drawButton(d, x0, 85 + (i / 3) * 21, x1 - x0, 21, labels[i], false, true);
    }
    ok = save(d, dir, "head_console") && ok;
  }
  { // Repeater picker, home first.
    HostDisplay d; d.startFrame(UIColor::window_bkg); d.setTextSize(1);
    header(d, "Rptr:CaptCallahan", true);
    const char* rows[] = {"HOME  CaptCallahan", "rptr 57f72334", "KG7QIN R-Observer", "Downtown Repeater",
                          "White Rock Bosa", "North Everett", "Woofy Repeater"};
    list(d, rows, 7, 0, "Back (55)", false);
    ok = save(d, dir, "head_picker") && ok;
  }
  { // Settings list.
    HostDisplay d; d.startFrame(UIColor::window_bkg); d.setTextSize(1);
    header(d, "Rptr:CaptCallahan", true);
    const char* rows[] = {"bridge.enabled", "bridge.format", "bridge.source", "bridge.channel",
                          "Forget password", "name", "radio"};
    list(d, rows, 7, -1, "Back", true);
    ok = save(d, dir, "head_settings") && ok;
  }
  { // Presets list (renderPresets).
    HostDisplay d; d.startFrame(UIColor::window_bkg); d.setTextSize(1);
    header(d, "Rptr:CaptCallahan", true);
    d.setColor(UIColor::secondary_txt); d.setCursor(2, 20); d.print("Tap a preset to load it");
    const char* rows[] = {"1 USA-Canada 910.525", "2 Test 925.5", "3 -", "4 -", "5 -"};
    const int top = 30, row_h = (148 - 21 - 2 - top) / 5;
    for (int i = 0; i < 5; i++) drawButton(d, 0, top + i * row_h, 240, row_h, rows[i], i == 0, i < 2);
    drawButton(d, 0, 148 - 21, 120, 21, "Refresh", false);
    drawButton(d, 120, 148 - 21, 120, 21, "Back", false);
    ok = save(d, dir, "head_presets") && ok;
  }
  { // Login entry (renderEntry): masked password, Login action.
    HostDisplay d; d.startFrame(UIColor::window_bkg); d.setTextSize(1);
    header(d, "Rptr:CaptCallahan", false);
    TouchKeyboard kb; kb.reset("abcdefghi", 63, true);
    d.setColor(UIColor::secondary_txt); d.setCursor(2, 20); d.print("Admin password:");
    d.setColor(UIColor::primary_txt); kb.drawText(d, 30);
    d.setColor(UIColor::secondary_txt); d.drawRect(0, 28, 240, 12);
    kb.draw(d, 40, 21, "Login");
    ok = save(d, dir, "head_login") && ok;
  }
  { // Compose with the 123 page (HeadCompose::render).
    HostDisplay d; d.startFrame(UIColor::window_bkg); d.setTextSize(1);
    drawHeaderButton(d, 0, 80, "To:Public");
    drawHeaderButton(d, 80, 160, "As:CaptCallahan");
    TouchKeyboard kb; kb.reset("Test Ab");
    kb.touch(4, 32 + 2 * 22 + 10, 240, 32, 22);    // mode: ABC
    kb.touch(4, 32 + 2 * 22 + 10, 240, 32, 22);    // mode: 123
    d.setColor(UIColor::primary_txt); kb.drawText(d, 21);
    d.setColor(UIColor::secondary_txt); d.drawRect(0, 19, 240, 12);
    kb.draw(d, 32, 22, "Send");
    ok = save(d, dir, "head_compose") && ok;
  }
  { // Companion home carousel page (header bar, page dots, title).
    HostDisplay d; d.startFrame(UIColor::window_bkg); d.setTextSize(1);
    d.setColor(UIColor::title_bkg); d.fillRect(0, 0, 240, 11);
    d.setColor(UIColor::title_txt); d.setCursor(0, 2); d.print("3523E3E8");
    d.setCursor(226, 2); d.print("0m");
    d.setColor(UIColor::corp_blue);
    for (int i = 0, x = 5; i < 8; i++, x += 10) { if (i == 0) d.fillRect(x - 1, 12, 4, 4); else d.fillRect(x, 13, 2, 2); }
    d.setColor(UIColor::primary_txt); d.setTextSize(2); d.drawTextCentered(120, 21, "HISTORY: 0");
    d.setTextSize(1); d.setColor(UIColor::secondary_txt); d.drawTextCentered(120, 46, "tap center: messages");
    ok = save(d, dir, "head_carousel") && ok;
  }
  std::printf(ok ? "PASS preview\n" : "FAIL preview\n");
  return ok ? 0 : 1;
}

int main(int argc, char** argv) {
  const std::string n = argc > 1 ? argv[1] : "";
  if (n == "preview") return preview(argc > 2 ? argv[2] : ".");
  if (n == "roles") roles();
  else if (n == "classify") classify();
  else if (n == "render") render();
  else { std::printf("unknown %s\n", n.c_str()); return 2; }
  return failures ? 1 : 0;
}
'''

PREVIEWS = ('head_console', 'head_picker', 'head_settings', 'head_presets', 'head_login', 'head_compose',
            'head_carousel')


def build(work):
    compiler = shutil.which('g++') or shutil.which('clang++')
    if compiler is None:
        raise unittest.SkipTest('a host C++17 compiler is required')
    sanitize = [] if os.name == 'nt' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    cpp = work / 'cskin.cpp'
    cpp.write_text(HARNESS, encoding='ascii')
    exe = work / ('cskin' + ('.exe' if os.name == 'nt' else ''))
    built = subprocess.run([compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                            *sanitize, '-I', str(UI), '-I', str(UI_NEW), '-isystem', str(ROOT / 'src'),
                            '-isystem', str(ROOT / 'test' / 'mocks'), str(cpp), str(UI / 'FrameBuffer565.cpp'),
                            str(UI / 'FrameBuffer332.cpp'), str(UI / 'FbFonts.cpp'), str(UI / 'FbFontsAA.cpp'),
                            '-o', str(exe)], capture_output=True, text=True, timeout=180)
    if built.returncode != 0:
        raise AssertionError(built.stdout + built.stderr)
    return exe


class CompanionSkinTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='freenove-cskin-')
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

    def test_roles(self):
        self.run_case('roles')

    def test_classify(self):
        self.run_case('classify')

    def test_render(self):
        self.run_case('render')

    def test_preview_renders(self):
        ran = subprocess.run([str(self.exe), 'preview', self.temp.name], capture_output=True, text=True, timeout=60)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_skin_uses_theme_tokens_only(self):
        src = (UI / 'CompanionSkin.h').read_text(encoding='ascii')
        body = src.split('namespace freenove_ui {', 1)[1]
        self.assertNotRegex(body, r'rgb565\(\s*\d')   # colours come from Theme.h
        self.assertNotIn('Arduino.h', src)
        self.assertNotIn('#include <vector>', src)

    def test_freenove_display_uses_skin(self):
        src = (VARIANT / 'FreenoveDisplay.cpp').read_text(encoding='ascii')
        hdr = (VARIANT / 'FreenoveDisplay.h').read_text(encoding='ascii')
        self.assertIn('FREENOVE_HMI_SKIN', hdr)
        self.assertIn('_skin.fill(', src)
        self.assertIn('_skin.rect(', src)
        self.assertIn('_shownSkin.draw(', src)
        self.assertIn('overlaySprite(', src)
        self.assertIn('displayNightActive()', src)   # night palette keeps the legacy path

    def test_other_boards_untouched(self):
        # The skin lives in the visual heads' display adapters only (Freenove,
        # and the CYD's visual head build): LGFXDisplay, the DisplayDriver API
        # and the companion screens do not know about it, so every other
        # board draws exactly as before.
        shared = [ROOT / 'src' / 'helpers' / 'ui' / 'LGFXDisplay.cpp', ROOT / 'src' / 'helpers' / 'ui' / 'LGFXDisplay.h',
                  ROOT / 'src' / 'helpers' / 'ui' / 'DisplayDriver.h', UI_NEW / 'HeadAdmin.h', UI_NEW / 'HeadCompose.h',
                  UI_NEW / 'TouchKeyboard.h', UI_NEW / 'UITask.cpp']
        for p in shared:
            self.assertNotIn('CompanionSkin', p.read_text(encoding='utf-8', errors='replace'), p.name)
            self.assertNotIn('HMI_SKIN', p.read_text(encoding='utf-8', errors='replace'), p.name)
        # The CYD uses it only in its visual head build (CYD_VISUAL_HEAD).
        import re
        cyd = re.sub(r'//[^\n]*', '', (ROOT / 'variants' / 'cyd_espnow_head' / 'CYDDisplay.h').read_text(encoding='utf-8'))
        visual = cyd[cyd.index('#if defined(FREENOVE_VISUAL_HEAD)'):]
        self.assertIn('CompanionSkin.h', visual[:visual.index('#else')])
        self.assertNotIn('CompanionSkin', cyd[:cyd.index('#if defined(FREENOVE_VISUAL_HEAD)')])
        others = [p for p in (ROOT / 'variants').glob('**/*')
                  if p.is_file() and p.suffix in ('.h', '.cpp') and 'freenove_s3_35_head' not in p.parts
                  and 'cyd_espnow_head' not in p.parts]
        for p in others:
            self.assertNotIn('CompanionSkin', p.read_text(encoding='utf-8', errors='replace'), str(p))


def write_preview():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from test_layouts import rgb565_rows, write_png
    with tempfile.TemporaryDirectory(prefix='freenove-cskin-preview-') as tmp:
        exe = build(Path(tmp))
        ran = subprocess.run([str(exe), 'preview', tmp], capture_output=True, text=True, timeout=60)
        if ran.returncode != 0:
            raise SystemExit(ran.stdout + ran.stderr)
        for name in PREVIEWS:
            raw = (Path(tmp) / (name + '.rgb565')).read_bytes()
            write_png(rgb565_rows(raw, scale=1), PREVIEW_DIR / (name + '_hmi_1x.png'))
            write_png(rgb565_rows(raw, scale=2), PREVIEW_DIR / (name + '_hmi.png'))
            print('wrote', PREVIEW_DIR / (name + '_hmi_1x.png'), 'and the 2x zoom')


if __name__ == '__main__':
    if '--preview' in sys.argv:
        write_preview()
    else:
        unittest.main()
