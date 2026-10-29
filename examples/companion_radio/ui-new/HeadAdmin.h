#pragma once

// Repeater admin console for a repeater head (UI_REPEATER_HEAD). Drives the
// companion text terminal (`to`, `login`, `cmd`) and shows its output, so the
// screen behaves exactly like a USB terminal session:
//   - header: target repeater (tap to cycle) and login state (tap to log in)
//   - console: tail of the terminal output
//   - buttons: ver / neighbors / clock / radio, settings, free command,
//     advert, back
// Settings reads a value with `cmd get <key>` and writes it with
// `cmd set <key> <value>`. Any `set` that can cut this head off from the
// repeater (bridge/espnow/wifi) or move its LoRa radio (radio/freq), from
// Settings or typed under Cmd, first shows a warning screen with the old
// value and how to recover; nothing is sent until Apply.
//
// The admin password is entered once per repeater and kept in this head's
// NVS (namespace "hadmin"); "Forget password" in Settings removes it.

#include <helpers/ui/UIScreen.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/AdvertDataHelpers.h>
#include <Preferences.h>
#include "../MyMesh.h"
#include "../HeadTerminalLog.h"
#include "TouchKeyboard.h"

extern MyMesh the_mesh;

class HeadAdminScreen : public UIScreen {
 public:
  using CloseFn = void (*)(void* ctx);

  HeadAdminScreen(CloseFn on_close, void* ctx) : _on_close(on_close), _ctx(ctx) {}

  void open() {
    _mode = Mode::Console;
    _status = nullptr;
    _touch_down = false;
    loadRepeaters();
  }

  int render(DisplayDriver& d) override {
    _w = d.width();
    _h = d.height();
    d.setTextSize(1);
    char buf[48];
    snprintf(buf, sizeof(buf), "Rptr:%s", _repeater_count ? _repeaters[_repeater_idx].name : "(none heard)");
    drawButton(d, 0, 0, kSplitX, kHeaderH, buf, false);
    drawButton(d, kSplitX, 0, d.width() - kSplitX, kHeaderH, loginLabel(), _login == Login::Ok);

    switch (_mode) {
      case Mode::Console: renderConsole(d); break;
      case Mode::Settings: renderSettings(d); break;
      case Mode::Password:
      case Mode::Command:
      case Mode::SetValue: renderEntry(d); break;
      case Mode::Confirm: renderConfirm(d); break;
    }
    return 500;
  }

  void poll() override {
    // Track login outcome and capture `cmd get` values as replies arrive.
    const uint32_t gen = headTerminalLog().generation();
    if (gen == _seen_generation) return;
    _seen_generation = gen;
    char tail[256];
    headTerminalLog().copyTail(tail, sizeof(tail));
    if (_login == Login::Pending) {
      if (strstr(tail, "accepted")) _login = Login::Ok;
      else if (strstr(tail, "rejected") || strstr(tail, "login to")) _login = Login::Failed;
    }
    if (_mode == Mode::SetValue && _awaiting_value && _keyboard.length() == 0) {
      char value[64];
      if (lastReplyValue(tail, value, sizeof(value))) {
        _keyboard.reset(value, TouchKeyboard::kCapacity - 1);
        snprintf(_old_value, sizeof(_old_value), "%s", value);
        _awaiting_value = false;
      }
    }
  }

  bool handleInput(char c) override {
    if (c == KEY_CANCEL) { back(); return true; }
    return true;
  }

  bool handleTouch(bool touched, int x, int y, int width) {
    if (!touched) { _touch_down = false; return false; }
    if (_touch_down) return false;
    _touch_down = true;
    _status = nullptr;
    if (y < kHeaderH) {
      if (x < kSplitX) {
        if (_repeater_count) {
          _repeater_idx = (_repeater_idx + 1) % _repeater_count;
          _login = Login::None;
        }
      } else {
        startLogin(false);
      }
      return true;
    }
    switch (_mode) {
      case Mode::Console: return touchConsole(x, y, width);
      case Mode::Settings: return touchSettings(x, y);
      case Mode::Confirm: return touchConfirm(x, y);
      default: return touchEntry(x, y, width);
    }
  }

 private:
  enum class Mode : uint8_t { Console, Settings, Password, Command, SetValue, Confirm };
  enum class Login : uint8_t { None, Pending, Ok, Failed };
  struct Repeater { uint8_t pub_prefix[4]; char name[32]; };
  struct Setting { const char* key; };

  static constexpr int kHeaderH = 18;
  static constexpr int kSplitX = 96;
  static constexpr int kButtonH = 21;
  static constexpr int kTextH = 14;
  static constexpr int kKeyH = 22;
  static constexpr int kMaxRepeaters = 8;
  static constexpr int kSettingRows = 7;

  static const Setting* settings(int& count) {
    static const Setting list[] = {
        {"name"}, {"radio"}, {"tx"}, {"advert.interval"}, {"flood.advert.interval"},
        {"flood.max"}, {"repeat"}, {"guest.password"}, {"bridge.enabled"},
        {"bridge.format"}, {"bridge.source"}, {"bridge.channel"},
        {nullptr},  // "Forget password"
    };
    count = sizeof(list) / sizeof(list[0]);
    return list;
  }

  // ---- data -------------------------------------------------------------

  void loadRepeaters() {
    _repeater_count = 0;
    const int n = the_mesh.getNumContacts();
    for (int i = 0; i < n && _repeater_count < kMaxRepeaters; i++) {
      ContactInfo c;
      if (!the_mesh.getContactByIdx(MAX_ANON_CONTACTS + i, c) || c.type != ADV_TYPE_REPEATER) continue;
      Repeater& r = _repeaters[_repeater_count++];
      memcpy(r.pub_prefix, c.id.pub_key, sizeof(r.pub_prefix));
      strncpy(r.name, c.name, sizeof(r.name) - 1);
      r.name[sizeof(r.name) - 1] = 0;
    }
    if (_repeater_idx >= _repeater_count) _repeater_idx = 0;
  }

  void prefKey(char* out, size_t size) const {
    const uint8_t* p = _repeaters[_repeater_idx].pub_prefix;
    snprintf(out, size, "pw%02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
  }

  bool loadPassword(char* out, size_t size) const {
    char key[16];
    prefKey(key, sizeof(key));
    Preferences prefs;
    if (!prefs.begin("hadmin", true)) return false;
    const size_t n = prefs.getString(key, out, size);
    prefs.end();
    return n > 1;
  }

  void savePassword(const char* pw) const {
    char key[16];
    prefKey(key, sizeof(key));
    Preferences prefs;
    if (!prefs.begin("hadmin", false)) return;
    prefs.putString(key, pw);
    prefs.end();
  }

  void forgetPassword() const {
    char key[16];
    prefKey(key, sizeof(key));
    Preferences prefs;
    if (!prefs.begin("hadmin", false)) return;
    prefs.remove(key);
    prefs.end();
  }

  // ---- terminal ---------------------------------------------------------

  bool selectRepeater() {
    if (_repeater_count == 0) { _status = "No repeater heard yet"; return false; }
    const uint8_t* p = _repeaters[_repeater_idx].pub_prefix;
    char line[24];
    snprintf(line, sizeof(line), "to %02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
    the_mesh.headTerminal(line);
    return true;
  }

  void run(const char* line, const char* echo = nullptr) {
    if (!selectRepeater()) return;
    headTerminalLog().print("\n> ");
    headTerminalLog().print(echo ? echo : line);
    headTerminalLog().print("\n");
    the_mesh.headTerminal(line);
  }

  void remote(const char* command) {
    char line[TouchKeyboard::kCapacity + 8];
    snprintf(line, sizeof(line), "cmd %s", command);
    run(line);
  }

  void startLogin(bool from_entry) {
    char pw[32];
    if (!from_entry && !loadPassword(pw, sizeof(pw))) {
      _mode = Mode::Password;
      _keyboard.reset("", 15, true);
      return;
    }
    if (from_entry) {
      strncpy(pw, _keyboard.text(), sizeof(pw) - 1);
      pw[sizeof(pw) - 1] = 0;
      savePassword(pw);
      _mode = Mode::Console;
    }
    char line[48];
    snprintf(line, sizeof(line), "login %s", pw);
    _login = Login::Pending;
    headTerminalLog().clear();  // so only this login's result is matched
    run(line, "login ****");
  }

  // The repeater's reply is the first non-empty line after the terminal's
  // "CLI -> from <name>" header, without the "> " / "-> " markers.
  static bool lastReplyValue(const char* tail, char* out, size_t size) {
    const char* hdr = strstr(tail, "CLI -> from");
    if (!hdr) return false;
    const char* p = strchr(hdr, '\n');
    while (p) {
      p++;
      const char* line_end = strchr(p, '\n');
      if (!line_end) line_end = p + strlen(p);
      const char* s = p;
      while (s < line_end && (*s == ' ' || *s == '>' || *s == '-')) s++;
      if (s < line_end) {
        size_t n = line_end - s;
        if (n >= size) n = size - 1;
        memcpy(out, s, n);
        out[n] = 0;
        return true;
      }
      p = *line_end ? line_end : nullptr;
    }
    return false;
  }

  // ---- console ----------------------------------------------------------

  const char* loginLabel() const {
    switch (_login) {
      case Login::Ok: return "Logged in";
      case Login::Pending: return "Logging in";
      case Login::Failed: return "Login failed";
      default: return "Login";
    }
  }

  void renderConsole(DisplayDriver& d) {
    const int top = kHeaderH + 1;
    const int bottom = d.height() - 2 * kButtonH;
    const int line_h = 10;
    const int max_lines = (bottom - top) / line_h;
    char tail[400];
    headTerminalLog().copyTail(tail, sizeof(tail));
    // Wrap into display lines, keeping the last max_lines.
    const int cols = d.width() / 6;
    const char* lines[24];
    int lens[24];
    int count = 0;
    const char* p = tail;
    while (*p) {
      const char* nl = strchr(p, '\n');
      int len = nl ? (int)(nl - p) : (int)strlen(p);
      const bool prompt_only = len <= 2 && (len == 0 || p[0] == '>');
      while (!prompt_only && len > 0) {
        const int take = len < cols ? len : cols;
        if (count == 24) { memmove(lines, lines + 1, 23 * sizeof(lines[0])); memmove(lens, lens + 1, 23 * sizeof(lens[0])); count--; }
        lines[count] = p; lens[count] = take; count++;
        p += take; len -= take;
      }
      p = nl ? nl + 1 : p + len;
      if (!nl) break;
    }
    const int first = count > max_lines ? count - max_lines : 0;
    d.setColor(UIColor::primary_txt);
    char line[40];
    for (int i = first; i < count; i++) {
      const int n = lens[i] < (int)sizeof(line) - 1 ? lens[i] : (int)sizeof(line) - 1;
      memcpy(line, lines[i], n);
      line[n] = 0;
      d.setCursor(1, top + (i - first) * line_h);
      d.print(line);
    }
    if (_status) {
      d.setColor(UIColor::warning_txt);
      d.setCursor(1, bottom - line_h);
      d.print(_status);
    }
    static const char* labels[8] = {"Ver", "Nbrs", "Clock", "Radio", "Settings", "Cmd", "Advert", "Back"};
    const int w = d.width() / 4;
    for (int i = 0; i < 8; i++) {
      drawButton(d, (i % 4) * w, bottom + (i / 4) * kButtonH, w, kButtonH, labels[i], false);
    }
  }

  bool touchConsole(int x, int y, int width) {
    const int bottom = _h - 2 * kButtonH;
    if (y < bottom) return false;
    const int idx = ((y - bottom) / kButtonH) * 4 + x / (width / 4);
    switch (idx) {
      case 0: remote("ver"); break;
      case 1: remote("neighbors"); break;
      case 2: remote("clock"); break;
      case 3: remote("get radio"); break;
      case 4: _mode = Mode::Settings; _settings_scroll = 0; break;
      case 5: _mode = Mode::Command; _keyboard.reset(""); _old_value[0] = 0; break;
      case 6: remote("advert"); break;
      case 7: back(); break;
      default: return false;
    }
    return true;
  }

  // ---- settings ---------------------------------------------------------

  void renderSettings(DisplayDriver& d) {
    int count;
    const Setting* list = settings(count);
    const int top = kHeaderH;
    const int row_h = (d.height() - top) / (kSettingRows + 1);
    for (int r = 0; r < kSettingRows && _settings_scroll + r < count; r++) {
      const Setting& s = list[_settings_scroll + r];
      drawButton(d, 0, top + r * row_h, d.width(), row_h,
                 s.key ? s.key : "Forget password", false);
    }
    const int y = top + kSettingRows * row_h;
    const int w = d.width() / 3;
    drawButton(d, 0, y, w, row_h, "Up", false);
    drawButton(d, w, y, w, row_h, "Down", false);
    drawButton(d, 2 * w, y, d.width() - 2 * w, row_h, "Back", false);
  }

  bool touchSettings(int x, int y) {
    int count;
    const Setting* list = settings(count);
    const int row_h = (_h - kHeaderH) / (kSettingRows + 1);
    const int r = (y - kHeaderH) / row_h;
    if (r == kSettingRows) {
      const int col = x / (_w / 3);
      if (col == 0 && _settings_scroll > 0) _settings_scroll--;
      else if (col == 1 && _settings_scroll + kSettingRows < count) _settings_scroll++;
      else if (col >= 2) _mode = Mode::Console;
      return true;
    }
    const int idx = _settings_scroll + r;
    if (r < 0 || idx >= count) return false;
    if (!list[idx].key) {
      forgetPassword();
      _login = Login::None;
      _status = "Password forgotten";
      _mode = Mode::Console;
      return true;
    }
    _setting = &list[idx];
    _old_value[0] = 0;
    _mode = Mode::SetValue;
    _keyboard.reset("");
    _awaiting_value = true;
    char line[48];
    snprintf(line, sizeof(line), "get %s", _setting->key);
    headTerminalLog().clear();  // so only this reply is read back
    remote(line);
    return true;
  }

  // ---- text entry (password, command, setting value) --------------------

  void renderEntry(DisplayDriver& d) {
    const int top = kHeaderH;
    char title[48];
    if (_mode == Mode::Password) snprintf(title, sizeof(title), "Admin password:");
    else if (_mode == Mode::Command) snprintf(title, sizeof(title), "Command for repeater:");
    else snprintf(title, sizeof(title), "set %s%s", _setting->key, _awaiting_value ? " (reading)" : "");
    d.setColor(_status ? UIColor::warning_txt : UIColor::secondary_txt);
    d.setCursor(2, top + 2);
    d.print(_status ? _status : title);
    d.setColor(UIColor::primary_txt);
    _keyboard.drawText(d, top + 12);
    d.setColor(UIColor::secondary_txt);
    d.drawRect(0, top + 10, d.width(), kTextH - 2);
    _keyboard.draw(d, top + 10 + kTextH - 2, kKeyH - 1, _mode == Mode::Password ? "Login" : "OK");
  }

  bool touchEntry(int x, int y, int width) {
    switch (_keyboard.touch(x, y, width, kHeaderH + 10 + kTextH - 2, kKeyH - 1)) {
      case TouchKeyboard::Result::Changed: return true;
      case TouchKeyboard::Result::Cancel: _mode = _mode == Mode::SetValue ? Mode::Settings : Mode::Console; return true;
      case TouchKeyboard::Result::Submit: submitEntry(); return true;
      case TouchKeyboard::Result::None: break;
    }
    return false;
  }

  void submitEntry() {
    if (_keyboard.length() == 0) return;
    if (_mode == Mode::Password) {
      startLogin(true);
      return;
    }
    if (_mode == Mode::Command) {
      snprintf(_pending, sizeof(_pending), "%s", _keyboard.text());
    } else {
      snprintf(_pending, sizeof(_pending), "set %s %s", _setting->key, _keyboard.text());
    }
    _pending_from = _mode;
    const Hazard hazard = hazardOf(_pending);
    if (hazard == Hazard::None) {
      remote(_pending);
      _mode = Mode::Console;
    } else {
      _hazard = hazard;
      _mode = Mode::Confirm;
    }
  }

  // ---- disclaimer for changes that can cut off the head or the mesh -----

  enum class Hazard : uint8_t { None, HeadLink, LoRa };

  // `set` commands that change the ESP-NOW link this head depends on, or the
  // repeater's LoRa radio. Matches both the Settings list and typed commands.
  static Hazard hazardOf(const char* command) {
    while (*command == ' ') command++;
    if (strncmp(command, "set ", 4) != 0) return Hazard::None;
    const char* key = command + 4;
    while (*key == ' ') key++;
    if (strncmp(key, "bridge.", 7) == 0 || strncmp(key, "espnow", 6) == 0
        || strncmp(key, "wifi", 4) == 0) return Hazard::HeadLink;
    if (strncmp(key, "radio", 5) == 0 || strncmp(key, "freq", 4) == 0) return Hazard::LoRa;
    return Hazard::None;
  }

  void renderConfirm(DisplayDriver& d) {
    const int top = kHeaderH + 2;
    d.setColor(UIColor::warning_txt);
    d.setCursor(2, top);
    d.print("WARNING - read before applying");
    char text[320];
    if (_hazard == Hazard::HeadLink) {
      snprintf(text, sizeof(text),
               "\"%s\" changes the repeater's ESP-NOW link. If it does not "
               "match this head, the head is cut off from the repeater and "
               "cannot undo it. Recovery: connect the repeater by USB and "
               "set it back%s%s.",
               _pending, _old_value[0] ? " to: " : "", _old_value);
    } else {
      snprintf(text, sizeof(text),
               "\"%s\" changes the repeater's LoRa radio. Phones, nodes and "
               "repeaters on the old settings lose it. This head stays "
               "connected over ESP-NOW and can set it back%s%s.",
               _pending, _old_value[0] ? " to: " : "", _old_value);
    }
    d.setColor(UIColor::primary_txt);
    drawWrapped(d, text, top + 11, d.height() - kButtonH - 2);
    const int y = d.height() - kButtonH;
    drawButton(d, 0, y, d.width() / 2, kButtonH, "Cancel", false);
    drawButton(d, d.width() / 2, y, d.width() - d.width() / 2, kButtonH, "Apply", true);
  }

  bool touchConfirm(int x, int y) {
    if (y < _h - kButtonH) return false;
    if (x >= _w / 2) {
      remote(_pending);
      _mode = Mode::Console;
    } else {
      _mode = _pending_from;  // back to editing, nothing sent
    }
    return true;
  }

  static void drawWrapped(DisplayDriver& d, const char* text, int y, int bottom) {
    const int cols = d.width() / 6 - 1;
    const int line_h = 10;
    char line[40];
    while (*text && y + line_h <= bottom) {
      int n = (int)strlen(text);
      if (n > cols) {
        n = cols;
        while (n > 0 && text[n] != ' ') n--;
        if (n == 0) n = cols;
      }
      const int copy = n < (int)sizeof(line) - 1 ? n : (int)sizeof(line) - 1;
      memcpy(line, text, copy);
      line[copy] = 0;
      d.setCursor(2, y);
      d.print(line);
      text += n;
      while (*text == ' ') text++;
      y += line_h;
    }
  }

  // ---- shared -----------------------------------------------------------

  void drawButton(DisplayDriver& d, int x, int y, int w, int h, const char* label, bool on) const {
    d.setColor(on ? UIColor::corp_blue : UIColor::title_bkg);
    d.fillRect(x + 1, y + 1, w - 2, h - 2);
    d.setColor(UIColor::secondary_txt);
    d.drawRect(x, y, w, h);
    d.setColor(UIColor::title_txt);
    d.drawTextEllipsized(x + 3, y + (h - 8) / 2, w - 6, label);
  }

  void back() {
    if (_mode == Mode::Console) {
      if (_on_close) _on_close(_ctx);
    } else {
      _mode = Mode::Console;
    }
  }

  CloseFn _on_close;
  void* _ctx;
  Mode _mode = Mode::Console;
  Login _login = Login::None;
  TouchKeyboard _keyboard;
  const Setting* _setting = nullptr;
  char _pending[TouchKeyboard::kCapacity + 8] = {};
  char _old_value[48] = {};
  Mode _pending_from = Mode::Console;
  Hazard _hazard = Hazard::None;
  bool _awaiting_value = false;
  bool _touch_down = false;
  const char* _status = nullptr;
  uint32_t _seen_generation = 0;
  int _settings_scroll = 0;
  Repeater _repeaters[kMaxRepeaters];
  int _repeater_count = 0;
  int _repeater_idx = 0;
  int _w = 160;
  int _h = 120;
};
