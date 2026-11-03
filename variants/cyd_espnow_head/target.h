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
#ifdef DISPLAY_CLASS
  #include "CYDDisplay.h"
  #include <helpers/ui/MomentaryButton.h>
#endif

class CYDHeadBoard : public ESP32Board {
 public:
  const char* getManufacturerName() const override {
    return "CYD ESP-NOW Head";
  }
};

extern CYDHeadBoard board;
extern HeadRadioDriver radio_driver;
extern ESP32RTCClock rtc_clock;
extern SensorManager sensors;

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
#endif

bool radio_init();
mesh::LocalIdentity radio_new_identity();
