#include "NodeCard.h"

#include <stdio.h>
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

const PanelRect NodeCard::kRect = {6, kHeaderH + 2, 468, 250};

namespace {

const int kColL = 16, kColR = 252, kSplitX = 240;
const int kLineH = 18;
const int kTextW = 218;   // a column's text width

void hashHex(char* out, size_t cap, const PathHash& h) {
  if (cap == 0) return;
  out[0] = 0;
  size_t used = 0;
  for (uint8_t i = 0; i < h.size && used + 3 <= cap; i++)
    used += (size_t)snprintf(out + used, cap - used, "%02X", h.bytes[i]);
}

// Name of a route hop: the repeater, the advert's sender (its whole key is
// in the packet), the one repeater the model resolves the hash to, else the
// hash.
void hopName(char* out, size_t cap, const MeshModel& m, const PathHash& h, const uint8_t* sender) {
  uint8_t rep[4];
  if (m.repeaterKey(rep) && h.matchesKey(rep)) { snprintf(out, cap, "M5"); return; }
  const NodeInfo* n = sender != nullptr ? findNode(m, sender) : m.findByHash(h);
  if (n != nullptr) { widgets::nodeName(out, cap, *n); return; }
  hashHex(out, cap, h);
}

// Appends s to the string in out (cut to fit).
void append(char* out, size_t cap, const char* s) {
  size_t n = strlen(out);
  while (*s && n + 1 < cap) out[n++] = *s++;
  out[n] = 0;
}

}  // namespace

const NodeInfo* uniqueNode(const MeshModel& m, const PathHash& h) {
  if (!h.valid()) return nullptr;
  const NodeInfo* found = nullptr;
  for (size_t i = 0; i < m.nodeCount(); i++) {
    const NodeInfo* n = m.node(i);
    if (!h.matchesKey(n->pub_prefix)) continue;
    if (found != nullptr) return nullptr;   // shared: nobody's
    found = n;
  }
  return found;
}

static bool isSender(const FeedItem& f, const NodeInfo& n) {
  return f.has_sender && memcmp(f.sender, n.pub_prefix, sizeof(f.sender)) == 0;
}

bool feedFromNode(const MeshModel& m, const FeedItem& f, const NodeInfo& n) {
  if (f.has_sender) return isSender(f, n);   // an advert: whose it is is known
  if (!f.via.valid()) return false;
  // The last hop: a repeater the hash resolves to uniquely (only repeaters
  // forward); any other node only for what it sent straight to the M5.
  if (n.type == ADV_TYPE_REPEATER && f.hops > 0) return m.findByHash(f.via) == &n;
  return f.hops == 0 && uniqueNode(m, f.via) == &n;
}

bool routeThroughNode(const MeshModel& m, const FeedItem& f, const NodeInfo& n) {
  if (f.route_len == 0) return false;
  if (routeHasSender(f) && isSender(f, n)) return true;   // its own advert
  if (n.type != ADV_TYPE_REPEATER) return false;
  const uint8_t first = routeHasSender(f) ? 1 : 0;
  for (uint8_t r = first; r < f.route_len; r++)
    if (f.route[r].matchesKey(n.pub_prefix) && m.findByHash(f.route[r]) == &n) return true;
  return false;
}

const NodeInfo* findNode(const MeshModel& m, const uint8_t key[4]) {
  for (size_t i = 0; i < m.nodeCount(); i++) {
    const NodeInfo* n = m.node(i);
    if (memcmp(n->pub_prefix, key, 4) == 0) return n;
  }
  return nullptr;
}

NodeCard::NodeCard()
    : _ctx(nullptr), _open(false), _drawn(false), _drawnMs(0), _drawnGen(0), _pressClose(false) {
  memset(_key, 0, sizeof(_key));
}

void NodeCard::open(const uint8_t key[4]) {
  memcpy(_key, key, 4);
  _open = true;
  _drawn = false;
  _pressClose = false;
}

const NodeInfo* NodeCard::node() const {
  if (_ctx == nullptr || _ctx->model == nullptr) return nullptr;
  return findNode(*_ctx->model, _key);
}

bool NodeCard::wantsFrame(uint32_t now_ms) const {
  if (!_open) return false;
  if (!_drawn) return true;
  const uint32_t since = now_ms - _drawnMs;
  if (_ctx != nullptr && _ctx->data_gen != _drawnGen && since >= kMinDataGapMs) return true;
  return since >= kRefreshMs;
}

bool NodeCard::touch(int x, int y, bool down) {
  if (!_open) return false;
  const bool onClose = x >= kCloseX - 4 && x < kCloseX + kCloseW + 4 && y >= kCloseY - 4 && y < kCloseY + kCloseH + 4;
  if (down) {
    _pressClose = onClose;
    return false;
  }
  const bool closing = _pressClose && onClose;
  _pressClose = false;
  if (closing) _open = false;
  return closing;
}

void NodeCard::draw(FrameBuffer565& fb, uint32_t now_ms) {
  _drawn = true;
  _drawnMs = now_ms;
  _drawnGen = _ctx != nullptr ? _ctx->data_gen : 0;
  const NodeInfo* n = node();
  char title[40];
  if (n != nullptr) widgets::nodeName(title, sizeof(title), *n);
  else snprintf(title, sizeof(title), "NODE %02X%02X%02X%02X", _key[0], _key[1], _key[2], _key[3]);
  drawPanel(fb, kRect, title, nullptr);
  fb.fillRoundRect(kCloseX, kCloseY, kCloseW, kCloseH, 4, theme::kTileOnFill);
  fb.strokeRoundRect(kCloseX, kCloseY, kCloseW, kCloseH, 4, theme::kAccent);
  fb.text(kCloseX + kCloseW / 2, kCloseY + kCloseH / 2, "CLOSE  X", theme::kAccent, kFontSmall,
          FrameBuffer565::kCentreMiddle);
  if (n == nullptr) {
    fb.text(kRect.x + kRect.w / 2, 150, "This node is no longer in the head's table", theme::kTextDim, kFontSmall,
            FrameBuffer565::kCentreTop);
    return;
  }
  // Who: type, key, last heard, hops, packets.
  char age[12], info[96];
  widgets::formatAge(age, sizeof(age), now_ms - n->last_heard_ms);
  snprintf(info, sizeof(info), "%s \xB7 %02X%02X%02X%02X \xB7 heard %s ago \xB7 advert %u hop%s \xB7 %lu packet%s",
           widgets::typeLong(n->type), n->pub_prefix[0], n->pub_prefix[1], n->pub_prefix[2], n->pub_prefix[3], age,
           (unsigned)n->hops, n->hops == 1 ? "" : "s", (unsigned long)n->packets, n->packets == 1 ? "" : "s");
  fb.text(kColL, 71, info, theme::kTextDim, kFontSmall);
  fb.vline(kSplitX, 94, 190, theme::kDivider);
  drawSignal(fb, *n);
  drawPath(fb, *n);
  drawPosition(fb, *n);
  drawPackets(fb, *n, now_ms);
}

void NodeCard::drawSignal(FrameBuffer565& fb, const NodeInfo& n) const {
  widgets::sectionLabel(fb, kColL, 94, "SIGNAL");
  // snr_hist is newest first: oldest left on the chart.
  int16_t v[32];
  const int count = (int)(sizeof(n.snr_hist) / sizeof(n.snr_hist[0]));
  for (int i = 0; i < count; i++) v[i] = n.snr_hist[count - 1 - i];
  int lo = 0, hi = 0;
  const int have = widgets::sparkRange(v, count, mesh::head::SNR_NONE, 5 * 4, lo, hi);
  const int bx = 48, by = 116, bw = 184, bh = 56;
  fb.fillRect(bx, by, bw, bh, theme::kOdoFill);
  fb.hline(bx, by + bh, bw, theme::kDivider);
  const widgets::SnrCell cell = widgets::snrCell(*_ctx->model, n);
  if (have == 0) {
    // Only heard through a relay: name it (every repeater the model heard)
    // and give the SNR the M5 heard its advert at.
    char via[48], fitted[48];
    if (cell.relayed && cell.via[0]) snprintf(via, sizeof(via), "heard via %s", cell.via);
    else snprintf(via, sizeof(via), "heard only via relays");
    widgets::fitText(fitted, sizeof(fitted), via, kFontSmall, kSplitX - 8 - (kColL + 66));
    fb.text(kColL + 66, 94, fitted, theme::kTextDim, kFontSmall);
    fb.text(bx + bw / 2, by + bh / 2, "no SNR samples", kDimText, kFontSmall, FrameBuffer565::kCentreMiddle);
    if (cell.relayed && cell.has_snr) {
      char snr[12], line[48];
      widgets::formatSnr(snr, sizeof(snr), cell.snr_q4);
      snprintf(line, sizeof(line), "Advert at %s dB from the relay", snr);
      fb.text(kColL, 180, line, theme::kText, kFontSmall);
    } else {
      fb.text(kColL, 180, "Signal is measured on the last hop only", theme::kTextDim, kFontSmall);
    }
    return;
  }
  char note[24];
  snprintf(note, sizeof(note), "%d sample%s", have, have == 1 ? "" : "s");
  fb.text(kSplitX - 8, 94, note, theme::kTextDim, kFontSmall, FrameBuffer565::kRightTop);
  // Range labels (whole dB).
  char a[16], b[16];
  snprintf(a, sizeof(a), "%+d", (hi >= 0 ? hi + 2 : hi - 2) / 4);
  snprintf(b, sizeof(b), "%+d", (lo >= 0 ? lo + 2 : lo - 2) / 4);
  fb.text(bx - 4, by - 2, a, theme::kTextDim, kFontSmall, FrameBuffer565::kRightTop);
  fb.text(bx - 4, by + bh - 14, b, theme::kTextDim, kFontSmall, FrameBuffer565::kRightTop);
  // 0 dB reference when in range.
  if (lo < 0 && hi > 0) {
    const int zy = widgets::sparkY(0, lo, hi, by + 3, bh - 6);
    fb.dashedLine(bx, zy, bx + bw - 1, zy, theme::kDivider, 3, 3, 0);
  }
  widgets::sparkline(fb, bx + 2, by + 3, bw - 4, bh - 6, v, count, mesh::head::SNR_NONE, lo, hi,
                     linkColour(n.last_snr_q4));
  char snr[12], line[48];
  widgets::formatSnr(snr, sizeof(snr), n.last_snr_q4);
  snprintf(line, sizeof(line), "SNR %s dB   RSSI %d dBm", snr, (int)n.last_rssi);
  fb.text(kColL, 180, line, theme::kText, kFontSmall);
}

void NodeCard::drawPath(FrameBuffer565& fb, const NodeInfo& n) const {
  const MeshModel& m = *_ctx->model;
  widgets::sectionLabel(fb, kColL, 204, "LAST PATH");
  const FeedItem* item = nullptr;
  for (size_t i = 0; i < m.feedCount() && item == nullptr; i++) {
    const FeedItem* f = m.feed(i);
    if (f == nullptr) break;
    if (routeThroughNode(m, *f, n)) item = f;
  }
  if (item == nullptr) {
    fb.text(kColL, 222, "No route through it heard yet", kDimText, kFontSmall);
    return;
  }
  // Hops in travel order, then the M5; wrapped at " > " into three lines.
  char hops[(int)mesh::head::FEED_ROUTE + 2][24];
  int count = 0;
  // The feed keeps the last FEED_ROUTE hops of a longer route.
  const int carried = item->payload_type == PAYLOAD_TYPE_ADVERT ? (int)item->hops + 1 : (int)item->hops;
  if (carried > (int)item->route_len) snprintf(hops[count++], sizeof(hops[0]), "...");
  for (uint8_t r = 0; r < item->route_len && count < (int)mesh::head::FEED_ROUTE + 1; r++)
    hopName(hops[count++], sizeof(hops[0]), m, item->route[r],
            r == 0 && routeHasSender(*item) && item->has_sender ? item->sender : nullptr);
  uint8_t rep[4];
  if (item->route_len == 0 || !(m.repeaterKey(rep) && item->route[item->route_len - 1].matchesKey(rep)))
    snprintf(hops[count++], sizeof(hops[0]), "M5");
  // Hops of this packet (the header's are the node's own advert).
  char note[40];
  snprintf(note, sizeof(note), "%s: this packet %u hop%s", payloadTypeName(item->payload_type),
           (unsigned)item->hops, item->hops == 1 ? "" : "s");
  fb.text(kSplitX - 8, 204, note, theme::kTextDim, kFontSmall, FrameBuffer565::kRightTop);
  char line[128];
  line[0] = 0;
  int y = 222;
  for (int i = 0; i < count && y <= 258; i++) {
    char next[128];
    next[0] = 0;
    append(next, sizeof(next), line);
    if (line[0]) append(next, sizeof(next), " > ");
    append(next, sizeof(next), hops[i]);
    if (line[0] && FrameBuffer565::textWidth(next, kFontSmall) > kTextW) {
      fb.text(kColL, y, line, theme::kText, kFontSmall);
      y += kLineH;
      line[0] = 0;
      append(line, sizeof(line), "> ");
      append(line, sizeof(line), hops[i]);
    } else {
      line[0] = 0;
      append(line, sizeof(line), next);
    }
  }
  if (line[0] && y <= 258) fb.text(kColL, y, line, theme::kText, kFontSmall);
}

void NodeCard::drawPosition(FrameBuffer565& fb, const NodeInfo& n) const {
  widgets::sectionLabel(fb, kColR, 94, "POSITION");
  int32_t lat = 0, lon = 0;
  const bool fix = _ctx->model->repeaterPosition(lat, lon);
  char l1[48], l2[48];
  widgets::positionLines(l1, sizeof(l1), l2, sizeof(l2), n, fix, lat, lon);
  const bool has = n.has_pos;
  fb.text(kColR, 112, l1, has ? theme::kText : kDimText, kFontSmall);
  fb.text(kColR, 130, l2, has && fix ? theme::kAccent : kDimText, kFontSmall);
}

void NodeCard::drawPackets(FrameBuffer565& fb, const NodeInfo& n, uint32_t now_ms) const {
  const MeshModel& m = *_ctx->model;
  widgets::sectionLabel(fb, kColR, 156, "LAST PACKETS");
  int shown = 0;
  for (size_t i = 0; i < m.feedCount() && shown < kLastPackets; i++) {
    const FeedItem* f = m.feed(i);
    if (f == nullptr) break;
    if (!feedFromNode(m, *f, n)) continue;
    const int y = 174 + shown * kLineH;
    char age[12], hops[12], sig[16];
    widgets::formatAge(age, sizeof(age), now_ms - f->ms);
    snprintf(hops, sizeof(hops), "%u hop%s", (unsigned)f->hops, f->hops == 1 ? "" : "s");
    fb.text(kColR, y, age, theme::kTextDim, kFontSmall);
    fb.text(kColR + 40, y, payloadTypeName(f->payload_type), feedTypeColour(f->payload_type), kFontSmall);
    fb.text(kColR + 92, y, hops, theme::kText, kFontSmall);
    if (f->observed) {
      widgets::formatSnr(sig, sizeof(sig), f->snr_q4);
      fb.text(kRect.x + kRect.w - 12, y, sig, linkColour(f->snr_q4), kFontSmall, FrameBuffer565::kRightTop);
    } else {
      fb.text(kRect.x + kRect.w - 12, y, "ESP-NOW", theme::kEsp, kFontSmall, FrameBuffer565::kRightTop);
    }
    shown++;
  }
  if (shown == 0) fb.text(kColR, 174, "None traced to it in the feed", kDimText, kFontSmall);
}

}  // namespace freenove_ui
