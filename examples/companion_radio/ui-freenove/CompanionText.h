#pragma once

// Anti-aliased text for the companion UI (HEAD page) on the Freenove head
// (pure C++11; host-tested by test/test_companion_text.py).
//
// The companion screens (ui-new Head* screens, TouchKeyboard, lists,
// console) draw through the DisplayDriver text API in logical coordinates
// (240x148, zoomed 2x to the 480x296 companion area). Their text used to be
// LovyanGFX's 6x8 bitmap font scaled up -- blocky. On the Freenove the
// display adapter (FreenoveDisplay) instead records each print() here and,
// after the zoomed sprite (boxes, fills, icons) is on the RGB565 companion
// frame, draws the text with the D-DIN AA fonts at full resolution.
//
// Sizes: setTextSize(1) -> 14 px, (2) -> 20 px, (3+) -> 28 px. Widths are
// reported in logical px (rounded up) so screens lay out with the real
// glyph metrics; each line is centred vertically in the old character cell
// (8 x size logical rows), so screens that place text by that cell (key
// labels, list rows, buttons) stay aligned. print() continues on the same
// line (the cursor advances by the text's width).

#include <stddef.h>
#include <stdint.h>

#include "FbFontsAA.h"
#include "FrameBuffer565.h"

namespace freenove_ui {

class CompanionText {
 public:
  static const int kZoom = 2;          // logical -> physical px
  static const int kMaxOps = 128;
  static const int kPoolBytes = 4096;

  CompanionText() { clear(); }

  static const AAFont& font(int size) {
    return size <= 1 ? kFontSmall : (size == 2 ? kFontText20 : kFontText28);
  }
  // Width in logical px, rounded up.
  static int logicalWidth(const char* s, int size) {
    const int w = FrameBuffer565::textWidth(s, font(size));
    return (w + kZoom - 1) / kZoom;
  }
  // The legacy line height rule (DisplayDriver::textLineHeight on the 6x8
  // font): unchanged, so screens keep their line spacing.
  static int lineHeight(int size) {
    const int glyph = 6 * (size < 1 ? 1 : size);
    return glyph <= 6 ? 10 : glyph * 2;
  }
  // Physical top of the AA line box for a logical cursor y.
  static int physicalTop(int ly, int size) {
    const AAFont& f = font(size);
    const int cell = 8 * (size < 1 ? 1 : size) * kZoom;
    return ly * kZoom + (cell - (f.ascent + f.descent)) / 2;
  }

  void clear() {
    _ops = 0;
    _used = 0;
  }
  int count() const { return _ops; }
  // Record a print at logical (lx, ly); returns the logical x after it.
  // `pad`: physical px added to x when drawn (the HMI skin's tile padding;
  // layout and occlusion keep the logical position).
  int add(int lx, int ly, int size, uint16_t colour, const char* s, int pad = 0) {
    if (s == nullptr) return lx;
    const int w = logicalWidth(s, size);
    size_t n = 0;
    while (s[n]) n++;
    if (n == 0 || _ops >= kMaxOps || _used + n + 1 > (size_t)kPoolBytes) return lx + w;
    Op& o = _op[_ops++];
    o.x = (int16_t)lx;
    o.y = (int16_t)ly;
    o.size = (uint8_t)(size < 1 ? 1 : (size > 3 ? 3 : size));
    o.colour = colour;
    o.pad = (int8_t)(pad < -16 ? -16 : (pad > 16 ? 16 : pad));
    o.text = (uint16_t)_used;
    for (size_t i = 0; i < n; i++) _pool[_used++] = s[i];
    _pool[_used++] = 0;
    return lx + w;
  }
  // A fill drawn after some text covers it (popups, highlights): text whose
  // cell intersects the logical rectangle is dropped.
  void occlude(int lx, int ly, int lw, int lh) {
    for (int i = 0; i < _ops; i++) {
      Op& o = _op[i];
      if (o.size == 0) continue;
      const int w = logicalWidth(_pool + o.text, o.size), h = 8 * o.size;
      if (o.x < lx + lw && o.x + w > lx && o.y < ly + lh && o.y + h > ly) o.size = 0;
    }
  }
  int visible() const {
    int n = 0;
    for (int i = 0; i < _ops; i++) n += _op[i].size != 0;
    return n;
  }
  // FNV-1a over every op (a text-only change must still be pushed).
  uint32_t hash() const {
    uint32_t h = 2166136261u;
    for (int i = 0; i < _ops; i++) {
      const Op& o = _op[i];
      const uint16_t v[5] = {(uint16_t)o.x, (uint16_t)o.y, (uint16_t)o.size, o.colour, (uint16_t)o.pad};
      for (int k = 0; k < 5; k++) {
        h = (h ^ (uint8_t)v[k]) * 16777619u;
        h = (h ^ (uint8_t)(v[k] >> 8)) * 16777619u;
      }
      for (const char* p = _pool + o.text; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
      h = (h ^ 0xFFu) * 16777619u;
    }
    return h;
  }
  // Draw every op (physical coordinates), clipped to rows < max_y.
  void draw(FrameBuffer565& fb, int max_y) const {
    fb.setClip(0, 0, fb.width(), max_y);
    for (int i = 0; i < _ops; i++) {
      const Op& o = _op[i];
      if (o.size == 0) continue;   // covered by a later fill
      fb.text(o.x * kZoom + o.pad, physicalTop(o.y, o.size), _pool + o.text, o.colour, font(o.size));
    }
    fb.resetClip();
  }

 private:
  struct Op {
    int16_t x, y;      // logical
    uint8_t size;      // 1..3; 0 = covered (occlude)
    uint16_t colour;   // RGB565
    int8_t pad;        // physical px added to x
    uint16_t text;     // offset in _pool
  };
  Op _op[kMaxOps];
  char _pool[kPoolBytes];
  int _ops;
  size_t _used;
};

}  // namespace freenove_ui
