#include "FreenoveDisplay.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

#include <helpers/ui/DisplayNightMode.h>
#include <helpers/ui/DisplayTouchCoordinates.h>

#include "BootBeacon.h"

#if FREENOVE_BRINGUP
  #include "FreenoveDemo.h"
  #include "Theme.h"
#endif

namespace {
constexpr int kFbBytes = freenove::kLandscapeW * freenove::kLandscapeH;
constexpr int kFb565Bytes = kFbBytes * 2;
// Landscape rows expanded per test-pattern push (multiple of kRowAlign).
constexpr int kPushRows = 8;
#if FREENOVE_BRINGUP
// Test pattern on screen before the framebuffer demo starts.
constexpr uint32_t kBringupPatternMs = 6000;
#endif
}  // namespace

namespace freenove {

Panel_Fb332::Panel_Fb332() {
  auto cfg = config();
  cfg.panel_width = kLandscapeW;
  cfg.panel_height = kCompanionH;
  cfg.memory_width = kLandscapeW;
  cfg.memory_height = kCompanionH;
  cfg.offset_x = 0;
  cfg.offset_y = 0;
  cfg.offset_rotation = 0;
  cfg.pin_cs = -1;
  cfg.pin_rst = -1;
  cfg.pin_busy = -1;
  cfg.readable = true;
  cfg.bus_shared = false;
  config(cfg);
  _write_depth = lgfx::rgb332_1Byte;
  _read_depth = lgfx::rgb332_1Byte;
}

uint32_t Panel_Fb332::_read_pixel_inner(uint_fast16_t x, uint_fast16_t y) {
  if (_fb == nullptr || x >= (uint_fast16_t)kLandscapeW
      || y >= (uint_fast16_t)kCompanionH) {
    return 0;
  }
  return _fb[y * kLandscapeW + x];
}

void Panel_Fb332::_draw_pixel_inner(uint_fast16_t x, uint_fast16_t y,
                                    uint32_t rawcolor) {
  if (_fb == nullptr || x >= (uint_fast16_t)kLandscapeW
      || y >= (uint_fast16_t)kCompanionH) {
    return;
  }
  _fb[y * kLandscapeW + x] = (uint8_t)rawcolor;
}

void Panel_Fb332::_fill_rect_inner(uint_fast16_t x, uint_fast16_t y,
                                   uint_fast16_t w, uint_fast16_t h,
                                   uint32_t rawcolor) {
  if (_fb == nullptr || x >= (uint_fast16_t)kLandscapeW
      || y >= (uint_fast16_t)kCompanionH) {
    return;
  }
  if (x + w > (uint_fast16_t)kLandscapeW) w = kLandscapeW - x;
  if (y + h > (uint_fast16_t)kCompanionH) h = kCompanionH - y;
  for (uint_fast16_t row = 0; row < h; row++) {
    memset(&_fb[(y + row) * kLandscapeW + x], (uint8_t)rawcolor, w);
  }
}

LGFX_Fb332::LGFX_Fb332(St77922Panel& panel) : _light(panel) {
  _panel.setBus(&_bus);
  _panel.setLight(&_light);
  setPanel(&_panel);
}

}  // namespace freenove

FreenoveDisplay::FreenoveDisplay()
  : LGFXDisplay(freenove::kLandscapeW, freenove::kCompanionH, _dev),
    _dev(_lcd) {}

void FreenoveDisplay::releaseAll() {
  if (!_pusher.end()) {
    // A push may still be reading _frontFb: leak the buffers rather than
    // free memory the push task could touch.
    _begun = false;
    return;
  }
  _dev.fbPanel().setFrameBuffer(nullptr);
  _companionView = FrameBuffer565(nullptr, 0, 0);
  if (_companion565 != nullptr) heap_caps_free(_companion565);
  _companion565 = nullptr;
  if (_frontFb != nullptr) heap_caps_free(_frontFb);
  if (_backFb != nullptr) heap_caps_free(_backFb);
  if (_rows565 != nullptr) heap_caps_free(_rows565);
#if FREENOVE_HMI_SKIN
  if (_backdrop565 != nullptr) heap_caps_free(_backdrop565);
  _backdrop565 = nullptr;
#endif
  _frontFb = nullptr;
  _backFb = nullptr;
  _rows565 = nullptr;
  _lcd.end();
  _begun = false;
}

bool FreenoveDisplay::begin() {
  if (_begun) return true;
  freenove::setBootStage(freenove::BootStage::Display);
  if (_panelLock == nullptr) _panelLock = xSemaphoreCreateMutex();

  if (!_lcd.begin()) {
    failed("ST77922 panel init");
    return false;
  }
  // Touch is reset here and probed once it has warmed up (St77922Touch).
  _touch.begin();
  freenove::setBootStage(freenove::BootStage::Panel);

  _frontFb = (uint8_t*)heap_caps_malloc(kFbBytes, MALLOC_CAP_SPIRAM);
  _backFb = (uint16_t*)heap_caps_malloc(kFb565Bytes, MALLOC_CAP_SPIRAM);
  _companion565 = (uint16_t*)heap_caps_malloc(kFb565Bytes, MALLOC_CAP_SPIRAM);
  _rows565 = (uint16_t*)heap_caps_malloc(
      kPushRows * freenove::kLandscapeW * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  if (_frontFb == nullptr || _backFb == nullptr || _companion565 == nullptr
      || _rows565 == nullptr) {
    Serial.println("freenove: PSRAM framebuffer allocation failed");
    releaseAll();
    failed("PSRAM framebuffer allocation");
    return false;
  }
#if FREENOVE_HMI_SKIN
  // Optional: without it the HEAD page keeps the legacy look.
  _backdrop565 = (uint16_t*)heap_caps_malloc(
      freenove::kLandscapeW * freenove::kCompanionH * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  if (_backdrop565 != nullptr) {
    freenove_ui::CompanionSkin::fillBackdrop(_backdrop565, freenove::kLandscapeW,
                                             freenove::kCompanionH, freenove::kLandscapeH);
  } else {
    Serial.println("freenove: HMI skin backdrop allocation failed; legacy HEAD look");
  }
#endif
  memset(_frontFb, 0, kFbBytes);
  memset(_backFb, 0, kFb565Bytes);
  memset(_companion565, 0, kFb565Bytes);
  buildLut565(_lut565, false);
  _companionView = FrameBuffer565(_companion565, freenove::kLandscapeW,
                                  freenove::kLandscapeH);
  _dev.fbPanel().setFrameBuffer(_frontFb);
  if (!_pusher.begin(_lcd, true)) {
    Serial.println("freenove: frame pusher init failed");
    releaseAll();
    failed("frame pusher init");
    return false;
  }

  Serial.printf(
      "freenove: framebuffers %d B (RGB332) + 2x%d B (RGB565) PSRAM; free internal %u B (largest %u),"
      " free PSRAM %u B\n",
      kFbBytes, kFb565Bytes, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  freenove::setBootStage(freenove::BootStage::Memory);

#if FREENOVE_BRINGUP
  showTestPattern();
  _lcd.setBacklight(DISPLAY_BRIGHTNESS);
  freenove::setBootStage(freenove::BootStage::Ready);
#if FREENOVE_COLOUR_CHECK
  showColourCheck();
#endif
  Serial.println("freenove: BRINGUP test pattern -- bars L->R white yellow "
                 "cyan green magenta red blue black; corners TL orange, "
                 "TR teal, BL purple, BR grey");
  freenove::startFramebufferDemo(_pusher, _lcd, kBringupPatternMs);
#endif

  if (!LGFXDisplay::begin()) {
    Serial.println("freenove: UI canvas allocation failed");
    releaseAll();
    failed("UI canvas allocation");
    return false;
  }
  _begun = true;
  freenove::setBootStage(freenove::BootStage::Ready);
  return true;
}

void FreenoveDisplay::failed(const char* what) {
  // Never go dark on a failed start: a lit blank glass says "firmware runs,
  // display did not start"; the bring-up beacon repeats `what` over USB.
  freenove::forceBacklightOn();
  freenove::setBootStage(freenove::BootStage::DisplayFailed, what);
}

#if FREENOVE_BRINGUP && FREENOVE_COLOUR_CHECK
// Labelled bars R G B C M Y W, then the HMI theme's navy / panel / cyan /
// green / amber, drawn twice: through the RGB565 page path and through the
// RGB332 LUT path (the 7b builds' path). Each frame is shown for
// kColourCheckMs, three times. Both must look identical on the glass and
// match their labels; if both are colour-negative the panel needs
// FREENOVE_PANEL_INVERT=1.
void FreenoveDisplay::showColourCheck() {
  using namespace freenove;
  static const struct { const char* name; uint8_t r, g, b; } kBars[] = {
    {"R", 255, 0, 0}, {"G", 0, 255, 0}, {"B", 0, 0, 255}, {"C", 0, 255, 255},
    {"M", 255, 0, 255}, {"Y", 255, 255, 0}, {"W", 255, 255, 255},
    {"NAVY", 8, 24, 36}, {"PANEL", 10, 60, 72}, {"CYAN", 40, 220, 240},
    {"GRN", 50, 230, 140}, {"AMB", 255, 175, 40}};
  const int n = (int)(sizeof(kBars) / sizeof(kBars[0]));
  delay(kBringupPatternMs);   // the test pattern first
  const int bw = kLandscapeW / 7;
  FrameBuffer565 f565(_backFb, kLandscapeW, kLandscapeH);
  FrameBuffer332 f332(_frontFb, kLandscapeW, kLandscapeH);
  for (int pass = 0; pass < 2; pass++) {
    const bool is565 = pass == 0;
    if (is565) f565.clear(0); else f332.clear(0);
    for (int i = 0; i < n; i++) {
      const int row = i < 7 ? 0 : 1;
      const int col = i < 7 ? i : i - 7;
      const int w = row == 0 ? bw : kLandscapeW / 5;
      const int x = col * w, y = 40 + row * 130, h = 120;
      if (is565) {
        f565.fillRect(x + 2, y, w - 4, h, rgb565(kBars[i].r, kBars[i].g, kBars[i].b));
        f565.text(x + w / 2, y + h + 4, kBars[i].name, 0xFFFF, kFontSmall, FrameBuffer565::kCentreTop);
      } else {
        f332.fillRect(x + 2, y, w - 4, h, FrameBuffer332::rgb(kBars[i].r, kBars[i].g, kBars[i].b));
        f332.text(x + w / 2 - FrameBuffer332::textWidth(kBars[i].name, kFont8x16Id) / 2, y + h + 4,
                  kBars[i].name, 0xFF, kFont8x16Id);
      }
    }
    if (is565) {
      f565.text(240, 8, "RGB565 PATH (pages)", 0xFFFF, kFontTitle, FrameBuffer565::kCentreTop);
    } else {
      f332.text(240 - FrameBuffer332::textWidth("RGB332 PATH (7b)", kFont8x16Id) / 2, 10, "RGB332 PATH (7b)", 0xFF,
                kFont8x16Id);
    }
  }
  for (int cycle = 0; cycle < 3; cycle++) {
    f565.markAll();
    _pusher.submit(f565);
    _pusher.wait();
    Serial.println("freenove: colour check -- RGB565 path");
    delay(kColourCheckMs);
    f332.markAll();
    _pusher.submit(f332);
    _pusher.wait();
    Serial.println("freenove: colour check -- RGB332 path");
    delay(kColourCheckMs);
  }
  memset(_frontFb, 0, kLandscapeW * kLandscapeH);
  memset(_backFb, 0, kLandscapeW * kLandscapeH * 2);
  _pusher.invalidate();
}
#endif

void FreenoveDisplay::showTestPattern() {
  using namespace freenove;
  for (int y0 = 0; y0 < kLandscapeH; y0 += kPushRows) {
    for (int r = 0; r < kPushRows; r++)
      for (int x = 0; x < kLandscapeW; x++)
        _rows565[r * kLandscapeW + x] = testPatternColor(x, y0 + r);
    _lcd.pushRows(_rows565, y0, kPushRows);
  }
  _lcd.waitIdle();
}

void FreenoveDisplay::pushFramebuffer() {
  if (_frontFb == nullptr || _companion565 == nullptr) return;
  // LGFX wrote the frame behind the primitives' back: expand all of it (the
  // view marks the whole frame); the planner's shadow compare sends only
  // what changed. Returns once the push task has the frame.
  uint16_t* dst = _companionView.rawPixels();
#if FREENOVE_HMI_SKIN
  if (_shownSkinOn && _backdrop565 != nullptr) {
    // Backdrop, the HMI shapes, then the sprite's own pixels (icons, QR,
    // emoji) where it is not background; the rows below are the tab bar's.
    constexpr int kSkinPx = freenove::kLandscapeW * freenove::kCompanionH;
    memcpy(dst, _backdrop565, kSkinPx * sizeof(uint16_t));
    _shownSkin.draw(_companionView, freenove::kCompanionH, _backdrop565, skinPalette());
    freenove_ui::CompanionSkin::overlaySprite(dst, _frontFb, kSkinPx, _shownSkinKey, _lut565);
    for (int i = kSkinPx; i < kFbBytes; i++) dst[i] = _lut565[_frontFb[i]];
  } else
#endif
  for (int i = 0; i < kFbBytes; i++) dst[i] = _lut565[_frontFb[i]];
#if FREENOVE_AA_COMPANION_TEXT
  _shownText.draw(_companionView, freenove::kCompanionH);
#endif
  if (_overlay != nullptr) _overlay(_companionView, _overlayCtx);
  _pusher.submit(_companionView);
}

void FreenoveDisplay::waitForPush() {
  // A push that does not finish in FramePusher::kWaitMs is stuck (SPI
  // timeouts are bounded far below that): LGFX may then race it while
  // writing _frontFb, so the next push resends the whole frame.
  if (!_pusher.wait()) {
    _pusher.invalidate();
    Serial.println("freenove: push still running; full resend next frame");
  }
}

void FreenoveDisplay::startFrame(ColorVal bkg) {
#if !FREENOVE_BRINGUP
  // startFrame may fillScreen() _frontFb (day/night switch): the previous
  // frame's push must be done first. Only a companion push reads _frontFb,
  // and only while the companion owns the panel: otherwise the pusher is
  // busy with page frames and waiting would couple this loop to them.
  if (panelOwner() == PanelOwner::Companion) waitForPush();
#endif
  LGFXDisplay::startFrame(bkg);
#if FREENOVE_AA_COMPANION_TEXT
  _text.clear();
#endif
#if FREENOVE_HMI_SKIN
  _skin.clear();
  _skinOn = _backdrop565 != nullptr && !mesh::ui::displayNightActive();
  // The sprite was just cleared to the background: its raw RGB332 value is
  // the key for "nothing drawn here" when the frame is composed.
  const uint8_t* raw = (const uint8_t*)buffer.getBuffer();
  if (raw != nullptr) _skinKey = raw[0];
#endif
}

#if FREENOVE_AA_COMPANION_TEXT
void FreenoveDisplay::setTextSize(int sz) {
  LGFXDisplay::setTextSize(sz);
  _aaSize = sz < 1 ? 1 : sz;
}

void FreenoveDisplay::setCursor(int x, int y) {
  LGFXDisplay::setCursor(x, y);
  _cursorX = x;
  _cursorY = y;
}

// Recorded, not drawn into the sprite: drawn AA over the zoomed frame.
void FreenoveDisplay::print(const char* str) {
  char ascii[256];
  translateUTF8ToBlocks(ascii, str != nullptr ? str : "", sizeof(ascii));
#if FREENOVE_HMI_SKIN
  if (_skinOn) {
    const freenove_ui::CompanionPalette pal = skinPalette();
    _cursorX = _text.add(_cursorX, _cursorY, _aaSize, freenove_ui::skinTextColour(_semantic, pal), ascii,
                         _skin.textPad(_cursorX, _cursorY, pal));
    return;
  }
#endif
  _cursorX = _text.add(_cursorX, _cursorY, _aaSize, (uint16_t)_color, ascii);
}

// A fill over earlier text (a popup, a highlight) hides that text.
void FreenoveDisplay::fillRect(int x, int y, int w, int h) {
#if FREENOVE_HMI_SKIN
  if (_skinOn) {
    // Recorded as an HMI shape; in the sprite it clears to the background,
    // so icons drawn under it earlier stay hidden as before.
    _skin.fill(x, y, w, h, _semantic);
    const int saved = _color;
    _color = renderColor(UIColor::window_bkg);
    LGFXDisplay::fillRect(x, y, w, h);
    _color = saved;
    _text.occlude(x, y, w, h);
    return;
  }
#endif
  LGFXDisplay::fillRect(x, y, w, h);
  _text.occlude(x, y, w, h);
}

#if FREENOVE_HMI_SKIN
freenove_ui::CompanionPalette FreenoveDisplay::skinPalette() {
  return freenove_ui::CompanionPalette{UIColor::window_bkg, UIColor::title_bkg, UIColor::popup_bkg,
                                       UIColor::corp_blue, UIColor::title_txt, UIColor::primary_txt,
                                       UIColor::secondary_txt, UIColor::warning_txt, UIColor::popup_txt};
}

void FreenoveDisplay::setColor(ColorVal c) {
  _semantic = c;
  LGFXDisplay::setColor(c);
}

void FreenoveDisplay::drawRect(int x, int y, int w, int h) {
  if (_skinOn) {
    _skin.rect(x, y, w, h, _semantic);
    return;
  }
  LGFXDisplay::drawRect(x, y, w, h);
}
#endif

uint16_t FreenoveDisplay::getTextWidth(const char* str) {
  char ascii[256];
  translateUTF8ToBlocks(ascii, str != nullptr ? str : "", sizeof(ascii));
  return (uint16_t)freenove_ui::CompanionText::logicalWidth(ascii, _aaSize);
}
#endif

void FreenoveDisplay::endFrame() {
  const bool had = _hasLastFrame;
  const uint32_t previous = _lastFrameHash;
#if !FREENOVE_BRINGUP
  // LGFX writes _frontFb below: the previous companion push must be done
  // (none is in flight while a visual page owns the panel; see startFrame).
  if (panelOwner() == PanelOwner::Companion) waitForPush();
#endif
  LGFXDisplay::endFrame();
#if FREENOVE_AA_COMPANION_TEXT
  // A frame whose text (or HMI shapes) alone changed is pushed too.
  const uint32_t textHash = _text.hash();
#if FREENOVE_HMI_SKIN
  const uint32_t skinHash = _skinOn ? _skin.hash() : 0;
  if (had && _hasLastFrame && _lastFrameHash == previous && textHash == _shownTextHash
      && skinHash == _shownSkinHash && _skinOn == _shownSkinOn) return;
  _shownSkin = _skin;   // UI loop, like the text
  _shownSkinHash = skinHash;
  _shownSkinOn = _skinOn;
  _shownSkinKey = _skinKey;
#else
  if (had && _hasLastFrame && _lastFrameHash == previous && textHash == _shownTextHash) return;
#endif
  _shownText = _text;   // UI loop: the only reader is pushFramebuffer, also here
  _shownTextHash = textHash;
#else
  if (had && _hasLastFrame && _lastFrameHash == previous) return;
#endif
#if FREENOVE_BRINGUP
  return;   // keep the test pattern on screen
#else
  // While a visual page owns the panel the frame stays in _frontFb; it is
  // pushed when the HEAD page is shown again.
  pushCompanionFrame();
#endif
}

bool FreenoveDisplay::lockPanel() {
  if (_panelLock == nullptr) return true;
  // A submit inside the lock waits at most kWaitMs for the previous push.
  return xSemaphoreTake(_panelLock,
                        pdMS_TO_TICKS(2 * FramePusher::kWaitMs)) == pdTRUE;
}

void FreenoveDisplay::unlockPanel() {
  if (_panelLock != nullptr) xSemaphoreGive(_panelLock);
}

void FreenoveDisplay::pushCompanionFrame() {
  if (!lockPanel()) return;
  if (panelOwner() == PanelOwner::Companion) {
    _companionRepaint.store(false);
    waitForPush();
    pushFramebuffer();   // with the overlay
  }
  unlockPanel();
}

void FreenoveDisplay::setPanelOwner(PanelOwner owner) {
  const bool locked = lockPanel();
  const PanelOwner was = panelOwner();
  if (owner != was) {
    if (owner == PanelOwner::Pages) {
      // A companion push still in flight would consume the invalidate.
      waitForPush();
      _pusher.invalidate();
      _companionRepaint.store(false);
    } else {
      _companionRepaint.store(true);
    }
    _owner.store((uint8_t)owner);
  }
  if (locked) unlockPanel();
}

bool FreenoveDisplay::submitPage(FrameBuffer565& fb) {
  if (!_begun || !lockPanel()) return false;
  bool taken = false;
  if (panelOwner() == PanelOwner::Pages) taken = _pusher.submit(fb);
  unlockPanel();
  return taken;
}

void FreenoveDisplay::setTouchRelayed(bool relayed) {
  portENTER_CRITICAL(&_relayMux);
  _relay.reset();
  portEXIT_CRITICAL(&_relayMux);
  _relayed.store(relayed);
}

void FreenoveDisplay::relayTouch(bool down, int x, int y) {
  portENTER_CRITICAL(&_relayMux);
  _relay.set(down, x, y);
  portEXIT_CRITICAL(&_relayMux);
}

void FreenoveDisplay::serviceRelay() {
  // UI loop context: DisplayDriver power state and LGFX belong to it.
  if (_activity.exchange(false)) wake(mesh::ui::DisplayWake::Button);
  if (_companionRepaint.load()) pushCompanionFrame();
}

bool FreenoveDisplay::getTouch(int* x, int* y) {
  if (x == nullptr || y == nullptr) return false;
  *x = *y = -1;
  int lx = -1, ly = -1;
  bool down = false;
  if (_relayed.load()) {
    serviceRelay();
    portENTER_CRITICAL(&_relayMux);
    down = _relay.read(lx, ly);
    portEXIT_CRITICAL(&_relayMux);
  } else {
    down = _touch.read(lx, ly);
#if FREENOVE_BRINGUP
    const uint32_t now = millis();
    if (down && (!_touchDown || now - _lastTouchLog >= 250)) {
      Serial.printf("touch x=%d y=%d (raw %d,%d)\n", lx, ly, _touch.rawX(),
                    _touch.rawY());
      _lastTouchLog = now;
    } else if (!down && _touchDown) {
      Serial.println("touch up");
    }
    _touchDown = down;
#endif
  }
  if (!down) return false;
  // The companion UI owns the rows above the tab bar. A relayed drag that
  // slides onto the bar stays on the companion's last row; a direct read
  // there (visual head not running) is not the companion's.
  if (_relayed.load() && ly >= freenove::kCompanionH) ly = freenove::kCompanionH - 1;
  return mesh::ui::panelTouchToLogical(lx, ly, freenove::kLandscapeW,
                                       freenove::kCompanionH, width(),
                                       height(), x, y);
}
