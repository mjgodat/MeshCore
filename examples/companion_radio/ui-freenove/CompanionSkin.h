#pragma once

// HMI skin for the companion UI (HEAD page) on the Freenove head (Task 9;
// pure C++11, host-tested by test/test_companion_skin.py).
//
// The companion screens (HeadAdmin console / picker / settings / presets /
// entry, HeadCompose, TouchKeyboard, the home carousel) draw through the
// DisplayDriver API with UIColor roles: a button is a title_bkg (or, when
// on, corp_blue; when disabled, window_bkg) fill inset by one pixel plus a
// secondary_txt rect around it; a key is a bare rect; a header is a
// full-width title_bkg fill. On the Freenove the display adapter
// (FreenoveDisplay) records those fills and rects here instead of drawing
// them flat into the LovyanGFX sprite, and renders them in the owner-
// approved HMI look (Theme.h, mockup mock_glass3) at full resolution:
//
//   button (fill + enclosing rect)  -> rounded tile, thin edge; lit = on
//   full-width button <= 18 rows    -> list row; lit = selected
//   disabled (window fill + rect)   -> hollow, dim edge
//   bare rect                       -> rounded key; corp_blue = action key
//   full-width bare rect            -> text field (dark well)
//   full-width title fill at top    -> header band
//   popup fill (+ border)           -> panel
//   window fill                     -> clears to the backdrop
//   small fills / rects, 1 px rects -> exact geometry, theme colour
//
// Geometry (and therefore every touch target) is unchanged: shapes keep the
// screens' logical rectangles, only inset by a pixel or two at 2x zoom so
// neighbouring tiles read as separate. Text colours map the same roles to
// theme text tokens (skinTextColour). The adapter composes a frame as:
// gradient backdrop, these shapes, the sprite's non-background pixels
// (icons, QR, emoji: overlaySprite), then the AA text (CompanionText).

#include <stddef.h>
#include <stdint.h>

#include "FrameBuffer565.h"
#include "HmiProfile.h"
#include "Theme.h"

namespace freenove_ui {

#if HMI_CYD
struct PanelRect;
// StubPages.cpp: the page backdrop (the CYD keeps no backdrop buffer).
void drawBackdrop(FrameBuffer565& fb, int x, int y, int w, int h, const PanelRect* panels, int n);
#endif

// The companion palette's semantic slots (UIColor values, day palette).
struct CompanionPalette {
  uint16_t window_bkg, title_bkg, popup_bkg, corp_blue;
  uint16_t title_txt, primary_txt, secondary_txt, warning_txt, popup_txt;
};

enum class SkinRole : uint8_t { Other, Window, Title, Popup, Accent, Text, Dim, Warning };

inline SkinRole skinRole(uint16_t c, const CompanionPalette& p) {
  if (c == p.window_bkg) return SkinRole::Window;
  if (c == p.title_bkg) return SkinRole::Title;
  if (c == p.popup_bkg) return SkinRole::Popup;
  if (c == p.corp_blue) return SkinRole::Accent;
  if (c == p.warning_txt) return SkinRole::Warning;
  if (c == p.secondary_txt) return SkinRole::Dim;
  if (c == p.title_txt || c == p.primary_txt || c == p.popup_txt) return SkinRole::Text;
  return SkinRole::Other;
}

// Text (and small marks drawn in a text role) in theme colours. Text in a
// background role sits on a fill that is now a dark tile: it reads white.
inline uint16_t skinTextColour(uint16_t c, const CompanionPalette& p) {
  switch (skinRole(c, p)) {
    case SkinRole::Window:
    case SkinRole::Title:
    case SkinRole::Popup:
    case SkinRole::Text: return theme::kText;
    case SkinRole::Dim: return theme::kTextDim;
    case SkinRole::Warning: return theme::kWarn;
    case SkinRole::Accent: return theme::kAccent;
    case SkinRole::Other: break;
  }
  return c;
}

enum class SkinKind : uint8_t { Erase, Plain, PlainStroke, Line, Header, Button, Row, Key, Field, Popup };

// A resolved shape, logical coordinates.
struct SkinShape {
  SkinKind kind = SkinKind::Plain;
  bool lit = false;       // on / selected / action
  bool enabled = true;    // false: hollow, dim edge
  int16_t x = 0, y = 0, w = 0, h = 0;
  uint16_t colour = 0;    // Plain / PlainStroke
};

class CompanionSkin {
 public:
  static const int kZoom = 2;            // logical -> physical px
  static const int kMaxOps = 128;
  static const int kMinBox = 8;          // logical; smaller marks stay exact
  static const int kMinKey = 12;         // a bare rect at least this tall is a key / field
  static const int kMaxRowH = 18;        // full-width buttons up to this are list rows
  static const int kBodyGap = 7;         // logical rows of page dots under the carousel title bar
  static const int kTextPad = 4;         // physical px, see textPad()
  static const int kBodyPad = 8;         // physical px, text at x 0 in the carousel body panel

  CompanionSkin() { clear(); }

  void clear() { _ops = 0; }
  int count() const { return _ops; }
  void fill(int x, int y, int w, int h, uint16_t colour) { add(false, x, y, w, h, colour); }
  void rect(int x, int y, int w, int h, uint16_t colour) { add(true, x, y, w, h, colour); }

  // FNV-1a over every op: a frame whose shapes alone changed is pushed.
  uint32_t hash() const {
    uint32_t h = 2166136261u;
    for (int i = 0; i < _ops; i++) {
      const Op& o = _op[i];
      const uint16_t v[6] = {(uint16_t)o.x, (uint16_t)o.y, (uint16_t)o.w, (uint16_t)o.h, o.colour,
                             (uint16_t)o.outline};
      for (int k = 0; k < 6; k++) {
        h = (h ^ (uint8_t)v[k]) * 16777619u;
        h = (h ^ (uint8_t)(v[k] >> 8)) * 16777619u;
      }
    }
    return h;
  }

  // Classify the recorded ops (see the table at the top). `out` holds
  // kMaxOps; returns the number of shapes, in drawing order.
  int resolve(SkinShape* out, int screen_w, const CompanionPalette& p) const {
    int n = 0;
    for (int i = 0; i < _ops; i++) {
      const Op& o = _op[i];
      const SkinRole role = skinRole(o.colour, p);
      SkinShape s;
      s.x = o.x;
      s.y = o.y;
      s.w = o.w;
      s.h = o.h;
      const bool fullWidth = o.x <= 0 && o.x + o.w >= screen_w;
      if (o.outline) {
        if (o.w <= 1 || o.h <= 1) {
          s.kind = SkinKind::Line;
        } else if (o.w < kMinBox || o.h < kMinKey) {
          s.kind = SkinKind::PlainStroke;
          s.colour = inkColour(o.colour, p);
        } else if (fullWidth) {
          s.kind = SkinKind::Field;
        } else {
          s.kind = SkinKind::Key;
          s.lit = role == SkinRole::Accent;
        }
        out[n++] = s;
        continue;
      }
      const bool boxRole = role == SkinRole::Window || role == SkinRole::Title || role == SkinRole::Popup ||
                           role == SkinRole::Accent;
      const bool merged = boxRole && i + 1 < _ops && encloses(_op[i + 1], o);
      if (merged) {
        const Op& r = _op[i + 1];
        s.x = r.x;
        s.y = r.y;
        s.w = r.w;
        s.h = r.h;
        i++;   // the rect is this box's edge
      }
      const bool big = s.w >= kMinBox && s.h >= kMinBox;
      const bool wide = s.x <= 0 && s.x + s.w >= screen_w;
      if (role == SkinRole::Window) {
        if (merged && big) {
          s.kind = wide && s.h <= kMaxRowH ? SkinKind::Row : SkinKind::Button;
          s.enabled = false;
        } else {
          s.kind = SkinKind::Erase;
        }
      } else if (big && !merged && wide && s.y <= 1 && role != SkinRole::Accent) {
        s.kind = SkinKind::Header;
        s.lit = role == SkinRole::Title;   // the home carousel's title bar: its body gets a panel
      } else if (big && role == SkinRole::Popup) {
        s.kind = SkinKind::Popup;
      } else if (big && (role == SkinRole::Title || role == SkinRole::Accent)) {
        {
          s.kind = wide && s.h <= kMaxRowH ? SkinKind::Row : SkinKind::Button;
          s.lit = role == SkinRole::Accent;
        }
      } else {
        s.kind = SkinKind::Plain;
        s.colour = role == SkinRole::Title || role == SkinRole::Popup ? theme::kPanelFill : inkColour(o.colour, p);
        if (merged) {   // a small box: keep its edge too
          out[n++] = s;
          s.kind = SkinKind::PlainStroke;
          s.colour = inkColour(_op[i].colour, p);
        }
      }
      out[n++] = s;
    }
    return n;
  }

  // Draw the shapes onto the companion frame (physical px), rows < max_y.
  // `backdrop` is the full-width gradient the frame started from (Erase and
  // hollow shapes restore it).
  void draw(FrameBuffer565& fb, int max_y, const uint16_t* backdrop, const CompanionPalette& p) const {
    static SkinShape shapes[kMaxOps];   // one drawing thread (the UI loop); off the stack
    const int n = resolve(shapes, fb.width() / kZoom, p);
    fb.setClip(0, 0, fb.width(), max_y);
    Body body;
    for (int i = 0; i < n; i++) {
      if (shapes[i].kind != SkinKind::Header || !shapes[i].lit) continue;
      // The home carousel: its page body (below the title bar and the page
      // dots) sits in one HMI panel; clears inside it restore the panel.
      body.on = true;
      body.x = 4;
      body.y = (shapes[i].y + shapes[i].h + kBodyGap) * kZoom;
      body.w = fb.width() - 8;
      body.h = max_y - 4 - body.y;
      tile(fb, body.x, body.y, body.w, body.h, theme::kPanelRadius, theme::kPanelFill, theme::kPanelEdge);
      break;
    }
    for (int i = 0; i < n; i++) drawShape(fb, shapes[i], max_y, backdrop, body);
    fb.resetClip();
  }

  // Physical px to add to a text drawn at logical (lx, ly): text that starts
  // at a tile's left edge (labels drawn at x + 3) gets breathing room from
  // the rounded edge. Boxes are looked up newest first.
  int textPad(int lx, int ly, const CompanionPalette& p) const {
    for (int i = _ops - 1; i >= 0; i--) {
      const Op& o = _op[i];
      if (lx < o.x || lx >= o.x + o.w || ly < o.y || ly >= o.y + o.h) continue;
      const Op* box = &o;
      if (o.outline) {
        if (i == 0 || !encloses(o, _op[i - 1])) return 0;   // a key / field: centred or own padding
        box = &_op[i - 1];
      }
      const SkinRole role = skinRole(box->colour, p);
      const bool boxRole = role == SkinRole::Window || role == SkinRole::Title || role == SkinRole::Popup ||
                           role == SkinRole::Accent;
      if (!boxRole || o.w < kMinBox || o.h < kMinBox) return 0;
      if (role == SkinRole::Window && !o.outline) return 0;   // a clear, not a tile
      return lx - o.x <= 4 ? kTextPad : 0;
    }
    // Text flush with the screen edge inside the home carousel's body panel.
    if (lx <= 2) {
      for (int i = 0; i < _ops; i++) {
        const Op& o = _op[i];
        if (!o.outline && o.x <= 0 && o.y <= 1 && o.w >= kMinBox && o.h >= kMinBox &&
            skinRole(o.colour, p) == SkinRole::Title && (i + 1 >= _ops || !encloses(_op[i + 1], o)) &&
            ly >= o.y + o.h + kBodyGap)
          return kBodyPad;
      }
    }
    return 0;
  }

  // The HMI backdrop (Theme.h gradient, dithered) for the top `rows` rows of
  // a w x full_h screen, so it matches the pages' backdrop and tab bar.
  static void fillBackdrop(uint16_t* px, int w, int rows, int full_h) {
    const theme::Rgb8 a = theme::kBgNear, b = theme::kBgFar;
    for (int y = 0; y < rows; y++) {
      for (int x = 0; x < w; x++) {
        const float t = 0.6f * (float)x / (float)w + 0.4f * (float)y / (float)full_h;
        px[y * w + x] = FrameBuffer565::dither((float)a.r + ((float)b.r - (float)a.r) * t,
                                               (float)a.g + ((float)b.g - (float)a.g) * t,
                                               (float)a.b + ((float)b.b - (float)a.b) * t, x, y);
      }
    }
  }

  // The sprite (RGB332, zoomed) over the composed frame: its background
  // pixels are transparent, everything else (icons, QR, emoji) expands
  // through the LUT.
  static void overlaySprite(uint16_t* dst, const uint8_t* src, int n, uint8_t key, const uint16_t* lut) {
    for (int i = 0; i < n; i++) {
      const uint8_t s = src[i];
      if (s != key) dst[i] = lut[s];
    }
  }

 private:
  struct Op {
    int16_t x, y, w, h;   // logical
    uint16_t colour;      // the UIColor value set when drawn
    bool outline;         // drawRect
  };

  void add(bool outline, int x, int y, int w, int h, uint16_t colour) {
    if (w <= 0 || h <= 0 || _ops >= kMaxOps) return;
    Op& o = _op[_ops++];
    o.x = (int16_t)x;
    o.y = (int16_t)y;
    o.w = (int16_t)w;
    o.h = (int16_t)h;
    o.colour = colour;
    o.outline = outline;
  }

  // A rect drawn right after a fill, around it (within 2 px): its edge.
  static bool encloses(const Op& r, const Op& f) {
    if (!r.outline) return false;
    const int l = f.x - r.x, t = f.y - r.y;
    const int rr = (r.x + r.w) - (f.x + f.w), b = (r.y + r.h) - (f.y + f.h);
    return l >= 0 && t >= 0 && rr >= 0 && b >= 0 && l <= 2 && t <= 2 && rr <= 2 && b <= 2;
  }

  // Small marks (dots, battery, dividers) in theme colours.
  static uint16_t inkColour(uint16_t c, const CompanionPalette& p) {
    switch (skinRole(c, p)) {
      case SkinRole::Title:
      case SkinRole::Popup: return theme::kDivider;
      case SkinRole::Window: return theme::kBg;
      default: return skinTextColour(c, p);
    }
  }

  struct Body {
    bool on = false;
    int x = 0, y = 0, w = 0, h = 0;   // physical
    bool inside(int px, int py) const {
      const int r = theme::kPanelRadius;
      return on && px >= x + 1 && px < x + w - 1 && py >= y + 1 && py < y + h - 1 &&
             !((px < x + r || px >= x + w - r) && (py < y + r || py >= y + h - r));
    }
  };

  static void restore(FrameBuffer565& fb, int x, int y, int w, int h, int max_y, const uint16_t* backdrop,
                      const Body& body) {
    const int W = fb.width();
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > W ? W : x + w, y1 = y + h > max_y ? max_y : y + h;
    if (y1 > fb.height()) y1 = fb.height();
#if HMI_CYD
    if (backdrop == nullptr) {
      // No backdrop buffer: the gradient itself, then the body panel's fill.
      drawBackdrop(fb, x0, y0, x1 - x0, y1 - y0, nullptr, 0);
      if (body.on)
        for (int yy = y0; yy < y1; yy++)
          for (int xx = x0; xx < x1; xx++)
            if (body.inside(xx, yy)) fb.pixel(xx, yy, theme::kPanelFill);
      return;
    }
#endif
    if (backdrop == nullptr && !body.on) {
      fb.fillRect(x0, y0, x1 - x0, y1 - y0, theme::kBg);
      return;
    }
    for (int yy = y0; yy < y1; yy++)
      for (int xx = x0; xx < x1; xx++)
        fb.pixel(xx, yy, body.inside(xx, yy) ? theme::kPanelFill : backdrop ? backdrop[yy * W + xx] : theme::kBg);
  }

  static void tile(FrameBuffer565& fb, int x, int y, int w, int h, int r, uint16_t fill, uint16_t edge) {
    fb.fillRoundRect(x, y, w, h, r, fill);
    fb.strokeRoundRect(x, y, w, h, r, edge);
  }

  static int radius(int h, int max_r) {
    const int r = h / 3;
    return r < max_r ? r : max_r;
  }

  static void drawShape(FrameBuffer565& fb, const SkinShape& s, int max_y, const uint16_t* backdrop,
                        const Body& body) {
    const int X = s.x * kZoom, Y = s.y * kZoom, W = s.w * kZoom, H = s.h * kZoom;
    switch (s.kind) {
      case SkinKind::Erase:
        restore(fb, X, Y, W, H, max_y, backdrop, body);
        return;
      case SkinKind::Plain:
        fb.fillRect(X, Y, W, H, s.colour);
        return;
      case SkinKind::PlainStroke:
        fb.fillRect(X, Y, W, kZoom, s.colour);
        fb.fillRect(X, Y + H - kZoom, W, kZoom, s.colour);
        fb.fillRect(X, Y, kZoom, H, s.colour);
        fb.fillRect(X + W - kZoom, Y, kZoom, H, s.colour);
        return;
      case SkinKind::Line:
        if (s.h <= 1) fb.hline(X, Y + kZoom / 2, W, theme::kDivider);
        else fb.vline(X + kZoom / 2, Y, H, theme::kDivider);
        return;
      case SkinKind::Header:
        fb.fillRect(X, Y, W, H, theme::kPanelFill);
        fb.hline(X, Y + H - 1, W, theme::kPanelEdge);
        return;
      case SkinKind::Popup:
        tile(fb, X + 1, Y + 1, W - 2, H - 2, theme::kPanelRadius, theme::kPanelFill, theme::kPanelEdge);
        return;
      case SkinKind::Field:
        tile(fb, X + 1, Y + 1, W - 2, H - 2, 4, theme::kOdoFill, theme::kAccentDim);
        return;
      case SkinKind::Key: {
        const int r = radius(H - 2, 5);
        tile(fb, X + 1, Y + 1, W - 2, H - 2, r, s.lit ? theme::kTileOnFill : theme::kTileOffFill,
             s.lit ? theme::kAccent : theme::kTileOffEdge);
        return;
      }
      case SkinKind::Row:
      case SkinKind::Button: {
        const bool row = s.kind == SkinKind::Row;
        const int ix = 2, iy = row ? 1 : 2;
        const int x = X + ix, y = Y + iy, w = W - 2 * ix, h = H - 2 * iy;
        const int r = radius(h, row ? 4 : 6);
        if (!s.enabled) {
          restore(fb, X, Y, W, H, max_y, backdrop, body);
          fb.strokeRoundRect(x, y, w, h, r, theme::kDivider);
        } else if (s.lit) {
          tile(fb, x, y, w, h, r, theme::kTileOnFill, theme::kAccent);
        } else if (row) {
          tile(fb, x, y, w, h, r, theme::kPanelFill, theme::kDivider);
        } else {
          tile(fb, x, y, w, h, r, theme::kTileOffFill, theme::kTileOffEdge);
        }
        return;
      }
    }
  }

  Op _op[kMaxOps];
  int _ops;
};

}  // namespace freenove_ui
