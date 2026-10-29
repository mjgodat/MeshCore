#pragma once

// Sticky backlight toggle on a dedicated button (BACKLIGHT_BTN). Each press
// flips the light on or off and the choice survives reboots. Brightness, where
// the hardware has a dimmer (the ThinkNode M5's rotary switch), is analog and
// outside firmware control.

#if defined(BACKLIGHT_BTN) && defined(ESP32)

#include <Arduino.h>
#include <Preferences.h>
// EXP_PIN_BACKLIGHT boards declare `expander` in their target.h, which callers
// include first. PCA9557.h has no include guard, so it is not included here.

namespace mesh {
namespace ui {

class BacklightToggleButton {
 public:
  void begin() {
    pinMode(BACKLIGHT_BTN, INPUT_PULLUP);
    Preferences prefs;
    // Read-write so a first boot creates the namespace instead of logging
    // NVS NOT_FOUND; nothing is written until the button is pressed.
    if (prefs.begin(kNamespace, false)) {
      lit_ = prefs.getBool(kKey, false);
      prefs.end();
    }
    drive(lit_);
    pressed_ = digitalRead(BACKLIGHT_BTN) == LOW;
    changed_at_ = millis();
  }

  // Returns true when this poll toggled the light.
  bool poll(uint32_t now) {
    const bool pressed = digitalRead(BACKLIGHT_BTN) == LOW;
    if (pressed == pressed_ || now - changed_at_ < kDebounceMillis) return false;
    pressed_ = pressed;
    changed_at_ = now;
    if (!pressed) return false;
    lit_ = !lit_;
    drive(lit_);
    Preferences prefs;
    if (prefs.begin(kNamespace, false)) {
      prefs.putBool(kKey, lit_);
      prefs.end();
    }
    return true;
  }

  bool lit() const { return lit_; }

 private:
  static constexpr const char* kNamespace = "backlight";
  static constexpr const char* kKey = "on";
  static constexpr uint32_t kDebounceMillis = 40;

  static void drive(bool on) {
#if defined(EXP_PIN_BACKLIGHT)
    expander.digitalWrite(EXP_PIN_BACKLIGHT, on ? HIGH : LOW);
#elif defined(DISP_BACKLIGHT)
    digitalWrite(DISP_BACKLIGHT, on ? HIGH : LOW);
#else
    (void)on;
#endif
  }

  bool lit_ = false;
  bool pressed_ = false;
  uint32_t changed_at_ = 0;
};

}  // namespace ui
}  // namespace mesh

#endif
