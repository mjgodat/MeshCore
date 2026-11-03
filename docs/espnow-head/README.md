# ESP-NOW repeater head (CYD)

Branch `head/cyd`, on top of `espnow/bridge-improvements` and
`espnow/observer-feed`.

A touch head for a LoRa repeater with the ESP-NOW bridge: an ESP-NOW-only
MeshCore companion (`MESH_PRIMARY_ESPNOW`, no LoRa) that chats through the
repeater and administers it from the screen.

- Board: CYD ESP32-2432S028 (ESP32, ST7789 320x240, XPT2046 touch).
- Env: `CYD_ESPNOW_Head_companion_usb`; setup in
  `variants/cyd_espnow_head/README.md`.
- On the repeater: `set espnow.enabled on`, `set bridge.format raw`, and
  `bridge.channel` equal to the head's `espnow.channel` (default 6).
- Screens (`examples/companion_radio/ui-new/Head*.h`, `UI_REPEATER_HEAD`):
  compose, repeater picker (home repeater first), login (masked, prefilled),
  admin console, settings, radio presets, on-screen keyboard with a
  PC-style 123 page.
- Contacts: a head auto-adds only its home repeater (the repeater heard at
  0 hops over the bridge); chat companions are saved on the first DM sent to
  them (`HeadContactPolicy.h`, `HeadHeardPeers.h`).
- Host tests: `test/test_head_helpers.py`, `test/test_head_contact_policy.py`.

Screenshots: CYD images will come with the CYD convergence work. The Freenove
head (`head/freenove`) has device screenshots of the same screens.
