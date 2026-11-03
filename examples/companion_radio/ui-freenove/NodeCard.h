#pragma once

// Node detail card of the Freenove visual head (pure C++11; host-tested and
// rendered to a preview by test/test_widgets.py). One card is shared by the
// pages that list nodes: a tap on a contact on HOME, MAP or NODES opens it
// over the page's content area; CLOSE (or another page) closes it.
//
// Layout (kRect = the content area between the header and the tab bar):
//   header    the node's name (16 px bold), CLOSE button on the right;
//             under it: type, key prefix, last heard, hops, packets
//   left      SIGNAL: the node's SNR history (MeshModel snr_hist, oldest
//             left) as a filled sparkline with its range, then the last
//             SNR / RSSI; LAST PATH: the route of the newest packet that
//             went through the node (names where known, hashes otherwise),
//             ending at the M5
//   right     POSITION: coordinates, distance and bearing from the
//             repeater (or why there are none: no GPS in the node's
//             adverts, or no repeater fix); LAST PACKETS: the five newest
//             packets from or via the node (age, type, hops, SNR)
// Real data only: every line comes from the model; what the model does not
// know is said, never invented.

#include <stdint.h>

#include <Packet.h>

#include "FrameBuffer565.h"
#include "MeshModel.h"
#include "Page.h"

namespace freenove_ui {

class NodeCard {
 public:
  static const PanelRect kRect;
  // CLOSE button (screen px).
#if HMI_CYD
  // CYD: the content area (4,34 312x174), two columns: SIGNAL / LAST PATH
  // left, POSITION / LAST PACKETS (4) right.
  static const int kCloseX = 250, kCloseY = 36, kCloseW = 62, kCloseH = 18;
  static const int kLastPackets = 4;
#else
  static const int kCloseX = 392, kCloseY = 45, kCloseW = 76, kCloseH = 21;
  static const int kLastPackets = 5;
#endif
  static const uint32_t kRefreshMs = 1000;   // ages tick
  static const uint32_t kMinDataGapMs = 250; // new data: at most 4 frames/s

  NodeCard();
  void attach(const PageContext* ctx) { _ctx = ctx; }

  void open(const uint8_t key[4]);
  void close() { _open = false; }
  bool isOpen() const { return _open; }
  const uint8_t* key() const { return _key; }

  // While open: true when the card wants a frame (just opened, new data,
  // the ages' once-a-second tick).
  bool wantsFrame(uint32_t now_ms) const;
  // Draws the whole card (opaque over kRect).
  void draw(FrameBuffer565& fb, uint32_t now_ms);
  // A touch while open (taps arrive as down then up): true = the card
  // changed (closed).
  bool touch(int x, int y, bool down);

  // The model node the card shows; null when it has left the table.
  const mesh::head::NodeInfo* node() const;

 private:
  void drawSignal(FrameBuffer565& fb, const mesh::head::NodeInfo& n) const;
  void drawPath(FrameBuffer565& fb, const mesh::head::NodeInfo& n) const;
  void drawPosition(FrameBuffer565& fb, const mesh::head::NodeInfo& n) const;
  void drawPackets(FrameBuffer565& fb, const mesh::head::NodeInfo& n, uint32_t now_ms) const;

  const PageContext* _ctx;
  bool _open;
  uint8_t _key[4];
  bool _drawn;
  uint32_t _drawnMs, _drawnGen;
  bool _pressClose;
};

// The feed item's route starts with its sender: an advert whose whole
// path (sender + forwarders) fits FeedItem::route.
inline bool routeHasSender(const mesh::head::FeedItem& f) {
  return f.payload_type == PAYLOAD_TYPE_ADVERT && f.route_len > 0 && f.route_len > f.hops;
}
// A feed item sent by the node (an advert it originated), relayed to the
// repeater by it (its last hop; a repeater), or sent by it straight to the
// repeater (0 hops).
// Hashes count only when they resolve uniquely (MeshModel::findByHash for
// forwarders; uniqueNode for a 0-hop last hop); an advert names its sender
// by key.
bool feedFromNode(const mesh::head::MeshModel& m, const mesh::head::FeedItem& f, const mesh::head::NodeInfo& n);
// The feed item's route went through the node: its own advert, or (only
// repeaters forward) a hop of the route.
bool routeThroughNode(const mesh::head::MeshModel& m, const mesh::head::FeedItem& f, const mesh::head::NodeInfo& n);
// The one node (any type) whose key starts with h; null when none or shared.
const mesh::head::NodeInfo* uniqueNode(const mesh::head::MeshModel& m, const mesh::head::PathHash& h);
// The node of the model with this key prefix; null when none.
const mesh::head::NodeInfo* findNode(const mesh::head::MeshModel& m, const uint8_t key[4]);

}  // namespace freenove_ui
