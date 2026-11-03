#include "NodesPage.h"

// CYD: these loops run once per band on a 240 MHz ESP32 without PSRAM;
// the default -Os costs about 2x here.
#if HMI_CYD
#pragma GCC optimize("O2")
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Packet.h>

#include "StubPages.h"

namespace freenove_ui {

using mesh::head::MeshModel;
using mesh::head::NodeInfo;

#if HMI_CYD
const PanelRect NodesPage::kPanel = {4, kHeaderH + 2, 312, 174};
#else
const PanelRect NodesPage::kPanel = {6, kHeaderH + 2, 468, 250};
#endif

namespace {

const uint32_t kStaleMs = 30u * 60u * 1000u;
// Column edges (screen px): NAME, TYPE, HEARD, SNR, HOPS, KM, PKTS.
// SNR is the wide one: "-12.5 via <relay name>".
#if HMI_CYD
// TYPE and KM have no width (not shown); shorter labels.
const int kColEdge[widgets::kColCount + 1] = {6, 98, 98, 142, 222, 262, 262, 302};
const int kSnrSlotW = 32;
const char* const kColLabel[widgets::kColCount] = {"NAME", "TYPE", "AGE", "SNR", "HOP", "KM", "PKT"};
const int kPad = 4;
#else
const int kColEdge[widgets::kColCount + 1] = {8, 122, 160, 222, 346, 386, 418, 462};
const int kSnrSlotW = 34;   // SNR value width inside the SNR column
const char* const kColLabel[widgets::kColCount] = {"NAME", "TYPE", "HEARD", "SNR", "HOPS", "KM", "PKTS"};
const int kPad = 6;
#endif
bool shown(int c) { return kColEdge[c + 1] > kColEdge[c]; }

bool leftAligned(int c) { return c == widgets::kColName || c == widgets::kColType || c == widgets::kColSnr; }

}  // namespace

NodesPage::NodesPage()
    : _card(nullptr), _alloc(nullptr), _rows(nullptr), _order(nullptr), _count(0), _sort(widgets::kColHeard), _desc(false), _scroll(0), _drawn(false), _built(false),
      _drawnMs(0), _drawnGen(0), _builtMs(0), _builtGen(0), _press(false), _drag(false), _pressX(0), _pressY(0),
      _scroll0(0) {
}

NodesPage::~NodesPage() {
  if (_rows == nullptr) return;
  if (_alloc != nullptr) _alloc->release(_rows, _alloc->ctx);
  else free(_rows);
}

bool NodesPage::ensureStorage() {
  if (_rows != nullptr) return true;
  const size_t bytes = (size_t)kMaxRows * (sizeof(widgets::NodeRow) + sizeof(uint16_t));
  void* p = _alloc != nullptr ? _alloc->alloc(bytes, _alloc->ctx) : malloc(bytes);
  if (p == nullptr) return false;
  memset(p, 0, bytes);
  _rows = (widgets::NodeRow*)p;
  _order = (uint16_t*)(_rows + kMaxRows);
  return true;
}

int NodesPage::panels(const PanelRect*& out) const {
  out = &kPanel;
  return 1;
}

int NodesPage::columnX0(int c) { return c >= 0 && c < widgets::kColCount ? kColEdge[c] : -1; }
int NodesPage::columnX1(int c) { return c >= 0 && c < widgets::kColCount ? kColEdge[c + 1] : -1; }

int NodesPage::columnAt(int x) {
  for (int c = 0; c < widgets::kColCount; c++)
    if (x >= kColEdge[c] && x < kColEdge[c + 1]) return c;
  return x >= kColEdge[widgets::kColCount] && x < kPanel.x + kPanel.w ? widgets::kColCount - 1 : -1;
}

namespace {
void headerLabel(char* out, size_t cap, int c, bool sorted, bool desc) {
  snprintf(out, cap, sorted ? (desc ? "%s v" : "%s ^") : "%s", kColLabel[c]);
}
}  // namespace

void NodesPage::headerSpan(int c, bool sorted, int& x0, int& x1) {
  char lab[16];
  headerLabel(lab, sizeof(lab), c, sorted, false);
  const int w = FrameBuffer565::textWidth(lab, kFontSmall);
  if (leftAligned(c)) { x0 = kColEdge[c] + kPad; x1 = x0 + w; }
  else { x1 = kColEdge[c + 1] - kPad; x0 = x1 - w; }
}

int NodesPage::maxScroll() const {
  const int total = _count * kRowH;
  return total > kRowsH ? total - kRowsH : 0;
}

bool NodesPage::rowKey(int i, uint8_t key[4]) const {
  if (i < 0 || i >= _count || _rows == nullptr) return false;
  memcpy(key, _rows[_order[i]].key, 4);
  return true;
}

void NodesPage::setSort(widgets::NodeColumn c, bool descending) {
  _sort = c;
  _desc = descending;
  if (_rows != nullptr) widgets::sortRows(_order, _count, _rows, _sort, _desc);
  _scroll = 0;
}

void NodesPage::onShow(uint32_t now_ms) {
  (void)now_ms;
  _drawn = false;
  _built = false;
  _press = _drag = false;
  if (_card != nullptr) _card->close();
}

void NodesPage::onHide() {
  if (_card != nullptr) _card->close();
}

bool NodesPage::wantsFrame(uint32_t now_ms) {
  if (_card != nullptr && _card->isOpen()) return _card->wantsFrame(now_ms);
  if (!_drawn) return true;
  const uint32_t since = now_ms - _drawnMs;
  if (_ctx != nullptr && _ctx->data_gen != _drawnGen && since >= 250) return true;
  return since >= 1000;
}

void NodesPage::rebuild(uint32_t now_ms) {
  const MeshModel* m = _ctx != nullptr ? _ctx->model : nullptr;
  _count = 0;
  _built = true;
  _builtMs = now_ms;
  _builtGen = _ctx != nullptr ? _ctx->data_gen : 0;
  if (m == nullptr || !m->ok() || !ensureStorage()) return;
  int32_t rlat = 0, rlon = 0;
  const bool fix = m->repeaterPosition(rlat, rlon);
  for (size_t i = 0; i < m->nodeCount() && _count < kMaxRows; i++) {
    const NodeInfo& n = *m->node(i);
    widgets::NodeRow& r = _rows[_count];
    memcpy(r.key, n.pub_prefix, 4);
    r.name = n.name;   // sorted on the raw name (display drops emoji only)
    r.type = n.type;
    r.hops = n.hops;
    const widgets::SnrCell cell = widgets::snrCell(*m, n);   // the value the SNR column shows
    r.has_snr = cell.has_snr;
    r.snr_q4 = cell.snr_q4;
    r.has_km = fix && n.has_pos && mesh::head::geoValid(n.lat_e6, n.lon_e6) && !(n.lat_e6 == 0 && n.lon_e6 == 0);
    r.km = r.has_km ? (float)mesh::head::distanceKm(rlat, rlon, n.lat_e6, n.lon_e6) : 0.0f;
    r.age_ms = now_ms - n.last_heard_ms;
    r.packets = n.packets;
    _order[_count] = (uint16_t)_count;
    _count++;
  }
  widgets::sortRows(_order, _count, _rows, _sort, _desc);
  if (_scroll > maxScroll()) _scroll = maxScroll();
}

bool NodesPage::touch(int x, int y, bool down) {
  if (_card != nullptr && _card->isOpen()) {
    const bool changed = _card->touch(x, y, down);
    if (changed && !_card->isOpen()) _drawn = false;
    return changed;
  }
  if (down) {
    if (!_press) {
      _press = true;
      _drag = false;
      _pressX = x;
      _pressY = y;
      _scroll0 = _scroll;
      return false;
    }
    if (!_drag && abs(y - _pressY) >= kSlop) _drag = true;
    if (!_drag) return false;
    int s = _scroll0 - (y - _pressY);
    if (s < 0) s = 0;
    if (s > maxScroll()) s = maxScroll();
    if (s == _scroll) return false;
    _scroll = s;
    return true;
  }
  const bool wasDrag = _drag;
  const bool pressed = _press;
  _press = _drag = false;
  if (!pressed || wasDrag || abs(x - _pressX) >= kSlop || abs(y - _pressY) >= kSlop) return false;
  // Tap: a column header sorts, a row opens its card.
  if (y >= kHeadY && y < kHeadY + kHeadH + 2) {
    const int c = columnAt(x);
    if (c < 0) return false;
    const widgets::NodeColumn col = (widgets::NodeColumn)c;
    setSort(col, col == _sort ? !_desc : widgets::defaultDescending(col));
    return true;
  }
  if (y >= kRowsY && y < kRowsY + kRowsH && x >= kPanel.x && x < kPanel.x + kPanel.w) {
    const int pos = (y - kRowsY + _scroll) / kRowH;
    uint8_t key[4];
    if (_card == nullptr || !rowKey(pos, key)) return false;
    _card->open(key);
    return true;
  }
  return false;
}

void NodesPage::drawBackground(FrameBuffer565& fb) {
  drawPanel(fb, kPanel, "NODES", nullptr);
  fb.hline(kPanel.x + (HMI_CYD ? 6 : 8), kHeadY + kHeadH - 1, kPanel.w - (HMI_CYD ? 12 : 16), theme::kDivider);
}

void NodesPage::drawRow(FrameBuffer565& fb, int pos, int y) const {
  const MeshModel& m = *_ctx->model;
  const widgets::NodeRow& r = _rows[_order[pos]];
  const NodeInfo* n = findNode(m, r.key);
  if (n == nullptr) return;
  if (pos % 2 == 1) fb.fillRect(kPanel.x + 4, y, kPanel.w - 14, kRowH, theme::kTileOffFill);
  const bool stale = r.age_ms >= kStaleMs;
  const uint16_t tc = stale ? kDimText : theme::kText;
  const int ty = y + 2;
  char buf[40], name[40];
  widgets::nodeName(buf, sizeof(buf), *n);
  widgets::fitText(name, sizeof(name), buf, kFontSmall, kColEdge[1] - kColEdge[0] - 2 * kPad);
  fb.text(kColEdge[0] + kPad, ty, name, tc, kFontSmall);
  if (shown(widgets::kColType)) fb.text(kColEdge[1] + kPad, ty, widgets::typeShort(r.type), theme::kTextDim, kFontSmall);
  widgets::formatAge(buf, sizeof(buf), r.age_ms);
  fb.text(kColEdge[3] - kPad, ty, buf, tc, kFontSmall, FrameBuffer565::kRightTop);
  // SNR: own for a node heard directly; "-4.2 via Downtown" for a relayed
  // one (the SNR the M5 heard its advert at, then the relay's name).
  {
    const widgets::SnrCell cell = widgets::snrCell(m, *n);
    const int x0 = kColEdge[3] + kPad, x1 = kColEdge[4] - kPad;
    const int slot = kSnrSlotW;   // the value, right-aligned, so "via" names line up
    if (cell.has_snr) {
      widgets::formatSnr(buf, sizeof(buf), cell.snr_q4);
      fb.text(x0 + slot, ty, buf, stale ? kDimText : linkColour(cell.snr_q4), kFontSmall, FrameBuffer565::kRightTop);
    } else if (cell.relayed) {
      fb.text(x0 + slot, ty, "--", kDimText, kFontSmall, FrameBuffer565::kRightTop);
    }
    if (cell.relayed) {
      char via[40], fitted[40];
      if (cell.via[0]) snprintf(via, sizeof(via), "via %s", cell.via);
      else snprintf(via, sizeof(via), "relayed");
      widgets::fitText(fitted, sizeof(fitted), via, kFontSmall, x1 - (x0 + slot + 5));
      fb.text(x0 + slot + 5, ty, fitted, kDimText, kFontSmall);
    } else if (!cell.has_snr) {
      fb.text(x0 + slot, ty, "--", kDimText, kFontSmall, FrameBuffer565::kRightTop);
    }
  }
  snprintf(buf, sizeof(buf), "%u", (unsigned)r.hops);
  fb.text(kColEdge[5] - kPad, ty, buf, tc, kFontSmall, FrameBuffer565::kRightTop);
  if (shown(widgets::kColKm)) {
    if (r.has_km) widgets::formatKm(buf, sizeof(buf), r.km, false);
    else snprintf(buf, sizeof(buf), "--");
    fb.text(kColEdge[6] - kPad, ty, buf, r.has_km ? tc : kDimText, kFontSmall, FrameBuffer565::kRightTop);
  }
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)r.packets);
  fb.text(kColEdge[7] - kPad, ty, buf, tc, kFontSmall, FrameBuffer565::kRightTop);
}

void NodesPage::draw(FrameBuffer565& fb, uint32_t now_ms) {
  _drawn = true;
  _drawnMs = now_ms;
  _drawnGen = _ctx != nullptr ? _ctx->data_gen : 0;
  const MeshModel* m = _ctx != nullptr ? _ctx->model : nullptr;
  if (m == nullptr || !m->ok()) {
    fb.text(kMidX, kMidY, "MODEL UNAVAILABLE", theme::kBad, kFontBody, FrameBuffer565::kCentreTop);
    return;
  }
  if (_card != nullptr && _card->isOpen()) {
    _card->draw(fb, now_ms);
    return;
  }
  // Rows follow the model (not mid-drag: the list would jump under the finger).
  if (firstBand() &&
      (!_built || (!_drag && (_ctx->data_gen != _builtGen || (uint32_t)(now_ms - _builtMs) >= 1000))))
    rebuild(now_ms);
  char note[40];
  if (HMI_CYD) snprintf(note, sizeof(note), "%d node%s", _count, _count == 1 ? "" : "s");
  else snprintf(note, sizeof(note), "%d node%s \xB7 tap a header to sort", _count, _count == 1 ? "" : "s");
  fb.text(kPanel.x + kPanel.w - kNoteInset, kPanel.y + kNoteDy, note, theme::kAccent, kFontSmall, FrameBuffer565::kRightTop);
  // Column headers; the sort column in the accent colour with ^ / v.
  for (int c = 0; c < widgets::kColCount; c++) {
    if (!shown(c)) continue;
    const bool active = c == _sort;
    char lab[16];
    headerLabel(lab, sizeof(lab), c, active, _desc);
    const uint16_t col = active ? theme::kAccent : theme::kTextDim;
    if (leftAligned(c)) fb.text(kColEdge[c] + kPad, kHeadY + 3, lab, col, kFontSmall);
    else fb.text(kColEdge[c + 1] - kPad, kHeadY + 3, lab, col, kFontSmall, FrameBuffer565::kRightTop);
  }
  if (_count == 0) {
    fb.text(kPanel.x + kPanel.w / 2, kRowsY + 60, "No nodes heard yet", theme::kTextDim, kFontSmall,
            FrameBuffer565::kCentreTop);
    return;
  }
  fb.setClip(kPanel.x + 2, kRowsY, kPanel.w - 4, kRowsH);
  const int first = _scroll / kRowH;
  for (int pos = first; pos < _count; pos++) {
    const int y = kRowsY + pos * kRowH - _scroll;
    if (y >= kRowsY + kRowsH) break;
    drawRow(fb, pos, y);
  }
  fb.resetClip();
  // Scroll bar when the list is longer than the panel.
  if (maxScroll() > 0) {
    const int trackX = kPanel.x + kPanel.w - 8, total = _count * kRowH;
    int h = kRowsH * kRowsH / total;
    if (h < 16) h = 16;
    const int y = kRowsY + (kRowsH - h) * _scroll / maxScroll();
    fb.fillRect(trackX, kRowsY, 3, kRowsH, theme::kGaugeTrack);
    fb.fillRect(trackX, y, 3, h, theme::kAccent);
  }
}

}  // namespace freenove_ui
