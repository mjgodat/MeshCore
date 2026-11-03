#pragma once

// Pushes frames to the ST77922, sending only what changed since the last
// push: RGB565 frames (FrameBuffer565: the visual head's pages and the
// companion UI) and RGB332 frames (FrameBuffer332: the bring-up demo).
//
// Per frame: the planner (PushPlanner565 / PushPlanner) takes the frame's
// tracked push spans (what the primitives drew since its last push), adds
// what the previous push changed, compares just those spans with a PSRAM
// shadow of the glass, and plans windows trimmed to the columns that really
// differ (4-row aligned, near windows merged). Each window goes to
// St77922Panel::pushRows565() (byte swap) or pushRows332() (wire-order LUT)
// while transposing into its two internal DMA bounce buffers -- the CPU
// fills one while the other is on the wire. The RGB565 shadow (307,200 B)
// is allocated by begin(); the RGB332 one (153,600 B) on the first RGB332
// submit. Switching between the two kinds resends the whole frame.
//
// Task mode (begin(panel, true)): planning and pushing run in a FreeRTOS
// task on core 0, so the caller (core 1) draws the next frame meanwhile.
// submit() returns as soon as the frame is handed over; the caller must not
// touch that framebuffer until wait() (or the next submit(), which waits
// first) returns -- double-buffer to keep drawing.
// Sync mode: submit() plans and pushes before returning.
//
// If the panel reports an SPI error during a push, the next push sends the
// whole frame (the glass may hold a partial update).

#include <stdint.h>

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "FrameBuffer332.h"
#include "FrameBuffer565.h"
#include "St77922Panel.h"

class FramePusher {
public:
  struct Stats {
    uint32_t submitted = 0;   // frames handed to submit()
    uint32_t pushed = 0;      // frames that sent at least one window
    uint32_t dropped = 0;     // submit() gave up waiting for the previous push
    uint32_t bands = 0;       // panel windows sent
    uint64_t bytes = 0;       // RGB565 pixel bytes sent
    uint64_t compared = 0;    // frame bytes compared with the glass shadow
    uint32_t diffUs = 0;      // time planning (compare + shadow update)
    uint32_t pushUs = 0;      // time from first window queued to bus idle
  };

  FramePusher() {}

  // Allocates the RGB565 glass shadow in PSRAM.
  bool begin(St77922Panel& panel, bool useTask);
  // False (and nothing released) when a push is still running after
  // kWaitMs: the task may still be reading the frame and the shadow.
  bool end();
  bool ready() const { return _panel != nullptr; }

  // Hand over a frame (480x320); takes (clears) its tracked push spans.
  // False when the previous push did not finish within kWaitMs (frame
  // dropped and counted; the next push resends the whole frame).
  bool submit(FrameBuffer565& fb);
  bool submit(FrameBuffer332& fb);
  // Block until the last submitted frame is on the glass. False on timeout.
  bool wait(uint32_t timeoutMs = kWaitMs);
  // The next push sends the whole frame.
  void invalidate() { _force.store(true); }
  // What the glass shows (480x320 RGB565, native order); read after wait().
  const uint16_t* shadow565() const { return _shadow565; }
  // Counters since the previous call (then reset).
  Stats takeStats();

  static constexpr uint32_t kWaitMs = 1000;

private:
  static void taskEntry(void* arg);
  bool takeIdle();
  void handOver();
  void pushNow();

  St77922Panel* _panel = nullptr;
  TaskHandle_t _task = nullptr;
  SemaphoreHandle_t _idle = nullptr;    // given while no push is in progress
  portMUX_TYPE _statsLock = portMUX_INITIALIZER_UNLOCKED;

  // Frame being pushed (caller-owned): exactly one is set per job.
  const FrameBuffer565* _job565 = nullptr;
  const FrameBuffer332* _job332 = nullptr;
  bool _last565 = true;                 // kind of the previous push
  std::atomic<bool> _force{true};

  uint16_t* _shadow565 = nullptr;       // what the glass shows (PSRAM)
  uint8_t* _shadow332 = nullptr;        // ...for RGB332 frames (lazy)
  PushPlanner565 _planner565{nullptr, 0, 0};
  PushPlanner _planner{nullptr, 0, 0};
  SpanBands _bands;
  uint16_t _lut[256];                   // RGB332 -> RGB565, wire order
  Stats _stats;
};
