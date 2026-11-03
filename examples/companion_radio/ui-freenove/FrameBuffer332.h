#pragma once

// 8-bit (RGB332) framebuffer and drawing primitives for the Freenove visual
// head, plus the change tracking that decides what goes to the panel.
// Pure C++ (no Arduino/IDF): host-tested by test/test_framebuffer332.py.
//
// Pixels are one byte, RRRGGGBB, row-major, `w` bytes per row. They are
// expanded to RGB565 only on the way to the panel (buildLut565 + FramePusher).
//
// Every primitive clips to the buffer: any int coordinates are safe.
//
// Change tracking
// ---------------
// Every primitive records, per row, the x-range it wrote (RowSpans), in two
// sets:
//   push spans   : everything written since the frame was last handed to the
//                  pusher (takePushSpans()). The pusher only looks at these
//                  rows/columns (plus what it pushed last time, see
//                  PushPlanner), so an unchanged screen costs nothing and a
//                  moving dot costs its own rows, not a full-frame hash.
//   object spans : what primitives drew since the last eraseTouched(). Pages
//                  keep a static background buffer and start each frame with
//                  eraseTouched(background): only the pixels last frame's
//                  objects covered are restored, not the 150 KB frame.
// Writers that bypass the primitives use rawPixels(), which marks the whole
// frame (or call markAll() after writing through another alias).
//
// Push planning
// -------------
// The panel is portrait, so a landscape row band is a column window on the
// glass (the ST77922 needs columns in multiples of 4: kBandAlign) and the
// landscape x-range is the panel row range. PushPlanner compares only the
// candidate spans with a shadow of the glass and keeps the columns that
// really changed (a page that redraws the same thing sends nothing), then
// planSpanBands() groups 4-row blocks and merges neighbours while one window
// is cheaper than two (kWindowCostBytes per window).
//
// diffRows()/planBands() are the plain full-frame variant (hash every row,
// full-width bands), kept for callers without tracking and for tests.

#include <stdint.h>

// Fonts for text().
constexpr uint8_t kFont6x8Id = 0;
constexpr uint8_t kFont8x16Id = 1;

// Mockup palette (layout-v2.html / visual-direction.html) on the RGB332 cube.
// Nearest RGB332 colour of each mockup hex (rounded per channel).
namespace fbpal {
constexpr uint8_t kBlack = 0x00;     // #03040a background
constexpr uint8_t kNavy = 0x01;      // #0b1030 deep panel blue
constexpr uint8_t kGrid = 0x05;      // #12324a rules / borders
constexpr uint8_t kCyan = 0x3B;      // #22e3ff links, HUD
constexpr uint8_t kSky = 0x7B;       // #5fd3ff HUD text
constexpr uint8_t kIce = 0x9B;       // #8fe9ff body text
constexpr uint8_t kGreen = 0x9E;     // #9dffb0 nodes / ok
constexpr uint8_t kAmber = 0xF9;     // #ffd166 names / warnings
constexpr uint8_t kMagenta = 0xEE;   // #ff5fa2 packets / alerts
constexpr uint8_t kViolet = 0xB3;    // #b18cff secondary
constexpr uint8_t kWhite = 0xFF;
}  // namespace fbpal

constexpr int kMaxFbRows = 320;

// Per-row touched x-range (inclusive); x0 > x1 = row untouched.
struct RowSpans {
  int16_t x0[kMaxFbRows];
  int16_t x1[kMaxFbRows];

  void reset();
  void setAll(int w, int h);
  bool has(int y) const { return x0[y] <= x1[y]; }
  // a <= b, both already clipped to the buffer.
  void add(int y, int a, int b) {
    if (a < x0[y]) x0[y] = (int16_t)a;
    if (b > x1[y]) x1[y] = (int16_t)b;
  }
  void merge(const RowSpans& o);
  int rows() const;   // rows touched
};

class FrameBuffer332 {
public:
  // Starts with every row in the push spans (contents unknown).
  FrameBuffer332(uint8_t* pixels /*w*h*/, int w = 480, int h = 320);

  // 8-bit channels to RGB332, rounding to the nearest level per channel.
  static uint8_t rgb(uint8_t r, uint8_t g, uint8_t b);
  // Glow ramp entry for colour c: level 0 = c, 1 = inner ring (~60%),
  // 2 = outer ring (~30%). Channels scale toward black, so the ramp keeps
  // the hue as far as the 332 cube allows; a lit colour never ramps to black.
  static uint8_t glowRamp(uint8_t c, int level);
  // Per-channel maximum (additive-looking blend for overlapping glows).
  static uint8_t maxBlend(uint8_t a, uint8_t b);

  int width() const { return _w; }
  int height() const { return _h; }
  const uint8_t* pixels() const { return _px; }
  // Writable pixels for code that bypasses the primitives (LGFX, raw
  // copies): marks the whole frame changed (markAll()), so the pusher
  // re-examines every row on the next submit.
  uint8_t* rawPixels() {
    markAll();
    return _px;
  }
  uint8_t get(int x, int y) const;   // 0 off-buffer

  void clear(uint8_t c);
  // Copy a whole w*h frame (e.g. the page background) into this buffer.
  void copyFrom(const uint8_t* src);
  // Restore what objects drew since the last call: from `background` (a w*h
  // frame) or, when it is null, fill with c. Clears the object spans.
  void eraseTouched(const uint8_t* background, uint8_t c = 0);

  void pixel(int x, int y, uint8_t c);
  void hline(int x, int y, int w, uint8_t c);
  void vline(int x, int y, int h, uint8_t c);
  void fillRect(int x, int y, int w, int h, uint8_t c);
  void rect(int x, int y, int w, int h, uint8_t c);
  void line(int x0, int y0, int x1, int y1, uint8_t c);
  // Dashes along the line: `on` pixels lit, `off` skipped, counted in steps
  // along the major axis from (x0, y0). Pixel i is lit when
  // (i - phase) mod (on + off) < on, so phase + 1 moves the dashes one pixel
  // toward (x1, y1) (marching ants).
  void dashedLine(int x0, int y0, int x1, int y1, uint8_t c, int on, int off,
                  int phase);
  void circle(int cx, int cy, int r, uint8_t c);
  void fillCircle(int cx, int cy, int r, uint8_t c);
  // Ring between radius r - thick and r, from a0 to a1 degrees. 0 deg = +x
  // (3 o'clock), angles grow clockwise on screen (y down); a1 < a0 wraps.
  void arc(int cx, int cy, int r, int a0_deg, int a1_deg, int thick,
           uint8_t c);
  // Monospace text, top-left at (x, y). font kFont6x8Id or kFont8x16Id;
  // characters outside 0x20..0x7E draw as '?'. Returns the x after the text.
  int text(int x, int y, const char* s, uint8_t c, uint8_t font);
  static int textWidth(const char* s, uint8_t font);
  // Glow dot: core disc (radius kGlowCore) in c, then two rings in
  // glowRamp(c, 1) and glowRamp(c, 2) out to kGlowOuter, max-blended so
  // overlapping glows brighten instead of erasing each other.
  void glowDot(int x, int y, uint8_t c);
  static constexpr int kGlowCore = 2;
  static constexpr int kGlowMid = 4;
  static constexpr int kGlowOuter = 6;

  uint32_t rowHash(int y) const;

  // Change tracking (see top of file).
  const RowSpans& pushSpans() const { return _push; }
  const RowSpans& objectSpans() const { return _obj; }
  // Merge the push spans into `into`, then clear them (the pusher calls this
  // when it takes the frame).
  void takePushSpans(RowSpans& into);
  // Everything changed (pixels written behind the primitives' back).
  void markAll();

  struct Sector;

private:
  // Record a drawn run on row y, columns a..b (clipped, a <= b).
  void touch(int y, int a, int b) {
    _push.add(y, a, b);
    _obj.add(y, a, b);
  }
  void ring(long long cx, long long cy, long long ro, long long ri, uint8_t c,
            const Sector* sector);

  uint8_t* _px;
  int _w;
  int _h;
  RowSpans _push;
  RowSpans _obj;
};

// RGB332 -> RGB565 lookup (bit replication: full-scale white and black are
// exact). byteswap = store each entry in wire (big-endian) order.
void buildLut565(uint16_t lut[256], bool byteswap);

struct DirtyRows {   // coalesced runs
  int count;
  int y0[kMaxFbRows];
  int n[kMaxFbRows];
};

// Hash every row of fb, compare with last_hashes (one per row), record the
// changed rows as runs in out and store the new hashes in last_hashes.
void diffRows(const FrameBuffer332& fb, uint32_t last_hashes[kMaxFbRows],
              DirtyRows& out);

// Panel bands from exact runs: each run expanded to `align` rows, then bands
// whose gap is <= mergeGap rows merged, clamped to [0, h). Full width.
constexpr int kBandAlign = 4;
constexpr int kBandMergeGap = 8;
void planBands(const DirtyRows& runs, int h, int align, int mergeGap,
               DirtyRows& bands);

// Pixel bytes the bands put on the wire (RGB565, w pixels per row).
long bandBytes(const DirtyRows& bands, int w);

// Windows with an x-range: rows [y0, y0 + n), landscape columns [x0, x1].
constexpr int kMaxBands = kMaxFbRows / kBandAlign;
struct SpanBands {
  int count;
  int16_t y0[kMaxBands];
  int16_t n[kMaxBands];
  int16_t x0[kMaxBands];
  int16_t x1[kMaxBands];
};

// Fixed cost of one panel window in wire-byte equivalents (three queued
// transactions per chunk); used to decide when two windows should merge.
constexpr long kWindowCostBytes = 1024;
void planSpanBands(const RowSpans& rows, int h, int align, long windowCost,
                   SpanBands& out);
long spanBandBytes(const SpanBands& bands);

// What the pusher does per frame, minus the SPI. It keeps a shadow copy of
// what the glass shows (a w*h RGB332 buffer, PSRAM on the device). The
// frame can only differ from the glass where it was drawn since its last
// push or where the previous push changed the glass (for a double-buffered
// page that is exactly where the other buffer differed), so only those spans
// are compared with the shadow -- from both ends, word by word -- and only
// the columns that really differ are sent and copied into the shadow. That
// comparison is the backstop: a redraw with identical pixels sends nothing,
// and the windows are trimmed to the changed columns, which a row hash
// cannot do.
class PushPlanner {
public:
  struct Stats {
    int candidateRows;
    long comparedBytes;  // frame bytes compared with the shadow
    int skippedRows;     // candidates identical to the glass
    int pushedRows;
  };
  // shadow: w*h bytes, owned by the caller, contents irrelevant (the first
  // plan sends everything).
  PushPlanner(uint8_t* shadow, int w = 480, int h = 320);
  // The next plan() sends the whole frame and compares every row on the
  // plan after it (first frame, SPI errors, a dropped submit: anything that
  // breaks the frame-to-frame chain the spans rely on).
  // A new FrameBuffer332 starts with every row marked, so a double-buffered
  // page needs nothing else.
  void invalidate() { _force = true; }
  // Take fb's push spans (and clear them). Call once per submitted frame.
  void take(FrameBuffer332& fb);
  // Plan the windows for fb (the frame last passed to take()) and record
  // them in the shadow.
  void plan(const FrameBuffer332& fb, SpanBands& out, Stats* stats = nullptr);

private:
  uint8_t* _shadow;
  int _w;
  int _h;
  RowSpans _cand;                     // spans taken from the frame
  RowSpans _prev;                     // columns changed by the last push
  bool _force;
};
