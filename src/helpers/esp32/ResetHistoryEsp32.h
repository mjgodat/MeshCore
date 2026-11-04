#pragma once

// ESP32 glue for ResetHistory.h: the record lives in NVS (namespace
// "mcrst"), the per-second copy in RTC memory. Include from ONE translation
// unit (the repeater's main.cpp): it defines the RTC variable.

#include <Arduino.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <helpers/ResetHistory.h>
#include <helpers/bridges/ObserverFrame.h>

namespace mesh {
namespace esp32 {

static RTC_NOINIT_ATTR mesh::resethistory::RtcCopy reset_history_rtc;

class ResetHistory {
public:
  // Once at boot, after the clock is restored.
  void begin() {
    mesh::resethistory::Record rec;
    memset(&rec, 0, sizeof(rec));
    bool loaded = false;
    Preferences prefs;
    if (prefs.begin(kNamespace, true)) {
      loaded = prefs.getBytes(kKey, &rec, sizeof(rec)) == sizeof(rec);
      prefs.end();
    }
    const int reason = static_cast<int>(esp_reset_reason());
    mesh::resethistory::onBoot(rec, loaded, reset_history_rtc, reason >= 0 && reason < 256 ? reason : 0);
    _rec = rec;
    _started = true;
    save();   // boots / prev run, so a run that dies in its first minute still counts
  }

  // Every loop: the RTC copy each second, NVS every checkpoint interval.
  void service(uint32_t uptime_s, uint32_t epoch) {
    if (!_started || uptime_s == _last_second) return;
    _last_second = uptime_s;
    const uint32_t min_heap = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    mesh::resethistory::rtcUpdate(reset_history_rtc, uptime_s, epoch, min_heap);
    if (mesh::resethistory::checkpointDue(uptime_s, _last_checkpoint, _any_checkpoint)) {
      mesh::resethistory::checkpoint(_rec, uptime_s, epoch, min_heap);
      _last_checkpoint = uptime_s;
      _any_checkpoint = true;
      save();
    }
  }

  size_t format(char* out, size_t cap) const {
    if (!_started) { if (cap) out[0] = 0; return 0; }
    return mesh::resethistory::format(_rec, out, cap, mesh::observer::resetReasonName);
  }

private:
  static constexpr const char* kNamespace = "mcrst";
  static constexpr const char* kKey = "rec";
  void save() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return;
    prefs.putBytes(kKey, &_rec, sizeof(_rec));
    prefs.end();
  }
  mesh::resethistory::Record _rec = {};
  uint32_t _last_second = UINT32_MAX, _last_checkpoint = 0;
  bool _any_checkpoint = false, _started = false;
};

}  // namespace esp32
}  // namespace mesh
