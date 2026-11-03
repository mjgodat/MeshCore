#pragma once

// Link settings of a visual head built with HEAD_UDP_TRANSPORT: how this
// head reaches its repeater (ESP-NOW, UDP over WiFi, both, or auto) and the
// UDP details, entered with the touch keyboard. Every change runs the same
// text command as the USB terminal (`set transport ...`, `set udp.* ...`,
// helpers/HeadTransport.h), so the rules (channel check, limits, masking)
// are the same. Opened from the companion carousel's "Link" page.
//
// The WiFi password and the secret are typed masked, never shown (only
// "set"), and never prefilled. A refusal (for example both/auto while the
// access point is on another channel) is shown in full until a tap.

#if defined(HEAD_UDP_TRANSPORT) && HEAD_UDP_TRANSPORT

#include <helpers/ui/UIScreen.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/esp32/HeadLinkRadio.h>
#include "TouchKeyboard.h"

class HeadLinkScreen : public UIScreen {
 public:
  using CloseFn = void (*)(void* ctx);
  HeadLinkScreen(CloseFn on_close, void* ctx) : _on_close(on_close), _ctx(ctx) {}

  void open() { _mode = Mode::List; _scroll = 0; _touch_down = false; _notice[0] = 0; }

  // Test consoles: never the password or the secret.
  void describe(char* out, size_t size) const {
    snprintf(out, size, "mode=%s", _mode == Mode::List ? "list" : _mode == Mode::Entry ? "entry" : "notice");
  }

  int render(DisplayDriver& d) override {
    _w = d.width();
    _h = d.height();
    d.setTextSize(1);
    renderHeader(d);
    switch (_mode) {
      case Mode::List: renderList(d); break;
      case Mode::Entry: renderEntry(d); break;
      case Mode::Notice: renderNotice(d); break;
    }
    return 1000;   // the header shows live link state
  }

  bool handleInput(char c) override {
    if (c == KEY_CANCEL) { back(); return true; }
    return true;
  }

  bool handleTouch(bool touched, int x, int y, int width) {
    (void)width;
    if (!touched) { _touch_down = false; return false; }
    if (_touch_down) return false;
    _touch_down = true;
    switch (_mode) {
      case Mode::List: return touchList(x, y);
      case Mode::Entry: return touchEntry(x, y);
      case Mode::Notice: _mode = Mode::List; return true;
    }
    return false;
  }

 private:
  enum class Mode : uint8_t { List, Entry, Notice };
  enum class Item : uint8_t { Transport, Ssid, Password, Repeater, Secret, Port, Failover, Status, Count };
  static constexpr int kHeaderH = 18;
  static constexpr int kTextH = 14;
  static constexpr int kKeyH = 22;
#ifndef UI_HEAD_LIST_ROWS
#define UI_HEAD_LIST_ROWS 7
#endif
  static constexpr int kRows = UI_HEAD_LIST_ROWS;

  void renderHeader(DisplayDriver& d) {
    mesh::headlink::Status st;
    HeadLinkRadio::instance()->status(st);
    const auto& c = HeadLinkRadio::instance()->config();
    char line[48];
    if (c.mode == mesh::headlink::Mode::EspNow) snprintf(line, sizeof(line), "Link: ESP-NOW ch %u", (unsigned)st.espnow_channel);
    else if (st.udp == mesh::headlink::LinkState::WrongChannel)
      snprintf(line, sizeof(line), "WiFi ch %u, ESP-NOW ch %u!", (unsigned)st.ap_channel, (unsigned)st.espnow_channel);
    else snprintf(line, sizeof(line), "Link: %s, UDP %s", mesh::headlink::activeName(c, st),
                  mesh::headlink::linkStateName(st.udp));
    const bool warn = st.udp == mesh::headlink::LinkState::WrongChannel;
    d.setColor(warn ? UIColor::warning_txt : UIColor::title_bkg);
    d.fillRect(0, 0, d.width(), kHeaderH - 1);
    d.setColor(warn ? UIColor::window_bkg : UIColor::title_txt);
    d.drawTextEllipsized(3, (kHeaderH - 8) / 2, d.width() - 6, line);
  }

  void label(Item item, char* out, size_t cap) const {
    const auto& c = HeadLinkRadio::instance()->config();
    char peer[24];
    switch (item) {
      case Item::Transport: snprintf(out, cap, "Link: %s", mesh::headlink::modeName(c.mode)); break;
      case Item::Ssid: snprintf(out, cap, "WiFi: %s", c.ssid[0] ? c.ssid : "-"); break;
      case Item::Password: snprintf(out, cap, "WiFi password: %s", c.password[0] ? "set" : "-"); break;
      case Item::Repeater:
        mesh::bridge::udp::formatPeer(c.repeater, peer, sizeof(peer));
        snprintf(out, cap, "Repeater: %s", peer);
        break;
      case Item::Secret: snprintf(out, cap, "Secret: %s", c.secret[0] ? "set" : "-"); break;
      case Item::Port: snprintf(out, cap, "Head UDP port: %u", (unsigned)c.local_port); break;
      case Item::Failover: snprintf(out, cap, "Auto fail-over: %u s", (unsigned)c.failover_s); break;
      default: snprintf(out, cap, "Status"); break;
    }
  }

  int rowH() const { return (_h - kHeaderH) / (kRows + 1); }

  void renderList(DisplayDriver& d) {
    const int row_h = rowH();
    const int count = (int)Item::Count;
    for (int r = 0; r < kRows && _scroll + r < count; r++) {
      char text[48];
      label((Item)(_scroll + r), text, sizeof(text));
      drawButton(d, 0, kHeaderH + r * row_h, d.width(), row_h, text, false);
    }
    const int y = kHeaderH + kRows * row_h;
    const int w = d.width() / 3;
    drawButton(d, 0, y, w, row_h, "Up", false);
    drawButton(d, w, y, w, row_h, "Down", false);
    drawButton(d, 2 * w, y, d.width() - 2 * w, row_h, "Back", false);
  }

  bool touchList(int x, int y) {
    const int row_h = rowH();
    const int r = (y - kHeaderH) / row_h;
    const int count = (int)Item::Count;
    if (y < kHeaderH) { showStatus(); return true; }
    if (r == kRows) {
      const int col = x / (_w / 3);
      if (col == 0 && _scroll > 0) _scroll--;
      else if (col == 1 && _scroll + kRows < count) _scroll++;
      else if (col >= 2) back();
      return true;
    }
    const int idx = _scroll + r;
    if (r < 0 || idx >= count) return false;
    const auto& c = HeadLinkRadio::instance()->config();
    _item = (Item)idx;
    switch (_item) {
      case Item::Transport: {
        const uint8_t next = (uint8_t)(((uint8_t)c.mode + 1) % 4);
        char cmd[40];
        snprintf(cmd, sizeof(cmd), "set transport %s", mesh::headlink::modeName((mesh::headlink::Mode)next));
        run(cmd);
        return true;
      }
      case Item::Status: showStatus(); return true;
      case Item::Ssid: openEntry(c.ssid, mesh::headlink::kSsidMax, false); return true;
      case Item::Password: openEntry("", mesh::headlink::kPasswordMax, true); return true;
      case Item::Secret: openEntry("", mesh::headlink::kSecretMax, true); return true;
      case Item::Repeater: {
        char peer[24] = "";
        if (c.repeater.enabled()) mesh::bridge::udp::formatPeer(c.repeater, peer, sizeof(peer));
        openEntry(peer, 21, false);
        return true;
      }
      case Item::Port: {
        char v[8]; snprintf(v, sizeof(v), "%u", (unsigned)c.local_port);
        openEntry(v, 5, false);
        return true;
      }
      case Item::Failover: {
        char v[8]; snprintf(v, sizeof(v), "%u", (unsigned)c.failover_s);
        openEntry(v, 3, false);
        return true;
      }
      default: return false;
    }
  }

  void openEntry(const char* initial, size_t max_len, bool masked) {
    _keyboard.reset(initial, max_len, masked);
    _mode = Mode::Entry;
  }

  const char* key() const {
    switch (_item) {
      case Item::Ssid: return "udp.ssid";
      case Item::Password: return "udp.password";
      case Item::Repeater: return "udp.peer";
      case Item::Secret: return "udp.secret";
      case Item::Port: return "udp.port";
      default: return "transport.failover";
    }
  }

  void renderEntry(DisplayDriver& d) {
    const int top = kHeaderH;
    char title[48];
    snprintf(title, sizeof(title), "%s:", key());
    d.setColor(UIColor::secondary_txt);
    d.setCursor(2, top + 2);
    d.print(title);
    d.setColor(UIColor::primary_txt);
    _keyboard.drawText(d, top + 12);
    d.setColor(UIColor::secondary_txt);
    d.drawRect(0, top + 10, d.width(), kTextH - 2);
    const int kb_top = top + 10 + kTextH - 2;
    _keyboard.draw(d, kb_top, TouchKeyboard::keyHeight(kb_top, d.height(), kKeyH - 1), "OK");
  }

  bool touchEntry(int x, int y) {
    switch (_keyboard.touch(x, y)) {
      case TouchKeyboard::Result::Changed: return true;
      case TouchKeyboard::Result::Cancel: wipeEntry(); _mode = Mode::List; return true;
      case TouchKeyboard::Result::Submit: {
        if (_keyboard.length() == 0) return true;
        char cmd[TouchKeyboard::kCapacity + 32];
        snprintf(cmd, sizeof(cmd), "set %s %s", key(), _keyboard.text());
        wipeEntry();
        run(cmd);
        memset(cmd, 0, sizeof(cmd));
        return true;
      }
      case TouchKeyboard::Result::None: break;
    }
    return false;
  }

  void wipeEntry() { _keyboard.reset("", TouchKeyboard::kCapacity - 1, true); }

  // Runs a terminal command; an error stays on screen until a tap.
  void run(const char* cmd) {
    char reply[160] = "";
    HeadLinkRadio::handleCommand(cmd, reply, sizeof(reply));
    if (strncmp(reply, "OK", 2) == 0) { _mode = Mode::List; return; }
    snprintf(_notice, sizeof(_notice), "%s", reply);
    _mode = Mode::Notice;
  }

  void showStatus() {
    HeadLinkRadio::handleCommand("get transport", _notice, sizeof(_notice));
    _mode = Mode::Notice;
  }

  // Word-wrapped notice text (refusals, the full `get transport` line).
  void renderNotice(DisplayDriver& d) {
    const int cols = d.width() / 6 > 4 ? d.width() / 6 : 4;
    const char* p = _notice[0] == '>' ? _notice + 2 : _notice;
    int y = kHeaderH + 3;
    d.setColor(strncmp(_notice, "Error", 5) == 0 ? UIColor::warning_txt : UIColor::primary_txt);
    while (*p && y + 8 <= d.height() - 10) {
      int n = (int)strlen(p);
      if (n > cols) {
        n = cols;
        while (n > 0 && p[n] != ' ') n--;
        if (n == 0) n = cols;
      }
      char line[64];
      if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
      memcpy(line, p, (size_t)n); line[n] = 0;
      d.setCursor(2, y);
      d.print(line);
      p += n;
      while (*p == ' ') p++;
      y += 10;
    }
    d.setColor(UIColor::secondary_txt);
    d.drawTextCentered(d.width() / 2, d.height() - 9, "tap to go back");
  }

  void back() {
    if (_mode != Mode::List) { wipeEntry(); _mode = Mode::List; return; }
    if (_on_close) _on_close(_ctx);
  }

  void drawButton(DisplayDriver& d, int x, int y, int w, int h, const char* text, bool on) const {
    d.setColor(on ? UIColor::corp_blue : UIColor::title_bkg);
    d.fillRect(x + 1, y + 1, w - 2, h - 2);
    d.setColor(UIColor::secondary_txt);
    d.drawRect(x, y, w, h);
    d.setColor(UIColor::title_txt);
    d.drawTextEllipsized(x + 3, y + (h - 8) / 2, w - 6, text);
  }

  CloseFn _on_close;
  void* _ctx;
  Mode _mode = Mode::List;
  Item _item = Item::Transport;
  int _scroll = 0;
  int _w = 160, _h = 120;
  bool _touch_down = false;
  char _notice[160] = {};
  TouchKeyboard _keyboard;
};

#endif
