#include "HomePage.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Packet.h>
#include <helpers/AdvertDataHelpers.h>

#include "../HeadContactPolicy.h"
#include "RadioLabel.h"
#include "StubPages.h"
#include "Widgets.h"

namespace freenove_ui {

using mesh::head::FeedItem;
using mesh::head::MeshModel;
using mesh::head::NodeInfo;
using mesh::head::PathHash;

namespace {

const char* const kRepeaterLabel = "M5";
const uint32_t kRepeaterKey = 0;            // edge endpoint key of the repeater
const uint32_t kUnknownKey = 0xFFFFFFFFu;   // a hash that resolves to nothing

uint32_t prefixKey(const uint8_t* p) {
  const uint32_t k = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
  if (k == kRepeaterKey) return 1u;
  return k != kUnknownKey ? k : kUnknownKey - 1u;
}
uint32_t nodeKey(const NodeInfo& n) { return prefixKey(n.pub_prefix); }

// Screen boxes already taken by labels and dots (label collision avoidance).
struct Boxes {
  enum { kMax = 96 };
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

// "+6.5" / "-7.2" from quarter-dB SNR, one decimal, no floats.
void formatSnr(char* out, size_t cap, int snr_q4) {
  const int tenths = abs(snr_q4) * 10 / 4;
  snprintf(out, cap, "%c%d.%d", snr_q4 < 0 ? '-' : '+', tenths / 10, tenths % 10);
}

// Copy at most max_chars of s (names are cut, never wrapped).
void clip(char* out, size_t cap, const char* s, size_t max_chars) {
  size_t n = 0;
  while (s[n] && n < max_chars && n + 1 < cap) {
    out[n] = s[n];
    n++;
  }
  out[n] = 0;
}

}  // namespace

void labelName(char* out, size_t cap, const char* name, size_t max_chars) {
  if (cap == 0) return;
  const size_t len = strlen(name);
  if (len <= max_chars || max_chars < 2) {
    clip(out, cap, name, max_chars);
    return;
  }
  clip(out, cap, name, max_chars - 1);   // cut: say so
  const size_t n = strlen(out);
  if (n + 1 < cap) { out[n] = '~'; out[n + 1] = 0; }
}

namespace {

void nodeName(char* out, size_t cap, const NodeInfo* n, size_t max_chars) {
  if (n == nullptr) { clip(out, cap, "?", max_chars); return; }
  if (n->name[0]) {
    char shown[40];   // emoji dropped; an emoji-only name -> "rptr 57f72334"
    head_policy::displayName(n->name, n->pub_prefix, n->type, shown, sizeof(shown));
    labelName(out, cap, shown, max_chars);
    return;
  }
  char hex[12];
  snprintf(hex, sizeof(hex), "%02X%02X", n->pub_prefix[0], n->pub_prefix[1]);
  clip(out, cap, hex, max_chars);
}

}  // namespace

uint16_t feedTypeColour(uint8_t type) {
  switch (type) {
    case PAYLOAD_TYPE_ADVERT:
    case PAYLOAD_TYPE_ACK: return theme::kFeedAdv;
    case PAYLOAD_TYPE_GRP_TXT:
    case PAYLOAD_TYPE_GRP_DATA: return theme::kFeedGrp;
    case PAYLOAD_TYPE_TXT_MSG: return theme::kFeedTxt;
    default: return theme::kFeedTrc;
  }
}

namespace {

int iround(float v) { return (int)lroundf(v); }

}  // namespace

uint16_t linkColour(int8_t snr_q4) {
  if (snr_q4 >= 5 * 4) return theme::kSigStrong;
  if (snr_q4 >= 0) return theme::kSigGood;
  if (snr_q4 >= -7 * 4) return theme::kSigWeak;
  return theme::kSigPoor;
}

const PanelRect HomePage::kTopoPanel = {6, 42, 288, 196};
const PanelRect HomePage::kNoisePanel = {300, 42, 174, 98};
const PanelRect HomePage::kPacketsPanel = {300, 144, 174, 94};

HomePage::HomePage()
    : _card(nullptr), _pressing(false), _pressX(0), _pressY(0), _mode(kGraph), _drawn(false), _drawnGen(0), _drawnMs(0), _syncGen(0), _syncMs(0), _synced(false),
      _feedSeq(0), _pathSeq(0), _graph((float)(kAreaW - kGraphRightMargin), (float)kAreaH), _geo(kAreaW, kAreaH, 14),
      _geoCentreKnown(false), _noGps(0), _edgeCount(0), _pathLen(0), _pathStart(0),
      _pathAnim(false), _captionTypeId(0xFF), _captionColour(theme::kAccent), _pressOnToggle(false), _pressTile(-1), _quiet(true),
      _lastDrawAnimated(false), _centrePinged(false), _centrePingMs(0), _request(kReqNone), _quickDrawn(0) {
  memset(_geoHas, 0, sizeof(_geoHas));
  memset(_geoX, 0, sizeof(_geoX));
  memset(_geoY, 0, sizeof(_geoY));
  memset(_edges, 0, sizeof(_edges));
  memset(_path, 0, sizeof(_path));
  memset(_pings, 0, sizeof(_pings));
  for (int i = 0; i < kMaxPings; i++) _pings[i].slot = -1;
  for (int i = 0; i < GraphLayout::kMaxNodes; i++) _slotNode[i] = nullptr;
  memset(_labOn, 0, sizeof(_labOn));
  memset(_labX, 0, sizeof(_labX));
  memset(_labY, 0, sizeof(_labY));
  memset(_labW, 0, sizeof(_labW));
  memset(_labH, 0, sizeof(_labH));
  _caption[0] = 0;
  _captionType[0] = 0;
}

int HomePage::panels(const PanelRect*& out) const {
  static const PanelRect kAll[3] = {kTopoPanel, kNoisePanel, kPacketsPanel};
  out = kAll;
  return 3;
}

void HomePage::onShow(uint32_t now_ms) {
  (void)now_ms;
  _drawn = false;
  _quiet = true;   // what arrived while hidden is shown, not replayed
  _pressOnToggle = false;
  _pressTile = -1;
  _pressing = false;
  _quick.cancel();
  if (_card != nullptr) _card->close();
}

void HomePage::onHide() {
  if (_card != nullptr) _card->close();
}

int HomePage::contactAt(int x, int y) const {
  if (x < kAreaX || x >= kAreaX + kAreaW || y < kAreaY || y >= kAreaY + kAreaH) return -1;
  int best = -1, bestD = kHitR * kHitR + 1;
  for (int s = 1; s < GraphLayout::kMaxNodes; s++) {
    int cx, cy;
    if (_slotNode[s] == nullptr || !slotPos(s, cx, cy)) continue;
    const int d = (cx - x) * (cx - x) + (cy - y) * (cy - y);
    if (d < bestD) { bestD = d; best = s; }
  }
  return best;
}

// ------------------------------------------------------------------ input

int HomePage::tileAt(int x, int y) {
  if (y < kTileY - 2 || y >= kTileY + kTileH + 2 || x < kTileX - 2) return -1;
  const int i = (x - kTileX + 2) / kTilePitch;
  if (i >= hmi::kTileCount) return -1;
  const int rx = x - (kTileX + i * kTilePitch);
  return rx >= -2 && rx < kTileW + 2 ? i : -1;
}

bool HomePage::touch(int x, int y, bool down) {
  if (_card != nullptr && _card->isOpen()) {
    const bool changed = _card->touch(x, y, down);
    if (changed && !_card->isOpen()) {
      _drawn = false;
      _quiet = true;   // what arrived under the card is shown, not replayed
    }
    return changed;
  }
  const bool onToggle = x >= kToggleX && x < kToggleX + kToggleW && y >= kToggleY - 2 && y < kToggleY + kToggleH;
  const int tile = tileAt(x, y);
  if (down) {
    // Where the press began decides (later downs are the finger moving).
    if (!_pressing) {
      _pressing = true;
      _pressX = (int16_t)x;
      _pressY = (int16_t)y;
      _pressOnToggle = onToggle;
      _pressTile = tile;
    }
    return false;
  }
  const bool still = _pressing && abs(x - _pressX) < kTapSlop && abs(y - _pressY) < kTapSlop;
  _pressing = false;
  const bool toggle = _pressOnToggle && onToggle;
  const int pressed = _pressTile;
  _pressOnToggle = false;
  _pressTile = -1;
  if (toggle) {
    _mode = _mode == kGraph ? kGeo : kGraph;
    _quick.cancel();
    return true;
  }
  // A tap that is not on a tile (or slid off the tile it began on) cancels
  // a pending confirm; never sends.
  const uint32_t now = _ctx != nullptr ? _ctx->now_ms : _drawnMs;
  const bool wasPending = _quick.pending(now);
  // A tap on a contact (no confirm pending): its node card.
  if (!wasPending && still && pressed < 0 && tile < 0 && _card != nullptr) {
    const int s = contactAt(x, y);
    if (s > 0) {
      // By the slot's key (the model may have changed since the last sync).
      const uint32_t k = _graph.key(s);
      const uint8_t key[4] = {(uint8_t)(k >> 24), (uint8_t)(k >> 16), (uint8_t)(k >> 8), (uint8_t)k};
      _card->open(key);
      return true;
    }
  }
  const int t = pressed >= 0 && pressed == tile ? tile : -1;
  const hmi::QuickAction a = _quick.tap(t, now);
  switch (a) {
    case hmi::kActAdvert: _request = kReqAdvert; break;
    case hmi::kActOpenHead: _request = kReqOpenHeadAdmin; break;
    case hmi::kActAdminLogin: _request = kReqAdminLogin; break;
    case hmi::kActOpenPresets: _request = kReqOpenPresets; break;
    case hmi::kActOpenSettings: _request = kReqOpenSettings; break;
    default: break;
  }
  return t >= 0 || wasPending;
}

PageRequest HomePage::takeRequest() {
  const PageRequest r = _request;
  _request = kReqNone;
  return r;
}

void HomePage::requestDone(PageRequest req, bool ok, uint32_t now_ms) {
  if (req == kReqAdvert) _quick.setResult(hmi::kTileAdvert, ok, now_ms);
}

// ------------------------------------------------------------ scheduling

bool HomePage::pinging(uint32_t now_ms) const {
  for (int i = 0; i < kMaxPings; i++)
    if (_pings[i].slot >= 0 && (uint32_t)(now_ms - _pings[i].start) < kPingMs) return true;
  return false;
}

bool HomePage::animating(uint32_t now_ms) const {
  if (_pathAnim && (uint32_t)(now_ms - _pathStart) < kPathAnimMs) return true;
  if (pinging(now_ms)) return true;
  if (_needle.settling() || _odo.rolling(now_ms)) return true;
  return _mode == kGraph && _graph.hot();
}

// What the quick-control row shows (pending confirm / result), so a change
// (including a timeout) gets a frame on time.
uint32_t HomePage::quickSignature(uint32_t now_ms) const {
  uint32_t sig = 0;
  for (int i = 0; i < hmi::kTileCount; i++) {
    sig = sig * 7u + (_quick.confirming(i, now_ms) ? 1u : 0u);
    sig = sig * 7u + (uint32_t)(_quick.result(i, now_ms) + 1);
  }
  // The ADVERT cooldown counts down in whole seconds.
  const uint32_t left = _quick.cooldownLeftMs(hmi::kTileAdvert, now_ms);
  return sig * 31u + (left + 999u) / 1000u;
}

bool HomePage::wantsFrame(uint32_t now_ms) {
  if (_card != nullptr && _card->isOpen()) return _card->wantsFrame(now_ms);
  if (!_drawn || _lastDrawAnimated) return true;
  if (_ctx != nullptr && _ctx->data_gen != _drawnGen) return true;
  if (animating(now_ms)) return true;
  if (quickSignature(now_ms) != _quickDrawn) return true;
  if (sweeping()) return (uint32_t)(now_ms - _drawnMs) >= kSweepFrameMs;   // sweep, <= 20 fps
  return (uint32_t)(now_ms - _drawnMs) >= 1000;   // ages, stale vitals
}

// ------------------------------------------------------------------ model

void HomePage::sync(uint32_t now_ms) {
  const MeshModel* m = _ctx != nullptr ? _ctx->model : nullptr;
  if (m == nullptr || !m->ok()) return;
  const uint32_t gen = _ctx->data_gen;
  if (_synced && gen == _syncGen && (uint32_t)(now_ms - _syncMs) < 1000) return;
  const bool animate = _synced && !_quiet;
  _synced = true;
  _quiet = false;
  _syncGen = gen;
  _syncMs = now_ms;

  const uint32_t fresh = m->feedSeq() - _feedSeq;
  learnRoutes(fresh);   // links first: new nodes are placed next to a neighbour
  syncNodes(now_ms);
  bool tookPath = false;
  if (m->pathSeq() != _pathSeq) {
    _pathSeq = m->pathSeq();
    takePath(animate, now_ms);
    tookPath = true;
  }
  rebuildLinks(now_ms);
  if (fresh != 0) {
    // The packet whose route animates already pings its sender.
    if (animate) takeFeed(now_ms, tookPath && _pathAnim ? m->lastPathItem() : nullptr);
    _feedSeq = m->feedSeq();
  }
  refreshGeo();
}

// Who is on screen (at most kMaxShown nodes). A shown node keeps its slot
// while it has been heard within kKeepMs; free slots go to the most recently
// heard nodes; a stale shown node is replaced only by a fresher waiting one.
// So on a large mesh the picture does not churn: newcomers wait for a slot
// instead of evicting someone heard a minute ago.
void HomePage::syncNodes(uint32_t now_ms) {
  const MeshModel* m = _ctx->model;
  // Map shown slots to model nodes; drop the ones the model no longer has.
  for (int s = 0; s < GraphLayout::kMaxNodes; s++) _slotNode[s] = nullptr;
  for (size_t i = 0; i < m->nodeCount(); i++) {
    const int s = _graph.find(nodeKey(*m->node(i)));
    if (s > 0) _slotNode[s] = m->node(i);
  }
  for (int s = 1; s < GraphLayout::kMaxNodes; s++)
    if (_graph.used(s) && _slotNode[s] == nullptr) dropSlot(s);

  // Waiting nodes, freshest first (top kMaxShown only).
  int wait[kMaxShown];
  uint32_t wage[kMaxShown];
  int waiting = 0;
  for (size_t i = 0; i < m->nodeCount(); i++) {
    const NodeInfo& n = *m->node(i);
    if (_graph.find(nodeKey(n)) >= 0) continue;
    const uint32_t a = now_ms - n.last_heard_ms;
    int at = waiting;
    while (at > 0 && wage[at - 1] > a) at--;
    if (at >= kMaxShown) continue;
    const int last = waiting < kMaxShown ? waiting : kMaxShown - 1;
    for (int j = last; j > at; j--) { wait[j] = wait[j - 1]; wage[j] = wage[j - 1]; }
    wait[at] = (int)i;
    wage[at] = a;
    if (waiting < kMaxShown) waiting++;
  }

  // Nodes the repeater hears directly link to the centre, stamped with when
  // they were last heard so the link expires (kLinkTtlMs) like route links.
  for (size_t i = 0; i < m->nodeCount(); i++) {
    const NodeInfo& n = *m->node(i);
    if (hasSignal(n) && (uint32_t)(now_ms - n.last_heard_ms) < kLinkTtlMs)
      addEdge(nodeKey(n), kRepeaterKey, n.last_heard_ms);
  }

  for (int next = 0; next < waiting; next++) {
    const NodeInfo& n = *m->node((size_t)wait[next]);
    if (_graph.count() >= kMaxShown) {
      // Full: replace the stalest shown node, only if it is stale and older.
      int victim = -1;
      uint32_t vage = 0;
      for (int s = 1; s < GraphLayout::kMaxNodes; s++) {
        if (_slotNode[s] == nullptr) continue;
        const uint32_t a = now_ms - _slotNode[s]->last_heard_ms;
        if (a >= kKeepMs && a > vage) { victim = s; vage = a; }
      }
      if (victim < 0 || vage <= wage[next]) break;
      dropSlot(victim);
    }
    const uint32_t k = nodeKey(n);
    int anchor = -1;
    for (int e = 0; e < _edgeCount && anchor < 0; e++) {
      const uint32_t other = _edges[e].a == k ? _edges[e].b : (_edges[e].b == k ? _edges[e].a : kUnknownKey);
      if (other == kRepeaterKey) anchor = 0;
      else if (other != kUnknownKey) anchor = _graph.find(other);
    }
    const int hops = hasSignal(n) ? 0 : (n.hops > 0 ? n.hops : 1);
    const int s = _graph.add(k, hops, anchor);
    if (s > 0) _slotNode[s] = &n;
  }
}

// A slot leaves the screen: nothing may keep pointing at it (it is reused).
void HomePage::dropSlot(int s) {
  _graph.remove(_graph.key(s));
  _slotNode[s] = nullptr;
  for (int i = 0; i < _pathLen; i++) if (_path[i] == s) _path[i] = -1;
  for (int i = 0; i < kMaxPings; i++) if (_pings[i].slot == s) _pings[i].slot = -1;
}

// Re-hearing a link only moves its time forward (a sync re-adding the same
// observation changes nothing); when the table is full the link heard
// longest ago makes room.
void HomePage::addEdge(uint32_t a, uint32_t b, uint32_t ms) {
  if (a == b || a == kUnknownKey || b == kUnknownKey) return;
  if (a > b) { const uint32_t t = a; a = b; b = t; }
  int victim = 0;
  for (int i = 0; i < _edgeCount; i++) {
    if (_edges[i].a == a && _edges[i].b == b) {
      if ((int32_t)(ms - _edges[i].ms) > 0) _edges[i].ms = ms;
      return;
    }
    if ((int32_t)(_edges[i].ms - _edges[victim].ms) < 0) victim = i;
  }
  if (_edgeCount == kMaxEdges && (int32_t)(ms - _edges[victim].ms) <= 0) return;   // older than all
  const int at = _edgeCount < kMaxEdges ? _edgeCount++ : victim;
  _edges[at].a = a;
  _edges[at].b = b;
  _edges[at].ms = ms;
}

void HomePage::rebuildLinks(uint32_t now_ms) {
  int la[kMaxEdges], lb[kMaxEdges];
  bool fresh[kMaxEdges];
  int n = 0;
  for (int i = 0; i < _edgeCount; i++) {
    if ((uint32_t)(now_ms - _edges[i].ms) >= kLinkTtlMs) continue;   // not heard lately
    const int a = _edges[i].a == kRepeaterKey ? 0 : _graph.find(_edges[i].a);
    const int b = _edges[i].b == kRepeaterKey ? 0 : _graph.find(_edges[i].b);
    if (a < 0 || b < 0) continue;
    la[n] = a; lb[n] = b;
    fresh[n] = !_graph.linked(a, b);
    n++;
  }
  _graph.clearLinks();
  for (int i = 0; i < n; i++) {
    _graph.link(la[i], lb[i]);
    if (fresh[i]) {   // a newly learnt link pulls its ends together a little
      _graph.warm(la[i], 3.0f);
      _graph.warm(lb[i], 3.0f);
    }
  }
}

int HomePage::slotForHash(const PathHash& h) const {
  if (!h.valid()) return -1;
  uint8_t rep4[4];
  if (_ctx->model->repeaterKey(rep4) && h.matchesKey(rep4)) return 0;
  int any = -1, anyCount = 0, rep = -1, repCount = 0;
  for (int s = 1; s < GraphLayout::kMaxNodes; s++) {
    const NodeInfo* n = _slotNode[s];
    if (n == nullptr || !h.matchesKey(n->pub_prefix)) continue;
    any = s; anyCount++;
    if (n->type == ADV_TYPE_REPEATER) { rep = s; repCount++; }
  }
  if (repCount == 1) return rep;
  return anyCount == 1 ? any : -1;
}

uint32_t HomePage::keyForHash(const PathHash& h) const {
  const MeshModel* m = _ctx->model;
  if (!h.valid()) return kUnknownKey;
  uint8_t rep4[4];
  if (m->repeaterKey(rep4) && h.matchesKey(rep4)) return kRepeaterKey;
  const NodeInfo* any = nullptr;
  const NodeInfo* rep = nullptr;
  int anyCount = 0, repCount = 0;
  for (size_t i = 0; i < m->nodeCount(); i++) {
    const NodeInfo* n = m->node(i);
    if (!h.matchesKey(n->pub_prefix)) continue;
    any = n; anyCount++;
    if (n->type == ADV_TYPE_REPEATER) { rep = n; repCount++; }
  }
  if (repCount == 1) return nodeKey(*rep);
  return anyCount == 1 ? nodeKey(*any) : kUnknownKey;
}

// Links heard on air from the routes of new feed items: neighbours on a
// path, and the last hop with the repeater that received it.
void HomePage::learnRoutes(uint32_t fresh) {
  const MeshModel* m = _ctx->model;
  const size_t scan = fresh < kMaxLearn ? fresh : kMaxLearn;
  for (size_t i = scan; i-- > 0;) {   // oldest first: the newest links win LRU
    const FeedItem* f = m->feed(i);
    if (f == nullptr) continue;
    uint32_t prev = kUnknownKey;
    for (uint8_t r = 0; r < f->route_len; r++) {
      const uint32_t k = keyForHash(f->route[r]);
      addEdge(prev, k, f->ms);   // unknown ends are ignored
      if (r + 1 == f->route_len) addEdge(k, kRepeaterKey, f->ms);
      prev = k;
    }
  }
}

void HomePage::takePath(bool animate, uint32_t now_ms) {
  const MeshModel* m = _ctx->model;
  PathHash hashes[65];
  const size_t n = m->lastPath(hashes, sizeof(hashes) / sizeof(hashes[0]));
  _pathLen = 0;
  char route[48];
  size_t used = 0;
  route[0] = 0;
  for (size_t i = 0; i < n; i++) {
    const int s = slotForHash(hashes[i]);
    if (s >= 0 && (_pathLen == 0 || _path[_pathLen - 1] != s) && _pathLen < kMaxPath - 1)
      _path[_pathLen++] = (int8_t)s;
    char name[12];
    if (s > 0) nodeName(name, sizeof(name), _slotNode[s], 8);
    else if (s == 0) clip(name, sizeof(name), kRepeaterLabel, 8);
    else {
      // Not on the graph: any repeater the model heard, else the hash.
      char full[28];
      widgets::relayName(full, sizeof(full), *m, hashes[i]);
      labelName(name, sizeof(name), full, 8);
    }
    if (used + strlen(name) + 2 < sizeof(route) - 4) {
      used += (size_t)snprintf(route + used, sizeof(route) - used, "%s>", name);
    } else if (used + 3 < sizeof(route)) {
      used += (size_t)snprintf(route + used, sizeof(route) - used, "..>");
      break;
    }
  }
  if (used + strlen(kRepeaterLabel) < sizeof(route))
    snprintf(route + used, sizeof(route) - used, "%s", kRepeaterLabel);
  if (_pathLen == 0 || _path[_pathLen - 1] != 0) _path[_pathLen++] = 0;

  const FeedItem* item = m->lastPathItem();
  char sig[16] = "";
  if (item != nullptr && item->observed) {
    formatSnr(sig, sizeof(sig), item->snr_q4);
    strncat(sig, "dB", sizeof(sig) - strlen(sig) - 1);
  } else if (item != nullptr) {
    snprintf(sig, sizeof(sig), "ESP-NOW");
  }
  snprintf(_captionType, sizeof(_captionType), "%s", item != nullptr ? payloadTypeName(item->payload_type) : "?");
  _captionTypeId = item != nullptr ? item->payload_type : 0xFF;
  snprintf(_caption, sizeof(_caption), "%s  %s", route, sig);
  _captionColour = item != nullptr && !item->observed ? theme::kEsp : theme::kAccent;

  _pathAnim = animate && _pathLen > 1;
  _pathStart = now_ms;
  if (_pathAnim && _path[0] > 0) startPing(_path[0], slotColour(_path[0], now_ms), now_ms);
}

void HomePage::takeFeed(uint32_t now_ms, const FeedItem* pathItem) {
  const MeshModel* m = _ctx->model;
  const uint32_t fresh = m->feedSeq() - _feedSeq;
  for (uint32_t i = 0; i < fresh && i < 3; i++) {
    const FeedItem* f = m->feed(i);
    if (f == nullptr) break;
    if (f == pathItem) continue;
    const int s = slotForHash(f->via);
    if (s > 0) startPing(s, slotColour(s, now_ms), now_ms);
    else if (!_centrePinged || (uint32_t)(now_ms - _centrePingMs) >= kCentrePingGapMs) {
      // The repeater heard something from an unknown node: at most one ring
      // per kCentrePingGapMs, so a busy mesh does not keep HOME animating.
      startPing(0, theme::kRepeater, now_ms);
      _centrePinged = true;
      _centrePingMs = now_ms;
    }
  }
}

void HomePage::startPing(int slot, uint16_t colour, uint32_t now_ms) {
  int at = 0;
  for (int i = 0; i < kMaxPings; i++) {
    if (_pings[i].slot == slot) { at = i; break; }   // restart, never stack
    if (_pings[i].slot < 0 || (uint32_t)(now_ms - _pings[i].start) >= kPingMs) { at = i; break; }
    if ((int32_t)(_pings[i].start - _pings[at].start) < 0) at = i;
  }
  _pings[at].slot = (int8_t)slot;
  _pings[at].colour = colour;
  _pings[at].start = now_ms;
}

void HomePage::refreshGeo() {
  const MeshModel* m = _ctx->model;
  GeoPoint pts[GraphLayout::kMaxNodes];
  size_t n = 0;
  int64_t sumLat = 0, sumLon = 0;
  for (int s = 1; s < GraphLayout::kMaxNodes; s++) {
    const NodeInfo* nd = _slotNode[s];
    if (nd == nullptr || !nd->has_pos) continue;
    pts[n].lat_e6 = nd->lat_e6;
    pts[n].lon_e6 = nd->lon_e6;
    sumLat += nd->lat_e6;
    sumLon += nd->lon_e6;
    n++;
  }
  _noGps = 0;
  for (size_t i = 0; i < m->nodeCount(); i++) _noGps += m->node(i)->has_pos ? 0 : 1;
  int32_t lat0 = 0, lon0 = 0;
  _geoCentreKnown = m->repeaterPosition(lat0, lon0);
  if (!_geoCentreKnown && n > 0) {
    lat0 = (int32_t)(sumLat / (int64_t)n);
    lon0 = (int32_t)(sumLon / (int64_t)n);
  }
  _geo.fit(lat0, lon0, pts, n);
  for (int s = 0; s < GraphLayout::kMaxNodes; s++) {
    _geoHas[s] = false;
    const NodeInfo* nd = _slotNode[s];
    if (nd == nullptr || !nd->has_pos) continue;
    int x = 0, y = 0;
    if (_geo.project(nd->lat_e6, nd->lon_e6, x, y)) {
      _geoHas[s] = true;
      _geoX[s] = (int16_t)x;
      _geoY[s] = (int16_t)y;
    }
  }
}

uint16_t HomePage::slotColour(int slot, uint32_t now_ms) const {
  if (slot == 0) return theme::kRepeater;
  const NodeInfo* n = slot > 0 && slot < GraphLayout::kMaxNodes ? _slotNode[slot] : nullptr;
  if (n == nullptr || !hasSignal(*n) || (uint32_t)(now_ms - n->last_heard_ms) >= kDimAfterMs) return theme::kStale;
  return linkColour(n->last_snr_q4);
}

bool HomePage::slotPos(int slot, int& x, int& y) const {
  if (slot < 0 || slot >= GraphLayout::kMaxNodes || !_graph.used(slot)) return false;
  if (_mode == kGraph) {
    x = kAreaX + iround(_graph.x(slot));
    y = kAreaY + iround(_graph.y(slot));
    return true;
  }
  if (slot == 0) {
    if (!_geoCentreKnown) return false;
    x = kAreaX + _geo.cx();
    y = kAreaY + _geo.cy();
    return true;
  }
  if (!_geoHas[slot]) return false;
  x = kAreaX + _geoX[slot];
  y = kAreaY + _geoY[slot];
  return true;
}

// ------------------------------------------------------------------ drawing

void HomePage::drawBackground(FrameBuffer565& fb) {
  drawPanel(fb, kTopoPanel, "MESH TOPOLOGY", nullptr);
  // Noise floor: the gauge's track and its quiet / noisy zones.
  drawPanel(fb, kNoisePanel, "NOISE FLOOR", "dBm");
  const float gx = (float)kGaugeX, gy = (float)kGaugeY, gr = (float)kGaugeR, gt = (float)kGaugeThick;
  const float a0 = (float)hmi::kGaugeStartDeg, a1 = a0 + (float)hmi::kGaugeSweepDeg;
  fb.aaArc(gx, gy, gr, gt, a0, a1, theme::kGaugeTrack);
  fb.aaArc(gx, gy, gr, gt, a0, hmi::gaugeAngle((float)hmi::kQuietMaxDbm), theme::kGood);
  fb.aaArc(gx, gy, gr, gt, hmi::gaugeAngle((float)hmi::kNoisyMinDbm), a1, theme::kWarn);
  // Packets: the odometer's windows.
  drawPanel(fb, kPacketsPanel, "PACKETS", "since boot");
  for (int i = 0; i < kOdoDigits; i++) {
    const int bx = kOdoX + i * kOdoPitch;
    fb.fillRoundRect(bx, kOdoY, kOdoW, kOdoH, 3, theme::kOdoFill);
    fb.strokeRoundRect(bx, kOdoY, kOdoW, kOdoH, 3, theme::kOdoEdge);
  }
}

bool HomePage::sweeping() const { return _mode == kGeo && _geoCentreKnown; }

void HomePage::draw(FrameBuffer565& fb, uint32_t now_ms) {
  _drawn = true;
  _drawnMs = now_ms;
  _drawnGen = _ctx != nullptr ? _ctx->data_gen : 0;
  if (_card != nullptr && _card->isOpen()) {
    _card->draw(fb, now_ms);
    _lastDrawAnimated = false;
    return;
  }
  drawToggle(fb);
  const MeshModel* m = _ctx != nullptr ? _ctx->model : nullptr;
  if (m == nullptr || !m->ok()) {
    fb.text(kAreaX + 16, kAreaY + 16, "MODEL UNAVAILABLE", theme::kBad, kFontBody);
    _lastDrawAnimated = false;
    return;
  }
  sync(now_ms);
  // Layout: a few cooling steps per frame (a new node glides in); in GEO
  // the graph is not visible, so it settles at once.
  if (_mode == kGraph) {
    for (int i = 0; i < 3 && _graph.hot(); i++) _graph.step();
  } else {
    for (int i = 0; i < 300 && _graph.hot(); i++) _graph.step();
  }
  drawTopology(fb, now_ms);
  drawCaption(fb, now_ms);
  drawNoise(fb, now_ms);
  drawPackets(fb, now_ms);
  drawQuick(fb, now_ms);
  _quickDrawn = quickSignature(now_ms);
  _lastDrawAnimated = animating(now_ms);
}

// The panel's header note names the view and toggles it.
void HomePage::drawToggle(FrameBuffer565& fb) const {
  const char* note = _mode == kGraph ? "GRAPH" : "GEO";
  fb.text(kTopoPanel.x + kTopoPanel.w - 8, kTopoPanel.y + 6, note, theme::kAccent, kFontSmall,
          FrameBuffer565::kRightTop);
}

// Range rings are drawn by drawTopology (their labels need the collision
// boxes); this adds the compass ticks and the sweep.
void HomePage::drawGeoOverlay(FrameBuffer565& fb, int cx, int cy, uint32_t now_ms) const {
  const int R = (kAreaW < kAreaH ? kAreaW : kAreaH) / 2 - 6;
  // N/E/S/W: a tick inward from the outer ring and the letter inside it.
  static const char* const kDir[4] = {"N", "E", "S", "W"};
  const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
  for (int i = 0; i < 4; i++) {
    fb.aaLine((float)(cx + dx[i] * R), (float)(cy + dy[i] * R), (float)(cx + dx[i] * (R - 6)),
              (float)(cy + dy[i] * (R - 6)), 1.0f, theme::kTextDim);
    fb.text(cx + dx[i] * (R - 14), cy + dy[i] * (R - 14), kDir[i], theme::kTextDim, kFontSmall,
            FrameBuffer565::kCentreMiddle);
  }
  // Sweep: the beam and two fading trail lines, clockwise from north.
  const float turn = (float)(now_ms % kSweepTurnMs) / (float)kSweepTurnMs;
  static const unsigned kAlpha[3] = {255, 110, 45};
  for (int k = 2; k >= 0; k--) {
    const float a = (turn * 360.0f - (float)k * 6.0f) * 3.14159265f / 180.0f;
    fb.aaLine((float)cx, (float)cy, (float)cx + sinf(a) * (float)R, (float)cy - cosf(a) * (float)R, 1.2f,
              theme::kSweep, kAlpha[k]);
  }
}

void HomePage::drawTopology(FrameBuffer565& fb, uint32_t now_ms) {
  const int ax0 = kAreaX, ay0 = kAreaY, ax1 = kAreaX + kAreaW, ay1 = kAreaY + kAreaH;
  fb.setClip(kAreaX, kAreaY, kAreaW, kAreaH);
  Boxes taken;
  auto fits = [&](int x, int y, int w, int h) {
    return x >= ax0 + 2 && y >= ay0 + 2 && x + w <= ax1 - 2 && y + h <= ay1 - 2 &&
           !taken.hits(x - 1, y - 1, x + w + 1, y + h + 1);
  };
  const AAFont* const fonts[1] = {&kFontSmall};

  // What is fixed first: the repeater ring and its label, the notes, every ring.
  int cx = 0, cy = 0;
  const bool centre = slotPos(0, cx, cy);
  const int repLabelX = cx + kReticleR + 4, repLabelY = cy - 1;
  if (centre) {
    taken.add(cx - kReticleR - 1, cy - kReticleR - 1, cx + kReticleR + 2, cy + kReticleR + 2);
    taken.add(repLabelX, repLabelY, repLabelX + FrameBuffer565::textWidth(kRepeaterLabel, kFontTitle),
              repLabelY + kFontTitle.ascent + kFontTitle.descent);
  }
  const int noteH = kFontSmall.ascent + kFontSmall.descent;
  const int noteY = ay1 - noteH - 3;
  char note[48];
  note[0] = 0;
  const char* topNote = nullptr;
  if (_mode == kGeo) {
    if (!_geoCentreKnown) topNote = "No repeater GPS fix";
    if (_noGps > 0) snprintf(note, sizeof(note), "%d without GPS", _noGps);
  }
  if (topNote != nullptr)
    taken.add(ax0 + 6, ay0 + 3, ax0 + 6 + FrameBuffer565::textWidth(topNote, kFontSmall), ay0 + 3 + noteH);
  if (note[0]) taken.add(ax0 + 6, noteY, ax0 + 6 + FrameBuffer565::textWidth(note, kFontSmall), noteY + noteH);
  if (_mode == kGeo && centre) {   // compass letters (drawGeoOverlay)
    const int R = (kAreaW < kAreaH ? kAreaW : kAreaH) / 2 - 6 - 14;
    const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
    for (int i = 0; i < 4; i++) taken.add(cx + dx[i] * R - 6, cy + dy[i] * R - 8, cx + dx[i] * R + 6, cy + dy[i] * R + 8);
  }
  int nx[GraphLayout::kMaxNodes], ny[GraphLayout::kMaxNodes];
  bool shown[GraphLayout::kMaxNodes];
  for (int s = 1; s < GraphLayout::kMaxNodes; s++) {
    shown[s] = _slotNode[s] != nullptr && slotPos(s, nx[s], ny[s]);
    if (shown[s]) taken.add(nx[s] - kNodeHalf - 1, ny[s] - kNodeHalf - 1, nx[s] + kNodeHalf + 2, ny[s] + kNodeHalf + 2);
  }
  // A label must not sit near ANOTHER contact's ring (it would read as that
  // contact's name): candidates within kForeignGap px of one are rejected.
  auto nearOther = [&](int self, int x, int y, int w, int h) {
    const int r = kNodeHalf + kForeignGap;
    for (int o = 1; o < GraphLayout::kMaxNodes; o++) {
      if (o == self || !shown[o]) continue;
      if (x < nx[o] + r + 1 && x + w > nx[o] - r && y < ny[o] + r + 1 && y + h > ny[o] - r) return true;
    }
    return false;
  };

  // Contact labels (14 px): the whole name, on the right, left, above or
  // below, at least kLabelGap px clear of its own ring. Contacts with their
  // own signal first.
  char labels[GraphLayout::kMaxNodes][40];
  int lx[GraphLayout::kMaxNodes], ly[GraphLayout::kMaxNodes];
  uint8_t lf[GraphLayout::kMaxNodes];
  bool lab[GraphLayout::kMaxNodes];
  // Whole names only: a name that does not fit anywhere is left unlabelled
  // rather than cut mid-word.
  static const size_t kLens[1] = {kLabelMaxChars};
  for (int pass = 0; pass < 2; pass++) {
    for (int s = 1; s < GraphLayout::kMaxNodes; s++) {
      if (pass == 0) lab[s] = false;
      if (!shown[s]) continue;
      const NodeInfo* n = _slotNode[s];
      if ((pass == 0) != hasSignal(*n)) continue;
      char raw[24];
      if (n->name[0]) head_policy::displayName(n->name, n->pub_prefix, n->type, raw, sizeof(raw));
      else snprintf(raw, sizeof(raw), "%02X%02X", n->pub_prefix[0], n->pub_prefix[1]);
      char dist[12] = "";
      if (_mode == kGeo && n->has_pos && _geoCentreKnown) {
        const double km = _geo.distanceKm(n->lat_e6, n->lon_e6);
        if (km < 10.0) snprintf(dist, sizeof(dist), " %.1fkm", km);
        else snprintf(dist, sizeof(dist), " %dkm", (int)(km + 0.5));
      }
      for (int font = 0; font < 1 && !lab[s]; font++) {
        const AAFont& f = *fonts[font];
        const int h = f.ascent + f.descent;
        for (int li = 0; li < 1 && !lab[s]; li++) {
          char name[24];
          labelName(name, sizeof(name), raw, kLens[li]);
          // GEO: with the distance first, then the name alone.
          for (int withDist = dist[0] ? 1 : 0; withDist >= 0 && !lab[s]; withDist--) {
            char text[40];
            snprintf(text, sizeof(text), "%s%s", name, withDist ? dist : "");
            const int w = FrameBuffer565::textWidth(text, f);
            const int x = nx[s], y = ny[s];
            const int g = kNodeHalf + 1 + kLabelGap;
            const int cand[4][2] = {{x + g, y - h / 2}, {x - g - w, y - h / 2}, {x - w / 2, y - g - h}, {x - w / 2, y + g}};
            for (int c = 0; c < 4 && !lab[s]; c++) {
              if (!fits(cand[c][0], cand[c][1], w, h) || nearOther(s, cand[c][0], cand[c][1], w, h)) continue;
              lab[s] = true;
              lx[s] = cand[c][0];
              ly[s] = cand[c][1];
              lf[s] = (uint8_t)font;
              memcpy(labels[s], text, sizeof(text));
              taken.add(lx[s], ly[s], lx[s] + w, ly[s] + h);
              _labW[s] = (int16_t)w;
              _labH[s] = (int16_t)h;
            }
          }
        }
      }
    }
  }

  for (int s = 0; s < GraphLayout::kMaxNodes; s++) {
    _labOn[s] = s > 0 && shown[s] && lab[s];
    if (_labOn[s]) { _labX[s] = (int16_t)lx[s]; _labY[s] = (int16_t)ly[s]; }
  }

  // GEO range rings (behind everything); their km labels go where no
  // contact label is: east, west, north or south of the ring.
  if (_mode == kGeo && centre) {
    const double maxPx = (double)((kAreaW < kAreaH ? kAreaW : kAreaH) / 2 - 6);
    const double km = GeoLayout::niceKm(maxPx / _geo.pxPerKm());
    const int sh = kFontSmall.ascent + kFontSmall.descent;
    for (int k = 1; k <= 2 && km > 0; k++) {
      const double ringKm = km * k / 2.0;
      const int r = (int)(ringKm * _geo.pxPerKm() + 0.5);
      if (r < 18) continue;
      fb.aaRing((float)cx, (float)cy, (float)r, 1.0f, theme::kGeoRing);
      char rl[16];
      if (ringKm >= 1.0) snprintf(rl, sizeof(rl), "%gkm", ringKm);
      else snprintf(rl, sizeof(rl), "%dm", (int)(ringKm * 1000.0 + 0.5));
      const int w = FrameBuffer565::textWidth(rl, kFontSmall);
      const int cand[4][2] = {{cx + r + 3, cy - sh - 1}, {cx - r - 3 - w, cy - sh - 1}, {cx + 4, cy - r - sh - 1}, {cx + 4, cy + r + 2}};
      for (int c = 0; c < 4; c++) {
        if (!fits(cand[c][0], cand[c][1], w, sh)) continue;
        fb.text(cand[c][0], cand[c][1], rl, theme::kTextDim, kFontSmall);
        taken.add(cand[c][0], cand[c][1], cand[c][0] + w, cand[c][1] + sh);
        break;
      }
    }
    fb.aaRing((float)cx, (float)cy, (float)((kAreaW < kAreaH ? kAreaW : kAreaH) / 2 - 6), 1.0f, theme::kGeoRing);
    drawGeoOverlay(fb, cx, cy, now_ms);
  }
  // Links.
  for (int l = 0; l < _graph.linkCount(); l++) {
    int x0, y0, x1, y1;
    if (slotPos(_graph.linkA(l), x0, y0) && slotPos(_graph.linkB(l), x1, y1))
      fb.aaLine((float)x0, (float)y0, (float)x1, (float)y1, 1.0f, theme::kLink);
  }
  drawPath(fb, now_ms, false);
  // Pings: an expanding ring that fades (clipped to the area).
  for (int i = 0; i < kMaxPings; i++) {
    const Ping& p = _pings[i];
    const uint32_t t = now_ms - p.start;
    int x, y;
    if (p.slot < 0 || t >= kPingMs || !slotPos(p.slot, x, y)) continue;
    // Starts just outside its ring (the M5's ring is bigger).
    const float r = (p.slot == 0 ? (float)kReticleR + 3.0f : 7.0f) + 14.0f * (float)t / (float)kPingMs;
    const unsigned alpha = 255u - 230u * t / kPingMs;
    fb.aaRing((float)x, (float)y, r, 1.6f, p.colour, alpha);
  }
  // Contacts: a ring in the link colour, and their labels.
  for (int s = 1; s < GraphLayout::kMaxNodes; s++) {
    if (!shown[s]) continue;
    const uint16_t c = slotColour(s, now_ms);
    fb.aaDisc((float)nx[s], (float)ny[s], 4.5f, theme::kPanelFill);
    fb.aaRing((float)nx[s], (float)ny[s], 4.5f, 2.0f, c);
    if (lab[s]) fb.text(lx[s], ly[s], labels[s], c, *fonts[lf[s]]);
  }
  // The repeater: white ring with a centre dot.
  if (centre) {
    fb.aaDisc((float)cx, (float)cy, (float)kReticleR + 0.5f, theme::kPanelFill);
    fb.aaRing((float)cx, (float)cy, (float)kReticleR + 0.5f, 2.0f, theme::kRepeater);
    fb.aaDisc((float)cx, (float)cy, 2.2f, theme::kRepeater);
    fb.text(repLabelX, repLabelY, kRepeaterLabel, theme::kRepeater, kFontTitle);
  }
  drawPath(fb, now_ms, true);
  // Notes.
  if (_mode == kGraph && _graph.count() == 0) {
    fb.text(ax0 + (kAreaW - kGraphRightMargin) / 2, ay0 + kAreaH / 2 + 22, "Listening...", theme::kTextDim, kFontBody,
            FrameBuffer565::kCentreTop);
  }
  if (topNote != nullptr) fb.text(ax0 + 6, ay0 + 3, topNote, kDimText, kFontSmall);
  if (note[0]) fb.text(ax0 + 6, noteY, note, kDimText, kFontSmall);
  fb.resetClip();
}

// The route (blip = false: before the contacts) or its travelling blip
// (blip = true: over everything, so a contact never hides it).
void HomePage::drawPath(FrameBuffer565& fb, uint32_t now_ms, bool blip) const {
  int px[kMaxPath], py[kMaxPath];
  int n = 0;
  for (int i = 0; i < _pathLen; i++) {
    if (slotPos(_path[i], px[n], py[n])) n++;
  }
  if (n < 2) return;
  const uint32_t t = now_ms - _pathStart;
  const bool flying = _pathAnim && t < kPathAnimMs;
  if (!blip) {
    // At rest a dim trace; in flight cyan, 2 px.
    for (int i = 0; i + 1 < n; i++)
      fb.aaLine((float)px[i], (float)py[i], (float)px[i + 1], (float)py[i + 1], flying ? 2.0f : 1.5f,
                flying ? theme::kRoute : theme::kRouteRest);
    return;
  }
  if (!flying) return;
  // The blip travels from the sender to the repeater.
  float len[kMaxPath];
  float total = 0;
  for (int i = 0; i + 1 < n; i++) {
    const float dx = (float)(px[i + 1] - px[i]), dy = (float)(py[i + 1] - py[i]);
    len[i] = sqrtf(dx * dx + dy * dy);
    total += len[i];
  }
  float d = total * (float)t / (float)kPathAnimMs;
  for (int i = 0; i + 1 < n; i++) {
    if (d <= len[i] || i + 2 == n) {
      const float f = len[i] > 0.01f ? fminf(d / len[i], 1.0f) : 1.0f;
      const float bx = (float)px[i] + (float)(px[i + 1] - px[i]) * f;
      const float by = (float)py[i] + (float)(py[i + 1] - py[i]) * f;
      fb.aaRing(bx, by, 6.0f, 1.5f, theme::kRoute, 150);
      fb.aaDisc(bx, by, 3.0f, theme::kBlip);
      break;
    }
    d -= len[i];
  }
}

// While the packet is in flight: its route along the bottom of the area,
// on a dark strip (labels under it reappear when it ends).
void HomePage::drawCaption(FrameBuffer565& fb, uint32_t now_ms) const {
  if (_caption[0] == 0 || !_pathAnim || (uint32_t)(now_ms - _pathStart) >= kPathAnimMs) return;
  const int y = kAreaY + kAreaH - kCaptionH;
  fb.setClip(kAreaX, y, kAreaW, kCaptionH);
  fb.fillRectAlpha(kAreaX, y, kAreaW, kCaptionH, theme::kPanelFill, 230);
  const int x = fb.text(kAreaX + 6, y + 1, _captionType, feedTypeColour(_captionTypeId), kFontSmall);
  fb.text(x + 6, y + 1, _caption, _captionColour, kFontSmall);
  fb.resetClip();
}

void HomePage::drawNoise(FrameBuffer565& fb, uint32_t now_ms) {
  const MeshModel* m = _ctx->model;
  const mesh::observer::Vitals* v = m->vitals();
  const bool live = v != nullptr && (uint32_t)(now_ms - m->vitalsMs()) < kRadioStaleMs;
  const int x = kNoisePanel.x + 10;
  if (v != nullptr) _needle.setTarget((float)v->noise_floor, now_ms);   // only vitals move it
  const float pos = _needle.update(now_ms);
  if (v != nullptr) {
    char num[12];
    snprintf(num, sizeof(num), "%d", (int)v->noise_floor);
    fb.text(x, 72, num, live ? theme::kText : kDimText, kFontBig);
    const hmi::NoiseRating r = hmi::noiseRating(v->noise_floor);
    const uint16_t rc = !live ? kDimText : (r == hmi::kNoiseQuiet ? theme::kGood : (r == hmi::kNoiseModerate ? theme::kWarn : theme::kBad));
    fb.text(x, 112, live ? hmi::noiseRatingName(r) : "stale", rc, kFontBody);
  } else {
    fb.text(x, 72, "---", kDimText, kFontBig);
    fb.text(x, 112, "no data", kDimText, kFontBody);
  }
  const float a = (_needle.has() ? hmi::gaugeAngle(pos) : (float)hmi::kGaugeStartDeg) * 3.14159265f / 180.0f;
  const float r = (float)(kGaugeR - 6);
  const uint16_t nc = live ? theme::kNeedle : kDimText;
  fb.aaLine((float)kGaugeX, (float)kGaugeY, (float)kGaugeX + cosf(a) * r, (float)kGaugeY + sinf(a) * r, 2.0f, nc);
  fb.aaDisc((float)kGaugeX, (float)kGaugeY, 3.0f, nc);
}

void HomePage::drawPackets(FrameBuffer565& fb, uint32_t now_ms) {
  const MeshModel* m = _ctx->model;
  _odo.set(m->feedSeq(), now_ms);
  const double shown = _odo.shown(now_ms);
  for (int i = 0; i < kOdoDigits; i++) {
    const int bx = kOdoX + i * kOdoPitch;
    int d = 0;
    float roll = 0;
    hmi::Odometer::digit(shown, kOdoDigits - 1 - i, d, roll);
    fb.setClip(bx + 1, kOdoY + 1, kOdoW - 2, kOdoH - 2);
    const int cx = bx + kOdoW / 2, cy = kOdoY + kOdoH / 2;
    const int off = (int)lroundf(roll * (float)kOdoH);
    char a[2] = {(char)('0' + d), 0}, b[2] = {(char)('0' + (d + 1) % 10), 0};
    fb.text(cx, cy - off, a, theme::kText, kFontDigits, FrameBuffer565::kCentreMiddle);
    if (off > 0) fb.text(cx, cy - off + kOdoH, b, theme::kText, kFontDigits, FrameBuffer565::kCentreMiddle);
    fb.resetClip();
  }
  // Rate (the repeater's RX/min, or the head's observed count when the
  // vitals contradict it: Widgets.h pickRx) and its trend.
  const mesh::observer::Vitals* v = m->vitals();
  const widgets::RxPick rx = widgets::rxFor(*m);
  const bool live = rx.from_head || (v != nullptr && (uint32_t)(now_ms - m->vitalsMs()) < kRadioStaleMs);
  char rate[12];
  if (rx.known) snprintf(rate, sizeof(rate), "%u", rx.rx);
  else snprintf(rate, sizeof(rate), "--");
  const int x = kPacketsPanel.x + 10;
  const int end = fb.text(x, 207, rate, live ? theme::kAccent : kDimText, kFontDigits);
  fb.text(end + 4, 212, rx.from_head ? "/min head" : "/min", theme::kTextDim, kFontBody);
  uint16_t rates[60];
  m->ratesPerMinute(rates);
  int pct = 0;
  if (hmi::rateTrend(rates, pct)) {
    char trend[16];
    uint16_t tc = theme::kTextDim;
    if (pct > 0) { snprintf(trend, sizeof(trend), "^ %d%%", pct); tc = theme::kGood; }
    else if (pct < 0) { snprintf(trend, sizeof(trend), "v %d%%", -pct); tc = theme::kWarn; }
    else snprintf(trend, sizeof(trend), "= 0%%");
    fb.text(kPacketsPanel.x + kPacketsPanel.w - 8, 212, trend, tc, kFontBody, FrameBuffer565::kRightTop);
  }
}

// A tile's sub-label only when it fits whole (never cut mid-word).
static bool fitsTile(const char* s, int w) { return FrameBuffer565::textWidth(s, kFontSmall) <= w; }

void HomePage::presetLabel(char* out, size_t size, const char* name, unsigned slot, int room) {
  if (slot == 0) { snprintf(out, size, "custom"); return; }
  if (name == nullptr || name[0] == 0) { snprintf(out, size, "slot %u", slot); return; }
  snprintf(out, size, "%s", name);
  if (fitsTile(out, room)) return;
  // Too wide ("USA-Canada"): cut and mark with "~", as the map labels do.
  size_t n = strlen(out);
  while (n > 1) {
    n--;
    out[n] = '~';
    out[n + 1] = 0;
    if (fitsTile(out, room)) return;
  }
}

void HomePage::drawQuick(FrameBuffer565& fb, uint32_t now_ms) const {
  static const char* const kTitles[hmi::kTileCount] = {"ADVERT", "PRESET", "OBSERVER", "BRIDGE", "TRACE", "ADMIN"};
  const MeshModel* m = _ctx->model;
  const mesh::observer::Vitals* v = m->vitals();
  const int room = kTileW - 6;
  for (int i = 0; i < hmi::kTileCount; i++) {
    const int x = kTileX + i * kTilePitch;
    const char* title = kTitles[i];
    char sub[24];
    uint16_t c = theme::kAccent;
    bool on = false;
    switch (i) {
      case hmi::kTileAdvert: {
        // The head's own advert (not the repeater's).
        const uint32_t left = _quick.cooldownLeftMs(i, now_ms);
        if (left > 0) {
          snprintf(sub, sizeof(sub), "wait %lus", (unsigned long)((left + 999u) / 1000u));
          c = theme::kTextDim;
        } else {
          snprintf(sub, sizeof(sub), "head");
        }
        break;
      }
      case hmi::kTilePreset: {
        const unsigned slot = v != nullptr ? v->preset_slot : 0;
        presetLabel(sub, sizeof(sub), slot >= 1 && slot <= 5 ? _ctx->preset_names[slot - 1] : "", slot, room);
        on = slot > 0;
        if (!on) c = theme::kTextDim;
        break;
      }
      case hmi::kTileObserver:
        on = _ctx->obs_seen && (uint32_t)(now_ms - _ctx->obs_last_ms) < kObserverFreshMs;
        snprintf(sub, sizeof(sub), on ? "ON" : "OFF");
        c = on ? theme::kGood : theme::kTextDim;
        break;
      case hmi::kTileBridge:
        on = _ctx->bridge_seen && (uint32_t)(now_ms - _ctx->bridge_last_ms) < kBridgeFreshMs;
        snprintf(sub, sizeof(sub), on ? "ON" : "IDLE");
        c = on ? theme::kGood : theme::kTextDim;
        break;
      case hmi::kTileTrace: snprintf(sub, sizeof(sub), "console"); break;
      default:
        snprintf(sub, sizeof(sub), "%s", adminLabel(_ctx->admin_logged_in));
        on = _ctx->admin_logged_in;
        c = on ? theme::kGood : theme::kWarn;
        break;
    }
    uint16_t edge = on ? c : theme::kTileOffEdge;
    uint16_t titleColour = on ? theme::kText : theme::kTextDim;
    if (_quick.confirming(i, now_ms)) {
      title = "CONFIRM?";
      snprintf(sub, sizeof(sub), "tap again");
      on = true;
      c = theme::kWarn;
      edge = theme::kWarn;
      titleColour = theme::kText;
    }
    const int res = _quick.result(i, now_ms);
    if (res >= 0) {
      snprintf(sub, sizeof(sub), res ? "SENT" : "FAILED");
      c = res ? theme::kGood : theme::kBad;
    }
    fb.fillRoundRect(x, kTileY, kTileW, kTileH, 4, on ? theme::kTileOnFill : theme::kTileOffFill);
    fb.strokeRoundRect(x, kTileY, kTileW, kTileH, 4, edge);
    fb.text(x + kTileW / 2, kTileY + 14, title, titleColour, kFontTile, FrameBuffer565::kCentreMiddle);
    fb.text(x + kTileW / 2, kTileY + 33, sub, c, kFontSmall, FrameBuffer565::kCentreMiddle);
  }
}

}  // namespace freenove_ui
