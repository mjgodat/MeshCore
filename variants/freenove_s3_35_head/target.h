#pragma once

#include <helpers/ESP32Board.h>
#include <helpers/esp32/ESPNOWRadio.h>
#if defined(HEAD_UDP_TRANSPORT) && HEAD_UDP_TRANSPORT
  #include <helpers/esp32/HeadLinkRadio.h>
  using HeadRadioDriver = HeadLinkRadio;   // ESP-NOW, plus UDP when selected
#else
  using HeadRadioDriver = ESPNOWRadio;
#endif
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
extern HeadRadioDriver radio_driver;
extern ESP32RTCClock rtc_clock;
extern SensorManager sensors;

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
#endif

bool radio_init();
mesh::LocalIdentity radio_new_identity();
