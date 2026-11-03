#pragma once

// Pure coordinate helpers for the Freenove FNK0104N (ST77922, 320x480
// portrait panel) driven as a 480x320 landscape screen. No Arduino/IDF
// dependencies: host-tested by test/test_freenove_geometry.py.
//
// Orientation. The ST77922's MADCTL.MV (row/column exchange) is not reliable
// across modules (see Espressif esp_lcd_st77922 README, "MADCTL MV axis
// swap"), so the panel always runs portrait and landscape pixels are
// transposed in software on push:
//
//   default : panel (px, py) = (319 - ly, lx)   landscape = panel turned 90
//             degrees clockwise (the landscape top edge is the panel's
//             right-hand column).
//   flip    : panel (px, py) = (ly, 479 - lx)   the default rotated 180
//             degrees (build flag FREENOVE_LANDSCAPE_FLIP=1).
//
// Which of the two is "right way up" for the board depends on how it is
// held (USB side); the bring-up test pattern shows it.

#include <stdint.h>

#ifndef FREENOVE_LANDSCAPE_FLIP
  #define FREENOVE_LANDSCAPE_FLIP 0
#endif

namespace freenove {

constexpr int kLandscapeW = 480;
constexpr int kLandscapeH = 320;
constexpr int kPanelW = 320;   // panel columns (CASET range 0..319)
constexpr int kPanelH = 480;   // panel rows    (RASET range 0..479)

// ST77922 column window: x_start % 4 == 0 and (x_end + 1) % 4 == 0
// (esp_lcd_st77922 README, "Notes"). Landscape rows become panel columns, so
// landscape row bands must start on and span multiples of 4.
constexpr int kRowAlign = 4;

// Touch mirror bits, applied in panel (portrait) space before rotation.
constexpr uint8_t kTouchMirrorX = 0x01;
constexpr uint8_t kTouchMirrorY = 0x02;

struct Point {
  int x;
  int y;
};

struct ColumnWindow {
  int c0;  // first panel column (inclusive)
  int c1;  // last panel column (inclusive)
};

inline Point toPanel(int lx, int ly, bool flip) {
  if (flip) return Point{ly, kPanelH - 1 - lx};
  return Point{kPanelW - 1 - ly, lx};
}

inline Point panelToLandscape(int px, int py, bool flip) {
  if (flip) return Point{kPanelH - 1 - py, px};
  return Point{py, kPanelW - 1 - px};
}

inline int clampInt(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Touch controller reading (portrait, 0..touchW-1 x 0..touchH-1) to landscape
// pixels. touchW/touchH are the resolution the controller reports (registers
// 0x0005..0x0008); <= 0 means "unknown, assume the panel's". Readings past the
// edge clamp; negative readings are rejected.
inline bool touchToLandscape(int tx, int ty, int touchW, int touchH, bool flip,
                             uint8_t mirror, int& lx, int& ly) {
  if (tx < 0 || ty < 0) return false;
  if (touchW <= 0) touchW = kPanelW;
  if (touchH <= 0) touchH = kPanelH;
  int px = (int)((long)tx * kPanelW / touchW);
  int py = (int)((long)ty * kPanelH / touchH);
  px = clampInt(px, 0, kPanelW - 1);
  py = clampInt(py, 0, kPanelH - 1);
  if (mirror & kTouchMirrorX) px = kPanelW - 1 - px;
  if (mirror & kTouchMirrorY) py = kPanelH - 1 - py;
  Point l = panelToLandscape(px, py, flip);
  lx = l.x;
  ly = l.y;
  return true;
}

// Touch timing. The controller sets 0x0010 bit 3 only when a *new* report is
// ready, so a held finger shows gaps between reports: keep it down until
// kTouchHoldMs pass with none. After reset the controller is not polled for
// kTouchWarmupMs; probe/resolution reads retry every kTouchRetryMs.
constexpr uint32_t kTouchHoldMs = 90;
constexpr uint32_t kTouchWarmupMs = 1000;
constexpr uint32_t kTouchRetryMs = 500;

class TouchHold {
public:
  // hasPoint: a valid point was read this poll (px, py). Returns whether the
  // finger counts as down; x/y get the last reported point while it is.
  bool update(bool hasPoint, int px, int py, uint32_t nowMs, int& x, int& y) {
    if (hasPoint) {
      _down = true;
      _x = px;
      _y = py;
      _lastMs = nowMs;
    } else if (_down && (uint32_t)(nowMs - _lastMs) >= kTouchHoldMs) {
      _down = false;
    }
    if (_down) {
      x = _x;
      y = _y;
    }
    return _down;
  }
  bool down() const { return _down; }

private:
  bool _down = false;
  int _x = -1;
  int _y = -1;
  uint32_t _lastMs = 0;
};

inline bool touchWarmedUp(uint32_t nowMs, uint32_t resetMs) {
  return (uint32_t)(nowMs - resetMs) >= kTouchWarmupMs;
}

class TouchRetry {
public:
  bool due(uint32_t nowMs) const {
    return !_tried || (uint32_t)(nowMs - _lastMs) >= kTouchRetryMs;
  }
  void attempted(uint32_t nowMs) {
    _tried = true;
    _lastMs = nowMs;
  }

private:
  bool _tried = false;
  uint32_t _lastMs = 0;
};

// Expand a landscape row range to kRowAlign boundaries, clamped to the
// screen. Returns false when nothing of the range is on screen.
inline bool alignRows(int y0, int rows, int& outY0, int& outRows) {
  if (rows <= 0) return false;
  int start = y0;
  int end = y0 + rows;  // exclusive
  if (start < 0) start = 0;
  if (end > kLandscapeH) end = kLandscapeH;
  if (start >= end) return false;
  start -= start % kRowAlign;
  end = (end + kRowAlign - 1) / kRowAlign * kRowAlign;
  if (end > kLandscapeH) end = kLandscapeH;
  outY0 = start;
  outRows = end - start;
  return true;
}

// Panel columns covered by landscape rows [y0, y0 + rows).
inline ColumnWindow bandColumns(int y0, int rows, bool flip) {
  if (flip) return ColumnWindow{y0, y0 + rows - 1};
  return ColumnWindow{kPanelW - y0 - rows, kPanelW - 1 - y0};
}

inline uint16_t swap16(uint16_t v) {
  return (uint16_t)((v << 8) | (v >> 8));
}

// Transpose part of a landscape row band into panel memory order.
//   src  : `rows` landscape rows of kLandscapeW RGB565 pixels (native order),
//          the band's first row first.
//   py0, npy : panel rows to produce (0..479).
//   dst  : npy * rows pixels; panel row py0 + r, column bandColumns().c0 + c
//          lands at dst[r * rows + c] -- the order RAMWR expects for the
//          window bandColumns() x [py0, py0 + npy).
//   swapBytes : emit big-endian (wire order) pixels.
inline void transposeBand(const uint16_t* src, int rows, int py0, int npy,
                          bool flip, bool swapBytes, uint16_t* dst) {
  for (int r = 0; r < npy; r++) {
    const int py = py0 + r;
    const int lx = flip ? (kPanelH - 1 - py) : py;
    uint16_t* out = dst + r * rows;
    for (int c = 0; c < rows; c++) {
      // Panel column c0 + c: default -> landscape row (rows - 1 - c) of the
      // band; flip -> row c.
      const int band_row = flip ? c : (rows - 1 - c);
      const uint16_t v = src[band_row * kLandscapeW + lx];
      out[c] = swapBytes ? swap16(v) : v;
    }
  }
}

// transposeBand() straight from an RGB332 framebuffer: src332 is the band's
// first landscape row (stride kLandscapeW, one byte per pixel) and every pixel
// goes through lut (256 entries, already in wire order when built
// byteswapped). Reads run along landscape rows (sequential bytes, kind to the
// PSRAM cache); the strided writes land in the internal bounce buffer.
inline void transposeBand332(const uint8_t* src332, int rows, int py0, int npy,
                             bool flip, const uint16_t* lut, uint16_t* dst) {
  for (int c = 0; c < rows; c++) {
    const int band_row = flip ? c : (rows - 1 - c);
    const uint8_t* line = src332 + band_row * kLandscapeW;
    uint16_t* out = dst + c;
    if (flip) {
      const uint8_t* p = line + (kPanelH - 1 - py0);
      for (int r = 0; r < npy; r++, out += rows) *out = lut[*p--];
    } else {
      const uint8_t* p = line + py0;
      for (int r = 0; r < npy; r++, out += rows) *out = lut[*p++];
    }
  }
}

// transposeBand() straight from a whole-frame RGB565 framebuffer (native
// order, stride kLandscapeW): src565 is the band's first landscape row.
// Like transposeBand332, reads run along landscape rows (sequential, kind to
// the PSRAM cache); every pixel is byte-swapped to wire order.
inline void transposeBand565(const uint16_t* src565, int rows, int py0, int npy,
                             bool flip, uint16_t* dst) {
  for (int c = 0; c < rows; c++) {
    const int band_row = flip ? c : (rows - 1 - c);
    const uint16_t* line = src565 + band_row * kLandscapeW;
    uint16_t* out = dst + c;
    if (flip) {
      const uint16_t* p = line + (kPanelH - 1 - py0);
      for (int r = 0; r < npy; r++, out += rows) *out = swap16(*p--);
    } else {
      const uint16_t* p = line + py0;
      for (int r = 0; r < npy; r++, out += rows) *out = swap16(*p++);
    }
  }
}

// RGB332 -> RGB565 by bit replication (full-scale white and black exact).
inline uint16_t rgb332To565(uint8_t c) {
  const uint8_t r3 = (c >> 5) & 0x07;
  const uint8_t g3 = (c >> 2) & 0x07;
  const uint8_t b2 = c & 0x03;
  const uint16_t r5 = (uint16_t)((r3 << 2) | (r3 >> 1));
  const uint16_t g6 = (uint16_t)((g3 << 3) | g3);
  const uint16_t b5 = (uint16_t)((b2 << 3) | (b2 << 1) | (b2 >> 1));
  return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// Bring-up test pattern (landscape): 8 vertical colour bars, 60 px each,
// left to right white, yellow, cyan, green, magenta, red, blue, black; a
// 24x24 marker with a 1 px dark-grey border in each corner: top-left orange,
// top-right teal, bottom-left purple, bottom-right grey.
constexpr uint16_t kMarkerTL = 0xFD20;      // orange
constexpr uint16_t kMarkerTR = 0x0451;      // teal
constexpr uint16_t kMarkerBL = 0x780F;      // purple
constexpr uint16_t kMarkerBR = 0x8410;      // grey
constexpr uint16_t kMarkerBorder = 0x4208;  // dark grey
constexpr int kMarkerSize = 24;

inline uint16_t testPatternColor(int lx, int ly) {
  const bool left = lx < kMarkerSize;
  const bool right = lx >= kLandscapeW - kMarkerSize;
  const bool top = ly < kMarkerSize;
  const bool bottom = ly >= kLandscapeH - kMarkerSize;
  if ((left || right) && (top || bottom)) {
    const int mx = left ? lx : lx - (kLandscapeW - kMarkerSize);
    const int my = top ? ly : ly - (kLandscapeH - kMarkerSize);
    if (mx == 0 || my == 0 || mx == kMarkerSize - 1 || my == kMarkerSize - 1)
      return kMarkerBorder;
    if (top) return left ? kMarkerTL : kMarkerTR;
    return left ? kMarkerBL : kMarkerBR;
  }
  static const uint16_t bars[8] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0,
                                   0xF81F, 0xF800, 0x001F, 0x0000};
  int b = lx / (kLandscapeW / 8);
  if (b < 0) b = 0;
  if (b > 7) b = 7;
  return bars[b];
}

}  // namespace freenove
