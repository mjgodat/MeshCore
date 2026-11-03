#pragma once

#include <helpers/ESP32Board.h>
#include <helpers/esp32/ESPNOWRadio.h>
#include <helpers/SensorManager.h>
#include "BootBeacon.h"
#ifdef DISPLAY_CLASS
  #include "FreenoveDisplay.h"
  #include <helpers/ui/MomentaryButton.h>
#endif

// Freenove FNK0104N (ESP32-S3, 16 MB flash, 8 MB OPI PSRAM, 3.5" ST77922
// QSPI 320x480 with integrated touch) as an ESP-NOW repeater head.
class FreenoveHeadBoard : public ESP32Board {
 public:
  const char* getManufacturerName() const override {
    return "Freenove ESP-NOW Head";
  }
};

extern FreenoveHeadBoard board;
extern ESPNOWRadio radio_driver;
extern ESP32RTCClock rtc_clock;
extern SensorManager sensors;

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
#endif

bool radio_init();
mesh::LocalIdentity radio_new_identity();
