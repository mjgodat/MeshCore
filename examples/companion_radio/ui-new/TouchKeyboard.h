#pragma once

// On-screen QWERTY keyboard for touch heads (UI_REPEATER_HEAD). Four rows in
// logical display coordinates: letters (or digits/symbols), shift, delete,
// symbols toggle, space, cancel and an action key ("Send", "OK").

#include <helpers/ui/DisplayDriver.h>
#include <string.h>

class TouchKeyboard {
 public:
  enum class Result : uint8_t { None, Changed, Submit, Cancel };

  static constexpr int kRows = 4;
  static constexpr size_t kCapacity = 168;

  void reset(const char* initial = "", size_t max_len = kCapacity - 1, bool masked = false) {
    _max_len = max_len < kCapacity - 1 ? max_len : kCapacity - 1;
    if (initial == nullptr) initial = "";
    _len = strlen(initial);
    if (_len > _max_len) _len = _max_len;
    memcpy(_text, initial, _len);
    _text[_len] = 0;
    _shift = false;
    _symbols = false;
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

  void draw(DisplayDriver& d, int top, int key_h, const char* action_label) const {
    for (int r = 0; r < kRows; r++) {
      Key keys[kMaxKeysPerRow];
      const int count = rowKeys(r, d.width(), action_label, keys);
      for (int k = 0; k < count; k++) drawKey(d, keys[k], top + r * key_h, key_h);
    }
  }

  Result touch(int x, int y, int width, int top, int key_h) {
    if (y < top) return Result::None;
    const int r = (y - top) / key_h;
    if (r < 0 || r >= kRows) return Result::None;
    Key keys[kMaxKeysPerRow];
    const int count = rowKeys(r, width, "", keys);
    for (int k = 0; k < count; k++) {
      if (x >= keys[k].x && x < keys[k].x + keys[k].w) return press(keys[k]);
    }
    return Result::None;
  }

 private:
  static constexpr int kMaxKeysPerRow = 11;
  enum KeyKind : uint8_t { CHAR, SHIFT, DEL, SYMBOLS, SPACE, ACTION, CANCEL };
  struct Key { int x, w; KeyKind kind; char ch; const char* label; };

  int rowKeys(int r, int width, const char* action_label, Key* out) const {
    static const char* letters[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const char* symbols[3] = {"1234567890", "-/:;()$&@", ".,?!'\"#"};
    const char* const* rows = _symbols ? symbols : letters;
    const int u = width / 10;
    int n = 0;
    if (r == 0 || r == 1) {
      const int len = strlen(rows[r]);
      const int x0 = (width - len * u) / 2;
      for (int i = 0; i < len; i++) out[n++] = {x0 + i * u, u, CHAR, rows[r][i], nullptr};
    } else if (r == 2) {
      const int len = strlen(rows[2]);
      const int side = (width - len * u) / 2;
      out[n++] = {0, side, SHIFT, 0, _shift ? "ABC" : "abc"};
      for (int i = 0; i < len; i++) out[n++] = {side + i * u, u, CHAR, rows[2][i], nullptr};
      out[n++] = {side + len * u, width - side - len * u, DEL, 0, "<-"};
    } else {
      out[n++] = {0, 2 * u, SYMBOLS, 0, _symbols ? "abc" : "123"};
      out[n++] = {2 * u, 4 * u, SPACE, ' ', "space"};
      out[n++] = {6 * u, u + u / 2, CANCEL, 0, "X"};
      out[n++] = {7 * u + u / 2, width - 7 * u - u / 2, ACTION, 0, action_label};
    }
    return n;
  }

  void drawKey(DisplayDriver& d, const Key& k, int y, int key_h) const {
    d.setColor(k.kind == ACTION ? UIColor::corp_blue : UIColor::secondary_txt);
    d.drawRect(k.x + 1, y + 1, k.w - 2, key_h - 2);
    d.setColor(UIColor::primary_txt);
    char cap[2] = {k.ch, 0};
    if (k.kind == CHAR && _shift && !_symbols && cap[0] >= 'a' && cap[0] <= 'z') cap[0] -= 32;
    d.drawTextCentered(k.x + k.w / 2, y + key_h / 2 - 4, k.label ? k.label : cap);
  }

  Result press(const Key& k) {
    switch (k.kind) {
      case CHAR: {
        char c = k.ch;
        if (_shift && !_symbols && c >= 'a' && c <= 'z') { c -= 32; _shift = false; }
        append(c);
        return Result::Changed;
      }
      case SPACE: append(' '); return Result::Changed;
      case SHIFT: _shift = !_shift; return Result::Changed;
      case SYMBOLS: _symbols = !_symbols; return Result::Changed;
      case DEL: if (_len) _text[--_len] = 0; return Result::Changed;
      case CANCEL: return Result::Cancel;
      case ACTION: return Result::Submit;
    }
    return Result::None;
  }

  void append(char c) {
    if (_len >= _max_len) return;
    _text[_len++] = c;
    _text[_len] = 0;
  }

  char _text[kCapacity] = {};
  size_t _len = 0;
  size_t _max_len = kCapacity - 1;
  bool _shift = false;
  bool _symbols = false;
  bool _masked = false;
};
