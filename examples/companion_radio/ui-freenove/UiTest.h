#pragma once

// On-device UI test hooks (FREENOVE_UITEST builds only, env
// Freenove_S3_35_Head_uitest): the USB console (UiTestConsole.cpp, on the
// companion loop) injects touch gestures and page changes that the page
// task (FreenoveUITask) picks up as if they came from the touch controller.
//
// Gesture: a press at (x1, y1) moving linearly to (x2, y2) over dur_ms, then
// released for kReleaseMs. A tap is a gesture with no movement held for
// kTapMs. One gesture at a time; a new one replaces an unfinished one.

#include <stdint.h>

#include "RelayNameStore.h"

namespace freenove_uitest {

constexpr uint32_t kTapMs = 120;
constexpr uint32_t kReleaseMs = 80;

// Console side.
void startGesture(int x1, int y1, int x2, int y2, uint32_t dur_ms);
bool gestureBusy();
void requestPage(int index);

// Page task side. overrideTouch() replaces the touch controller's sample
// while a gesture is in progress (true then).
bool overrideTouch(uint32_t now_ms, bool& down, int& x, int& y);
int takePageRequest();   // -1 = none
void notePage(int index, const char* title);
// The relay-name store the page task loaded at boot (console "relays").
void noteRelayNames(const mesh::head::RelayNameStore* store);

// CYD screenshots (no framebuffer to read): the console asks the page task
// to render the current page once more into `sink`; the page task answers
// with shotDone(). takeShotRequest() is true once per request.
typedef void (*ShotSink)(int y, const uint16_t* row, void* ctx);
void requestShot(ShotSink sink, void* ctx);
bool takeShotRequest(ShotSink& sink, void*& ctx);
void shotDone();
bool shotPending();

// Page task: a sample from the touch controller itself (no gesture
// injected); press edges are counted with the last press position.
void noteRealTouch(bool down, int x, int y);

// Either side.
int currentPage();
const char* currentPageTitle();
// Page task: the last stats window (frames rendered over window_ms, time
// rendering / pushing, bands pushed), for the console's "stats".
void noteStats(uint32_t frames, uint32_t window_ms, uint32_t render_us, uint32_t push_us, uint32_t bands,
               uint32_t chrome_us, uint32_t page_us, uint32_t header_us);

}  // namespace freenove_uitest
