#pragma once

// On-screen QWERTY keyboard for touch heads (UI_REPEATER_HEAD). Four rows in
// logical display coordinates: two character rows; the mode key, a third
// character row and delete; space, cancel (X) and the action key ("Send",
// "OK", "Login").
//
// Two layouts, chosen per screen at build time (TOUCH_KEYBOARD_PC_LAYOUT):
//
// Mode key (default; the CYD head): one mode key (left of the third row,
// labelled with the current mode, lit when not lower case) cycles abc ->
// ABC -> 123 -> abc. ABC types capitals until cycled on (no one-shot
// shift). The "123" page is the phone layout in the 26 letter slots:
// 1234567890 / -/:;()$&@ / .,?!'"#. Not typeable on screen:
// % ^ * + = _ ~ { } [ ] < > \ | and the backtick. Bottom row: space, X,
// action.
//
// PC (TOUCH_KEYBOARD_PC_LAYOUT=1; the Freenove head): a shift key (abc /
// ABC, one capital then back) left of the third row; the bottom row is
// [123 / #+=][space][X][action]. "123" follows a PC keyboard: digits, their
// shifted symbols !@#$%^&*(), then -_=+.,?; "#+=" holds ~`{}[]<>\| ;:'"/
// and -_=+.,?; on those pages the shift slot is "abc" (back to letters).
//
// Every key that can be touched is drawn: draw() records the geometry it
// drew with (only rows that fit on the screen are drawn) and touch(x, y)
// hit-tests against exactly that. keyHeight() sizes the rows so all four
// fit below `top` (the 2.8" CYD's HEAD page has 105 logical rows; larger
// screens keep the hosts' usual key height).

#include <helpers/ui/DisplayDriver.h>
#include <string.h>

#ifndef TOUCH_KEYBOARD_PC_LAYOUT
#define TOUCH_KEYBOARD_PC_LAYOUT 0
#endif

class TouchKeyboard {
 public:
  enum class Result : uint8_t { None, Changed, Submit, Cancel };

  static constexpr int kRows = 4;
  static constexpr size_t kCapacity = 168;

  // Row height for a keyboard whose first row starts at `top` on a screen
  // `height` rows tall: max_h, or less so that all kRows rows fit.
  static int keyHeight(int top, int height, int max_h) {
    const int fit = (height - top) / kRows;
    return fit < max_h ? fit : max_h;
  }

  void reset(const char* initial = "", size_t max_len = kCapacity - 1, bool masked = false) {
    _max_len = max_len < kCapacity - 1 ? max_len : kCapacity - 1;
    if (initial == nullptr) initial = "";
    _len = strlen(initial);
    if (_len > _max_len) _len = _max_len;
    memcpy(_text, initial, _len);
    _text[_len] = 0;
    _mode = LOWER;
    _shift = false;
    _masked = masked;
  }

  const char* text() const { return _text; }
  size_t length() const { return _len; }
  void setMaxLength(size_t max_len) { _max_len = max_len < kCapacity - 1 ? max_len : kCapacity - 1; }
  size_t maxLength() const { return _max_len; }

  // Text line showing the tail (masked for passwords) with a cursor.
  void drawText(DisplayDriver& d, int y) const {
    char shown[kCapacity + 1];
    if (_masked) {
      memset(shown, '*', _len);
      shown[_len] = 0;
    } else {
      strcpy(shown, _text);
    }
    const char* tail = shown;
    while (*tail && d.getTextWidth(tail) > d.width() - 8) tail++;
    d.setCursor(2, y);
    d.print(tail);
    d.print("_");
  }

  void draw(DisplayDriver& d, int top, int key_h, const char* action_label) {
    _drawnTop = top;
    _drawnKeyH = key_h;
    _drawnWidth = d.width();
    _drawnRows = 0;
    for (int r = 0; r < kRows; r++) {
      if (key_h <= 0 || top + (r + 1) * key_h > d.height()) break;   // never a key off the screen
      Key keys[kMaxKeysPerRow];
      const int count = rowKeys(r, d.width(), action_label, keys);
      for (int k = 0; k < count; k++) drawKey(d, keys[k], top + r * key_h, key_h);
      _drawnRows = r + 1;
    }
  }

  // The current page: "abc", "ABC" or "123" (mode key); "abc", "ABC"
  // (shift on), "123" or "#+=" (PC).
  const char* modeLabel() const {
#if TOUCH_KEYBOARD_PC_LAYOUT
    if (_mode == LOWER) return _shift ? "ABC" : "abc";
    return _mode == SYM1 ? "123" : "#+=";
#else
    static const char* const kLabels[3] = {"abc", "ABC", "123"};
    return kLabels[_mode];
#endif
  }

  // Rows the last draw() put on the screen (kRows when they all fit).
  int drawnRows() const { return _drawnRows; }

  // Hit test against what the last draw() drew: rows that were not drawn
  // (or nothing drawn yet) are not touchable.
  Result touch(int x, int y) {
    if (_drawnRows <= 0 || y < _drawnTop || _drawnKeyH <= 0) return Result::None;
    const int r = (y - _drawnTop) / _drawnKeyH;
    if (r < 0 || r >= _drawnRows) return Result::None;
    Key keys[kMaxKeysPerRow];
    const int count = rowKeys(r, _drawnWidth, "", keys);
    for (int k = 0; k < count; k++) {
      if (x >= keys[k].x && x < keys[k].x + keys[k].w) return press(keys[k]);
    }
    return Result::None;
  }

 private:
  static constexpr int kMaxKeysPerRow = 11;
  enum KeyKind : uint8_t { CHAR, MODE, DEL, SPACE, ACTION, CANCEL, SHIFT, SYMBOLS_KEY, LETTERS_KEY };
#if TOUCH_KEYBOARD_PC_LAYOUT
  // LOWER = the letters page (with _shift), SYM1 = 123, SYM2 = #+=.
  enum Mode : uint8_t { LOWER, SYM1, SYM2 };
#else
  enum Mode : uint8_t { LOWER, UPPER, SYMBOLS, kModes };
#endif
  struct Key { int x, w; KeyKind kind; char ch; const char* label; };

  int rowKeys(int r, int width, const char* action_label, Key* out) const {
    static const char* letters[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
#if TOUCH_KEYBOARD_PC_LAYOUT
    static const char* sym1[3] = {"1234567890", "!@#$%^&*()", "-_=+.,?"};
    static const char* sym2[3] = {"~`{}[]<>\\|", ";:'\"/", "-_=+.,?"};
    const char* const* rows = _mode == LOWER ? letters : (_mode == SYM1 ? sym1 : sym2);
#else
    static const char* symbols[3] = {"1234567890", "-/:;()$&@", ".,?!'\"#"};
    const char* const* rows = _mode == SYMBOLS ? symbols : letters;
#endif
    const int u = width / 10;
    int n = 0;
    if (r == 0 || r == 1) {
      const int len = strlen(rows[r]);
      const int x0 = (width - len * u) / 2;
      for (int i = 0; i < len; i++) out[n++] = {x0 + i * u, u, CHAR, rows[r][i], nullptr};
    } else if (r == 2) {
      const int len = strlen(rows[2]);
      const int side = (width - len * u) / 2;
#if TOUCH_KEYBOARD_PC_LAYOUT
      if (_mode == LOWER) out[n++] = {0, side, SHIFT, 0, _shift ? "ABC" : "abc"};
      else out[n++] = {0, side, LETTERS_KEY, 0, "abc"};
#else
      out[n++] = {0, side, MODE, 0, modeLabel()};
#endif
      for (int i = 0; i < len; i++) out[n++] = {side + i * u, u, CHAR, rows[2][i], nullptr};
      out[n++] = {side + len * u, width - side - len * u, DEL, 0, "<-"};
    } else {
#if TOUCH_KEYBOARD_PC_LAYOUT
      out[n++] = {0, 2 * u, SYMBOLS_KEY, 0, _mode == SYM1 ? "#+=" : "123"};
      out[n++] = {2 * u, 4 * u, SPACE, ' ', "space"};
      out[n++] = {6 * u, u + u / 2, CANCEL, 0, "X"};
      out[n++] = {7 * u + u / 2, width - 7 * u - u / 2, ACTION, 0, action_label};
#else
      out[n++] = {0, 5 * u, SPACE, ' ', "space"};
      out[n++] = {5 * u, 2 * u, CANCEL, 0, "X"};
      out[n++] = {7 * u, width - 7 * u, ACTION, 0, action_label};
#endif
    }
    return n;
  }

  void drawKey(DisplayDriver& d, const Key& k, int y, int key_h) const {
    // The action key, and (mode key layout) the mode key when not lower
    // case, are lit.
    const bool lit = k.kind == ACTION || (k.kind == MODE && _mode != LOWER);
    d.setColor(lit ? UIColor::corp_blue : UIColor::secondary_txt);
    d.drawRect(k.x + 1, y + 1, k.w - 2, key_h - 2);
    d.setColor(UIColor::primary_txt);
    char cap[2] = {k.ch, 0};
    if (k.kind == CHAR && capitals() && cap[0] >= 'a' && cap[0] <= 'z') cap[0] -= 32;
    d.drawTextCentered(k.x + k.w / 2, y + key_h / 2 - 4, k.label ? k.label : cap);
  }

  Result press(const Key& k) {
    switch (k.kind) {
      case CHAR: {
        char c = k.ch;
        if (capitals() && c >= 'a' && c <= 'z') {
          c -= 32;
          _shift = false;   // PC: one capital (the mode key's ABC stays)
        }
        append(c);
        return Result::Changed;
      }
      case SPACE: append(' '); return Result::Changed;
#if TOUCH_KEYBOARD_PC_LAYOUT
      case SHIFT: _shift = !_shift; return Result::Changed;
      case SYMBOLS_KEY: _mode = _mode == SYM1 ? SYM2 : SYM1; return Result::Changed;
      case LETTERS_KEY: _mode = LOWER; return Result::Changed;
      case MODE: return Result::None;
#else
      case MODE: _mode = (Mode)((_mode + 1) % kModes); return Result::Changed;
      case SHIFT:
      case SYMBOLS_KEY:
      case LETTERS_KEY: return Result::None;
#endif
      case DEL: if (_len) _text[--_len] = 0; return Result::Changed;
      case CANCEL: return Result::Cancel;
      case ACTION: return Result::Submit;
    }
    return Result::None;
  }

  // Letters type as capitals: ABC (mode key) or shift on (PC).
  bool capitals() const {
#if TOUCH_KEYBOARD_PC_LAYOUT
    return _mode == LOWER && _shift;
#else
    return _mode == UPPER;
#endif
  }

  void append(char c) {
    if (_len >= _max_len) return;
    _text[_len++] = c;
    _text[_len] = 0;
  }

  char _text[kCapacity] = {};
  size_t _len = 0;
  size_t _max_len = kCapacity - 1;
  Mode _mode = LOWER;
  bool _shift = false;   // PC layout: the next letter is a capital
  bool _masked = false;
  int _drawnTop = 0, _drawnKeyH = 0, _drawnWidth = 0, _drawnRows = 0;
};
