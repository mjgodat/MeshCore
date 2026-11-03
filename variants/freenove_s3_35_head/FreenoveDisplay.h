#pragma once

// Bring-up DisplayDriver for the Freenove FNK0104N head: runs MeshCore's
// standard companion UI (the CYD head's screens) on the 480x320 landscape
// ST77922 until the Freenove page framework (plan Tasks 5-9) replaces it.
//
// LGFXDisplay draws into its usual sprite and zooms it onto an LGFX "device"
// whose panel is a plain 480x320 RGB332 framebuffer in PSRAM. When a frame
// changes it is expanded to an RGB565 companion frame (LUT, PSRAM) -- the
// HEAD page's tab bar is drawn over its bottom rows -- and FramePusher sends
// the columns that changed while St77922Panel transposes landscape ->
// portrait into its DMA bounce buffers; the push runs on core 0.
//
// A 480x320 RGB565 PSRAM buffer is reserved at boot for the page
// framework's double buffering (its other buffer and the background layer
// are allocated by FreenoveUITask).
//
// Screen off only drops the backlight (LGFXDisplay::turnOff): the touch
// controller is part of the same TDDI chip, so the panel is never put to
// sleep while the UI may need a wake-up touch.
//
// FREENOVE_BRINGUP=1: boot shows the test pattern (FreenoveGeometry.h) for
// kBringupPatternMs, then the framebuffer demo (FreenoveDemo.h) takes the
// panel and logs fps / push volume over USB once per second; every touch is
// logged over USB in landscape pixels. The companion UI is not pushed.
//
// Visual head (FreenoveUITask, FREENOVE_VISUAL_HEAD): the page framework
// renders its own RGB332 frames and shares the panel with the companion UI
// (its HEAD page). PanelOwner says whose frames reach the pusher; owner
// changes and every submit happen under one lock, so a companion frame can
// never land on the glass while a visual page is current or vice versa.
// The page task owns the touch controller and relays HEAD-page touches to
// getTouch(), which the companion UI polls.

#include <atomic>

#include <helpers/ui/LGFXDisplay.h>

#include "CompanionSkin.h"
#include "CompanionText.h"
#include "FramePusher.h"
#include "PageNav.h"
#include "FreenoveGeometry.h"
#include "St77922Panel.h"
#include "St77922Touch.h"

#ifndef FREENOVE_BRINGUP
  #define FREENOVE_BRINGUP 0
#endif
// Bring-up only: after the test pattern, labelled colour bars through both
// pixel paths (RGB565 and RGB332), see showColourCheck().
// Companion UI text through the D-DIN AA renderer (CompanionText.h) instead
// of LovyanGFX's scaled 6x8 bitmap font. Freenove only; other boards keep
// their fonts.
#ifndef FREENOVE_AA_COMPANION_TEXT
  #define FREENOVE_AA_COMPANION_TEXT 1
#endif
#ifndef FREENOVE_COLOUR_CHECK
  #define FREENOVE_COLOUR_CHECK 0
#endif
// HEAD page in the HMI style (CompanionSkin.h): the companion screens'
// fills and rects become HMI panels, tiles, rows and keys over the navy
// gradient, and their text takes the theme colours. Needs the AA text path;
// the night palette keeps the legacy look. Freenove only.
#ifndef FREENOVE_HMI_SKIN
  #define FREENOVE_HMI_SKIN FREENOVE_AA_COMPANION_TEXT
#endif
#if FREENOVE_HMI_SKIN && !FREENOVE_AA_COMPANION_TEXT
  #error "FREENOVE_HMI_SKIN needs FREENOVE_AA_COMPANION_TEXT"
#endif

namespace freenove {

// Rows the companion UI draws into. With the visual head the bottom
// kTabBarH rows belong to the page tab bar (drawn by the HEAD page's
// overlay): the companion's LGFX device is only kCompanionH rows tall, so it
// can never draw over the bar, and its touches map from that area alone.
#if defined(FREENOVE_VISUAL_HEAD) && FREENOVE_VISUAL_HEAD && !FREENOVE_BRINGUP
constexpr int kCompanionH = kLandscapeH - freenove_ui::kTabBarH;
#else
constexpr int kCompanionH = kLandscapeH;
#endif

// LovyanGFX panel backed by an RGB332 framebuffer (rotation 0, 480 x
// kCompanionH; row stride kLandscapeW).
class Panel_Fb332 : public lgfx::Panel_FlexibleFrameBuffer {
public:
  Panel_Fb332();
  void setFrameBuffer(uint8_t* fb) { _fb = fb; }
  uint8_t* frameBuffer() const { return _fb; }
  lgfx::color_depth_t setColorDepth(lgfx::color_depth_t) override {
    _write_depth = lgfx::rgb332_1Byte;
    _read_depth = lgfx::rgb332_1Byte;
    return lgfx::rgb332_1Byte;
  }

protected:
  uint32_t _read_pixel_inner(uint_fast16_t x, uint_fast16_t y) override;
  void _draw_pixel_inner(uint_fast16_t x, uint_fast16_t y,
                         uint32_t rawcolor) override;
  void _fill_rect_inner(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w,
                        uint_fast16_t h, uint32_t rawcolor) override;

private:
  uint8_t* _fb = nullptr;
};

// Backlight through St77922Panel (LEDC on GPIO 41).
class PanelLight : public lgfx::ILight {
public:
  explicit PanelLight(St77922Panel& panel) : _panel(panel) {}
  bool init(uint8_t brightness) override {
    _panel.setBacklight(brightness);
    return true;
  }
  void setBrightness(uint8_t brightness) override {
    _panel.setBacklight(brightness);
  }

private:
  St77922Panel& _panel;
};

class LGFX_Fb332 : public lgfx::LGFX_Device {
public:
  explicit LGFX_Fb332(St77922Panel& panel);
  Panel_Fb332& fbPanel() { return _panel; }

private:
  Panel_Fb332 _panel;
  lgfx::Bus_NULL _bus;
  PanelLight _light;
};

}  // namespace freenove

class FreenoveDisplay : public LGFXDisplay {
public:
  FreenoveDisplay();
  bool begin();
  void startFrame(ColorVal bkg = UIColor::window_bkg) override;
  void endFrame() override;
  bool getTouch(int* x, int* y) override;
#if FREENOVE_AA_COMPANION_TEXT
  void setTextSize(int sz) override;
  void setCursor(int x, int y) override;
  void print(const char* str) override;
  uint16_t getTextWidth(const char* str) override;
  void fillRect(int x, int y, int w, int h) override;
#if FREENOVE_HMI_SKIN
  void setColor(ColorVal c) override;
  void drawRect(int x, int y, int w, int h) override;
#endif
  int textLineHeight() override { return freenove_ui::CompanionText::lineHeight(_aaSize); }
#endif

  St77922Panel& panel() { return _lcd; }
  St77922Touch& touch() { return _touch; }
  FramePusher& pusher() { return _pusher; }

  // ---- Visual head (FreenoveUITask) ----
  enum class PanelOwner : uint8_t { Companion, Pages };
  // Any task. To Pages: waits for a companion push in flight, then the next
  // page frame is a full resend. To Companion: the UI loop repaints the
  // companion frame on its next touch poll (getTouch()).
  void setPanelOwner(PanelOwner owner);
  PanelOwner panelOwner() const { return (PanelOwner)_owner.load(); }
  // Page task: hand a finished page frame to the pusher (waits for the
  // previous push). False when the pages do not own the panel or the push
  // was dropped (the pusher then resends everything next frame).
  bool submitPage(FrameBuffer565& fb);
  // While relayed, the page task reads the touch controller and getTouch()
  // returns what relayTouch() reported (HEAD page). relayActivity(): a
  // visual page was touched -- the UI loop wakes the screen / restarts the
  // auto-off timer as for a companion touch.
  void setTouchRelayed(bool relayed);
  void relayTouch(bool down, int x, int y);
  void relayActivity() { _activity.store(true); }
  // Drawn over the companion frame before each push while it owns the panel.
  typedef void (*Overlay)(FrameBuffer565& fb, void* ctx);
  void setCompanionOverlay(Overlay fn, void* ctx) {
    _overlayCtx = ctx;
    _overlay = fn;
  }
  // The 480x320 RGB565 PSRAM buffer reserved at boot for the pages.
  uint16_t* reservedPageBuffer() { return _backFb; }

private:
  bool lockPanel();
  void unlockPanel();
  void pushCompanionFrame();
  void serviceRelay();
  void pushFramebuffer();
  void waitForPush();
  void releaseAll();
  void failed(const char* what);
  void showTestPattern();
#if FREENOVE_BRINGUP && FREENOVE_COLOUR_CHECK
  void showColourCheck();
  static constexpr uint32_t kColourCheckMs = 3000;
#endif

  St77922Panel _lcd;
  St77922Touch _touch;
  FramePusher _pusher;
  freenove::LGFX_Fb332 _dev;
  uint8_t* _frontFb = nullptr;   // LGFX render target (PSRAM)
  // The companion frame as pushed: _frontFb expanded to RGB565 (+ overlay).
  uint16_t* _companion565 = nullptr;
  FrameBuffer565 _companionView{nullptr, 0, 0};
  uint16_t _lut565[256];         // RGB332 -> RGB565, native order
  uint16_t* _backFb = nullptr;   // reserved for the page framework (PSRAM)
#if FREENOVE_AA_COMPANION_TEXT
  // Text of the frame being drawn, and of the frame on the glass (drawn
  // over every companion push).
  freenove_ui::CompanionText _text;
  freenove_ui::CompanionText _shownText;
  uint32_t _shownTextHash = 0;
  int _aaSize = 1;
  int _cursorX = 0, _cursorY = 0;
#endif
#if FREENOVE_HMI_SKIN
  // Shapes of the frame being drawn and of the frame on the glass; the
  // backdrop (PSRAM, companion rows) every skinned frame starts from.
  static freenove_ui::CompanionPalette skinPalette();
  freenove_ui::CompanionSkin _skin;
  freenove_ui::CompanionSkin _shownSkin;
  uint32_t _shownSkinHash = 0;
  uint16_t* _backdrop565 = nullptr;
  ColorVal _semantic = 0;        // the UIColor value last set
  bool _skinOn = false;          // this frame is skinned (day palette, backdrop)
  bool _shownSkinOn = false;
  uint8_t _skinKey = 0;          // the sprite's background, RGB332 raw
  uint8_t _shownSkinKey = 0;
#endif
  uint16_t* _rows565 = nullptr;  // expansion buffer, kPushRows rows (PSRAM)
  bool _begun = false;           // set only when begin() fully succeeded
  SemaphoreHandle_t _panelLock = nullptr;   // owner changes + submits
  std::atomic<uint8_t> _owner{(uint8_t)PanelOwner::Companion};
  std::atomic<bool> _companionRepaint{false};
  std::atomic<bool> _activity{false};
  std::atomic<bool> _relayed{false};
  portMUX_TYPE _relayMux = portMUX_INITIALIZER_UNLOCKED;
  freenove_ui::TouchRelay _relay;
  Overlay _overlay = nullptr;
  void* _overlayCtx = nullptr;
#if FREENOVE_BRINGUP
  bool _touchDown = false;
  uint32_t _lastTouchLog = 0;
#endif
};
