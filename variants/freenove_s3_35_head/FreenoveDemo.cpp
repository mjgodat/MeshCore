#include "FreenoveDemo.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <stdio.h>

#include "FreenoveGeometry.h"

namespace freenove {
namespace {

constexpr uint32_t kModeMs = 10000;
constexpr int kDots = 8;
constexpr int kFbBytes = kLandscapeW * kLandscapeH;

enum Mode { kAnim = 0, kIdle = 1, kFull = 2, kModes = 3 };
const char* const kModeNames[kModes] = {"anim", "idle", "full"};

struct Dot {
  int x, y;     // 1/16 px
  int vx, vy;   // 1/16 px per frame
  uint8_t c;
};

// The page pattern the framework will use: a static background drawn once,
// two double-buffered frames that start as copies of it, and per frame
// eraseTouched(background) + draw the moving objects.
struct Demo {
  FramePusher* pusher;
  St77922Panel* panel;
  uint32_t delayMs;
  uint8_t* bg;
  uint8_t* buf[2];
  FrameBuffer332 fb[2] = {FrameBuffer332(nullptr, 0, 0),
                          FrameBuffer332(nullptr, 0, 0)};
  bool needsBackground[2];
  Dot dots[kDots];
  char hud[64];
};

Demo g_demo;

void stepDots(Demo& d) {
  const int maxX = (kLandscapeW - 1) * 16, maxY = (kLandscapeH - 1) * 16;
  for (Dot& p : d.dots) {
    p.x += p.vx;
    p.y += p.vy;
    if (p.x < 8 * 16) { p.x = 8 * 16; p.vx = -p.vx; }
    if (p.x > maxX - 8 * 16) { p.x = maxX - 8 * 16; p.vx = -p.vx; }
    if (p.y < 28 * 16) { p.y = 28 * 16; p.vy = -p.vy; }
    if (p.y > maxY - 8 * 16) { p.y = maxY - 8 * 16; p.vy = -p.vy; }
  }
}

void drawBackground(uint8_t* px) {
  FrameBuffer332 fb(px, kLandscapeW, kLandscapeH);
  fb.clear(fbpal::kBlack);
  for (int x = 40; x < kLandscapeW; x += 40) fb.vline(x, 24, kLandscapeH - 24, fbpal::kNavy);
  for (int y = 40; y < kLandscapeH; y += 40) fb.hline(0, y, kLandscapeW, fbpal::kNavy);
  fb.circle(240, 172, 120, fbpal::kGrid);
  fb.arc(240, 172, 90, 200, 340, 3, fbpal::kGrid);
  fb.hline(0, 22, kLandscapeW, fbpal::kGrid);
  fb.text(6, 3, "FREENOVE HEAD", fbpal::kCyan, kFont8x16Id);
}

void render(Demo& d, int cur, Mode mode, uint32_t frame) {
  FrameBuffer332& fb = d.fb[cur];
  if (mode == kFull) {
    // Every row differs from the previous frame (worst case).
    fb.clear((uint8_t)(frame * 37u + 1u));
    fb.text(6, 3, "FULL-SCREEN CHANGE", fbpal::kWhite, kFont8x16Id);
    fb.text(6, 300, d.hud, fbpal::kWhite, kFont6x8Id);
    d.needsBackground[cur] = true;
    return;
  }
  if (d.needsBackground[cur]) {
    fb.copyFrom(d.bg);
    d.needsBackground[cur] = false;
  } else {
    fb.eraseTouched(d.bg);   // only what last frame's objects covered
  }
  const Dot& a = d.dots[0];
  const Dot& b = d.dots[1];
  // Marching link: phase advances one pixel per frame toward dot b.
  fb.dashedLine(a.x / 16, a.y / 16, b.x / 16, b.y / 16, fbpal::kCyan, 6, 4,
                mode == kAnim ? (int)frame : 0);
  for (const Dot& p : d.dots) fb.glowDot(p.x / 16, p.y / 16, p.c);
  fb.text(160, 7, kModeNames[mode], fbpal::kAmber, kFont6x8Id);
  fb.text(6, kLandscapeH - 10, d.hud, fbpal::kSky, kFont6x8Id);
}

void demoTask(void* arg) {
  Demo& d = *static_cast<Demo*>(arg);
  vTaskDelay(pdMS_TO_TICKS(d.delayMs));
  Serial.println("freenove: framebuffer demo running (modes anim/idle/full, "
                 "10 s each)");
  d.pusher->invalidate();

  uint32_t frame = 0, framesThisSec = 0;
  uint64_t renderUs = 0;
  int cur = 0;
  int mode = kAnim;
  const int64_t start = esp_timer_get_time();
  int64_t lastLog = start, modeStart = start;
  d.pusher->takeStats();
  for (;;) {
    const int64_t t0 = esp_timer_get_time();
    if (mode == kAnim) stepDots(d);
    render(d, cur, (Mode)mode, frame);
    const int64_t t1 = esp_timer_get_time();
    renderUs += (uint64_t)(t1 - t0);
    // Waits for the previous frame's push, then hands this one over; the
    // other buffer is free to draw into once this returns.
    d.pusher->submit(d.fb[cur]);
    cur ^= 1;
    frame++;
    framesThisSec++;
    vTaskDelay(1);   // let the companion loop (same core) run

    const int64_t now = esp_timer_get_time();
    if (now - lastLog >= 1000000) {
      const double wallUs = (double)(now - lastLog);
      const FramePusher::Stats s = d.pusher->takeStats();
      const double fps = framesThisSec * 1e6 / wallUs;
      const uint32_t n = s.submitted ? s.submitted : 1;
      Serial.printf(
          "demo mode=%s fps=%.1f render_us=%u cpu1=%.0f%% diff_us=%u "
          "push_us=%u push_busy=%.0f%% kb/frame=%.1f bands/frame=%.2f "
          "cmp_kb/frame=%.1f pushed=%u/%u dropped=%u spi_err=%u\n",
          kModeNames[mode], fps,
          (unsigned)(framesThisSec ? renderUs / framesThisSec : 0),
          renderUs * 100.0 / wallUs, (unsigned)(s.diffUs / n),
          (unsigned)(s.pushUs / n), (s.diffUs + s.pushUs) * 100.0 / wallUs,
          (double)s.bytes / n / 1024.0, (double)s.bands / n,
          (double)s.compared / n / 1024.0, (unsigned)s.pushed,
          (unsigned)s.submitted, (unsigned)s.dropped,
          (unsigned)d.panel->errors());
      snprintf(d.hud, sizeof(d.hud), "%s %.0f FPS  %.1f KB/F  %u BANDS",
               kModeNames[mode], fps, (double)s.bytes / n / 1024.0,
               (unsigned)(s.bands / n));
      framesThisSec = 0;
      renderUs = 0;
      lastLog = now;
    }
    if (now - modeStart >= (int64_t)kModeMs * 1000) {
      mode = (mode + 1) % kModes;
      modeStart = now;
    }
  }
}

}  // namespace

bool startFramebufferDemo(FramePusher& pusher, St77922Panel& panel,
                          uint32_t delayMs) {
  Demo& d = g_demo;
  d.pusher = &pusher;
  d.panel = &panel;
  d.delayMs = delayMs;
  d.bg = (uint8_t*)heap_caps_malloc(kFbBytes, MALLOC_CAP_SPIRAM);
  d.buf[0] = (uint8_t*)heap_caps_malloc(kFbBytes, MALLOC_CAP_SPIRAM);
  d.buf[1] = (uint8_t*)heap_caps_malloc(kFbBytes, MALLOC_CAP_SPIRAM);
  if (d.bg == nullptr || d.buf[0] == nullptr || d.buf[1] == nullptr) {
    Serial.println("freenove: demo framebuffer allocation failed");
    heap_caps_free(d.bg);
    heap_caps_free(d.buf[0]);
    heap_caps_free(d.buf[1]);
    return false;
  }
  drawBackground(d.bg);
  for (int i = 0; i < 2; i++) {
    d.fb[i] = FrameBuffer332(d.buf[i], kLandscapeW, kLandscapeH);
    d.needsBackground[i] = true;
  }
  const uint8_t colours[] = {fbpal::kCyan, fbpal::kMagenta, fbpal::kGreen,
                             fbpal::kAmber, fbpal::kViolet, fbpal::kSky,
                             fbpal::kIce, fbpal::kWhite};
  for (int i = 0; i < kDots; i++) {
    Dot& p = d.dots[i];
    p.x = (40 + i * 53) * 16;
    p.y = (60 + (i * 37) % 220) * 16;
    p.vx = (i % 2 ? 1 : -1) * (20 + i * 7);
    p.vy = (i % 3 ? 1 : -1) * (14 + i * 5);
    p.c = colours[i];
  }
  snprintf(d.hud, sizeof(d.hud), "starting");
  if (xTaskCreatePinnedToCore(demoTask, "fbdemo", 6144, &d, 1, nullptr, 1)
      != pdPASS) {
    Serial.println("freenove: demo task create failed");
    heap_caps_free(d.bg);
    heap_caps_free(d.buf[0]);
    heap_caps_free(d.buf[1]);
    return false;
  }
  return true;
}

}  // namespace freenove
