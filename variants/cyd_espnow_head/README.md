# CYD ESP-NOW Repeater Head

A wireless, portable head for a headless LoRa repeater. The head is an
ordinary MeshCore companion with its own identity whose only radio is
ESP-NOW. It reaches the LoRa mesh through the repeater's built-in ESP-NOW
bridge, so it can read traffic, post to channels, send DMs and administer the
repeater (`login` / `cmd`) from across the yard without a phone.

Board: ESP32-2432S028 "Cheap Yellow Display" (ESP32-D0WD, 4 MB, no PSRAM).

## Builds

| Env | What |
|-----|------|
| `CYD_ESPNOW_Head_companion_usb` | MeshCore only, USB terminal |

## Repeater settings

On the LoRa repeater (any ESP32 repeater with the ESP-NOW bridge):

```text
set espnow.enabled on
set bridge.format raw
get bridge.source      (tx)
get bridge.channel     (6)
```

`raw` carries plain MeshCore packets, which the head (and every other ESP-NOW
MeshCore node) understands. The bridge secret only applies to the `wrapped`
format; MeshCore's own end-to-end encryption still protects messages. `raw`
does not start while the repeater runs a WiFi AP (`get wifi` must be off).

The head uses ESP-NOW channel **6** by default (`get/set espnow.channel`); it
must match `bridge.channel`.

## Defaults that differ from other companions

- **Power saving starts off.** With it on, the WiFi radio sleeps and the head
  hears nothing over ESP-NOW (sends never complete, `stats-radio-diag` shows
  `err_flags=8`). Leave `powersaving off`.
- **Flood retry count starts at 1.** The bridge never echoes the head's own
  flood packets back, so the companion's usual 15 "retry until heard"
  attempts are always spent (16 ESP-NOW frames per post). One retry covers a
  lost frame; the repeater deduplicates the copy.

## Using it over USB

The USB port opens the companion text terminal: `list`, `to <name>`,
`login <password>`, `cmd <command>`, `public <text>`, `channels`, `help`.

The head has no RTC. Set its clock (`time <epoch>`) after each power-up until
the clock screen takes time from the repeater; remote admin rejects requests
whose timestamps go backwards.

## What the head does not see (`bridge.source tx`)

The bridge forwards to ESP-NOW only what the repeater **transmits** on LoRa:

- Seen: everything the repeater repeats (floods, adverts it relays), its own
  adverts, and its replies to the head (login, CLI, status).
- Not seen: packets the repeater hears but does not repeat — zero-hop adverts
  from neighbours, direct traffic not routed through it, and duplicates.
- No "heard by repeater" echo for the head's own channel posts: the bridge
  marks them as already seen.

Verified 2026-10-28 against a ThinkNode M5 repeater (Cascade v1.18.0.1):
contacts learned from relayed adverts, `login`, `cmd ver`, `cmd neighbors`,
`cmd get bridge.format`, and Public posts reaching the repeater.

Remote replies take ~1.5 s round trip; the terminal's first login timeout
(~1 s) can report "timed out" even though the login succeeds.
