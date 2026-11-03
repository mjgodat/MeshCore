# Heads over UDP/IP (optional)

ESP-NOW is, and stays, the default link between a repeater and its visual
heads: no WiFi network, no router, no login. A head and a repeater can also
use an existing WiFi network, through Cascade's WiFi UDP bridge
(`docs/udp_bridge_setup.md`). Both sides opt in separately; an owner who
never changes these settings sees no difference.

## Settings

Repeater (any build with the ESP-NOW bridge; `udp` and `both` need a build
with the UDP bridge, for the ThinkNode M5 `ThinkNode_M5_Repeater_bridge_espnow_udp`):

| Command | Default | What |
|---------|---------|------|
| `set observer on\|off` | `off` | The observer feed itself (same switch as `espnow.observer`) |
| `set observer.transport espnow\|udp\|both` | `espnow` | Where the feed goes. `both` sends every frame on both, through one shared rate limit (20/s, burst 40) |
| `get observer.transport` | | Mode plus live state, e.g. `> both (espnow ch 6 running; udp ready subs=1)` |
| `get espnow.stats obs`, `get udp.stats obs` | | Per-transport counters: queued, ok, fail, rate, preempt, overflow (UDP: subs, peer.ok, peer.fail) |
| `set udp.enabled`, `udp.port`, `udp.peer.1..8`, `bridge.secret`, `wifi.ssid`, `wifi.pwd` | | Cascade's UDP bridge and WiFi settings, unchanged |

Head (CYD and Freenove product builds, `HEAD_UDP_TRANSPORT`): the companion
carousel's **Link** page opens the same settings on the screen (keyboard for
the text fields, the password and secret typed masked and never shown).
From the USB terminal:

| Command | Default | What |
|---------|---------|------|
| `set transport espnow\|udp\|both\|auto` | `espnow` | How this head reaches the repeater |
| `get transport` | | Mode, transport in use, ESP-NOW channel and vitals age, UDP state, e.g. `> auto via espnow; espnow ch 6 vitals 3s; udp ready 192.168.1.60:4404 vitals 4s` |
| `set transport.failover <10-600>` | `30` s | auto: no ESP-NOW vitals for this long -> send over UDP |
| `set udp.ssid <name>` | | WiFi network (spaces allowed) |
| `set udp.password [<8-63 chars>]` | | WiFi password; empty = open network. Never printed (`get udp.password` shows `set`) |
| `set udp.peer <repeater IPv4:port>\|off` | `off` | The repeater's address and its `udp.port` |
| `set udp.secret <1-15 chars>` | | The repeater's `bridge.secret`. Never printed |
| `set udp.port <1-65535>` | `4404` | Port this head listens on (the repeater's `udp.peer.N` must use it) |
| `get udp` | | All of the above, masked |

The head's settings are kept in its NVS (namespace `headlink`), apart from
the companion preferences and contacts.

Modes on the head:

| Mode | Mesh packets (chat, login, CLI) | Observer feed |
|------|--------------------------------|---------------|
| `espnow` | ESP-NOW | ESP-NOW. WiFi is never joined; the UDP code is not even allocated |
| `udp` | UDP only (ESP-NOW traffic ignored) | UDP only |
| `both` | sent on both, received on both (MeshCore drops the duplicate) | both; each frame shown once |
| `auto` | ESP-NOW while the repeater's ESP-NOW vitals arrive; after `transport.failover` s without them, UDP; back to ESP-NOW after 3 ESP-NOW vitals in a row and at least 30 s on UDP (no flapping). UDP lost -> ESP-NOW at once | both; each frame shown once |

`auto` needs the repeater's ESP-NOW vitals, so the repeater runs
`observer on` with `observer.transport both`. Observer frames received on
both transports are recognised by a hash of the whole frame (a record
carries the repeater's receive time, vitals its uptime), so the frame format
did not change and older heads and repeaters are unaffected.

## The channel rule

An ESP32 has one 2.4 GHz radio: ESP-NOW and a WiFi station always share one
channel. The rule here is the one Cascade already uses for ESP-NOW nodes:
**ESP-NOW keeps its configured channel, and a station joins an access point
only on that channel** (`mesh::wifi::beginStation`, which scans only the
ESP-NOW channel). The repeater's ESP-NOW bridge does the same, so the router
must be on the ESP-NOW channel (the heads' and bridge's default is 6) for
the repeater to join WiFi at all.

- A head never moves its ESP-NOW channel to follow WiFi; ESP-NOW peers are
  never lost behind the owner's back.
- If the head's network is on another channel, the head does not join it.
  It runs one all-channel scan for that network (at most every 10 minutes;
  ESP-NOW receive pauses about 1.5 s while the radio visits the other
  channels) and reports it: `udp wrong-channel (AP on ch 11)` in
  `get transport`, a warning header on the Link screen and the Link page.
- `set transport both|auto` (head) and `set observer.transport both`
  (repeater) are refused while the access point is known to be on another
  channel: `Error: both refused. WiFi AP on ch 11, ESP-NOW on ch 6: set the
  router to ch 6 or ch 11 on all ESP-NOW nodes (bridge.channel/espnow.channel)`.
- To use a router on another channel, move every ESP-NOW node instead:
  `set bridge.channel <n>` on the repeater, `set espnow.channel <n>` (and a
  reboot) on every head and relay.

## Pairing (manual)

The UDP bridge only talks to listed peers (`udp.peer.1..8`), and every frame
is authenticated with `bridge.secret`. Pairing is therefore two settings on
each side. Reserve both addresses on the router (the bridge stores IPv4
addresses, not names).

On the repeater (USB terminal, or the head's admin console over ESP-NOW):

```text
set wifi.ssid <network>          (and set wifi.pwd <password>, once)
set bridge.secret <secret>       (if not set yet; 1-15 characters)
set udp.port 4404
set udp.peer.1 <head IP>:4404
set bridge.source tx             (the repeater's own replies reach the head)
set udp.enabled on
set observer on
set observer.transport both      (or udp)
get udp.status
```

On the head (Link page, or USB terminal):

```text
set udp.ssid <network>
set udp.password <password>
set udp.peer <repeater IP>:4404
set udp.secret <the repeater's bridge.secret>
set transport auto               (or udp / both)
get transport                    (shows this head's IP once joined)
```

The head's IP is shown by `get transport` (and the Link screen) once it has
joined; enter it on the repeater as `udp.peer.N`. A handshake-assisted
pairing (the repeater adding a peer that proves the secret) was considered
and left out: anyone holding the secret could then add peers, which changes
Cascade's explicit allowlist model. With a static lease this is a one-time
step.

## Security

- UDP payloads are **authenticated, not encrypted** (HMAC-SHA256 with
  `bridge.secret`, per-boot challenge handshake, replay-checked sequence
  numbers). Anyone on the network can read them.
- Observer frames contain raw LoRa packets the repeater received (already
  public on air; DMs and channel messages stay MeshCore-encrypted), the
  repeater's vitals (noise floor, rates, battery) and its saved position.
- Mesh frames carry ordinary MeshCore packets (same exposure as on LoRa).
- The head keeps the WiFi password and the secret in its NVS; they are never
  printed, logged or shown, and the keyboard entry is masked.
- Use a trusted WPA2/WPA3 network; no port forwarding or Internet exposure
  is needed (see the client-isolation notes in `docs/udp_bridge_setup.md`).

## Wire format (for a desktop or web head)

A non-ESP32 head (a PC dashboard, for example) is out of scope for this
build but can be written from this section. Everything is in two
header-only C++11 files that compile on a PC: `src/helpers/bridges/UDPBridgeProtocol.h`
(framing, handshake, replay window) and `src/helpers/bridges/ObserverFrame.h`
(observer records); `test/test_observer_transport.py` shows them used on a
host.

UDP datagram (big-endian):

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | `MCUD` |
| 4 | 1 | version `1` |
| 5 | 1 | kind: 1 Hello, 2 Challenge, 3 Proof, 4 Mesh, 5 Probe, 6 Observer, 7 Subscribe |
| 6 | 2 | payload length |
| 8 | 8 | sender boot id (random per socket, never 0) |
| 16 | 8 | recipient boot id (0 only for Hello) |
| 24 | 4 | sequence (per sender boot, starts at 1, strictly increasing) |
| 28 | n | payload |
| 28+n | 16 | first 16 bytes of HMAC-SHA256(key = secret bytes, data = `MeshCore UDP bridge v1` plus its NUL byte (23 bytes), then bytes 0..28+n) |

Session: each side sends Hello (no payload) every second until it has a
session, then every 10 s. On a Hello, answer Challenge (recipient = the
Hello's sender, payload = 8 random bytes); on a Challenge addressed to you,
answer Proof with the same 8 bytes; a Proof that matches your challenge makes
its sender your peer's current boot. Data kinds (4-7) are accepted only from
that boot, addressed to your boot, with a sequence above the last one
accepted. Both directions must complete before data flows. Source address
and port must be the configured peer's.

Payloads: Mesh = one raw MeshCore packet (2-255 bytes). Subscribe = 1 byte
(`1` on, `0` off); the repeater keeps it 60 s, so a head re-sends it every
10 s. Observer = one observer frame (little-endian):

| Bytes | Packet record (kind 1) | Vitals (kind 2), 43 bytes |
|-------|------------------------|---------------------------|
| 0-3 | magic `B0 42 53 4F` | same |
| 4 | version `1` | same |
| 5 | kind `1` | kind `2` |
| then | rx_ms u32, snr (quarter dB) i8, rssi i16, disposition u8 (bit 7 = truncated), len u8, raw[len] (max 235) | uptime_s u32, noise_floor i16, rx/min u16, tx/min u16, dup/min u16, airtime per mille u16, ESP-NOW nodes u8, battery mV u16, lat_e6 i32, lon_e6 i32, has_pos u8, freq f32, bw f32, sf u8, cr u8, preset_slot u8 |

The same observer frames travel over ESP-NOW (one frame per ESP-NOW
message); there is one format for both transports.

## Owner test plan (hardware)

Needs the ThinkNode M5 flashed with `ThinkNode_M5_Repeater_bridge_espnow_udp`,
a head (CYD or Freenove) with this firmware, and a 2.4 GHz network on the
ESP-NOW channel (6 unless changed). Keep a USB terminal open on both.

1. **ESP-NOW unchanged.** Before any UDP setting: `get transport` on the head
   shows `> espnow; ch 6 vitals <n>s`; chat, login and the pages work as
   before. `get observer.transport` on the repeater shows `> espnow ...`.
2. **Repeater on WiFi.** Pair as above (`udp.enabled on`, `udp.peer.1 <head>`).
   `get udp.status` reaches `ready` once the head is up; `get wifi.status`
   shows the channel equals `bridge.channel`.
3. **Head UDP only.** On the head `set transport udp`; within ~30 s
   `get transport` shows `udp ready <ip>` and `vitals` counting. Repeater:
   `set observer.transport udp`; `get udp.stats obs` shows `subs=1` and
   `ok` rising. The head's HOME/TELEMETRY pages update; switch off the M5's
   ESP-NOW (`set espnow.enabled off`) and they still update; log in and run
   a command from the admin console; send a channel message. Switch ESP-NOW
   back on.
4. **Both.** Repeater `set observer.transport both`, head `set transport both`.
   Pages update once per frame (packet counter does not double); `get
   espnow.stats obs` and `get udp.stats obs` both rise; `rate` stays equal
   on both under load.
5. **Auto fail-over.** Head `set transport auto` (`get transport` -> `via
   espnow`). On the repeater `set espnow.enabled off`: after ~30 s
   (`transport.failover`) `get transport` shows `via udp`, chat and login
   still work. `set espnow.enabled on`: after 3 vitals (~15 s) and at least
   30 s on UDP, back to `via espnow`. Repeat with a shorter
   `set transport.failover 10`.
6. **Channel rule.** Move the router to another channel (or point
   `udp.ssid` at a network on another channel): the head shows
   `udp wrong-channel (AP on ch N)`, the Link page turns orange, ESP-NOW
   keeps working, and `set transport both` / `auto` is refused with the
   fix in the reply. On the repeater, `set observer.transport both` is
   refused while its station is on another channel.
7. **Back to default.** Head `set transport espnow`: WiFi disconnects,
   `get transport` shows `> espnow`, everything works over ESP-NOW;
   repeater `set observer.transport espnow`, `set udp.enabled off`.
8. **Secrets.** `get udp` shows `password=set secret=set`; no terminal or
   debug output contains them.
