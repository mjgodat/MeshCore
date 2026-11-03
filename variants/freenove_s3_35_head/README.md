# Freenove ESP32-S3 3.5" ESP-NOW Repeater Head

A visual head for a LoRa repeater, on the Freenove FNK0104N: ESP32-S3,
16 MB flash, 8 MB OPI PSRAM, native USB-CDC, 3.5" ST77922 QSPI panel
(320x480 portrait, driven as 480x320 landscape) with integrated touch.

The head is a MeshCore companion whose only radio is ESP-NOW (the CYD head's
job, `variants/cyd_espnow_head/README.md`): it reaches LoRa through its
repeater's raw-format ESP-NOW bridge. On top of that it shows what the
repeater hears, from the repeater's observer feed: live topology, telemetry,
a map and a node table. The companion UI (messages, compose, admin console)
is the fifth page, HEAD.

## Builds

| Env | What |
|-----|------|
| `Freenove_S3_35_Head_companion_usb` | The product: visual pages + companion, USB companion link |
| `Freenove_S3_35_Head_uitest` | Same firmware, but the USB CDC is a text test console instead of the companion link (see "UI test console") |
| `Freenove_S3_35_Head_companion_bringup` | Panel bring-up: test pattern, colour check, framebuffer demo with fps over USB |

Partition table: `default_16MB.csv` (two 6.25 MB app slots, 3.4 MB SPIFFS).

Build and flash (one PlatformIO process at a time):

```text
pio run -e Freenove_S3_35_Head_companion_usb
esptool --chip esp32s3 --port COMx --after watchdog-reset \
  write_flash 0x10000 .pio/build/Freenove_S3_35_Head_companion_usb/firmware.bin
```

That is an app-only update; a blank board needs the bootloader and
partition table too (`pio run -t upload`, or the merged image at 0x0).

Board quirks:

- After a flash with BOOT held, the chip stays in download mode: esptool's
  default RTS reset does not leave it on this board; `--after watchdog-reset`
  does.
- Open the USB serial port with DTR and RTS de-asserted. Asserted lines hold
  the S3's USB-JTAG in reset and the board looks dead.
- The first boot after an erase formats SPIFFS (1-2 min).
- On Windows the compiler command exceeds the 32,767-character limit; the env
  runs `variants/m5stack_cardputer_adv/win_long_cmdline.py` (-iprefix include
  shortening, no-op on other systems).

## Repeater side

On the repeater (the ThinkNode M5 runs `ThinkNode_M5_Repeater`, ESP-NOW
bridge built in):

```text
set espnow.enabled on
set bridge.format raw
set espnow.observer on
get espnow.observer
```

`bridge.channel` / `espnow.channel` must match the head (channel 6). The
bridge settings are the CYD head's; `espnow.observer` is off by default and
persists. With it off, the bridge's frames and `get espnow.stats tx` are
byte-identical to a build without the observer.

## Observer feed

The repeater sends two kinds of ESP-NOW frame to the head
(`src/helpers/bridges/ObserverFrame.h`, magic `B0 42 53 4F`, version 1):

- **Packet records**: every LoRa packet the repeater receives, raw MeshCore
  bytes (up to 235; longer packets are cut and flagged) with SNR, RSSI and the
  repeater's receive time.
- **Vitals**, every 5 s: uptime, noise floor, RX/TX/duplicates per minute and
  airtime (over the last minute, `VitalsWindow`), ESP-NOW peers, battery,
  GPS position when it has a fix, radio (freq/BW/SF/CR) and preset slot.

Admission (`ObserverRateLimiter.h`): packet records pass a token bucket
(20/s, burst 40) into a 4-deep outbox; a full outbox drops the oldest record.
Vitals bypass the bucket. Mesh frames always go first: while mesh traffic is
queued on the bridge, new records are dropped. Observer frames are never
counted as mesh traffic, repeated or retried. They follow `espnow.peer`
(unicast) when set.

The head also feeds every MeshCore packet it receives over the bridge into
the same model, so the pages show traffic even with the observer off (no
SNR/RSSI or vitals then).

## How attribution works

`examples/companion_radio/ui-freenove/MeshModel.h`. LoRa SNR/RSSI belongs to
the last-hop transmitter, so:

- a 0-hop advert is attributed to its sender;
- a flooded packet with hops is attributed to the last path hash, resolved
  to a node only when exactly one known repeater's public key starts with
  that hash (compared at the hash's own size, 1-3 bytes); otherwise it stays
  a hash;
- direct-routed packets (they carry the remaining route, not the travelled
  one) and TRACE packets (their path holds SNR bytes) are not attributed.

Duplicates: a packet already seen in the last 64 updates only signal (an
observer repeat adds an SNR sample to its relay); a bridged repeat is
ignored. The same packet via observer and via bridge counts once. All model
time is the head's own clock, so an M5 reboot or millis wrap does not stall
the rates. Adverts are not signature-checked (display only). The node table
is bounded; when full, the least recently heard node is evicted.

## Pages

A tab bar along the bottom (HOME, TELEMETRY, MAP, NODES, HEAD) on every page.
The header shows "M5 // <repeater name>", the repeater's frequency and SF/BW
(from vitals; grey when stale, "NO REPEATER DATA" without vitals), an
observer-link icon (four arcs, lit by how fresh the newest observer frame
is) and the clock. Unknown values read "--"; nothing is invented.

- **HOME**: MESH TOPOLOGY with the repeater at the centre and contacts as
  rings coloured by link quality (green >= 5 dB, cyan >= 0, amber >= -7,
  violet below, grey stale). GRAPH (force-directed) or GEO (GPS around the
  repeater, km rings; "No repeater GPS fix" without one). A packet with a
  path draws its route with a travelling blip to the M5. NOISE FLOOR gauge,
  PACKETS odometer and RX/min with trend. QUICK row: ADVERT (the head's own
  advert: tap, "CONFIRM?", tap again; then a 30 s rest), PRESET (the
  repeater's presets list), OBSERVER and BRIDGE (its settings), TRACE (admin
  console), ADMIN (admin console, logged in with the saved password).
- **TELEMETRY**: gauges for last SNR, RSSI, airtime and duplicates; packets
  per minute over the last 60 min; SNR waterfall; the three most active
  direct nodes with SNR sparklines; noise floor history; ESP-NOW peers,
  battery, uptime.
- **MAP**: contacts with GPS around the repeater, north up, range rings and
  distances. Without a repeater fix the contacts are placed around their own
  centre with a scale bar and no distances. Nodes without GPS are never
  plotted (at 0,0 or elsewhere); they are counted ("N without GPS"). No base
  map tiles.
- **NODES**: one row per node: NAME, TYPE, HEARD, SNR, HOPS, KM, PKTS. SNR is
  the node's own last-hop SNR when its latest advert came straight to the M5;
  for a relayed node it is the SNR the M5 heard that advert at plus the relay,
  e.g. "-4.3 via Downtown". Relays are named from every repeater the head has
  heard an advert from (not the contact table); an unknown or ambiguous hash
  stays hex. Tap a header to sort, again to reverse; drag to scroll.
- **Node card**: tap a contact on HOME, MAP or NODES: SNR history (or "heard
  via <relay>" and the advert's SNR), last SNR/RSSI, last path, position (or
  why there is none), last five packets.
  CLOSE returns.
- **HEAD**: MeshCore's companion UI with the CYD head's screens (home
  carousel, messages, compose with "Send as", recently heard, admin console
  with login, presets, settings list), drawn at 2x in the same HMI style
  (`CompanionSkin.h`). Messages (`UI_HEAD_CHAT_LIST`, `ui-new/HeadChats.h`)
  are a conversation list (each channel and DM with its newest message, age
  and unread count, newest activity first; Home, New) and, on a tap, the
  thread: oldest to newest with sender and age, newest at the bottom, drag or
  Up/Down to scroll, "Write message" opens compose on that conversation and
  returns to the thread. An incoming message opens its thread unless the chat
  screen is already showing. The head's own posts appear as "You". The
  messages are the companion's 32-message store (161 B each), not a copy. The companion owns the screen only on this page; it
  keeps running on the other pages.

## Controls

- **Tab bar**: tap a tab to jump to that page.
- **Swipe** left/right above the tab bar: next/previous page (no wrap). On
  HEAD every gesture above the bar goes to the companion UI; swipe in the
  tab bar to leave it.
- **Tap**: buttons, tiles, contacts, column headers. **Vertical drag**:
  scroll lists.
- **Keyboard** (HEAD compose / login / console): letters, PC-style 123 page
  and #+= page. Shift is one-shot (the next letter only); there are no
  sticky or latching keys.
- **BOOT button**: the 1/2/3/4-click navigation of other single-button
  companions.
- The screen turns off after 5 min idle; a touch wakes it.

## Contact policy

The head keeps its table for its own use (`HeadContactPolicy.h`). It adds
only its home repeater (the repeater whose advert arrives at 0 hops: the
bridge forwards what that repeater transmits, and everything it relays has
at least one hop) and chat contacts under the normal auto-add settings.
Other repeaters, rooms and sensors are not added; they still appear on the
visual pages, which come from the model, not the contact table. When the
table (64) is full, the home repeater's advert evicts an old heard repeater.
The home repeater is remembered across reboots, listed first in the admin
console and offered first after the head itself in compose "As".

## UI test console

`Freenove_S3_35_Head_uitest` replaces the companion link on USB with a line
console (`ui-freenove/UiTestConsole.cpp`), 115200 baud, for driving the UI
from a host script. Not for daily use; flash the product env afterwards.

| Command | Effect |
|---------|--------|
| `tap X Y` | press + release at (X, Y), 480x320 landscape |
| `swipe X1 Y1 X2 Y2 MS` | press, move over MS ms, release |
| `shot [A B]` | the glass (FramePusher's RGB565 shadow), all rows or A..B: `SHOT 480 320`, one `Q<row> <base64>` per row (runs of count, pixel lo, pixel hi), `END SHOT` |
| `page N` | show page N (0 HOME .. 4 HEAD) |
| `status` | page, display on/off, companion screen / admin state, contact count, home repeater, uptime |
| `log` | the admin console's terminal log (tail); a password is only ever `login ****` |
| `contacts` | the contact table: type, key prefix, lastmod, fav/home, name |
| `inject CH FROM TEXT` | a received message into the UI's store as if heard (CH = channel index, -1 = DM from FROM; `_` in FROM = space); nothing is sent |
| `wake` | wake the screen as a touch would |
| `ping` | liveness |

Every command answers `OK <cmd>` or `ERR <why>`; `tap` and `swipe` also
answer `DONE <cmd>` once the gesture is released.

## Hardware

| Function | GPIO |
|----------|------|
| QSPI CS / SCLK | 10 / 12 |
| QSPI D0 / D1 / D2 / D3 | 11 / 13 / 14 / 9 (SPI2, 80 MHz) |
| Backlight (active high, LEDC ch 7) | 41 |
| Touch I2C SDA / SCL (`Wire`, also ES8311) | 38 / 39 |
| Touch reset / interrupt | 48 / 47 |
| BOOT button | 0 |

Panel: ST77922 over IDF `spi_master` in quad mode (Arduino-ESP32 2.0.17's
IDF 4.4 esp_lcd has no QSPI panel IO). Commands are opcode `02`, address
`00 cmd 00`, parameters on one line; pixels are opcode `32`, address `00 2C
00`, data on four lines, RGB565 big-endian. The panel stays portrait;
landscape rows are transposed into panel columns on push, in bands of 4 rows
(the ST77922 column window must start and end on 4-pixel boundaries).

Colour inversion: this glass shows inverted colours with INVOFF (navy as
tan/white), so init sends INVON (`FREENOVE_PANEL_INVERT=1`, the default in
`St77922Panel.h`). Set it to 0 only for a panel that shows inverted colours
with the default.

Frames: the pages draw into two 480x320 RGB565 framebuffers in PSRAM over a
cached per-page background layer, at most 60 fps and only when something
changed (new data, touch, animation). `FramePusher` compares the tracked
spans with a PSRAM shadow of the glass and pushes only the changed columns
(4-row aligned), on core 0 while core 1 draws the next frame; an unchanged
frame pushes nothing. A full-screen change is ~300 KB on the wire (~23 ms).
Text uses anti-aliased D-DIN bitmaps (`FbFontsAA.cpp`).

Touch: I2C 0x55, 16-bit register addresses. `0x0010` bit 3 = report holds
points; 7-byte records from `0x0014`; `0x0005..0x0008` = touch resolution.

## Bring-up

Flash `Freenove_S3_35_Head_companion_bringup`. Expected:

- USB log: `freenove: framebuffers 2x153600 B PSRAM; free internal ...`,
  `st77922 touch: fw=.. res=WxH points=N`.
- Screen: eight vertical bars, left to right white, yellow, cyan, green,
  magenta, red, blue, black; 24 px corner squares (dark-grey border):
  top-left orange, top-right teal, bottom-left purple, bottom-right grey.
  Then labelled colour bars through the RGB565 and RGB332 paths.
- Touching a corner prints `touch x=.. y=.. (raw ..,..)` in landscape pixels
  (0..479, 0..319), and `touch up` on release.
- Then the framebuffer demo: bouncing glow dots, a marching dashed line, HUD
  text. Modes rotate every 10 s: `anim`, `idle` (nothing to push) and `full`
  (whole screen changes every frame). Once per second:
  `demo mode=anim fps=.. render_us=.. cpu1=..% diff_us=.. push_us=..
  push_busy=..% kb/frame=.. bands/frame=.. pushed=../.. dropped=.. spi_err=..`

Fix-ups (uncomment in `platformio.ini`):

- Picture upside down (bars run black..white, orange corner bottom-right):
  `-D FREENOVE_LANDSCAPE_FLIP=1` (rotates both picture and touch 180 degrees).
- Picture mirrored: `-D FREENOVE_MADCTL=0x40` (columns) or `0x80` (rows).
- Picture right but touch mirrored: `-D FREENOVE_TOUCH_MIRROR=1` (X), `2` (Y)
  or `3` (both), in panel (portrait) space.

## Sources

- Panel protocol and command flow: Espressif `esp_lcd_st77922`
  (Apache-2.0), adapted; see `St77922Panel.cpp`.
- 320x480 init table: xiaozhi-esp32 `lcdwiki-es3c35p` board (MIT), same
  panel and pinout; see `St77922Panel.cpp`.
- Fonts: anti-aliased D-DIN / D-DIN Bold (Datto, SIL OFL 1.1,
  `ui-freenove/LICENSE.D-DIN-OFL.txt`; `tools/gen_aa_fonts.py`); bring-up
  bitmap fonts 6x8 from Adafruit-GFX glcdfont (BSD) and 8x16 from DejaVu Sans
  Mono Bold (`ui-freenove/LICENSE.DejaVu`; `tools/gen_fonts.py`).
- No Freenove vendor, SquatchWatch-CYD or WiChat code is used.
