#pragma once

// Visual head theme: the owner-approved HMI look (Task 7c, mockup
// mock_glass3.png): dark navy-teal backdrop with a diagonal gradient, rounded
// navy panels with a thin cyan edge and a soft glow, cyan accents. Every
// page, the header and the tab bar take their colours from these tokens only
// (test/test_layouts.py enforces it), so a later theme change is one file.
// Values are RGB565, written as rgb565(r, g, b) with the mockup's 8-bit
// channels.

#include <stdint.h>

#include "FrameBuffer565.h"

namespace freenove_ui {
namespace theme {

// Backdrop: diagonal gradient, t = 0.6 x/W + 0.4 y/H, from kBgNear (t = 0,
// top left) to kBgFar (t = 1, bottom right); dithered when drawn.
struct Rgb8 { uint8_t r, g, b; };
constexpr Rgb8 kBgNear = {10, 32, 50};
constexpr Rgb8 kBgFar = {4, 14, 24};
constexpr uint16_t kBg = rgb565(6, 20, 32);           // flat fallback
// Panel glow: the edge colour blurred (sigma 4 px) and added to the backdrop.
constexpr Rgb8 kGlow = {20, 120, 140};
constexpr int kGlowPercent = 30;                       // peak, % of kGlow
constexpr uint16_t kGlow565 = rgb565(20, 120, 140);    // kGlow in RGB565 (the CYD's faint edge rings)

// Panels.
constexpr uint16_t kPanelFill = rgb565(8, 24, 36);
constexpr uint16_t kPanelEdge = rgb565(30, 130, 150);
constexpr uint16_t kDivider = rgb565(25, 70, 85);      // under panel headers
constexpr int kPanelRadius = 6;

// Text.
constexpr uint16_t kText = rgb565(235, 245, 250);      // titles, values (white)
constexpr uint16_t kTextDim = rgb565(120, 150, 165);   // secondary, units
constexpr uint16_t kAccent = rgb565(40, 220, 240);     // cyan: notes, readouts, active
constexpr uint16_t kAccentDim = rgb565(20, 90, 110);
constexpr uint16_t kStale = rgb565(110, 125, 135);     // stale / unknown (grey)

// Signal (link quality, SNR >= 5 / >= 0 / >= -7 / below) and states.
constexpr uint16_t kSigStrong = rgb565(50, 230, 140);  // green
constexpr uint16_t kSigGood = kAccent;                 // cyan
constexpr uint16_t kSigWeak = rgb565(255, 175, 40);    // amber
constexpr uint16_t kSigPoor = rgb565(190, 110, 255);   // magenta-violet
constexpr uint16_t kGood = kSigStrong;                 // "on", "quiet"
constexpr uint16_t kWarn = kSigWeak;                   // "moderate", no data
constexpr uint16_t kBad = rgb565(255, 80, 70);         // "noisy", errors

// Topology.
constexpr uint16_t kLink = rgb565(28, 60, 72);         // links between contacts
constexpr uint16_t kRoute = kAccent;                   // last route (2 px)
constexpr uint16_t kRouteRest = rgb565(24, 120, 140);  // the route after its animation
constexpr uint16_t kBlip = rgb565(255, 255, 255);      // travelling packet
constexpr uint16_t kRepeater = kText;                  // the M5 ring + dot
constexpr uint16_t kGeoRing = kDivider;                // GEO range rings, compass
constexpr uint16_t kSweep = kAccent;                   // GEO sweep beam
constexpr uint16_t kEsp = rgb565(80, 150, 255);        // heard over ESP-NOW only

// Route caption packet types: never a contact, route or blip colour.
constexpr uint16_t kFeedGrp = rgb565(120, 200, 255);   // sky
constexpr uint16_t kFeedTxt = rgb565(220, 240, 120);   // yellow-green
constexpr uint16_t kFeedAdv = kSigWeak;                // amber (adverts, ACKs)
constexpr uint16_t kFeedTrc = rgb565(160, 150, 200);   // lilac (trace, other)
constexpr uint16_t kFeedEsp = kEsp;

// Gauges and counters.
constexpr uint16_t kGaugeTrack = rgb565(25, 60, 72);
constexpr uint16_t kNeedle = kText;
constexpr uint16_t kOdoFill = rgb565(2, 10, 16);
constexpr uint16_t kOdoEdge = rgb565(30, 100, 120);

// Quick-control tiles and tabs.
constexpr uint16_t kTileOnFill = rgb565(10, 60, 72);
constexpr uint16_t kTileOffFill = rgb565(6, 18, 28);
constexpr uint16_t kTileOffEdge = rgb565(30, 90, 105);
constexpr uint16_t kTabFill = kPanelFill;              // other tabs
constexpr uint16_t kTabActiveFill = kTileOnFill;       // current tab
constexpr uint16_t kTabOutline = kTileOffEdge;
constexpr uint16_t kTabActiveOutline = kAccent;
constexpr uint16_t kTabText = kTextDim;
constexpr uint16_t kTabActiveText = kAccent;

}  // namespace theme
}  // namespace freenove_ui
