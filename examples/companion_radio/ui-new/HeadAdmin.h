#pragma once

// Repeater admin console for a repeater head (UI_REPEATER_HEAD). Drives the
// companion text terminal (`to`, `login`, `cmd`) and shows its output, so the
// screen behaves exactly like a USB terminal session:
//   - header: target repeater (tap for the repeater list) and login state
//     (tap to log in)
//   - console: tail of the terminal output
//   - buttons: ver / neighbors / clock / radio, settings, free command,
//     advert, presets, back
// Settings reads a value with `cmd get <key>` and writes it with
// `cmd set <key> <value>`. Any `set` that can cut this head off from the
// repeater (bridge/espnow/wifi) or move its LoRa radio (radio/freq), and a
// typed `load preset`, first shows a warning screen with the old value and
// how to recover; nothing is sent until Apply.
//
// Presets (logged in only) lists the repeater's radio presets
// (`cmd get preset`) and loads one with `cmd load preset <n>`. A remote load
// reverts after 10 minutes unless a fresh admin login confirms it, so the
// screen after a load offers Confirm, which re-sends `login` once the radio
// switch has settled and then re-reads the list.
//
// Repeater list: the head's home repeater (the one heard at 0 hops over the
// bridge, MyMesh::headHomeRepeater()) first, then the others most recently
// heard first; built on demand from the contact table, scrolled a page at a
// time.
//
// Login always opens the password entry, prefilled (masked) with the saved
// password if there is one, so it can be kept, edited or retyped. The
// password is saved in this head's NVS (namespace "hadmin") under the
// repeater it was sent to, and only once that repeater accepted it; a failed
// login re-opens the entry. "Forget password" in Settings removes it.
// "Clear heard contacts" (Settings, with a confirm) drops heard repeaters,
// rooms and sensors, keeping the home repeater, favourites and chats.

#include <helpers/ui/UIScreen.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/AdvertDataHelpers.h>
#include <Preferences.h>
#include "../MyMesh.h"
#include "../HeadTerminalLog.h"
#include "../HeadContactPolicy.h"
#include "TouchKeyboard.h"
#include "HeadPresets.h"

extern MyMesh the_mesh;

class HeadAdminScreen : public UIScreen {
 public:
  using CloseFn = void (*)(void* ctx);

  HeadAdminScreen(CloseFn on_close, void* ctx) : _on_close(on_close), _ctx(ctx) {}

  void open() {
    _mode = Mode::Console;
    _status = nullptr;
    _touch_down = false;
    _presets_awaiting = false;
    _preset_step = PresetStep::Idle;
    _commit_detached = false;
    _preset_msg[0] = 0;
    _presets_after_login = false;
    loadRepeaters();
  }

  bool loggedIn() const { return _login == Login::Ok; }

  // For the USB test console (uitest builds) and diagnostics: never the
  // password.
  void describe(char* out, size_t size) const {
    snprintf(out, size, "mode=%s login=%s repeater=%s repeaters=%d",
             modeName(), loginLabel(), _has_sel ? _sel.name : "(none)", _pick_count);
  }

  int render(DisplayDriver& d) override {
    _w = d.width();
    _h = d.height();
    d.setTextSize(1);
    char buf[48];
    snprintf(buf, sizeof(buf), "Rptr:%s", _has_sel ? _sel.name : "(none heard)");
    drawButton(d, 0, 0, kSplitX, kHeaderH, buf, false);
    drawButton(d, kSplitX, 0, d.width() - kSplitX, kHeaderH, loginLabel(), _login == Login::Ok);

    switch (_mode) {
      case Mode::Console: renderConsole(d); break;
      case Mode::Settings: renderSettings(d); break;
      case Mode::Password:
      case Mode::Command:
      case Mode::SetValue: renderEntry(d); break;
      case Mode::Confirm: renderConfirm(d); break;
      case Mode::Presets: renderPresets(d); break;
      case Mode::PresetAsk: renderPresetAsk(d); break;
      case Mode::PresetCommit: renderPresetCommit(d); break;
      case Mode::Pick: renderPick(d); break;
      case Mode::ClearAsk: renderClearAsk(d); break;
    }
    return 500;
  }

  void poll() override {
    pollPresetTimers();
    // Track login outcome and capture `cmd get` values as replies arrive.
    const uint32_t gen = headTerminalLog().generation();
    if (gen == _seen_generation) return;
    _seen_generation = gen;
    char tail[400];  // a full `get preset` reply plus its header
    headTerminalLog().copyTail(tail, sizeof(tail));
    if (_login == Login::Pending) {
      const head_policy::LoginReply reply = head_policy::classifyLoginReply(tail);
      if (reply == head_policy::LoginReply::Accepted) {
        _login = Login::Ok;
        // Saved only now, and under the repeater the login went to.
        if (_login_pw[0]) savePasswordFor(_login_prefix, _login_pw);
      } else if (reply == head_policy::LoginReply::Failed) {
        _login = Login::Failed;
        // A typed password that failed: back to the entry to fix it.
        if (_login_from_entry && _mode == Mode::Console) {
          _mode = Mode::Password;
          _keyboard.reset(_login_pw, kPasswordMax, true);
          _login_note = true;
          // The companion refuses a new login for 30 s after one timed out.
          _login_wait = strstr(tail, "quarantined") != nullptr || strstr(tail, "timed out") != nullptr;
        }
      }
      if (_login != Login::Pending) {
        wipe(_login_pw, sizeof(_login_pw));
        if (_presets_after_login) {
          _presets_after_login = false;
          if (_login == Login::Ok) openPresets();
        }
      }
    }
    if (_mode == Mode::SetValue && _awaiting_value && _keyboard.length() == 0) {
      char value[64];
      if (lastReplyValue(tail, value, sizeof(value))) {
        _keyboard.reset(value, TouchKeyboard::kCapacity - 1);
        snprintf(_old_value, sizeof(_old_value), "%s", value);
        _awaiting_value = false;
      }
    }
    pollPresetReplies(tail);
  }

  bool handleInput(char c) override {
    if (c == KEY_CANCEL) { back(); return true; }
    return true;
  }

  // The Freenove HOME page's ADMIN tile (after open()): as the header's
  // Login button, the password entry prefilled with the saved password.
  void loginSaved() {
    if (_login == Login::Ok) return;
    if (!presetBusy()) openLoginEntry();
  }

  // The preset list (Freenove HOME's PRESET tile, after open()): at once
  // when logged in, else after a saved-password login succeeds (presets
  // need admin rights).
  void openPresetsScreen() {
    if (_login == Login::Ok) { openPresets(); return; }
    if (presetBusy()) return;
    openLoginEntry();
    _presets_after_login = _mode == Mode::Password;  // cleared on Cancel
  }

  // The settings list scrolled to the bridge keys (HOME's OBSERVER and
  // BRIDGE tiles, after open()); set changes keep their warning screen.
  void openSettingsScreen() {
    _mode = Mode::Settings;
    int count;
    const Setting* list = settings(count);
    _settings_scroll = 0;
    for (int i = 0; i < count; i++) {
      if (list[i].key && strcmp(list[i].key, "bridge.enabled") == 0) {
        _settings_scroll = i + kSettingRows > count ? (count > kSettingRows ? count - kSettingRows : 0) : i;
        break;
      }
    }
  }

  bool handleTouch(bool touched, int x, int y, int width) {
    if (!touched) { _touch_down = false; return false; }
    if (_touch_down) return false;
    _touch_down = true;
    _status = nullptr;
    if (y < kHeaderH) {
      if (x < kSplitX) {
        if (!presetBusy()) openPick();
      } else if (!presetBusy()) {  // a login clears the log and would lose the reply
        openLoginEntry();
      }
      return true;
    }
    switch (_mode) {
      case Mode::Console: return touchConsole(x, y, width);
      case Mode::Settings: return touchSettings(x, y);
      case Mode::Confirm: return touchConfirm(x, y);
      case Mode::Presets: return touchPresets(x, y);
      case Mode::PresetAsk: return touchPresetAsk(x, y);
      case Mode::PresetCommit: return touchPresetCommit(x, y);
      case Mode::Pick: return touchPick(x, y);
      case Mode::ClearAsk: return touchClearAsk(x, y);
      default: return touchEntry(x, y, width);
    }
  }

 private:
  enum class Mode : uint8_t {
    Console, Settings, Password, Command, SetValue, Confirm, Presets, PresetAsk, PresetCommit,
    Pick, ClearAsk
  };
  // Load/confirm progress for a remote `load preset`.
  enum class PresetStep : uint8_t { Idle, AwaitLoad, Loaded, CommitWait, CommitLogin };
  enum class Login : uint8_t { None, Pending, Ok, Failed };
  struct Repeater { uint8_t pub_prefix[4]; char name[32]; };
  // key: a repeater setting (`get`/`set`); without one, a local action.
  enum class Action : uint8_t { Remote, ForgetPassword, ClearHeard };
  struct Setting { const char* key; Action action; };

  static constexpr int kHeaderH = 18;
  static constexpr int kSplitX = 96;
  static constexpr int kButtonH = 21;
  static constexpr int kTextH = 14;
  static constexpr int kKeyH = 22;
  static constexpr int kMaxPick = MAX_CONTACTS < 255 ? MAX_CONTACTS : 255;  // the whole table
  static constexpr size_t kPasswordMax = 15;  // the terminal's limit
  // Rows of the settings list and the repeater picker (plus a button row).
  // The CYD visual head's HEAD page (105 logical rows) shows 5, so each row
  // stays a finger-sized 28 px.
#ifndef UI_HEAD_LIST_ROWS
#define UI_HEAD_LIST_ROWS 7
#endif
  static constexpr int kSettingRows = UI_HEAD_LIST_ROWS;
  static constexpr int kConsoleCols = 3;
  static constexpr int kConsoleRows = 3;
  static constexpr int kPresetSlots = 5;
  static constexpr int kPresetsButton = 7;  // index in the console grid
  static constexpr uint32_t kRevertMillis = 10UL * 60UL * 1000UL;  // repeater's revert timer
  static constexpr uint32_t kSwitchSettleMillis = 3000;  // radio switches ~2 s after the reply
  static constexpr uint32_t kReplyTimeoutMillis = 30000;

  static const Setting* settings(int& count) {
    static const Setting list[] = {
        {"name", Action::Remote}, {"radio", Action::Remote}, {"tx", Action::Remote},
        {"advert.interval", Action::Remote}, {"flood.advert.interval", Action::Remote},
        {"flood.max", Action::Remote}, {"repeat", Action::Remote}, {"guest.password", Action::Remote},
        {"bridge.enabled", Action::Remote}, {"bridge.format", Action::Remote},
        {"bridge.source", Action::Remote}, {"bridge.channel", Action::Remote},
        {nullptr, Action::ForgetPassword}, {nullptr, Action::ClearHeard},
    };
    count = sizeof(list) / sizeof(list[0]);
    return list;
  }

  // ---- data -------------------------------------------------------------

  // Every repeater in the contact table (indexes into it): home first,
  // then most recently heard. The selection stays if it is still there,
  // else the first entry (the home repeater once it has been heard).
  void loadRepeaters() {
    _pick_count = 0;
    uint32_t heard[kMaxPick];
    bool home[kMaxPick];
    const int n = the_mesh.getNumContacts();
    for (int i = 0; i < n && _pick_count < kMaxPick; i++) {
      ContactInfo c;
      if (!the_mesh.getContactByIdx(MAX_ANON_CONTACTS + i, c) || c.type != ADV_TYPE_REPEATER) continue;
      heard[_pick_count] = c.lastmod;
      home[_pick_count] = the_mesh.isHeadHomeRepeater(c);
      _pick[_pick_count++] = (uint16_t)(MAX_ANON_CONTACTS + i);
    }
    // Sort positions 0..count-1 by their keys, then map to contact indexes.
    uint16_t order[kMaxPick];
    for (int i = 0; i < _pick_count; i++) order[i] = (uint16_t)i;
    head_policy::sortRepeaters(order, _pick_count, [&](uint16_t k) {
      return head_policy::RepeaterKey{home[k], heard[k]};
    });
    uint16_t sorted[kMaxPick];
    for (int i = 0; i < _pick_count; i++) sorted[i] = _pick[order[i]];
    memcpy(_pick, sorted, sizeof(_pick[0]) * _pick_count);
    _pick_revision = the_mesh.getContactTableRevision();

    if (_has_sel && pickIndexOf(_sel.pub_prefix) >= 0) return;
    _has_sel = false;
    ContactInfo c;
    if (_pick_count > 0 && the_mesh.getContactByIdx(_pick[0], c)) select(c);
  }

  int pickIndexOf(const uint8_t* prefix) const {
    for (int i = 0; i < _pick_count; i++) {
      ContactInfo c;
      if (the_mesh.getContactByIdx(_pick[i], c) && memcmp(c.id.pub_key, prefix, 4) == 0) return i;
    }
    return -1;
  }

  // A new target: login state and presets belong to the previous one.
  void select(const ContactInfo& c) {
    const bool changed = !_has_sel || memcmp(_sel.pub_prefix, c.id.pub_key, sizeof(_sel.pub_prefix)) != 0;
    memcpy(_sel.pub_prefix, c.id.pub_key, sizeof(_sel.pub_prefix));
    head_policy::displayName(c.name, c.id.pub_key, c.type, _sel.name, sizeof(_sel.name));
    _has_sel = true;
    if (!changed) return;
    _login = Login::None;
    _presets_valid = false;
    _presets_awaiting = false;
    _preset_step = PresetStep::Idle;
    _commit_detached = false;
    if (isPresetMode()) _mode = Mode::Console;
  }

  static void prefKeyFor(const uint8_t* p, char* out, size_t size) {
    snprintf(out, size, "pw%02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
  }

  void prefKey(char* out, size_t size) const { prefKeyFor(_sel.pub_prefix, out, size); }

  static void wipe(char* buf, size_t size) {
    volatile char* p = buf;
    for (size_t i = 0; i < size; i++) p[i] = 0;
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

  static void savePasswordFor(const uint8_t* prefix, const char* pw) {
    char key[16];
    prefKeyFor(prefix, key, sizeof(key));
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
    if (!_has_sel) { _status = "No repeater heard yet"; return false; }
    const uint8_t* p = _sel.pub_prefix;
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

  // Header Login / HOME's ADMIN: the password entry, prefilled (masked)
  // with the saved password for the selected repeater, if any.
  void openLoginEntry() {
    if (!_has_sel) { _status = "No repeater heard yet"; return; }
    char pw[32] = {};
    loadPassword(pw, sizeof(pw));
    _mode = Mode::Password;
    _keyboard.reset(pw, kPasswordMax, true);
    _login_note = false;
    wipe(pw, sizeof(pw));
  }

  // Preset commit: the saved password, no entry. False when none is saved.
  bool loginWithSaved() {
    char pw[32];
    if (!loadPassword(pw, sizeof(pw))) return false;
    sendLogin(pw, false);
    wipe(pw, sizeof(pw));
    return true;
  }

  // from_entry: typed (or confirmed) in the entry; it is saved once the
  // repeater accepts it, and a failure re-opens the entry.
  void sendLogin(const char* pw, bool from_entry) {
    memcpy(_login_prefix, _sel.pub_prefix, sizeof(_login_prefix));
    _login_from_entry = from_entry;
    if (from_entry) snprintf(_login_pw, sizeof(_login_pw), "%s", pw);
    else _login_pw[0] = 0;
    char line[48];
    snprintf(line, sizeof(line), "login %s", pw);
    _login = Login::Pending;
    headTerminalLog().clear();  // so only this login's result is matched
    run(line, "login ****");
    wipe(line, sizeof(line));
  }

  // The repeater's reply is the first non-empty line after the terminal's
  // last "CLI -> from <name>" header, without the "> " / "-> " markers.
  static bool lastReplyValue(const char* tail, char* out, size_t size) {
    const char* hdr = nullptr;
    for (const char* h = strstr(tail, "CLI -> from"); h; h = strstr(h + 1, "CLI -> from")) hdr = h;
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

  const char* modeName() const {
    static const char* names[] = {"console", "settings", "password", "command", "setvalue", "confirm",
                                  "presets", "presetask", "presetcommit", "pick", "clearask"};
    return names[(int)_mode];
  }

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
    const int bottom = d.height() - kConsoleRows * kButtonH;
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
        const int take = fitChars(d, p, len < cols ? len : cols);
        if (count == 24) { memmove(lines, lines + 1, 23 * sizeof(lines[0])); memmove(lens, lens + 1, 23 * sizeof(lens[0])); count--; }
        lines[count] = p; lens[count] = take; count++;
        p += take; len -= take;
      }
      p = nl ? nl + 1 : p + len;
      if (!nl) break;
    }
    const int first = count > max_lines ? count - max_lines : 0;
    d.setColor(UIColor::primary_txt);
    char line[64];  // > cols: a full wrapped line (40 chars) plus NUL
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
    // 3x3 grid. "Settings" sits in the last column, which gets the
    // remainder pixel(s) of the width, so its label fits unclipped.
    static const char* labels[kConsoleCols * kConsoleRows] = {
        "Ver", "Nbrs", "Clock", "Radio", "Cmd", "Settings", "Advert", "Presets", "Back"};
    for (int i = 0; i < kConsoleCols * kConsoleRows; i++) {
      const int col = i % kConsoleCols;
      const int x0 = col * d.width() / kConsoleCols;
      const int x1 = (col + 1) * d.width() / kConsoleCols;
      const bool enabled = i != kPresetsButton || _login == Login::Ok;
      drawButton(d, x0, bottom + (i / kConsoleCols) * kButtonH, x1 - x0, kButtonH, labels[i], false, enabled);
    }
  }

  // How many of the first n chars fit the width (fonts wider than 6 px
  // per char, e.g. the Freenove's, would otherwise clip the line's end).
  static int fitChars(DisplayDriver& d, const char* p, int n) {
    char buf[64];
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    while (n > 1) {
      memcpy(buf, p, n);
      buf[n] = 0;
      if (d.getTextWidth(buf) <= d.width() - 6) break;  // margin: AA glyph overhang
      n--;
    }
    return n;
  }

  bool touchConsole(int x, int y, int width) {
    const int bottom = _h - kConsoleRows * kButtonH;
    if (y < bottom || width <= 0) return false;
    int col = x * kConsoleCols / width;
    if (col >= kConsoleCols) col = kConsoleCols - 1;
    const int idx = ((y - bottom) / kButtonH) * kConsoleCols + col;
    switch (idx) {
      case 0: remote("ver"); break;
      case 1: remote("neighbors"); break;
      case 2: remote("clock"); break;
      case 3: remote("get radio"); break;
      case 4: _mode = Mode::Command; _keyboard.reset(""); _old_value[0] = 0; break;
      case 5: _mode = Mode::Settings; _settings_scroll = 0; break;
      case 6: remote("advert"); break;
      case kPresetsButton:
        if (_login != Login::Ok) { _status = "Log in first"; break; }
        openPresets();
        break;
      case 8: back(); break;
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
      const char* label = s.key ? s.key
          : s.action == Action::ForgetPassword ? "Forget password" : "Clear heard contacts";
      drawButton(d, 0, top + r * row_h, d.width(), row_h, label, false);
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
    if (list[idx].action == Action::ForgetPassword) {
      forgetPassword();
      _login = Login::None;
      _status = "Password forgotten";
      _mode = Mode::Console;
      return true;
    }
    if (list[idx].action == Action::ClearHeard) {
      _clear_count = the_mesh.headClearHeardContacts(true);
      _mode = Mode::ClearAsk;
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
    if (_mode == Mode::Password) {
      snprintf(title, sizeof(title), "%s",
               !_login_note ? "Admin password:"
               : _login_wait ? "Login failed (retry in 30 s) - password:" : "Login failed - password:");
    }
    else if (_mode == Mode::Command) snprintf(title, sizeof(title), "Command for repeater:");
    else snprintf(title, sizeof(title), "set %s%s", _setting->key, _awaiting_value ? " (reading)" : "");
    d.setColor(_status ? UIColor::warning_txt : UIColor::secondary_txt);
    d.setCursor(2, top + 2);
    d.print(_status ? _status : title);
    d.setColor(UIColor::primary_txt);
    _keyboard.drawText(d, top + 12);
    d.setColor(UIColor::secondary_txt);
    d.drawRect(0, top + 10, d.width(), kTextH - 2);
    const int kb_top = top + 10 + kTextH - 2;
    _keyboard.draw(d, kb_top, TouchKeyboard::keyHeight(kb_top, d.height(), kKeyH - 1),
                   _mode == Mode::Password ? "Login" : "OK");
  }

  bool touchEntry(int x, int y, int width) {
    (void)width;
    switch (_keyboard.touch(x, y)) {   // the keys as last drawn
      case TouchKeyboard::Result::Changed: return true;
      case TouchKeyboard::Result::Cancel:
        if (_mode == Mode::Password) _presets_after_login = false;
        _mode = _mode == Mode::SetValue ? Mode::Settings : Mode::Console;
        return true;
      case TouchKeyboard::Result::Submit: submitEntry(); return true;
      case TouchKeyboard::Result::None: break;
    }
    return false;
  }

  void submitEntry() {
    if (_keyboard.length() == 0) return;
    if (_mode == Mode::Password) {
      _mode = Mode::Console;
      sendLogin(_keyboard.text(), true);
      _keyboard.reset("", kPasswordMax, true);  // no copy left in the entry
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

  enum class Hazard : uint8_t { None, HeadLink, LoRa, Preset };

  // `set` commands that change the ESP-NOW link this head depends on, or the
  // repeater's LoRa radio, and a typed `load preset`. Matches both the
  // Settings list and typed commands.
  static Hazard hazardOf(const char* command) {
    if (isLoadPresetCommand(command)) return Hazard::Preset;
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
    } else if (_hazard == Hazard::Preset) {
      snprintf(text, sizeof(text),
               "\"%s\" moves the repeater's LoRa radio to that preset; nodes "
               "on the old settings lose it. It reverts in 10 min unless you "
               "confirm under Presets.",
               _pending);
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
      if (_hazard == Hazard::Preset) _load_sent_ms = millis();  // Presets > Confirm waits for the switch
      _mode = Mode::Console;
    } else {
      _mode = _pending_from;  // back to editing, nothing sent
    }
    return true;
  }

  // Returns the y below the last line drawn.
  static int drawWrapped(DisplayDriver& d, const char* text, int y, int bottom) {
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
    return y;
  }

  // ---- presets ----------------------------------------------------------
  //
  // Presets: list from `get preset`, one row per slot (active highlighted,
  // empty greyed). PresetAsk: confirm a load. PresetCommit: after a load,
  // Confirm re-sends `login` (the repeater commits a remote load on a fresh
  // admin login) and shows an approximate revert countdown.

  bool isPresetMode() const {
    return _mode == Mode::Presets || _mode == Mode::PresetAsk || _mode == Mode::PresetCommit;
  }

  const char* repeaterName() const { return _has_sel ? _sel.name : "?"; }

  const char* presetName(int i) const {
    return i >= 0 && i < kPresetSlots && _presets.slot[i].name[0] ? _presets.slot[i].name : "preset";
  }

  void setPresetMsg(const char* text) { snprintf(_preset_msg, sizeof(_preset_msg), "%s", text); }

  void openPresets() {
    _mode = Mode::Presets;
    _preset_msg[0] = 0;
    refreshPresets();
  }

  void refreshPresets() {
    _presets_awaiting = true;
    _presets_sent_ms = millis();
    headTerminalLog().clear();  // so only this reply is parsed
    remote("get preset");
  }

  void sendPresetLoad() {
    char line[24];
    snprintf(line, sizeof(line), "load preset %d", _preset_sel + 1);
    _preset_msg[0] = 0;
    _presets_awaiting = false;  // a list reply now would be mixed up with this one
    headTerminalLog().clear();
    remote(line);
    _load_sent_ms = millis();
    _revert_deadline_ms = _load_sent_ms + kRevertMillis;
    _preset_step = PresetStep::AwaitLoad;
    _mode = Mode::PresetCommit;
  }

  // The list says a load is pending (from this head or anyone): open the
  // Confirm screen, keeping the local countdown if this head started it.
  void openPresetCommitFromList() {
    _preset_sel = headPresetActiveSlot(_presets);
    if (_preset_step == PresetStep::Idle || _preset_step == PresetStep::AwaitLoad) {
      _revert_deadline_ms = millis() + (uint32_t)_presets.pending_minutes * 60000UL;
      _preset_step = PresetStep::Loaded;
    }
    _commit_detached = false;
    _preset_msg[0] = 0;
    _mode = Mode::PresetCommit;
  }

  void startPresetCommit() {
    char pw[32];
    const bool saved = loadPassword(pw, sizeof(pw));
    wipe(pw, sizeof(pw));
    if (!saved) {
      setPresetMsg("No saved password - tap Login");
      return;
    }
    _preset_step = PresetStep::CommitWait;  // pollPresetTimers() logs in once the switch settled
  }

  void presetBack() {
    if (_mode == Mode::PresetAsk) {
      _mode = Mode::Presets;
    } else if (_mode == Mode::PresetCommit) {
      _mode = Mode::Presets;
      if (_preset_step == PresetStep::AwaitLoad) {
        _preset_step = PresetStep::Idle;  // a late reply is ignored; the list tells
        setPresetMsg("Tap Refresh to check the load");
      } else if (_preset_step == PresetStep::CommitWait) {
        _preset_step = PresetStep::Loaded;  // cancelled before the login went out
        refreshPresets();
      } else if (_preset_step == PresetStep::CommitLogin) {
        _commit_detached = true;  // login already sent: finish silently
      } else if (_preset_step == PresetStep::Loaded) {
        refreshPresets();
      }
    } else {
      _mode = Mode::Console;
    }
  }

  // A preset request or commit is in flight: its reply must not be lost to
  // a log clear (login, refresh) or mixed up with another request's.
  bool presetBusy() const {
    return _presets_awaiting || _preset_step == PresetStep::AwaitLoad
        || _preset_step == PresetStep::CommitWait || _preset_step == PresetStep::CommitLogin;
  }

  // Copies the terminal's own "ERROR: ..." line (send failure, timeout).
  static bool terminalError(const char* tail, char* out, size_t size) {
    const char* e = strstr(tail, "ERROR:");
    if (!e) return false;
    const char* end = strchr(e, '\n');
    size_t n = end ? (size_t)(end - e) : strlen(e);
    if (n >= size) n = size - 1;
    memcpy(out, e, n);
    out[n] = 0;
    return true;
  }

  // Time-driven steps; runs every loop, before the log-change check.
  void pollPresetTimers() {
    const uint32_t now = millis();
    if (_presets_awaiting && now - _presets_sent_ms > kReplyTimeoutMillis) {
      _presets_awaiting = false;
      setPresetMsg("No reply - tap Refresh");
    }
    if (_preset_step == PresetStep::AwaitLoad && now - _load_sent_ms > kReplyTimeoutMillis) {
      _preset_step = PresetStep::Idle;
      setPresetMsg("No reply to load - Refresh");
      if (_mode == Mode::PresetCommit) _mode = Mode::Presets;
    }
    if (_preset_step == PresetStep::CommitWait && now - _load_sent_ms >= kSwitchSettleMillis) {
      _preset_step = PresetStep::CommitLogin;
      _commit_ms = now;
      if (!loginWithSaved()) {  // saved password: stays on this screen
        _preset_step = PresetStep::Loaded;
        setPresetMsg("No saved password - tap Login");
      }
    }
    if (_preset_step == PresetStep::CommitLogin && _login == Login::Pending
        && now - _commit_ms > kReplyTimeoutMillis) {
      _preset_step = PresetStep::Loaded;
      _commit_detached = false;
      setPresetMsg("No login reply - try again");
    }
  }

  // Reply-driven steps; `tail` is the terminal log since the last clear().
  void pollPresetReplies(const char* tail) {
    char err[sizeof(_preset_msg)];
    if (_preset_step == PresetStep::CommitLogin) {
      // poll() has already turned the login reply into _login.
      if (_login == Login::Ok) {
        _preset_step = PresetStep::Idle;
        if (_commit_detached) {
          // The user went Back while the login was out: no mode change.
          _commit_detached = false;
          if (_mode == Mode::Presets) refreshPresets();
          return;
        }
        _mode = Mode::Presets;
        _preset_msg[0] = 0;
        refreshPresets();  // shows the committed slot, no pending banner
      } else if (_login == Login::Failed) {
        _commit_detached = false;
        _preset_step = PresetStep::Loaded;
        setPresetMsg("Login failed - not confirmed");
      }
      return;
    }
    const bool cli_reply = strstr(tail, "CLI -> from") != nullptr;
    if (_preset_step == PresetStep::AwaitLoad) {
      // Only "OK - loading ..." or "Error ..." answer a load; any other
      // reply (e.g. a late `get preset`) is ignored until the timeout.
      bool failed = false;
      if (cli_reply && isLoadPresetOk(tail)) {
        _preset_step = PresetStep::Loaded;
      } else if (cli_reply) {
        failed = lastReplyValue(tail, err, sizeof(err)) && strncmp(err, "Error", 5) == 0;
      } else {
        failed = terminalError(tail, err, sizeof(err));  // the head could not send it
      }
      if (failed) {
        _preset_step = PresetStep::Idle;
        setPresetMsg(err);
        if (_mode == Mode::PresetCommit) _mode = Mode::Presets;
      }
      return;
    }
    if (!_presets_awaiting) return;
    if (cli_reply) {
      _presets_awaiting = false;
      HeadPresetList list;
      if (parseHeadPresetReply(tail, list)) {
        _presets = list;
        _presets_valid = true;
        if (headPresetListSink()) headPresetListSink()(list);
        if (!list.pending && _preset_step == PresetStep::Loaded) _preset_step = PresetStep::Idle;
      } else {
        setPresetMsg(lastReplyValue(tail, err, sizeof(err)) ? err : "Unreadable preset reply");
      }
    } else if (terminalError(tail, err, sizeof(err))) {
      _presets_awaiting = false;
      setPresetMsg(err);
    }
  }

  int presetRowsTop() const { return kHeaderH + 11; }
  int presetRowH(int h) const { return (h - kButtonH - presetRowsTop()) / kPresetSlots; }
  int presetConfirmTop() const { return kHeaderH + 35; }

  void renderPresets(DisplayDriver& d) {
    char line[48];
    const char* banner = nullptr;
    bool warn = false;
    if (_preset_msg[0]) {
      banner = _preset_msg;
      warn = true;
    } else if (_presets_awaiting) {
      banner = "Reading presets...";
    } else if (_presets_valid && _presets.pending) {
      // Tapping the banner opens Confirm.
      snprintf(line, sizeof(line), "Pending - reverts in ~%um >", (unsigned)_presets.pending_minutes);
      banner = line;
      warn = true;
    } else if (_presets_valid && _presets.names_only) {
      banner = "Names only (reply too long)";
    } else if (_presets_valid) {
      banner = "Tap a preset to load it";
    }
    if (banner) {
      d.setColor(warn ? UIColor::warning_txt : UIColor::secondary_txt);
      d.drawTextEllipsized(2, kHeaderH + 1, d.width() - 4, banner);
    }
    const int top = presetRowsTop();
    const int row_h = presetRowH(d.height());
    const int active = _presets_valid ? headPresetActiveSlot(_presets) : -1;
    char label[32];
    for (int i = 0; i < kPresetSlots; i++) {
      const HeadPresetSlot& s = _presets.slot[i];
      const bool used = _presets_valid && s.used;
      if (!_presets_valid) snprintf(label, sizeof(label), "%d ...", i + 1);
      else if (!s.used) snprintf(label, sizeof(label), "%d -", i + 1);
      else if (s.freq[0]) snprintf(label, sizeof(label), "%d %s %s", i + 1, s.name, s.freq);
      else snprintf(label, sizeof(label), "%d %s", i + 1, s.name);
      drawButton(d, 0, top + i * row_h, d.width(), row_h, label, i == active, used);
    }
    const int y = d.height() - kButtonH;
    drawButton(d, 0, y, d.width() / 2, kButtonH, "Refresh", false, !presetBusy());
    drawButton(d, d.width() / 2, y, d.width() - d.width() / 2, kButtonH, "Back", false);
  }

  bool touchPresets(int x, int y) {
    const bool had_msg = _preset_msg[0] != 0;
    _preset_msg[0] = 0;
    if (y >= _h - kButtonH) {
      if (x >= _w / 2) _mode = Mode::Console;
      else if (!presetBusy()) refreshPresets();
      return true;
    }
    const int top = presetRowsTop();
    if (y < top) {
      if (!had_msg && _presets_valid && _presets.pending) openPresetCommitFromList();
      return true;
    }
    if (presetBusy()) return had_msg;  // a reply is still due
    const int i = (y - top) / presetRowH(_h);
    if (i < 0 || i >= kPresetSlots || !_presets_valid || !_presets.slot[i].used) return had_msg;
    _preset_sel = i;
    if (_presets.slot[i].active) {
      snprintf(_preset_msg, sizeof(_preset_msg), "Already on %s", _presets.slot[i].name);
      return true;
    }
    _mode = Mode::PresetAsk;
    return true;
  }

  void renderPresetAsk(DisplayDriver& d) {
    const int bottom = d.height() - kButtonH - 2;
    char text[96];
    snprintf(text, sizeof(text), "Load %s on %s?", presetName(_preset_sel), repeaterName());
    d.setColor(UIColor::primary_txt);
    int y = drawWrapped(d, text, kHeaderH + 2, bottom);
    const HeadPresetSlot& s = _presets.slot[_preset_sel];
    if (s.freq[0]) {
      snprintf(text, sizeof(text), "%s MHz", s.freq);
      y = drawWrapped(d, text, y, bottom);
    }
    d.setColor(UIColor::warning_txt);
    drawWrapped(d, "It reverts in 10 min unless you confirm.", y + 4, bottom);
    const int by = d.height() - kButtonH;
    drawButton(d, 0, by, d.width() / 2, kButtonH, "Cancel", false);
    drawButton(d, d.width() / 2, by, d.width() - d.width() / 2, kButtonH, "Load", true);
  }

  bool touchPresetAsk(int x, int y) {
    if (y < _h - kButtonH) return false;
    if (x >= _w / 2) sendPresetLoad();
    else _mode = Mode::Presets;
    return true;
  }

  void renderPresetCommit(DisplayDriver& d) {
    const int top = kHeaderH + 2;
    char line[48];
    switch (_preset_step) {
      case PresetStep::AwaitLoad: snprintf(line, sizeof(line), "Loading %s...", presetName(_preset_sel)); break;
      case PresetStep::CommitWait: snprintf(line, sizeof(line), "Waiting for radio switch..."); break;
      case PresetStep::CommitLogin: snprintf(line, sizeof(line), "Logging in to confirm..."); break;
      default: snprintf(line, sizeof(line), "On %s, not confirmed", presetName(_preset_sel)); break;
    }
    d.setColor(UIColor::primary_txt);
    d.drawTextEllipsized(2, top, d.width() - 4, line);
    if (_preset_step != PresetStep::AwaitLoad) {
      const int32_t left = (int32_t)(_revert_deadline_ms - millis());
      if (left > 0) {
        const unsigned long secs = (unsigned long)left / 1000UL;
        snprintf(line, sizeof(line), "Reverts in ~%lu:%02lu (approx)", secs / 60UL, secs % 60UL);
      } else {
        snprintf(line, sizeof(line), "Revert time passed - go Back");
      }
      d.setColor(UIColor::warning_txt);
      d.drawTextEllipsized(2, top + 11, d.width() - 4, line);
    }
    if (_preset_msg[0]) {
      d.setColor(UIColor::warning_txt);
      d.drawTextEllipsized(2, top + 22, d.width() - 4, _preset_msg);
    }
    const bool can_confirm = _preset_step == PresetStep::Loaded;
    const int cy = presetConfirmTop();
    drawButton(d, 0, cy, d.width(), d.height() - kButtonH - 2 - cy, "Confirm", can_confirm, can_confirm);
    drawButton(d, 0, d.height() - kButtonH, d.width(), kButtonH, "Back", false);
  }

  bool touchPresetCommit(int x, int y) {
    (void)x;
    _preset_msg[0] = 0;
    if (y >= _h - kButtonH) {
      presetBack();
      return true;
    }
    if (y >= presetConfirmTop() && _preset_step == PresetStep::Loaded) startPresetCommit();
    return true;
  }

  // ---- repeater list -----------------------------------------------------
  //
  // kSettingRows rows (home marked), then Up / Down (a page) / Back.

  void openPick() {
    loadRepeaters();
    if (_pick_count == 0) { _status = "No repeater heard yet"; return; }
    const int sel = _has_sel ? pickIndexOf(_sel.pub_prefix) : 0;
    _pick_scroll = head_policy::scrollToShow(sel < 0 ? 0 : sel, 0, kSettingRows, _pick_count);
    _mode = Mode::Pick;
  }

  void renderPick(DisplayDriver& d) {
    if (_pick_revision != the_mesh.getContactTableRevision()) loadRepeaters();
    const int top = kHeaderH;
    const int row_h = (d.height() - top) / (kSettingRows + 1);
    char label[48];
    for (int r = 0; r < kSettingRows && _pick_scroll + r < _pick_count; r++) {
      ContactInfo c;
      if (!the_mesh.getContactByIdx(_pick[_pick_scroll + r], c)) continue;
      const bool home = the_mesh.isHeadHomeRepeater(c);
      char name[32];
      head_policy::displayName(c.name, c.id.pub_key, c.type, name, sizeof(name));
      snprintf(label, sizeof(label), "%s%s", home ? "HOME  " : "", name);
      const bool on = _has_sel && memcmp(c.id.pub_key, _sel.pub_prefix, sizeof(_sel.pub_prefix)) == 0;
      drawButton(d, 0, top + r * row_h, d.width(), row_h, label, on);
    }
    const int y = top + kSettingRows * row_h;
    const int w = d.width() / 3;
    drawButton(d, 0, y, w, row_h, "Up", false, _pick_scroll > 0);
    drawButton(d, w, y, w, row_h, "Down", false, _pick_scroll + kSettingRows < _pick_count);
    char back[16];
    snprintf(back, sizeof(back), "Back (%d)", _pick_count);
    drawButton(d, 2 * w, y, d.width() - 2 * w, row_h, back, false);
  }

  bool touchPick(int x, int y) {
    const int row_h = (_h - kHeaderH) / (kSettingRows + 1);
    const int r = (y - kHeaderH) / row_h;
    if (r == kSettingRows) {
      const int col = x / (_w / 3);
      if (col == 0) {
        _pick_scroll = _pick_scroll > kSettingRows ? _pick_scroll - kSettingRows : 0;
      } else if (col == 1) {
        if (_pick_scroll + kSettingRows < _pick_count) _pick_scroll += kSettingRows;
        _pick_scroll = head_policy::scrollToShow(_pick_scroll, _pick_scroll, kSettingRows, _pick_count);
      } else {
        _mode = Mode::Console;
      }
      return true;
    }
    const int idx = _pick_scroll + r;
    if (r < 0 || idx >= _pick_count) return false;
    ContactInfo c;
    if (!the_mesh.getContactByIdx(_pick[idx], c) || c.type != ADV_TYPE_REPEATER) {
      loadRepeaters();  // the table changed under the list
      return true;
    }
    select(c);
    _mode = Mode::Console;
    return true;
  }

  // ---- clear heard contacts ---------------------------------------------

  void renderClearAsk(DisplayDriver& d) {
    const int bottom = d.height() - kButtonH - 2;
    char text[200];
    snprintf(text, sizeof(text),
             "Remove %d heard repeaters, rooms and sensors from this head's contacts? "
             "Keeps the home repeater, favourites and chat contacts. Others come back "
             "only if added again.",
             _clear_count);
    d.setColor(UIColor::primary_txt);
    drawWrapped(d, text, kHeaderH + 2, bottom);
    const int by = d.height() - kButtonH;
    drawButton(d, 0, by, d.width() / 2, kButtonH, "Cancel", false);
    drawButton(d, d.width() / 2, by, d.width() - d.width() / 2, kButtonH, "Clear", true, _clear_count > 0);
  }

  bool touchClearAsk(int x, int y) {
    if (y < _h - kButtonH) return false;
    if (x >= _w / 2 && _clear_count > 0) {
      const int removed = the_mesh.headClearHeardContacts(false);
      snprintf(_note, sizeof(_note), "Removed %d contacts", removed);
      _status = _note;
      loadRepeaters();
      _mode = Mode::Console;
    } else {
      _mode = Mode::Settings;
    }
    return true;
  }

  // ---- shared -----------------------------------------------------------

  // A disabled button is drawn hollow with dim text.
  void drawButton(DisplayDriver& d, int x, int y, int w, int h, const char* label, bool on,
                  bool enabled = true) const {
    d.setColor(on ? UIColor::corp_blue : enabled ? UIColor::title_bkg : UIColor::window_bkg);
    d.fillRect(x + 1, y + 1, w - 2, h - 2);
    d.setColor(UIColor::secondary_txt);
    d.drawRect(x, y, w, h);
    d.setColor(enabled ? UIColor::title_txt : UIColor::secondary_txt);
    d.drawTextEllipsized(x + 3, y + (h - 8) / 2, w - 6, label);
  }

  void back() {
    if (_mode == Mode::Console) {
      if (_on_close) _on_close(_ctx);
    } else if (isPresetMode()) {
      presetBack();
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
  HeadPresetList _presets = {};
  bool _presets_valid = false;
  bool _presets_awaiting = false;
  uint32_t _presets_sent_ms = 0;
  int _preset_sel = 0;
  char _preset_msg[48] = {};
  PresetStep _preset_step = PresetStep::Idle;
  uint32_t _load_sent_ms = 0;
  uint32_t _revert_deadline_ms = 0;
  uint32_t _commit_ms = 0;
  bool _commit_detached = false;  // Back pressed while the commit login was out
  bool _presets_after_login = false;  // openPresetsScreen(): open the list once logged in
  Repeater _sel = {};
  bool _has_sel = false;
  uint16_t _pick[kMaxPick] = {};   // contact indexes, display order
  int _pick_count = 0;
  int _pick_scroll = 0;
  uint32_t _pick_revision = 0;
  int _clear_count = 0;
  char _note[40] = {};
  // The login in flight: target, and the typed password until the reply
  // (saved on success, offered again on failure, then wiped).
  uint8_t _login_prefix[4] = {};
  char _login_pw[kPasswordMax + 1] = {};
  bool _login_from_entry = false;
  bool _login_note = false;   // the entry was re-opened after a failed login
  bool _login_wait = false;   // ...after a timeout: the next try needs 30 s
  int _w = 160;
  int _h = 120;
};
