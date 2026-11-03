#pragma once

// ESP32-2432S028 "Cheap Yellow Display": ST7789 320x240 on VSPI, XPT2046
// resistive touch on its own HSPI pins, PWM backlight on GPIO21. Pin and panel
// settings are the ones proven on this board by wichat/lora_term.
//
// Visual head (FREENOVE_VISUAL_HEAD with HMI_PROFILE_CYD, env
// CYD_ESPNOW_Head_companion_usb): the Freenove head's pages
// (ui-freenove/FreenoveUITask) share the panel with the companion UI, which
// is the HEAD page. There is no full-screen buffer (no PSRAM): every frame is
// composed in horizontal bands (StripRenderer, one 320 x kBandRows RGB565
// buffer in internal RAM) and each band that changed is pushed over SPI.
//   - Pages: the page task renders and pushes (renderPage()).
//   - HEAD: the companion UI draws its usual RGB332 sprite (160 x 105
//     logical, 2x zoom); endFrame() composes the zoomed sprite and the page
//     tab bar below it (the overlay) and pushes the bands that changed.
// PanelOwner says whose frames reach the glass; owner changes, renders and
// pushes happen under one lock (both run on core 1, the lock also orders
// LovyanGFX's SPI use). The page task reads the touch controller and relays
// HEAD-page touches to getTouch(), which the companion UI polls.

#include <helpers/ui/ColorTheme.h>
#include <helpers/ui/LGFXDisplay.h>
#include <Preferences.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#if defined(FREENOVE_VISUAL_HEAD) && FREENOVE_VISUAL_HEAD
  #define CYD_VISUAL_HEAD 1
  #include <atomic>
  #include <freertos/FreeRTOS.h>
  #include <freertos/semphr.h>
  #include "FrameBuffer565.h"
  #include "PageNav.h"
  #include "StripRenderer.h"
#else
  #define CYD_VISUAL_HEAD 0
#endif

class LGFX_CYD : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;

public:
  LGFX_CYD() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      _bus.config(cfg);
    }
    _panel.setBus(&_bus);
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = true;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 21;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.x_min = 0;
      cfg.x_max = 239;
      cfg.y_min = 0;
      cfg.y_max = 319;
      cfg.pin_int = 36;
      cfg.pin_cs = 33;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.spi_host = HSPI_HOST;
      cfg.freq = 1000000;
      cfg.pin_sclk = 25;
      cfg.pin_mosi = 32;
      cfg.pin_miso = 39;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

namespace cyd {
constexpr int kScreenW = 320;
constexpr int kScreenH = 240;
#if CYD_VISUAL_HEAD
// Rows of the companion UI on the HEAD page; the tab bar is below.
constexpr int kCompanionH = kScreenH - freenove_ui::kTabBarH;
// Band height of the strip renderer: 320 x 40 x 2 = 25.6 KB.
constexpr int kBandRows = 40;
#else
constexpr int kCompanionH = kScreenH;
#endif
}  // namespace cyd

class CYDDisplay : public LGFXDisplay {
  LGFX_CYD disp;

  static constexpr const char* kCalNamespace = "tcal";
  static constexpr const char* kCalKey = "lgfx8";
  static constexpr int kRecalibratePin = 0;  // BOOT button

  // Restore saved XPT2046 calibration, or run LovyanGFX's corner-tap routine
  // on first boot (or when BOOT is held at power-up) and save the result.
  void calibrateTouch() {
    uint16_t cal[8] = {};
    Preferences prefs;
    pinMode(kRecalibratePin, INPUT_PULLUP);
    const bool force = digitalRead(kRecalibratePin) == LOW;
    bool have = false;
    if (prefs.begin(kCalNamespace, false)) {
      have = !force && prefs.getBytes(kCalKey, cal, sizeof(cal)) == sizeof(cal);
      prefs.end();
    }
    if (!have) {
      disp.fillScreen(TFT_BLACK);
      disp.setTextColor(TFT_WHITE, TFT_BLACK);
      disp.setTextDatum(lgfx::middle_center);
      disp.drawString("Touch the arrow in each corner",
                      disp.width() / 2, disp.height() / 2);
      disp.calibrateTouch(cal, TFT_RED, TFT_BLACK, 20);
      if (prefs.begin(kCalNamespace, false)) {
        prefs.putBytes(kCalKey, cal, sizeof(cal));
        prefs.end();
      }
      disp.setTextDatum(lgfx::top_left);
    }
    disp.setTouchCalibrate(cal);
  }

public:
  CYDDisplay() : LGFXDisplay(cyd::kScreenW, cyd::kCompanionH, disp) {}

#if !CYD_VISUAL_HEAD
  bool begin() {
    if (!LGFXDisplay::begin()) return false;
    calibrateTouch();
    // The zoomed sprite blit can leave power-up noise at the panel edges.
    disp.fillScreen(mesh::ui::color_theme::background(UIColor::window_bkg));
    return true;
  }
#else
  bool begin();
  // The band buffer was allocated: the visual head can run.
  bool hmiReady() const { return _pages != nullptr; }
  void startFrame(ColorVal bkg = UIColor::window_bkg) override;
  void endFrame() override;
  bool getTouch(int* x, int* y) override;

  // The touch controller, read by the page task (screen pixels).
  class Touch {
   public:
    explicit Touch(LGFX_CYD& d) : _d(d) {}
    bool read(int& x, int& y);
   private:
    LGFX_CYD& _d;
  };
  Touch& touch() { return _touchReader; }

  // ---- Visual head (FreenoveUITask) ----
  enum class PanelOwner : uint8_t { Companion, Pages };
  // Any task. To Pages: the next page frame is pushed whole. To Companion:
  // the UI loop recomposes the HEAD page on its next touch poll.
  void setPanelOwner(PanelOwner owner);
  PanelOwner panelOwner() const { return (PanelOwner)_owner.load(); }
  // Page task: compose one page frame band by band (draw is called once per
  // band, FrameBuffer565 in band mode) and push the bands that changed.
  // False when the pages do not own the panel (nothing drawn).
  bool renderPage(StripRenderer::DrawFn draw, void* ctx);
  // Bands pushed by the last renderPage(), and the frame's push time.
  int lastBandsPushed() const { return _lastPushed; }
  uint32_t lastPushUs() const { return _lastPushUs; }
  // While relayed, the page task reads the touch controller and getTouch()
  // returns what relayTouch() reported (HEAD page). relayActivity(): a
  // visual page was touched -- the UI loop wakes the screen / restarts the
  // auto-off timer as for a companion touch.
  void setTouchRelayed(bool relayed);
  void relayTouch(bool down, int x, int y);
  void relayActivity() { _activity.store(true); }
  // Drawn under the companion area (the tab bar) on every HEAD frame.
  typedef void (*Overlay)(FrameBuffer565& fb, void* ctx);
  void setCompanionOverlay(Overlay fn, void* ctx) {
    _overlayCtx = ctx;
    _overlay = fn;
  }
  // Test console screenshots: the frame as composed (the ST7789 on this
  // board does not read back), one row at a time (RGB565, native order,
  // kScreenW px). dumpPage: page task, the current page's frame (draw as
  // for renderPage); dumpCompanion: UI loop, the HEAD frame. Nothing is
  // pushed and the band hashes are untouched.
  typedef void (*RowSink)(int y, const uint16_t* row, void* ctx);
  bool dumpPage(StripRenderer::DrawFn draw, void* drawCtx, RowSink sink, void* sinkCtx);
  bool dumpCompanion(RowSink sink, void* sinkCtx);
  // Test console: labelled colour bars (panel colour order check) until the
  // next frame of whoever owns the panel.
  void showColourBars();

private:
  bool lockPanel();
  void unlockPanel();
  void composeCompanion(bool force);
  void serviceRelay();
  static void drawHeadBand(FrameBuffer565& fb, int band, void* ctx);
  static void pushBand(const uint16_t* px, int w, int y0, int rows, void* ctx);
  static void dumpBand(const uint16_t* px, int w, int y0, int rows, void* ctx);
  RowSink _dumpSink = nullptr;
  void* _dumpCtx = nullptr;

  Touch _touchReader{disp};
  uint16_t* _strip = nullptr;             // kScreenW x kBandRows (internal RAM)
  StripRenderer* _pages = nullptr;        // band hashes of the page frames
  StripRenderer* _head = nullptr;         // ...and of the HEAD frames
  uint16_t _lut565[256];                  // RGB332 sprite -> RGB565
  SemaphoreHandle_t _panelLock = nullptr; // owner changes, renders, pushes (recursive)
  std::atomic<uint8_t> _owner{(uint8_t)PanelOwner::Companion};
  std::atomic<bool> _companionRepaint{false};
  std::atomic<bool> _activity{false};
  std::atomic<bool> _relayed{false};
  portMUX_TYPE _relayMux = portMUX_INITIALIZER_UNLOCKED;
  freenove_ui::TouchRelay _relay;
  Overlay _overlay = nullptr;
  void* _overlayCtx = nullptr;
  int _lastPushed = 0;
  uint32_t _lastPushUs = 0;
  uint32_t _pushUs = 0;                   // accumulates during one render
#endif
};
