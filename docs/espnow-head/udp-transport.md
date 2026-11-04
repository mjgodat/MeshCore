# Heads over UDP/IP (optional)

ESP-NOW is, and stays, the default link between a repeater and its visual
heads: no WiFi network, no router, no login. A head and a repeater can also
use an existing WiFi network, through Cascade's WiFi UDP bridge
(`docs/udp_bridge_setup.md`). Both sides opt in separately; an owner who
never changes these settings sees no difference.

> **Before pairing over UDP: give the repeater a `bridge.secret` that is
> NOT its admin password.** The head stores the secret unencrypted in its
> NVS (anyone holding the head can read it), and it authenticates every UDP
> frame. See [Rotating bridge.secret](#rotating-bridgesecret).

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

- Neither side ever moves its ESP-NOW channel to follow WiFi; ESP-NOW
  peers are never lost behind the owner's back.
- Joining never blocks the mesh loop: an asynchronous scan of the ESP-NOW
  channel only, then WiFi's asynchronous join to the BSSID found there.
  Failed attempts back off 15 s, 30 s, 60 s, then every 2 minutes.
- If the router changes channel while joined (auto-channel, channel-switch
  announcement), the station is disconnected at once, the ESP-NOW channel
  restored and UDP pauses (repeater `get udp.status`:
  `ap-moved-off-espnow-channel`; head: `udp wrong-channel`). It rejoins only
  when the router is back on the ESP-NOW channel. Pin the router's channel.
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
set bridge.secret <secret>       (1-15 characters, NOT the admin password)
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

## Rotating bridge.secret

`bridge.secret` is shared by every node that uses it: the repeater's UDP
peers (the heads in UDP mode) and, in `wrapped` ESP-NOW format only, other
bridges. Heads on plain ESP-NOW (`raw`) do not use it. To change it:

1. On the repeater: `set bridge.secret <new secret>` (1-15 characters, not
   the admin password). UDP sessions restart at once; the heads lose UDP.
2. On each head that uses UDP: `set udp.secret <new secret>` (USB terminal,
   or the Link page; heads in `auto`/`both` keep working over ESP-NOW
   meanwhile, a `udp`-only head needs USB or a temporary
   `set transport espnow`).
3. On any other bridge using `wrapped` format: `set bridge.secret` to the
   same value.
4. Check `get transport` on each head (`udp ready`) and `get udp.status` on
   the repeater (`ready=` counts the heads).

## Security

- UDP payloads are **authenticated, not encrypted** (HMAC-SHA256 with
  `bridge.secret`, per-boot challenge handshake, replay-checked sequence
  numbers). Anyone on the network can read them.
- Observer frames contain raw LoRa packets the repeater received (already
  public on air; DMs and channel messages stay MeshCore-encrypted), the
  repeater's vitals (noise floor, rates, battery) and its saved position.
- Mesh frames carry ordinary MeshCore packets (same exposure as on LoRa).
- The head keeps the WiFi password and the secret in its NVS
  **unencrypted**; they are never printed, logged or shown, and the keyboard
  entry is masked, but anyone with the device can read flash. Hence a
  `bridge.secret` distinct from the admin password.
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

Needs the ThinkNode M5 flashed with `ThinkNode_M5_Repeater_bridge_espnow_udp`
(keep the known-good image and USB access ready), a head (CYD or Freenove)
with this firmware, and a 2.4 GHz network. Keep a USB terminal open on both.

1. **Boot the `_udp` image with UDP off.** Do not set `udp.enabled`. For at
   least 10 minutes confirm LoRa RX/TX (`stats`, neighbours), the ESP-NOW
   bridge, the head's pages, chat and admin login are as before.
   `get observer.transport` shows `> espnow ...`; on the head
   `get transport` shows `> espnow; ch 6 vitals <n>s`.
2. **Pin the router to channel 6** (the ESP-NOW channel; fixed, not auto).
   Set a `bridge.secret` that is not the admin password.
3. **Enable UDP on the repeater.** `set wifi.ssid`, `set wifi.pwd`,
   `set udp.peer.1 <head IP>:4404`, `set bridge.source tx`,
   `set udp.enabled on`. Watch `get udp.status` (`ready` once the head is
   up), heap, and the LoRa RX counters and neighbour list for 15 minutes.
4. **Head UDP only.** Pair the head (`udp.ssid`, `udp.password`, `udp.peer`,
   `udp.secret`), `set transport udp`; `get transport` shows `udp ready` and
   `vitals` counting. Repeater `set observer.transport udp`;
   `get udp.stats obs` shows `subs=1` and `ok` rising. Pages update; with
   the M5's ESP-NOW off (`set espnow.enabled off`) they still update; log in,
   run a command, send a channel message. `set espnow.enabled on` again.
5. **Both.** Repeater `set observer.transport both`, head `set transport both`.
   Pages update once per frame (packet counter does not double); both `obs`
   pages rise.
6. **Auto fail-over.** Head `set transport auto` (`via espnow`). Repeater
   `set espnow.enabled off`: after ~30 s `via udp`, chat and login still
   work. `set espnow.enabled on`: after 3 vitals and at least 30 s on UDP,
   back to `via espnow`; it does not switch again for at least 30 s.
7. **Channel safety (F1).** With everything joined, change the router's
   channel to another one (e.g. 11). Within a second or two: the repeater's
   `get udp.status` shows `ap-moved-off-espnow-channel`, the head shows
   `udp wrong-channel`, ESP-NOW chat between head and repeater and the
   head's pages keep working (ESP-NOW stayed on ch 6), LoRa is unaffected.
   `set observer.transport both` (repeater) and `set transport auto` (head)
   are refused with the fix in the reply. Put the router back on 6: both
   rejoin within the back-off (at most 2 minutes).
8. **Roll back.** Head `set transport espnow`; repeater
   `set observer.transport espnow`, `set udp.enabled off`. Confirm the
   step-1 baseline. `get udp` on the head shows `password=set secret=set`;
   no output anywhere contains them.

### Results, 2026-11-03 (ThinkNode M5 + CYD, router fixed on channel 2)

Run on channel 2 because the router could not be moved to 6; both devices
were put on channel 2 for the test and back on 6 afterwards.

| Step | Result |
|------|--------|
| 1 `_udp` image, UDP off: LoRa, ESP-NOW, pages, login as before | PASS |
| 2-3 Pinned channel, separate setup | PASS (channel 2 on both) |
| 3-4 UDP only: vitals, pages, login and Ver over UDP | PASS, but see the blocker below |
| 5 both: one copy of each frame (packet counter not doubled) | PASS |
| 6 auto: fail-over after ~30 s without ESP-NOW vitals, fail-back, no flapping | PASS |
| 7 Channel safety (simulated by moving the repeater's bridge channel) | FAIL: the bridge stopped instead of the station |
| 8 Roll back | PASS |

Found on hardware, fixed since, and re-tested in the second test below:

- **Panic when WiFi first starts while the ESP-NOW bridge runs.** The
  bridge started the WiFi driver through ESP-IDF without a network
  interface; Arduino's first `WiFi.mode(WIFI_STA)` then created the station
  netif after the driver's `STA_START` event, so the netif never started
  (no lwIP input, `dhcp client start failed`, a null call in
  `esp_netif_receive`). In UDP builds the bridge now starts the radio
  through Arduino's `WiFi.mode(WIFI_STA)`, as the heads do: the netif exists
  before `esp_wifi_start` and is started by the driver's own `STA_START`.
  The station join refuses a radio started without Arduino instead of
  creating a late netif; it never stops or re-initialises the radio.
  Non-UDP bridge images keep their ESP-IDF-only start unchanged.
- **A bridge channel change while joined elsewhere stopped the bridge.**
  Now the station is dropped first (`wrong-channel`), the bridge starts on
  the new channel, a lost race is retried every 2 s, and the station
  rejoins only on the bridge channel. The M5 coming back with the bridge
  off after the rollback was a different cause: the UDP image saved its
  bridge intent without the marker the Full image expects; it now saves it.
- **Clearing.** `set wifi.ssid off` on the repeater (password included),
  `set udp.ssid off`, `set udp.secret off`, `set udp.password off`,
  `set udp.peer off` on the heads. `off` is therefore reserved in these
  settings: a network (or secret) literally named `off` cannot be set.
- **Bridge intent marker.** Every dedicated ESP-NOW bridge image (the
  default `ThinkNode_M5_Repeater_bridge_espnow`, the `_udp` env, the DevKit
  relay's bridge builds) now saves its bridge on/off as the explicit
  ESP-NOW intent with the Full-profile marker. Harmless for those images
  (they read `bridge_enabled` as before); it only lets a Full image flashed
  over them keep the bridge on.
- **Partition table.** `ThinkNode_M5_Repeater_bridge_espnow_udp` now uses
  the M5's shipped Full layout (`variants/dual_ota_full_4MB.csv`, 1984 KiB
  app slots, byte-identical `partitions.bin`); the image fills 93% of a
  slot. The upstream `ThinkNode_M5_Repeater_bridge_espnow` env keeps
  upstream's `min_spiffs.csv` (1920 KiB slots + coredump, SPIFFS 128 KiB),
  which differs from the owner's M5. It is not changed here: a full flash of
  a different table would shrink SPIFFS for everyone else using that env.
  Flash it to the owner's M5 app-only at 0x10000 only after checking that
  the image fits 1984 KiB and the device boots from ota_0 (as done on
  2026-11-03), or use the `_udp` env.

### Second supervised test (after the fixes)

Same setup as above (router on channel 2 unless it can be moved to 6;
rollback image and USB access ready). Check `partitions.bin` of the build
equals the M5's table before writing app-only at 0x10000.

1. **Baseline, UDP off**, as step 1 above.
2. **Runtime enable, no panic.** With the bridge running: `set wifi.ssid`,
   `set wifi.pwd`, peers, `set udp.enabled on`. Check `powerlog` and
   `stats-core` uptime: no panic/reboot; `get udp.status` ready; head
   pages live over UDP; login and Ver over UDP. Repeat `udp.enabled off` /
   `on` twice; no reboot.
3. **both / auto**, as steps 5-6 above.
4. **Channel change while joined.** `set bridge.channel 6` with WiFi on 2:
   reply `OK`, `get espnow.status` `ready ch=6/6`, `get udp.status`
   `wrong-channel`, ESP-NOW heads on 6 keep working; `get espnow.enabled`
   stays on. Then `set bridge.channel 2`: the station rejoins within 2 min.
5. **LoRa during UDP**: `stats-packets` recv climbing throughout.
6. **Clearing and roll back.** Repeater: `set udp.enabled off`,
   `set wifi.ssid off` (`get wifi.ssid` empty), `observer.transport espnow`,
   `bridge.channel 6`. Head: `set udp.ssid off`, `set udp.secret off`,
   `set udp.password off`, `set udp.peer off`, `transport espnow`,
   `espnow.channel 6`. Flash the rollback image app-only: `get espnow.enabled`
   on without any manual fix, `ver`, `get radio`, `get preset` as before.

### Results of the second supervised test, 2026-11-03 (espnow-head 749098ecd)

ThinkNode M5 + CYD, router on channel 2, both devices back on channel 6 at
the end.

| Step | Result |
|------|--------|
| 1 Baseline, UDP off | PASS |
| 2 UDP enabled at runtime with the bridge running; off/on twice | PASS: no panic, no reset, join and UDP vitals each time |
| 3 both (no duplicate frames) / auto (fail-over ~35 s, fail-back after 3 vitals) | PASS |
| 4 Bridge channel change while joined | PASS: bridge up on the new channel, WiFi dropped with its reason, bridge still enabled after a reboot, rejoined in 15 s on return |
| 5 LoRa receive throughout | PASS |
| 6 Clear with `off`, roll back | PASS: Full image back with the ESP-NOW bridge on by itself |

Open finding: after a reboot with UDP saved on while the access point is
not on the bridge channel, the bridge ran with WiFi modem sleep
(`ps=1 awake=0`) until the next bridge restart; the UDP station's power-save
choice is made before the bridge counts as constraining the channel.
