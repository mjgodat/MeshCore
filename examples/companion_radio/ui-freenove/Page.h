#pragma once

// A page of the Freenove visual head (pure C++; pages draw only into a
// FrameBuffer565 and read the model, so they are host-testable).
//
// Rendering contract (FreenoveUITask, double-buffered RGB565 in PSRAM):
//   - When a page is shown, the framework builds the page background layer
//     once: the backdrop (gradient + the glow around panels()), the page's
//     drawBackground() (panel bodies, static labels, gauge scales) and the
//     tab bar (drawPageChrome). Both framebuffers start as copies of it.
//   - Each frame the framework restores what the last frame drawn in that
//     buffer covered (FrameBuffer565::eraseTouched(background)) and calls
//     draw(): a page draws only its dynamic layer, every frame, in full.
//     The pusher sends only the columns that really changed. After draw()
//     the framework draws the header (drawHeader: repeater, radio, link,
//     clock) over rows 0..kHeaderH-1.
//   - Pages keep their content between the header and the tab bar.
//   - The framework renders a frame only when something asks for one:
//     the page was just shown, wantsFrame() (new data or an animation in
//     flight), or touch() returned true. Otherwise the render task idles.
// Time: every now_ms is the head's millis() (MeshModel's clock contract).

#include <stdint.h>

#include "FrameBuffer565.h"
#include "HmiProfile.h"
#include "MeshModel.h"

namespace freenove_ui {

// What the framework shares with every page. Read-only for pages; updated
// by the render task between frames (same task: no locking).
struct PageContext {
  const mesh::head::MeshModel* model;
  uint32_t now_ms;        // head clock of the current loop iteration (touch handling)
  uint32_t data_gen;      // bumps whenever the model took input
  uint32_t obs_frames;    // observer frames received (ESP-NOW)
  uint32_t obs_dropped;   // dropped by the receive queue (drop-oldest)
  uint32_t obs_bad;       // passed the demux but failed to decode
  uint32_t obs_last_ms;   // head clock of the newest observer frame...
  bool obs_seen;          // ...valid once one has arrived
  uint32_t bridge_last_ms;   // head clock of the newest packet the head received itself
  bool bridge_seen;
  char clock_hm[12];      // header clock "13:42" or uptime "T+05:12" (hmi::formatClock)
  char clock_date[24];    // "31 Oct UTC" or "UPTIME"
  // The repeater's preset names (slot 1..5 at index 0..4) as the HEAD's
  // admin console last read them; "" when unknown.
  char preset_names[5][13];
  bool admin_logged_in;   // the HEAD admin console is logged in (ADMIN tile)
  // The repeater's name until the model learns it from its own advert: the
  // name last learned (kept in NVS), else the head's home-repeater contact.
  char repeater_name[24];
  // Strip rendering (CYD, StripRenderer.h): the index of the band being
  // drawn. A frame's draw is replayed once per band; pages advance their
  // state (layout steps, sync, counters) only on band 0, so every band sees
  // the same frame. Always 0 on a full framebuffer (Freenove).
  int band;
};

// A rounded panel of the page (glow is drawn around it in the backdrop).
struct PanelRect {
  int16_t x, y, w, h;
};

// What a page asks the framework (device glue) to do for it.
enum PageRequest {
  kReqNone,
  kReqAdvert,          // send the head's advert (already confirmed on the page)
  kReqOpenHeadAdmin,   // show the HEAD page with the repeater admin console
  kReqAdminLogin,      // ...and log in with the saved admin password
  kReqOpenPresets,     // ...the repeater's preset list (log in first if needed)
  kReqOpenSettings,    // ...the repeater settings list (bridge, ...)
};

class Page {
 public:
  Page() : _ctx(nullptr) {}
  virtual ~Page() {}

  virtual const char* title() const = 0;
  // Panels of the page, for the backdrop's glow.
  virtual int panels(const PanelRect*& out) const { out = nullptr; return 0; }
  // Static layer, drawn once into the page background when shown.
  virtual void drawBackground(FrameBuffer565& fb) { (void)fb; }
  // Dynamic layer (see the contract above).
  virtual void draw(FrameBuffer565& fb, uint32_t now_ms) = 0;
  // Touch forwarded by PageNav (taps arrive as down then up at the same
  // point). True = the page changed and needs a frame.
  virtual bool touch(int x, int y, bool down) { (void)x; (void)y; (void)down; return false; }
  // True while the page needs frames on its own (new data, animation).
  virtual bool wantsFrame(uint32_t now_ms) { (void)now_ms; return false; }
  virtual void onShow(uint32_t now_ms) { (void)now_ms; }
  virtual void onHide() {}
  // The page is rendered by the companion UI (DisplayDriver/LGFX), not into
  // the framebuffer: the framework hands the panel and touch over to it.
  virtual bool external() const { return false; }
  // A request raised by the last touch (taken once), and its outcome.
  virtual PageRequest takeRequest() { return kReqNone; }
  virtual void requestDone(PageRequest req, bool ok, uint32_t now_ms) { (void)req; (void)ok; (void)now_ms; }

  void attach(const PageContext* ctx) { _ctx = ctx; }

 protected:
  // The first (or only) draw of this frame: state may advance.
  bool firstBand() const { return _ctx == nullptr || _ctx->band == 0; }
  const PageContext* _ctx;
};

}  // namespace freenove_ui
