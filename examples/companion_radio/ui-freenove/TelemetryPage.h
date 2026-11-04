#pragma once

// TELEMETRY page of the Freenove visual head (pure C++11; host-tested and
// rendered to a preview by test/test_widgets.py). Real data only: every
// figure comes from the model (observer packets, the repeater's vitals);
// what is not known reads "--" in grey.
//
// Layout (HMI look of HOME), 480x320 landscape, header y 0..39:
//   gauges  (y 42..111) LAST SNR (newest observed packet; who it came
//           via), RSSI (same packet), AIRTIME (vitals; TX/min under it),
//           DUPES (duplicates / RX per minute, vitals): ring gauge + value
//   PACKETS / MIN (6,116 228x92): the last 60 minutes as a filled area
//           chart with a time axis (-60m .. now) and a count axis; note:
//           the repeater's RX/min
//   SNR WATERFALL (240,116 234x92): one column per observed packet,
//           newest right, its height and colour (Widgets.h ramp) the SNR
//   ACTIVE NODES (6,212 228x80): the 3 nodes heard directly with the most
//           packets: name, SNR history sparkline, last SNR
//   NOISE (240,212 116x80): the repeater's noise floor per minute (60 min)
//   VITALS (362,212 112x80, five rows, untitled): ESP-NOW peers, battery,
//           uptime, HEAP: the repeater's free heap now with a trend line of
//           the lowest free heap per 10 min over up to 8 h (a leak falls),
//           HEAP MIN: the lowest since its boot (observer health frame;
//           "--" from repeaters that do not send it)
// Frames: when the model changes (at most every kMinDataGapMs) and once a
// second for ages and staleness.
//
// CYD (HMI_CYD, 320x240): the four gauges (4..316, y 34..93), PACKETS / MIN
// (4,98 190x64), NOISE (198,98 118x64) and VITALS as one strip (4,166
// 312x42) of four columns: ESP-NOW (15 min), battery, uptime, HEAP (free now, the
// min since boot in its label row, the 8 h trend line next to the value). SNR WATERFALL and ACTIVE NODES are left out: the 2.8" screen
// has room for them only at an unreadable size.

#include <stdint.h>

#include "Page.h"

namespace freenove_ui {

class TelemetryPage : public Page {
 public:
  static const PanelRect kGauge[4];
  static const PanelRect kRatePanel, kSnrPanel, kActivePanel, kNoisePanel, kVitalsPanel;
  static const uint32_t kMinDataGapMs = 250;
  static const int kWaterfallColW = 2;   // px per packet
  static const int kActiveRows = 3;
  // Health frames come every 30 s: older than this reads stale.
  static const uint32_t kHealthStaleMs = 95000;

  TelemetryPage();
  const char* title() const override { return "TELEMETRY"; }
  int panels(const PanelRect*& out) const override;
  void drawBackground(FrameBuffer565& fb) override;
  void draw(FrameBuffer565& fb, uint32_t now_ms) override;
  bool wantsFrame(uint32_t now_ms) override;
  void onShow(uint32_t now_ms) override;

 private:
  void drawGauges(FrameBuffer565& fb, uint32_t now_ms) const;
  void drawRates(FrameBuffer565& fb, uint32_t now_ms) const;
  void drawWaterfall(FrameBuffer565& fb) const;
  void drawActive(FrameBuffer565& fb, uint32_t now_ms) const;
  void drawNoise(FrameBuffer565& fb, uint32_t now_ms) const;
  void drawVitals(FrameBuffer565& fb, uint32_t now_ms) const;
  void drawHeap(FrameBuffer565& fb, uint32_t now_ms) const;

  bool _drawn;
  uint32_t _drawnMs, _drawnGen;
};

}  // namespace freenove_ui
