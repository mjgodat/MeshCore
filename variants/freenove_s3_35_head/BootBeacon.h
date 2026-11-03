#pragma once

// Boot breadcrumbs for the Freenove FNK0104N head, so a failed start is never
// silent (a dark board with no USB output cannot tell "never ran" from "ran
// headless" from "busy formatting SPIFFS").
//
//  - A static constructor drives the backlight (GPIO 41) high before the
//    Arduino core and setup() run: a lit (blank) glass means the application
//    image started. St77922Panel::begin() takes the pin over for LEDC.
//  - bootBreadcrumb(), called right after Serial.begin() in setup(), prints
//    "freenove: boot", the reset reason and the PSRAM the core found.
//  - One-shot lines printed before a host opens the port are lost (the HWCDC
//    TX ring is purged when a new host session starts), so bring-up builds
//    also run a beacon that repeats the current boot stage every 2 s until
//    the display is up -- and forever if the display failed.
//  - A display start failure leaves the backlight on (forceBacklightOn()).

#include <stdint.h>

namespace freenove {

enum class BootStage : uint8_t {
  Setup,           // setup() entered (filesystem, prefs: first boot formats SPIFFS)
  Display,         // FreenoveDisplay::begin() entered
  Panel,           // ST77922 + touch initialised
  Memory,          // PSRAM framebuffers allocated
  Ready,           // display started (beacon stops)
  DisplayFailed,   // display start failed (beacon keeps repeating)
};

void bootBreadcrumb();
void setBootStage(BootStage stage, const char* detail = nullptr);
// Backlight full on as a plain GPIO (detaches LEDC first).
void forceBacklightOn();

}  // namespace freenove
