#include "StubPages.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Packet.h>

#include "../HeadContactPolicy.h"
#include "HmiLogic.h"
#include "PageNav.h"

namespace freenove_ui {

namespace {

const char* const kTabLabels[PageSet::kCount] = {"HOME", "TELEMETRY", "MAP", "NODES", "HEAD"};

// Tab tiles: inset in their PageNav slot, 20 px tall from the bar's top.
constexpr int kTabInsetX = 3;
constexpr int kTabTileH = 22;
constexpr int kTabRadius = 5;

// Glow: the panel edge blurred with a Gaussian (sigma kGlowSigma px), peak
// kGlowPercent of kGlow, added to the gradient outside the panel.
constexpr float kGlowSigma = 4.0f;
constexpr int kGlowReach = 12;   // px beyond which the glow is < 1 level

// Signed distance from pixel centre (x, y) to the edge of a rounded panel
// (negative inside).
float panelDistance(const PanelRect& p, float x, float y) {
  const float r = (float)theme::kPanelRadius;
  const float hx = (float)p.w * 0.5f, hy = (float)p.h * 0.5f;
  const float cx = (float)p.x - 0.5f + hx, cy = (float)p.y - 0.5f + hy;
  const float qx = fabsf(x - cx) - (hx - r), qy = fabsf(y - cy) - (hy - r);
  const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
  const float inside = qx > qy ? qx : qy;
  return sqrtf(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - r;
}

}  // namespace

const char* tabLabel(int i) { return i >= 0 && i < PageSet::kCount ? kTabLabels[i] : ""; }

void drawBackdrop(FrameBuffer565& fb, int x, int y, int w, int h, const PanelRect* panels, int n) {
  const int W = fb.width(), H = fb.height();
  if (W <= 0 || H <= 0) return;
  const int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  const int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
  uint16_t* px = fb.rawPixels();
  const theme::Rgb8 a = theme::kBgNear, b = theme::kBgFar, g = theme::kGlow;
  const float peak = (float)theme::kGlowPercent / 100.0f;
  const float inv2s2 = 1.0f / (2.0f * kGlowSigma * kGlowSigma);
  for (int yy = y0; yy < y1; yy++) {
    for (int xx = x0; xx < x1; xx++) {
      const float t = 0.6f * (float)xx / (float)W + 0.4f * (float)yy / (float)H;
      float r = (float)a.r + ((float)b.r - (float)a.r) * t;
      float gg = (float)a.g + ((float)b.g - (float)a.g) * t;
      float bb = (float)a.b + ((float)b.b - (float)a.b) * t;
      for (int i = 0; i < n; i++) {
        const PanelRect& p = panels[i];
        if (xx < p.x - kGlowReach || xx >= p.x + p.w + kGlowReach || yy < p.y - kGlowReach ||
            yy >= p.y + p.h + kGlowReach)
          continue;
        if (xx >= p.x + kGlowReach && xx < p.x + p.w - kGlowReach && yy >= p.y + kGlowReach &&
            yy < p.y + p.h - kGlowReach)
          continue;   // deep inside: the panel covers it
        const float d = panelDistance(p, (float)xx, (float)yy) + 1.0f;   // edge line centre
        const float k = peak * expf(-d * d * inv2s2);
        r += (float)g.r * k;
        gg += (float)g.g * k;
        bb += (float)g.b * k;
      }
      px[yy * W + xx] = FrameBuffer565::dither(r, gg, bb, xx, yy);
    }
  }
}

void drawBackdrop(FrameBuffer565& fb, const PanelRect* panels, int n) {
  drawBackdrop(fb, 0, 0, fb.width(), fb.height(), panels, n);
}

void drawPanel(FrameBuffer565& fb, const PanelRect& p, const char* title, const char* note) {
  fb.fillRoundRect(p.x, p.y, p.w, p.h, theme::kPanelRadius, theme::kPanelFill);
  fb.strokeRoundRect(p.x, p.y, p.w, p.h, theme::kPanelRadius, theme::kPanelEdge);
  if (title != nullptr && title[0]) {
    fb.text(p.x + 10, p.y + 5, title, theme::kText, kFontTitle);
    fb.hline(p.x + 8, p.y + kPanelHeaderH - 1, p.w - 16, theme::kDivider);
  }
  if (note != nullptr && note[0]) fb.text(p.x + p.w - 8, p.y + 6, note, theme::kAccent, kFontSmall,
                                          FrameBuffer565::kRightTop);
}

void drawTabBar(FrameBuffer565& fb, int index, int count) {
  NavConfig cfg = defaultNavConfig();
  cfg.width = fb.width();
  cfg.height = fb.height();
  PageNav nav(count, 0, cfg);
  const int y0 = nav.barY();
  drawBackdrop(fb, 0, y0, fb.width(), cfg.barH, nullptr, 0);
  for (int i = 0; i < count; i++) {
    const int x = nav.tabX(i) + kTabInsetX;
    const int w = nav.tabX(i + 1) - nav.tabX(i) - 2 * kTabInsetX;
    const bool cur = i == index;
    fb.fillRoundRect(x, y0, w, kTabTileH, kTabRadius, cur ? theme::kTabActiveFill : theme::kTabFill);
    fb.strokeRoundRect(x, y0, w, kTabTileH, kTabRadius, cur ? theme::kTabActiveOutline : theme::kTabOutline);
    fb.text(x + w / 2, y0 + kTabTileH / 2, tabLabel(i), cur ? theme::kTabActiveText : theme::kTabText, kFontSmall,
            FrameBuffer565::kCentreMiddle);
  }
}

void drawPageChrome(FrameBuffer565& fb, Page& page, int index, int count) {
  const PanelRect* panels = nullptr;
  const int n = page.panels(panels);
  drawBackdrop(fb, panels, n);
  page.drawBackground(fb);
  drawTabBar(fb, index, count);
}

const AAFont& headerName(char* out, size_t cap, const char* repeaterName, int max_px) {
  if (cap == 0) return kFontName;
  char name[40];
  size_t n = 0;
  char shown[40];   // drawable characters only (D-DIN has no emoji)
  head_policy::displayName(repeaterName, nullptr, head_policy::kTypeRepeater, shown, sizeof(shown));
  const char* src = strcmp(shown, "rptr") != 0 ? shown : "REPEATER";
  for (; src[n] && n + 1 < sizeof(name); n++) {
    const char c = src[n];
    name[n] = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
  }
  name[n] = 0;
  // "M5 // NAME" at 18 px, then 16 px; then the name alone; then whole words
  // of it (cut at a space or hyphen), never mid-word.
  const AAFont* const fonts[2] = {&kFontName, &kFontTitle};
  for (int f = 0; f < 2; f++) {
    snprintf(out, cap, "M5 // %s", name);
    if (FrameBuffer565::textWidth(out, *fonts[f]) <= max_px) return *fonts[f];
  }
  for (;;) {
    snprintf(out, cap, "%s", name);
    if (FrameBuffer565::textWidth(out, kFontTitle) <= max_px) return kFontTitle;
    size_t cut = strlen(name);
    while (cut > 0 && name[cut - 1] != ' ' && name[cut - 1] != '-') cut--;
    while (cut > 0 && (name[cut - 1] == ' ' || name[cut - 1] == '-')) cut--;
    if (cut == 0) {   // one long word: the plain label
      snprintf(out, cap, "M5");
      return kFontTitle;
    }
    name[cut] = 0;
  }
}

const char* repeaterDisplayName(const PageContext& ctx) {
  const char* learned = ctx.model != nullptr ? ctx.model->repeaterName() : "";
  return learned[0] ? learned : ctx.repeater_name;
}

uint16_t radioColour(RadioState state, uint16_t live) {
  return state == kRadioLive ? live : (state == kRadioStale ? kDimText : theme::kWarn);
}

// Header (kHeaderH rows), every glyph >= 14 px:
//   x 8..208   repeater name (18 px bold) over the letter-spaced subline
//   x 210..234 observer-link icon (bottom row)
//   ..x 328    frequency (26 px), then MHz / SF.BW (14 px) from x 332
//   ..x 476    clock (24 px; uptime 16 px) over the date / "UPTIME"
void drawHeader(FrameBuffer565& fb, const PageContext& ctx, uint32_t now_ms) {
  const mesh::head::MeshModel* m = ctx.model;
  char name[48];
  const AAFont& nf = headerName(name, sizeof(name), repeaterDisplayName(ctx), kHeaderNameMaxPx);
  fb.text(8, &nf == &kFontName ? 1 : 3, name, theme::kText, nf);
  fb.text(8, 22, "OBSERVER \xB7 MESH \xB7 ESP-NOW", theme::kAccent, kFontSmall, FrameBuffer565::kLeftTop, 1);
  // Radio from the repeater's vitals.
  const mesh::observer::Vitals* v = m != nullptr ? m->vitals() : nullptr;
  char freq[32], detail[40], unused[48];
  const RadioState state =
      formatRepeaterRadio(unused, sizeof(unused), v, m != nullptr ? m->vitalsMs() : 0, now_ms);
  if (state == kRadioNone) {
    fb.text(405, 12, kNoRepeaterData, theme::kWarn, kFontSmall, FrameBuffer565::kRightTop);
  } else {
    formatRadioParts(freq, sizeof(freq), detail, sizeof(detail), v);
    fb.text(328, 1, freq, radioColour(state, theme::kAccent), kFontFreq, FrameBuffer565::kRightTop);
    if (state == kRadioStale) fb.text(332, 3, "STALE", theme::kWarn, kFontSmall);
    else fb.text(332, 3, "MHz", theme::kTextDim, kFontSmall);
    fb.text(332, 22, detail, theme::kTextDim, kFontSmall);
  }
  // Observer link: four arcs, lit by how fresh the newest observer frame is.
  const int lit = hmi::linkLevel(ctx.obs_seen, now_ms - ctx.obs_last_ms);
  const float ix = 222.0f, iy = 36.0f;
  for (int i = 0; i < 4; i++) {
    const float r = (float)(i + 1) * 3.0f;
    fb.aaArc(ix, iy, r + 0.6f, 1.5f, 225.0f, 315.0f, i < lit ? theme::kAccent : theme::kAccentDim);
  }
  fb.aaDisc(ix, iy, 1.4f, lit > 0 ? theme::kAccent : theme::kAccentDim);
  // Clock (or the smaller uptime).
  const bool uptime = ctx.clock_hm[0] == 'T';
  fb.text(476, uptime ? 4 : 0, ctx.clock_hm, theme::kText, uptime ? kFontTitle : kFontClock, FrameBuffer565::kRightTop);
  fb.text(476, 23, ctx.clock_date, theme::kTextDim, kFontSmall, FrameBuffer565::kRightTop);
}

const char* payloadTypeName(uint8_t type) {
  switch (type) {
    case PAYLOAD_TYPE_REQ: return "REQ";
    case PAYLOAD_TYPE_RESPONSE: return "RSP";
    case PAYLOAD_TYPE_TXT_MSG: return "TXT";
    case PAYLOAD_TYPE_ACK: return "ACK";
    case PAYLOAD_TYPE_ADVERT: return "ADV";
    case PAYLOAD_TYPE_GRP_TXT: return "GRP";
    case PAYLOAD_TYPE_GRP_DATA: return "GDT";
    case PAYLOAD_TYPE_ANON_REQ: return "ANON";
    case PAYLOAD_TYPE_PATH: return "PATH";
    case PAYLOAD_TYPE_TRACE: return "TRC";
    case PAYLOAD_TYPE_MULTIPART: return "MULTI";
    case PAYLOAD_TYPE_CONTROL: return "CTRL";
    case PAYLOAD_TYPE_OTA: return "OTA";
    case PAYLOAD_TYPE_RAW_CUSTOM: return "RAW";
    default: return "?";
  }
}

// --------------------------------------------------------------- name stubs

namespace {
const PanelRect kStubPanel = {6, kHeaderH + 2, 468, 292 - kHeaderH - 2};
}  // namespace

int NamedStubPage::panels(const PanelRect*& out) const {
  out = &kStubPanel;
  return 1;
}

void NamedStubPage::drawBackground(FrameBuffer565& fb) {
  drawPanel(fb, kStubPanel, _title, "coming next");
  const int cx = kStubPanel.x + kStubPanel.w / 2;
  fb.text(cx, 140, _title, theme::kAccent, kFontName, FrameBuffer565::kCentreTop, 2);
  if (_blurb != nullptr) fb.text(cx, 168, _blurb, theme::kTextDim, kFontSmall, FrameBuffer565::kCentreTop);
}

// ----------------------------------------------------------------- page set

PageSet::PageSet() {
  _home.setCard(&_card);
  _map.setCard(&_card);
  _nodes.setCard(&_card);
  _pages[kHome] = &_home;
  _pages[kTelemetry] = &_telemetry;
  _pages[kMap] = &_map;
  _pages[kNodes] = &_nodes;
  _pages[kHead] = &_head;
}

void PageSet::attach(const PageContext* ctx) {
  for (int i = 0; i < kCount; i++) _pages[i]->attach(ctx);
  _card.attach(ctx);
}

}  // namespace freenove_ui
