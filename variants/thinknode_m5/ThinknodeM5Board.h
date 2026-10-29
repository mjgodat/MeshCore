#pragma once

#include <Arduino.h>
#include <helpers/RefCountedDigitalPin.h>
#include <helpers/ESP32Board.h>
#include <PCA9557.h>

extern PCA9557 expander;

class ThinknodeM5Board : public ESP32Board {
  bool _hw_v2 = false;

public:

  void begin();

  // v1.0 (L76K) or v2.0 (AG3352), read from the revision strap in begin().
  bool isHardwareV2() const { return _hw_v2; }
  const char* hardwareRevision() const { return _hw_v2 ? "v2.0 (AG3352)" : "v1.0 (L76K)"; }
  // The GPS slide switch is sensed by firmware: GPIO10 on v1.0, GPIO18 on
  // v2.0 (where GPIO10 powers the GPS core instead). HIGH = GPS on.
  uint8_t gpsSwitchPin() const { return _hw_v2 ? 18 : PIN_GPS_SWITCH; }
  bool supportsBatteryCalibration() const override { return true; }

  uint16_t getBattMilliVolts() override;
  const char* getManufacturerName() const override ;

  void applyLedPolicy() override {
    if (!mesh::leds::enabled()) expander.digitalWrite(EXP_PIN_LED, LOW);
  }

  void onBeforeTransmit() override {
    if (mesh::leds::enabled()) expander.digitalWrite(EXP_PIN_LED, HIGH);   // turn TX LED on
  }
  void onAfterTransmit() override {
    expander.digitalWrite(EXP_PIN_LED, LOW);   // turn TX LED off
  }
};