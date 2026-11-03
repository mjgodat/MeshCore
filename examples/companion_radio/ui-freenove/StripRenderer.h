#pragma once

// Strip renderer for screens without a full-frame buffer (the CYD head:
// classic ESP32, no PSRAM). Pure C++11, host-tested by
// test/test_strip_renderer.py.
//
// One small band buffer (w x bandRows RGB565, internal RAM) is drawn
// band by band: for each band the frame's whole draw is replayed into a
// FrameBuffer565 in band mode (its clip is the band, so only the band's
// pixels are written and everything else is skipped early), then the band
// is handed to `push` (blocking SPI on the device). A band whose pixels
// hash the same as last frame's is not pushed, so an idle screen sends
// nothing and a clock tick sends one band.
//
// The draw must produce the same pixels whichever band it is clipped to:
// pages advance their state (layout steps, animations) only on band 0
// (PageContext::band), never on the replays.

#include <stdint.h>

#include "FrameBuffer565.h"

class StripRenderer {
 public:
  // Bands with a hash (beyond them a band is always pushed).
  static const int kMaxBands = 32;
  // Draws the whole frame into fb (clipped to fb's band); band = index.
  typedef void (*DrawFn)(FrameBuffer565& fb, int band, void* ctx);
  // Sends rows y0..y0+rows-1 (row-major, w px per row).
  typedef void (*PushFn)(const uint16_t* px, int w, int y0, int rows, void* ctx);

  StripRenderer(uint16_t* buf, int w, int h, int bandRows)
      : _buf(buf), _w(w), _h(h), _bandRows(bandRows > 0 ? bandRows : 1), _valid(false), _pushed(0) {
    _bands = _h > 0 ? (_h + _bandRows - 1) / _bandRows : 0;
    for (int i = 0; i < kMaxBands; i++) _hash[i] = 0;
  }

  int bands() const { return _bands; }
  int bandRows() const { return _bandRows; }
  int width() const { return _w; }
  int height() const { return _h; }
  // The next render pushes every band (the glass is unknown).
  void invalidate() { _valid = false; }
  int lastPushed() const { return _pushed; }

  // Renders one frame; returns the number of bands pushed (force: every
  // band, e.g. for a screenshot).
  int render(DrawFn draw, void* dctx, PushFn push, void* pctx, bool force = false) {
    _pushed = 0;
    if (_buf == nullptr || draw == nullptr) return 0;
    FrameBuffer565 fb(_buf, _w, _h, 0, _bandRows);
    for (int b = 0; b < _bands; b++) {
      const int y0 = b * _bandRows;
      const int rows = y0 + _bandRows <= _h ? _bandRows : _h - y0;
      fb.setBand(y0, rows);
      draw(fb, b, dctx);
      if (b < kMaxBands) {
        const uint32_t h = hashPixels(_buf, (long)_w * rows);
        if (_valid && !force && h == _hash[b]) continue;
        _hash[b] = h;
      }
      if (push != nullptr) push(_buf, _w, y0, rows, pctx);
      _pushed++;
    }
    _valid = true;
    return _pushed;
  }

  // FNV-1a over 32-bit words (fast on the ESP32; a collision only means one
  // band is not resent until it changes again).
  static uint32_t hashPixels(const uint16_t* px, long n) {
    uint32_t h = 2166136261u;
    long i = 0;
    for (; i + 1 < n; i += 2) h = (h ^ ((uint32_t)px[i] | ((uint32_t)px[i + 1] << 16))) * 16777619u;
    if (i < n) h = (h ^ px[i]) * 16777619u;
    return h;
  }

 private:
  uint16_t* _buf;
  int _w, _h, _bandRows, _bands;
  bool _valid;
  int _pushed;
  uint32_t _hash[kMaxBands];
};
