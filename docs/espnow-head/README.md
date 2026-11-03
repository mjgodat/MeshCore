# ESP-NOW repeater heads (CYD and Freenove)

Branches `head/cyd` and `head/freenove`, on top of
`espnow/bridge-improvements` and `espnow/observer-feed`.

A touch head for a LoRa repeater with the ESP-NOW bridge: an ESP-NOW-only
MeshCore companion (`MESH_PRIMARY_ESPNOW`, no LoRa) that chats through the
repeater and administers it from the screen. The Freenove head also draws the
repeater's observer feed (`set espnow.observer on` on the repeater).

| Head | Env | Setup |
|------|-----|-------|
| CYD ESP32-2432S028 (ST7789 320x240, XPT2046 touch) | `CYD_ESPNOW_Head_companion_usb` | `variants/cyd_espnow_head/README.md` |
| Freenove FNK0104N (ESP32-S3, ST77922 480x320, touch) | `Freenove_S3_35_Head_companion_usb` | `variants/freenove_s3_35_head/README.md` |

On the repeater: `set espnow.enabled on`, `set bridge.format raw`, and
`bridge.channel` equal to the head's `espnow.channel` (default 6); for the
Freenove pages also `set espnow.observer on`.

## Screens

Freenove head, 480x320. Real screenshots are from a quiet area (few nodes);
chat contents are injected test messages, passwords are masked, node names
and positions are public adverts. The busy HOME image is rendered by the same
page code with sample data (`test/test_layouts.py --preview`), not taken
from a device. CYD images will come with the CYD convergence work.

| | |
|-|-|
| ![HOME](images/home.png) HOME: topology around the home repeater, noise floor, packet counter, quick controls | ![HOME busy](images/home_busy_render.png) HOME on a busy mesh: rendered by the same page code with sample data |
| ![HOME card](images/home_card.png) HOME with a node card open | ![TELEMETRY](images/telemetry.png) TELEMETRY: repeater vitals |
| ![MAP](images/map.png) MAP: node positions from adverts | ![NODES](images/nodes.png) NODES: sortable, SNR and the relay it came through |
| ![Node card](images/node_card.png) Node detail card | ![Chats](images/chats.png) Chats list |
| ![Thread](images/chat_thread.png) Chat thread | ![Picker](images/repeater_picker.png) Repeater picker, home repeater first |
| ![Login](images/login.png) Login (password masked) | ![Presets](images/presets.png) Radio presets |
| ![Keyboard](images/keyboard_123.png) Keyboard, PC-style 123 page | |
