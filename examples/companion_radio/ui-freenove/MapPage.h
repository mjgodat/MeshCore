#pragma once

// MAP page of the Freenove visual head (pure C++11; host-tested and
// rendered to a preview by test/test_widgets.py): the contacts that carry a
// GPS position plotted around the repeater (GeoLayout, north up), full
// screen.
//
//   repeater fix   the M5 at the centre, range rings with km labels,
//                  bearing spokes every 45 degrees with N/E/S/W, each
//                  contact labelled "name 4.1km"
//   no fix         (the repeater reports none): said so at the top; the
//                  contacts are placed around their own centre with a scale
//                  bar and a north mark, no distances or rings from the M5
//   always         contacts as rings in their link-quality colour (grey
//                  when stale or only heard via relays); "N without GPS"
//                  counts the rest; a packet whose route crosses plotted
//                  contacts draws a fading arc per hop (and to the M5 with
//                  a fix) with a travelling dot. Tap a contact: the node
//                  card. No base map yet (an offline tile layer is future
//                  work).

#include <stdint.h>

#include "GeoLayout.h"
#include "NodeCard.h"
#include "Page.h"

namespace freenove_ui {

class MapPage : public Page {
 public:
  static const PanelRect kPanel;
#if HMI_CYD
  // CYD: the whole content area (4,34 312x174); no travelling packet on the
  // arcs (they show while active, then go).
  static const int kAreaX = 6, kAreaY = 56, kAreaW = 308, kAreaH = 150;
#else
  static const int kAreaX = 8, kAreaY = 68, kAreaW = 464, kAreaH = 222;
#endif
  static const int kMaxShown = 64;
  static const int kMaxArcs = 8;
  static const uint32_t kArcMs = 2500;
  static const int kHitR = 18;          // tap distance to a contact (px)
  static const int kNodeR = HMI_CYD ? 4 : 5;

  MapPage();
  const char* title() const override { return "MAP"; }
  int panels(const PanelRect*& out) const override;
  void drawBackground(FrameBuffer565& fb) override;
  void draw(FrameBuffer565& fb, uint32_t now_ms) override;
  bool touch(int x, int y, bool down) override;
  bool wantsFrame(uint32_t now_ms) override;
  void onShow(uint32_t now_ms) override;
  void onHide() override;

  void setCard(NodeCard* card) { _card = card; }
  // What the last frame plotted (tests): contacts and where.
  int shownCount() const { return _shown; }
  bool shown(int i, int& x, int& y, uint8_t key[4]) const;
  bool repeaterFix() const { return _fix; }
  int withoutGps() const { return _noGps; }
  int arcsActive(uint32_t now_ms) const;

 private:
  struct Arc { int16_t x0, y0, x1, y1; uint32_t start; };

  void sync(uint32_t now_ms);
  void takeArcs(uint32_t now_ms);
  // sender: the advert's sender key (route[0]); null: h must resolve to one
  // repeater of the model (MeshModel::findByHash).
  bool hashPos(const mesh::head::PathHash& h, const uint8_t* sender, int& x, int& y) const;
  // takenBoxes: the label collision boxes (MapPage.cpp Boxes).
  void placeLabels(void* takenBoxes);
  void labelText(char* out, size_t cap, const mesh::head::NodeInfo& n, bool withDist) const;
  void drawGrid(FrameBuffer565& fb, int cx, int cy, void* takenBoxes) const;
  void drawArcs(FrameBuffer565& fb, uint32_t now_ms) const;
  int hit(int x, int y) const;

  NodeCard* _card;
  GeoLayout _geo;
  bool _drawn, _synced, _fix;
  uint32_t _drawnMs, _drawnGen, _syncGen, _feedSeq;
  int _shown, _noGps, _withGps;
  int16_t _x[kMaxShown], _y[kMaxShown];
  uint8_t _key[kMaxShown][4];
  int16_t _labX[kMaxShown], _labY[kMaxShown];
  uint8_t _labMode[kMaxShown];   // 0 none, 1 name, 2 name + distance
  Arc _arcs[kMaxArcs];
  int _arcNext;
  bool _press;
  int _pressX, _pressY;
};

}  // namespace freenove_ui
