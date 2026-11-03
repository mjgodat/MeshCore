#pragma once

// RGB565 framebuffer and anti-aliased drawing for the Freenove visual head's
// HMI theme (Task 7c). Pure C++11 (no Arduino/IDF): host-tested by
// test/test_framebuffer565.py and rendered to previews by test_layouts.py.
//
// Pixels are native-order RGB565 (bit 15..11 red), row-major, `w` pixels per
// row; St77922Panel byte-swaps them for the wire while transposing.
//
// Change tracking is the same as FrameBuffer332's (see there): every write
// records its x-range per row in the push spans (what the pusher compares
// with the glass) and the object spans (what eraseTouched() restores from
// the page's cached background layer). Pages draw their dynamic layer over
// a restored background each frame, so a frame costs what it drew.
//
// Anti-aliasing: lines, rings/arcs/discs and rounded rectangles compute a
// coverage per pixel from the exact distance to the shape (pixel centres at
// integer coordinates) and blend; only the pixels near an edge are
// evaluated. Text uses 4-bit alpha glyphs (FbFontsAA.h).
//
// Everything clips to the buffer and to the clip rectangle (setClip):
// any coordinates are safe.
//
// Band mode (the CYD's strip renderer, no full-screen buffer): a buffer of
// `rows` rows holds rows y0..y0+rows-1 of a w x h frame. Coordinates stay
// those of the whole frame; the clip never leaves the band, so a page draws
// exactly as on a full frame and only the band's pixels are written.
// setBand() moves the same buffer to another band. A full frame is the band
// y0 = 0, rows = h (the plain constructor), so nothing changes for it.

#include <stdint.h>

#include "FbFontsAA.h"
#include "FrameBuffer332.h"   // RowSpans, SpanBands, planSpanBands

// 8-bit channels to RGB565 (truncating, like most RGB565 tools).
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (uint16_t)(b >> 3));
}

class FrameBuffer565 {
public:
  // Text anchor (like PIL's): left/right/centre x, top of the line box or
  // the middle of ascender + descender.
  enum Anchor { kLeftTop, kRightTop, kCentreTop, kLeftMiddle, kRightMiddle, kCentreMiddle };

  // Starts with every row in the push spans (contents unknown).
  FrameBuffer565(uint16_t* pixels /*w*h*/, int w = 480, int h = 320);
  // Band mode: `strip` holds w * rows pixels, rows y0.. of a w x h frame.
  FrameBuffer565(uint16_t* strip, int w, int h, int y0, int rows);
  // Band mode: the buffer now holds rows y0..y0+rows-1 (rows <= the
  // buffer's rows); resets the clip to the band and the object spans.
  void setBand(int y0, int rows);
  int bandY0() const { return _y0; }
  int bandRows() const { return _rows; }
  // The band's own pixels (row 0 = frame row bandY0()).
  const uint16_t* bandPixels() const { return _buf; }

  int width() const { return _w; }
  int height() const { return _h; }
  const uint16_t* pixels() const { return _px; }
  // Writable pixels behind the primitives' back: marks the whole frame.
  // Indexed with frame coordinates (y * width() + x); in band mode only the
  // band's rows (clipY0()..clipY1() after resetClip()) may be written.
  uint16_t* rawPixels() {
    markAll();
    return _px;
  }
  uint16_t get(int x, int y) const;   // 0 off-buffer

  // Channel helpers.
  static uint8_t red(uint16_t c) { return (uint8_t)(((c >> 11) & 31) * 255 / 31); }
  static uint8_t green(uint16_t c) { return (uint8_t)(((c >> 5) & 63) * 255 / 63); }
  static uint8_t blue(uint16_t c) { return (uint8_t)((c & 31) * 255 / 31); }
  // d + (s - d) * a / 255 per channel.
  static uint16_t mix(uint16_t d, uint16_t s, unsigned a);
  // Float channels (0..255) to RGB565 with a 4x4 ordered dither at (x, y):
  // smooth gradients without banding.
  static uint16_t dither(float r, float g, float b, int x, int y);

  // Clip rectangle (inclusive x0..x0+w-1); resetClip() = whole buffer
  // (in band mode: the band). The clip never leaves the band.
  void setClip(int x, int y, int w, int h);
  void resetClip();
  int clipY0() const { return _cy0; }
  int clipY1() const { return _cy1; }   // inclusive; < clipY0() when empty

  void clear(uint16_t c);
  void copyFrom(const uint16_t* src);   // whole w*h frame
  // Restore what objects drew since the last call from `background` (a w*h
  // frame) or fill with c when it is null; clears the object spans.
  void eraseTouched(const uint16_t* background, uint16_t c = 0);

  void pixel(int x, int y, uint16_t c);
  void blendPixel(int x, int y, uint16_t c, unsigned alpha);
  void hline(int x, int y, int w, uint16_t c);
  void vline(int x, int y, int h, uint16_t c);
  void fillRect(int x, int y, int w, int h, uint16_t c);
  void fillRectAlpha(int x, int y, int w, int h, uint16_t c, unsigned alpha);
  void rect(int x, int y, int w, int h, uint16_t c);
  // Hard (1 px) lines; dashes as FrameBuffer332::dashedLine.
  void line(int x0, int y0, int x1, int y1, uint16_t c);
  void dashedLine(int x0, int y0, int x1, int y1, uint16_t c, int on, int off, int phase);

  // Anti-aliased line `width` px wide with round caps.
  void aaLine(float x0, float y0, float x1, float y1, float width, uint16_t c, unsigned alpha = 255);
  // Anti-aliased ring between radius r - thick and r (thick >= r: a disc);
  // with a0..a1 (degrees, 0 = +x, clockwise on screen) only that arc.
  void aaRing(float cx, float cy, float r, float thick, uint16_t c, unsigned alpha = 255);
  void aaArc(float cx, float cy, float r, float thick, float a0_deg, float a1_deg, uint16_t c,
             unsigned alpha = 255);
  void aaDisc(float cx, float cy, float r, uint16_t c, unsigned alpha = 255) {
    aaRing(cx, cy, r, r + 1.0f, c, alpha);
  }
  // Rounded rectangle covering pixels x..x+w-1, y..y+h-1 (corner radius r).
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c, unsigned alpha = 255);
  // 1 px outline along the inside edge of the same shape.
  void strokeRoundRect(int x, int y, int w, int h, int r, uint16_t c, unsigned alpha = 255);

  // Anti-aliased text; (x, y) placed per `anchor` (top = top of the line
  // box, the font's ascent above the baseline). `spacing` adds px between
  // glyphs. Returns the pen x after the text (left anchors) or x.
  int text(int x, int y, const char* s, uint16_t c, const AAFont& f, Anchor anchor = kLeftTop,
           int spacing = 0);
  static int textWidth(const char* s, const AAFont& f, int spacing = 0);

  // Change tracking (see FrameBuffer332.h).
  const RowSpans& pushSpans() const { return _push; }
  const RowSpans& objectSpans() const { return _obj; }
  void takePushSpans(RowSpans& into);
  void markAll();

private:
  void touch(int y, int a, int b) {
    _push.add(y, a, b);
    _obj.add(y, a, b);
  }
  bool clipRow(int y, int& a, int& b) const;
  void put(int x, int y, uint16_t c);              // clipped, tracked
  void putBlend(int x, int y, uint16_t c, unsigned a);
  void cornerFill(int cx, int cy, int dx, int dy, int r, uint16_t c, unsigned alpha, bool stroke);
  int drawText(int x, int yTop, const char* s, uint16_t c, const AAFont& f, int spacing);

  uint16_t* _px;    // frame row 0 (band mode: _buf - _y0 * _w; only band rows are addressed)
  uint16_t* _buf;   // the buffer as allocated
  int _w;
  int _h;
  int _y0, _rows;   // band held by _buf (full frame: 0, _h)
  int _cx0, _cy0, _cx1, _cy1;   // clip, inclusive
  RowSpans _push;
  RowSpans _obj;
};

// PushPlanner (FrameBuffer332.h) for RGB565 frames: a w*h RGB565 shadow of
// the glass; only the columns that really changed are planned.
class PushPlanner565 {
public:
  typedef PushPlanner::Stats Stats;
  PushPlanner565(uint16_t* shadow, int w = 480, int h = 320);
  void invalidate() { _force = true; }
  void take(FrameBuffer565& fb);
  void plan(const FrameBuffer565& fb, SpanBands& out, Stats* stats = nullptr);

private:
  uint16_t* _shadow;
  int _w;
  int _h;
  RowSpans _cand;
  RowSpans _prev;
  bool _force;
};
