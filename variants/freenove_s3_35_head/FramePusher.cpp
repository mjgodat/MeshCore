#include "FramePusher.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "FreenoveGeometry.h"

namespace {
constexpr uint32_t kTaskStack = 4096;
constexpr UBaseType_t kTaskPriority = 2;   // above idle/loop, far below WiFi
constexpr BaseType_t kTaskCore = 0;        // UI/render stays on core 1
constexpr size_t kPixels = (size_t)freenove::kLandscapeW * freenove::kLandscapeH;
}  // namespace

bool FramePusher::begin(St77922Panel& panel, bool useTask) {
  if (_panel != nullptr) return true;
  buildLut565(_lut, true);
  _shadow565 = (uint16_t*)heap_caps_malloc(kPixels * 2, MALLOC_CAP_SPIRAM);
  if (_shadow565 == nullptr) return false;
  _planner565 = PushPlanner565(_shadow565, freenove::kLandscapeW, freenove::kLandscapeH);
  _force.store(true);
  _idle = xSemaphoreCreateBinary();
  if (_idle == nullptr) {
    heap_caps_free(_shadow565);
    _shadow565 = nullptr;
    return false;
  }
  xSemaphoreGive(_idle);
  _panel = &panel;
  if (useTask) {
    if (xTaskCreatePinnedToCore(taskEntry, "fbpush", kTaskStack, this,
                                kTaskPriority, &_task, kTaskCore) != pdPASS) {
      Serial.println("framepusher: task create failed, using sync pushes");
      _task = nullptr;
    }
  }
  return true;
}

bool FramePusher::end() {
  if (_panel == nullptr) return true;
  // Take (and keep) the idle token: no push can start or be running after.
  if (xSemaphoreTake(_idle, pdMS_TO_TICKS(kWaitMs)) != pdTRUE) {
    Serial.println("framepusher: end() while a push is still running; "
                   "keeping buffers");
    return false;
  }
  if (_task != nullptr) {
    vTaskDelete(_task);
    _task = nullptr;
  }
  vSemaphoreDelete(_idle);
  _idle = nullptr;
  heap_caps_free(_shadow565);
  heap_caps_free(_shadow332);
  _shadow565 = nullptr;
  _shadow332 = nullptr;
  _planner565 = PushPlanner565(nullptr, 0, 0);
  _planner = PushPlanner(nullptr, 0, 0);
  _panel = nullptr;
  return true;
}

bool FramePusher::wait(uint32_t timeoutMs) {
  if (_idle == nullptr) return true;
  if (xSemaphoreTake(_idle, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;
  xSemaphoreGive(_idle);
  return true;
}

// Wait for the previous push. On a timeout the frame is dropped: the
// planners rely on consecutive submits (the other buffer differs from the
// glass only where the last push changed it); a skipped frame breaks that,
// so the next push resends everything.
bool FramePusher::takeIdle() {
  if (xSemaphoreTake(_idle, pdMS_TO_TICKS(kWaitMs)) == pdTRUE) return true;
  _force.store(true);
  portENTER_CRITICAL(&_statsLock);
  _stats.submitted++;
  _stats.dropped++;
  portEXIT_CRITICAL(&_statsLock);
  return false;
}

void FramePusher::handOver() {
  if (_task != nullptr) {
    xTaskNotifyGive(_task);       // the task gives _idle back when done
  } else {
    pushNow();
    xSemaphoreGive(_idle);
  }
}

bool FramePusher::submit(FrameBuffer565& fb) {
  if (_panel == nullptr || fb.pixels() == nullptr) return false;
  if (fb.width() != freenove::kLandscapeW || fb.height() != freenove::kLandscapeH) return false;
  if (!takeIdle()) return false;
  // No push is running: safe to hand the spans to the planner.
  _planner565.take(fb);
  _job565 = &fb;
  _job332 = nullptr;
  handOver();
  return true;
}

bool FramePusher::submit(FrameBuffer332& fb) {
  if (_panel == nullptr || fb.pixels() == nullptr) return false;
  if (fb.width() != freenove::kLandscapeW || fb.height() != freenove::kLandscapeH) return false;
  if (!takeIdle()) return false;
  if (_shadow332 == nullptr) {
    _shadow332 = (uint8_t*)heap_caps_malloc(kPixels, MALLOC_CAP_SPIRAM);
    if (_shadow332 == nullptr) {
      xSemaphoreGive(_idle);
      return false;
    }
    _planner = PushPlanner(_shadow332, freenove::kLandscapeW, freenove::kLandscapeH);
  }
  _planner.take(fb);
  _job332 = &fb;
  _job565 = nullptr;
  handOver();
  return true;
}

void FramePusher::taskEntry(void* arg) {
  FramePusher* self = static_cast<FramePusher*>(arg);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    self->pushNow();
    xSemaphoreGive(self->_idle);
  }
}

void FramePusher::pushNow() {
  const bool is565 = _job565 != nullptr;
  // The other kind's shadow no longer matches the glass: everything is new.
  if (_force.exchange(false) || is565 != _last565) {
    _planner565.invalidate();
    _planner.invalidate();
  }
  _last565 = is565;

  const int64_t t0 = esp_timer_get_time();
  PushPlanner::Stats ps;
  if (is565) _planner565.plan(*_job565, _bands, &ps);
  else _planner.plan(*_job332, _bands, &ps);
  const int64_t t1 = esp_timer_get_time();

  const uint32_t errorsBefore = _panel->errors();
  for (int i = 0; i < _bands.count; i++) {
    if (is565) {
      _panel->pushRows565(_job565->pixels(), _bands.y0[i], _bands.n[i], _bands.x0[i], _bands.x1[i]);
    } else {
      _panel->pushRows332(_job332->pixels(), _lut, _bands.y0[i], _bands.n[i], _bands.x0[i], _bands.x1[i]);
    }
  }
  if (_bands.count > 0) _panel->waitIdle();
  const int64_t t2 = esp_timer_get_time();
  if (_panel->errors() != errorsBefore) _force.store(true);   // resend all

  portENTER_CRITICAL(&_statsLock);
  _stats.submitted++;
  if (_bands.count > 0) _stats.pushed++;
  _stats.bands += (uint32_t)_bands.count;
  _stats.bytes += (uint64_t)spanBandBytes(_bands);
  _stats.compared += (uint64_t)ps.comparedBytes;
  _stats.diffUs += (uint32_t)(t1 - t0);
  _stats.pushUs += (uint32_t)(t2 - t1);
  portEXIT_CRITICAL(&_statsLock);
}

FramePusher::Stats FramePusher::takeStats() {
  portENTER_CRITICAL(&_statsLock);
  Stats s = _stats;
  _stats = Stats();
  portEXIT_CRITICAL(&_statsLock);
  return s;
}
