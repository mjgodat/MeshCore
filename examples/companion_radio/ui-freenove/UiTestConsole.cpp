// USB test console for the Freenove head (FREENOVE_UITEST builds only; env
// Freenove_S3_35_Head_uitest). The USB CDC carries this line console instead
// of the companion protocol. Host side: .superpowers/uitest/ui.py.
//
//   tap X Y                   press + release at (X, Y) (480x320 landscape;
//                             CYD: 320x240)
//   swipe X1 Y1 X2 Y2 MS      press, move over MS ms, release
//   shot [A B]                the glass (rows A..B) (FramePusher's RGB565 shadow):
//                               "SHOT 480 320", one "Q<row> <base64>" per row
//                               (runs: count, pixel lo, pixel hi),
//                               "END SHOT"
//   page N                    show page N (0 HOME .. 4 HEAD)
//   status                    page, display, companion screen / admin state,
//                             contacts, home repeater
//   log                       the admin console's terminal log (tail)
//   wake                      wake the screen as a touch would
//   contacts                  the contact table (type, key prefix, lastmod, name)
//   inject CH FROM TEXT...    a received message into the companion UI's store
//                             as if heard (nothing is sent): CH = channel index
//                             or -1 for a DM from FROM; "_" in FROM = space
//   stats                     (CYD) the page task's last 10 s: fps, render and
//                             push ms per frame, bands per frame, free heap
//   bars                      (CYD) colour bars on the panel (colour order
//                             check) until the next frame
// CYD (HMI_PROFILE_CYD): the console runs at UITEST_BAUD (921600) on the
// CH340 UART; "shot" reads the glass back from the ST7789 (rows are raw
// RGB565 "R<row>" when that is shorter than the runs).
// Every command ends with one "OK <cmd>" or "ERR <why>" line; tap/swipe
// answer "DONE tap" once the gesture has been released. Never prints a
// password: the terminal log only ever holds "login ****".

#if defined(FREENOVE_UITEST) && FREENOVE_UITEST

#ifndef UITEST_BAUD
#define UITEST_BAUD 921600
#endif

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <esp_heap_caps.h>

#include <esp_timer.h>

#include "UiTest.h"
#include "target.h"
#if HMI_CYD
#include "StubPages.h"
#endif
#include <Preferences.h>
#include <helpers/ui/DisplayPowerPolicy.h>
#include "../MyMesh.h"
#include "../HeadTerminalLog.h"
#include "../HeadContactPolicy.h"
#include "UITask.h"

extern MyMesh the_mesh;
extern UITask ui_task;

namespace freenove_uitest {

namespace {

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
bool s_active = false;
bool s_started = false;
int s_x1, s_y1, s_x2, s_y2;
uint32_t s_t0 = 0, s_dur = 0;
std::atomic<int> s_pageReq{-1};
std::atomic<int> s_page{0};
const char* volatile s_pageTitle = "?";

}  // namespace

void startGesture(int x1, int y1, int x2, int y2, uint32_t dur_ms) {
  portENTER_CRITICAL(&s_mux);
  s_x1 = x1; s_y1 = y1; s_x2 = x2; s_y2 = y2;
  s_dur = dur_ms;
  s_started = false;
  s_active = true;
  portEXIT_CRITICAL(&s_mux);
}

bool gestureBusy() {
  portENTER_CRITICAL(&s_mux);
  const bool busy = s_active;
  portEXIT_CRITICAL(&s_mux);
  return busy;
}

bool overrideTouch(uint32_t now_ms, bool& down, int& x, int& y) {
  portENTER_CRITICAL(&s_mux);
  bool used = false;
  if (s_active) {
    // The gesture's clock starts at the page task's first sample (its
    // now_ms can be older than the console's millis()).
    if (!s_started) { s_started = true; s_t0 = now_ms; }
    const uint32_t t = now_ms - s_t0;
    if (t < s_dur) {
      down = true;
      x = s_x1 + (int)((int64_t)(s_x2 - s_x1) * (int32_t)t / (int32_t)s_dur);
      y = s_y1 + (int)((int64_t)(s_y2 - s_y1) * (int32_t)t / (int32_t)s_dur);
    } else if (t < s_dur + 17) {
      down = true;   // the end point, sampled at least once
      x = s_x2;
      y = s_y2;
    } else {
      down = false;
      x = y = -1;
      if (t >= s_dur + 17 + kReleaseMs) s_active = false;
    }
    used = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return used;
}

void requestPage(int index) { s_pageReq.store(index); }
int takePageRequest() { return s_pageReq.exchange(-1); }
const mesh::head::RelayNameStore* s_relayNames = nullptr;
void noteRelayNames(const mesh::head::RelayNameStore* store) { s_relayNames = store; }

void notePage(int index, const char* title) {
  s_pageTitle = title;
  s_page.store(index);
}
int currentPage() { return s_page.load(); }
const char* currentPageTitle() { return s_pageTitle; }

std::atomic<bool> s_shotReq{false};
std::atomic<bool> s_shotBusy{false};
ShotSink s_shotSink = nullptr;
void* s_shotCtx = nullptr;
void requestShot(ShotSink sink, void* ctx) {
  s_shotSink = sink;
  s_shotCtx = ctx;
  s_shotBusy.store(true);
  s_shotReq.store(true);
}
bool takeShotRequest(ShotSink& sink, void*& ctx) {
  if (!s_shotReq.exchange(false)) return false;
  sink = s_shotSink;
  ctx = s_shotCtx;
  return true;
}
void shotDone() { s_shotBusy.store(false); }
bool shotPending() { return s_shotBusy.load(); }

std::atomic<uint32_t> s_stressUntil{0};
std::atomic<int32_t> s_minIdle{-1};
std::atomic<unsigned> s_stackPages{0}, s_stackLoop{0};
bool stressActive() {
  const uint32_t until = s_stressUntil.load();
  return until != 0 && (int32_t)(until - millis()) > 0;
}
uint32_t minIdleOverride(uint32_t builtin) {
  const int32_t v = s_minIdle.load();
  return v >= 0 ? (uint32_t)v : builtin;
}
void noteStacks(unsigned pages, unsigned loop) {
  s_stackPages.store(pages);
  s_stackLoop.store(loop);
}
// Companion loop responsiveness: calls of freenoveUiTestLoop() (one per
// loop() pass) and the longest gap between two, since the last "stats".
std::atomic<uint32_t> s_loopCount{0}, s_loopMaxGapUs{0};
uint32_t s_loopLastUs = 0, s_loopWindowMs = 0;

std::atomic<uint32_t> s_realPresses{0};
std::atomic<int> s_realX{-1}, s_realY{-1};
bool s_realDown = false;
void noteRealTouch(bool down, int x, int y) {
  if (down && !s_realDown) {
    s_realPresses.fetch_add(1);
    s_realX.store(x);
    s_realY.store(y);
  }
  s_realDown = down;
}

portMUX_TYPE s_statsMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t s_stats[8] = {0, 0, 0, 0, 0, 0, 0, 0};
void noteStats(uint32_t frames, uint32_t window_ms, uint32_t render_us, uint32_t push_us, uint32_t bands,
               uint32_t chrome_us, uint32_t page_us, uint32_t header_us) {
  portENTER_CRITICAL(&s_statsMux);
  s_stats[0] = frames;
  s_stats[1] = window_ms;
  s_stats[2] = render_us;
  s_stats[3] = push_us;
  s_stats[4] = bands;
  s_stats[5] = chrome_us;
  s_stats[6] = page_us;
  s_stats[7] = header_us;
  portEXIT_CRITICAL(&s_statsMux);
}

}  // namespace freenove_uitest

namespace {

char s_line[96];
size_t s_lineLen = 0;
bool s_reportDone = false;
const char* s_doneName = "";

void writeLine(const char* text) {
  // One write per line: other writers (log lines from the page task) land
  // between lines, never inside one.
  // HWCDC accepts what fits in its TX buffer: retry the rest (bounded, so
  // a host that stopped reading cannot hang the loop).
  const uint8_t* p = (const uint8_t*)text;
  size_t left = strlen(text);
  uint32_t progress = millis();
  while (left > 0 && millis() - progress < 250) {
    const size_t n = Serial.write(p, left);
    p += n;
    left -= n;
    if (n == 0) delay(1);
    else progress = millis();
  }
  Serial.flush();   // drain before the next line: HWCDC drops on overflow
}

void reply(const char* fmt, ...) {
  char buf[320];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
  va_end(ap);
  if (n < 0) n = 0;
  if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
  buf[n++] = '\n';
  buf[n] = 0;
  writeLine(buf);
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64(const uint8_t* in, size_t len, char* out) {
  size_t o = 0;
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t b0 = in[i];
    const uint32_t b1 = i + 1 < len ? in[i + 1] : 0;
    const uint32_t b2 = i + 2 < len ? in[i + 2] : 0;
    const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    out[o++] = kB64[(v >> 18) & 63];
    out[o++] = kB64[(v >> 12) & 63];
    out[o++] = i + 1 < len ? kB64[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < len ? kB64[v & 63] : '=';
  }
  return o;
}

#if HMI_CYD
constexpr int kShotW = freenove_ui::profile::kScreenW, kShotH = freenove_ui::profile::kScreenH;

struct ShotRange { int first, last; };

// One row: "Q<row>" runs or "R<row>" raw pixels, whichever is shorter.
void shotRow(int row, const uint16_t* r, void* ctx) {
  const ShotRange* range = static_cast<const ShotRange*>(ctx);
  if (row < range->first || row > range->last) return;
  static char line[8 + ((kShotW * 3 + 2) / 3) * 4 + 2];
  static uint8_t runs[kShotW * 3];
  const int w = kShotW;
  size_t rn = 0;
  for (int x = 0; x < w;) {
    int k = 1;
    while (x + k < w && k < 255 && r[x + k] == r[x]) k++;
    runs[rn++] = (uint8_t)k;
    runs[rn++] = (uint8_t)(r[x] & 0xff);
    runs[rn++] = (uint8_t)(r[x] >> 8);
    x += k;
  }
  int n;
  if (rn <= (size_t)w * 2) {
    n = snprintf(line, 8, "Q%d ", row);
    n += (int)base64(runs, rn, line + n);
  } else {
    n = snprintf(line, 8, "R%d ", row);
    n += (int)base64((const uint8_t*)r, (size_t)w * 2, line + n);   // little-endian RGB565
  }
  line[n++] = '\n';
  line[n] = 0;
  writeLine(line);
}

// The frame as composed (CYDDisplay::dumpPage / dumpCompanion): the HEAD
// page here on the UI loop, a visual page by the page task.
void cmdShot(int first, int last) {
  static ShotRange range;
  range.first = first < 0 ? 0 : first;
  range.last = last >= kShotH ? kShotH - 1 : last;
  reply("SHOT %d %d", kShotW, kShotH);
  if (strcmp(freenove_uitest::currentPageTitle(), "HEAD") == 0) {
    if (!display.dumpCompanion(shotRow, &range)) {
      reply("ERR panel busy");
      return;
    }
  } else {
    freenove_uitest::requestShot(shotRow, &range);
    const uint32_t t0 = millis();
    while (freenove_uitest::shotPending() && millis() - t0 < 10000) delay(2);
    if (freenove_uitest::shotPending()) {
      reply("ERR page task busy");
      return;
    }
  }
  reply("END SHOT");
}

// Render micro-benchmarks on a private band buffer (UI loop): which
// primitive costs what on this CPU.
void cmdBench() {
  const int W = kShotW, H = kShotH, R = 40;
  uint16_t* buf = (uint16_t*)heap_caps_malloc((size_t)W * R * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (buf == nullptr) { reply("ERR no memory"); return; }
  FrameBuffer565 fb(buf, W, H, 0, R);
  const freenove_ui::PanelRect p = {4, 4, 190, 132};
  struct Item { const char* name; uint32_t us; };
  Item items[10];
  int n = 0;
  auto timeIt = [&](const char* name, int reps, void (*fn)(FrameBuffer565&)) {
    const int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < reps; i++) { fb.setBand(0, R); fn(fb); }
    items[n].name = name;
    items[n].us = (uint32_t)((esp_timer_get_time() - t0) / reps);
    n++;
  };
  timeIt("gradient_band", 20, [](FrameBuffer565& f) { freenove_ui::drawBackdrop(f, nullptr, 0); });
  timeIt("fill_band", 20, [](FrameBuffer565& f) { f.fillRect(0, 0, f.width(), 40, 0x1234); });
  timeIt("panel", 20, [](FrameBuffer565& f) {
    static const freenove_ui::PanelRect q = {4, 4, 190, 132};
    freenove_ui::drawPanel(f, q, "MESH TOPOLOGY", "GRAPH");
  });
  timeIt("roundrect", 50, [](FrameBuffer565& f) { f.fillRoundRect(10, 10, 75, 38, 4, 0x1234); f.strokeRoundRect(10, 10, 75, 38, 4, 0xFFFF); });
  timeIt("text14_20ch", 50, [](FrameBuffer565& f) { f.text(4, 10, "Capt4McNeil 1.2km via", 0xFFFF, kFontSmall); });
  timeIt("text16_title", 50, [](FrameBuffer565& f) { f.text(4, 10, "MESH TOPOLOGY", 0xFFFF, kFontTitle); });
  timeIt("aaArc_r17", 20, [](FrameBuffer565& f) { f.aaArc(100, 20, 17, 4, 150, 390, 0xFFFF); });
  timeIt("aaLine_100", 50, [](FrameBuffer565& f) { f.aaLine(0, 0, 100, 30, 1.0f, 0xFFFF); });
  timeIt("tabbar", 20, [](FrameBuffer565& f) { f.setBand(200, 40); freenove_ui::drawTabBar(f, 0, 5); });
  (void)p;
  heap_caps_free(buf);
  char line[300];
  int k = snprintf(line, sizeof(line), "BENCH");
  for (int i = 0; i < n && k < (int)sizeof(line) - 1; i++) k += snprintf(line + k, sizeof(line) - k, " %s=%lu", items[i].name, (unsigned long)items[i].us);
  reply("%s", line);
}

void cmdStats() {
  // Loop responsiveness since the last "stats" (then reset).
  const uint32_t nowMs = millis();
  const uint32_t window = nowMs - freenove_uitest::s_loopWindowMs;
  const double loopHz = window ? freenove_uitest::s_loopCount.exchange(0) * 1000.0 / window : 0.0;
  const double loopGapMs = freenove_uitest::s_loopMaxGapUs.exchange(0) / 1000.0;
  freenove_uitest::s_loopWindowMs = nowMs;
  uint32_t s[8];
  portENTER_CRITICAL(&freenove_uitest::s_statsMux);
  memcpy(s, freenove_uitest::s_stats, sizeof(s));
  portEXIT_CRITICAL(&freenove_uitest::s_statsMux);
  const double f = s[0] ? (double)s[0] : 1.0;
  reply("STATS frames=%lu window_ms=%lu fps=%.1f render_ms=%.1f chrome_ms=%.1f page_ms=%.1f header_ms=%.1f "
        "push_ms=%.1f frame_ms=%.1f bands=%.1f heap=%u min_heap=%u largest=%u cpu=%u real_presses=%lu last=%d,%d "
        "stack_free_pages=%u stack_free_loop=%u loop_hz=%.0f loop_max_gap_ms=%.1f",
        (unsigned long)s[0], (unsigned long)s[1], s[1] ? s[0] * 1000.0 / s[1] : 0.0, s[2] / 1000.0 / f,
        s[5] / 1000.0 / f, s[6] / 1000.0 / f, s[7] / 1000.0 / f,
        s[3] / 1000.0 / f, (s[2] + s[3]) / 1000.0 / f, s[4] / f,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)getCpuFrequencyMhz(),
        (unsigned long)freenove_uitest::s_realPresses.load(), freenove_uitest::s_realX.load(),
        freenove_uitest::s_realY.load(), freenove_uitest::s_stackPages.load(), freenove_uitest::s_stackLoop.load(),
        loopHz, loopGapMs);
}
#else
void cmdShot(int first, int last) {
  FramePusher& pusher = display.pusher();
  pusher.wait();
  const uint16_t* px = pusher.shadow565();
  if (px == nullptr) { reply("ERR no shadow"); return; }
  const int w = freenove::kLandscapeW, h = freenove::kLandscapeH;
  if (first < 0) first = 0;
  if (last >= h) last = h - 1;
  reply("SHOT %d %d", w, h);
  static char line[8 + ((freenove::kLandscapeW * 3 + 2) / 3) * 4 + 2];
  // Rows as runs: (count, pixel lo, pixel hi) triples, base64 ("Q<row>").
  // Screens are mostly flat colour, so this is a fraction of the raw row.
  static uint8_t runs[freenove::kLandscapeW * 3];
  for (int row = first; row <= last; row++) {
    const uint16_t* r = px + (size_t)row * w;
    size_t rn = 0;
    for (int x = 0; x < w;) {
      int k = 1;
      while (x + k < w && k < 255 && r[x + k] == r[x]) k++;
      runs[rn++] = (uint8_t)k;
      runs[rn++] = (uint8_t)(r[x] & 0xff);
      runs[rn++] = (uint8_t)(r[x] >> 8);
      x += k;
    }
    int n = snprintf(line, 8, "Q%d ", row);
    n += (int)base64(runs, rn, line + n);
    line[n++] = '\n';
    line[n] = 0;
    writeLine(line);
  }
  reply("END SHOT");
}
#endif

void cmdStatus() {
  char head[240];
  ui_task.describeHead(head, sizeof(head));
  const uint8_t* home = the_mesh.headHomeRepeater();
  char home_hex[16] = "none";
  if (home) snprintf(home_hex, sizeof(home_hex), "%02x%02x%02x%02x", home[0], home[1], home[2], home[3]);
  reply("STATUS page=%d:%s display=%s %s contacts=%d heard=%d home=%s uptime_s=%lu",
        freenove_uitest::currentPage(), freenove_uitest::currentPageTitle(),
        display.isOn() ? "on" : "off", head, the_mesh.getNumContacts(), the_mesh.headHeardCount(), home_hex,
        (unsigned long)(millis() / 1000));
}

void cmdLog() {
  static char tail[1100];
  headTerminalLog().copyTail(tail, sizeof(tail));
  reply("LOG BEGIN");
  char* p = tail;
  while (*p) {
    char* nl = strchr(p, '\n');
    if (nl) *nl = 0;
    // Drop CRs; prefix each line so the host can tell it from other output.
    char* cr;
    while ((cr = strchr(p, '\r')) != nullptr) memmove(cr, cr + 1, strlen(cr));
    if (*p) reply("L %s", p);
    if (!nl) break;
    p = nl + 1;
  }
  reply("LOG END");
}

// Every stored contact: type (1 chat, 2 repeater, 3 room, 4 sensor), key
// prefix, lastmod (head clock), favourite, home, drawable name.
void cmdContacts() {
  const int n = the_mesh.getNumContacts();
  reply("CONTACTS %d", n);
  for (int i = 0; i < n; i++) {
    ContactInfo c;
    if (!the_mesh.getContactByIdx(MAX_ANON_CONTACTS + i, c)) continue;
    char name[40];
    head_policy::displayName(c.name, c.id.pub_key, c.type, name, sizeof(name));
    reply("C %u %02x%02x%02x%02x %lu %s%s %s", (unsigned)c.type, c.id.pub_key[0], c.id.pub_key[1],
          c.id.pub_key[2], c.id.pub_key[3], (unsigned long)c.lastmod,
          (c.flags & 1) ? "fav" : "-", the_mesh.isHeadHomeRepeater(c) ? ",home" : "", name);
  }
  reply("END CONTACTS");
}

// Stored relay names (key prefix, name), oldest first. Test-only: read
// from the console while the page task may add one (benign for a dump).
void cmdRelays() {
  static uint8_t blob[mesh::head::RelayNameStore::kBlobMax];
  const size_t len = freenove_uitest::s_relayNames ? freenove_uitest::s_relayNames->serialize(blob, sizeof(blob)) : 0;
  const size_t n = len ? blob[3] : 0;
  reply("RELAYS %u", (unsigned)n);
  for (size_t i = 0; i < n; i++) {
    const uint8_t* e = blob + mesh::head::RelayNameStore::kBlobHeader + i * mesh::head::RelayNameStore::kBlobEntry;
    char name[mesh::head::RelayNameStore::kNameLen];
    memcpy(name, e + 4, sizeof(name));
    name[sizeof(name) - 1] = 0;
    reply("R %02x%02x%02x%02x %s", e[0], e[1], e[2], e[3], name);
  }
  reply("END RELAYS");
}

// Test-only: a received message, delivered to the UI exactly as MyMesh does
// (channel label "Ch N name" for a channel post). Nothing goes on air.
void cmdInject(char* rest) {
  char* ch = strtok(rest, " \t");
  char* from = strtok(nullptr, " \t");
  char* text = strtok(nullptr, "");
  if (ch == nullptr || from == nullptr || text == nullptr) {
    reply("ERR inject CH FROM TEXT");
    return;
  }
  for (char* p = from; *p; p++) if (*p == '_') *p = ' ';
  const int idx = atoi(ch);
  if (idx >= 0) {
    ChannelDetails details;
    if (!the_mesh.getChannel(idx, details) || details.name[0] == 0) {
      reply("ERR no channel %d", idx);
      return;
    }
    char label[64], body[200];
    snprintf(label, sizeof(label), "Ch %d %s", idx, details.name);
    snprintf(body, sizeof(body), "%s: %s", from, text);
    ui_task.newMsg(1, label, body, 0, idx, details.name, -1);
  } else {
    ui_task.newMsg(0xFF, from, text, 0, -1, nullptr, -1);
  }
  reply("OK inject");
}

void run(char* line) {
  // Test-only: one line for the companion text terminal (the USB terminal of
  // the product build, e.g. `get transport`, `set espnow.channel 2`); the
  // reply lands in the terminal log (`log`), never echoed here.
  if (strncmp(line, "term ", 5) == 0) {
    the_mesh.handleTerminalCommand(line + 5);
    reply("OK term");
    return;
  }
  if (strncmp(line, "inject ", 7) == 0) {
    cmdInject(line + 7);
    return;
  }
  char* argv[8];
  int argc = 0;
  for (char* tok = strtok(line, " \t"); tok && argc < 8; tok = strtok(nullptr, " \t")) argv[argc++] = tok;
  if (argc == 0) return;
  const char* cmd = argv[0];
  if (strcmp(cmd, "tap") == 0 && argc == 3) {
    freenove_uitest::startGesture(atoi(argv[1]), atoi(argv[2]), atoi(argv[1]), atoi(argv[2]),
                                  freenove_uitest::kTapMs);
    s_reportDone = true;
    s_doneName = "tap";
    reply("OK tap");
  } else if (strcmp(cmd, "swipe") == 0 && argc == 6) {
    int ms = atoi(argv[5]);
    if (ms < 20) ms = 20;
    freenove_uitest::startGesture(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), (uint32_t)ms);
    s_reportDone = true;
    s_doneName = "swipe";
    reply("OK swipe");
  } else if (strcmp(cmd, "shot") == 0) {
    // "shot" = every row; "shot A B" = rows A..B (a host re-asking for
    // rows it lost).
#if HMI_CYD
    if (argc == 3) cmdShot(atoi(argv[1]), atoi(argv[2]));
    else cmdShot(0, kShotH - 1);
#else
    if (argc == 3) cmdShot(atoi(argv[1]), atoi(argv[2]));
    else cmdShot(0, freenove::kLandscapeH - 1);
#endif
  } else if (strcmp(cmd, "page") == 0 && argc == 2) {
    freenove_uitest::requestPage(atoi(argv[1]));
    reply("OK page");
  } else if (strcmp(cmd, "status") == 0) {
    cmdStatus();
  } else if (strcmp(cmd, "log") == 0) {
    cmdLog();
  } else if (strcmp(cmd, "wake") == 0) {
    display.relayActivity();
    reply("OK wake");
  } else if (strcmp(cmd, "contacts") == 0) {
    cmdContacts();
  } else if (strcmp(cmd, "relays") == 0) {
    cmdRelays();
#if HMI_CYD
  } else if (strcmp(cmd, "stats") == 0) {
    cmdStats();
  } else if (strcmp(cmd, "stress") == 0 && argc == 2) {
    freenove_uitest::s_stressUntil.store(millis() + (uint32_t)atoi(argv[1]));
    reply("OK stress");
  } else if (strcmp(cmd, "idle") == 0 && argc == 2) {
    freenove_uitest::s_minIdle.store(atoi(argv[1]));
    reply("OK idle");
  } else if (strcmp(cmd, "bench") == 0) {
    cmdBench();
  } else if (strcmp(cmd, "bars") == 0) {
    display.showColourBars();
    reply("OK bars");
#endif
  } else if (strcmp(cmd, "display") == 0) {
    // The screen power profiles (display_prefs) and the one-time migration flag.
    const mesh::ui::DisplayPowerPrefs& d = mesh::ui::displayPowerPrefs();
    Preferences p;
    bool mig = false;
    unsigned from = 0, fromMode = 0;
    if (p.begin("hdisp", true)) {
      mig = p.getBool("mig1", false);
      from = p.getUShort("from", 0);
      fromMode = p.getUChar("frommode", 0);
      p.end();
    }
    reply("DISPLAY battery=%s/%u usb=%s/%u migrated=%d (was usb %s/%u) on=%d",
          mesh::ui::displayModeName(d.battery.mode), (unsigned)d.battery.seconds, mesh::ui::displayModeName(d.usb.mode),
          (unsigned)d.usb.seconds, mig ? 1 : 0, mesh::ui::displayModeName((mesh::ui::DisplayMode)fromMode), from,
          display.isOn() ? 1 : 0);
  } else if (strcmp(cmd, "ping") == 0) {
    reply("OK ping");
  } else {
    reply("ERR unknown command (tap, swipe, shot, page, status, log, wake, contacts, relays, inject, ping)");
  }
}

}  // namespace

// Companion loop (main.cpp loop()).
void freenoveUiTestLoop() {
#if HMI_CYD
  {
    const uint32_t us = (uint32_t)esp_timer_get_time();
    if (freenove_uitest::s_loopLastUs != 0) {
      const uint32_t gap = us - freenove_uitest::s_loopLastUs;
      if (gap > freenove_uitest::s_loopMaxGapUs.load()) freenove_uitest::s_loopMaxGapUs.store(gap);
    }
    freenove_uitest::s_loopLastUs = us;
    freenove_uitest::s_loopCount.fetch_add(1);
  }
#endif
  static bool started = false;
  if (!started) {
    started = true;
#if HMI_CYD
    // CH340 UART: a faster console (shots are ~200 KB).
    Serial.flush();
    Serial.begin(UITEST_BAUD);
#else
    Serial.setTxTimeoutMs(1000);
    Serial.begin(115200);
#endif
    reply("UITEST ready (tap, swipe, shot, page, status, log, wake, ping)");
  }
  if (s_reportDone && !freenove_uitest::gestureBusy()) {
    s_reportDone = false;
    reply("DONE %s", s_doneName);
  }
  int budget = 64;
  while (budget-- > 0 && Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\r' || c == '\n') {
      if (s_lineLen > 0) {
        s_line[s_lineLen] = 0;
        s_lineLen = 0;
        run(s_line);
      }
    } else if (s_lineLen < sizeof(s_line) - 1) {
      s_line[s_lineLen++] = (char)c;
    }
  }
}

#endif
