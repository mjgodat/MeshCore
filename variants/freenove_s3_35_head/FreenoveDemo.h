#pragma once

// Bring-up framebuffer demo (FREENOVE_BRINGUP=1 only): measures the RGB332
// row-diff push path on hardware.
//
// A task on core 1 renders into two PSRAM RGB332 framebuffers (double
// buffered) the way pages will: a static background drawn once, then per
// frame eraseTouched(background) and the moving objects only. Each finished
// frame goes to FramePusher (push task on core 0). Modes rotate every
// kModeMs:
//   anim : bouncing glow dots, a marching dashed line between two of them,
//          a dashed border, HUD text
//   idle : one static frame re-submitted (row hashes find nothing to send)
//   full : the whole screen changes every frame (worst case)
// Once per second it logs over USB:
//   demo mode=anim fps=.. render_us=.. cpu1=..% diff_us=.. push_us=..
//        push_busy=..% kb/frame=.. bands/frame=.. cmp_kb/frame=..
//        pushed=../.. dropped=.. spi_err=..
// fps counts frames submitted; cpu1 is render time / wall time on the render
// core; diff_us is planning (shadow compare of the tracked spans);
// push_busy is push-task wall time (plan + push, mostly waiting on DMA) /
// wall time; kb/frame is RGB565 bytes on the wire per submitted frame;
// cmp_kb/frame is frame bytes compared with the glass shadow.

#include "FramePusher.h"
#include "St77922Panel.h"

namespace freenove {

// Starts the demo task after `delayMs` (the test pattern stays up until
// then). The demo owns the panel from then on. False when the framebuffers
// cannot be allocated.
bool startFramebufferDemo(FramePusher& pusher, St77922Panel& panel,
                          uint32_t delayMs);

}  // namespace freenove
