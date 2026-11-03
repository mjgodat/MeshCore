# CYD Repeater Head Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A touch-screen ESP32 "head" for the headless M5 repeater: a MeshCore companion whose radio is ESP-NOW (through the M5's raw bridge), showing what the repeater relays, administering it remotely, and sharing the ESP-NOW air with WiChat.

**Architecture:** New Cascade variant `variants/cyd_espnow_head/`, modeled on `variants/sensecap_indicator-espnow` (`MESH_PRIMARY_ESPNOW` + `ESPNOWRadio` + companion example + `ui-new` + LGFX display + `HAS_TOUCH`). A generic "foreign frame" hook in `ESPNOWRadio` lets the head hand WiChat frames to a small WiChat module; head-only screens (Repeater, Admin console, Compose with touch keyboard) are added to `ui-new` behind `UI_REPEATER_HEAD`. WiChat firmware gets a matching demux, MeshCore-frame relay and channel 6.

**Tech Stack:** C++ / Arduino-ESP32 via PlatformIO (Cascade `esp32_base`), LovyanGFX, ESP-NOW; host tests in Python that compile C++ with `g++` (repo pattern, e.g. `test/test_cardputer_mesh_storage.py`).

**Spec:** `docs/superpowers/specs/2026-10-28-cyd-repeater-head-design.md`

## Global Constraints

- Cascade `AGENTS.md`: only one PlatformIO process per checkout at a time. Windows: `$env:PLATFORMIO_CORE_DIR='C:\pio'`; add `pre:variants/m5stack_cardputer_adv/win_long_cmdline.py` via `PLATFORMIO_EXTRA_SCRIPTS` if the command line overflows.
- The head is an ordinary MeshCore node using ordinary MeshCore packets; no side protocol, no crypto changes.
- No repeater firmware change unless Task 1 proves a gap. M5 (COM19) is live: announce every M5 command to the owner and get an OK before running it; opening COM19 resets it.
- Secrets (admin password, bridge secret, WiFi) never in committed source.
- Shared ESP-NOW channel **6**. Demux rule: **WiChat ⇔ len ≥ 38 && bytes[16..21] == `57 43 57 43 57 43` && LE u16 at 24 == `0x5743`**; everything else is MeshCore.
- Head never auto-adverts (companion default); "Advert" is user-triggered only.
- Nothing is committed or pushed without the owner's OK (commit steps below are local and run only once the owner has said to commit).
- Targets: `CYD_ESPNOW_Head_companion_usb` (esp32dev, 4 MB, no PSRAM, ST7789 320×240 + XPT2046 per `wichat/lora_term/src/display.cpp`) and `Freenove_S3_ESPNOW_Head_companion_usb` (FNK0104N, ST77922 QSPI, I2C touch 0x55 per `wichat/cyd/src`).
- **Two flavours per target, one codebase.** Mesh-only envs above; Mesh + WiChat envs `CYD_ESPNOW_Head_WiChat_companion_usb` and `Freenove_S3_ESPNOW_Head_WiChat_companion_usb` extend them with `-D HEAD_WICHAT=1` (`extends =` the mesh env, no duplicated flags). All WiChat code and UI sits behind `#if HEAD_WICHAT`; the mesh-only build must not change when the flag is added. Build order: Tasks 1, 4, 5, 6, 7 (mesh-only, working head) → Tasks 2, 3, 8 (WiChat flavour) → Task 9.

## Review Focus

1. **A WiChat send completing while a MeshCore send is in flight** must not mark the MeshCore send complete (shared `esp_now` send callback) — Task 2 test `foreign_send_does_not_complete_mesh_send`.
2. **A MeshCore packet whose bytes happen to start `08 00`** must still reach MeshCore, and a 37-byte WiChat-looking frame must not be claimed as WiChat — Task 2 tests `mesh_frame_starting_0x08_is_not_wichat`, `short_frame_is_not_wichat`.
3. **Reboot with no repeater reachable**: clock must never go below the persisted floor, so later logins aren't rejected as replays — Task 6 test `clock_floor_survives_reboot`.
4. **WiChat relay storm**: a MeshCore frame relayed by two WiChat nodes must be rebroadcast at most once per node — Task 8 test `meshcore_frame_relayed_once`.
5. **"Send as" with a long message**: `"<repeater name>: <text>"` must be truncated to the MeshCore group-text limit, never overflow — Task 5 test `send_as_repeater_truncates`.

---

### Task 1: Headless head variant + M5 raw-bridge interop

Proves the whole idea before any UI exists.

**Files:**
- Create: `variants/cyd_espnow_head/platformio.ini`, `target.h`, `target.cpp`, `README.md`
- Modify: none outside the variant

**Interfaces:**
- Produces: env `CYD_ESPNOW_Head_companion_usb` (no display yet); `extern ESPNOWRadio radio_driver; extern ESP32RTCClock rtc_clock; bool radio_init(); mesh::LocalIdentity radio_new_identity();` (same shape as `sensecap_indicator-espnow/target.*`).

- [ ] **Step 1:** Create the variant from `sensecap_indicator-espnow` minus display/touch/sensors: `board = esp32dev`, `-D MESH_PRIMARY_ESPNOW=1`, `-D ENABLE_USB_INTERFACE`, `-D MAX_CONTACTS=100`, `-D MAX_GROUP_CHANNELS=8`, `-D DISABLE_WIFI_OTA=1`, `-D PIN_BOARD_SDA=-1 -D PIN_BOARD_SCL=-1`, `-D ESPNOW_CHANNEL_DEFAULT=6` (or the existing Cascade setting for the primary ESP-NOW channel — find it in `helpers/esp32/WiFiRadioPolicy.h` / `restoreEspNowChannel()` and use that). Pick a partition table that fits (`min_spiffs.csv` or `huge_app.csv`); record flash/RAM.
- [ ] **Step 2: Build.** Run `pio run -e CYD_ESPNOW_Head_companion_usb`. Expected: SUCCESS; note flash % and RAM %.
- [ ] **Step 3: Read M5 state (owner OK first).** On COM19: `get espnow`, `get wifi`, `get bridge.format`, `get bridge.source`, `get bridge.channel`, `ver`. Report to owner. Stop if a WiFi AP is active.
- [ ] **Step 4: Switch M5 (owner OK first).** `set bridge.format raw`; confirm `bridge.source tx`, channel 6.
- [ ] **Step 5: Flash head to COM3** at 0x10000 (app) — COM3 currently runs `lora_term`, which is being retired.
- [ ] **Step 6: Verify interop over the head's USB terminal.** Expected, within 10 min: `list` shows contacts learned from adverts the M5 repeats; `to CaptCallahan-Base` → `login <pw>` → "login OK"; `cmd ver` returns the M5 version; `public test from head` appears on the T114/phone. M5 `espnow.stats` shows rx frames > 0.
- [ ] **Step 7: Record the `bridge.source tx` gap.** Compare 10 min of M5 USB RX log vs head `list`/messages; write findings in the variant README ("what the head does not see"). If replies or common traffic are missing, stop and report to the owner (possible upstream `rx+tx` bridge option).
- [ ] **Step 8: Commit** (when owner allows): `git add variants/cyd_espnow_head && git commit -m "Add headless CYD ESP-NOW head companion variant"`

### Task 2: ESP-NOW foreign-frame hook + WiChat classifier

**Files:**
- Create: `src/helpers/esp32/ESPNowForeignFrames.h` (classifier + hook declarations)
- Modify: `src/helpers/esp32/ESPNOWRadio.cpp` (`OnDataRecv`, `OnDataSent`), `src/helpers/esp32/ESPNOWRadio.h`
- Test: `test/test_espnow_foreign_frames.py`

**Interfaces:**
- Produces:
  - `namespace mesh::espnow { bool isWiChatFrame(const uint8_t* data, size_t len); }` (pure, header-only)
  - weak hook `extern "C" bool espnowForeignFrameRecv(const uint8_t* src_mac, const uint8_t* data, int len, int8_t rssi);` — default returns `false`; if it returns `true` the frame is consumed and not queued for MeshCore. Called first in `OnDataRecv` (ISR-ish WiFi task context: implementations must only copy into a queue).
  - `bool ESPNOWRadio::sendForeignFrame(const uint8_t* data, size_t len);` — broadcast send that returns `false` while a MeshCore send is active; its completion callback must not touch `is_send_complete` / `last_send_status`.

- [ ] **Step 1: Write failing tests** in `test/test_espnow_foreign_frames.py` (compile `ESPNowForeignFrames.h` with g++ like other host tests):
  - `wichat_frame_detected`: 38-byte buffer, `buf[16..21]=57 43 57 43 57 43`, `buf[24]=0x43, buf[25]=0x57` → true
  - `short_frame_is_not_wichat`: same but 37 bytes → false
  - `mesh_frame_starting_0x08_is_not_wichat`: 60-byte buffer starting `08 00`, other bytes 0 → false
  - `fragment_envelope_is_not_wichat`: buffer starting `FE 4D 43 46` → false
  - `foreign_send_does_not_complete_mesh_send`: (stub `esp_now_send` + callback) start mesh send, then `sendForeignFrame` → returns false while active; after mesh completes, foreign send succeeds and its callback leaves mesh state unchanged.
- [ ] **Step 2: Run** `python -B test/test_espnow_foreign_frames.py -v` (needs host g++: CI, or WSL `wsl -d kali-linux -e g++ --version`; if missing, ask the owner to `sudo apt install g++` there). Expected: FAIL (header missing).
- [ ] **Step 3: Implement** the header, hook call in `OnDataRecv` (before the length/queue checks), and `sendForeignFrame` with a `foreign_send_pending` flag that `OnDataSent` checks first. In the head variant, the mesh-only build defines `espnowForeignFrameRecv` to return `isWiChatFrame(...)` (drop WiChat frames); the `HEAD_WICHAT` build replaces it with Task 3's queueing version.
- [ ] **Step 4: Run tests** — PASS. Build `Generic_ESPNOW_comp_radio_usb` and `SenseCapIndicator-ESPNow_comp_radio_usb` to prove no regression.
- [ ] **Step 5: Commit** (when allowed): `"Add ESP-NOW foreign frame hook and WiChat classifier"`

### Task 3: WiChat module on the head

**Files:**
- Create: `variants/cyd_espnow_head/wichat/frame.h`, `frame.cpp` (copied from `C:\Users\Micha\wichat\node\src\frame.*`, attribution comment), `variants/cyd_espnow_head/WiChatLink.h/.cpp`
- Test: `test/test_cyd_head_wichat.py`

**Interfaces:**
- Consumes: Task 2 hook and `sendForeignFrame`, `isWiChatFrame`.
- Produces: `class WiChatLink { public: void begin(const uint8_t local_mac[6], const char* callsign); void loop(); bool sendChat(const char* text); const WiChatMsg* recent(int i) const; int recentCount() const; uint32_t version() const; };` with `struct WiChatMsg { char sender[16]; char text[WICHAT_MAX_PAYLOAD+1]; uint32_t at_ms; bool outgoing; };`. Behavior: queue from hook (depth 8, drop when full), `frame_decode`, dedup ring of 128 msg_ids, BEACON → callsign table (MAC→name, 16 entries), CHAT → recent list (16) and relay with TTL−1 if TTL>1, own frames ignored, periodic BEACON every 30 s with the head's name. ESP-NOW only (no promiscuous 802.11).

All of this task is compiled only when `HEAD_WICHAT=1`; add the `*_WiChat_companion_usb` env (`extends` the mesh env + `-D HEAD_WICHAT=1`) here.

- [ ] **Step 1: Failing tests:** `chat_frame_added_once` (same msg_id twice → one entry), `relay_decrements_ttl` (TTL 3 → relayed frame TTL 2; TTL 1 → not relayed), `beacon_names_sender` (beacon then chat → sender = callsign, else MAC hex), `own_frame_ignored`.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement (send path stubbed in tests). **Step 4:** Run → PASS; build head env.
- [ ] **Step 5: Hardware check:** a WiChat CYD/node on channel 6 (after Task 8) and the head exchange chats. **Step 6: Commit** (when allowed).

### Task 4: CYD display + touch with the standard ui-new screens

**Files:**
- Create: `variants/cyd_espnow_head/CYDDisplay.h` (LGFX ST7789 + XPT2046 `DisplayDriver`, modeled on `sensecap_indicator-espnow/SCIndicatorDisplay.h`), `CYDTouchCal.h/.cpp` (calibration stored in NVS; flow ported from `wichat/lora_term/src/touch_cal.cpp`; run at boot if absent or if BOOT (GPIO0) held)
- Modify: `variants/cyd_espnow_head/platformio.ini` (`DISPLAY_CLASS=CYDDisplay`, `HAS_TOUCH`, `-I examples/companion_radio/ui-new`, `+<../examples/companion_radio/ui-new/*.cpp>`, `+<helpers/ui/LGFXDisplay.cpp>`, `UI_MESSAGES_HOME_PAGE=1`, `UI_ZOOM`/`UI_COORD_SCALE` chosen so text is readable at arm's length on 320×240, `AUTO_OFF_MILLIS=300000UL`), `target.h/.cpp` (display instance, `PIN_USER_BTN=0`)

**Interfaces:** Produces `DISPLAY_CLASS display;` usable by ui-new and later tasks.

- [ ] **Step 1:** Implement driver + calibration. **Step 2:** Build; record flash/RAM; if RAM won't fit, reduce `UI_BUFFER_COLOR_DEPTH`/zoom canvas before cutting features and report numbers.
- [ ] **Step 3: Hardware check (COM3):** calibration runs once; home pages (status, recent heard, messages, advert) render and tap zones (prev / select / next) navigate; screen auto-off and wake on tap. **Step 4: Commit** (when allowed).

### Task 5: Compose with touch keyboard and "Send as"

**Files:**
- Create: `examples/companion_radio/ui-new/TouchKeyboard.h/.cpp` (QWERTY, shift, numbers/symbols page, backspace, send; layout from `wichat/lora_term/src/keyboard.cpp`), `examples/companion_radio/ui-new/HeadCompose.h/.cpp`
- Modify: `examples/companion_radio/ui-new/UITask.cpp` (open compose from the Messages page under `#if UI_REPEATER_HEAD`)
- Test: `test/test_cyd_head_compose.py`

**Interfaces:**
- Consumes: `BaseChatMesh::sendGroupMessage(uint32_t ts, mesh::GroupChannel& ch, const char* sender_name, const char* text, int len)`, `sendMessage(...)` for contacts, `WiChatLink::sendChat`.
- Produces: `int buildSendAsText(const char* sender_name, const char* text, char* out, size_t out_size)` (pure; returns length) — used only to preview/truncate; actual send passes `sender_name` = head name or paired repeater's name.

- [ ] **Step 1: Failing tests:** `send_as_repeater_truncates` (sender 32 chars + text 200 chars → length ≤ MeshCore group text max, NUL-terminated), `send_as_head_uses_node_name`.
- [ ] **Step 2–4:** FAIL → implement → PASS. Destinations: channels (from companion channel list), contacts, and "WiChat" only under `#if HEAD_WICHAT` (wired up in Task 3's flavour; mesh-only build has no WiChat entry). "Send as" toggle shown only for channel destinations; default = head.
- [ ] **Step 5: Hardware check:** channel post as head and as `CaptCallahan-Base` both appear on T114 with the right name; DM shows delivery ACK; WiChat chat reaches a WiChat CYD. **Step 6: Commit** (when allowed).

### Task 6: Repeater page, Admin console, clock

**Files:**
- Create: `examples/companion_radio/ui-new/HeadAdmin.h/.cpp` (console screen), `variants/cyd_espnow_head/HeadClock.h/.cpp`, `HeadPairing.h/.cpp`
- Modify: `examples/companion_radio/ui-new/UITask.cpp` (`HomePage::REPEATER` first under `UI_REPEATER_HEAD`), variant `platformio.ini` (`-D UI_REPEATER_HEAD=1`, enable the companion stream-terminal feature if gated)
- Test: `test/test_cyd_head_clock.py`

**Interfaces:**
- Consumes: `MyMesh::beginStreamTerminal(Stream&)`, `runStreamTerminal(char*)`, `endStreamTerminal(Stream&)` — the Admin console is a stream terminal whose `Stream` is a 2 KB ring buffer rendered on screen; it issues `to <repeater>`, `login <pw>`, `cmd <x>` exactly as a USB user would. Password typed via Task 5 keyboard, held in RAM only.
- Produces: `class HeadClock { void begin(); void onTrustedTime(uint32_t epoch); uint32_t floorEpoch() const; }` — persists a floor in NVS (write at most every 10 min and on trusted time); on boot sets RTC to `max(rtc, floor)`. `class HeadPairing { bool pairedKey(uint8_t out[PUB_KEY_SIZE]) const; bool pair(const ContactInfo&); }` — pairing = choose a repeater from heard contacts (stored in NVS).
- Repeater page shows (refresh button runs `cmd` `ver`, `get radio`, `neighbors` and the companion's status request for battery/uptime/noise/packets): name, battery, uptime, noise floor, last RSSI/SNR, RX/TX counts, neighbor count with SNR, logged-in state. One-tap commands: `ver`, `neighbors`, `get radio`, `advert`. Free-text command line below.
- **Repeater Settings panel (owner, 2026-10-28):** after login, a touch page that reads values with `cmd get <key>` and changes them with `cmd set <key> <value>`: name, radio (freq/bw/sf/cr), tx, advert.interval / flood.advert.interval, flood.max, bridge.enabled/format/source/channel, guest.password; buttons for `advert` and `reboot`. Radio and bridge changes require a confirm step (they can cut the head off). No such panel exists in firmware today (phone app only).
- Clock: request repeater time without login (companion's existing clock/status request), call `onTrustedTime`; if repeater time < build epoch, prompt manual entry, then offer `cmd time <epoch>` when logged in.

- [ ] **Step 1: Failing tests:** `clock_floor_survives_reboot` (floor 1,793,000,000 persisted; boot with RTC 0 → RTC ≥ floor), `clock_never_moves_backwards` (trusted time older than floor → ignored).
- [ ] **Step 2–4:** FAIL → implement → PASS; build.
- [ ] **Step 5: Hardware check:** after a head reboot with the M5 unplugged, then replugged: login still succeeds; console shows `neighbors` output; Repeater page fills. **Step 6: Commit** (when allowed).

### Task 7: Freenove ESP32-S3 3.5" target

**Files:** Create `variants/cyd_espnow_head/FreenoveDisplay.h` (ST77922 QSPI + I2C touch 0x55, from `wichat/cyd/src`); Modify variant `platformio.ini` (env `Freenove_S3_ESPNOW_Head_companion_usb`, `board = esp32-s3-devkitc-1`, PSRAM flags, larger limits `MAX_CONTACTS=350`, `MAX_GROUP_CHANNELS=40`).

- [ ] **Step 1:** Build all head envs (serially): both mesh-only envs, and both WiChat envs once Task 3 exists. **Step 2: Hardware check on COM6:** same checks as Tasks 4–6. **Step 3: Commit** (when allowed).

### Task 8: WiChat firmware shares the air (separate repo `C:\Users\Micha\wichat`)

**Files:**
- Modify: `node/src/radio_esp32.cpp`, `cyd/src/radio_esp32.cpp`, `cyd_35/src/radio_esp32.cpp`, `cyd_28/src/radio_esp32.cpp` (`espnow_rx_cb`), each `config.h` (`WICHAT_DEFAULT_CH 6`)
- Create: `lib/espnow_share/espnow_share.h` (shared: `wichat_is_frame()` with the same rule as Task 2; `meshcore_relay_seen(const uint8_t*, size_t)` FNV-1a-32 over the bytes, 64-entry ring)
- Test: `lib/espnow_share/test_espnow_share.c` (host gcc, `make`-free one-liner build)

**Interfaces:** Produces `bool wichat_is_frame(const uint8_t*, size_t)`, `bool meshcore_relay_should_forward(const uint8_t*, size_t)` (true the first time a frame hash is seen).

- [ ] **Step 1: Failing tests:** `meshcore_frame_relayed_once` (same bytes twice → true then false), `wichat_rule_matches_head` (same vectors as Task 2).
- [ ] **Step 2–4:** FAIL → implement → PASS.
- [ ] **Step 5:** In `espnow_rx_cb`: WiChat frames → existing queue; other frames → if `WICHAT_ROLE_REPEATER` and `meshcore_relay_should_forward` → `esp_now_send` broadcast unchanged; else drop. Build each project (`pio run` in each dir, one at a time).
- [ ] **Step 6: Hardware check:** a WiChat node between head and M5 relays MeshCore frames (head works with the node as the only link); WiChat chat still works between CYDs on channel 6. **Step 7: Commit** in the wichat repo (when allowed) and update `project_wichat.md` memory.

### Task 10: Headless ESP32 DevKit ESP-NOW repeater (owner, 2026-10-28)

Cheap ESP32 dev boards act as repeaters, like WiChat's `node/`.

**Files:** Create `variants/esp32dev_espnow_repeater/platformio.ini` (reuses `variants/generic_espnow` target; `board = esp32dev`, `MESH_ESPNOW_CHANNEL=6`, `min_spiffs.csv`). Envs: `ESP32Dev_ESPNOW_repeater` (mesh-only, simple_repeater) and, after Task 3, `ESP32Dev_ESPNOW_WiChat_repeater` (`-D HEAD_WICHAT=1`, WiChat relay-only: dedup + TTL−1 rebroadcast, no UI).

- [ ] **Step 1:** Build mesh-only env. **Step 2: Hardware check:** DevKit between head and M5: head works with the DevKit as the only link (M5 out of direct ESP-NOW range or bridge channel moved), `cmd neighbors` on the DevKit lists the head/M5. **Step 3:** WiChat flavour after Task 3; WiChat CYD chats relay through it. **Step 4: Commit** (when allowed).

### Task 9: Docs and retirement notes

- [ ] Variant `README.md`: hardware, envs, M5 settings (`bridge.format raw`, `bridge.source tx`, channel 6), pairing, admin, "what the head does not see", WiChat sharing.
- [ ] Mark `wichat/lora_term` and `wichat/gateway` as superseded (README note in each, no deletion).
- [ ] Commit (when allowed).
