#include <Arduino.h>

#include "CYDDisplay.h"

#if CYD_VISUAL_HEAD

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <helpers/ui/DisplayTouchCoordinates.h>

#include "FrameBuffer332.h"   // buildLut565

namespace {

constexpr size_t kStripPx = (size_t)cyd::kScreenW * cyd::kBandRows;

}  // namespace

bool CYDDisplay::Touch::read(int& x, int& y) {
  lgfx::touch_point_t tp;
  if (_d.getTouch(&tp) == 0) {
    x = y = -1;
    return false;
  }
  x = tp.x;
  y = tp.y;
  return true;
}

bool CYDDisplay::begin() {
  if (!LGFXDisplay::begin()) return false;
  calibrateTouch();
  // Frames arrive as RGB565 (the companion sprite is expanded through a LUT).
  disp.setColorDepth(16);
  buildLut565(_lut565, false);
  _panelLock = xSemaphoreCreateRecursiveMutex();
  _strip = (uint16_t*)heap_caps_malloc(kStripPx * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (_strip != nullptr && _panelLock != nullptr) {
    _pages = new StripRenderer(_strip, cyd::kScreenW, cyd::kScreenH, cyd::kBandRows);
    _head = new StripRenderer(_strip, cyd::kScreenW, cyd::kScreenH, cyd::kBandRows);
  }
  if (_pages == nullptr || _head == nullptr) {
    // No band buffer: the companion UI alone, pushed by LovyanGFX as before.
    Serial.println("cyd: no strip buffer; companion UI only");
    delete _pages;
    delete _head;
    _pages = _head = nullptr;
    heap_caps_free(_strip);
    _strip = nullptr;
  }
  disp.fillScreen(mesh::ui::color_theme::background(UIColor::window_bkg));
  return true;
}

bool CYDDisplay::lockPanel() {
  if (_panelLock == nullptr) return true;
  return xSemaphoreTakeRecursive(_panelLock, pdMS_TO_TICKS(1000)) == pdTRUE;
}

void CYDDisplay::unlockPanel() {
  if (_panelLock != nullptr) xSemaphoreGiveRecursive(_panelLock);
}

void CYDDisplay::startFrame(ColorVal bkg) {
  // A day/night switch fills the panel (LovyanGFX): ordered with the pushes,
  // and every band is resent afterwards.
  const bool night = _nightFrame;
  const bool locked = lockPanel();
  LGFXDisplay::startFrame(bkg);
  if (night != _nightFrame && _pages != nullptr) {
    _pages->invalidate();
    _head->invalidate();
  }
  if (locked) unlockPanel();
}

void CYDDisplay::endFrame() {
  if (_strip == nullptr) {
    LGFXDisplay::endFrame();
    return;
  }
  const uint32_t hash = frameHash();
  const bool changed = !_hasLastFrame || hash != _lastFrameHash;
  _lastFrameHash = hash;
  _hasLastFrame = true;
  if (changed || _companionRepaint.load()) composeCompanion(false);
}

// UI loop: the HEAD page = the zoomed companion sprite + the tab bar.
void CYDDisplay::composeCompanion(bool force) {
  if (_head == nullptr || !lockPanel()) return;
  if (panelOwner() == PanelOwner::Companion) {
    _companionRepaint.store(false);
    _pushUs = 0;
    _head->render(drawHeadBand, this, pushBand, this, force);
  }
  unlockPanel();
}

void CYDDisplay::drawHeadBand(FrameBuffer565& fb, int band, void* ctx) {
  (void)band;
  CYDDisplay* self = static_cast<CYDDisplay*>(ctx);
  const uint8_t* src = static_cast<const uint8_t*>(self->buffer.getBuffer());
  const int sw = self->buffer.width(), sh = self->buffer.height();
  const int zoom = 2;
  int y0 = fb.clipY0(), y1 = fb.clipY1() + 1;
  if (y1 > cyd::kCompanionH) y1 = cyd::kCompanionH;
  uint16_t* px = fb.rawPixels();
  for (int y = y0; y < y1; y++) {
    uint16_t* row = px + (long)y * fb.width();
    const int sy = y / zoom;
    if (src == nullptr || sy >= sh) {
      for (int x = 0; x < fb.width(); x++) row[x] = 0;
      continue;
    }
    const uint8_t* s = src + (long)sy * sw;
    for (int x = 0; x < fb.width(); x++) {
      const int sx = x / zoom;
      row[x] = sx < sw ? self->_lut565[s[sx]] : 0;
    }
  }
  if (self->_overlay != nullptr) {
    fb.resetClip();
    self->_overlay(fb, self->_overlayCtx);
  }
}

void CYDDisplay::pushBand(const uint16_t* px, int w, int y0, int rows, void* ctx) {
  CYDDisplay* self = static_cast<CYDDisplay*>(ctx);
  const int64_t t0 = esp_timer_get_time();
  self->disp.pushImage(0, y0, w, rows, reinterpret_cast<const lgfx::rgb565_t*>(px));
  self->_pushUs += (uint32_t)(esp_timer_get_time() - t0);
}

bool CYDDisplay::renderPage(StripRenderer::DrawFn draw, void* ctx) {
  if (_pages == nullptr || !lockPanel()) return false;
  bool drawn = false;
  if (panelOwner() == PanelOwner::Pages) {
    _pushUs = 0;
    _lastPushed = _pages->render(draw, ctx, pushBand, this);
    _lastPushUs = _pushUs;
    drawn = true;
  }
  unlockPanel();
  return drawn;
}

void CYDDisplay::setPanelOwner(PanelOwner owner) {
  const bool locked = lockPanel();
  if (owner != panelOwner()) {
    if (owner == PanelOwner::Pages) {
      if (_pages != nullptr) _pages->invalidate();
    } else {
      if (_head != nullptr) _head->invalidate();
      _companionRepaint.store(true);
    }
    _owner.store((uint8_t)owner);
  }
  if (locked) unlockPanel();
}

void CYDDisplay::setTouchRelayed(bool relayed) {
  portENTER_CRITICAL(&_relayMux);
  _relay.reset();
  portEXIT_CRITICAL(&_relayMux);
  _relayed.store(relayed);
}

void CYDDisplay::relayTouch(bool down, int x, int y) {
  portENTER_CRITICAL(&_relayMux);
  _relay.set(down, x, y);
  portEXIT_CRITICAL(&_relayMux);
}

void CYDDisplay::serviceRelay() {
  // UI loop context: DisplayDriver power state and the sprite belong to it.
  if (_activity.exchange(false)) wake(mesh::ui::DisplayWake::Button);
  if (_companionRepaint.load()) composeCompanion(true);
}

bool CYDDisplay::getTouch(int* x, int* y) {
  if (x == nullptr || y == nullptr) return false;
  *x = *y = -1;
  if (!_relayed.load()) return LGFXDisplay::getTouch(x, y);
  serviceRelay();
  int lx = -1, ly = -1;
  portENTER_CRITICAL(&_relayMux);
  const bool down = _relay.read(lx, ly);
  portEXIT_CRITICAL(&_relayMux);
  if (!down) return false;
  // The companion UI owns the rows above the tab bar; a drag that slides
  // onto the bar stays on its last row.
  if (ly >= cyd::kCompanionH) ly = cyd::kCompanionH - 1;
  return mesh::ui::panelTouchToLogical(lx, ly, cyd::kScreenW, cyd::kCompanionH, width(), height(), x, y);
}

void CYDDisplay::dumpBand(const uint16_t* px, int w, int y0, int rows, void* ctx) {
  CYDDisplay* self = static_cast<CYDDisplay*>(ctx);
  for (int r = 0; r < rows; r++) self->_dumpSink(y0 + r, px + (long)r * w, self->_dumpCtx);
}

bool CYDDisplay::dumpPage(StripRenderer::DrawFn draw, void* drawCtx, RowSink sink, void* sinkCtx) {
  if (_strip == nullptr || sink == nullptr || !lockPanel()) return false;
  StripRenderer once(_strip, cyd::kScreenW, cyd::kScreenH, cyd::kBandRows);   // no hashes kept
  _dumpSink = sink;
  _dumpCtx = sinkCtx;
  once.render(draw, drawCtx, dumpBand, this, true);
  _dumpSink = nullptr;
  if (_pages != nullptr) _pages->invalidate();   // the strip held other pixels meanwhile: harmless, but resend
  unlockPanel();
  return true;
}

bool CYDDisplay::dumpCompanion(RowSink sink, void* sinkCtx) {
  if (_strip == nullptr || sink == nullptr || !lockPanel()) return false;
  StripRenderer once(_strip, cyd::kScreenW, cyd::kScreenH, cyd::kBandRows);
  _dumpSink = sink;
  _dumpCtx = sinkCtx;
  once.render(drawHeadBand, this, dumpBand, this, true);
  _dumpSink = nullptr;
  unlockPanel();
  return true;
}

void CYDDisplay::showColourBars() {
  if (!lockPanel()) return;
  static const uint32_t kBars[8] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0x00FFFF, 0xFF00FF, 0xFFFF00, 0x000000};
  static const char* const kNames[8] = {"R", "G", "B", "W", "C", "M", "Y", "K"};
  const int w = cyd::kScreenW / 8;
  for (int i = 0; i < 8; i++) {
    disp.fillRect(i * w, 0, w, cyd::kScreenH, lgfx::color888(kBars[i] >> 16, (kBars[i] >> 8) & 0xFF, kBars[i] & 0xFF));
    disp.setTextColor(i == 3 || i == 6 || i == 4 ? TFT_BLACK : TFT_WHITE);
    disp.drawString(kNames[i], i * w + w / 2 - 3, cyd::kScreenH / 2);
  }
  if (_pages != nullptr) _pages->invalidate();
  if (_head != nullptr) _head->invalidate();
  unlockPanel();
}

#endif
