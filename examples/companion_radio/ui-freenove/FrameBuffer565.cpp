// RGB565 framebuffer with anti-aliased primitives (see FrameBuffer565.h).
// Own implementation (MIT, MeshCore).
//
// Speed notes (ESP32-S3: single-precision FPU, PSRAM behind a cache): float
// maths only, rows written left to right, and AA shapes evaluate only the
// pixels within a pixel of an edge (interiors are plain spans).

#include "HmiProfile.h"
// CYD: these loops run once per band on a 240 MHz ESP32 without PSRAM;
// the default -Os costs about 2x here.
#if HMI_CYD
#pragma GCC optimize("O2")
#endif

#include "FrameBuffer565.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace {

inline int clampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline unsigned covAlpha(float cov, unsigned alpha) {
  if (cov <= 0.0f) return 0;
  if (cov >= 1.0f) return alpha;
  return (unsigned)(cov * (float)alpha + 0.5f);
}

// Endpoints beyond this are clipped first (keeps the stepping in int range).
const float kFar = 8192.0f;

// Liang-Barsky clip of a segment to [lo, hi] boxes; false when outside.
bool clipSegment(float& x0, float& y0, float& x1, float& y1, float lox, float loy, float hix, float hiy) {
  float t0 = 0.0f, t1 = 1.0f;
  const float dx = x1 - x0, dy = y1 - y0;
  const float p[4] = {-dx, dx, -dy, dy};
  const float q[4] = {x0 - lox, hix - x0, y0 - loy, hiy - y0};
  for (int k = 0; k < 4; k++) {
    if (p[k] == 0.0f) {
      if (q[k] < 0.0f) return false;
      continue;
    }
    const float r = q[k] / p[k];
    if (p[k] < 0.0f) {
      if (r > t1) return false;
      if (r > t0) t0 = r;
    } else {
      if (r < t0) return false;
      if (r < t1) t1 = r;
    }
  }
  const float ax = x0 + t0 * dx, ay = y0 + t0 * dy;
  x1 = x0 + t1 * dx;
  y1 = y0 + t1 * dy;
  x0 = ax;
  y0 = ay;
  return true;
}

const uint8_t kBayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

}  // namespace

// ---------------------------------------------------------------------------

FrameBuffer565::FrameBuffer565(uint16_t* pixels, int w, int h)
  : _px(pixels), _buf(pixels), _w(w > 0 ? w : 0), _h(h > 0 ? h : 0) {
  if (_px == nullptr) _w = _h = 0;
  if (_w > 0x7FFF) _w = 0x7FFF;
  if (_h > kMaxFbRows) _h = kMaxFbRows;
  _y0 = 0;
  _rows = _h;
  resetClip();
  _push.setAll(_w, _h);
  _obj.reset();
}

FrameBuffer565::FrameBuffer565(uint16_t* strip, int w, int h, int y0, int rows)
  : _px(strip), _buf(strip), _w(w > 0 ? w : 0), _h(h > 0 ? h : 0), _y0(0), _rows(0) {
  if (_buf == nullptr || rows <= 0) _w = _h = 0;
  if (_w > 0x7FFF) _w = 0x7FFF;
  if (_h > kMaxFbRows) _h = kMaxFbRows;
  setBand(y0, rows);
  _push.setAll(_w, _h);
}

void FrameBuffer565::setBand(int y0, int rows) {
  _y0 = clampI(y0, 0, _h);
  _rows = clampI(rows, 0, _h - _y0);
  // Frame coordinates index the band through _px: only rows _y0.._y0+_rows-1
  // are ever addressed (the clip never leaves them). Integer arithmetic:
  // the pointer itself would point before the buffer.
  _px = (uint16_t*)((uintptr_t)_buf - (uintptr_t)((long)_y0 * _w) * sizeof(uint16_t));
  resetClip();
  _obj.reset();
}

uint16_t FrameBuffer565::get(int x, int y) const {
  if (x < 0 || y < _y0 || x >= _w || y >= _y0 + _rows) return 0;
  return _px[y * _w + x];
}

uint16_t FrameBuffer565::mix(uint16_t d, uint16_t s, unsigned a) {
  if (a >= 255) return s;
  if (a == 0) return d;
#if HMI_CYD
  // CYD: 5-bit alpha on the spread 0x07E0F81F form, two multiplies and no
  // divides (the exact form below costs three divisions per pixel).
  const uint32_t a5 = (a * 33u) >> 8;
  const uint32_t fg = ((uint32_t)s | ((uint32_t)s << 16)) & 0x07E0F81Fu;
  const uint32_t bg = ((uint32_t)d | ((uint32_t)d << 16)) & 0x07E0F81Fu;
  const uint32_t m = ((fg * a5 + bg * (32u - a5)) >> 5) & 0x07E0F81Fu;
  return (uint16_t)(m | (m >> 16));
#endif
  const int ia = (int)a;
  const int dr = (d >> 11) & 31, dg = (d >> 5) & 63, db = d & 31;
  const int sr = (s >> 11) & 31, sg = (s >> 5) & 63, sb = s & 31;
  const int r = dr + ((sr - dr) * ia + (sr >= dr ? 127 : -127)) / 255;
  const int g = dg + ((sg - dg) * ia + (sg >= dg ? 127 : -127)) / 255;
  const int b = db + ((sb - db) * ia + (sb >= db ? 127 : -127)) / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

uint16_t FrameBuffer565::dither(float r, float g, float b, int x, int y) {
  const float t = ((float)kBayer4[y & 3][x & 3] + 0.5f) / 16.0f;   // 0..1
  const int r5 = clampI((int)(clampF(r, 0, 255) * 31.0f / 255.0f + t), 0, 31);
  const int g6 = clampI((int)(clampF(g, 0, 255) * 63.0f / 255.0f + t), 0, 63);
  const int b5 = clampI((int)(clampF(b, 0, 255) * 31.0f / 255.0f + t), 0, 31);
  return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

void FrameBuffer565::setClip(int x, int y, int w, int h) {
  const int top = _y0, bottom = _y0 + _rows - 1;   // the band (full frame: 0.._h-1)
  _cx0 = clampI(x, 0, _w);
  _cy0 = clampI(y, top, bottom + 1);
  _cx1 = clampI((int)((long)x + w - 1), -1, _w - 1);
  _cy1 = clampI((int)((long)y + h - 1), top - 1, bottom);
}

void FrameBuffer565::resetClip() {
  _cx0 = 0;
  _cy0 = _y0;
  _cx1 = _w - 1;
  _cy1 = _y0 + _rows - 1;
}

bool FrameBuffer565::clipRow(int y, int& a, int& b) const {
  if (y < _cy0 || y > _cy1) return false;
  if (a < _cx0) a = _cx0;
  if (b > _cx1) b = _cx1;
  return a <= b;
}

void FrameBuffer565::takePushSpans(RowSpans& into) {
  into.merge(_push);
  _push.reset();
}

void FrameBuffer565::markAll() { _push.setAll(_w, _h); }

void FrameBuffer565::clear(uint16_t c) {
  for (long i = 0, n = (long)_w * _rows; i < n; i++) _buf[i] = c;
  _push.setAll(_w, _h);
  _obj.reset();
}

void FrameBuffer565::copyFrom(const uint16_t* src) {
  if (_w <= 0 || _rows <= 0 || src == nullptr) return;
  memcpy(_buf, src + (long)_y0 * _w, (size_t)_w * (size_t)_rows * 2);
  _push.setAll(_w, _h);
  _obj.reset();
}

void FrameBuffer565::eraseTouched(const uint16_t* background, uint16_t c) {
  for (int y = 0; y < _h; y++) {
    if (!_obj.has(y)) continue;
    const int a = _obj.x0[y], b = _obj.x1[y];
    uint16_t* row = _px + y * _w;
    if (background != nullptr) {
      memcpy(row + a, background + y * _w + a, (size_t)(b - a + 1) * 2);
    } else {
      for (int x = a; x <= b; x++) row[x] = c;
    }
    _push.add(y, a, b);
  }
  _obj.reset();
}

void FrameBuffer565::put(int x, int y, uint16_t c) {
  if (x < _cx0 || x > _cx1 || y < _cy0 || y > _cy1) return;
  _px[y * _w + x] = c;
  touch(y, x, x);
}

void FrameBuffer565::putBlend(int x, int y, uint16_t c, unsigned a) {
  if (a == 0 || x < _cx0 || x > _cx1 || y < _cy0 || y > _cy1) return;
  uint16_t& p = _px[y * _w + x];
  p = mix(p, c, a);
  touch(y, x, x);
}

void FrameBuffer565::pixel(int x, int y, uint16_t c) { put(x, y, c); }

void FrameBuffer565::blendPixel(int x, int y, uint16_t c, unsigned alpha) { putBlend(x, y, c, alpha); }

void FrameBuffer565::hline(int x, int y, int w, uint16_t c) {
  if (w <= 0) return;
  const long long e = (long long)x + w - 1;
  int a = x, b = e > _w ? _w : (int)e;
  if (!clipRow(y, a, b)) return;
  uint16_t* row = _px + y * _w;
  for (int i = a; i <= b; i++) row[i] = c;
  touch(y, a, b);
}

void FrameBuffer565::vline(int x, int y, int h, uint16_t c) {
  if (h <= 0 || x < _cx0 || x > _cx1) return;
  const int y0 = y > _cy0 ? y : _cy0;
  const long long ye = (long long)y + h - 1;
  const int y1 = ye < _cy1 ? (int)ye : _cy1;
  for (int yy = y0; yy <= y1; yy++) {
    _px[yy * _w + x] = c;
    touch(yy, x, x);
  }
}

void FrameBuffer565::fillRect(int x, int y, int w, int h, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  const int y0 = y > _cy0 ? y : _cy0;
  const long long ye = (long long)y + h - 1;
  const int y1 = ye < _cy1 ? (int)ye : _cy1;
  for (int yy = y0; yy <= y1; yy++) hline(x, yy, w, c);
}

void FrameBuffer565::fillRectAlpha(int x, int y, int w, int h, uint16_t c, unsigned alpha) {
  if (w <= 0 || h <= 0 || alpha == 0) return;
  const int y0 = y > _cy0 ? y : _cy0;
  const long long ye = (long long)y + h - 1;
  const int y1 = ye < _cy1 ? (int)ye : _cy1;
  for (int yy = y0; yy <= y1; yy++) {
    const long long e = (long long)x + w - 1;
    int a = x, b = e > _w ? _w : (int)e;
    if (!clipRow(yy, a, b)) continue;
    uint16_t* row = _px + yy * _w;
    for (int i = a; i <= b; i++) row[i] = mix(row[i], c, alpha);
    touch(yy, a, b);
  }
}

void FrameBuffer565::rect(int x, int y, int w, int h, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  hline(x, y, w, c);
  if (h > 1) hline(x, y + h - 1, w, c);
  if (h > 2) {
    vline(x, y + 1, h - 2, c);
    if (w > 1) vline(x + w - 1, y + 1, h - 2, c);
  }
}

void FrameBuffer565::line(int x0, int y0, int x1, int y1, uint16_t c) { dashedLine(x0, y0, x1, y1, c, 1, 0, 0); }

void FrameBuffer565::dashedLine(int x0, int y0, int x1, int y1, uint16_t c, int on, int off, int phase) {
  if (_w <= 0 || _h <= 0 || on <= 0) return;
  if (off < 0) off = 0;
  const int period = on + off;
  float fx0 = (float)x0, fy0 = (float)y0, fx1 = (float)x1, fy1 = (float)y1;
  int skip = 0;
  if (fabsf(fx0) > kFar || fabsf(fy0) > kFar || fabsf(fx1) > kFar || fabsf(fy1) > kFar) {
    const float sx = fx0, sy = fy0;
    if (!clipSegment(fx0, fy0, fx1, fy1, -64.0f, -64.0f, (float)_w + 64.0f, (float)_h + 64.0f)) return;
    // Keep the dash pattern anchored to the original start.
    const float steps = fmaxf(fabsf(fx0 - sx), fabsf(fy0 - sy));
    skip = (int)fmodf(floorf(steps + 0.5f), (float)period);
  }
  int ax = (int)floorf(fx0 + 0.5f), ay = (int)floorf(fy0 + 0.5f);
  const int bx = (int)floorf(fx1 + 0.5f), by = (int)floorf(fy1 + 0.5f);
  const int dx = bx > ax ? bx - ax : ax - bx, dy = by > ay ? by - ay : ay - by;
  const int sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1;
  const bool xMajor = dx >= dy;
  const int n = xMajor ? dx : dy;
  int err = (xMajor ? dx : dy) / 2;
  int dash = (((0 + skip - phase) % period) + period) % period;
  for (int i = 0; i <= n; i++) {
    if (dash < on) put(ax, ay, c);
    if (++dash >= period) dash = 0;
    if (xMajor) {
      ax += sx;
      err -= dy;
      if (err < 0) {
        ay += sy;
        err += dx;
      }
    } else {
      ay += sy;
      err -= dx;
      if (err < 0) {
        ax += sx;
        err += dy;
      }
    }
  }
}

// Capsule rasterisation: per row, the x-range within (half width + 1) of the
// segment, then the exact distance per pixel.
void FrameBuffer565::aaLine(float x0, float y0, float x1, float y1, float width, uint16_t c, unsigned alpha) {
  if (_w <= 0 || _h <= 0 || width <= 0.0f || alpha == 0) return;
  if (fabsf(x0) > kFar || fabsf(y0) > kFar || fabsf(x1) > kFar || fabsf(y1) > kFar) {
    if (!clipSegment(x0, y0, x1, y1, -64.0f, -64.0f, (float)_w + 64.0f, (float)_h + 64.0f)) return;
  }
  const float R = width * 0.5f;
  const float M = R + 1.0f;   // evaluation margin
  const float dx = x1 - x0, dy = y1 - y0;
  const float len2 = dx * dx + dy * dy;
  const float len = sqrtf(len2);
  const int ya = clampI((int)floorf(fminf(y0, y1) - M), _cy0, _cy1 + 1);
  const int yb = clampI((int)ceilf(fmaxf(y0, y1) + M), _cy0 - 1, _cy1);
  for (int y = ya; y <= yb; y++) {
    const float py = (float)y;
    // Conservative x-range: the segment's x at this row +- the margin over
    // the line's slope, joined with the cap discs.
    float lo = 1e9f, hi = -1e9f;
    for (int e = 0; e < 2; e++) {
      const float ex = e ? x1 : x0, ey = e ? y1 : y0;
      const float ddy = py - ey;
      if (fabsf(ddy) <= M) {
        const float h = sqrtf(M * M - ddy * ddy);
        lo = fminf(lo, ex - h);
        hi = fmaxf(hi, ex + h);
      }
    }
    if (len > 1e-3f && fabsf(dy) > 1e-3f) {
      // Band |cross| <= M*len, with t in [0, len2]; x where the centre line
      // crosses this row, widened by M / |sin|.
      const float t = (py - y0) / dy;
      if (t >= -M / fabsf(dy) && t <= 1.0f + M / fabsf(dy)) {
        const float xc = x0 + t * dx;
        const float wv = M * len / fabsf(dy);
        const float tc = clampF(t, 0.0f, 1.0f);
        const float xcc = x0 + tc * dx;
        lo = fminf(lo, fminf(xc, xcc) - wv);
        hi = fmaxf(hi, fmaxf(xc, xcc) + wv);
      }
    } else if (len > 1e-3f && fabsf(py - y0) <= M) {
      lo = fminf(lo, fminf(x0, x1) - M);
      hi = fmaxf(hi, fmaxf(x0, x1) + M);
    }
    if (lo > hi) continue;
    int a = (int)floorf(lo), b = (int)ceilf(hi);
    if (!clipRow(y, a, b)) continue;
    uint16_t* row = _px + y * _w;
    int first = -1, last = -1;
    for (int x = a; x <= b; x++) {
      const float px = (float)x;
      float t = len2 > 1e-6f ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0.0f;
      t = clampF(t, 0.0f, 1.0f);
      const float qx = x0 + t * dx - px, qy = y0 + t * dy - py;
      const float d = sqrtf(qx * qx + qy * qy);
      const unsigned a8 = covAlpha(R + 0.5f - d, alpha);
      if (a8 == 0) continue;
      row[x] = mix(row[x], c, a8);
      if (first < 0) first = x;
      last = x;
    }
    if (first >= 0) touch(y, first, last);
  }
}

void FrameBuffer565::aaRing(float cx, float cy, float r, float thick, uint16_t c, unsigned alpha) {
  aaArc(cx, cy, r, thick, 0.0f, 360.0f, c, alpha);
}

void FrameBuffer565::aaArc(float cx, float cy, float r, float thick, float a0_deg, float a1_deg, uint16_t c,
                           unsigned alpha) {
  if (_w <= 0 || _h <= 0 || r <= 0.0f || thick <= 0.0f || alpha == 0) return;
  if (fabsf(cx) > kFar || fabsf(cy) > kFar || r > kFar) return;
  const float ri = r - thick;                 // inner edge (<= 0: disc)
  float span = a1_deg - a0_deg;
  const bool full = span >= 360.0f || span <= -360.0f;
  if (!full) {
    span = fmodf(span, 360.0f);
    if (span < 0) span += 360.0f;
  }
  float a0 = fmodf(a0_deg, 360.0f);
  if (a0 < 0) a0 += 360.0f;
  const float kDeg = 180.0f / 3.14159265f;
  const float ro = r + 1.0f, rin = ri - 1.0f;
  const int ya = clampI((int)floorf(cy - ro), _cy0, _cy1 + 1);
  const int yb = clampI((int)ceilf(cy + ro), _cy0 - 1, _cy1);
  for (int y = ya; y <= yb; y++) {
    const float dy = (float)y - cy;
    if (fabsf(dy) > ro) continue;
    const float xo = sqrtf(ro * ro - dy * dy);
    const float xi = (rin > 0.0f && fabsf(dy) < rin) ? sqrtf(rin * rin - dy * dy) : -1.0f;
    float spans[2][2];
    int ns = 0;
    if (xi < 0.0f) {
      spans[ns][0] = cx - xo; spans[ns][1] = cx + xo; ns++;
    } else {
      spans[ns][0] = cx - xo; spans[ns][1] = cx - xi; ns++;
      spans[ns][0] = cx + xi; spans[ns][1] = cx + xo; ns++;
    }
    uint16_t* row = _px + y * _w;
    for (int s = 0; s < ns; s++) {
      int a = (int)floorf(spans[s][0]), b = (int)ceilf(spans[s][1]);
      if (!clipRow(y, a, b)) continue;
      int first = -1, last = -1;
      for (int x = a; x <= b; x++) {
        const float dx = (float)x - cx;
        const float d = sqrtf(dx * dx + dy * dy);
        float cov = clampF(r + 0.5f - d, 0.0f, 1.0f);
        if (ri > 0.0f) cov *= clampF(d - (ri - 0.5f), 0.0f, 1.0f);
        if (cov <= 0.0f) continue;
        if (!full) {
          float ang = atan2f(dy, dx) * kDeg - a0;   // clockwise on screen (y down)
          while (ang < 0) ang += 360.0f;
          while (ang >= 360.0f) ang -= 360.0f;
          // Distance (px along the circle) inside the nearer edge; negative
          // when outside the arc.
          float inside;
          if (ang <= span) inside = fminf(ang, span - ang);
          else inside = -fminf(ang - span, 360.0f - ang);
          cov *= clampF(inside / kDeg * d + 0.5f, 0.0f, 1.0f);
          if (cov <= 0.0f) continue;
        }
        row[x] = mix(row[x], c, covAlpha(cov, alpha));
        if (first < 0) first = x;
        last = x;
      }
      if (first >= 0) touch(y, first, last);
    }
  }
}

// One corner square: (cx, cy) is the corner's arc centre (pixel centre
// coordinates), dx/dy = -1 or +1 the direction away from the rectangle's
// middle. Pixels of the r x r square beyond the centre are evaluated.
void FrameBuffer565::cornerFill(int cx, int cy, int dx, int dy, int r, uint16_t c, unsigned alpha, bool stroke) {
  const float R = (float)r;
  for (int j = 0; j < r; j++) {
    const int y = cy + dy * (j + 1);
    for (int i = 0; i < r; i++) {
      const int x = cx + dx * (i + 1);
      // The rectangle's edge is the pixel boundary, so the arc centre sits
      // half a pixel off this pixel grid: (i + 0.5, j + 0.5) from it.
      const float fx = (float)i + 0.5f, fy = (float)j + 0.5f;
      const float sd = sqrtf(fx * fx + fy * fy) - R;   // to the edge, + outside
      float cov;
      if (stroke) cov = clampF(1.0f - fabsf(sd + 0.5f), 0.0f, 1.0f);
      else cov = clampF(0.5f - sd, 0.0f, 1.0f);
      const unsigned a8 = covAlpha(cov, alpha);
      if (a8) putBlend(x, y, c, a8);
    }
  }
}

void FrameBuffer565::fillRoundRect(int x, int y, int w, int h, int r, uint16_t c, unsigned alpha) {
  if (w <= 0 || h <= 0 || alpha == 0) return;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  if (r < 0) r = 0;
  // Middle band, then the strips beside the corners, then the corners.
  if (alpha >= 255) {
    fillRect(x, y + r, w, h - 2 * r, c);
    fillRect(x + r, y, w - 2 * r, r, c);
    fillRect(x + r, y + h - r, w - 2 * r, r, c);
  } else {
    fillRectAlpha(x, y + r, w, h - 2 * r, c, alpha);
    fillRectAlpha(x + r, y, w - 2 * r, r, c, alpha);
    fillRectAlpha(x + r, y + h - r, w - 2 * r, r, c, alpha);
  }
  if (r == 0) return;
  cornerFill(x + r, y + r, -1, -1, r, c, alpha, false);
  cornerFill(x + w - 1 - r, y + r, 1, -1, r, c, alpha, false);
  cornerFill(x + r, y + h - 1 - r, -1, 1, r, c, alpha, false);
  cornerFill(x + w - 1 - r, y + h - 1 - r, 1, 1, r, c, alpha, false);
}

void FrameBuffer565::strokeRoundRect(int x, int y, int w, int h, int r, uint16_t c, unsigned alpha) {
  if (w <= 0 || h <= 0 || alpha == 0) return;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  if (r < 0) r = 0;
  if (alpha >= 255) {
    hline(x + r, y, w - 2 * r, c);
    hline(x + r, y + h - 1, w - 2 * r, c);
    vline(x, y + r, h - 2 * r, c);
    vline(x + w - 1, y + r, h - 2 * r, c);
  } else {
    fillRectAlpha(x + r, y, w - 2 * r, 1, c, alpha);
    fillRectAlpha(x + r, y + h - 1, w - 2 * r, 1, c, alpha);
    fillRectAlpha(x, y + r, 1, h - 2 * r, c, alpha);
    fillRectAlpha(x + w - 1, y + r, 1, h - 2 * r, c, alpha);
  }
  if (r == 0) return;
  cornerFill(x + r, y + r, -1, -1, r, c, alpha, true);
  cornerFill(x + w - 1 - r, y + r, 1, -1, r, c, alpha, true);
  cornerFill(x + r, y + h - 1 - r, -1, 1, r, c, alpha, true);
  cornerFill(x + w - 1 - r, y + h - 1 - r, 1, 1, r, c, alpha, true);
}

// ---------------------------------------------------------------- text

int FrameBuffer565::textWidth(const char* s, const AAFont& f, int spacing) {
  if (s == nullptr) return 0;
  int q = 0, n = 0, prev = -1;
  for (; *s; s++) {
    const int gi = aaGlyphIndex((unsigned char)*s);
    if (gi < 0 || f.glyphs[gi].adv == 0) continue;
    if (prev >= 0 && f.kernCount) q += aaKern(f, prev, gi);
    q += f.glyphs[gi].adv;
    prev = gi;
    n++;
  }
  return (q + 2) / 4 + (n > 1 ? (n - 1) * spacing : 0);
}

int FrameBuffer565::text(int x, int y, const char* s, uint16_t c, const AAFont& f, Anchor anchor, int spacing) {
  if (s == nullptr) return x;
  int w = 0;
  if (anchor != kLeftTop && anchor != kLeftMiddle) w = textWidth(s, f, spacing);
  int left = x;
  if (anchor == kRightTop || anchor == kRightMiddle) left = x - w;
  else if (anchor == kCentreTop || anchor == kCentreMiddle) left = x - w / 2;
  int top = y;
  if (anchor == kLeftMiddle || anchor == kRightMiddle || anchor == kCentreMiddle)
    top = y - (f.ascent + f.descent) / 2;
  const int end = drawText(left, top, s, c, f, spacing);
  return anchor == kLeftTop || anchor == kLeftMiddle ? end : x;
}

int FrameBuffer565::drawText(int x, int yTop, const char* s, uint16_t c, const AAFont& f, int spacing) {
  long penq = (long)x * 4;
  const int base = yTop + f.ascent;
  int prev = -1;
  for (; *s; s++) {
    const int gi = aaGlyphIndex((unsigned char)*s);
    if (gi < 0) continue;
    const AAGlyph& g = f.glyphs[gi];
    if (g.adv == 0) continue;
    // The running pen is rounded once per glyph position (never the
    // advances themselves), so spacing does not drift.
    if (prev >= 0) penq += (long)spacing * 4 + (f.kernCount ? aaKern(f, prev, gi) : 0);
    prev = gi;
    const long gx = (penq + 2) / 4 + g.x;
    const int gy = base + g.y;
    if (g.w > 0 && gx + g.w > _cx0 && gx <= _cx1 && gy + g.h > _cy0 && gy <= _cy1) {
      const uint8_t* bits = f.bitmap + g.offset;
      int idx = 0;
      for (int r = 0; r < g.h; r++) {
        const int yy = gy + r;
        int first = -1, last = -1;
        for (int col = 0; col < g.w; col++, idx++) {
          const uint8_t byte = bits[idx >> 1];
          const unsigned a4 = (idx & 1) ? (byte & 0x0F) : (byte >> 4);
          if (a4 == 0) continue;
          const long xx = gx + col;
          if (xx < _cx0 || xx > _cx1 || yy < _cy0 || yy > _cy1) continue;
          uint16_t& p = _px[yy * _w + xx];
          p = mix(p, c, a4 * 17u);
          if (first < 0) first = (int)xx;
          last = (int)xx;
        }
        if (first >= 0) touch(yy, first, last);
      }
    }
    penq += g.adv;
  }
  const long end = (penq + 2) / 4;
  return end > 0x7FFFFFF ? 0x7FFFFFF : (int)end;
}

// ---------------------------------------------------------------- planner

namespace {

// First and last differing pixel of a[lo..hi] vs b[lo..hi]; false if equal.
bool diffExtent16(const uint16_t* a, const uint16_t* b, int lo, int hi, int& first, int& last) {
  int i = lo;
  while (i <= hi && a[i] == b[i]) i++;
  if (i > hi) return false;
  first = i;
  int j = hi;
  while (j > first && a[j] == b[j]) j--;
  last = j;
  return true;
}

}  // namespace

PushPlanner565::PushPlanner565(uint16_t* shadow, int w, int h)
  : _shadow(shadow), _w(w > 0 ? w : 0), _h(h > 0 ? h : 0), _force(true) {
  if (_w > 0x7FFF) _w = 0x7FFF;
  if (_h > kMaxFbRows) _h = kMaxFbRows;
  _cand.reset();
  _prev.reset();
}

void PushPlanner565::take(FrameBuffer565& fb) { fb.takePushSpans(_cand); }

void PushPlanner565::plan(const FrameBuffer565& fb, SpanBands& out, Stats* stats) {
  out.count = 0;
  Stats st = {0, 0, 0, 0};
  const int w = fb.width() < _w ? fb.width() : _w;
  const int h = fb.height() < _h ? fb.height() : _h;
  const uint16_t* px = fb.pixels();
  const bool force = _force;
  _force = false;
  _cand.merge(_prev);
  _prev.reset();
  if (_shadow == nullptr || px == nullptr || w <= 0 || h <= 0) {
    _cand.reset();
    if (stats != nullptr) *stats = st;
    return;
  }
  if (force) {
    memcpy(_shadow, px, (size_t)w * (size_t)h * 2);
    st.candidateRows = st.pushedRows = h;
    _prev.setAll(w, h);
    _cand.reset();
    out.y0[0] = 0;
    out.n[0] = (int16_t)h;
    out.x0[0] = 0;
    out.x1[0] = (int16_t)(w - 1);
    out.count = 1;
    if (stats != nullptr) *stats = st;
    return;
  }
  for (int y = 0; y < h; y++) {
    if (!_cand.has(y)) continue;
    st.candidateRows++;
    int a = _cand.x0[y], b = _cand.x1[y];
    if (a < 0) a = 0;
    if (b > w - 1) b = w - 1;
    if (a > b) continue;
    const uint16_t* row = px + y * w;
    uint16_t* glass = _shadow + y * w;
    int first = 0, last = 0;
    if (!diffExtent16(row, glass, a, b, first, last)) {
      st.comparedBytes += (long)(b - a + 1) * 2;
      st.skippedRows++;
      continue;
    }
    st.comparedBytes += (long)((first - a) + (b - last) + 2) * 2;
    memcpy(glass + first, row + first, (size_t)(last - first + 1) * 2);
    _prev.add(y, first, last);
    st.pushedRows++;
  }
  _cand.reset();
  planSpanBands(_prev, h, kBandAlign, kWindowCostBytes, out);
  if (stats != nullptr) *stats = st;
}
