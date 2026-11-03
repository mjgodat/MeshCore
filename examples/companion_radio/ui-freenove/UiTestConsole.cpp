// USB test console for the Freenove head (FREENOVE_UITEST builds only; env
// Freenove_S3_35_Head_uitest). The USB CDC carries this line console instead
// of the companion protocol. Host side: .superpowers/uitest/ui.py.
//
//   tap X Y                   press + release at (X, Y) (480x320 landscape)
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
// Every command ends with one "OK <cmd>" or "ERR <why>" line; tap/swipe
// answer "DONE tap" once the gesture has been released. Never prints a
// password: the terminal log only ever holds "login ****".

#if defined(FREENOVE_UITEST) && FREENOVE_UITEST

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "UiTest.h"
#include "target.h"
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
    if (argc == 3) cmdShot(atoi(argv[1]), atoi(argv[2]));
    else cmdShot(0, freenove::kLandscapeH - 1);
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
  } else if (strcmp(cmd, "ping") == 0) {
    reply("OK ping");
  } else {
    reply("ERR unknown command (tap, swipe, shot, page, status, log, wake, contacts, relays, inject, ping)");
  }
}

}  // namespace

// Companion loop (main.cpp loop()).
void freenoveUiTestLoop() {
  static bool started = false;
  if (!started) {
    started = true;
    Serial.setTxTimeoutMs(1000);
    Serial.begin(115200);
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
