# ESP-NOW repeater heads (CYD and Freenove)

Branches `head/cyd`, `head/freenove` and `head/cyd-hmi`, on top of
`espnow/bridge-improvements` and `espnow/observer-feed`.

A touch head for a LoRa repeater with the ESP-NOW bridge: an ESP-NOW-only
MeshCore companion (`MESH_PRIMARY_ESPNOW`, no LoRa) that chats through the
repeater and administers it from the screen. Both heads also draw the
repeater's observer feed (`set espnow.observer on` on the repeater) on the
same pages: HOME, TELEMETRY, MAP, NODES and HEAD (the companion screens in
the same HMI style). One page codebase (`examples/companion_radio/ui-freenove`)
with two screen profiles: the Freenove draws 480x320 frames in PSRAM, the CYD
(no PSRAM) draws 320x240 frames in 320x40 bands (`HMI_PROFILE_CYD`).

| Head | Env | Setup |
|------|-----|-------|
| CYD ESP32-2432S028 (ST7789 320x240, XPT2046 touch) | `CYD_ESPNOW_Head_companion_usb` | `variants/cyd_espnow_head/README.md` |
| Freenove FNK0104N (ESP32-S3, ST77922 480x320, touch) | `Freenove_S3_35_Head_companion_usb` | `variants/freenove_s3_35_head/README.md` |

On the repeater: `set espnow.enabled on`, `set bridge.format raw`, and
`bridge.channel` equal to the head's `espnow.channel` (default 6); for the
pages also `set espnow.observer on`.

## On-screen keyboard (per head)

- **Freenove (PC layout, `TOUCH_KEYBOARD_PC_LAYOUT=1`):** a shift key
  (abc / ABC, one capital) left of the third row; the bottom row is 123 /
  #+=, space, X (cancel) and the action key. 123 holds the digits, their
  shifted symbols `!@#$%^&*()` and `-_=+.,?`; #+= holds
  `` ~`{}[]<>\| ``, `;:'"/` and `-_=+.,?`; abc returns to letters.
  Checked on the glass (2026-11-04).
- **CYD (mode key):** one mode key (left of the third row, labelled with
  the current mode, lit when not lower case) cycles abc -> ABC -> 123 ->
  abc; ABC stays capitals until cycled. 123 is the phone layout:
  `1234567890`, `-/:;()$&@`, `.,?!'"#`. The bottom row is space, X and the
  action key. Not typeable on the CYD's screen keyboard:
  `% ^ * + = _ ~ { } [ ] < > \ |` and the backtick (use the USB terminal).

On both, every key that can be touched is drawn; on the CYD's HEAD page the
rows shrink (32 / 36 px) so all four fit.

## Screen timeout and gesture hints (both heads)

- The screen turns off 2 minutes after the last touch (fresh installs; a
  head that still had the old 15 s default moves to 2 min once). The
  companion home carousel's "Screen" page changes it: tap the centre to
  cycle 30 s, 1 min, 2 min, 5 min, Never. Saved with the display settings
  (`display.timeout` / `display.mode`). A touch on a dark screen only wakes
  it.
- Faint chevrons show where the screen swipes or scrolls: left / right on
  the companion home carousel (its Previous / Next touch zones, drawn as
  columns with a thin divider),
  up / down at the ends of the NODES list and of the chats conversation
  list (only where there is more). Each is a real tap target (a page per
  tap) and brightens for 1.5 s when the screen appears or moves. The chats
  thread, settings list and repeater picker keep their Up / Down buttons.

## CYD screens (320x240)

Real screenshots from the CYD (`CYD_ESPNOW_Head_uitest`, frames as
composed for the panel), quiet area; chat contents are injected test
messages, the login field holds four masked test digits, node names and
positions are public adverts. The CYD has no animations (the route is
highlighted, gauges and counters just update) and shows fewer panels:
TELEMETRY leaves out the SNR waterfall and ACTIVE NODES, HOME has four quick
tiles (ADVERT, PRESET, BRIDGE, ADMIN; OBSERVER and TRACE are left out, the
BRIDGE and ADMIN tiles open the same screens), NODES has no TYPE and KM
columns.

| | |
|-|-|
| ![HOME](images/cyd_home.png) HOME | ![HOME confirm](images/cyd_home_confirm.png) HOME, ADVERT armed (CONFIRM?) |
| ![TELEMETRY](images/cyd_telemetry.png) TELEMETRY | ![MAP](images/cyd_map.png) MAP (repeater without a GPS fix) |
| ![NODES](images/cyd_nodes.png) NODES, sorted by SNR | ![Node card](images/cyd_node_card.png) Node card |
| ![HEAD](images/cyd_head.png) HEAD: companion home | ![Chats](images/cyd_chats.png) Chats list |
| ![Thread](images/cyd_chat_thread.png) Chat thread | ![Compose](images/cyd_compose_123.png) Compose, 123 mode |
| ![Login](images/cyd_login.png) Login entry (masked) | ![Console](images/cyd_console.png) Admin console |
| ![Picker](images/cyd_picker.png) Repeater picker | |

## Freenove screens (480x320)

Freenove head, 480x320. Real screenshots are from a quiet area (few nodes);
chat contents are injected test messages, passwords are masked, node names
and positions are public adverts. The busy HOME image is rendered by the same
page code with sample data (`test/test_layouts.py --preview`), not taken
from a device.

| | |
|-|-|
| ![HOME](images/home.png) HOME: topology around the home repeater, noise floor, packet counter, quick controls | ![HOME busy](images/home_busy_render.png) HOME on a busy mesh: rendered by the same page code with sample data |
| ![HOME card](images/home_card.png) HOME with a node card open | ![TELEMETRY](images/telemetry.png) TELEMETRY: repeater vitals |
| ![MAP](images/map.png) MAP: node positions from adverts | ![NODES](images/nodes.png) NODES: sortable, SNR and the relay it came through |
| ![Node card](images/node_card.png) Node detail card | ![Chats](images/chats.png) Chats list |
| ![Thread](images/chat_thread.png) Chat thread | ![Picker](images/repeater_picker.png) Repeater picker, home repeater first |
| ![Login](images/login.png) Login (password masked) | ![Presets](images/presets.png) Radio presets |
| ![Keyboard](images/keyboard_123.png) Keyboard, PC-style 123 page (the Freenove's layout) | |
