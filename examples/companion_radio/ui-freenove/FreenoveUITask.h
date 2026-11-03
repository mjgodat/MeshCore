#pragma once

// Freenove visual head: the page framework (FREENOVE_VISUAL_HEAD builds).
//
// Tasks and cores
//   WiFi task (core 0)   ESPNOWRadio's receive callback hands observer frames
//                        to onObserverFrame(): one bounded copy into _ring
//                        under a spinlock (drop-oldest), nothing else.
//                        The companion loop likewise copies every MeshCore
//                        packet the head receives into _bridged
//                        (freenoveHeadReceived, from MyMesh::logRxRaw).
//   page task (core 1)   this class: every kFramePeriodMs it drains _ring
//                        into the MeshModel (head clock), polls the touch
//                        controller, runs PageNav and, only when something
//                        asked for a frame, renders the current page into one
//                        of two PSRAM RGB565 framebuffers (over the page's
//                        cached background layer) and submits it.
//   push task (core 0)   FramePusher: plans and DMA-pushes the changed
//                        columns while the next frame is drawn.
//   UI loop (core 1)     the companion (MeshCore + UITask) as before. It owns
//                        the screen only on the HEAD page; elsewhere it keeps
//                        rendering into its own buffer, which is not pushed.
//                        serviceUiLoop() runs HOME's quick-control requests
//                        there (advert, admin console, login): the companion
//                        objects belong to that loop.
// The model and pages are touched only by the page task (no locks).
//
// Frame budget: kFramePeriodMs (17 ms, <= 60 fps) per loop iteration via
// xTaskDelayUntil; a frame is rendered only when the page was just shown,
// touch changed it or Page::wantsFrame() (new data / animation). Otherwise
// the loop just drains, polls touch and sleeps.
//
// CYD (HMI_PROFILE_CYD, classic ESP32 without PSRAM): the same task and
// pages, with the model in internal RAM (HMI_MODEL_NODES / HMI_MODEL_FEED),
// no framebuffers and no background layer: CYDDisplay::renderPage() replays
// the page's whole frame (drawFrame) once per band and pushes the bands that
// changed. Touch is polled every kFramePeriodMs (25 ms); frames are capped at
// one per kMinFrameGapMs (66 ms, <= 15 fps); an idle page pushes nothing.

#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <helpers/bridges/ObserverDemux.h>

#include <atomic>

#include "FrameBuffer565.h"
#include "HmiLogic.h"
#include "HmiProfile.h"
#include "MeshModel.h"
#include "RelayNameStore.h"
#include "PageNav.h"
#include "StubPages.h"

#if HMI_CYD
class CYDDisplay;
typedef CYDDisplay HeadDisplay;
#else
class FreenoveDisplay;
typedef FreenoveDisplay HeadDisplay;
#endif

// Companion loop (MyMesh::logRxRaw): a MeshCore packet this head received
// itself (ESP-NOW). Bounded copy for the page task; no-op before begin().
void freenoveHeadReceived(const uint8_t* raw, int len);

class FreenoveUITask {
 public:
  explicit FreenoveUITask(HeadDisplay& display);

  // Allocates the model and framebuffers (PSRAM), registers the observer
  // handler with ESPNOWRadio and starts the page task. Call once the display
  // and the companion UI are up. False (companion UI keeps the screen) when
  // memory or the task cannot be had.
  // self_pub_key: the head's own public key (its adverts are not drawn).
  bool begin(const uint8_t* self_pub_key);
  // UI loop (main loop): run a request a page raised (HOME quick controls)
  // on the companion's own loop. Cheap when there is none.
  void serviceUiLoop();

#if HMI_CYD
  static constexpr uint32_t kFramePeriodMs = 25;   // touch poll
  static constexpr uint32_t kMinFrameGapMs = 66;   // <= 15 fps
#else
  static constexpr uint32_t kFramePeriodMs = 17;   // <= 60 fps
  static constexpr uint32_t kMinFrameGapMs = 0;
#endif
  static constexpr size_t kRingDepth = 16;          // observer frames (~4 KB)
  static constexpr size_t kBridgedDepth = 8;        // received packets (~2 KB)

 private:
  static void taskEntry(void* arg);
  static void onObserverFrame(const uint8_t* frame, size_t len, void* ctx);
  static void drawHeadOverlay(FrameBuffer565& fb, void* ctx);
#if HMI_CYD
  static void drawBand(FrameBuffer565& fb, int band, void* ctx);
#endif
  friend void freenoveHeadReceived(const uint8_t* raw, int len);

  void run();
  void drainObserver(uint32_t now_ms);
  void pollTouch(uint32_t now_ms);
  void showPage(int index, uint32_t now_ms);
  void renderFrame(uint32_t now_ms);
  void updateRadio(uint32_t now_ms);
  void updateClock(uint32_t now_ms);
  void handleRequest(freenove_ui::PageRequest req, uint32_t now_ms);
  freenove_ui::Page* current() { return _pages.page(_nav.current()); }

  HeadDisplay& _display;
  TaskHandle_t _task = nullptr;

  // WiFi task -> page task.
  portMUX_TYPE _ringLock = portMUX_INITIALIZER_UNLOCKED;
  mesh::observer::FrameRing<kRingDepth> _ring;
  // Companion loop -> page task (same lock): packets the head received.
  mesh::observer::FrameRing<kBridgedDepth> _bridged;

  mesh::head::MeshModel* _model = nullptr;
  freenove_ui::PageContext _ctx;
  freenove_ui::PageSet _pages;
  freenove_ui::PageNav _nav;

#if HMI_CYD
  uint32_t _lastFrameMs = 0;
  uint32_t _pushUs = 0;            // time pushing bands since the last stats line
  uint32_t _bandsPushed = 0;
  uint32_t _chromeUs = 0, _pageUs = 0, _headerUs = 0;   // render split (drawBand)
#else
  uint16_t* _bg = nullptr;                  // current page background layer (PSRAM)
  uint16_t* _buf[2] = {nullptr, nullptr};   // double-buffered frames (PSRAM)
  FrameBuffer565 _fb[2] = {FrameBuffer565(nullptr, 0, 0), FrameBuffer565(nullptr, 0, 0)};
  int _cur = 0;
  bool _needsCopy[2] = {true, true};
#endif
  bool _bgDirty = true;          // rebuild _bg before the next frame (Freenove); a new page (CYD)
  bool _forceFrame = true;
  bool _wasOn = true;
  bool _touchDown = false;
  freenove_ui::Page* _shown = nullptr;
  uint32_t _framesRendered = 0;
  uint32_t _renderUs = 0;          // time in renderFrame since the last stats line
  uint32_t _bgUs = 0;              // ...of which building backgrounds
  // Top HUD line: the repeater's radio (RadioLabel.h), from observer vitals.
  char _radioLabel[48] = "NO REPEATER DATA";
  freenove_ui::RadioState _radioState = freenove_ui::kRadioNone;
  uint32_t _radioGen = 0;
  uint32_t _radioCheckMs = 0;
  // Header clock: the head RTC once it has been set this session, else uptime.
  freenove_ui::hmi::ClockTrack _clock;
  uint32_t _clockCheckMs = 0;
  int _linkLevel = -1;
  uint32_t _presetGen = 0;   // preset names copied into _ctx
  uint32_t _nameGen = 0;     // repeater fallback name copied into _ctx
  uint32_t _homeCheckMs = 0; // UI loop: home-repeater contact name refresh
  void updateRepeaterName();
  // Relay names kept across reboots (RelayNameStore.h, PSRAM): loaded from
  // NVS at begin(), written back by the page task when due (throttled).
  mesh::head::RelayNameStore* _names = nullptr;
  void saveRelayNames(uint32_t now_ms);
  // Page task -> UI loop: one request at a time (0 = none), and its outcome
  // back (0 = none, 1 = ok, 2 = failed).
  std::atomic<uint8_t> _uiRequest{0};
  std::atomic<uint8_t> _uiResult{0};
  freenove_ui::PageRequest _resultFor = freenove_ui::kReqNone;
};
