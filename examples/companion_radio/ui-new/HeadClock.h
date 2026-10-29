#pragma once

// Repeater head clock floor (UI_REPEATER_HEAD). The head has no RTC battery,
// and remote admin rejects requests whose timestamps go backwards, so keep a
// persisted floor: on boot the clock never starts below the last time this
// head knew. Mesh clock sync (COMPANION_MESH_CLOCK_SYNC) then corrects it
// forward from adverts and channel posts.

#include <Arduino.h>
#include <Preferences.h>
#include <MeshCore.h>

class HeadClockFloor {
 public:
  explicit HeadClockFloor(mesh::RTCClock& rtc) : _rtc(rtc) {}

  void begin() {
    Preferences prefs;
    if (prefs.begin(kNamespace, false)) {
      _saved = prefs.getUInt(kKey, 0);
      prefs.end();
    }
    if (_saved > _rtc.getCurrentTime()) _rtc.setCurrentTime(_saved);
  }

  // Persist the current time at most every kSaveIntervalSecs of clock time,
  // so flash wear stays negligible while a reboot loses at most that much.
  void loop() {
    const uint32_t now = _rtc.getCurrentTime();
    if (now < _saved + kSaveIntervalSecs) return;
    Preferences prefs;
    if (prefs.begin(kNamespace, false)) {
      prefs.putUInt(kKey, now);
      prefs.end();
    }
    _saved = now;
  }

 private:
  static constexpr const char* kNamespace = "hclock";
  static constexpr const char* kKey = "floor";
  static constexpr uint32_t kSaveIntervalSecs = 600;

  mesh::RTCClock& _rtc;
  uint32_t _saved = 0;
};
