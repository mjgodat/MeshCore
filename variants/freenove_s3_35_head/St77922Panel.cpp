// ST77922 QSPI panel driver for the Freenove FNK0104N.
//
// Sources and licences
// --------------------
// 1. QSPI framing, command flow and register semantics are adapted from
//    Espressif's esp_lcd_st77922 component (esp-iot-solution,
//    components/display/lcd/esp_lcd_st77922: esp_lcd_st77922_general.c
//    tx_param()/tx_color()/panel_st77922_reset()/panel_st77922_init()/
//    panel_st77922_mirror(), priv_include/st77922_interface.h opcodes):
//
//      SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
//      SPDX-License-Identifier: Apache-2.0
//      Licensed under the Apache License, Version 2.0 (the "License"); you
//      may not use this file except in compliance with the License. You may
//      obtain a copy of the License at
//      http://www.apache.org/licenses/LICENSE-2.0
//      Unless required by applicable law or agreed to in writing, software
//      distributed under the License is distributed on an "AS IS" BASIS,
//      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
//      implied. See the License for the specific language governing
//      permissions and limitations under the License.
//
//    Modified: rewritten for IDF 4.4 spi_master (no esp_lcd QSPI panel IO),
//    C++ class, landscape transpose with DMA bounce buffers, no reset GPIO.
//
// 2. The 320x480 panel initialisation table (kInitTable) is the one used for
//    the same panel/pinout (LCDwiki ES3C35P: ST77922 QSPI on CS10/SCLK12/
//    D0 11/D1 13/D2 14/D3 9, BL41) in xiaozhi-esp32,
//    main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc, lcd_init_cmds[]:
//
//      Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.
//      Copyright (c) 2025 Project Contributors
//      MIT License. Permission is hereby granted, free of charge, to any
//      person obtaining a copy of this software and associated documentation
//      files (the "Software"), to deal in the Software without restriction,
//      including without limitation the rights to use, copy, modify, merge,
//      publish, distribute, sublicense, and/or sell copies of the Software,
//      and to permit persons to whom the Software is furnished to do so,
//      subject to the following conditions: The above copyright notice and
//      this permission notice shall be included in all copies or substantial
//      portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT
//      WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
//      THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
//      NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
//      LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
//      OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
//      WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
//    Modified: re-encoded as a byte table. Espressif's built-in default
//    table targets a 532x300 module (CASET 0..0x213) and is not used.
//
// No Freenove vendor code, SquatchWatch-CYD or WiChat code is used.

#include "St77922Panel.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {

// st77922_interface.h
constexpr uint8_t kOpWriteCmd = 0x02;     // LCD_OPCODE_WRITE_CMD
constexpr uint8_t kOpWriteColor = 0x32;   // LCD_OPCODE_WRITE_COLOR

constexpr uint8_t kCmdSwReset = 0x01;
constexpr uint8_t kCmdSleepIn = 0x10;
constexpr uint8_t kCmdSleepOut = 0x11;
constexpr uint8_t kCmdInvOff = 0x20;
constexpr uint8_t kCmdInvOn = 0x21;
constexpr uint8_t kCmdDispOff = 0x28;
constexpr uint8_t kCmdDispOn = 0x29;
constexpr uint8_t kCmdCaset = 0x2A;
constexpr uint8_t kCmdRaset = 0x2B;
constexpr uint8_t kCmdRamwr = 0x2C;
constexpr uint8_t kCmdMadctl = 0x36;
constexpr uint8_t kCmdColmod = 0x3A;

// Encoding: cmd, data length, delay ms, data bytes...; terminated by 0xFF 0xFF.
// (0xFF is not used as a command in the table.)
const uint8_t kInitTable[] = {
  0xF1, 1, 0, 0x00,
  0x60, 3, 0, 0x00, 0x00, 0x00,
  0x65, 1, 0, 0x80,
  0x79, 1, 0, 0x06,
  0x7B, 3, 0, 0x00, 0x08, 0x08,
  0x80, 11, 0, 0x55, 0x62, 0x2F, 0x17, 0xF0, 0x52, 0x70, 0xD2, 0x52, 0x62, 0xEA,
  0x81, 4, 0, 0x26, 0x52, 0x72, 0x27,
  0x84, 2, 0, 0x92, 0x25,
  0x87, 6, 0, 0x10, 0x10, 0x58, 0x00, 0x02, 0x3A,
  0x88, 15, 0, 0x00, 0x00, 0x2C, 0x10, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01,
               0x01, 0x01, 0x00, 0x06,
  0x89, 3, 0, 0x00, 0x00, 0x00,
  0x8A, 11, 0, 0x13, 0x00, 0x2C, 0x00, 0x00, 0x2C, 0x10, 0x10, 0x00, 0x3E, 0x19,
  0x8B, 9, 0, 0x15, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x97, 0x8E,
  0x8C, 13, 0, 0x1D, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x50, 0x0F, 0x01, 0xC5,
               0x12, 0x09,
  0x8D, 1, 0, 0x0C,
  0x8E, 6, 0, 0x33, 0x01, 0x0C, 0x13, 0x01, 0x01,
  0xB3, 2, 0, 0x00, 0x30,
  0xF1, 1, 0, 0x00,
  0x71, 1, 0, 0xD0,
  0x66, 2, 0, 0x02, 0x3F,
  0xBE, 3, 0, 0x26, 0x00, 0x9D,
  0x70, 12, 0, 0x01, 0xA0, 0x11, 0x40, 0xE0, 0x00, 0x11, 0x69, 0x11, 0x00, 0x00,
               0x1A,
  0x90, 9, 0, 0x04, 0x04, 0x55, 0x74, 0x00, 0x40, 0x43, 0x27, 0x27,
  0x91, 9, 0, 0x04, 0x04, 0x55, 0x75, 0x00, 0x40, 0x42, 0x27, 0x27,
  0x92, 10, 0, 0x04, 0x44, 0x55, 0xC0, 0x06, 0x00, 0x07, 0x05, 0x90, 0x27,
  0x93, 10, 0, 0x04, 0x43, 0x11, 0x00, 0x00, 0x00, 0x00, 0x05, 0x90, 0x27,
  0x94, 6, 0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x95, 5, 0, 0x96, 0x16, 0x00, 0x00, 0xFF,
  0x96, 12, 0, 0x44, 0x53, 0x03, 0x12, 0x23, 0x24, 0x06, 0x05, 0x94, 0x27, 0x00,
               0x44,
  0x97, 12, 0, 0x44, 0x53, 0x47, 0x56, 0x20, 0x20, 0x02, 0x01, 0x94, 0x27, 0x00,
               0x44,
  0xBA, 5, 0, 0x55, 0x94, 0x2D, 0x94, 0x27,
  0x9A, 7, 0, 0x40, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
  0x9B, 7, 0, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
  0x9C, 13, 0, 0x5C, 0x12, 0x00, 0x00, 0x10, 0x12, 0x00, 0x00, 0x10, 0x02, 0x00,
               0x00, 0x00,
  0x9D, 8, 0, 0x8A, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01,
  0x9E, 7, 0, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01,
  0xB4, 12, 0, 0x1D, 0x1C, 0x1E, 0x0B, 0x14, 0x02, 0x13, 0x09, 0x1E, 0x00, 0x1E,
               0x10,
  0xB5, 12, 0, 0x1D, 0x1C, 0x1E, 0x0A, 0x15, 0x03, 0x11, 0x08, 0x1E, 0x01, 0x1E,
               0x12,
  0xB6, 7, 0, 0x77, 0x77, 0x00, 0x0A, 0xFF, 0x0A, 0xFF,
  0x86, 14, 0, 0xCD, 0x04, 0xB1, 0x02, 0x58, 0x12, 0x58, 0x0C, 0x13, 0x01, 0xA5,
               0x00, 0xA5, 0xA5,
  0xB7, 16, 0, 0x07, 0x0A, 0x0E, 0x06, 0x05, 0x03, 0x2B, 0x03, 0x03, 0x42, 0x07,
               0x10, 0x10, 0x2E, 0x3F, 0x0D,
  0xB8, 16, 0, 0x07, 0x0A, 0x0D, 0x05, 0x05, 0x02, 0x2B, 0x02, 0x03, 0x42, 0x06,
               0x10, 0x0F, 0x2E, 0x3F, 0x0D,
  0xB9, 2, 0, 0x23, 0x23,
  0xBF, 6, 0, 0x10, 0x14, 0x14, 0x0B, 0x0B, 0x0B,
  0xF2, 1, 0, 0x00,
  0x73, 5, 0, 0x04, 0xDA, 0x12, 0x54, 0x47,
  0x77, 5, 0, 0x6B, 0x5B, 0xFD, 0xC3, 0xC5,
  0x7A, 2, 0, 0x15, 0x27,
  0x7B, 2, 0, 0x04, 0x57,
  0x7E, 2, 0, 0x01, 0x0E,
  0xBF, 1, 0, 0x36,
  0xE3, 2, 0, 0x40, 0x40,
  0xF0, 1, 0, 0x00,
  0xD0, 1, 0, 0x00,
  0x2A, 4, 0, 0x00, 0x00, 0x01, 0x3F,      // columns 0..319
  0x2B, 4, 0, 0x00, 0x00, 0x01, 0xDF,      // rows 0..479
  0x21, 0, 0,
  0x11, 0, 120,
  0x29, 0, 0,
  0x2C, 0, 0,
  0x3A, 1, 0, 0x01,
  0x36, 1, 0, 0x00,
  0x35, 1, 20, 0x01,
  0xFF, 0xFF,
};

}  // namespace

bool St77922Panel::writeCommand(uint8_t cmd, const uint8_t* data, size_t len) {
  if (_dev == nullptr) return false;
  // Polling transactions must not overlap queued ones.
  waitIdle();
  // esp_lcd_st77922 tx_param(): 8-bit opcode 0x02, 24-bit address 00 cmd 00,
  // parameters on one data line.
  uint8_t local[16];
  spi_transaction_t t;
  memset(&t, 0, sizeof(t));
  t.cmd = kOpWriteCmd;
  t.addr = (uint32_t)cmd << 8;
  if (len > 0 && len <= 4) {
    t.flags = SPI_TRANS_USE_TXDATA;
    memcpy(t.tx_data, data, len);
  } else if (len > 0) {
    if (len > sizeof(local)) len = sizeof(local);
    memcpy(local, data, len);  // stack: internal, DMA-capable
    t.tx_buffer = local;
  }
  t.length = len * 8;
  if (spi_device_polling_transmit(_dev, &t) != ESP_OK) {
    noteError("command");
    return false;
  }
  return true;
}

bool St77922Panel::sendInitTable() {
  size_t i = 0;
  while (i + 1 < sizeof(kInitTable)) {
    const uint8_t cmd = kInitTable[i];
    const uint8_t len = kInitTable[i + 1];
    if (cmd == 0xFF && len == 0xFF) break;
    const uint8_t wait = kInitTable[i + 2];
    if (!writeCommand(cmd, &kInitTable[i + 3], len)) return false;
    if (wait) delay(wait);
    i += 3 + len;
  }
  return true;
}

void St77922Panel::noteError(const char* what) {
  _errors++;
  // Rate-limited log: the first error, then every 64th.
  if ((_errors & 63) == 1) {
    Serial.printf("st77922: SPI %s failed (errors=%u)\n", what,
                  (unsigned)_errors);
  }
}

void St77922Panel::release() {
  for (Chunk& c : _chunks) {
    if (c.buf != nullptr) heap_caps_free(c.buf);
    c.buf = nullptr;
    c.queued = 0;
  }
  if (_dev != nullptr) {
    spi_bus_remove_device(_dev);
    _dev = nullptr;
  }
  if (_busReady) {
    spi_bus_free(SPI2_HOST);
    _busReady = false;
  }
  _pending = 0;
  _ready = false;
}

bool St77922Panel::begin() {
  if (_ready) return true;

  // Backlight off until the panel holds a picture.
  if (!_backlightReady) {
    ledcSetup(kBacklightChannel, 20000, 8);
    ledcAttachPin(kPinBacklight, kBacklightChannel);
    _backlightReady = true;
  }
  ledcWrite(kBacklightChannel, 0);

  spi_bus_config_t bus;
  memset(&bus, 0, sizeof(bus));
  bus.data0_io_num = kPinD0;
  bus.data1_io_num = kPinD1;
  bus.data2_io_num = kPinD2;
  bus.data3_io_num = kPinD3;
  bus.sclk_io_num = kPinSclk;
  bus.data4_io_num = -1;
  bus.data5_io_num = -1;
  bus.data6_io_num = -1;
  bus.data7_io_num = -1;
  bus.max_transfer_sz = kChunkPixels * 2;
  bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
  esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    Serial.printf("st77922: spi_bus_initialize failed %d\n", (int)err);
    release();
    return false;
  }
  _busReady = true;

  spi_device_interface_config_t dev;
  memset(&dev, 0, sizeof(dev));
  dev.command_bits = 8;
  dev.address_bits = 24;
  dev.mode = 0;
  dev.clock_speed_hz = kClockHz;
  dev.spics_io_num = kPinCs;
  dev.queue_size = 8;   // two chunks x three transactions in flight
  dev.flags = SPI_DEVICE_HALFDUPLEX;
  err = spi_bus_add_device(SPI2_HOST, &dev, &_dev);
  if (err != ESP_OK) {
    Serial.printf("st77922: spi_bus_add_device failed %d\n", (int)err);
    _dev = nullptr;
    release();
    return false;
  }

  for (Chunk& c : _chunks) {
    c.buf = (uint16_t*)heap_caps_malloc(kChunkPixels * 2,
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    c.queued = 0;
    if (c.buf == nullptr) {
      Serial.println("st77922: no DMA memory for bounce buffers");
      release();
      return false;
    }
  }

  // panel_st77922_reset(): no reset GPIO on this board -> software reset.
  bool ok = writeCommand(kCmdSwReset, nullptr, 0);
  delay(120);
  // panel_st77922_init(): MADCTL and COLMOD first, then the vendor table.
  const uint8_t madctl0 = FREENOVE_MADCTL;
  const uint8_t colmod = 0x55;   // RGB565
  ok = ok && writeCommand(kCmdMadctl, &madctl0, 1);
  ok = ok && writeCommand(kCmdColmod, &colmod, 1);
  ok = ok && sendInitTable();
  // The table ends with MADCTL 00; re-apply the configured orientation
  // (xiaozhi applies esp_lcd_panel_mirror()/invert_color(false) here).
  const uint8_t madctl = FREENOVE_MADCTL;
  ok = ok && writeCommand(kCmdMadctl, &madctl, 1);
  ok = ok && writeCommand(FREENOVE_PANEL_INVERT ? kCmdInvOn : kCmdInvOff, nullptr, 0);
  ok = ok && writeCommand(kCmdDispOn, nullptr, 0);
  if (!ok) {
    Serial.println("st77922: panel init failed");
    release();
    return false;
  }

  _ready = true;
  return true;
}

void St77922Panel::end() {
  if (_dev != nullptr) waitIdle();
  setBacklight(0);
  release();
}

void St77922Panel::setBacklight(uint8_t level) {
  if (!_backlightReady) return;
  ledcWrite(kBacklightChannel, level);
}

void St77922Panel::sleep(bool on) {
  if (!_ready) return;
  if (on) {
    writeCommand(kCmdDispOff, nullptr, 0);
    writeCommand(kCmdSleepIn, nullptr, 0);
    delay(120);
  } else {
    writeCommand(kCmdSleepOut, nullptr, 0);
    delay(120);
    writeCommand(kCmdDispOn, nullptr, 0);
  }
}

void St77922Panel::recover() {
  // A result did not arrive in time. Collect whatever has completed without
  // waiting, then forget the rest so no later wait blocks on it.
  spi_transaction_t* done = nullptr;
  while (_pending > 0
         && spi_device_get_trans_result(_dev, &done, 0) == ESP_OK) {
    _pending--;
  }
  _pending = 0;
  for (Chunk& c : _chunks) c.queued = 0;
}

bool St77922Panel::collect(int n) {
  // Results come back in queue order; every wait is bounded.
  spi_transaction_t* done = nullptr;
  for (int k = 0; k < n && _pending > 0; k++) {
    if (spi_device_get_trans_result(_dev, &done,
                                    pdMS_TO_TICKS(kTimeoutMs)) != ESP_OK) {
      noteError("result timeout");
      recover();
      return false;
    }
    _pending--;
  }
  return true;
}

void St77922Panel::waitIdle() {
  if (_dev == nullptr) return;
  if (_pending > 0) collect(_pending);
  _pending = 0;
  for (Chunk& c : _chunks) c.queued = 0;
}

void St77922Panel::setWindowTrans(spi_transaction_t& t, uint8_t cmd, int a,
                                  int b) {
  memset(&t, 0, sizeof(t));
  t.cmd = kOpWriteCmd;
  t.addr = (uint32_t)cmd << 8;
  t.flags = SPI_TRANS_USE_TXDATA;
  t.tx_data[0] = (uint8_t)(a >> 8);
  t.tx_data[1] = (uint8_t)a;
  t.tx_data[2] = (uint8_t)(b >> 8);
  t.tx_data[3] = (uint8_t)b;
  t.length = 32;
}

bool St77922Panel::queue(Chunk& c, spi_transaction_t& t) {
  if (spi_device_queue_trans(_dev, &t, pdMS_TO_TICKS(kTimeoutMs)) != ESP_OK) {
    noteError("queue");
    return false;
  }
  c.queued++;
  _pending++;
  return true;
}

void St77922Panel::pushRows(const uint16_t* px, int y0, int rows) {
  if (px == nullptr) return;
  pushBand(px, nullptr, nullptr, nullptr, y0, rows, 0,
           freenove::kLandscapeW - 1);
}

void St77922Panel::pushRows332(const uint8_t* fb332, const uint16_t* lut565,
                               int y0, int rows, int x0, int x1) {
  if (fb332 == nullptr || lut565 == nullptr) return;
  pushBand(nullptr, fb332, lut565, nullptr, y0, rows, x0, x1);
}

void St77922Panel::pushRows565(const uint16_t* fb565, int y0, int rows, int x0,
                               int x1) {
  if (fb565 == nullptr) return;
  pushBand(nullptr, nullptr, nullptr, fb565, y0, rows, x0, x1);
}

void St77922Panel::pushBand(const uint16_t* px565, const uint8_t* fb332,
                            const uint16_t* lut565, const uint16_t* frame565,
                            int y0, int rows, int x0, int x1) {
  using namespace freenove;
  if (!_ready) return;
  if (rows <= 0 || y0 < 0 || y0 + rows > kLandscapeH) return;
  if (y0 % kRowAlign != 0 || rows % kRowAlign != 0) return;
  if (x0 < 0) x0 = 0;
  if (x1 > kLandscapeW - 1) x1 = kLandscapeW - 1;
  if (x0 > x1) return;

  const bool flip = FREENOVE_LANDSCAPE_FLIP != 0;
  const ColumnWindow win = bandColumns(y0, rows, flip);
  // Panel rows per chunk; rows <= 320 keeps this >= 15.
  const int perChunk = kChunkPixels / rows;

  // Landscape columns x0..x1 are panel rows x0..x1 (flipped: mirrored).
  const int pyBegin = flip ? (kPanelH - 1 - x1) : x0;
  const int pyEnd = flip ? (kPanelH - x0) : (x1 + 1);   // exclusive
  for (int py = pyBegin; py < pyEnd; py += perChunk) {
    const int npy = (pyEnd - py) < perChunk ? (pyEnd - py) : perChunk;
    Chunk& c = _chunks[_nextChunk];
    // Chunks alternate, so a chunk still holding transactions is the older
    // of the two in flight: its results are next in the queue.
    if (c.queued > 0) {
      if (!collect(c.queued)) return;   // recovered; drop the rest of this push
      c.queued = 0;
    }
    _nextChunk ^= 1;

    // The CPU fills this bounce buffer while the other chunk's DMA runs.
    if (frame565 != nullptr) {
      transposeBand565(frame565 + y0 * kLandscapeW, rows, py, npy, flip, c.buf);
    } else if (fb332 != nullptr) {
      transposeBand332(fb332 + y0 * kLandscapeW, rows, py, npy, flip, lut565,
                       c.buf);
    } else {
      transposeBand(px565, rows, py, npy, flip, true, c.buf);
    }

    setWindowTrans(c.caset, kCmdCaset, win.c0, win.c1);
    setWindowTrans(c.raset, kCmdRaset, py, py + npy - 1);
    memset(&c.color, 0, sizeof(c.color));
    // esp_lcd_st77922 tx_color(): opcode 0x32, address 00 2C 00, pixel data
    // on four lines (1-1-4).
    c.color.cmd = kOpWriteColor;
    c.color.addr = (uint32_t)kCmdRamwr << 8;
    c.color.flags = SPI_TRANS_MODE_QIO;
    c.color.tx_buffer = c.buf;
    c.color.length = (size_t)npy * rows * 16;

    // Never send pixels without their window: stop at the first failure.
    if (!queue(c, c.caset) || !queue(c, c.raset) || !queue(c, c.color)) return;
  }
}
