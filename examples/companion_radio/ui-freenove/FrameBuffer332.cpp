// RGB332 framebuffer engine for the Freenove visual head (see FrameBuffer332.h).
// Own implementation (MIT, MeshCore). Fonts: FbFonts.cpp.
//
// Speed notes (ESP32-S3: single-precision FPU only, PSRAM behind a cache):
// no double-precision maths and no 64-bit division in per-pixel or per-row
// loops; spans are written left to right with one row pointer; glow dots use
// precomputed half-width tables.

#include "FrameBuffer332.h"

#include <math.h>
#include <string.h>

#include "FbFonts.h"

namespace {

typedef long long i64;

inline i64 minI(i64 a, i64 b) { return a < b ? a : b; }
inline i64 maxI(i64 a, i64 b) { return a > b ? a : b; }

// floor(sqrt(v)) for 0 <= v < 2^63. Small values (every on-screen shape) use
// the single-precision FPU; the double path only runs for absurd radii.
i64 isqrtFast(i64 v) {
  if (v <= 0) return 0;
  i64 s;
  if (v < (1LL << 30)) {
    s = (i64)sqrtf((float)v);
  } else {
    s = (i64)sqrt((double)v);
  }
  while (s > 0 && s * s > v) s--;
  while ((s + 1) * (s + 1) <= v) s++;
  return s;
}

inline uint32_t rotl32(uint32_t v, int r) { return (v << r) | (v >> (32 - r)); }

// Whole-number modulo that is never negative.
inline i64 modPos(i64 a, i64 m) {
  const i64 r = a % m;
  return r < 0 ? r + m : r;
}

// Glow dot half-widths per |dy| (isqrt(r^2 - dy^2)), -1 = row outside.
static_assert(FrameBuffer332::kGlowCore == 2 && FrameBuffer332::kGlowMid == 4
                  && FrameBuffer332::kGlowOuter == 6,
              "kGlowHalf matches these radii");
const int8_t kGlowHalf[3][FrameBuffer332::kGlowOuter + 1] = {
  {2, 1, 0, -1, -1, -1, -1},   // r = kGlowCore (2)
  {4, 3, 3, 2, 0, -1, -1},     // r = kGlowMid (4)
  {6, 5, 5, 5, 4, 3, 0},       // r = kGlowOuter (6)
};

}  // namespace

// ---------------------------------------------------------------------------
// RowSpans

void RowSpans::reset() {
  for (int y = 0; y < kMaxFbRows; y++) {
    x0[y] = 0x7FFF;
    x1[y] = -1;
  }
}

void RowSpans::setAll(int w, int h) {
  if (h > kMaxFbRows) h = kMaxFbRows;
  for (int y = 0; y < kMaxFbRows; y++) {
    x0[y] = y < h && w > 0 ? 0 : 0x7FFF;
    x1[y] = y < h && w > 0 ? (int16_t)(w - 1) : -1;
  }
}

void RowSpans::merge(const RowSpans& o) {
  for (int y = 0; y < kMaxFbRows; y++) {
    if (o.x0[y] < x0[y]) x0[y] = o.x0[y];
    if (o.x1[y] > x1[y]) x1[y] = o.x1[y];
  }
}

int RowSpans::rows() const {
  int n = 0;
  for (int y = 0; y < kMaxFbRows; y++) n += has(y) ? 1 : 0;
  return n;
}

// ---------------------------------------------------------------------------
// FrameBuffer332

FrameBuffer332::FrameBuffer332(uint8_t* pixels, int w, int h)
  : _px(pixels), _w(w > 0 ? w : 0), _h(h > 0 ? h : 0) {
  if (_px == nullptr) _w = _h = 0;
  if (_w > 0x7FFF) _w = 0x7FFF;          // spans are int16
  if (_h > kMaxFbRows) _h = kMaxFbRows;  // span/hash tables
  // Contents unknown until drawn: everything counts as changed.
  _push.setAll(_w, _h);
  _obj.reset();
}

uint8_t FrameBuffer332::rgb(uint8_t r, uint8_t g, uint8_t b) {
  const unsigned r3 = (r * 7u + 127u) / 255u;
  const unsigned g3 = (g * 7u + 127u) / 255u;
  const unsigned b2 = (b * 3u + 127u) / 255u;
  return (uint8_t)((r3 << 5) | (g3 << 2) | b2);
}

uint8_t FrameBuffer332::glowRamp(uint8_t c, int level) {
  if (level <= 0) return c;
  // Inner ring ~60%, outer ring ~30% of each channel, rounded.
  const unsigned num = level == 1 ? 6u : 3u;
  const unsigned r3 = (c >> 5) & 7u, g3 = (c >> 2) & 7u, b2 = c & 3u;
  unsigned r = (r3 * num + 5u) / 10u;
  unsigned g = (g3 * num + 5u) / 10u;
  unsigned b = (b2 * num + 5u) / 10u;
  if (c != 0 && r == 0 && g == 0 && b == 0) {
    // Keep the dominant channel at its lowest lit level.
    if (r3 * 3 >= g3 * 3 && r3 * 3 >= b2 * 7) r = 1;
    else if (g3 * 3 >= b2 * 7) g = 1;
    else b = 1;
  }
  return (uint8_t)((r << 5) | (g << 2) | b);
}

uint8_t FrameBuffer332::maxBlend(uint8_t a, uint8_t b) {
  const uint8_t r = (a & 0xE0) > (b & 0xE0) ? (a & 0xE0) : (b & 0xE0);
  const uint8_t g = (a & 0x1C) > (b & 0x1C) ? (a & 0x1C) : (b & 0x1C);
  const uint8_t bl = (a & 0x03) > (b & 0x03) ? (a & 0x03) : (b & 0x03);
  return (uint8_t)(r | g | bl);
}

uint8_t FrameBuffer332::get(int x, int y) const {
  if (x < 0 || y < 0 || x >= _w || y >= _h) return 0;
  return _px[y * _w + x];
}

void FrameBuffer332::takePushSpans(RowSpans& into) {
  into.merge(_push);
  _push.reset();
}

void FrameBuffer332::markAll() { _push.setAll(_w, _h); }

void FrameBuffer332::clear(uint8_t c) {
  if (_w <= 0 || _h <= 0) return;
  memset(_px, c, (size_t)_w * (size_t)_h);
  _push.setAll(_w, _h);
  _obj.reset();
}

void FrameBuffer332::copyFrom(const uint8_t* src) {
  if (_w <= 0 || _h <= 0 || src == nullptr) return;
  memcpy(_px, src, (size_t)_w * (size_t)_h);
  _push.setAll(_w, _h);
  _obj.reset();
}

void FrameBuffer332::eraseTouched(const uint8_t* background, uint8_t c) {
  for (int y = 0; y < _h; y++) {
    if (!_obj.has(y)) continue;
    const int a = _obj.x0[y], b = _obj.x1[y];
    uint8_t* row = _px + y * _w;
    if (background != nullptr) {
      memcpy(row + a, background + y * _w + a, (size_t)(b - a + 1));
    } else {
      memset(row + a, c, (size_t)(b - a + 1));
    }
    _push.add(y, a, b);
  }
  _obj.reset();
}

void FrameBuffer332::pixel(int x, int y, uint8_t c) {
  if (x < 0 || y < 0 || x >= _w || y >= _h) return;
  _px[y * _w + x] = c;
  touch(y, x, x);
}

void FrameBuffer332::hline(int x, int y, int w, uint8_t c) {
  if (w <= 0 || y < 0 || y >= _h) return;
  const i64 x0 = maxI(x, 0);
  const i64 x1 = minI((i64)x + w, _w);
  if (x0 >= x1) return;
  memset(&_px[y * _w + (int)x0], c, (size_t)(x1 - x0));
  touch(y, (int)x0, (int)x1 - 1);
}

void FrameBuffer332::vline(int x, int y, int h, uint8_t c) {
  if (h <= 0 || x < 0 || x >= _w) return;
  const int y0 = (int)maxI(y, 0);
  const int y1 = (int)minI((i64)y + h, _h);
  if (y0 >= y1) return;
  uint8_t* p = _px + y0 * _w + x;
  for (int yy = y0; yy < y1; yy++, p += _w) {
    *p = c;
    touch(yy, x, x);
  }
}

void FrameBuffer332::fillRect(int x, int y, int w, int h, uint8_t c) {
  if (w <= 0 || h <= 0) return;
  const i64 y0 = maxI(y, 0);
  const i64 y1 = minI((i64)y + h, _h);
  for (i64 yy = y0; yy < y1; yy++) hline(x, (int)yy, w, c);
}

void FrameBuffer332::rect(int x, int y, int w, int h, uint8_t c) {
  if (w <= 0 || h <= 0) return;
  hline(x, y, w, c);
  if (h > 1) hline(x, (int)minI((i64)y + h - 1, 0x7FFFFFFF), w, c);
  if (h > 2 && (i64)y + 1 <= 0x7FFFFFFF) vline(x, y + 1, h - 2, c);
  if (w > 1 && h > 2 && (i64)y + 1 <= 0x7FFFFFFF)
    vline((int)minI((i64)x + w - 1, 0x7FFFFFFF), y + 1, h - 2, c);
}

void FrameBuffer332::line(int x0, int y0, int x1, int y1, uint8_t c) {
  dashedLine(x0, y0, x1, y1, c, 1, 0, 0);
}

// Lines step along the major axis; step i (0..N) sits at
// major = start + i, minor = start + round(i * dminor / N). Only the steps
// whose pixels can land on the buffer are visited (clipped on the step
// parameter), so far-off-screen endpoints cost nothing extra. Endpoints are
// first clipped to a guard box when they are too large for exact 64-bit
// stepping, with the dash pattern re-anchored to the original start. Inside
// the loop the minor offset and dash counter advance incrementally (no
// division per pixel).
void FrameBuffer332::dashedLine(int x0, int y0, int x1, int y1, uint8_t c,
                                int on, int off, int phase) {
  if (_w <= 0 || _h <= 0 || on <= 0) return;
  if (off < 0) off = 0;
  const i64 period = (i64)on + off;
  i64 ph = phase;

  i64 ax = x0, ay = y0, bx = x1, by = y1;
  const i64 kBig = 1 << 20;
  if (ax < -kBig || ax > kBig || ay < -kBig || ay > kBig || bx < -kBig
      || bx > kBig || by < -kBig || by > kBig) {
    // Rare: clip the real segment to a guard box in doubles, then round.
    const double g = 4096.0;
    double t0 = 0.0, t1 = 1.0;
    const double dx = (double)(bx - ax), dy = (double)(by - ay);
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {(double)ax + g, (double)(_w + g) - (double)ax,
                         (double)ay + g, (double)(_h + g) - (double)ay};
    for (int k = 0; k < 4; k++) {
      if (p[k] == 0.0) {
        if (q[k] < 0.0) return;
        continue;
      }
      const double r = q[k] / p[k];
      if (p[k] < 0.0) {
        if (r > t1) return;
        if (r > t0) t0 = r;
      } else {
        if (r < t0) return;
        if (r < t1) t1 = r;
      }
    }
    const double adx = fabs(dx), ady = fabs(dy);
    const double n = adx > ady ? adx : ady;
    // Re-anchor the dash pattern: the new start is step k of the old line.
    const i64 k0 = (i64)floor(t0 * n + 0.5);
    ph -= modPos(k0, period);
    const i64 nax = (i64)floor((double)ax + t0 * dx + 0.5);
    const i64 nay = (i64)floor((double)ay + t0 * dy + 0.5);
    const i64 nbx = (i64)floor((double)ax + t1 * dx + 0.5);
    const i64 nby = (i64)floor((double)ay + t1 * dy + 0.5);
    ax = nax; ay = nay; bx = nbx; by = nby;
  }

  const i64 dx = bx - ax, dy = by - ay;
  const i64 adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  const bool xMajor = adx >= ady;
  const i64 n = xMajor ? adx : ady;
  const i64 dmin = xMajor ? dy : dx;
  const i64 adm = dmin < 0 ? -dmin : dmin;
  const int smaj = (xMajor ? dx : dy) < 0 ? -1 : 1;
  const int smin = dmin < 0 ? -1 : 1;
  const i64 maj0 = xMajor ? ax : ay, min0 = xMajor ? ay : ax;
  const i64 majLim = xMajor ? _w : _h, minLim = xMajor ? _h : _w;

  // Steps whose major coordinate is on the buffer.
  i64 i0 = 0, i1 = n;
  if (smaj > 0) {
    i0 = maxI(i0, -maj0);
    i1 = minI(i1, majLim - 1 - maj0);
  } else {
    i0 = maxI(i0, maj0 - (majLim - 1));
    i1 = minI(i1, maj0);
  }
  if (i0 > i1) return;
  // Narrow by the minor axis (with a step of slack for rounding). Values are
  // bounded by 2^21 here, so single precision is close enough with the slack.
  if (dmin != 0) {
    const float lo = (float)(-1 - min0) * (float)n / (float)dmin;
    const float hi = (float)(minLim - min0) * (float)n / (float)dmin;
    const float a = lo < hi ? lo : hi, b = lo < hi ? hi : lo;
    if (b < (float)i0 - 2.0f || a > (float)i1 + 2.0f) return;
    const i64 na = (i64)floorf(a) - 2, nb = (i64)ceilf(b) + 2;
    if (na > i0) i0 = na;
    if (nb < i1) i1 = nb;
    if (i0 > i1) return;
  } else if (min0 < 0 || min0 >= minLim) {
    return;
  }

  // Minor offset at step i: floor((2 * i * adm + n) / (2 * n)) (round half
  // up); kept as quotient + remainder and advanced by adding 2 * adm.
  const i64 twoN = 2 * (n > 0 ? n : 1);
  const i64 num0 = 2 * i0 * adm + n;
  i64 q = num0 / twoN;
  i64 rem = num0 - q * twoN;
  i64 dash = modPos(i0 - ph, period);
  for (i64 i = i0; i <= i1; i++) {
    if (dash < on) {
      const i64 mj = maj0 + smaj * i;
      const i64 mn = min0 + smin * q;
      if (mn >= 0 && mn < minLim && mj >= 0 && mj < majLim) {
        const int x = (int)(xMajor ? mj : mn);
        const int y = (int)(xMajor ? mn : mj);
        _px[y * _w + x] = c;
        touch(y, x, x);
      }
    }
    if (++dash >= period) dash = 0;
    rem += 2 * adm;
    if (rem >= twoN) {
      rem -= twoN;
      q++;
    }
  }
}

// Pixels with ri^2 < dx^2 + dy^2 <= ro^2 (ri < 0: none excluded), row by row
// over the buffer only, written in contiguous spans; with a sector, only
// pixels whose direction lies between the sector's rays.
struct FrameBuffer332::Sector {
  bool wide;       // span > 180 degrees
  float v0x, v0y, v1x, v1y;

  bool contains(float px, float py) const {
    const float eps = -1e-4f * (fabsf(px) + fabsf(py) + 1.0f);
    if (!wide) {
      // Between the two rays, on the bisector's side (a zero span is one ray).
      return (v0x * py - v0y * px) >= eps && (px * v1y - py * v1x) >= eps
             && (px * (v0x + v1x) + py * (v0y + v1y)) >= eps;
    }
    // Outside only when strictly inside the complementary (< 180) sector.
    return !((v1x * py - v1y * px) > -eps && (px * v0y - py * v0x) > -eps);
  }
};

void FrameBuffer332::ring(i64 cx, i64 cy, i64 ro, i64 ri, uint8_t c,
                          const Sector* sector) {
  if (ro < 0 || _w <= 0 || _h <= 0) return;
  if (cx + ro < 0 || cx - ro >= _w || cy + ro < 0 || cy - ro >= _h) return;
  const i64 ro2 = ro * ro;
  const i64 ri2 = ri >= 0 ? ri * ri : -1;
  const int y0 = (int)maxI(cy - ro, 0), y1 = (int)minI(cy + ro, _h - 1);
  for (int y = y0; y <= y1; y++) {
    const i64 dy = y - cy;
    const i64 dy2 = dy * dy;
    if (dy2 > ro2) continue;
    const i64 xo = isqrtFast(ro2 - dy2);
    const i64 xi = (ri2 >= 0 && dy2 <= ri2) ? isqrtFast(ri2 - dy2) : -1;
    // Two spans [cx - xo, cx - xi - 1] and [cx + xi + 1, cx + xo]; one when
    // nothing is excluded on this row.
    i64 spans[2][2];
    int ns = 0;
    if (xi < 0) {
      spans[ns][0] = cx - xo; spans[ns][1] = cx + xo; ns++;
    } else if (xi < xo) {
      spans[ns][0] = cx - xo; spans[ns][1] = cx - xi - 1; ns++;
      spans[ns][0] = cx + xi + 1; spans[ns][1] = cx + xo; ns++;
    }
    uint8_t* row = _px + y * _w;
    for (int s = 0; s < ns; s++) {
      const i64 sa = maxI(spans[s][0], 0), sb = minI(spans[s][1], _w - 1);
      if (sa > sb) continue;
      const int a = (int)sa, b = (int)sb;
      if (sector == nullptr) {
        memset(row + a, c, (size_t)(b - a + 1));
        touch(y, a, b);
        continue;
      }
      const float fdy = (float)dy;
      int first = -1, last = -1;
      for (int x = a; x <= b; x++) {
        if (!sector->contains((float)(x - cx), fdy)) continue;
        row[x] = c;
        if (first < 0) first = x;
        last = x;
      }
      if (first >= 0) touch(y, first, last);
    }
  }
}

void FrameBuffer332::circle(int cx, int cy, int r, uint8_t c) {
  if (r < 0) return;
  ring(cx, cy, r, (i64)r - 1, c, nullptr);
}

void FrameBuffer332::fillCircle(int cx, int cy, int r, uint8_t c) {
  if (r < 0) return;
  ring(cx, cy, r, -1, c, nullptr);
}

void FrameBuffer332::arc(int cx, int cy, int r, int a0_deg, int a1_deg,
                         int thick, uint8_t c) {
  if (r < 0 || thick <= 0) return;
  const i64 ri = (i64)r - thick;
  const i64 sweep = (i64)a1_deg - a0_deg;
  if (sweep >= 360 || sweep <= -360) {
    ring(cx, cy, r, ri < 0 ? -1 : ri, c, nullptr);
    return;
  }
  const i64 span = modPos(sweep, 360);
  const float kRad = 3.14159265358979f / 180.0f;
  const float a0 = (float)modPos(a0_deg, 360) * kRad;
  const float a1 = a0 + (float)span * kRad;
  Sector s;
  s.wide = span > 180;
  s.v0x = cosf(a0); s.v0y = sinf(a0);
  s.v1x = cosf(a1); s.v1y = sinf(a1);
  ring(cx, cy, r, ri < 0 ? -1 : ri, c, &s);
}

int FrameBuffer332::textWidth(const char* s, uint8_t font) {
  if (s == nullptr) return 0;
  const int adv = font == kFont8x16Id ? 8 : 6;
  return (int)strlen(s) * adv;
}

int FrameBuffer332::text(int x, int y, const char* s, uint8_t c,
                         uint8_t font) {
  if (s == nullptr) return x;
  const bool big = font == kFont8x16Id;
  const int gw = big ? 8 : 6, gh = big ? 16 : 8;
  const uint8_t* data = big ? kFont8x16 : kFont6x8;
  i64 cx = x;
  const bool rowsVisible = !((i64)y + gh <= 0 || y >= _h);
  for (; *s != 0; s++, cx += gw) {
    if (!rowsVisible || cx + gw <= 0) continue;
    if (cx >= _w) {
      cx += (i64)gw * (i64)strlen(s);   // account for the rest, draw nothing
      break;
    }
    int ch = (unsigned char)*s;
    if (ch < kFontFirstChar || ch > kFontLastChar) ch = '?';
    const uint8_t* g = data + (ch - kFontFirstChar) * gh;
    const bool inside = cx >= 0 && cx + gw <= _w;
    for (int r = 0; r < gh; r++) {
      const i64 yy = (i64)y + r;
      const uint8_t bits = g[r];
      if (yy < 0 || yy >= _h || bits == 0) continue;
      uint8_t* row = _px + yy * _w;
      int first = -1, last = -1;
      for (int col = 0; col < gw; col++) {
        if (!(bits & (0x80 >> col))) continue;
        const i64 xx = cx + col;
        if (!inside && (xx < 0 || xx >= _w)) continue;
        row[xx] = c;
        if (first < 0) first = (int)xx;
        last = (int)xx;
      }
      if (first >= 0) touch((int)yy, first, last);
    }
  }
  if (cx > 0x7FFFFFFF) return 0x7FFFFFFF;
  return (int)cx;
}

// One pass per row: outer ring | inner ring | core | inner ring | outer ring,
// left to right, from the half-width tables.
void FrameBuffer332::glowDot(int x, int y, uint8_t c) {
  const int R = kGlowOuter;
  if ((i64)x + R < 0 || (i64)x - R >= _w || (i64)y + R < 0
      || (i64)y - R >= _h) {
    return;
  }
  const uint8_t c1 = glowRamp(c, 1), c2 = glowRamp(c, 2);
  const int ya = (int)maxI((i64)y - R, 0), yb = (int)minI((i64)y + R, _h - 1);
  for (int yy = ya; yy <= yb; yy++) {
    const int ady = yy > y ? yy - y : y - yy;
    const int ho = kGlowHalf[2][ady], hm = kGlowHalf[1][ady],
              hc = kGlowHalf[0][ady];
    if (ho < 0) continue;
    const int a = (int)maxI((i64)x - ho, 0), b = (int)minI((i64)x + ho, _w - 1);
    if (a > b) continue;
    uint8_t* row = _px + yy * _w;
    for (int xx = a; xx <= b; xx++) {
      const int adx = xx > x ? xx - x : x - xx;
      if (adx <= hc) row[xx] = c;
      else if (adx <= hm) row[xx] = maxBlend(row[xx], c1);
      else row[xx] = maxBlend(row[xx], c2);
    }
    touch(yy, a, b);
  }
}

// Murmur3-style mixing of the row's 32-bit words: every byte position
// affects the result, and two changes in one row do not cancel the way they
// can in a plain sum or xor.
uint32_t FrameBuffer332::rowHash(int y) const {
  if (y < 0 || y >= _h) return 0;
  const uint8_t* p = _px + y * _w;
  uint32_t h = 0x9747B28Cu;
  int i = 0;
  for (; i + 4 <= _w; i += 4) {
    uint32_t k;
    memcpy(&k, p + i, 4);
    k *= 0xCC9E2D51u;
    k = rotl32(k, 15);
    k *= 0x1B873593u;
    h ^= k;
    h = rotl32(h, 13);
    h = h * 5u + 0xE6546B64u;
  }
  for (; i < _w; i++) {
    h ^= p[i];
    h *= 0x01000193u;
  }
  h ^= (uint32_t)_w;
  h ^= h >> 16;
  h *= 0x85EBCA6Bu;
  h ^= h >> 13;
  h *= 0xC2B2AE35u;
  h ^= h >> 16;
  return h;
}

void buildLut565(uint16_t lut[256], bool byteswap) {
  for (int c = 0; c < 256; c++) {
    const unsigned r3 = (c >> 5) & 7u, g3 = (c >> 2) & 7u, b2 = c & 3u;
    const unsigned r5 = (r3 << 2) | (r3 >> 1);
    const unsigned g6 = (g3 << 3) | g3;
    const unsigned b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
    const uint16_t v = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    lut[c] = byteswap ? (uint16_t)((v << 8) | (v >> 8)) : v;
  }
}

void diffRows(const FrameBuffer332& fb, uint32_t last_hashes[kMaxFbRows],
              DirtyRows& out) {
  out.count = 0;
  const int h = fb.height() < kMaxFbRows ? fb.height() : kMaxFbRows;
  int runStart = -1;
  for (int y = 0; y < h; y++) {
    const uint32_t hh = fb.rowHash(y);
    const bool changed = hh != last_hashes[y];
    last_hashes[y] = hh;
    if (changed && runStart < 0) runStart = y;
    if (!changed && runStart >= 0) {
      out.y0[out.count] = runStart;
      out.n[out.count] = y - runStart;
      out.count++;
      runStart = -1;
    }
  }
  if (runStart >= 0) {
    out.y0[out.count] = runStart;
    out.n[out.count] = h - runStart;
    out.count++;
  }
}

void planBands(const DirtyRows& runs, int h, int align, int mergeGap,
               DirtyRows& bands) {
  bands.count = 0;
  if (align < 1) align = 1;
  if (h > kMaxFbRows) h = kMaxFbRows;
  for (int i = 0; i < runs.count && i < kMaxFbRows; i++) {
    if (runs.n[i] <= 0) continue;
    int a = runs.y0[i] < 0 ? 0 : runs.y0[i];
    int b = runs.y0[i] + runs.n[i];
    if (b > h) b = h;
    if (a >= b) continue;
    a -= a % align;
    b = (b + align - 1) / align * align;
    if (b > h) b = h;
    if (bands.count > 0) {
      int& pn = bands.n[bands.count - 1];
      const int pend = bands.y0[bands.count - 1] + pn;
      if (a - pend <= mergeGap) {
        if (b > pend) pn = b - bands.y0[bands.count - 1];
        continue;
      }
    }
    bands.y0[bands.count] = a;
    bands.n[bands.count] = b - a;
    bands.count++;
  }
}

long bandBytes(const DirtyRows& bands, int w) {
  long total = 0;
  for (int i = 0; i < bands.count; i++) total += (long)bands.n[i] * w * 2;
  return total;
}

// Groups of `align` rows (the panel's column rule) with the union of their
// spans; neighbouring groups (gaps included) merge while one window costs no
// more than two: rows * width * 2 + windowCost each.
void planSpanBands(const RowSpans& rows, int h, int align, long windowCost,
                   SpanBands& out) {
  out.count = 0;
  if (align < 1) align = 1;
  if (h > kMaxFbRows) h = kMaxFbRows;
  int cy0 = -1, cy1 = 0, ca = 0, cb = 0;   // current band [cy0, cy1), [ca, cb]
  for (int g = 0; g < h; g += align) {
    const int gEnd = g + align < h ? g + align : h;
    int a = 0x7FFF, b = -1;
    for (int y = g; y < gEnd; y++) {
      if (!rows.has(y)) continue;
      if (rows.x0[y] < a) a = rows.x0[y];
      if (rows.x1[y] > b) b = rows.x1[y];
    }
    if (a > b) continue;
    if (cy0 >= 0) {
      const int ma = a < ca ? a : ca, mb = b > cb ? b : cb;
      const long merged = (long)(gEnd - cy0) * (mb - ma + 1) * 2 + windowCost;
      const long separate = (long)(cy1 - cy0) * (cb - ca + 1) * 2
                            + (long)(gEnd - g) * (b - a + 1) * 2
                            + 2 * windowCost;
      if (merged <= separate && out.count < kMaxBands) {
        cy1 = gEnd;
        ca = ma;
        cb = mb;
        continue;
      }
      out.y0[out.count] = (int16_t)cy0;
      out.n[out.count] = (int16_t)(cy1 - cy0);
      out.x0[out.count] = (int16_t)ca;
      out.x1[out.count] = (int16_t)cb;
      out.count++;
    }
    cy0 = g;
    cy1 = gEnd;
    ca = a;
    cb = b;
  }
  if (cy0 >= 0) {
    out.y0[out.count] = (int16_t)cy0;
    out.n[out.count] = (int16_t)(cy1 - cy0);
    out.x0[out.count] = (int16_t)ca;
    out.x1[out.count] = (int16_t)cb;
    out.count++;
  }
}

long spanBandBytes(const SpanBands& bands) {
  long total = 0;
  for (int i = 0; i < bands.count; i++)
    total += (long)bands.n[i] * (bands.x1[i] - bands.x0[i] + 1) * 2;
  return total;
}

// ---------------------------------------------------------------------------
// PushPlanner

namespace {

inline uint32_t load32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

// First and last differing byte of a[lo..hi] and b[lo..hi] (inclusive);
// false when equal. Scans from both ends a word at a time.
bool diffExtent(const uint8_t* a, const uint8_t* b, int lo, int hi,
                int& first, int& last) {
  int i = lo;
  while (i <= hi && (i & 3) != 0 && a[i] == b[i]) i++;
  if (i <= hi && (i & 3) == 0) {
    while (i + 3 <= hi && load32(a + i) == load32(b + i)) i += 4;
    while (i <= hi && a[i] == b[i]) i++;
  }
  if (i > hi) return false;
  first = i;
  int j = hi;
  while (j > first && ((j + 1) & 3) != 0 && a[j] == b[j]) j--;
  if (j > first && ((j + 1) & 3) == 0) {
    while (j - 3 > first && load32(a + j - 3) == load32(b + j - 3)) j -= 4;
  }
  while (j > first && a[j] == b[j]) j--;
  last = j;
  return true;
}

}  // namespace

PushPlanner::PushPlanner(uint8_t* shadow, int w, int h)
  : _shadow(shadow), _w(w > 0 ? w : 0), _h(h > 0 ? h : 0), _force(true) {
  if (_w > 0x7FFF) _w = 0x7FFF;
  if (_h > kMaxFbRows) _h = kMaxFbRows;
  _cand.reset();
  _prev.reset();
}

void PushPlanner::take(FrameBuffer332& fb) { fb.takePushSpans(_cand); }

void PushPlanner::plan(const FrameBuffer332& fb, SpanBands& out,
                       Stats* stats) {
  out.count = 0;
  Stats st = {0, 0, 0, 0};
  const int w = fb.width() < _w ? fb.width() : _w;
  const int h = fb.height() < _h ? fb.height() : _h;
  const uint8_t* px = fb.pixels();
  const bool force = _force;
  _force = false;
  // The glass shows the previous frame; this frame differs from it only
  // where this buffer was drawn since its last push, or where the previous
  // push changed the glass.
  _cand.merge(_prev);
  _prev.reset();
  if (_shadow == nullptr || px == nullptr || w <= 0 || h <= 0) {
    _cand.reset();
    if (stats != nullptr) *stats = st;
    return;
  }
  if (force) {
    // Glass unknown: send everything. What the next frame (the other
    // buffer) differs in is unknown too (a dropped submit breaks the A/B
    // chain), so it is compared in full.
    memcpy(_shadow, px, (size_t)w * (size_t)h);
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
    const uint8_t* row = px + y * w;
    uint8_t* glass = _shadow + y * w;
    int first = 0, last = 0;
    if (!diffExtent(row, glass, a, b, first, last)) {
      st.comparedBytes += b - a + 1;
      st.skippedRows++;
      continue;
    }
    st.comparedBytes += (first - a) + (b - last) + 2;
    memcpy(glass + first, row + first, (size_t)(last - first + 1));
    _prev.add(y, first, last);
    st.pushedRows++;
  }
  _cand.reset();
  planSpanBands(_prev, h, kBandAlign, kWindowCostBytes, out);
  if (stats != nullptr) *stats = st;
}
