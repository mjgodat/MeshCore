# ESP32 DevKit ESP-NOW Repeater

A cheap, headless relay for the ESP-NOW side of the mesh: an ordinary
MeshCore repeater (own identity, adverts, neighbours, remote admin) whose only
radio is ESP-NOW. Put a few between a repeater head and a LoRa repeater's
ESP-NOW bridge to extend range across a yard or building.

Board: any classic ESP32 DevKit (ESP32-D0WD, 4 MB, no PSRAM). Reuses the
generic ESP-NOW target (`variants/generic_espnow`).

| Env | What |
|-----|------|
| `ESP32Dev_ESPNOW_repeater` | MeshCore repeater over ESP-NOW only |

## Network

- ESP-NOW channel **6**, matching the LoRa repeater's `bridge.channel`, the
  repeater head and (later) WiChat. Change with `set espnow.channel <1-13>`.
- The LoRa repeater's bridge must use `bridge.format raw` (plain MeshCore
  packets).
- Floods are repeated with normal MeshCore duplicate suppression, so several
  relays and the bridge never loop a packet.

## First setup (USB serial, 115200)

```text
password <new admin password>
set name <relay name>
```

The build default admin password is `password`; change it before deploying.
Then manage it remotely from a repeater head (Repeater page) or any companion.

## Defaults that differ from other repeaters

- **Power saving starts off.** Repeater power saving light-sleeps after five
  minutes without a USB data session and relies on the LoRa radio's interrupt
  to wake. ESP-NOW cannot wake the chip, so a relay powered from a charger
  would stop hearing anything. Leave `powersaving off`.

## Future: WiChat

WiChat will share this ESP-NOW channel. A WiChat-aware build of this relay is
planned; this build ignores non-MeshCore frames.
