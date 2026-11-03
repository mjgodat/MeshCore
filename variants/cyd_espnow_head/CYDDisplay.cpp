#include <Arduino.h>

#include "CYDDisplay.h"

#if CYD_VISUAL_HEAD

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <helpers/ui/DisplayTouchCoordinates.h>

#include "FrameBuffer332.h"   // buildLut565
#include "StubPages.h"        // drawBackdrop

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
  // A day/night switch fills the panel (LovyanGFX): only then is the panel
  // lock needed (waiting out a page frame), and every band is resent after.
  // Ordinary frames draw into the sprite only and never wait for the pages.
  const bool night = _nightFrame;
  const bool switching = mesh::ui::displayNightActive() != _nightFrame;
  const bool locked = switching && _panelLock != nullptr &&
                      xSemaphoreTakeRecursive(_panelLock, portMAX_DELAY) == pdTRUE;
  LGFXDisplay::startFrame(bkg);
  if (night != _nightFrame && _pages != nullptr) {
    _pages->invalidate();
    _head->invalidate();
  }
  if (locked) unlockPanel();
  _text.clear();
  _skin.clear();
  _skinOn = _strip != nullptr && !mesh::ui::displayNightActive();
  // The sprite was just cleared to the background: its raw value is the
  // key for "nothing drawn here" when the frame is composed.
  const uint8_t* raw = (const uint8_t*)buffer.getBuffer();
  if (raw != nullptr) _skinKey = raw[0];
}

void CYDDisplay::endFrame() {
  if (_strip == nullptr) {
    LGFXDisplay::endFrame();
    return;
  }
  const uint32_t hash = frameHash() ^ (_text.hash() * 31u) ^ (_skinOn ? _skin.hash() * 17u : 0x5A5Au);
  const bool changed = !_shownValid || hash != _shownHash;
  _shownHash = hash;
  _shownValid = true;
  _hasLastFrame = true;
  if (changed || _companionRepaint.load()) composeCompanion(false);
}

freenove_ui::CompanionPalette CYDDisplay::skinPalette() {
  return freenove_ui::CompanionPalette{UIColor::window_bkg, UIColor::title_bkg, UIColor::popup_bkg,
                                       UIColor::corp_blue, UIColor::title_txt, UIColor::primary_txt,
                                       UIColor::secondary_txt, UIColor::warning_txt, UIColor::popup_txt};
}

void CYDDisplay::setTextSize(int sz) {
  LGFXDisplay::setTextSize(sz);
  _aaSize = sz < 1 ? 1 : sz;
}

void CYDDisplay::setCursor(int x, int y) {
  LGFXDisplay::setCursor(x, y);
  _cursorX = x;
  _cursorY = y;
}

void CYDDisplay::setColor(ColorVal c) {
  _semantic = c;
  LGFXDisplay::setColor(c);
}

// Recorded, not drawn into the sprite: drawn in D-DIN over the composed frame.
void CYDDisplay::print(const char* str) {
  if (_strip == nullptr) {
    LGFXDisplay::print(str);
    return;
  }
  char ascii[256];
  translateUTF8ToBlocks(ascii, str != nullptr ? str : "", sizeof(ascii));
  if (_skinOn) {
    const freenove_ui::CompanionPalette pal = skinPalette();
    _cursorX = _text.add(_cursorX, _cursorY, _aaSize, freenove_ui::skinTextColour(_semantic, pal), ascii,
                         _skin.textPad(_cursorX, _cursorY, pal));
    return;
  }
  _cursorX = _text.add(_cursorX, _cursorY, _aaSize, (uint16_t)_color, ascii);
}

uint16_t CYDDisplay::getTextWidth(const char* str) {
  if (_strip == nullptr) return LGFXDisplay::getTextWidth(str);
  char ascii[256];
  translateUTF8ToBlocks(ascii, str != nullptr ? str : "", sizeof(ascii));
  return (uint16_t)freenove_ui::CompanionText::logicalWidth(ascii, _aaSize);
}

// A fill over earlier text (a popup, a highlight) hides that text.
void CYDDisplay::fillRect(int x, int y, int w, int h) {
  if (_strip != nullptr && _skinOn) {
    // An HMI shape; in the sprite it clears to the background, so icons
    // drawn under it earlier stay hidden as before.
    _skin.fill(x, y, w, h, _semantic);
    const int saved = _color;
    _color = renderColor(UIColor::window_bkg);
    LGFXDisplay::fillRect(x, y, w, h);
    _color = saved;
    _text.occlude(x, y, w, h);
    return;
  }
  LGFXDisplay::fillRect(x, y, w, h);
  _text.occlude(x, y, w, h);
}

void CYDDisplay::drawRect(int x, int y, int w, int h) {
  if (_strip != nullptr && _skinOn) {
    _skin.rect(x, y, w, h, _semantic);
    return;
  }
  LGFXDisplay::drawRect(x, y, w, h);
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

// The HEAD frame, one band: (skin) HMI backdrop and shapes, the sprite's
// own pixels (all of them without the skin), the D-DIN text, the tab bar.
void CYDDisplay::drawHeadBand(FrameBuffer565& fb, int band, void* ctx) {
  (void)band;
  CYDDisplay* self = static_cast<CYDDisplay*>(ctx);
  const uint8_t* src = static_cast<const uint8_t*>(self->buffer.getBuffer());
  const int sw = self->buffer.width(), sh = self->buffer.height();
  const int zoom = 2;
  const bool skin = self->_skinOn;
  if (skin) {
    freenove_ui::drawBackdrop(fb, 0, 0, fb.width(), cyd::kCompanionH, nullptr, 0);
    self->_skin.draw(fb, cyd::kCompanionH, nullptr, skinPalette());
  }
  int y0 = fb.clipY0(), y1 = fb.clipY1() + 1;
  if (y1 > cyd::kCompanionH) y1 = cyd::kCompanionH;
  uint16_t* px = fb.rawPixels();
  const uint8_t key = self->_skinKey;
  for (int y = y0; y < y1; y++) {
    uint16_t* row = px + (long)y * fb.width();
    const int sy = y / zoom;
    if (src == nullptr || sy >= sh) {
      if (!skin) for (int x = 0; x < fb.width(); x++) row[x] = 0;
      continue;
    }
    const uint8_t* s = src + (long)sy * sw;
    for (int x = 0; x < fb.width(); x++) {
      const int sx = x / zoom;
      if (sx >= sw) continue;
      const uint8_t v = s[sx];
      if (!skin) row[x] = self->_lut565[v];
      else if (v != key) row[x] = self->_lut565[v];   // icons, QR: over the skin
    }
  }
  fb.resetClip();
  self->_text.draw(fb, cyd::kCompanionH);
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

bool CYDDisplay::renderPage(StripRenderer::DrawFn draw, void* ctx, int maxY) {
  if (_pages == nullptr || !lockPanel()) return false;
  bool drawn = false;
  if (panelOwner() == PanelOwner::Pages) {
    _pushUs = 0;
    _lastPushed = _pages->render(draw, ctx, pushBand, this, false, true, maxY);
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
  if (_pages == nullptr || sink == nullptr || !lockPanel()) return false;
  _dumpSink = sink;
  _dumpCtx = sinkCtx;
  _pages->render(draw, drawCtx, dumpBand, this, true, false);   // hashes untouched (the glass is)
  _dumpSink = nullptr;
  unlockPanel();
  return true;
}

bool CYDDisplay::dumpCompanion(RowSink sink, void* sinkCtx) {
  if (_head == nullptr || sink == nullptr || !lockPanel()) return false;
  _dumpSink = sink;
  _dumpCtx = sinkCtx;
  _head->render(drawHeadBand, this, dumpBand, this, true, false);
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
