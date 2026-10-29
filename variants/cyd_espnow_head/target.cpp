#include <Arduino.h>
#include "target.h"
#include <helpers/ArduinoHelpers.h>
#include <helpers/ESP32TrueRandom.h>

CYDHeadBoard board;

ESPNOWRadio radio_driver;

ESP32RTCClock rtc_clock;
SensorManager sensors;

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  // BOOT button: same 1/2/3/4-click navigation as other single-button
  // companions, alongside the touchscreen.
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true, true, true);
#endif

bool radio_init() {
  rtc_clock.begin();

  radio_driver.init();

  return true;  // success
}

// Combine the normal software source with true entropy captured before RF/ADC
// initialization. The hardware source is never read in pseudo-random-only mode.
class ESP_RNG : public mesh::RNG {
public:
  void random(uint8_t* dest, size_t sz) override {
    for (size_t i = 0; i < sz; ++i) {
      dest[i] = (::random(0, 256) & 0xFF);
    }
    mesh::mixESP32TrueRandom(dest, sz);
  }
};

mesh::LocalIdentity radio_new_identity() {
  ESP_RNG rng;
  return mesh::LocalIdentity(&rng);  // create new random identity
}
