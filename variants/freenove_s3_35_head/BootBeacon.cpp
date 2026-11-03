#include "BootBeacon.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "St77922Panel.h"

#ifndef FREENOVE_BRINGUP
  #define FREENOVE_BRINGUP 0
#endif

namespace freenove {
namespace {

constexpr uint32_t kBeaconPeriodMs = 2000;
constexpr uint32_t kBeaconStack = 3072;

volatile BootStage g_stage = BootStage::Setup;
const char* volatile g_detail = nullptr;

const char* stageName(BootStage s) {
  switch (s) {
    case BootStage::Setup: return "setup (filesystem/prefs; first boot formats SPIFFS)";
    case BootStage::Display: return "display begin";
    case BootStage::Panel: return "panel+touch up";
    case BootStage::Memory: return "PSRAM framebuffers allocated";
    case BootStage::Ready: return "display ready";
    case BootStage::DisplayFailed: return "DISPLAY FAILED";
  }
  return "?";
}

const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

void printStage() {
  const char* detail = g_detail;
  Serial.printf("freenove: alive %lus stage=%s%s%s psram=%u/%u B\n",
                (unsigned long)(millis() / 1000), stageName(g_stage),
                detail ? ": " : "", detail ? detail : "",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
}

void beaconTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kBeaconPeriodMs));
    if (g_stage == BootStage::Ready) break;
    printStage();
  }
  vTaskDelete(nullptr);
}

// Runs before the Arduino core initialises and before setup(): plain GPIO
// only (no Serial, no LEDC).
__attribute__((constructor)) void earlyBacklightOn() {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << St77922Panel::kPinBacklight;
  cfg.mode = GPIO_MODE_OUTPUT;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);
  gpio_set_level((gpio_num_t)St77922Panel::kPinBacklight, 1);
}

}  // namespace

void bootBreadcrumb() {
  Serial.println("freenove: boot");
  const esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("freenove: reset reason %d (%s); PSRAM %u B total, %u B free\n",
                (int)reason, resetReasonName(reason),
                (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#if FREENOVE_BRINGUP
  xTaskCreatePinnedToCore(beaconTask, "fnbeacon", kBeaconStack, nullptr, 1,
                          nullptr, 0);
#endif
}

void setBootStage(BootStage stage, const char* detail) {
  g_detail = detail;
  g_stage = stage;
  if (stage == BootStage::DisplayFailed) printStage();
}

void forceBacklightOn() {
  ledcDetachPin(St77922Panel::kPinBacklight);
  pinMode(St77922Panel::kPinBacklight, OUTPUT);
  digitalWrite(St77922Panel::kPinBacklight, HIGH);
}

}  // namespace freenove
