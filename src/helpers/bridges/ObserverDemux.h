#pragma once

// Observer frames on the ESP-NOW receive path (visual head).
// Pure, header-only, C++11, no Arduino.
//
// ESPNOWRadio's receive callback calls demuxObserverFrame() first: an
// observer frame (ObserverFrame.h: 4-byte magic, version, kind and exact
// length) goes to the registered handler and never reaches the MeshCore
// receive queue, raw-fragment reassembly or WiChat. Everything else -- and
// every frame while no handler is registered -- continues on the old path
// untouched, so builds without a visual head behave exactly as before.
//
// FrameRing is the bounded queue between the handler (WiFi task) and the UI
// task: whole frames, drop-oldest when full. It is not synchronised; producer
// and consumer wrap push()/pop() in the same critical section.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ObserverFrame.h"

namespace mesh { namespace observer {

// Called from the WiFi task: copy the frame and return (no blocking, no
// logging). `frame` is only valid during the call.
typedef void (*FrameHandler)(const uint8_t* frame, size_t len, void* ctx);

// True when `frame` is an observer frame and was handed to `handler`; false
// (frame untouched, handler not called) otherwise or when handler is null.
inline bool demuxObserverFrame(const uint8_t* frame, size_t len, FrameHandler handler, void* ctx) {
  if (handler == nullptr || !isObserverFrame(frame, len)) return false;
  handler(frame, len, ctx);
  return true;
}

template <size_t Depth>
class FrameRing {
 public:
  static const size_t MAX_FRAME = 250;   // one ESP-NOW frame

  FrameRing() : _head(0), _count(0), _pushed(0), _dropped(0) { memset(_lens, 0, sizeof(_lens)); }

  // Stores a copy; when full the oldest frame is dropped (counted). False
  // (nothing stored) for a null or empty frame or one over MAX_FRAME.
  bool push(const uint8_t* frame, size_t len) {
    if (frame == nullptr || len == 0 || len > MAX_FRAME) return false;
    if (_count == Depth) { _count--; _dropped++; }   // oldest = slot after the newest
    const size_t slot = _head;
    memcpy(_frames[slot], frame, len);
    _lens[slot] = (uint8_t)len;
    _head = (_head + 1) % Depth;
    _count++;
    _pushed++;
    return true;
  }

  // Oldest frame into out (cap bytes); false when empty. A frame larger
  // than cap is discarded and counted as dropped.
  bool pop(uint8_t* out, size_t cap, size_t& len) {
    len = 0;
    if (_count == 0) return false;
    const size_t slot = (_head + Depth - _count) % Depth;
    _count--;
    if (out == nullptr || _lens[slot] > cap) { _dropped++; return false; }
    len = _lens[slot];
    memcpy(out, _frames[slot], len);
    return true;
  }

  size_t count() const { return _count; }
  uint32_t pushed() const { return _pushed; }
  uint32_t dropped() const { return _dropped; }

 private:
  uint8_t _frames[Depth][MAX_FRAME];
  uint8_t _lens[Depth];
  size_t _head;     // next write slot
  size_t _count;
  uint32_t _pushed;
  uint32_t _dropped;
};

}}  // namespace mesh::observer
