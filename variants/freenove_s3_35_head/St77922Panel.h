#pragma once

// ST77922 QSPI panel driver for the Freenove FNK0104N (ESP32-S3, 320x480
// portrait panel used as a 480x320 landscape screen).
//
// Arduino-ESP32 2.0.17 ships IDF 4.4, whose esp_lcd SPI panel IO has no QSPI
// ("quad_mode") support, so this drives the bus with IDF spi_master directly
// using the ST77922 QSPI framing documented by Espressif's esp_lcd_st77922
// component (see St77922Panel.cpp for attribution).
//
// Pixels go out as RGB565 big-endian. Landscape rows are transposed into
// panel columns in internal DMA bounce buffers (FreenoveGeometry.h), so the
// caller's buffer may live anywhere (PSRAM included) and is free to reuse as
// soon as pushRows() returns; the last transfers may still be in flight
// (waitIdle() blocks until the bus is quiet).

#include <stddef.h>
#include <stdint.h>
#include <driver/spi_master.h>

#include "FreenoveGeometry.h"

// MADCTL (36h) written after the init table. 0x00 = the orientation the init
// table leaves; bit 6 / bit 7 mirror columns / rows (esp_lcd_st77922
// panel_st77922_mirror) if the glass turns out mirrored on hardware.
// Display inversion after init: 1 = INVON, 0 = INVOFF (the xiaozhi/esp_lcd
// setting). The FNK0104N glass shows inverted colours (navy as tan/white)
// with INVOFF; confirmed on hardware 2026-10-31. One panel command, applied
// the same to RGB332 and RGB565 frames (neither pixel path inverts).
#ifndef FREENOVE_PANEL_INVERT
  #define FREENOVE_PANEL_INVERT 1
#endif
#ifndef FREENOVE_MADCTL
  #define FREENOVE_MADCTL 0x00
#endif

class St77922Panel {
public:
  // Freenove FNK0104N wiring (owner's board notes; matches the LCDwiki
  // ES3C35P board config in xiaozhi-esp32).
  static constexpr int kPinCs = 10;
  static constexpr int kPinSclk = 12;
  static constexpr int kPinD0 = 11;
  static constexpr int kPinD1 = 13;
  static constexpr int kPinD2 = 14;
  static constexpr int kPinD3 = 9;
  static constexpr int kPinBacklight = 41;   // active high
  static constexpr int kClockHz = 80 * 1000 * 1000;
  static constexpr uint8_t kBacklightChannel = 7;

  // Pixels per DMA chunk (one bounce buffer); 9,600 bytes of internal RAM each.
  static constexpr int kChunkPixels = 4800;
  // Bound on every SPI queue/result wait; one chunk takes well under 1 ms.
  static constexpr int kTimeoutMs = 200;

  bool begin();
  // Backlight off; free the bounce buffers, SPI device and bus (begin() can
  // run again afterwards).
  void end();
  bool ready() const { return _ready; }

  // Push `rows` landscape rows starting at landscape row y0. px holds
  // rows * 480 RGB565 pixels in native byte order, row y0 first. y0 and rows
  // must be multiples of freenove::kRowAlign (use freenove::alignRows());
  // misaligned or off-screen requests are ignored.
  void pushRows(const uint16_t* px, int y0, int rows);
  // Same, straight from an RGB332 framebuffer: fb332 is the whole 480x320
  // landscape frame (one byte per pixel, PSRAM is fine) and lut565 the
  // 256-entry wire-order (byteswapped) RGB565 table (buildLut565(.., true)).
  // The LUT expansion happens while transposing into the bounce buffers, so
  // no intermediate RGB565 frame or line buffer exists. Same alignment rules.
  // x0..x1 (landscape columns, inclusive) trims the window: only those panel
  // rows are sent (the panel row window has no alignment rule).
  void pushRows332(const uint8_t* fb332, const uint16_t* lut565, int y0,
                   int rows, int x0 = 0, int x1 = freenove::kLandscapeW - 1);
  // Same, from a whole 480x320 RGB565 framebuffer (native byte order, PSRAM
  // is fine): bytes are swapped to wire order while transposing.
  void pushRows565(const uint16_t* fb565, int y0, int rows, int x0 = 0,
                   int x1 = freenove::kLandscapeW - 1);

  void setBacklight(uint8_t level);
  // Display off + sleep in (true) / sleep out + display on (false).
  void sleep(bool on);
  void waitIdle();
  // SPI failures/timeouts since boot (each one recovered).
  uint32_t errors() const { return _errors; }

private:
  struct Chunk {
    uint16_t* buf = nullptr;
    int queued = 0;           // this chunk's transactions not yet collected
    spi_transaction_t caset;
    spi_transaction_t raset;
    spi_transaction_t color;
  };

  bool writeCommand(uint8_t cmd, const uint8_t* data, size_t len);
  bool sendInitTable();
  bool queue(Chunk& c, spi_transaction_t& t);
  bool collect(int n);
  void recover();
  void release();
  void noteError(const char* what);
  void setWindowTrans(spi_transaction_t& t, uint8_t cmd, int a, int b);
  // One band: px565 (rows x 480 native RGB565, band first), fb332 + lut,
  // or frame565 (the whole RGB565 frame).
  void pushBand(const uint16_t* px565, const uint8_t* fb332,
                const uint16_t* lut565, const uint16_t* frame565, int y0,
                int rows, int x0, int x1);

  spi_device_handle_t _dev = nullptr;
  Chunk _chunks[2];
  int _nextChunk = 0;
  int _pending = 0;         // transactions queued and not yet collected
  uint32_t _errors = 0;
  bool _busReady = false;
  bool _ready = false;
  bool _backlightReady = false;
};
