#pragma once

// Repeater head (UI_REPEATER_HEAD): copies everything the companion text
// terminal prints into a small ring buffer, while still writing it to the
// real terminal stream. The on-screen Admin console renders the tail, so the
// screen shows exactly what a USB terminal user would see.

#include <Arduino.h>

class HeadTerminalLog : public Stream {
 public:
  static constexpr size_t kCapacity = 1536;

  Stream& wrap(Stream& out) {
    _out = &out;
    return *this;
  }

  size_t write(uint8_t c) override {
    push(c);
    return _out ? _out->write(c) : 1;
  }
  size_t write(const uint8_t* data, size_t len) override {
    for (size_t i = 0; i < len; i++) push(data[i]);
    return _out ? _out->write(data, len) : len;
  }
  int availableForWrite() override { return _out ? _out->availableForWrite() : 256; }
  int available() override { return _out ? _out->available() : 0; }
  int read() override { return _out ? _out->read() : -1; }
  int peek() override { return _out ? _out->peek() : -1; }
  void flush() override { if (_out) _out->flush(); }

  // Bumps on every write, so a screen can redraw only when text arrived.
  uint32_t generation() const { return _generation; }

  // Copy the newest bytes (at most dst_size - 1), oldest first, NUL-terminated.
  size_t copyTail(char* dst, size_t dst_size) const {
    if (dst_size == 0) return 0;
    const size_t n = _len < dst_size - 1 ? _len : dst_size - 1;
    const size_t start = (_head + kCapacity - n) % kCapacity;
    for (size_t i = 0; i < n; i++) dst[i] = _buf[(start + i) % kCapacity];
    dst[n] = 0;
    return n;
  }

  void clear() {
    _len = 0;
    _generation++;
  }

 private:
  void push(uint8_t c) {
    if (c == '\r' || c == 0) return;
    _buf[_head] = static_cast<char>(c);
    _head = (_head + 1) % kCapacity;
    if (_len < kCapacity) _len++;
    _generation++;
  }

  Stream* _out = nullptr;
  char _buf[kCapacity] = {};
  size_t _head = 0;
  size_t _len = 0;
  uint32_t _generation = 0;
};

HeadTerminalLog& headTerminalLog();
