#include "MapPage.h"

// CYD: these loops run once per band on a 240 MHz ESP32 without PSRAM;
// the default -Os costs about 2x here.
#if HMI_CYD
#pragma GCC optimize("O2")
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Packet.h>
#include <helpers/AdvertDataHelpers.h>

#include "StubPages.h"
#include "Widgets.h"

namespace freenove_ui {

using mesh::head::FeedItem;
using mesh::head::MeshModel;
using mesh::head::NodeInfo;
using mesh::head::PathHash;

#if HMI_CYD
const PanelRect MapPage::kPanel = {4, kHeaderH + 2, 312, 174};
#else
const PanelRect MapPage::kPanel = {6, kHeaderH + 2, 468, 250};
#endif

namespace {

const uint32_t kStaleMs = 30u * 60u * 1000u;   // as HOME: not heard for 30 min = grey

struct Boxes {
  enum { kMax = 160 };
  int16_t x0[kMax], y0[kMax], x1[kMax], y1[kMax];
  int n;
  Boxes() : n(0) {}
  void add(int ax, int ay, int bx, int by) {
    if (n >= kMax) return;
    x0[n] = (int16_t)ax; y0[n] = (int16_t)ay; x1[n] = (int16_t)bx; y1[n] = (int16_t)by; n++;
  }
  bool hits(int ax, int ay, int bx, int by) const {
    for (int i = 0; i < n; i++)
      if (ax < x1[i] && bx > x0[i] && ay < y1[i] && by > y0[i]) return true;
    return false;
  }
};

bool hasSignal(const NodeInfo& n) { return n.snr_hist[0] != mesh::head::SNR_NONE; }

uint16_t nodeColour(const NodeInfo& n, uint32_t now_ms) {
  if (!hasSignal(n) || (uint32_t)(now_ms - n.last_heard_ms) >= kStaleMs) return theme::kStale;
  return linkColour(n.last_snr_q4);
}

}  // namespace

MapPage::MapPage()
    : _card(nullptr), _geo(kAreaW, kAreaH, 24), _drawn(false), _synced(false), _fix(false), _drawnMs(0), _drawnGen(0),
      _syncGen(0), _feedSeq(0), _shown(0), _noGps(0), _withGps(0), _arcNext(0), _press(false), _pressX(0), _pressY(0) {
  memset(_x, 0, sizeof(_x));
  memset(_y, 0, sizeof(_y));
  memset(_key, 0, sizeof(_key));
  memset(_labX, 0, sizeof(_labX));
  memset(_labY, 0, sizeof(_labY));
  memset(_labMode, 0, sizeof(_labMode));
  memset(_arcs, 0, sizeof(_arcs));
}

int MapPage::panels(const PanelRect*& out) const {
  out = &kPanel;
  return 1;
}

void MapPage::onShow(uint32_t now_ms) {
  (void)now_ms;
  _drawn = false;
  _synced = false;   // arcs of packets heard while hidden are not replayed
  _press = false;
  for (int i = 0; i < kMaxArcs; i++) _arcs[i].start = 0, _arcs[i].x0 = _arcs[i].x1 = -1;
  if (_card != nullptr) _card->close();
}

void MapPage::onHide() {
  if (_card != nullptr) _card->close();
}

bool MapPage::shown(int i, int& x, int& y, uint8_t key[4]) const {
  if (i < 0 || i >= _shown) return false;
  x = _x[i];
  y = _y[i];
  memcpy(key, _key[i], 4);
  return true;
}

int MapPage::arcsActive(uint32_t now_ms) const {
  int n = 0;
  for (int i = 0; i < kMaxArcs; i++)
    if (_arcs[i].x0 >= 0 && (uint32_t)(now_ms - _arcs[i].start) < kArcMs) n++;
  return n;
}

bool MapPage::wantsFrame(uint32_t now_ms) {
  if (_card != nullptr && _card->isOpen()) return _card->wantsFrame(now_ms);
  if (!_drawn) return true;
  if (!profile::kAnimations) {
    // CYD: a frame when an arc ends, none while it shows.
    if (arcsActive(now_ms) < arcsActive(_drawnMs)) return true;
  } else if (arcsActive(now_ms) > 0 || arcsActive(_drawnMs) > 0) {
    return true;
  }
  if (_ctx != nullptr && _ctx->data_gen != _drawnGen) return true;
  return (uint32_t)(now_ms - _drawnMs) >= 1000;
}

void MapPage::drawBackground(FrameBuffer565& fb) {
  drawPanel(fb, kPanel, "MAP", nullptr);
}

// ------------------------------------------------------------------ model

bool MapPage::hashPos(const PathHash& h, const uint8_t* sender, int& x, int& y) const {
  const MeshModel& m = *_ctx->model;
  uint8_t rep[4];
  if (sender == nullptr && h.valid() && m.repeaterKey(rep) && h.matchesKey(rep)) {
    if (!_fix) return false;
    x = kAreaX + _geo.cx();
    y = kAreaY + _geo.cy();
    return true;
  }
  // The advert's sender by its key; a forwarder only when the hash resolves
  // to one repeater among all the model's nodes (plotted or not).
  const NodeInfo* n = sender != nullptr ? findNode(m, sender) : m.findByHash(h);
  if (n == nullptr) return false;
  for (int i = 0; i < _shown; i++) {
    if (memcmp(_key[i], n->pub_prefix, 4) != 0) continue;
    x = _x[i];
    y = _y[i];
    return true;
  }
  return false;   // resolved, but not plotted (no GPS)
}

void MapPage::takeArcs(uint32_t now_ms) {
  const MeshModel& m = *_ctx->model;
  const uint32_t fresh = m.feedSeq() - _feedSeq;
  _feedSeq = m.feedSeq();
  for (uint32_t i = 0; i < fresh && i < 4; i++) {
    const FeedItem* f = m.feed(i);
    if (f == nullptr || f->route_len == 0) continue;
    int px = 0, py = 0;
    bool prev = false;
    for (uint8_t r = 0; r <= f->route_len; r++) {
      int x = 0, y = 0;
      bool ok;
      if (r < f->route_len) {
        ok = hashPos(f->route[r], r == 0 && routeHasSender(*f) && f->has_sender ? f->sender : nullptr, x, y);
      } else {   // the last hop to the repeater that heard it
        ok = _fix;
        x = kAreaX + _geo.cx();
        y = kAreaY + _geo.cy();
      }
      if (ok && prev && (x != px || y != py)) {
        Arc& a = _arcs[_arcNext];
        _arcNext = (_arcNext + 1) % kMaxArcs;
        a.x0 = (int16_t)px; a.y0 = (int16_t)py; a.x1 = (int16_t)x; a.y1 = (int16_t)y;
        a.start = now_ms;
      }
      prev = ok;
      px = x;
      py = y;
    }
  }
}

void MapPage::sync(uint32_t now_ms) {
  const MeshModel& m = *_ctx->model;
  if (_synced && _ctx->data_gen == _syncGen) return;
  const bool first = !_synced;
  _synced = true;
  _syncGen = _ctx->data_gen;
  // The freshest kMaxShown contacts with a position.
  uint16_t pick[kMaxShown];
  int n = 0;
  _noGps = 0;
  _withGps = 0;
  for (size_t i = 0; i < m.nodeCount(); i++) {
    const NodeInfo& nd = *m.node(i);
    if (!nd.has_pos || !mesh::head::geoValid(nd.lat_e6, nd.lon_e6) || (nd.lat_e6 == 0 && nd.lon_e6 == 0)) {
      _noGps++;
      continue;
    }
    _withGps++;
    const uint32_t age = now_ms - nd.last_heard_ms;
    int at = n;
    while (at > 0 && (uint32_t)(now_ms - m.node(pick[at - 1])->last_heard_ms) > age) at--;
    if (at >= kMaxShown) continue;
    const int last = n < kMaxShown ? n : kMaxShown - 1;
    for (int j = last; j > at; j--) pick[j] = pick[j - 1];
    pick[at] = (uint16_t)i;
    if (n < kMaxShown) n++;
  }
  int32_t lat0 = 0, lon0 = 0;
  _fix = m.repeaterPosition(lat0, lon0);
  GeoPoint pts[kMaxShown];
  memset(pts, 0, sizeof(pts));
  int64_t sLat = 0, sLon = 0;
  for (int i = 0; i < n; i++) {
    const NodeInfo& nd = *m.node(pick[i]);
    pts[i].lat_e6 = nd.lat_e6;
    pts[i].lon_e6 = nd.lon_e6;
    sLat += nd.lat_e6;
    sLon += nd.lon_e6;
  }
  if (!_fix && n > 0) {
    lat0 = (int32_t)(sLat / n);
    lon0 = (int32_t)(sLon / n);
  }
  _geo.fit(lat0, lon0, pts, (size_t)n);
  _shown = 0;
  for (int i = 0; i < n; i++) {
    const NodeInfo& nd = *m.node(pick[i]);
    int x, y;
    if (!_geo.project(nd.lat_e6, nd.lon_e6, x, y)) continue;
    _x[_shown] = (int16_t)(kAreaX + x);
    _y[_shown] = (int16_t)(kAreaY + y);
    memcpy(_key[_shown], nd.pub_prefix, 4);
    _shown++;
  }
  if (first) _feedSeq = m.feedSeq();
  else takeArcs(now_ms);
}

// ------------------------------------------------------------------ input

int MapPage::hit(int x, int y) const {
  int best = -1, bestD = kHitR * kHitR + 1;
  for (int i = 0; i < _shown; i++) {
    const int dx = _x[i] - x, dy = _y[i] - y;
    const int d = dx * dx + dy * dy;
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}

bool MapPage::touch(int x, int y, bool down) {
  if (_card != nullptr && _card->isOpen()) {
    const bool changed = _card->touch(x, y, down);
    if (changed && !_card->isOpen()) _drawn = false;
    return changed;
  }
  if (down) {
    if (!_press) { _press = true; _pressX = x; _pressY = y; }
    return false;
  }
  const bool tap = _press && abs(x - _pressX) < 12 && abs(y - _pressY) < 12;
  _press = false;
  if (!tap || _card == nullptr) return false;
  const int i = hit(x, y);
  if (i < 0) return false;
  _card->open(_key[i]);
  return true;
}

// ---------------------------------------------------------------- drawing

// "name 4.1km" (distance from the repeater, with a fix) or the name.
void MapPage::labelText(char* out, size_t cap, const NodeInfo& n, bool withDist) const {
  char name[40], shown[40];
  widgets::nodeName(name, sizeof(name), n);
  widgets::fitText(shown, sizeof(shown), name, kFontSmall, 120);
  int32_t rlat = 0, rlon = 0;
  if (withDist && _ctx->model->repeaterPosition(rlat, rlon)) {
    char d[16];
    widgets::formatKm(d, sizeof(d), mesh::head::distanceKm(rlat, rlon, n.lat_e6, n.lon_e6), true);
    snprintf(out, cap, "%s %s", shown, d);
  } else {
    snprintf(out, cap, "%s", shown);
  }
}

// Each contact's label (right, left, above or below its ring), with the
// distance first, then the name alone; none where nothing fits. Contacts
// come before the grid's ring labels.
void MapPage::placeLabels(void* takenBoxes) {
  Boxes& taken = *static_cast<Boxes*>(takenBoxes);
  const int sh = kFontSmall.ascent + kFontSmall.descent;
  for (int i = 0; i < _shown; i++) {
    _labMode[i] = 0;
    const NodeInfo* nd = findNode(*_ctx->model, _key[i]);
    if (nd == nullptr) continue;
    for (int withDist = _fix ? 1 : 0; withDist >= 0 && _labMode[i] == 0; withDist--) {
      char text[64];
      labelText(text, sizeof(text), *nd, withDist == 1);
      const int w = FrameBuffer565::textWidth(text, kFontSmall);
      const int g = kNodeR + 4;
      const int cand[4][2] = {{_x[i] + g, _y[i] - sh / 2}, {_x[i] - g - w, _y[i] - sh / 2}, {_x[i] - w / 2, _y[i] - g - sh},
                              {_x[i] - w / 2, _y[i] + g}};
      for (int k = 0; k < 4; k++) {
        const int lx = cand[k][0], ly = cand[k][1];
        if (lx < kAreaX + 2 || ly < kAreaY + 2 || lx + w > kAreaX + kAreaW - 2 || ly + sh > kAreaY + kAreaH - 2) continue;
        if (taken.hits(lx - 1, ly - 1, lx + w + 1, ly + sh + 1)) continue;
        taken.add(lx, ly, lx + w, ly + sh);
        _labX[i] = (int16_t)lx;
        _labY[i] = (int16_t)ly;
        _labMode[i] = (uint8_t)(withDist ? 2 : 1);
        break;
      }
    }
  }
}

void MapPage::drawGrid(FrameBuffer565& fb, int cx, int cy, void* takenBoxes) const {
  Boxes& taken = *static_cast<Boxes*>(takenBoxes);
  const int sh = kFontSmall.ascent + kFontSmall.descent;
  const int R = kAreaW / 2 - 4;
  // Bearing spokes every 45 degrees, N/E/S/W letters.
  for (int k = 0; k < 8; k++) {
    const float a = (float)k * 45.0f * 3.14159265f / 180.0f;
    const int ex = cx + (int)lroundf(sinf(a) * (float)R), ey = cy - (int)lroundf(cosf(a) * (float)R);
    fb.dashedLine(cx, cy, ex, ey, theme::kGeoRing, 2, 5, 0);
  }
  // Range rings: a round step from the shorter half of the area.
  const double half = (double)(kAreaH / 2 - 10);
  const double step = GeoLayout::niceKm(half / _geo.pxPerKm()) / 2.0;
  if (step > 0) {
    for (int k = 1; k <= 8; k++) {
      const double km = step * k;
      const int r = (int)(km * _geo.pxPerKm() + 0.5);
      if (r < 16) continue;
      if (r > R) break;
      fb.aaRing((float)cx, (float)cy, (float)r, 1.0f, theme::kGeoRing);
      char lab[16];
      if (km >= 1.0) snprintf(lab, sizeof(lab), "%gkm", km);
      else snprintf(lab, sizeof(lab), "%dm", (int)(km * 1000.0 + 0.5));
      // On the ring at 30 degrees east of north when inside, else on its east side.
      // Where no contact or label is: on the ring 30 degrees east of north,
      // east of it, or west of it.
      const int w = FrameBuffer565::textWidth(lab, kFontSmall);
      const int cand[3][2] = {{cx + (int)lroundf((float)r * 0.5f) + 3, cy - (int)lroundf((float)r * 0.866f) - 14},
                              {cx + r + 3, cy - 15}, {cx - r - 3 - w, cy - 15}};
      for (int c = 0; c < 3; c++) {
        const int lx = cand[c][0], ly = cand[c][1];
        if (lx < kAreaX + 2 || ly < kAreaY + 2 || lx + w > kAreaX + kAreaW - 2 || ly + sh > kAreaY + kAreaH - 2) continue;
        if (taken.hits(lx - 2, ly - 2, lx + w + 2, ly + sh + 2)) continue;
        fb.text(lx, ly, lab, theme::kTextDim, kFontSmall);
        taken.add(lx, ly, lx + w, ly + sh);
        break;
      }
    }
  }
  static const char* const kDir[4] = {"N", "E", "S", "W"};
  const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
  for (int i = 0; i < 4; i++) {
    int tx = cx + dx[i] * (R - 8), ty = cy + dy[i] * (R - 8);
    if (ty < kAreaY + 10) ty = kAreaY + 10;
    if (ty > kAreaY + kAreaH - 10) ty = kAreaY + kAreaH - 10;
    fb.text(tx, ty, kDir[i], theme::kTextDim, kFontSmall, FrameBuffer565::kCentreMiddle);
  }
}

void MapPage::drawArcs(FrameBuffer565& fb, uint32_t now_ms) const {
  for (int i = 0; i < kMaxArcs; i++) {
    const Arc& a = _arcs[i];
    const uint32_t t = now_ms - a.start;
    if (a.x0 < 0 || t >= kArcMs) continue;
    // A quadratic curve bowed to the left of travel by 20 % of its length.
    const float x0 = a.x0, y0 = a.y0, x1 = a.x1, y1 = a.y1;
    const float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
    const float qx = mx + (y1 - y0) * 0.2f, qy = my - (x1 - x0) * 0.2f;
    const float life = (float)t / (float)kArcMs;
    const unsigned alpha = !profile::kAnimations || life < 0.5f ? 255u : (unsigned)(255.0f * (1.0f - life) * 2.0f);
    float px = x0, py = y0;
    for (int s = 1; s <= 16; s++) {
      const float u = (float)s / 16.0f, v = 1.0f - u;
      const float bx = v * v * x0 + 2 * v * u * qx + u * u * x1, by = v * v * y0 + 2 * v * u * qy + u * u * y1;
      fb.aaLine(px, py, bx, by, 2.0f, theme::kRoute, alpha);
      px = bx;
      py = by;
    }
    if (profile::kAnimations && life < 0.6f) {   // the packet travelling along it
      const float u = life / 0.6f, v = 1.0f - u;
      const float bx = v * v * x0 + 2 * v * u * qx + u * u * x1, by = v * v * y0 + 2 * v * u * qy + u * u * y1;
      fb.aaRing(bx, by, 6.0f, 1.5f, theme::kRoute, 150);
      fb.aaDisc(bx, by, 3.0f, theme::kBlip);
    }
  }
}

void MapPage::draw(FrameBuffer565& fb, uint32_t now_ms) {
  _drawn = true;
  _drawnMs = now_ms;
  _drawnGen = _ctx != nullptr ? _ctx->data_gen : 0;
  const MeshModel* m = _ctx != nullptr ? _ctx->model : nullptr;
  if (m == nullptr || !m->ok()) {
    fb.text(kMidX, kMidY, "MODEL UNAVAILABLE", theme::kBad, kFontBody, FrameBuffer565::kCentreTop);
    return;
  }
  if (firstBand()) sync(now_ms);
  if (_card != nullptr && _card->isOpen()) {
    _card->draw(fb, now_ms);
    return;
  }
  char note[32];
  snprintf(note, sizeof(note), "%d with GPS", _withGps);
  fb.text(kPanel.x + kPanel.w - kNoteInset, kPanel.y + kNoteDy, note, theme::kAccent, kFontSmall, FrameBuffer565::kRightTop);
  fb.setClip(kAreaX, kAreaY, kAreaW, kAreaH);
  const int cx = kAreaX + _geo.cx(), cy = kAreaY + _geo.cy();
  Boxes taken;
  const int sh = kFontSmall.ascent + kFontSmall.descent;
  // Notes first (labels keep clear of them).
  char gpsNote[32] = "";
  if (_noGps > 0) snprintf(gpsNote, sizeof(gpsNote), "%d without GPS", _noGps);
  const char* top = _fix ? nullptr
                         : (HMI_CYD ? "Repeater has no GPS fix" : "Repeater has no GPS fix: contacts placed by their own positions");
  if (top != nullptr) taken.add(kAreaX + 4, kAreaY + 2, kAreaX + 4 + FrameBuffer565::textWidth(top, kFontSmall), kAreaY + 2 + sh);
  if (gpsNote[0])
    taken.add(kAreaX + 4, kAreaY + kAreaH - sh - 3, kAreaX + 4 + FrameBuffer565::textWidth(gpsNote, kFontSmall),
              kAreaY + kAreaH - 3);
  for (int i = 0; i < _shown; i++) taken.add(_x[i] - kNodeR - 1, _y[i] - kNodeR - 1, _x[i] + kNodeR + 2, _y[i] + kNodeR + 2);
  if (_fix) taken.add(cx - 10, cy - 10, cx + 32, cy + 10);   // the M5 and its label
  placeLabels(&taken);
  if (_fix) {
    drawGrid(fb, cx, cy, &taken);
  } else if (_shown > 0) {
    // Scale bar (bottom right) and a north mark (top right).
    const double km = GeoLayout::niceKm(90.0 / _geo.pxPerKm());
    const int len = (int)(km * _geo.pxPerKm() + 0.5);
    const int bx1 = kAreaX + kAreaW - 10, bx0 = bx1 - len, by = kAreaY + kAreaH - 8;
    fb.hline(bx0, by, len, theme::kTextDim);
    fb.vline(bx0, by - 4, 5, theme::kTextDim);
    fb.vline(bx1, by - 4, 5, theme::kTextDim);
    char lab[16];
    if (km >= 1.0) snprintf(lab, sizeof(lab), "%gkm", km);
    else snprintf(lab, sizeof(lab), "%dm", (int)(km * 1000.0 + 0.5));
    fb.text((bx0 + bx1) / 2, by - 5 - sh, lab, theme::kTextDim, kFontSmall, FrameBuffer565::kCentreTop);
    taken.add(bx0 - 2, by - 6 - sh, bx1 + 2, by + 2);
    const int nx = kAreaX + kAreaW - 16, ny = kAreaY + 30;
    fb.aaLine((float)nx, (float)ny + 8, (float)nx, (float)ny - 8, 1.5f, theme::kTextDim);
    fb.aaLine((float)nx, (float)ny - 8, (float)nx - 4, (float)ny - 3, 1.5f, theme::kTextDim);
    fb.aaLine((float)nx, (float)ny - 8, (float)nx + 4, (float)ny - 3, 1.5f, theme::kTextDim);
    fb.text(nx, ny + 10, "N", theme::kTextDim, kFontSmall, FrameBuffer565::kCentreTop);
    taken.add(nx - 8, ny - 10, nx + 8, ny + 26);
  }
  drawArcs(fb, now_ms);
  // Contacts and their labels, placed with the notes taken.
  for (int i = 0; i < _shown; i++) {
    const NodeInfo* nd = findNode(*m, _key[i]);
    if (nd == nullptr) continue;
    const uint16_t c = nodeColour(*nd, now_ms);
    fb.aaDisc((float)_x[i], (float)_y[i], (float)kNodeR, theme::kPanelFill);
    fb.aaRing((float)_x[i], (float)_y[i], (float)kNodeR, 2.0f, c);
    if (_labMode[i] == 0) continue;
    char text[64];
    labelText(text, sizeof(text), *nd, _labMode[i] == 2);
    fb.text(_labX[i], _labY[i], text, c, kFontSmall);
  }
  if (_fix) {
    fb.aaDisc((float)cx, (float)cy, 8.5f, theme::kPanelFill);
    fb.aaRing((float)cx, (float)cy, 8.5f, 2.0f, theme::kRepeater);
    fb.aaDisc((float)cx, (float)cy, 2.2f, theme::kRepeater);
    fb.text(cx + 12, cy - 1, "M5", theme::kRepeater, kFontTitle);
  }
  if (_shown == 0)
    fb.text(kAreaX + kAreaW / 2, kAreaY + kAreaH / 2 + (_fix ? 24 : 0), "No contacts with a GPS position yet",
            theme::kTextDim, kFontSmall, FrameBuffer565::kCentreTop);
  if (top != nullptr) fb.text(kAreaX + 4, kAreaY + 2, top, theme::kWarn, kFontSmall);
  if (gpsNote[0]) fb.text(kAreaX + 4, kAreaY + kAreaH - sh - 3, gpsNote, kDimText, kFontSmall);
  fb.resetClip();
}

}  // namespace freenove_ui
