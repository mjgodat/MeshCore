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
| `set udp.peer <repeater IPv4:port>\|off` | `off` | The repeater's address and its `udp.port`. `off` = learned from the repeater's announces ([discovery](#discovery-and-auto-pairing)); a set address always wins |
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
  In udp or auto mode, once ESP-NOW has been silent for the fail-over time
  (counted from boot when it was never heard), it runs one all-channel scan for that network (at most every 10 minutes;
  ESP-NOW receive pauses about 2-3 s while the radio visits the other
  channels, which costs nothing then) and reports it:
  `udp wrong-channel (AP on ch 11)` in `get transport`, a warning header on
  the Link screen and the Link page. While ESP-NOW is healthy the head never
  leaves its channel.
- `set transport both|auto` (head) and `set observer.transport both`
  (repeater) are refused while the access point is known to be on another
  channel: `Error: both refused. WiFi AP on ch 11, ESP-NOW on ch 6: set the
  router to ch 6 or ch 11 on all ESP-NOW nodes (bridge.channel/espnow.channel)`.
- To use a router on another channel, move every ESP-NOW node instead:
  `set bridge.channel <n>` on the repeater, `set espnow.channel <n>` (and a
  reboot) on every head and relay.

## Discovery and auto-pairing

Pairing no longer needs any IP address typed in. Discovery exists only in
repeater images with the UDP bridge (for the M5:
`ThinkNode_M5_Repeater_bridge_espnow_udp`). With the default
`bridge.announce auto`, the repeater announces itself while its UDP bridge
runs: every 5 s over ESP-NOW (on its bridge, in its `bridge.format`) and every
10 s over UDP (to the subnet broadcast address on its `udp.port`, and to each
UDP peer). `on` announces always, `off` never. An announce carries the
repeater's name, its key prefix, its ESP-NOW channel and format, its UDP
address and port, and what it offers. It never carries the WiFi password or
any secret, but anyone in range can read it. It is authenticated with an
8-byte HMAC-SHA256 tag keyed with a key derived from the secret
(PBKDF2-HMAC-SHA256, 2048 iterations, derived once and cached), so only a
head that has the secret uses it, and a recorded announce is a slow offline
guessing target. The secret is the repeater's `udp.secret`, or its
`bridge.secret` while `udp.secret` is off.

On the head:

1. Set `udp.ssid`, `udp.password` and `udp.secret` once (the repeater's
   `udp.secret`, or its `bridge.secret` if it has none). Leave `udp.peer` at
   `off`.
2. `set transport auto` (or `udp` / `both`).
3. The first announce the head verifies, heard over ESP-NOW or over UDP,
   gives it the repeater's address (`get transport`: `..., udp.peer auto
   192.168.1.50:4404`; the Link page shows `Repeater: auto ...`). The head
   joins WiFi and answers the announce over UDP with its own address (signed
   the same way, echoing one of the repeater's last 12 announces; at most one
   answer a second, normally one every 30 s).

On the repeater, a verified, fresh answer, sent from the address it signs and
echoing a newer announce than that head's last answer (so a recorded answer
cannot be replayed or re-pointed), adds the head as a UDP peer at run
time (`get bridge.autopeers`, `get udp.status` `auto=1`): at most 4 heads, at
most 4 new ones per minute, each kept 5 minutes after its last answer (a head
answers every 30 s, and at once after the repeater reboots or the head's
address changes). Auto peers are never saved and only use free
`udp.peer.N` slots. The authenticated UDP session (Hello / Challenge / Proof,
replay window) then runs exactly as with manual peers; the head subscribes
to the observer feed as before.

Precedence: a manual `udp.peer` on the head and a manual `udp.peer.N` on the
repeater always win; a head already listed in `udp.peer.N` is not added
again. The learned address follows one repeater (key prefix); another
repeater's announces are ignored until the current one has been silent for
2 minutes. Replays are ignored too: within one repeater boot the announce
counter must grow, and a new boot id is accepted only after the old one has
been silent for 15 s (a reboot). A head with no home repeater yet takes the first announced
repeater as home; an existing home is never replaced by an announce (adverts
heard at 0 hops keep deciding, as before).

Turn it off with `set bridge.announce off` on the repeater: then pair by hand
as below. Requirements: the head's `udp.port` must equal the repeater's
`udp.port` (both default 4404) for the broadcast announce to reach it; a head
that hears the repeater over ESP-NOW learns the address there regardless.
Some access points drop broadcasts between clients (client or "AP"
isolation); the ESP-NOW announce, or manual pairing, still works then.

## Bridged-packet rate limit (`bridge.rxlimit`)

New packets arriving from the ESP-NOW and UDP bridges into the repeater (and
so towards its LoRa airtime) can share one budget: `set bridge.rxlimit
<0-6000>` packets per minute, bursts of a fifth of that. Default `0` = no
limit, so an upgrade changes nothing; suggested `300` (bursts of 60). Only
packets that pass the bridge's duplicate check count, so `espnow.repeat`
copies and echoes from relays or another bridge never spend it. Each unique
packet costs one token even when it arrives over both bridges (a head on
`transport both`): the limiter remembers the last 16 packets it admitted (by
their seen-table hash) and lets the other bridge's copy through free,
counted as `shared=`; the mesh then drops that copy as a duplicate. OTA
packets are always charged. Excess packets are dropped and counted (`get
bridge.rxlimit`: `passed= dropped= shared=`, and `filter` in `get
espnow.stats drops` / `get udp.stats drops`). Nothing changes on the wire.
Raise it for a busy bridge-to-bridge link.

## A separate UDP secret on the repeater (`udp.secret`)

`set udp.secret <1-15 characters>` on the repeater keys its UDP sessions and
discovery instead of `bridge.secret` (`off` = back to `bridge.secret`, the
default). The heads' `udp.secret` must equal it. It is never printed and the
UDP sessions restart when it changes. Use it when the ESP-NOW link is
`wrapped`: the wrapped format is network isolation, not secrecy (an XOR with
no nonce over largely known plaintext, so the secret can be recovered from a
recorded frame), and that key should not also open the UDP bridge.

## Wrapped format on heads and relays (optional)

Nothing changes by default: primary ESP-NOW nodes (heads, the DevKit relay,
`Generic_ESPNOW_*`) stay `raw`, and the repeater's `bridge.format` keeps its
upstream default. Both formats are supported; pick per installation:

| | `raw` | `wrapped` |
|-|-------|-----------|
| Interop | Stock MeshCore ESP-NOW firmware (`Generic_ESPNOW`, upstream heads) | Only nodes set to `wrapped` with the same secret |
| Setup | Nothing to manage | One secret on every node, switched together |
| Packet size | Full MeshCore packets (up to 255 bytes, two frames when needed) | Up to 246 bytes; observer records up to 231 raw bytes |
| Protection | MeshCore's own encryption only (DMs, channels) | Plus isolation from other ESP-NOW users nearby and blocking of casual injection; not secrecy (XOR, no nonce: the secret can be recovered from recorded frames) |

Discovery, auto-pairing and `bridge.rxlimit` work the same in both formats.

To use wrapped, primary ESP-NOW nodes speak the bridge's `wrapped` format: `set espnow.secret <bridge.secret>`, then
`set espnow.format wrapped`, then reboot (both settings apply at the next
boot; the secret is never printed). It is byte-for-byte the repeater's
`bridge.format wrapped`; the whole ESP-NOW link then needs the secret: mesh
packets, the observer feed (wrapped too; a packet record then carries at most
231 raw bytes, marked truncated) and the announces. Receive is strict on both
sides: a wrapped node drops raw frames (including any it queued at boot
before its saved format applied) and a raw node cannot read wrapped ones, so
every node on the channel must be switched. Wrapped is isolation, not
secrecy: MeshCore's own encryption still protects DMs and channel messages,
but the wrapped secret itself can be recovered from the air.

### Switching an installation to wrapped (optional procedure)

Only if wrapped is wanted. Do it in this order, **in one session**, so that no node is cut off while it
still needs to be reached over ESP-NOW. A repeater in `bridge.format wrapped`
also wraps its observer feed, so raw heads lose their pages (and chat) from
step 4 until they are rebooted wrapped in step 5: prepare the heads in step
2 before touching the repeater, and do not leave them raw. Keep USB access to
the repeater (or a LoRa admin path) and to the heads as the fallback. Before
flashing new firmware onto a repeater that is already `wrapped` (check
`get bridge.format`), prepare and reboot its heads in the same session.

1. **Pick a secret** (1-15 characters, not the admin password). On the
   repeater: `set bridge.secret <secret>`. If the repeater has no
   `udp.secret` of its own, UDP heads need `set udp.secret` to the same
   value; better, give the repeater its own `udp.secret` first (see above).
   `bridge.format` stays `raw` for now, so nothing changes on ESP-NOW yet.
2. **Prepare every head and relay** while they still talk raw:
   `set espnow.secret <secret>` and `set espnow.format wrapped`. Nothing
   changes until they reboot (`get espnow.format` shows `wrapped (saved; raw
   until reboot)`). Heads: USB terminal (local clients only). Relays: USB, or
   remote admin over the mesh.
3. **Reboot the relays, farthest from the repeater first**, so a relay never
   cuts the path to one behind it before that one is done. Each one drops off
   ESP-NOW until step 4.
4. **Switch the repeater:** `set bridge.format wrapped` (applies at once;
   from USB, from LoRa remote admin, or from a head's admin console: the
   change is applied even if the reply does not get back).
5. **Reboot the heads at once** (power cycle or `reboot` on the USB
   terminal). They come back wrapped and see the repeater again (pages, chat,
   login).
6. **Check:** on each node `get espnow.format` (`> wrapped (refused: not
   wrapped 0, wrong secret 0)`; a growing `wrong secret` count means a node
   with another secret), on the repeater `get espnow.stats rx` (`crc` not
   growing) and `get espnow.nodes`.

To go back, reverse it: `set espnow.format raw` on heads and relays (saved,
applies at reboot), reboot the relays farthest first, `set bridge.format raw`
on the repeater, reboot the heads. A node that ends up on the wrong format
is reached over USB (or rebooted after a USB `set espnow.format`).

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
set bridge.source tx             (the repeater's own replies reach the head)
set udp.enabled on               (joins WiFi now; no peer needed yet)
set udp.peer.1 <head IP>:4404    (once the head shows its address)
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

Manual pairing is still supported and always takes precedence over
[discovery](#discovery-and-auto-pairing). The head's IP is shown by `get transport` (and the Link screen) once it has
joined; enter it on the repeater as `udp.peer.N`. The repeater joins WiFi as
soon as `udp.enabled` is on (its address: `get udp.status`); peers only
decide who receives frames. `set udp.enabled off` leaves the network again
when UDP joined it (a network MQTT or WebConfig joined is left alone). A handshake-assisted
pairing was left out at first because anyone holding the secret could then
add peers; it now exists as [discovery](#discovery-and-auto-pairing)
(owner decision 2026-11-04), bounded (4 slots, 4 new heads per minute,
5-minute expiry) and switchable (`bridge.announce off`).

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
- Discovery frames are authenticated, not encrypted: anyone on the network
  or the ESP-NOW channel sees the repeater's name, key prefix (both already
  public in its adverts), channel and LAN address, and can try to guess the
  secret offline (slowed by the PBKDF2 key derivation). Without the secret no
  one can make the repeater add a peer; with it, at most 4 run-time peers, 4
  new per minute, each gone 5 minutes after its last answer. Answers are
  accepted only from the address they sign and only once per announce.
- `wrapped` ESP-NOW is isolation, not secrecy (see above); give the
  repeater its own `udp.secret` so the LAN key is not the wrapped key.
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

Health (kind 3), 24 bytes from version-1 repeaters: magic, version `1`,
kind `3`, then body version u8 (`1`), uptime_s u32 (keeps every frame
distinct for the heads' both-transport de-duplication), free heap u32,
minimum free heap since boot u32, largest free block u32, reset reason u8
(the ESP-IDF
`esp_reset_reason_t` value: 1 power-on, 3 software, 4 panic, 5 interrupt
WDT, 6 task WDT, 9 brownout, ...). The repeater sends one with the first
vitals after the feed starts and then every 30 s (status frame: not rate
limited, like vitals). Decoders accept 24 to 246 bytes and ignore what
follows the fields they know, so later versions only append. Heads from
before this frame do not know kind 3 and drop it (MeshCore rejects its first
byte); the vitals frame itself is unchanged, because those heads check its
length exactly.

The same observer frames travel over ESP-NOW (one frame per ESP-NOW
message); there is one format for both transports.

Discovery frames are plain datagrams (not `MCUD`), on ESP-NOW and UDP alike
(`src/helpers/bridges/DiscoveryFrame.h`, little-endian): magic `B0 42 44 43`,
version `1`, kind (`1` announce, 53 bytes; `2` reply, 51 bytes), body, then
8 tag bytes = the first 8 bytes of HMAC-SHA256(key = secret, `MeshCore
discovery v1` plus its NUL, then every byte before the tag). Announce body:
boot id u32, counter u32, key prefix [6], name [16] (NUL padded), ESP-NOW
channel u8 (0 = bridge off), format u8 (0 raw, 1 wrapped), capabilities u8
(1 ESP-NOW, 2 UDP, 4 observer, 8 observer on ESP-NOW, 16 observer on UDP,
32 accepts replies), UDP IPv4 [4], UDP port u16. Reply body: the announce's
boot id and counter, the head's key prefix [6] and name [16], flags u8
(1 = wants the observer feed), its UDP IPv4 [4] and port u16 (the repeater
uses the datagram's source address). On a wrapped ESP-NOW link they are
wrapped like everything else.

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
   stays on. Then `set bridge.channel 2`: the station looks on the new
   channel within about 1 s and rejoins.
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

Finding from step 4, fixed since (confirmed on hardware in the third
test below): after a reboot with UDP saved on while the access point was not
on the bridge channel, the bridge ran with WiFi modem sleep
(`ps=1 awake=0`). Now, whenever the ESP-NOW bridge runs, the radio stays
awake (`WIFI_PS_NONE`) in any order of events: every station owner's
modem-sleep request (UDP station, MQTT, WebConfig) is replaced by "awake"
while the bridge runs, the bridge sets it when it starts on a shared
station, and in the `_udp` M5 image every `esp_wifi_set_ps` call is wrapped
at link time (`-Wl,--wrap`) so a sleep request never reaches the driver
while the bridge runs; a 250 ms check in the bridge undoes anything that
slips past (counted). If the driver refuses (Bluetooth active) that check
backs off up to 10 s and logs once; `get espnow.stats awake` shows repairs
and refusals. `get wifi.powersave` shows the saved setting and that it is
effectively `none` while the bridge runs. When a bridge that started on a
shared station stops, the station owner's own preference is restored at
once.

### Results of the third supervised test, 2026-11-04 (espnow-head 671b684b7)

ThinkNode M5 (`_udp` image) + CYD, router on channel 2, both devices back on
channel 6 at the end. No reset other than the deliberate ones; LoRa receive
climbing throughout.

| Step | Result |
|------|--------|
| `ver` | PASS: `v1.17.1-dev-671b684b (Build: 04-Nov-2026)` |
| Join with no `udp.peer` set | PASS: `udp.enabled on` alone joins (`ready ... peers=0`) |
| UDP enabled at runtime; off/on twice | PASS: `off` leaves the network (`wifi.status` idle, `udp.status off ip=0.0.0.0`), `on` rejoins, no reset |
| both (no duplicates) / auto (fail-over, fail-back) | PASS |
| Radio awake: runtime enable, off/on, bridge channel change, reboot with UDP on (AP on and off the bridge channel), bridge off/on | PASS: `ps=0 awake=1` and `get espnow.stats awake` `ps=0 repairs=0 refused=0` every time |
| `set wifi.powersave min` while the bridge runs | PASS: `OK - saved (radio kept awake while the ESP-NOW bridge runs)`; `get` `min (saved; effective none while the ESP-NOW bridge runs)` |
| Head: no all-channel scan while ESP-NOW is healthy | PASS: only scans of the ESP-NOW channel |
| Head: all-channel diagnosis when ESP-NOW is silent | FAIL, fixed since: the result was lost to Arduino's early scan timeout; re-tested on the CYD alone: `udp wrong-channel (AP on ch 2)` (fix round 8) |
| Clear with `off`, roll back | PASS |

### Supervised test for discovery, wrapped heads and the rate limit

Images: the repeater from `ThinkNode_M5_Repeater_bridge_espnow_udp`, the
heads from `CYD_ESPNOW_Head_companion_usb` / `Freenove_S3_35_Head_companion_usb`,
optionally the DevKit relay from `ESP32Dev_ESPNOW_repeater`. Router on the
ESP-NOW channel as before; rollback images and USB on every node.

Steps 1-3, 5 and 6 run in `raw` (the default for heads and the
repeater's `bridge.format raw` used with heads); step 4 is optional, only
if the owner chooses wrapped.

1. **Baseline (raw).** Check `get bridge.format` on the repeater: with
   `raw` the heads stay raw. If it is `wrapped` today, either
   `set bridge.format raw` for this test, or prepare the heads first
   (`espnow.secret` = `bridge.secret`, `espnow.format wrapped`) and reboot
   them right after the repeater, or its heads lose the observer feed.
   Flash all; nothing else configured. `get bridge.announce`
   `> auto`; `get bridge.rxlimit` `> 0 (no limit)`; `get espnow.format` on
   the heads as prepared; pages, chat, login as before. Then
   `set bridge.rxlimit 300` and check `dropped=0` after 10 minutes of normal
   use. First check of discovery over UDP: does the head see the broadcast
   announce (`get transport` shows `repeater ...` while only WiFi connects
   them)?
2. **Auto-pairing.** Repeater: WiFi, `bridge.secret`, `udp.enabled on`, no
   `udp.peer.N`. Head: `udp.ssid`, `udp.password`, `udp.secret`, `udp.peer
   off`, `set transport auto`. Within ~30 s: head `get transport` shows
   `repeater <name> ch 6 raw ...s, udp.peer auto <repeater IP>:4404` and
   `udp ready`; repeater `get bridge.autopeers` lists the head,
   `get udp.status` `auto=1 ready=1`. With `observer.transport both`, auto
   fail-over to UDP works as in the earlier tests.
3. **Precedence and expiry.** Repeater `set udp.peer.1 <head IP>:4404`: the
   auto entry goes away within 5 min and nothing is duplicated. Head
   `set udp.peer <wrong IP>:4404`: the head stops using the learned address.
   Undo both. Power the head off: the repeater's auto peer disappears after
   about 5 minutes.
4. **Wrapped installation (optional).** Only if the owner chooses
   wrapped: follow "Switching an installation to wrapped" above, then repeat
   step 2 (discovery) and step 5 (rate limit) in wrapped. Afterwards: chat, login, pages (observer feed) as before; `get
   espnow.format` on each head `wrapped (refused: ... wrong secret 0)`.
   Put one head on another `espnow.secret` (and reboot it): it loses the
   repeater, its `wrong secret` count grows; restore. Then switch back to raw
   in the reverse order.
5. **Rate limit.** `set bridge.rxlimit 6`; send 10 channel messages quickly
   from a head: `get bridge.rxlimit` shows drops, LoRa is not flooded; with
   `espnow.repeat 3` on the head, its copies do not count. Then
   `set bridge.rxlimit 300` (or 0).
6. **Separate UDP secret.** Repeater `set udp.secret <other>`: the UDP heads
   drop off (`udp handshake`); `set udp.secret <other>` on each head: they
   come back via discovery. `get udp.secret` shows only `set`.

### Results of the discovery test, 2026-11-04 (espnow-head e02373f90, raw)

ThinkNode M5 (`_udp` image) + CYD (product image). Router on channel 2, so both
devices were on channel 2 for the UDP steps and back on 6 at the end. A
temporary `udp.secret` was used on both devices and cleared afterwards. Wrapped
was not exercised.

| Step | Result |
|------|--------|
| New defaults after flashing: `bridge.announce auto`, `bridge.rxlimit 0`, `udp.secret off` | PASS |
| `auto` without UDP: nothing announced; `on`: the head verifies the ESP-NOW announce | PASS |
| Subnet broadcast reaches other WiFi clients (no AP isolation on this router) | PASS |
| Pairing learned over ESP-NOW only (head on another `udp.port`): auto peer added, UDP session and UDP vitals; no IP typed | PASS |
| Pairing learned over UDP broadcast only (repeater `espnow.enabled off`, head rebooted): auto peer moved to the new port | PASS |
| Spoofed reply (signed address differs from sender) refused; replayed reply refused | PASS |
| `bridge.rxlimit 300`: `espnow.repeat 3` copies counted once (`dupe=2`); `rxlimit 1` drops; back to 0 | PASS |
| `auto` stops announcing when UDP is turned off | PASS |
| 10 min with UDP on: no reset (uptime continuous), LoRa receive climbing, no panic | PASS |
| Roll back to the previous image with config cleared | PASS |

### Known limits

- UDP payloads are authenticated, not encrypted; the head keeps the WiFi
  password and the secret unencrypted in its NVS.
- `bridge.rxlimit` charges a packet that arrives over both bridges once
  (fixed in the reliability round); its second copy still reaches the mesh,
  which drops it as a duplicate.
- Discovery over UDP needs the head's `udp.port` to equal the repeater's
  (both default 4404) and an access point that forwards broadcasts between
  clients; otherwise the head learns the address over ESP-NOW, or is
  paired by hand.
- In `wrapped` the repeater's observer records carry at most 231 raw bytes
  over ESP-NOW (235 in `raw` and over UDP); longer packets are marked
  truncated. Wrapped mesh packets are limited to 246 bytes, as on the bridge.
- The heads' on-screen keyboard cannot type `% ^ * + = _ ~ { } [ ] < > \ |`
  or the backtick: such passwords or secrets need the USB terminal for now
  (long-press symbols may be added to the keyboard later).
- `auto` detects ESP-NOW loss from the repeater's vitals, so it needs
  `observer on` and `observer.transport both` on the repeater.
- On builds with MQTT and the ESP-NOW bridge, MQTT's modem-sleep setting no
  longer applies while the bridge runs: the radio stays awake by design
  (more power).
- The upstream `ThinkNode_M5_Repeater_bridge_espnow` env keeps upstream's
  partition table (`min_spiffs.csv`), unlike the owner's M5; the
  `_udp` env matches it.
- The M5 UDP station itself asks for no modem sleep while the bridge is
  enabled, so when the bridge stops the radio stays awake until the next
  join; the saved `wifi.powersave` applies from then on.
- The refusal back-off (`refused=`) is host-tested only: the M5 repeater has
  no Bluetooth to make the driver refuse.
- The prompt retry after a channel change (fix round 8) is host-tested only;
  the M5 was not used for it. Between channel changes the station still
  backs off up to 2 min while the access point stays off the ESP-NOW
  channel (no beacon watching).
