#include "TelemetryPage.h"

#include <stdio.h>
#include <string.h>

#include "RadioLabel.h"
#include "StubPages.h"
#include "Widgets.h"

namespace freenove_ui {

using mesh::head::FeedItem;
using mesh::head::MeshModel;
using mesh::head::NodeInfo;

const PanelRect TelemetryPage::kGauge[4] = {{6, 42, 114, 70}, {124, 42, 114, 70}, {242, 42, 114, 70}, {360, 42, 114, 70}};
const PanelRect TelemetryPage::kRatePanel = {6, 116, 228, 92};
const PanelRect TelemetryPage::kSnrPanel = {240, 116, 234, 92};
const PanelRect TelemetryPage::kActivePanel = {6, 212, 228, 80};
const PanelRect TelemetryPage::kNoisePanel = {240, 212, 116, 80};
const PanelRect TelemetryPage::kVitalsPanel = {362, 212, 112, 80};

namespace {

const char* const kGaugeTitles[4] = {"LAST SNR", "RSSI", "AIRTIME", "DUPES"};
const float kStartDeg = 135.0f, kSweepDeg = 270.0f;
const int kGaugeCx = 27, kGaugeCy = 47;   // ring centre in its panel
const float kGaugeR = 19.0f, kGaugeThick = 4.5f;

// Chart plot boxes (screen px).
const int kRateX = 42, kRateY = 146, kRateW = 182, kRateH = 40;
const int kWfX = 276, kWfY = 146, kWfW = 188, kWfH = 54;
const int kNfX = 250, kNfY = 242, kNfW = 96, kNfH = 40;

uint16_t rssiColour(int rssi) {
  if (rssi >= -90) return theme::kSigStrong;
  if (rssi >= -105) return theme::kSigGood;
  if (rssi >= -115) return theme::kSigWeak;
  return theme::kSigPoor;
}

// A round axis top for a count (>= 4): 4, 5, 10, 20, 25, 50, 100, ...
int niceMax(int v) {
  static const int kSteps[] = {4, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000, 10000, 20000, 50000};
  for (size_t i = 0; i < sizeof(kSteps) / sizeof(kSteps[0]); i++)
    if (v <= kSteps[i]) return kSteps[i];
  return 65535;
}

void formatUptime(char* out, size_t cap, uint32_t s) {
  if (s >= 86400u) snprintf(out, cap, "%lud %luh", (unsigned long)(s / 86400u), (unsigned long)(s % 86400u / 3600u));
  else if (s >= 3600u) snprintf(out, cap, "%luh %lum", (unsigned long)(s / 3600u), (unsigned long)(s % 3600u / 60u));
  else snprintf(out, cap, "%lum", (unsigned long)(s / 60u));
}

}  // namespace

TelemetryPage::TelemetryPage() : _drawn(false), _drawnMs(0), _drawnGen(0) {}

int TelemetryPage::panels(const PanelRect*& out) const {
  static const PanelRect kAll[9] = {kGauge[0], kGauge[1], kGauge[2], kGauge[3], kRatePanel,
                                    kSnrPanel, kActivePanel, kNoisePanel, kVitalsPanel};
  out = kAll;
  return 9;
}

void TelemetryPage::onShow(uint32_t now_ms) {
  (void)now_ms;
  _drawn = false;
}

bool TelemetryPage::wantsFrame(uint32_t now_ms) {
  if (!_drawn) return true;
  const uint32_t since = now_ms - _drawnMs;
  if (_ctx != nullptr && _ctx->data_gen != _drawnGen && since >= kMinDataGapMs) return true;
  return since >= 1000;
}

void TelemetryPage::drawBackground(FrameBuffer565& fb) {
  for (int i = 0; i < 4; i++) {
    drawPanel(fb, kGauge[i], kGaugeTitles[i], nullptr);
    widgets::ringGaugeTrack(fb, (float)(kGauge[i].x + kGaugeCx), (float)(kGauge[i].y + kGaugeCy), kGaugeR, kGaugeThick,
                            kStartDeg, kSweepDeg);
  }
  drawPanel(fb, kRatePanel, "PACKETS / MIN", nullptr);
  // Axes of the rate chart: time ticks along the bottom.
  fb.hline(kRateX, kRateY + kRateH, kRateW, theme::kDivider);
  fb.text(kRateX, kRateY + kRateH + 2, "-60m", theme::kTextDim, kFontSmall);
  fb.text(kRateX + kRateW / 2, kRateY + kRateH + 2, "-30m", theme::kTextDim, kFontSmall, FrameBuffer565::kCentreTop);
  fb.text(kRateX + kRateW, kRateY + kRateH + 2, "now", theme::kTextDim, kFontSmall, FrameBuffer565::kRightTop);
  drawPanel(fb, kSnrPanel, "SNR WATERFALL", "newest right");
  fb.fillRect(kWfX, kWfY, kWfW, kWfH, theme::kOdoFill);
  char lab[8];
  snprintf(lab, sizeof(lab), "%+d", widgets::kWaterfallMaxQ4 / 4);
  fb.text(kWfX - 4, kWfY - 3, lab, theme::kTextDim, kFontSmall, FrameBuffer565::kRightTop);
  snprintf(lab, sizeof(lab), "%+d", widgets::kWaterfallMinQ4 / 4);
  fb.text(kWfX - 4, kWfY + kWfH - 13, lab, theme::kTextDim, kFontSmall, FrameBuffer565::kRightTop);
  const int zy = widgets::sparkY(0, widgets::kWaterfallMinQ4, widgets::kWaterfallMaxQ4, kWfY, kWfH);
  fb.text(kWfX - 4, zy - 8, "0", theme::kTextDim, kFontSmall, FrameBuffer565::kRightTop);
  drawPanel(fb, kActivePanel, "ACTIVE NODES", "SNR dB");
  drawPanel(fb, kNoisePanel, "NOISE", nullptr);
  drawPanel(fb, kVitalsPanel, "VITALS", nullptr);
  static const char* const kVit[3] = {"ESP-NOW 15m", "BATT", "UPTIME"};
  for (int i = 0; i < 3; i++) fb.text(kVitalsPanel.x + 8, 240 + i * 17, kVit[i], theme::kTextDim, kFontSmall);
}

void TelemetryPage::draw(FrameBuffer565& fb, uint32_t now_ms) {
  _drawn = true;
  _drawnMs = now_ms;
  _drawnGen = _ctx != nullptr ? _ctx->data_gen : 0;
  const MeshModel* m = _ctx != nullptr ? _ctx->model : nullptr;
  if (m == nullptr || !m->ok()) {
    fb.text(240, 150, "MODEL UNAVAILABLE", theme::kBad, kFontBody, FrameBuffer565::kCentreTop);
    return;
  }
  drawGauges(fb, now_ms);
  drawRates(fb, now_ms);
  drawWaterfall(fb);
  drawActive(fb, now_ms);
  drawNoise(fb, now_ms);
  drawVitals(fb, now_ms);
}

void TelemetryPage::drawGauges(FrameBuffer565& fb, uint32_t now_ms) const {
  const MeshModel& m = *_ctx->model;
  const FeedItem* last = nullptr;
  for (size_t i = 0; i < m.feedCount() && last == nullptr; i++) {
    const FeedItem* f = m.feed(i);
    if (f != nullptr && f->observed) last = f;
  }
  const mesh::observer::Vitals* v = m.vitals();
  const bool live = v != nullptr && (uint32_t)(now_ms - m.vitalsMs()) < kRadioStaleMs;
  for (int i = 0; i < 4; i++) {
    const PanelRect& p = kGauge[i];
    const float cx = (float)(p.x + kGaugeCx), cy = (float)(p.y + kGaugeCy), r = kGaugeR, thick = kGaugeThick;
    char val[16] = "--", sub[32] = "";
    bool has = false;
    float x = 0, lo = 0, hi = 1;
    uint16_t c = theme::kAccent;
    bool fresh = true;
    switch (i) {
      case 0:
        lo = -20.0f; hi = 15.0f;
        snprintf(sub, sizeof(sub), "dB");
        if (last != nullptr) {
          has = true;
          x = (float)last->snr_q4 / 4.0f;
          widgets::formatSnr(val, sizeof(val), last->snr_q4);
          c = linkColour(last->snr_q4);
          fresh = (uint32_t)(now_ms - last->ms) < kRadioStaleMs;
          char age[12];
          widgets::formatAge(age, sizeof(age), now_ms - last->ms);
          snprintf(sub, sizeof(sub), "dB \xB7 %s", age);
        }
        break;
      case 1:
        lo = -130.0f; hi = -40.0f;
        snprintf(sub, sizeof(sub), "dBm");
        if (last != nullptr && last->rssi != 0) {
          has = true;
          x = (float)last->rssi;
          snprintf(val, sizeof(val), "%d", (int)last->rssi);
          c = rssiColour(last->rssi);
          fresh = (uint32_t)(now_ms - last->ms) < kRadioStaleMs;
        }
        break;
      case 2:
        lo = 0.0f; hi = 10.0f;
        snprintf(sub, sizeof(sub), "TX --/min");
        if (v != nullptr) {
          has = true;
          x = (float)v->airtime_permille / 10.0f;
          snprintf(val, sizeof(val), "%u.%u%%", (unsigned)(v->airtime_permille / 10), (unsigned)(v->airtime_permille % 10));
          c = v->airtime_permille < 50 ? theme::kGood : (v->airtime_permille < 100 ? theme::kWarn : theme::kBad);
          snprintf(sub, sizeof(sub), "TX %u/min", (unsigned)v->tx_per_min);
          fresh = live;
        }
        break;
      default:
        lo = 0.0f; hi = 50.0f;
        snprintf(sub, sizeof(sub), "--/min");
        if (v != nullptr) snprintf(sub, sizeof(sub), "%u/min", (unsigned)v->dup_per_min);
        // A dupe % only over a repeater RX that is non-zero and agrees with
        // what the head observed (Widgets.h pickRx); else "--".
        if (v != nullptr && widgets::rxFor(m).dup_ok) {
          has = true;
          const unsigned total = (unsigned)v->rx_per_min;
          const unsigned pct = ((unsigned)v->dup_per_min * 100u + total / 2u) / total;
          x = (float)pct;
          snprintf(val, sizeof(val), "%u%%", pct > 999u ? 999u : pct);
          c = pct < 20 ? theme::kGood : (pct < 40 ? theme::kWarn : theme::kBad);
          snprintf(sub, sizeof(sub), "%u/min", (unsigned)v->dup_per_min);
          fresh = live;
        }
        break;
    }
    if (!fresh) c = kDimText;
    widgets::ringGauge(fb, cx, cy, r, thick, kStartDeg, kSweepDeg, has, x, lo, hi, c);
    fb.text(p.x + p.w - 8, p.y + 27, val, has && fresh ? theme::kText : kDimText, kFontDigits, FrameBuffer565::kRightTop);
    fb.text(p.x + p.w - 8, p.y + 51, sub, theme::kTextDim, kFontSmall, FrameBuffer565::kRightTop);
  }
}

void TelemetryPage::drawRates(FrameBuffer565& fb, uint32_t now_ms) const {
  const MeshModel& m = *_ctx->model;
  uint16_t rates[60];
  m.ratesPerMinute(rates);
  int16_t v[60];
  int peak = 0;
  for (int i = 0; i < 60; i++) {
    v[i] = (int16_t)(rates[i] > 32767 ? 32767 : rates[i]);
    if (v[i] > peak) peak = v[i];
  }
  const int top = niceMax(peak);
  char lab[8];
  snprintf(lab, sizeof(lab), "%d", top);
  fb.text(kRateX - 4, kRateY - 7, lab, theme::kTextDim, kFontSmall, FrameBuffer565::kRightTop);
  fb.text(kRateX - 4, kRateY + kRateH - 12, "0", theme::kTextDim, kFontSmall, FrameBuffer565::kRightTop);
  fb.dashedLine(kRateX, kRateY, kRateX + kRateW - 1, kRateY, theme::kDivider, 2, 4, 0);
  fb.dashedLine(kRateX, kRateY + kRateH / 2, kRateX + kRateW - 1, kRateY + kRateH / 2, theme::kDivider, 2, 4, 0);
  widgets::sparkline(fb, kRateX, kRateY, kRateW, kRateH, v, 60, INT16_MIN, 0, top, theme::kAccent, 70);
  // Note: the repeater's RX/min (vitals), or the head's own observed count
  // ("head") when the vitals contradict it.
  const mesh::observer::Vitals* vt = m.vitals();
  const widgets::RxPick rx = widgets::rxFor(m);
  const bool live = rx.from_head || (vt != nullptr && (uint32_t)(now_ms - m.vitalsMs()) < kRadioStaleMs);
  char note[32];
  if (!rx.known) snprintf(note, sizeof(note), "RX --");
  else if (rx.from_head) snprintf(note, sizeof(note), "RX %u/min head", rx.rx);
  else snprintf(note, sizeof(note), "RX %u/min", rx.rx);
  fb.text(kRatePanel.x + kRatePanel.w - 8, kRatePanel.y + 6, note, live ? theme::kAccent : kDimText, kFontSmall,
          FrameBuffer565::kRightTop);
}

void TelemetryPage::drawWaterfall(FrameBuffer565& fb) const {
  const MeshModel& m = *_ctx->model;
  const int cols = kWfW / kWaterfallColW;
  int col = cols - 1;
  const int zy = widgets::sparkY(0, widgets::kWaterfallMinQ4, widgets::kWaterfallMaxQ4, kWfY, kWfH);
  fb.dashedLine(kWfX, zy, kWfX + kWfW - 1, zy, theme::kDivider, 2, 4, 0);
  int drawn = 0;
  for (size_t i = 0; i < m.feedCount() && col >= 0; i++) {
    const FeedItem* f = m.feed(i);
    if (f == nullptr) break;
    if (!f->observed) continue;
    const int x = kWfX + col * kWaterfallColW;
    const int y = widgets::sparkY(f->snr_q4, widgets::kWaterfallMinQ4, widgets::kWaterfallMaxQ4, kWfY, kWfH);
    const uint16_t c = widgets::waterfallColour(f->snr_q4);
    fb.fillRectAlpha(x, y, kWaterfallColW, kWfY + kWfH - y, c, 70);
    fb.fillRect(x, y, kWaterfallColW, 2, c);
    col--;
    drawn++;
  }
  if (drawn == 0)
    fb.text(kWfX + kWfW / 2, kWfY + kWfH / 2, "no packets observed yet", kDimText, kFontSmall,
            FrameBuffer565::kCentreMiddle);
}

void TelemetryPage::drawActive(FrameBuffer565& fb, uint32_t now_ms) const {
  const MeshModel& m = *_ctx->model;
  // The kActiveRows nodes heard directly within the hour with the most packets.
  const NodeInfo* top[kActiveRows] = {nullptr, nullptr, nullptr};
  for (size_t i = 0; i < m.nodeCount(); i++) {
    const NodeInfo* n = m.node(i);
    if (n->snr_hist[0] == mesh::head::SNR_NONE || (uint32_t)(now_ms - n->last_heard_ms) >= 3600000u) continue;
    for (int k = 0; k < kActiveRows; k++) {
      if (top[k] == nullptr || n->packets > top[k]->packets) {
        for (int j = kActiveRows - 1; j > k; j--) top[j] = top[j - 1];
        top[k] = n;
        break;
      }
    }
  }
  if (top[0] == nullptr) {
    fb.text(kActivePanel.x + 10, 248, "None heard directly in the last hour", kDimText, kFontSmall);
    return;
  }
  for (int k = 0; k < kActiveRows && top[k] != nullptr; k++) {
    const NodeInfo& n = *top[k];
    const int y = 239 + k * 17;
    char name[40], shown[24];
    widgets::nodeName(name, sizeof(name), n);
    widgets::fitText(shown, sizeof(shown), name, kFontSmall, 80);
    const uint16_t c = linkColour(n.last_snr_q4);
    fb.text(kActivePanel.x + 8, y, shown, c, kFontSmall);
    int16_t v[32];
    for (int i = 0; i < 32; i++) v[i] = n.snr_hist[31 - i];
    int lo = 0, hi = 0;
    widgets::sparkRange(v, 32, mesh::head::SNR_NONE, 4 * 4, lo, hi);
    widgets::sparkline(fb, 98, y + 2, 84, 13, v, 32, mesh::head::SNR_NONE, lo, hi, c, 50);
    char snr[12];
    widgets::formatSnr(snr, sizeof(snr), n.last_snr_q4);
    fb.text(kActivePanel.x + kActivePanel.w - 8, y, snr, theme::kText, kFontSmall, FrameBuffer565::kRightTop);
  }
}

void TelemetryPage::drawNoise(FrameBuffer565& fb, uint32_t now_ms) const {
  const MeshModel& m = *_ctx->model;
  const mesh::observer::Vitals* v = m.vitals();
  const bool live = v != nullptr && (uint32_t)(now_ms - m.vitalsMs()) < kRadioStaleMs;
  char note[16];
  if (v != nullptr) snprintf(note, sizeof(note), "%d dBm", (int)v->noise_floor);
  else snprintf(note, sizeof(note), "-- dBm");
  fb.text(kNoisePanel.x + kNoisePanel.w - 8, kNoisePanel.y + 6, note, live ? theme::kAccent : kDimText, kFontSmall,
          FrameBuffer565::kRightTop);
  int16_t nf[60];
  bool has[60];
  m.noiseFloorPerMinute(nf, has);
  for (int i = 0; i < 60; i++) if (!has[i]) nf[i] = INT16_MIN;
  int lo = 0, hi = 0;
  if (widgets::sparkRange(nf, 60, INT16_MIN, 6, lo, hi) == 0) {
    fb.text(kNoisePanel.x + kNoisePanel.w / 2, kNfY + kNfH / 2, "no vitals", kDimText, kFontSmall,
            FrameBuffer565::kCentreMiddle);
    return;
  }
  char a[16], b[16];
  snprintf(a, sizeof(a), "%d", hi);
  snprintf(b, sizeof(b), "%d", lo);
  fb.text(kNfX + kNfW, kNfY - 4, a, theme::kTextDim, kFontSmall, FrameBuffer565::kRightTop);
  fb.text(kNfX + kNfW, kNfY + kNfH - 10, b, theme::kTextDim, kFontSmall, FrameBuffer565::kRightTop);
  widgets::sparkline(fb, kNfX, kNfY + 8, kNfW - 30, kNfH - 10, nf, 60, INT16_MIN, lo, hi,
                     live ? theme::kSigWeak : kDimText, 60);
}

void TelemetryPage::drawVitals(FrameBuffer565& fb, uint32_t now_ms) const {
  const MeshModel& m = *_ctx->model;
  const mesh::observer::Vitals* v = m.vitals();
  const bool live = v != nullptr && (uint32_t)(now_ms - m.vitalsMs()) < kRadioStaleMs;
  char vals[3][16];
  if (v != nullptr) {
    snprintf(vals[0], sizeof(vals[0]), "%u", (unsigned)v->espnow_nodes);
    if (v->batt_mv > 0) snprintf(vals[1], sizeof(vals[1]), "%u.%02uV", (unsigned)(v->batt_mv / 1000), (unsigned)(v->batt_mv % 1000 / 10));
    else snprintf(vals[1], sizeof(vals[1]), "--");
    formatUptime(vals[2], sizeof(vals[2]), v->uptime_s);
  } else {
    for (int i = 0; i < 3; i++) snprintf(vals[i], sizeof(vals[i]), "--");
  }
  for (int i = 0; i < 3; i++)
    fb.text(kVitalsPanel.x + kVitalsPanel.w - 8, 240 + i * 17, vals[i], live ? theme::kText : kDimText, kFontSmall,
            FrameBuffer565::kRightTop);
}

}  // namespace freenove_ui
