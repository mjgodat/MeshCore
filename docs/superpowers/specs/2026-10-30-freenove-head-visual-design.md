# Freenove 3.5" Visual Repeater Head — Design

Date: 2026-10-30. Owner: mjgodat. Status: draft for review. Branch family: feature/cyd-espnow-head (CYD head) + M5 repeater firmware.

## Why

The CYD head lets the owner control their ThinkNode M5 repeater over ESP-NOW but shows little of what the repeater is doing. The Freenove ESP32-S3 3.5" board (S3 dual core, PSRAM, 480×320 touch) can be a fast, futuristic, live window into **the repeater and the signals coming into it**: packets moving through the mesh, who is out there and where, and how good the links are — across as many pages as needed — while still doing what the CYD head does.

## Goals

Pages (owner approved the structure from mockup `layout-v2.html`):
1. **HOME** — mesh topology with packets animating along their real hop paths; **GRAPH ↔ GEO** toggle (free-form graph vs nodes placed by advert GPS); live packet feed on the right; vitals bar at the bottom.
2. **TELEMETRY** — gauges (last SNR, RSSI, airtime, duplicate %), per-node SNR sparklines, packets/min history (60 min), SNR waterfall (one column per packet), repeater vitals (noise floor, ESP-NOW node count, battery, uptime).
3. **MAP** — full-screen geo view of nodes with GPS; arcs fire per packet; distances/bearings from the repeater; tap a node for details.
4. **NODES** — sortable table: name, type, last heard, SNR (heard via), hops, distance, packet count.
5. **HEAD** — the CYD head's compose, Repeater admin (Login, console, Settings, Presets). Owner wants this page kept.
- Tapping a node anywhere opens a **node detail card**: signal history, last path(s), position/distance, last packets.
- Geolocation from advert payloads (lat/lon when present); nodes without GPS appear in graph/feed/table but not on GEO/MAP.
- The head itself need not appear as a node on the graph/map. Distances and bearings are from the **repeater's** position.

### Look and speed (owner, 2026-10-30)
- **Look: the mockup style** (owner prefers it over SquatchWatch's vaporwave theme): near-black blue background, neon cyan/green/amber/magenta/violet, glow, animated dashed links, travelling packet dots, monospace HUD text. Mockups: `.superpowers/brainstorm/46706-1793343834/content/visual-direction.html` and `layout-v2.html` (copy them into the plan workspace for builders).
- **Speed: match SquatchWatch-CYD's animation smoothness** (owner reference). Achieve it with our **own implementation** of the same techniques (techniques are not covered by its GPLv3 licence; its code is — do not copy SquatchWatch or WiChat display code):
  - **8-bit (RGB332) full-screen framebuffer** in PSRAM (480×320 = 150 KB; two for double buffering), expanded to RGB565 on the wire through a 256-entry lookup table built once.
  - **Per-row change detection**: hash each row of the outgoing frame; rows unchanged since the last push are not sent.
  - **DMA-driven QSPI pushes** in hardware-sized bursts, overlapping the CPU drawing the next frame (render on one core, push/IO concurrent).
  - Palette constrained to what RGB332 renders well (design the neon colours against the 332 cube; glow via a few precomputed ramp entries).
  - Target ≥30 fps on animated pages; measure with a frame profiler.

## Data: what the head can know

Today the M5's ESP-NOW bridge forwards only packets the M5 **transmits** (`bridge.source tx`, raw format `fmt=raw`), and no per-packet LoRa signal data crosses ESP-NOW. "The repeater and the signals coming into it" requires an **M5 observer feed** (owner's stated focus; in scope):

- New, opt-in on the M5 (`set espnow.observer on`), separate from mesh packet bridging so mesh behavior is unchanged when on or off.
- **Per received LoRa packet:** the **raw packet bytes** (header, path, payload; ≤ ~184 B) plus SNR, RSSI, receive timestamp, and the M5's disposition (forwarded / duplicate / dropped). Carrying raw bytes (designer's call) lets the head decode everything itself — advert sender, names, lat/lon, path hashes, group texts it has keys for — including packets the M5 heard but did not retransmit.
- **Periodic vitals (~5 s):** noise floor, RX/TX packets per minute, airtime %, duplicate count, ESP-NOW node count, battery mV, uptime, current radio tuple/preset, and the repeater's own lat/lon.
- **Signal attribution (designer's call):** LoRa SNR/RSSI belongs to the **last-hop transmitter**. Attribute to the sender for 0-hop packets; otherwise to the last path hash ("heard via <hash/name>"), resolved to a node only when the hash maps uniquely to a known repeater (`path.hash.mode 2` → 2-byte hashes); ambiguous hashes shown as the hash. UI labels say "heard via".
- Frames have a header that cannot collide with MeshCore raw frames, the raw-mode fragment format (`ESPNowRawReassembler`, `FE 4D 43 46`) or WiChat frames; the head routes them to the observer decoder before the MeshCore/WiChat parsers.
- Rate-limited (drop oldest per-packet records under load; vitals always sent). Reuse the repeater's existing per-packet RX hooks that already capture SNR/RSSI (as MQTTBridge does) rather than adding a parallel capture.
- The head also decodes ordinary bridged packets as today.

## Rendering structure (designer's call)

- Panel: ST77922 over IDF `spi_master` quad mode; the panel is **320×480 portrait**, so landscape output is transposed on push; RGB565 byte-swapped on the wire. Written from Freenove's vendor driver / datasheet (pins below), not from SquatchWatch or WiChat code.
- Visual pages draw into the 8-bit framebuffer through a small primitives layer (pixels, lines, circles/arcs, text, glow ramps).
- HEAD page: reuse the CYD head's Head*.h screens via a `DisplayDriver` adapter that renders into a region of the framebuffer; touch comes from a new ST77922 I²C (0x55) touch driver feeding the same touch events.
- Headroom: PSRAM holds framebuffers, node table (e.g. 256 nodes), packet ring (e.g. 512 records), histories; internal RAM stays for the mesh/companion. The first plan task confirms the companion build + HEAD page + framebuffers fit (16 MB flash, OPI PSRAM).

## Hardware / build facts

Freenove FNK0104N: ESP32-S3, native USB-CDC (303A:1001, COM6 last seen), 16 MB flash, OPI PSRAM (`board_build.arduino.memory_type = qio_opi`), ST77922 QSPI display (CS 10, SCLK 12, D0 11, D1 13, D2 14, D3 9, backlight 41, SPI2_HOST, 80 MHz), touch I²C 0x55 (integrated TDDI). Board notes: memory file `freenove_esp32s3_board.md`; vendor repo https://github.com/Freenove/Freenove_ESP32_S3_Display.

## Constraints

- Same branch family as the CYD head; M5 observer feed off by default, no mesh behavior change when off, M5 still fits its app slot.
- ESP-NOW channel 6, LR mode, like the rest of the system.
- Code stays MIT-compatible (no GPL code copied).
- Owner rules: local commits allowed for this project, never push without asking, the M5 is a live repeater (announce before flashing).

## Left to the builder

Exact observer frame format, task/core split, graph layout algorithm (simple force-directed with damping), RGB332 palette tuning, fonts, history sizes within budget, animation timing.
