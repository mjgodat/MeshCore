#pragma once

// HOME page of the Freenove visual head: the main operating page (pure
// C++11; host-tested and rendered to previews by test/test_layouts.py).
//
// Layout (owner-approved HMI mockup mock_glass3.png), 480x320 landscape:
//   y 0..39     framework header (repeater, radio, link icon, clock).
//   MESH TOPOLOGY panel (6,42 288x196): the repeater as a white ring "M5"
//               at the centre, contacts as rings coloured by link quality
//               (green >= 5 dB, cyan >= 0, amber >= -7, violet below; grey
//               stale), Body-font labels (collision-checked), dim links.
//               GRAPH = force-directed (GraphLayout); GEO = GPS around the
//               repeater (GeoLayout) with km range rings, N/E/S/W ticks and a
//               slow sweep. The header note ("GRAPH" / "GEO") toggles the
//               two; while a packet is in flight its route caption shows
//               along the bottom of the area.
//   NOISE FLOOR panel (300,42 174x98): reading (dBm), rating (quiet /
//               moderate / noisy, HmiLogic.h) and a ring gauge whose needle
//               eases toward the vitals value.
//   PACKETS panel (300,144 174x94): six rolling odometer digits
//               (packets the head has seen since boot) and the repeater's
//               RX packets/min with the trend against the previous minutes.
//   QUICK row (y 244..289): ADVERT / PRESET / OBSERVER / BRIDGE / TRACE /
//               ADMIN tiles (HmiLogic.h QuickControls): ADVERT ("head":
//               the head's own advert) asks "CONFIRM?", sends on a second
//               tap and then rests 30 s; PRESET opens the repeater's preset
//               list, OBSERVER/BRIDGE its settings list, TRACE the admin
//               console, ADMIN the console with a saved-password login.
//   tab bar     y 296..319: framework (never drawn here).
//
// CYD (HMI_CYD, 320x240, header y 0..31, tab bar y 210..239): the same
// panels, smaller: MESH TOPOLOGY (4,34 190x132), NOISE FLOOR (198,34
// 118x64), PACKETS (198,102 118x64) and four quick tiles (ADVERT, PRESET,
// BRIDGE, ADMIN; y 170..207). No animations there: a packet's route is
// highlighted for kPathAnimMs (no blip), no ping rings, no GEO sweep, the
// layout settles at once, the needle and the odometer just show the value.
//
// Motion is event driven: a new packet with a path draws its route in cyan
// with a white blip travelling sender -> ... -> M5 over kPathAnimMs, then
// the route rests dim; the contact a packet came from gets one expanding
// ring ping. A new contact eases into place (GraphLayout cooling); the
// needle settles after new vitals; the odometer rolls when the count
// changes. GEO's sweep is the one ambient motion (one turn per
// kSweepTurnMs, redrawn at most every kSweepFrameMs) while GEO is shown with
// a repeater fix; otherwise the page asks for one frame a second.

#include <stdint.h>

#include "GeoLayout.h"
#include "GraphLayout.h"
#include "HmiLogic.h"
#include "NodeCard.h"
#include "Page.h"
#include "Theme.h"

namespace freenove_ui {

// Link-quality colour from quarter-dB SNR: >= 5 dB green, >= 0 cyan,
// >= -7 amber, else violet.
uint16_t linkColour(int8_t snr_q4);
// Caption colour of a payload type (Theme.h kFeed*: never a contact, route
// or blip colour).
uint16_t feedTypeColour(uint8_t type);
// Display form of a name (case kept, like the mockup): at most max_chars;
// a longer name is cut to max_chars - 1 and marked with a trailing '~'
// ("CardPut~").
void labelName(char* out, size_t cap, const char* name, size_t max_chars);

class HomePage : public Page {
 public:
  enum Mode { kGraph, kGeo };

  // Panels (screen coordinates; the header is rows 0..39). Every text is
  // >= 14 px (StubPages.h fonts).
  static const PanelRect kTopoPanel, kNoisePanel, kPacketsPanel;
#if HMI_CYD
  static const int kAreaX = 6, kAreaY = 56, kAreaW = 186, kAreaH = 108;
  static const int kGraphRightMargin = 40;
  static const int kToggleX = 124, kToggleY = 34, kToggleW = 70, kToggleH = 22;
  static const int kCaptionH = 18;
  static const int kGaugeX = 290, kGaugeY = 78, kGaugeR = 17, kGaugeThick = 4;
  static const int kOdoX = 206, kOdoPitch = 17, kOdoY = 126, kOdoW = 15, kOdoH = 22, kOdoDigits = 6;
  static const int kTileX = 4, kTilePitch = 79, kTileY = 170, kTileW = 75, kTileH = 38;
  // Tiles shown on the CYD, left to right (hmi::QuickTile indices):
  // OBSERVER and TRACE open the same screens as BRIDGE and ADMIN.
  static const int kTilesShown = 4;
#else
  // Topology area inside its panel (under the panel header).
  static const int kAreaX = 8, kAreaY = 68, kAreaW = 284, kAreaH = 168;
  // The force layout keeps nodes out of the right kGraphRightMargin px of
  // the area, so labels can sit to the right of their dots (mockup).
  static const int kGraphRightMargin = 56;
  // Header note ("GRAPH" / "GEO"): tap target of the toggle.
  static const int kToggleX = 200, kToggleY = 42, kToggleW = 94, kToggleH = 26;
  // Route caption: a strip along the bottom of the area while the packet
  // is in flight.
  static const int kCaptionH = 18;
  // Noise gauge.
  static const int kGaugeX = 440, kGaugeY = 106, kGaugeR = 26, kGaugeThick = 5;
  // Odometer cells.
  static const int kOdoX = 312, kOdoPitch = 25, kOdoY = 172, kOdoW = 22, kOdoH = 30, kOdoDigits = 6;
  // Quick-control tiles (a row of their own, no panel).
  static const int kTileX = 6, kTilePitch = 78, kTileY = 244, kTileW = 76, kTileH = 46;
  static const int kTilesShown = 6;
#endif
  // The hmi::QuickTile shown at position i (0..kTilesShown-1).
  static int tileId(int i);

  static const int kLabelMaxChars = 31; // contact names on the map: whole
  static const int kLabelGap = 4;       // label to its own ring (px)
  static const int kForeignGap = 4;     // label to another contact's ring
  static const int kNodeHalf = 4;       // contact ring: radius 4
  static const int kReticleR = 8;       // repeater ring
  static const int kMaxShown = GraphLayout::kMaxNodes - 1;   // nodes on screen
  static const uint32_t kPathAnimMs = 2000;
  static const uint32_t kPingMs = 1200;
  static const uint32_t kDimAfterMs = 30u * 60u * 1000u;     // not heard: grey
  static const uint32_t kKeepMs = 10u * 60u * 1000u;         // shown node keeps its slot
  static const uint32_t kLinkTtlMs = 60u * 60u * 1000u;      // link not heard: not drawn
  static const uint32_t kCentrePingGapMs = 2000;             // M5 rings, rate limit
  static const uint32_t kSweepTurnMs = 6000;                 // GEO sweep: one turn
  static const uint32_t kSweepFrameMs = 50;                  // ...redrawn at <= 20 fps
  static const uint32_t kObserverFreshMs = 60000;            // OBSERVER tile: ON
  static const uint32_t kBridgeFreshMs = 10u * 60u * 1000u;  // BRIDGE tile: ON

  HomePage();
  const char* title() const override { return "HOME"; }
  int panels(const PanelRect*& out) const override;
  void drawBackground(FrameBuffer565& fb) override;
  void draw(FrameBuffer565& fb, uint32_t now_ms) override;
  bool touch(int x, int y, bool down) override;
  bool wantsFrame(uint32_t now_ms) override;
  void onShow(uint32_t now_ms) override;
  void onHide() override;
  PageRequest takeRequest() override;
  void requestDone(PageRequest req, bool ok, uint32_t now_ms) override;

  // The shared node card: a tap on a contact opens it over the page.
  void setCard(NodeCard* card) { _card = card; }
  static const int kHitR = 14;          // tap distance to a contact (px)
  static const int kTapSlop = 12;       // a contact tap ends within this of its press
  // The contact slot under (x, y) in the topology area, -1 for none.
  int contactAt(int x, int y) const;

  Mode mode() const { return _mode; }
  void setMode(Mode m) { _mode = m; }
  // True while something moves (path, ping, layout settling, needle,
  // odometer).
  bool animating(uint32_t now_ms) const;
  // True while a ping ring is expanding.
  bool pinging(uint32_t now_ms) const;
  // True while the GEO sweep runs (GEO shown, repeater position known).
  bool sweeping() const;
  const GraphLayout& graph() const { return _graph; }
  const hmi::Needle& needle() const { return _needle; }
  const hmi::Odometer& odometer() const { return _odo; }
  const hmi::QuickControls& quick() const { return _quick; }
  // Tile index at (x, y), -1 for none.
  static int tileAt(int x, int y);
  // PRESET tile sub-label: the cached name of the active slot (cut with "~"
  // to fit room px), else "slot N", else "custom" (no slot).
  static void presetLabel(char* out, size_t size, const char* name, unsigned slot, int room);
  // ADMIN tile sub-label.
  static const char* adminLabel(bool logged_in) { return logged_in ? "console" : "login"; }
  // The last route as graph slots (sender first, 0 = the repeater, -1 = a
  // node that has left the screen since).
  int pathLength() const { return _pathLen; }
  int pathSlot(int i) const { return i >= 0 && i < _pathLen ? _path[i] : -1; }
  // Where the last frame put a contact and its label (screen px; tests).
  bool contactPos(int slot, int& x, int& y) const { return slotPos(slot, x, y); }
  bool labelRect(int slot, int& x, int& y, int& w, int& h) const {
    if (slot <= 0 || slot >= GraphLayout::kMaxNodes || !_labOn[slot]) return false;
    x = _labX[slot]; y = _labY[slot]; w = _labW[slot]; h = _labH[slot];
    return true;
  }

 private:
  struct Ping { int8_t slot; uint16_t colour; uint32_t start; };
  struct Edge { uint32_t a, b; uint32_t ms; };   // keys (0 = the repeater); ms = last heard
  static const int kMaxPings = 4;
  static const int kMaxEdges = 64;
  static const int kMaxPath = 16;
  static const uint32_t kMaxLearn = 128;   // feed items scanned for links per sync

  void sync(uint32_t now_ms);
  void syncNodes(uint32_t now_ms);
  void rebuildLinks(uint32_t now_ms);
  void addEdge(uint32_t a, uint32_t b, uint32_t ms);
  void dropSlot(int s);
  uint32_t keyForHash(const mesh::head::PathHash& h) const;
  void learnRoutes(uint32_t fresh);
  void takePath(bool animate, uint32_t now_ms);
  void takeFeed(uint32_t now_ms, const mesh::head::FeedItem* pathItem);
  void refreshGeo();
  void startPing(int slot, uint16_t colour, uint32_t now_ms);
  int slotForHash(const mesh::head::PathHash& h) const;
  uint16_t slotColour(int slot, uint32_t now_ms) const;
  bool slotPos(int slot, int& x, int& y) const;
  uint32_t quickSignature(uint32_t now_ms) const;

  void drawToggle(FrameBuffer565& fb) const;
  void drawTopology(FrameBuffer565& fb, uint32_t now_ms);
  void drawPath(FrameBuffer565& fb, uint32_t now_ms, bool blip) const;
  void drawGeoOverlay(FrameBuffer565& fb, int cx, int cy, uint32_t now_ms) const;
  void drawCaption(FrameBuffer565& fb, uint32_t now_ms) const;
  void drawNoise(FrameBuffer565& fb, uint32_t now_ms);
  void drawPackets(FrameBuffer565& fb, uint32_t now_ms);
  void drawQuick(FrameBuffer565& fb, uint32_t now_ms) const;

  NodeCard* _card;
  bool _pressing;
  int16_t _pressX, _pressY;
  Mode _mode;
  bool _drawn;
  uint32_t _drawnGen, _drawnMs;
  uint32_t _syncGen, _syncMs;
  bool _synced;
  uint32_t _feedSeq, _pathSeq;
  GraphLayout _graph;
  // GEO positions per graph slot (area coordinates).
  GeoLayout _geo;
  bool _geoHas[GraphLayout::kMaxNodes];
  int16_t _geoX[GraphLayout::kMaxNodes], _geoY[GraphLayout::kMaxNodes];
  bool _geoCentreKnown;
  int _noGps;
  Edge _edges[kMaxEdges];
  int _edgeCount;
  // Last path: graph slots, sender first, the repeater (slot 0) last.
  int8_t _path[kMaxPath];
  int _pathLen;
  uint32_t _pathStart;
  bool _pathAnim;
  char _captionType[8];
  uint8_t _captionTypeId;
  char _caption[96];
  uint16_t _captionColour;
  Ping _pings[kMaxPings];
  bool _pressOnToggle;
  int _pressTile;
  bool _quiet;              // next sync adopts new data without animating it
  bool _lastDrawAnimated;   // one more frame after motion stops
  bool _centrePinged;
  uint32_t _centrePingMs;
  hmi::Needle _needle;
  hmi::Odometer _odo;
  hmi::QuickControls _quick;
  PageRequest _request;
  uint32_t _quickDrawn;     // quickSignature() of the last frame
  // Model node per graph slot (valid between syncs; the model only changes
  // when data_gen does).
  const mesh::head::NodeInfo* _slotNode[GraphLayout::kMaxNodes];
  // Labels placed by the last drawTopology.
  bool _labOn[GraphLayout::kMaxNodes];
  int16_t _labX[GraphLayout::kMaxNodes], _labY[GraphLayout::kMaxNodes];
  int16_t _labW[GraphLayout::kMaxNodes], _labH[GraphLayout::kMaxNodes];
};

}  // namespace freenove_ui
