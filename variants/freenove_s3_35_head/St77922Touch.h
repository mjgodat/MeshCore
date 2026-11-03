#pragma once

// Touch half of the ST77922 TDDI on the Freenove FNK0104N: I2C 0x55 on
// SDA 38 / SCL 39 (shared with the ES8311 codec), reset 48, interrupt 47.
//
// Register layout (16-bit register address, big-endian on the wire), as used
// for the same controller by xiaozhi-esp32 (MIT; main/boards/lcdwiki-es3c35p/
// lcdwiki-es3c35p.cc touchpad_read()/InitializeTouch()) and independently by
// Bokbacken/MyEsp-OnTheRoad main/board_touch.cpp:
//   0x0000        firmware version (probe)
//   0x0005..0x08  max X (hi, lo), max Y (hi, lo)  -- touch resolution
//   0x0009        max touch points
//   0x0010        touch info; bit 3 set = report holds coordinates
//   0x0014        point records, 7 bytes each:
//                 b0 bit7 = valid, x = (b0 & 0x3F) << 8 | b1,
//                 y = (b2 & 0x3F) << 8 | b3
// Coordinates are portrait panel coordinates; read() returns landscape
// pixels via freenove::touchToLandscape().

#include <stdint.h>
#include <Wire.h>

#include "FreenoveGeometry.h"

#ifndef FREENOVE_TOUCH_MIRROR
  // Bit 0 mirrors X, bit 1 mirrors Y (panel space), for hardware fix-ups.
  #define FREENOVE_TOUCH_MIRROR 0
#endif

class St77922Touch {
public:
  static constexpr uint8_t kAddress = 0x55;
  static constexpr int kPinSda = 38;
  static constexpr int kPinScl = 39;
  static constexpr int kPinReset = 48;
  static constexpr int kPinInt = 47;
  static constexpr int kMaxPoints = 10;

  explicit St77922Touch(TwoWire& wire = Wire) : _wire(wire) {}

  // Expects the bus already started on kPinSda/kPinScl (ESP32Board::begin()
  // with PIN_BOARD_SDA/SCL). Resets the controller; it is probed from read()
  // after freenove::kTouchWarmupMs, retrying every kTouchRetryMs.
  bool begin();
  // Controller answered the probe.
  bool ready() const { return _ready; }

  // Landscape pixels while a finger is down (held over short gaps between
  // reports, freenove::TouchHold); false when not touched.
  bool read(int& x, int& y);
  // Last raw (portrait) reading, for bring-up logs.
  int rawX() const { return _rawX; }
  int rawY() const { return _rawY; }
  int resolutionX() const { return _resX; }
  int resolutionY() const { return _resY; }
  uint8_t firmware() const { return _fw; }

private:
  bool readReg(uint16_t reg, uint8_t* buf, size_t len);
  bool probe(uint32_t now);
  void readResolution(uint32_t now);
  bool pollPoint(int& x, int& y);

  TwoWire& _wire;
  bool _started = false;
  bool _ready = false;
  bool _haveResolution = false;
  uint32_t _resetMs = 0;
  int _probeFailures = 0;
  freenove::TouchRetry _probeRetry;
  freenove::TouchRetry _resRetry;
  freenove::TouchHold _hold;
  uint8_t _fw = 0;
  int _resX = 0;
  int _resY = 0;
  int _points = 1;
  int _rawX = -1;
  int _rawY = -1;
};
