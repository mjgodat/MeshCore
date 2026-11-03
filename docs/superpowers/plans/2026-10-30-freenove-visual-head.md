# Freenove 3.5" Visual Repeater Head Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Freenove ESP32-S3 3.5" ESP-NOW head that shows, live and fast, what the ThinkNode M5 repeater hears (topology, feed, telemetry, map, nodes) and keeps the CYD head's compose/admin/presets, fed by a new opt-in M5 observer stream.

**Architecture:** M5 repeater gains `espnow.observer`: per-packet raw bytes + SNR/RSSI from `logRxRaw` and periodic vitals, sent as collision-safe ESP-NOW frames. The head (MeshCore companion on ESPNOWRadio, like the CYD head) demuxes observer frames into a pure-C++ data model (nodes, packet ring, histories, attribution, geo). Pages render into an 8-bit PSRAM framebuffer pushed to the ST77922 over QSPI DMA with per-row change detection (our own implementation of SquatchWatch's speed techniques — no copied code).

**Tech Stack:** C++17, ESP32-S3 Arduino (PlatformIO), IDF `spi_master` QSPI, ESP-NOW (LR, ch 6), Python unittest + g++ host harnesses (repo pattern), WSL ASan as test authority.

**Spec:** `docs/superpowers/specs/2026-10-30-freenove-head-visual-design.md` (mockups: `.superpowers/brainstorm/46706-1793343834/content/visual-direction.html`, `layout-v2.html` — copy into the plan workspace in Task 0).

## Global Constraints

- Branch `feature/freenove-head` in new worktree `C:\Users\Micha\mc_freenove`, based on a refreshed `integration/m5-combined` (Task 0). Local commits allowed; never push without the owner.
- Code must stay MIT-compatible: do not copy SquatchWatch-CYD (GPLv3) or WiChat display code; write the ST77922 driver from Freenove's vendor driver / datasheet.
- Board: Freenove FNK0104N, ESP32-S3, 16 MB flash, OPI PSRAM (`board_build.arduino.memory_type = qio_opi`), native USB (COM6 last seen). ST77922 QSPI: CS 10, SCLK 12, D0 11, D1 13, D2 14, D3 9, BL 41, SPI2_HOST, 80 MHz; panel 320×480 portrait → landscape 480×320 via transpose on push; RGB565 byte-swapped on wire. Touch: integrated, I²C 0x55.
- Framebuffer: RGB332, 480×320 = 153,600 B, two buffers in PSRAM; 256-entry RGB332→RGB565 LUT; rows unchanged since last push are not sent; DMA pushes overlap drawing. Target ≥30 fps on animated pages.
- ESP-NOW channel 6, LR, raw bridge format (`fmt=raw`). Observer frames must not collide with MeshCore raw frames, raw fragments (`FE 4D 43 46`), or WiChat frames.
- Observer feed off by default (`set espnow.observer on|off`, persisted like `espnow.peer`); when off, M5 on-air/bridge behavior is byte-identical to today; M5 must still fit its app slot.
- Signal attribution: SNR/RSSI → last-hop transmitter; 0-hop → sender; else last path hash ("heard via"), resolved to a node only when unique; ambiguous stays a hash.
- Distances/bearings from the repeater's lat/lon (from observer vitals). The head is not drawn as a node.
- Pages: HOME (GRAPH↔GEO topology + live feed + vitals bar), TELEMETRY, MAP, NODES, HEAD (CYD compose/admin/presets), node detail card on tap.
- One PlatformIO process at a time per checkout; hardware steps (flashing, ports) are done by the controller; announce before flashing the M5.

## Review Focus

1. Observer flood: a busy mesh (≥10 packets/s) must not starve ESP-NOW mesh traffic or vitals — test `observer_rate_limit_drops_oldest_keeps_vitals` (Task 2).
2. Malformed/truncated observer frames or MeshCore packets must never crash the head or corrupt the model — tests `decode_rejects_truncated` (Task 1) and `model_ignores_garbage_packets` (Task 4).
3. Node table full (more nodes than capacity) evicts least-recently-heard, never the repeater's last-hop neighbors currently animating — test `node_table_evicts_lru` (Task 4).
4. Nodes with no GPS / repeater with no GPS: GEO/MAP must degrade (list-only, "no repeater position") rather than plot at 0,0 — tests `geo_missing_repeater_position` and `node_without_gps_not_plotted` (Task 4).
5. A frame where nothing changed must push zero rows (idle CPU/bus near zero), and a full-screen change must still complete within the frame budget — tests `push_skips_unchanged_rows` (Task 5) and the fps measurement in Task 6.

---

### Task 0: Worktree and baseline

- [ ] **Step 1:** Refresh `integration/m5-combined`: merge the latest `feature/radio-presets` (post-upstream-merge, CI-green) and, once reviewed, `feature/espnow-repeat`; resolve conflicts; run WSL `test_radio_presets test_head_helpers test_cardputer_chat test_espnow_repeat`.
Expected: OK.
- [ ] **Step 2:** `git worktree add -b feature/freenove-head C:\Users\Micha\mc_freenove integration/m5-combined`; copy the spec, this plan and both mockups into the new worktree's plan workspace.
- [ ] **Step 3:** Build `CYD_ESPNOW_Head_companion_usb` and `ThinkNode_M5_Repeater` in the new worktree. Expected: SUCCESS (baseline sizes recorded in the ledger).

---

### Task 1: Observer wire protocol (pure codec)

**Files:**
- Create: `src/helpers/bridges/ObserverFrame.h` (header-only, no Arduino)
- Test: `test/test_observer_frame.py`

**Interfaces — Produces:**
```cpp
namespace mesh { namespace observer {
constexpr uint8_t MAGIC0 = 0x4F, MAGIC1 = 0x42;   // 'O','B' — must differ from raw/fragment/WiChat headers
constexpr uint8_t VERSION = 1;
enum class Kind : uint8_t { Packet = 1, Vitals = 2 };
enum class Disposition : uint8_t { Unknown = 0, Forwarded = 1, Duplicate = 2, Dropped = 3 };
struct PacketRecord { uint32_t rx_ms; int8_t snr_q4; int16_t rssi; Disposition disp; uint8_t len; uint8_t raw[184]; };
struct Vitals { uint32_t uptime_s; int16_t noise_floor; uint16_t rx_per_min, tx_per_min, dup_per_min;
                uint16_t airtime_permille; uint8_t espnow_nodes; uint16_t batt_mv;
                int32_t lat_e6, lon_e6; bool has_pos; float freq; float bw; uint8_t sf, cr; uint8_t preset_slot; };
size_t encodePacket(const PacketRecord&, uint8_t* out, size_t cap);   // ≤ 250 B (ESP-NOW max)
size_t encodeVitals(const Vitals&, uint8_t* out, size_t cap);
bool   isObserverFrame(const uint8_t* buf, size_t len);              // magic + version + kind check
bool   decodePacket(const uint8_t* buf, size_t len, PacketRecord& out);
bool   decodeVitals(const uint8_t* buf, size_t len, Vitals& out);
}}
```
Little-endian fixed layout; header = MAGIC0 MAGIC1 VERSION KIND.

- [ ] **Step 1: Failing tests** — `roundtrip_packet` (184-byte raw, snr −7.25 dB → snr_q4 −29, rssi −104), `roundtrip_vitals` (with and without position), `decode_rejects_truncated` (every prefix length < full → false), `rejects_wrong_magic_and_version`, `no_collision_with_meshcore_raw` (a valid MeshCore raw packet whose first two bytes are any header byte + path → `isObserverFrame` false; a raw fragment starting `FE 4D 43 46` → false; a WiChat frame with magic 0x5743 at offset 24 → false), `fits_espnow_mtu` (max encode ≤ 250).
- [ ] **Step 2: Run** `python -m unittest test_observer_frame -v` (from `test/`, WinLibs g++ on PATH) — FAIL (missing header).
- [ ] **Step 3: Implement** `ObserverFrame.h`. Verify MeshCore raw header bytes can never be `0x4F 0x42` as a valid first-two-byte pair given header bit layout; if they can, pick a magic that cannot and update the test.
- [ ] **Step 4: Run** — PASS; also in WSL (ASan).
- [ ] **Step 5: Commit** `feat(observer): ESP-NOW observer frame codec`.

---

### Task 2: M5 observer feed

**Files:**
- Create: `src/helpers/ObserverSettings.h/.cpp` (persist on/off like `ESPNowPeerSettings`), `src/helpers/bridges/ObserverRateLimiter.h` (pure)
- Modify: `src/helpers/bridges/ESPNowBridge.h/.cpp` (`bool sendObserverFrame(const uint8_t*, size_t)`), `examples/simple_repeater/MyMesh.cpp` (`logRxRaw` at ~1262 emits a PacketRecord when enabled; a 5 s vitals timer in `loop`), `src/helpers/CommonCLI.cpp` (`get/set espnow.observer on|off`, admin-only), `docs/espnow_bridge_setup.md`, `docs/cli_commands.md`
- Test: `test/test_observer_feed.py`

**Interfaces — Consumes:** Task 1 codec. **Produces:** `set espnow.observer on|off` (default off).

Rate limiter: token bucket for packet records (default 20/s, burst 40); vitals bypass the bucket; when the bridge TX queue is non-empty for mesh traffic, packet records are dropped first (mesh frames always win).

- [ ] **Step 1: Failing tests** — `observer_off_by_default`, `observer_setting_persists` (round trip through the settings file), `observer_rate_limit_drops_oldest_keeps_vitals` (60 records in 1 s → ≤ 40 sent; a vitals record in the same second still sent), `mesh_frames_preempt_observer` (pending mesh frame → observer record dropped, mesh frame sent), contract: `logRxRaw` calls the observer only when enabled; region/filter-manager allow-lists don't include `espnow.observer`.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement.** Disposition: `Unknown` unless cheaply known from `logRx` for the same packet hash within the same loop pass (designer: correlate by hash; leave Unknown if not).
- [ ] **Step 4: Run** tests (Windows + WSL) — PASS; refresh `cli_reply_text` hashes if the fixture tracks CommonCLI.cpp. Build `ThinkNode_M5_Repeater` — SUCCESS, flash % recorded; with observer off, confirm no change in `get espnow.stats tx` behavior.
- [ ] **Step 5: Commit** `feat(observer): opt-in M5 observer feed over ESP-NOW`.

---

### Task 3: Freenove board bring-up (display, touch, memory fit)

**Files:**
- Create: `variants/freenove_s3_35_head/` — `platformio.ini` (env `Freenove_S3_35_Head_companion_usb`, extends the CYD head's flags: `MESH_PRIMARY_ESPNOW=1`, `MESH_ESPNOW_CHANNEL=6`, `UI_REPEATER_HEAD=1`, CYD-head trims, `-DBOARD_HAS_PSRAM`, `ARDUINO_USB_CDC_ON_BOOT=1`, 16 MB, a ≥3 MB app partition), `target.h/.cpp`, `ST77922Panel.h/.cpp` (IDF QSPI init/sleep/backlight, `pushRows(const uint16_t* rgb565, int y0, int rows)` with landscape→portrait transpose), `St77922Touch.h/.cpp` (I²C 0x55 read → x,y in landscape coords), `README.md`
- Test: `test/test_freenove_geometry.py` (pure transpose/coord mapping)

**Interfaces — Produces:** `class St77922Panel { bool begin(); void pushRows(const uint16_t* px, int y0, int rows); void setBacklight(uint8_t); }`, `class St77922Touch { bool begin(); bool read(int& x, int& y); }`, `toPanel(x,y)` mapping helpers.

- [ ] **Step 1: Failing tests** — `landscape_to_portrait_corners` (landscape (0,0),(479,0),(0,319),(479,319) map to the 4 panel corners per the vendor orientation), `touch_coords_landscape`.
- [ ] **Step 2: Run** — FAIL. **Step 3: Implement** from the Freenove vendor driver (cite file/function names in comments; no SquatchWatch/WiChat code).
- [ ] **Step 4:** Tests PASS. Build the env — SUCCESS; record internal RAM, PSRAM and flash use (companion + HEAD screens + 2×153.6 KB PSRAM framebuffers must fit — if not, stop and report).
- [ ] **Step 5 (controller, hardware):** flash COM6; boot shows a test pattern (8 colour bars + corner markers in landscape); touching each corner prints coordinates over USB within ±10 px.
- [ ] **Step 6: Commit** `feat(freenove): ST77922 QSPI panel, touch and head env`.

---

### Task 4: Head data model (pure)

**Files:**
- Create: `examples/companion_radio/ui-freenove/MeshModel.h/.cpp` (no Arduino), `examples/companion_radio/ui-freenove/GeoMath.h`
- Test: `test/test_mesh_model.py`

**Interfaces — Consumes:** Task 1 `PacketRecord`, `Vitals`. **Produces:**
```cpp
struct NodeInfo { uint8_t pub_prefix[4]; uint16_t path_hash; char name[24]; uint8_t type; bool has_pos; int32_t lat_e6, lon_e6;
                  uint32_t last_heard_ms; int8_t last_snr_q4; int16_t last_rssi; uint8_t hops; uint32_t packets; int8_t snr_hist[32]; };
struct FeedItem { uint32_t ms; uint8_t payload_type; uint8_t hops; int8_t snr_q4; uint16_t via_hash; char label[28]; };
class MeshModel {
 public:
  explicit MeshModel(size_t node_capacity = 256, size_t ring_capacity = 512);   // storage allocated by caller-provided allocator (PSRAM on device)
  void onObserverPacket(const observer::PacketRecord&);   // parses raw MeshCore packet
  void onVitals(const observer::Vitals&);
  void onBridgedPacket(const uint8_t* raw, size_t len);   // packets the head already receives
  size_t nodeCount() const; const NodeInfo* node(size_t i) const; const NodeInfo* findByHash(uint16_t) const;
  const FeedItem* feed(size_t newest_index) const;
  bool repeaterPosition(int32_t& lat, int32_t& lon) const;
  void ratesPerMinute(uint16_t out[60]) const;   // last 60 minutes
  // path of the most recent packet for animation: node indices from origin to repeater
  size_t lastPath(uint16_t* hashes, size_t cap) const;
};
double distanceKm(int32_t lat1_e6, int32_t lon1_e6, int32_t lat2_e6, int32_t lon2_e6);   // haversine
double bearingDeg(int32_t lat1_e6, int32_t lon1_e6, int32_t lat2_e6, int32_t lon2_e6);
```
- [ ] **Step 1: Failing tests** — `advert_creates_node_with_gps` (a real MeshCore advert with name "Jupiter", lat 47.606209 lon −122.332069 → node has_pos, name), `snr_attributed_to_last_hop` (3-hop group text → sample on last path hash; 0-hop advert → sender), `ambiguous_hash_stays_hash` (two known repeaters sharing a 2-byte hash → no node resolved), `node_table_evicts_lru`, `model_ignores_garbage_packets` (random bytes, truncated headers → no crash, no node), `geo_missing_repeater_position` (no vitals position → repeaterPosition false), `node_without_gps_not_plotted` (has_pos false), `rates_per_minute_window` (60-minute rollover), `distance_bearing` (Seattle→Portland ≈ 233.7 km, bearing ≈ 191°; ±0.5 km/±1°).
- [ ] **Step 2: Run** — FAIL. **Step 3: Implement** (reuse the repo's packet header parsing helpers for path length/hash size; advert appdata parsing as in existing advert parser). **Step 4:** PASS (Windows + WSL ASan).
- [ ] **Step 5: Commit** `feat(freenove): mesh model, attribution and geo math`.

---

### Task 5: Fast framebuffer engine

**Files:**
- Create: `examples/companion_radio/ui-freenove/FrameBuffer332.h/.cpp` (pure drawing + row hashing), `variants/freenove_s3_35_head/FramePusher.h/.cpp` (LUT expand + DMA push via `St77922Panel`)
- Test: `test/test_framebuffer332.py`

**Interfaces — Produces:**
```cpp
class FrameBuffer332 {
 public:
  FrameBuffer332(uint8_t* pixels /*480*320*/, int w = 480, int h = 320);
  static uint8_t rgb(uint8_t r, uint8_t g, uint8_t b);         // to RGB332
  void clear(uint8_t c); void pixel(int x, int y, uint8_t c); void hline(int x, int y, int w, uint8_t c); void vline(...);
  void line(int x0, int y0, int x1, int y1, uint8_t c); void dashedLine(int x0,int y0,int x1,int y1,uint8_t c,int on,int off,int phase);
  void circle(int cx, int cy, int r, uint8_t c); void fillCircle(...); void arc(int cx,int cy,int r,int a0_deg,int a1_deg,int thick,uint8_t c);
  void text(int x, int y, const char* s, uint8_t c, uint8_t font);   // fonts: 6x8 mono, 8x16 mono
  void glowDot(int x, int y, uint8_t c);                             // core + 2 ramp rings
  uint32_t rowHash(int y) const;
};
void buildLut565(uint16_t lut[256], bool byteswap);
struct DirtyRows { int count; int y0[320]; int n[320]; };   // coalesced runs
void diffRows(const FrameBuffer332&, uint32_t last_hashes[320], DirtyRows& out);
```
- [ ] **Step 1: Failing tests** — `lut_matches_formula` (all 256 entries vs reference, byteswap on/off), `primitives_clip_safely` (all primitives with coordinates far off-screen → no out-of-bounds writes, checked with ASan in WSL), `push_skips_unchanged_rows` (identical frame → 0 dirty rows; one changed pixel at y=100 → exactly one run [100,1]; adjacent changed rows coalesce), `dashed_line_phase_animates` (phase+1 shifts pattern by one pixel), `glow_ramp_colours_in_palette`.
- [ ] **Step 2: Run** — FAIL. **Step 3: Implement**; `FramePusher` expands dirty runs through the LUT into a DMA-capable internal-RAM line buffer (e.g. 32 rows) and queues transfers while the next frame draws. **Step 4:** PASS.
- [ ] **Step 5 (controller, hardware):** a bouncing-dots demo page shows the measured fps over USB; record idle fps/CPU and full-screen-change fps.
- [ ] **Step 6: Commit** `feat(freenove): RGB332 framebuffer with row-diff DMA push`.

---

### Task 6: Head receive path + page framework

**Files:**
- Modify: `src/helpers/esp32/ESPNOWRadio.cpp` (`OnDataRecv` ~54-58: if `observer::isObserverFrame` → hand to a registered callback, skip MeshCore/WiChat parsing; otherwise unchanged)
- Create: `examples/companion_radio/ui-freenove/FreenoveUITask.h/.cpp` (owns model, framebuffer, pages, touch, swipe/page dots; renders at a fixed frame budget on core 1), `Page.h` (`virtual void draw(FrameBuffer332&, uint32_t now_ms); virtual bool touch(int x,int y,bool down);`)
- Test: `test/test_observer_demux.py` (contract + pure demux helper)

- [ ] **Step 1: Failing tests** — `observer_frames_bypass_meshcore_parser`, `meshcore_frames_unchanged_with_observer_registered`, `page_swipe_navigation` (pure page index logic: swipe left/right, dots), `node_tap_opens_detail` (hit-test helper).
- [ ] **Step 2: Run** — FAIL. **Step 3: Implement.** **Step 4:** PASS; build env — SUCCESS.
- [ ] **Step 5: Commit** `feat(freenove): observer demux and page framework`.

---

### Task 7: HOME page (topology + feed + vitals)

**Files:** Create `examples/companion_radio/ui-freenove/HomePage.h/.cpp`, `GraphLayout.h` (pure force-directed layout with damping, fixed repeater at centre), `GeoLayout.h` (pure lat/lon → screen projection centred on the repeater, auto zoom to fit). Test: `test/test_layouts.py`.

Visuals per mockup `layout-v2.html` page 1: nodes as glowing dots coloured by link quality (green good, cyan ok, amber weak, magenta poor; thresholds SNR ≥ 5, ≥ 0, ≥ −7, else), labels, links dim, the last packet's path drawn as an animated dashed bright line with a travelling dot (≈2 s), ping rings on the sender, GRAPH|GEO toggle top bar, live feed column right (newest top, colour by type: ADV amber, GRP magenta, TXT cyan, ESP-NOW violet; SNR in green), vitals bar bottom (RX/m, TX/m, AIR %, NF, NODES, ESP).

- [ ] **Step 1: Failing tests** — `graph_layout_converges_and_stays_on_screen` (20 random nodes → all inside the 340×270 area after 200 iterations, max movement < 0.5 px), `graph_layout_stable_when_node_added` (existing nodes move < 20 px), `geo_projection_fits_all_nodes` (bounding box inside area, repeater at centre), `geo_projection_single_node`.
- [ ] **Step 2–4:** FAIL → implement → PASS; build SUCCESS.
- [ ] **Step 5 (controller, hardware):** with the M5 observer on, HOME shows real nodes, animated paths on new packets, feed scrolling; fps logged ≥ 30 while animating.
- [ ] **Step 6: Commit** `feat(freenove): HOME topology, feed and vitals`.

---

### Task 8: TELEMETRY, MAP, NODES pages + node detail card

**Files:** Create `TelemetryPage.*`, `MapPage.*`, `NodesPage.*`, `NodeCard.*` in `examples/companion_radio/ui-freenove/`; pure helpers `Widgets.h` (gauge arc value→angle, sparkline scaling, waterfall column colour map). Test: `test/test_widgets.py`.

Per mockup `layout-v2.html` page 2 for TELEMETRY (gauges SNR/RSSI/AIR/DUP, per-node sparklines for the 3 most active, 60-min packets/min bars, SNR waterfall one column per packet, vitals bar). MAP = GEO view full screen with distance labels and per-packet arcs. NODES = scrollable table (name, type, last heard, SNR via, hops, km, packets), tap header to sort. Node card = signal history sparkline, last path list, position/distance/bearing, last 5 packets.

- [ ] **Step 1: Failing tests** — `gauge_angle_clamps`, `sparkline_scales_to_box`, `waterfall_colour_map_monotonic`, `nodes_sort_by_each_column` (stable sort), `node_card_handles_no_gps`.
- [ ] **Step 2–4:** FAIL → implement → PASS; build SUCCESS.
- [ ] **Step 5 (controller, hardware):** walk all pages, tap a node from HOME, MAP and NODES.
- [ ] **Step 6: Commit** `feat(freenove): telemetry, map, nodes and node card`.

---

### Task 9: HEAD page (CYD screens on the Freenove)

**Files:** Create `examples/companion_radio/ui-freenove/FbDisplayDriver.h/.cpp` (a `DisplayDriver` implementation drawing into a framebuffer region with integer scale, mapping DisplayDriver colours to RGB332), `HeadPage.*` (hosts HeadCompose/HeadAdmin/HeadPresets, forwards touch). Test: `test/test_fb_display_driver.py` (pure: text/rect/colour mapping into a framebuffer).

- [ ] **Step 1: Failing tests** — `fb_driver_draws_text_and_rects_in_region`, `fb_driver_clips_to_region`, `colour_map_covers_displaydriver_colours`.
- [ ] **Step 2–4:** FAIL → implement → PASS; build SUCCESS; re-run `test_head_helpers`.
- [ ] **Step 5 (controller, hardware):** HEAD page: login to the M5, run a Ver command, open Presets list (do not load a preset unless the owner chooses).
- [ ] **Step 6: Commit** `feat(freenove): HEAD page with compose/admin/presets`.

---

### Task 10: End-to-end, docs, finish

- [ ] **Step 1 (controller):** announce, flash the M5 from this branch; `set espnow.observer on`; verify `get espnow.stats tx` still sane and mesh unaffected; Freenove shows live data on all pages for 15 minutes; record fps, CPU, PSRAM/heap.
- [ ] **Step 2:** `variants/freenove_s3_35_head/README.md`: build/flash, pages, observer feed, how attribution works, controls.
- [ ] **Step 3:** WSL full run of all suites touched; builds: Freenove env, CYD head, ThinkNode M5, DevKit relay.
- [ ] **Step 4: Commit** `docs(freenove): visual head guide`.
