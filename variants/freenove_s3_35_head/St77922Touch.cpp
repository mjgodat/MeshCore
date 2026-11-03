// ST77922 integrated touch (I2C 0x55) for the Freenove FNK0104N.
// Register layout: see St77922Touch.h. Written for MeshCore (MIT); no
// Freenove vendor, SquatchWatch-CYD or WiChat code is used.

#include "St77922Touch.h"

#include <Arduino.h>

namespace {
constexpr uint16_t kRegFirmware = 0x0000;
constexpr uint16_t kRegResolution = 0x0005;   // max_x hi/lo, max_y hi/lo, max points
constexpr uint16_t kRegInfo = 0x0010;
constexpr uint16_t kRegPoint0 = 0x0014;
constexpr int kRecordBytes = 7;
constexpr int kProbeLogAfter = 10;           // attempts (~5 s) before saying so
}  // namespace

bool St77922Touch::readReg(uint16_t reg, uint8_t* buf, size_t len) {
  _wire.beginTransmission(kAddress);
  _wire.write((uint8_t)(reg >> 8));
  _wire.write((uint8_t)reg);
  if (_wire.endTransmission(false) != 0) return false;
  const size_t got = _wire.requestFrom((uint16_t)kAddress, (uint8_t)len, true);
  if (got != len) {
    while (_wire.available()) _wire.read();
    return false;
  }
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)_wire.read();
  return true;
}

bool St77922Touch::begin() {
  pinMode(kPinInt, INPUT);
  pinMode(kPinReset, OUTPUT);
  digitalWrite(kPinReset, LOW);
  delay(10);
  digitalWrite(kPinReset, HIGH);
  _resetMs = millis();
  _wire.setClock(400000);
  // The controller is probed from read() once it has warmed up
  // (freenove::kTouchWarmupMs), so boot does not wait for it.
  _started = true;
  return true;
}

bool St77922Touch::probe(uint32_t now) {
  if (!_probeRetry.due(now)) return false;
  _probeRetry.attempted(now);
  uint8_t fw = 0;
  if (!readReg(kRegFirmware, &fw, 1)) {
    if (++_probeFailures == kProbeLogAfter) {
      Serial.println("st77922 touch: no ACK at 0x55 yet, still retrying");
    }
    return false;
  }
  _fw = fw;
  _ready = true;
  Serial.printf("st77922 touch: fw=0x%02X\n", _fw);
  return true;
}

void St77922Touch::readResolution(uint32_t now) {
  if (!_resRetry.due(now)) return;
  _resRetry.attempted(now);
  uint8_t info[5] = {0};
  if (!readReg(kRegResolution, info, sizeof(info))) return;
  const int rx = (info[0] << 8) | info[1];
  const int ry = (info[2] << 8) | info[3];
  // Only trust a plausible report; otherwise keep retrying.
  if (rx < 100 || rx > 4096 || ry < 100 || ry > 4096) return;
  if (info[4] < 1 || info[4] > kMaxPoints) return;
  _resX = rx;
  _resY = ry;
  _points = info[4];
  _haveResolution = true;
  Serial.printf("st77922 touch: res=%dx%d points=%d\n", _resX, _resY,
                _points);
}

bool St77922Touch::pollPoint(int& x, int& y) {
  uint8_t info = 0;
  if (!readReg(kRegInfo, &info, 1)) return false;
  if ((info & 0x08) == 0) return false;   // no new report
  uint8_t buf[kRecordBytes * kMaxPoints];
  if (!readReg(kRegPoint0, buf, (size_t)kRecordBytes * _points)) return false;
  for (int i = 0; i < _points; i++) {
    const uint8_t* r = &buf[i * kRecordBytes];
    if ((r[0] & 0x80) == 0) continue;
    _rawX = ((r[0] & 0x3F) << 8) | r[1];
    _rawY = ((r[2] & 0x3F) << 8) | r[3];
    return freenove::touchToLandscape(_rawX, _rawY, _resX, _resY,
                                      FREENOVE_LANDSCAPE_FLIP != 0,
                                      FREENOVE_TOUCH_MIRROR, x, y);
  }
  return false;
}

bool St77922Touch::read(int& x, int& y) {
  if (!_started) return false;
  const uint32_t now = millis();
  if (!freenove::touchWarmedUp(now, _resetMs)) return false;
  if (!_ready && !probe(now)) return false;
  if (!_haveResolution) readResolution(now);
  int px = -1, py = -1;
  const bool fresh = pollPoint(px, py);
  // A read error or a poll without a new report both count as "no new
  // report": the hold-over keeps a held finger down between reports.
  return _hold.update(fresh, px, py, now, x, y);
}
