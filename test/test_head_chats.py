"""Repeater-head chat screen (UI_HEAD_CHAT_LIST, Freenove): the pure
conversation helpers (src/helpers/ui/HeadChatList.h) and the list / thread
view (examples/companion_radio/ui-new/HeadChats.h) drawn on a recording
DisplayDriver over the companion's real CompanionMessageHistory store.

Host harness (g++ -std=c++11 -Wall -Wextra -Werror; ASan/UBSan off Windows).
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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <helpers/ui/CompanionMessageHistory.h>
#include <helpers/ui/HeadChatList.h>
#include "HeadChats.h"

ColorVal UIColor::window_bkg = 1, UIColor::title_bkg = 2, UIColor::title_txt = 3, UIColor::primary_txt = 4,
         UIColor::secondary_txt = 5, UIColor::warning_txt = 6, UIColor::popup_bkg = 7, UIColor::popup_txt = 8,
         UIColor::corp_blue = 9;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

using namespace mesh::ui;
typedef CompanionMessageHistory<32, 161> History;
typedef History::Entry Entry;
typedef chats::ChatRow ChatRow;

// Logical 240x148, 6 px per character, 10 px lines (the CYD/Freenove rule).
struct Text { int x, y; ColorVal c; std::string s; };
struct Box { int x, y, w, h; };
class RecDisplay : public DisplayDriver {
 public:
  RecDisplay() : DisplayDriver(240, 148) {}
  std::vector<Text> texts;
  std::vector<Box> boxes;   // drawRect outlines
  int rects = 0;
  ColorVal colour = 0;
  int cx = 0, cy = 0;
  bool isOn() override { return true; }
  void turnOn() override {}
  void turnOff() override {}
  void clear() override { texts.clear(); boxes.clear(); rects = 0; }
  void startFrame(ColorVal) override { clear(); }
  void setTextSize(int) override {}
  void setColor(ColorVal c) override { colour = c; }
  void setCursor(int x, int y) override { cx = x; cy = y; }
  void print(const char* s) override { texts.push_back(Text{cx, cy, colour, s}); cx += 6 * (int)std::strlen(s); }
  void fillRect(int, int, int, int) override { rects++; }
  void drawRect(int x, int y, int w, int h) override { rects++; boxes.push_back(Box{x, y, w, h}); }
  void drawXbm(int, int, const uint8_t*, int, int) override {}
  uint16_t getTextWidth(const char* s) override { return (uint16_t)(6 * std::strlen(s)); }
  int textLineHeight() override { return 10; }
  void endFrame() override {}
  const Text* find(const char* needle) const {
    for (size_t i = 0; i < texts.size(); i++) if (texts[i].s.find(needle) != std::string::npos) return &texts[i];
    return nullptr;
  }
};

static bool always(const Entry&) { return true; }
struct Six { int operator()(const char*, size_t) const { return 6; } };

// ------------------------------------------------------------ pure helpers

static void split_sender() {
  History h;
  h.add(1000, 0, "Public", "Public [2h]:", "Asta X1: hello there");
  h.add(1100, -1, nullptr, "Bob [direct]:", "hi: how are you");
  h.add(1200, 1, "#testing", "CaptCallahan", "CaptCallahan: on air", -1, true);
  h.add(1300, 1, "#testing", "#testing [1h]:", "no sender here");
  char who[36];
  const char* body = chats::splitSender(*h.newest(3), who, sizeof(who));
  CHECK(std::strcmp(who, "Asta X1") == 0 && std::strcmp(body, "hello there") == 0);
  body = chats::splitSender(*h.newest(2), who, sizeof(who));
  CHECK(std::strcmp(who, "Bob") == 0 && std::strcmp(body, "hi: how are you") == 0);   // a DM is from its peer
  body = chats::splitSender(*h.newest(1), who, sizeof(who));
  CHECK(std::strcmp(who, "You") == 0 && std::strcmp(body, "on air") == 0);
  body = chats::splitSender(*h.newest(0), who, sizeof(who));
  CHECK(who[0] == 0 && std::strcmp(body, "no sender here") == 0);
  CHECK(chats::peerLength("Bob [3h]:") == 3 && chats::peerLength("Bob") == 3 && chats::peerLength(nullptr) == 0);
  if (!failures) std::printf("PASS split_sender\n");
}

static void rows_group_and_order() {
  History h;
  h.add(1000, 0, "Public", "Public [2h]:", "A: one");
  h.add(1100, -1, nullptr, "Bob [direct]:", "dm one");
  h.add(1200, 1, "#testing", "#testing [1h]:", "C: two");
  h.add(1300, -1, nullptr, "Bob [2h]:", "dm two");
  h.add(1400, 0, "Public", "Public [0h]:", "D: three");
  h.markRead(h.newest(0));   // the newest Public post was read
  const int ch[3] = {0, 1, 2};
  const char* names[3] = {"Public", "#testing", "#quiet"};
  const char* dms[2] = {"Bob", "Carol"};
  ChatRow rows[8];
  const int n = chats::buildRows(h, always, ch, names, 3, dms, 2, rows, 8);
  CHECK(n == 5);
  // Newest activity first: Public (age 0), Bob (1), #testing (2); then quiet
  // channels, then quiet contacts.
  CHECK(rows[0].channel_idx == 0 && std::strcmp(rows[0].name, "Public") == 0 && rows[0].count == 2 && rows[0].unread == 1);
  CHECK(rows[0].newest_age == 0);
  CHECK(rows[1].channel_idx == chats::kDirect && std::strcmp(rows[1].name, "Bob") == 0 && rows[1].count == 2 &&
        rows[1].unread == 2 && rows[1].newest_age == 1);
  CHECK(rows[2].channel_idx == 1 && rows[2].newest_age == 2 && rows[2].count == 1);
  CHECK(rows[3].channel_idx == 2 && std::strcmp(rows[3].name, "#quiet") == 0 && rows[3].newest_age == -1 && rows[3].count == 0);
  CHECK(rows[4].channel_idx == chats::kDirect && std::strcmp(rows[4].name, "Carol") == 0 && rows[4].newest_age == -1);
  // Thread membership.
  CHECK(chats::inThread(*h.newest(1), rows[1]) && chats::inThread(*h.newest(3), rows[1]));
  CHECK(!chats::inThread(*h.newest(0), rows[1]) && chats::inThread(*h.newest(4), rows[0]));
  // Cap respected.
  CHECK(chats::buildRows(h, always, ch, names, 3, dms, 2, rows, 2) == 2);
  if (!failures) std::printf("PASS rows_group_and_order\n");
}

static std::vector<std::string> wrap(const char* s, int width) {
  std::vector<std::string> out;
  const char* p = s;
  while (*p) {
    const char* next = p;
    const size_t len = chats::nextLine(p, width, Six(), &next);
    out.push_back(std::string(p, len));
    if (next == p) break;
    p = next;
  }
  return out;
}

static void wrap_lines() {
  std::vector<std::string> l = wrap("the quick brown fox", 60);   // 10 chars a line
  CHECK(l.size() == 2 && l[0] == "the quick" && l[1] == "brown fox");
  l = wrap("abcdefghijklmnop", 60);                                 // a long word breaks inside
  CHECK(l.size() == 2 && l[0] == "abcdefghij" && l[1] == "klmnop");
  l = wrap("one\ntwo", 600);
  CHECK(l.size() == 2 && l[0] == "one" && l[1] == "two");
  l = wrap("1234567890 next", 60);                                  // exact fit, then a space
  CHECK(l.size() == 2 && l[0] == "1234567890" && l[1] == "next");
  CHECK(chats::countLines("", 60, Six()) == 1 && chats::countLines("a b c d e f g h i j k l", 60, Six()) == 3);
  l = wrap("\xC3\xA9\xC3\xA9\xC3\xA9", 12);                        // UTF-8 characters are not split
  CHECK(l.size() == 2 && l[0] == "\xC3\xA9\xC3\xA9" && l[1] == "\xC3\xA9");
  CHECK(chats::clampScroll(-5, 300, 100) == 0 && chats::clampScroll(500, 300, 100) == 200 &&
        chats::clampScroll(50, 80, 100) == 0);
  if (!failures) std::printf("PASS wrap_lines\n");
}

// ------------------------------------------------------------ the view

typedef HeadChatsView<History> View;
static const int kCh[2] = {0, 1};
static const char* kChNames[2] = {"Public", "#testing"};
static const char* kDms[1] = {"Bob"};
static const View::Sources kSrc = {kCh, kChNames, 2, kDms, 1};

static View::Action tap(View& v, int x, int y) {
  v.handleTouch(true, x, y, 240, 148);
  return v.handleTouch(false, x, y, 240, 148);
}

static void list_then_thread() {
  History h;
  for (int i = 0; i < 20; i++) {
    char msg[96];
    std::snprintf(msg, sizeof(msg), "User%d: message number %d with enough words to wrap onto a second line", i, i);
    h.add(1000 + (uint64_t)i * 1000, 0, "Public", "Public [1h]:", msg);
  }
  h.add(30000, -1, nullptr, "Bob [direct]:", "are you there?");
  View v;
  RecDisplay d;
  v.showList();
  v.render(d, h, kSrc, 90000, true, always);
  CHECK(!v.inThread());
  CHECK(d.find("Home") && d.find("New") && d.find("Chats"));
  CHECK(d.find("21 new") != nullptr);                       // header: all unread
  const Text* bob = d.find("@ Bob");
  const Text* pub = d.find("# Public");
  const Text* test = d.find("#testing");
  CHECK(bob && pub && test && bob->y < pub->y && pub->y < test->y);   // newest activity first
  CHECK(d.find("1m") != nullptr);                           // Bob: 60 s ago
  CHECK(d.find("are you there?") && d.find("No messages yet"));
  CHECK(d.find("20 new") != nullptr);                       // Public's unread badge
  // Tap the Public row: its thread, newest at the bottom, pinned.
  CHECK(pub != nullptr);
  if (pub == nullptr) return;
  CHECK(tap(v, 120, pub->y + 4) == View::Action::Changed && v.inThread());
  CHECK(v.thread().channel_idx == 0);
  d.clear();
  v.render(d, h, kSrc, 90000, true, always);
  CHECK(v.maxScroll() > 0 && v.scroll() == v.maxScroll());
  const Text* last = d.find("User19");
  const Text* bar = d.find("Write message");
  CHECK(last && bar && last->y < bar->y && bar->y - last->y < 60);   // newest just above the bar
  CHECK(d.find("User0") == nullptr);                        // oldest scrolled off the top
  CHECK(d.find("Up") && d.find("Down"));
  CHECK(h.unreadCount() == 1);                              // reading marked Public read (Bob's left)
  // Message lines are wholly inside the body.
  for (size_t i = 0; i < d.texts.size(); i++) {
    const Text& t = d.texts[i];
    if (t.s == "Write message" || t.s == "Up" || t.s == "Down") continue;
    CHECK(t.y >= View::kHeaderH || t.x < 120 || t.y < 18);
    CHECK(t.y + 10 <= 148 - View::kBarH || t.y < View::kHeaderH);
  }
  // Oldest -> newest top to bottom.
  const Text* u18 = d.find("User18");
  CHECK(u18 && last && u18->y < last->y);
  // Up scrolls toward older messages; Down returns.
  const int bottom = v.scroll();
  CHECK(tap(v, 10, 140) == View::Action::Changed && v.scroll() < bottom);
  d.clear();
  v.render(d, h, kSrc, 90000, true, always);
  CHECK(tap(v, 230, 140) == View::Action::Changed);
  // A drag scrolls and never taps; dragging down shows older messages.
  const int before = v.scroll();
  v.handleTouch(true, 120, 40, 240, 148);
  for (int y = 40; y <= 100; y += 10) v.handleTouch(true, 120, y, 240, 148);
  CHECK(v.handleTouch(false, 120, 100, 240, 148) == View::Action::None);
  CHECK(v.scroll() == (before - 60 < 0 ? 0 : before - 60));
  // New message while scrolled up: stays put (not pinned).
  const int kept = v.scroll();
  h.add(95000, 0, "Public", "Public [0h]:", "Late: newest");
  d.clear();
  v.render(d, h, kSrc, 96000, true, always);
  CHECK(v.scroll() == kept);
  // Back to the bottom: pinned again, a new message keeps it at the bottom.
  v.handleKey(1); v.handleKey(1); v.handleKey(1); v.handleKey(1); v.handleKey(1);
  d.clear();
  v.render(d, h, kSrc, 96000, true, always);
  CHECK(v.scroll() == v.maxScroll());
  h.add(97000, 0, "Public", "Public [0h]:", "Later: even newer");
  d.clear();
  v.render(d, h, kSrc, 98000, true, always);
  CHECK(v.scroll() == v.maxScroll() && d.find("even newer") != nullptr);
  // Write: compose on this channel.
  CHECK(tap(v, 120, 140) == View::Action::Compose && v.composeChannel() == 0 && v.composePeer()[0] == 0);
  // Header "Chats" goes back to the list; there "Home" leaves.
  CHECK(tap(v, 10, 8) == View::Action::Changed && !v.inThread());
  d.clear();
  v.render(d, h, kSrc, 98000, true, always);
  CHECK(tap(v, 10, 8) == View::Action::Home);
  // New (no conversation) and a DM thread's compose target.
  CHECK(tap(v, 230, 8) == View::Action::Compose && v.composeChannel() == View::kNoTarget);
  bob = d.find("@ Bob");
  CHECK(bob != nullptr);
  if (bob == nullptr) return;
  CHECK(tap(v, 120, bob->y + 4) == View::Action::Changed && v.inThread());
  d.clear();
  v.render(d, h, kSrc, 98000, true, always);
  CHECK(d.find("are you there?") && d.find("Bob") && !d.find("User"));
  CHECK(tap(v, 120, 140) == View::Action::Compose && v.composeChannel() == chats::kDirect &&
        std::strcmp(v.composePeer(), "Bob") == 0);
  // Button keys: enter = back.
  CHECK(v.handleKey(0) == View::Action::Changed && !v.inThread());
  CHECK(v.handleKey(0) == View::Action::Home);
  if (!failures) std::printf("PASS list_then_thread\n");
}

static void open_thread_of_incoming() {
  History h;
  h.add(1000, 1, "#testing", "#testing [1h]:", "Asta X1: T");
  View v;
  RecDisplay d;
  v.openThreadOf(*h.newest(0));
  CHECK(v.inThread() && v.thread().channel_idx == 1);
  v.render(d, h, kSrc, 2000, false, always);
  CHECK(d.find("#testing") && d.find("Asta X1") && d.find("T") && h.unreadCount() == 1);   // display off: still unread
  // A short thread sits on the bar: its only message just above "Write".
  const Text* only = d.find("Asta X1");
  const Text* write = d.find("Write message");
  CHECK(only && write && only->y > 80 && write->y - only->y <= 30);
  h.add(1500, -1, nullptr, "Bob [direct]:", "yo");
  v.openThreadOf(*h.newest(0));
  CHECK(v.thread().channel_idx == chats::kDirect && std::strcmp(v.thread().name, "Bob") == 0);
  // Empty store: the list says so (no channels, no contacts).
  History empty;
  View e;
  const View::Sources none = {nullptr, nullptr, 0, nullptr, 0};
  e.render(d, empty, none, 0, true, always);
  CHECK(d.find("No channels or chats yet") != nullptr);
  if (!failures) std::printf("PASS open_thread_of_incoming\n");
}

// The conversation list's scroll hints: up / down tiles in a column right
// of the rows, shown only where there are more conversations, drawn where
// they are hit; a tap scrolls a page; the rows never reach the column.
static bool drawnBox(const RecDisplay& d, int x, int y, int w, int h) {
  for (size_t i = 0; i < d.boxes.size(); i++)
    if (d.boxes[i].x == x && d.boxes[i].y == y && d.boxes[i].w == w && d.boxes[i].h == h) return true;
  return false;
}

static void list_hints() {
  History h;
  for (int i = 0; i < 12; i++) {
    char from[40];
    std::snprintf(from, sizeof(from), "Peer%d [direct]:", i);
    h.add(1000 + (uint64_t)i * 1000, -1, nullptr, from, "hello");
  }
  View v;
  RecDisplay d;
  v.showList();
  v.render(d, h, kSrc, 90000, true, always);
  CHECK(v.maxScroll() > 0);
  int x, y, w, hh;
  // Top: no up tile; a down tile, drawn exactly where it is hit.
  CHECK(!v.listHint(false, x, y, w, hh));
  CHECK(v.listHint(true, x, y, w, hh));
  CHECK(drawnBox(d, x, y, w, hh) && x + w <= 240 && y + hh <= 148);
  const int column = x;
  for (size_t i = 0; i < d.boxes.size(); i++) {   // rows stay left of the column
    const Box& b = d.boxes[i];
    if (b.w > 40 && b.y >= View::kHeaderH) CHECK(b.x + b.w <= column);
  }
  // The empty top of the column is not a control: nothing happens.
  CHECK(tap(v, column + 2, View::kHeaderH + 4) == View::Action::None && !v.inThread() && v.scroll() == 0);
  // Down tile: a page down (middle: both tiles).
  CHECK(tap(v, x + w / 2, y + hh / 2) == View::Action::Changed && v.scroll() > 0 && !v.inThread());
  d.clear();
  v.render(d, h, kSrc, 90100, true, always);
  int ux, uy, uw, uh;
  CHECK(v.listHint(false, ux, uy, uw, uh) && drawnBox(d, ux, uy, uw, uh));
  if (v.scroll() < v.maxScroll()) CHECK(v.listHint(true, x, y, w, hh) && drawnBox(d, x, y, w, hh));
  // To the bottom: the down tile goes, the up tile stays.
  for (int i = 0; i < 20 && v.listHint(true, x, y, w, hh); i++) {
    tap(v, x + w / 2, y + hh / 2);
    d.clear();
    v.render(d, h, kSrc, 90200, true, always);
  }
  CHECK(v.scroll() == v.maxScroll() && !v.listHint(true, x, y, w, hh) && v.listHint(false, ux, uy, uw, uh));
  CHECK(!drawnBox(d, x, y, w, hh));
  // Up tile: back up a page.
  const int bottom = v.scroll();
  CHECK(tap(v, ux + uw / 2, uy + uh / 2) == View::Action::Changed && v.scroll() < bottom);
  // A short list: no column, no tiles, rows full width.
  History few;
  few.add(1000, -1, nullptr, "Bob [direct]:", "hi");
  View s;
  RecDisplay ds;
  s.showList();
  s.render(ds, few, kSrc, 90000, true, always);
  CHECK(!s.listHint(false, x, y, w, hh) && !s.listHint(true, x, y, w, hh));
  bool wide = false;
  for (size_t i = 0; i < ds.boxes.size(); i++) wide |= ds.boxes[i].w == 240;
  CHECK(wide);
  if (!failures) std::printf("PASS list_hints\n");
}

int main(int argc, char** argv) {
  const std::string name = argc > 1 ? argv[1] : "";
  if (name == "split_sender") split_sender();
  else if (name == "rows_group_and_order") rows_group_and_order();
  else if (name == "wrap_lines") wrap_lines();
  else if (name == "list_then_thread") list_then_thread();
  else if (name == "open_thread_of_incoming") open_thread_of_incoming();
  else if (name == "list_hints") list_hints();
  else { std::printf("unknown %s\n", name.c_str()); return 2; }
  return failures ? 1 : 0;
}
'''


class HeadChatsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            raise unittest.SkipTest('a host C++ compiler is required')
        sanitize = [] if os.name == 'nt' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        cls.temp = tempfile.TemporaryDirectory(prefix='head-chats-')
        work = Path(cls.temp.name)
        cpp = work / 'chats.cpp'
        cpp.write_text(HARNESS, encoding='ascii')
        cls.exe = work / ('chats' + ('.exe' if os.name == 'nt' else ''))
        # DisplayDriver.h (shared, pre-existing) has an unused parameter.
        built = subprocess.run([
            compiler, '-std=c++11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', *sanitize,
            '-I', str(UI_NEW), '-I', str(ROOT / 'src'), str(cpp), '-o', str(cls.exe),
        ], capture_output=True, text=True, timeout=180)
        if built.returncode != 0:
            cls.temp.cleanup()
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_scenario(self, name):
        ran = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=30)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('PASS ' + name, ran.stdout)

    def test_split_sender(self):
        self.run_scenario('split_sender')

    def test_rows_group_and_order(self):
        self.run_scenario('rows_group_and_order')

    def test_wrap_lines(self):
        self.run_scenario('wrap_lines')

    def test_list_then_thread(self):
        self.run_scenario('list_then_thread')

    def test_open_thread_of_incoming(self):
        self.run_scenario('open_thread_of_incoming')

    def test_list_hints(self):
        self.run_scenario('list_hints')

    def test_wiring_is_freenove_only(self):
        # The chat list replaces the one-message swipe view only where
        # UI_HEAD_CHAT_LIST is set (the visual heads: Freenove and the CYD).
        ui = (UI_NEW / 'UITask.cpp').read_text(encoding='utf-8')
        self.assertIn('#include "HeadChats.h"', ui)
        self.assertRegex(ui, r'#if UI_HEAD_CHAT_LIST')
        freenove = (ROOT / 'variants' / 'freenove_s3_35_head' / 'platformio.ini').read_text(encoding='utf-8')
        self.assertRegex(freenove, r'-D UI_HEAD_CHAT_LIST=1')
        self.assertRegex(freenove, r'-D UI_MSG_PREVIEW_SIZE=161')
        cyd = (ROOT / 'variants' / 'cyd_espnow_head' / 'platformio.ini').read_text(encoding='utf-8')
        self.assertRegex(cyd, r'-D UI_HEAD_CHAT_LIST=1')
        self.assertRegex(cyd, r'-D UI_MSG_PREVIEW_SIZE=161')
        others = (ROOT / 'platformio.ini').read_text(encoding='utf-8')
        self.assertNotIn('UI_HEAD_CHAT_LIST=1', others)
        # The view reuses the companion's store: no message buffer of its own.
        view = (UI_NEW / 'HeadChats.h').read_text(encoding='utf-8')
        self.assertNotRegex(view, r'Entry\s+_\w+\s*\[')
        self.assertNotIn('the_mesh', view)


if __name__ == '__main__':
    unittest.main()
