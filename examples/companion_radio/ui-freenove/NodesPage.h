#pragma once

// NODES page of the Freenove visual head (pure C++11; host-tested and
// rendered to a preview by test/test_widgets.py): every node of the model
// as a status row, like the P4 HMI's "SYSTEM STATUS" panel.
//
//   columns   NAME, TYPE, HEARD (age), SNR (own last-hop SNR for a node
//             heard directly; "-4.2 via Downtown" -- the SNR the M5 heard
//             its advert at and the relay, named from every repeater the
//             model heard, hex when unknown/ambiguous -- for a relayed one;
//             Widgets.h snrCell), HOPS, KM (from the repeater; needs both
//             fixes), PKTS
//   sort      tap a column header: by that column (its default direction);
//             tap it again: reversed. Stable (Widgets.h sortRows); rows
//             without the value (no SNR, no distance) stay last.
//   scroll    vertical drag; a bar on the right shows where you are
//   tap row   the node card (NodeCard.h)
// Rows are rebuilt from the model when it changes and once a second.

#include <stdint.h>

#include "NodeCard.h"
#include "Page.h"
#include "MeshModel.h"
#include "Widgets.h"

namespace freenove_ui {

class NodesPage : public Page {
 public:
  static const PanelRect kPanel;
#if HMI_CYD
  // CYD: NAME, AGE, SNR, HOP, PKT (TYPE and KM are left out: no room).
  static const int kHeadY = 56, kHeadH = 20;
  static const int kRowsY = 78, kRowH = 24, kRowsH = 128;   // finger-sized rows
#ifdef HMI_MODEL_NODES
  static const int kMaxRows = HMI_MODEL_NODES;
#else
  static const int kMaxRows = 64;
#endif
#else
  static const int kHeadY = 68, kHeadH = 22;     // column header row
  static const int kRowsY = 92, kRowH = 20, kRowsH = 198;
  static const int kMaxRows = 256;
#endif
  static const int kSlop = 10;                   // px before a press is a drag

  NodesPage();
  const char* title() const override { return "NODES"; }
  int panels(const PanelRect*& out) const override;
  void drawBackground(FrameBuffer565& fb) override;
  void draw(FrameBuffer565& fb, uint32_t now_ms) override;
  bool touch(int x, int y, bool down) override;
  bool wantsFrame(uint32_t now_ms) override;
  void onShow(uint32_t now_ms) override;
  void onHide() override;

  void setCard(NodeCard* card) { _card = card; }
  // Up / down chevron tiles at the ends of the scroll column (drawn = hit;
  // only where the list has more that way): a tap scrolls a page.
  static const int kHintW = 12, kHintH = 18;
  bool hintRect(bool down, int& x, int& y, int& w, int& h) const;   // false: not shown
  // Where the row table (kMaxRows rows, ~7 KB) is allocated, on first use:
  // PSRAM on the device; null = malloc.
  void setAllocator(const mesh::head::ModelAllocator* a) { _alloc = a; }
  ~NodesPage();
  // Column under x in the header row (-1 none).
  static int columnAt(int x);
  // Screen x span of column c's header label (with the sort mark when it
  // is the sort column): headers never overlap their neighbours.
  static void headerSpan(int c, bool sorted, int& x0, int& x1);
  // Left and right edge of column c (screen px).
  static int columnX0(int c);
  static int columnX1(int c);

  widgets::NodeColumn sortColumn() const { return _sort; }
  bool sortDescending() const { return _desc; }
  void setSort(widgets::NodeColumn c, bool descending);
  int rowCount() const { return _count; }
  int scroll() const { return _scroll; }
  int maxScroll() const;
  // Key of the row shown at position i (sorted order); false out of range.
  bool rowKey(int i, uint8_t key[4]) const;
  // Rebuild the rows from the model now (tests; draw does it when needed).
  void rebuild(uint32_t now_ms);

 private:
  void drawRow(FrameBuffer565& fb, int pos, int y) const;

  NodeCard* _card;
  bool ensureStorage();
  const mesh::head::ModelAllocator* _alloc;
  widgets::NodeRow* _rows;   // kMaxRows, allocated on first use
  uint16_t* _order;
  int _count;
  widgets::NodeColumn _sort;
  bool _desc;
  int _scroll;
  bool _drawn, _built;
  uint32_t _drawnMs, _drawnGen, _builtMs, _builtGen;
  bool _press, _drag;
  int _pressX, _pressY, _scroll0;
  uint32_t _hintMs;   // shown / scrolled: chevrons bright for a moment
};

}  // namespace freenove_ui
