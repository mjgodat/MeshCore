#pragma once

// The visual head's page set and shared chrome (pure C++, host-tested by
// test/test_observer_demux.py, test_freenove_tabbar.py and test_layouts.py).
//
// Page order (owner-approved mockup layout-v2.html): HOME, TELEMETRY, MAP,
// NODES, HEAD. HOME is the main operating page (HomePage.h), then
// TELEMETRY (TelemetryPage.h), MAP (MapPage.h) and NODES (NodesPage.h);
// HOME, MAP and NODES share one node card (NodeCard.h). HEAD is the
// companion UI itself (external page; Task 9 hosts the Head screens in the
// framebuffer).
//
// Chrome (HMI theme, mockup mock_glass3.png):
//   backdrop  diagonal gradient (dithered) + soft glow around each panel
//   header    rows 0..kHeaderH-1, drawn every frame: "M5 // <REPEATER>"
//             with the "OBSERVER - MESH - ESP-NOW" subline, the repeater's
//             frequency (vitals; stale grey / NO REPEATER DATA), MHz and
//             SF/BW, the observer-link icon, the clock (or uptime)
//   panels    rounded navy boxes with a cyan edge, caps title, divider
//   tab bar   five rounded tiles along the bottom kTabBarH rows

#include <stdint.h>

#include "HmiProfile.h"
#include "HomePage.h"
#include "MapPage.h"
#include "NodeCard.h"
#include "NodesPage.h"
#include "Page.h"
#include "RadioLabel.h"
#include "TelemetryPage.h"
#include "Theme.h"

namespace freenove_ui {

static const int kHeaderH = profile::kHeaderH;
// Panel header row (title, note, divider on its last row).
static const int kPanelHeaderH = profile::kPanelHeaderH;
// Widest repeater name in the header (cut with '~' beyond).
#if HMI_CYD
static const int kHeaderNameMaxPx = 210;
#else
static const int kHeaderNameMaxPx = 200;
#endif

// Tab label of page i ("HOME", "TELEMETRY", ...); "" out of range.
const char* tabLabel(int i);
// Backdrop of the rectangle (x, y, w, h): the gradient plus the glow of
// `panels` (n of them). Whole-screen form for page backgrounds.
void drawBackdrop(FrameBuffer565& fb, int x, int y, int w, int h, const PanelRect* panels, int n);
void drawBackdrop(FrameBuffer565& fb, const PanelRect* panels, int n);
// A panel: fill, edge, caps title (white) and the divider; `note` (cyan,
// right) may be null.
void drawPanel(FrameBuffer565& fb, const PanelRect& p, const char* title, const char* note);
// A page's whole background layer: the backdrop with the page's panel glow,
// Page::drawBackground(), then the tab bar.
void drawPageChrome(FrameBuffer565& fb, Page& page, int index, int count);
// The tab bar along the bottom kTabBarH rows (PageNav geometry), over its
// own strip of backdrop: the current tab teal with a cyan edge and cyan
// text, the others navy with dim text. The HEAD page draws it under the
// companion UI's area.
void drawTabBar(FrameBuffer565& fb, int index, int count);
// The header (dynamic: drawn every frame by the framework).
void drawHeader(FrameBuffer565& fb, const PageContext& ctx, uint32_t now_ms);
// A whole frame without a cached background layer, in the order the
// layered Freenove path composes it: backdrop (CYD: the gradient without
// glow), the page's static layer, the tab bar, the page's dynamic layer,
// the header. The CYD's strip renderer replays it once per band (fb in band
// mode, ctx.band = the band's index); hosts use it as the full-frame
// reference.
void drawFrame(FrameBuffer565& fb, Page& page, const PageContext& ctx, int index, int count, uint32_t now_ms);
// "M5 // CAPTCALLAHAN-BASE" (upper case) in the font it fits max_px with
// (18 px, else 16 px); else the name alone, else its leading whole words.
const AAFont& headerName(char* out, size_t cap, const char* repeaterName, int max_px);
// The repeater's name for the header: learned from its advert (model),
// else ctx.repeater_name; "" when neither is known.
const char* repeaterDisplayName(const PageContext& ctx);
// Text colour for a radio state.
uint16_t radioColour(RadioState state, uint16_t live);
// Dimmed text (stale data).
static const uint16_t kDimText = theme::kStale;
// Short feed name of a MeshCore payload type (ADV, GRP, TXT, ...); "?" if unknown.
const char* payloadTypeName(uint8_t type);

class NamedStubPage : public Page {
 public:
  NamedStubPage(const char* title, const char* blurb) : _title(title), _blurb(blurb) {}
  const char* title() const override { return _title; }
  int panels(const PanelRect*& out) const override;
  void drawBackground(FrameBuffer565& fb) override;
  void draw(FrameBuffer565& fb, uint32_t now_ms) override { (void)fb; (void)now_ms; }

 private:
  const char* _title;
  const char* _blurb;
};

// HEAD placeholder: the companion UI (CYD head screens through FreenoveDisplay)
// owns the panel while this page is current.
class HeadPlaceholderPage : public Page {
 public:
  const char* title() const override { return "HEAD"; }
  void draw(FrameBuffer565& fb, uint32_t now_ms) override { (void)fb; (void)now_ms; }
  bool external() const override { return true; }
};

class PageSet {
 public:
  enum { kHome, kTelemetry, kMap, kNodes, kHead, kCount };
  PageSet();
  void attach(const PageContext* ctx);
  // Large page tables (NODES rows) come from this allocator (PSRAM on the
  // device); call before the pages are first shown.
  void setAllocator(const mesh::head::ModelAllocator* a) { _nodes.setAllocator(a); }
  Page* page(int i) { return i >= 0 && i < kCount ? _pages[i] : nullptr; }

 private:
  NodeCard _card;
  HomePage _home;
  TelemetryPage _telemetry;
  MapPage _map;
  NodesPage _nodes;
  HeadPlaceholderPage _head;
  Page* _pages[kCount];
};

}  // namespace freenove_ui
