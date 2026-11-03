#pragma once

// Screen profile of the visual head pages (pure C++11).
//
// One page codebase, two screens, chosen at build time:
//   Freenove FNK0104N (default): 480x320, full RGB565 framebuffers in PSRAM,
//     a cached background layer per page (FreenoveUITask).
//   CYD ESP32-2432S028 (-D HMI_PROFILE_CYD=1): 320x240 on a classic ESP32
//     without PSRAM. No full-screen buffer: FreenoveUITask renders each frame
//     in horizontal bands (StripRenderer.h) by replaying the page draw per
//     band; no background layer, no animations (route as a highlighted
//     path, gauges and counters just update), 10-15 fps cap.
// Pages keep their Freenove layout constants as they were; the CYD values
// sit next to them under HMI_CYD, so the Freenove build is unchanged.

#if defined(HMI_PROFILE_CYD) && HMI_PROFILE_CYD
#define HMI_CYD 1
#else
#define HMI_CYD 0
#endif

namespace freenove_ui {
namespace profile {

#if HMI_CYD
constexpr int kScreenW = 320;
constexpr int kScreenH = 240;
constexpr int kHeaderH = 32;       // header rows (repeater, radio, clock)
constexpr int kTabBarH = 30;       // finger-sized tabs for resistive touch
constexpr int kPanelHeaderH = 22;  // panel title row (divider on its last row)
constexpr bool kAnimations = false;
#else
constexpr int kScreenW = 480;
constexpr int kScreenH = 320;
constexpr int kHeaderH = 40;
constexpr int kTabBarH = 24;
constexpr int kPanelHeaderH = 26;
constexpr bool kAnimations = true;
#endif
// First and last content row between the header and the tab bar.
constexpr int kContentY = kHeaderH + 2;
constexpr int kContentBottom = kScreenH - kTabBarH - 2;   // exclusive

}  // namespace profile
}  // namespace freenove_ui
