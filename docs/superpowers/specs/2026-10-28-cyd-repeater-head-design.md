# CYD Repeater Head — Design

Date: 2026-10-28
Status: draft for review

## Intent

A headless MeshCore repeater (today: the M5 ThinkNode running Cascade, name
`CaptCallahan-Base`) gets a **portable, wireless head**: a cheap ESP32 touch
display (a "CYD") that shows what the repeater sees and lets the owner act on it
from several yards away — without a phone, a companion radio, or a USB cable.

The link between head and repeater is **ESP-NOW**, using the repeater's existing
ESP-NOW bridge. The head is a real MeshCore node whose "radio" is ESP-NOW; the
repeater is its antenna onto the LoRa mesh.

Why: the owner wants to "hack" a repeater into having a face, and to retire the
current proof-of-concept (`C:\Users\Micha\wichat\lora_term`), which hand-builds
encrypted Public-channel packets and pushes them over ESP-NOW. That PoC proved
the path works (CYD → ESP-NOW → M5 → LoRa flood, confirmed received on other
nodes) but is send-only, has a fake clock, and hardcodes everything.

## Goals

1. **See what the repeater sees.** Channel messages, recent adverts / nodes
   heard, and the repeater's own status (battery, noise floor, packet counts,
   neighbors, uptime, radio settings).
2. **Act on the repeater remotely.** Log in to the repeater as admin and run CLI
   commands from the head, using MeshCore's standard remote-admin mechanism
   (anonymous login request → session → CLI-data text messages, as the
   companion's `login` / `cmd` terminal commands already do).
3. **Send chats.** Compose on a touch keyboard and send on a channel, with a
   per-message **"Send as: <head name> / <repeater name>"** toggle for channel
   posts. "As repeater" posts are ordinary group texts whose text is
   `"<repeater name>: <message>"`, exactly what `lora_term` does today. Direct
   messages always go out as the head's own identity.
4. **Run on cheap hardware.** First target: the ESP32 CYD now on COM3 (classic
   ESP32, 4 MB flash, no PSRAM, 320×240). *Claude's call:* the panel/touch setup
   proven on that board by `wichat/lora_term` is ground truth — LovyanGFX
   `Panel_ST7789` + `Touch_XPT2046` (resistive, with the saved calibration flow).
   Older notes calling it ILI9341 or a 3.5" GT911 board are superseded. Second target,
   same code: Freenove FNK0104N ESP32-S3 3.5" (ST77922 QSPI, I2C touch at 0x55).

## Non-goals

- Feature parity with the Cardputer UI or the phone app. The owner asked for
  **essential functionality** and a **very simple GUI**, comparable to the stock
  MeshCore / Meshtastic on-device screens: a few pages you tap between.
- BLE / phone app support on the head.
- LoRa on the head.
- Multi-repeater management in v1 (one paired repeater; see Open items).
- Keeping `lora_term` working. It is retired by this project.

## Anti-goals (would be a failure even if it "works")

- Anything outside MeshCore's ethos: no private side-protocol, no backdoor CLI
  channel, no modified crypto, no packets that a stock MeshCore node would treat
  as malformed. The head must be an ordinary MeshCore node using ordinary
  MeshCore packets.
- Requiring custom firmware on the repeater for basic operation. The repeater
  runs stock Cascade; configuration changes are fine, firmware changes only if a
  gap is proven (see Repeater configuration).
- A head that makes the repeater less reliable as a repeater (e.g. breaking its
  LoRa radio — note the known issue that a *custom* ESP-NOW build on the
  ESP32-S3 M5 broke LoRa; the stock bridge does not).
- A UI that needs a manual to use.

## Identity and visibility (decided)

The head has **its own MeshCore identity** (key pair, name). It is needed anyway:
remote admin authenticates the head's key.

Following stock companion behavior, the head **does not advertise automatically**.
It stays effectively hidden until the owner taps "Advert" (zero-hop or flood),
same as a companion. Its home screen is about the **repeater**, not itself.

## Architecture

```
 [CYD head]  --ESP-NOW (raw MeshCore frames)-->  [M5 repeater: ESP-NOW bridge]  --LoRa-->  mesh
            <--ESP-NOW (packets bridged by M5)--
```

- **Head firmware** = a new board variant in the Cascade repo
  (`mikecarper/MeshCore`, branch `keymindCascade`; local worktree
  `C:\Users\Micha\mc_pr`), built on the existing companion example with
  `ESPNOWRadio` as the primary radio — the same combination as the existing
  `Generic_ESPNOW_comp_radio_usb` environment (`variants/generic_espnow`).
  This reuses the companion's contacts, channels, message store, remote-admin
  login and CLI-over-mesh, and USB terminal.
- **A simple touch UI** for the companion on these displays. Prefer extending an
  existing companion UI (`examples/companion_radio/ui-new` or `ui-orig`) over a
  new one, if it can drive a 320×240 touch panel within the ESP32's RAM.
  Builder's call which, with RAM/flash numbers to justify it.
- **Display/touch drivers:** reuse what already works on this hardware in
  `C:\Users\Micha\wichat` (`lora_term/` and `cyd_28/` for the 2.8" CYD,
  `cyd/` for the Freenove S3; LovyanGFX, saved touch calibration) where it fits
  the variant structure.

### Repeater configuration

The M5's ESP-NOW bridge settings (current values in brackets):

- `bridge.format` [wrapped] → must be **`raw`** so the bridge speaks the same
  frame format as `ESPNOWRadio` nodes. Verify this interoperates before anything
  else is built. Raw mode removes the bridge secret; acceptable because MeshCore
  packets carry their own end-to-end crypto and admin still needs the password.
  Raw mode will not start while the M5 runs a WiFi access point — check
  `get wifi` / `get espnow` on the M5 first and report before changing anything.
- `bridge.source` [tx] — only `rx` or `tx` exist:
  - `rx` forwards what the repeater hears, but **not** what it transmits, so the
    head would miss the repeater's replies to its own login and CLI commands.
  - `tx` forwards what the repeater transmits: its own replies plus every flood
    it repeats.
  - Expected choice: **`tx`**. The builder must verify what the head actually
    sees with `tx` (which received packets are *not* forwarded: duplicates,
    filtered, non-repeated directs) and report the gap. Adding an `rx+tx`
    ("both") option to the bridge is acceptable as a small, generally-useful
    upstream change **only if** that gap matters in practice.
- ESP-NOW channel: the M5 bridge uses channel 6 today; head must match.
- ESP-NOW PHY mode: `ESPNOWRadio` on dedicated targets uses LR-only; the
  builder must confirm the M5 bridge can exchange frames with it (or use the
  coexistence mask) — part of the first interoperability check.

### Shared ESP-NOW air: WiChat + MeshCore (owner requirement)

The owner's WiChat system (`C:\Users\Micha\wichat`) already uses ESP-NOW. The
ESP-NOW layer must carry **both** WiChat and MeshCore traffic.

**Two builds from one codebase (owner, 2026-10-28):** carrying WiChat is a plus,
not a requirement for the head, so every target ships in two flavours from the
same source, selected by the compile flag `HEAD_WICHAT`:

- **Mesh** (`HEAD_WICHAT` unset): MeshCore only. WiChat frames on the air are
  recognised by the demux rule below and silently dropped; no WiChat code or
  UI is compiled in.
- **Mesh + WiChat** (`-D HEAD_WICHAT=1`): adds the WiChat module (receive, relay,
  beacons, send) and the "WiChat" conversation/destination in the UI.

The Mesh build is built and proven first; the WiChat flavour is layered on
without changing Mesh-build behaviour.

Facts from the code (2026-10-28):

- WiChat ESP-NOW payload = the full encoded WiChat frame, i.e. a 24-byte 802.11
  data header (frame control `08 00`, BSSID `57:43:57:43:57:43` at offset 16)
  followed by the WiChat header with magic `0x5743` (LE) at offset 24
  (`wichat/node/src/radio_esp32.cpp`, `wichat/gateway/src/main.cpp`).
  Protocol mask 11B|LR, ESP-NOW rate `WIFI_PHY_RATE_LORA_250K`, default channel 1.
- MeshCore raw ESP-NOW frame = a serialized MeshCore packet, or a 2-frame
  fragment envelope starting `FE 4D 43 46` (`src/helpers/ESPNowRawFragmentation.h`).
- Wrapped bridge frame starts `C0 3E`.
- M5 bridge: protocol mask 11B|11G|11N|LR; channel 6 today.
- The first byte alone does **not** separate them (`0x08` is a valid MeshCore
  header), so the demux rule is: **WiChat ⇔ len ≥ 38 and BSSID at 16 matches and
  magic at 24 matches; everything else goes to the MeshCore path** (which
  already rejects malformed frames and handles the `FE` envelope).

Requirements:

1. **Head:** its ESP-NOW receive path demuxes per the rule above. MeshCore
   frames go to `ESPNOWRadio`; WiChat frames go to a WiChat handler. v1 shows
   WiChat chat messages in Messages as their own conversation ("WiChat") and can
   send to it; WiChat relay duties (TTL decrement, rebroadcast, dedup by msg_id)
   follow the existing WiChat node logic.
2. **WiChat nodes / CYDs** (`node/`, `cyd_28/`, `cyd_35/`, `cyd/`): their
   ESP-NOW receive path must not mis-parse MeshCore frames (drop anything that
   fails the WiChat signature), and **relay nodes rebroadcast MeshCore frames
   once** (dedup on a hash of the frame bytes, bounded seen-cache) so a few
   ESP32s can extend the range between head and repeater. MeshCore's own
   duplicate suppression (bridge seen-cache, mesh packet hashes) absorbs extra
   copies.
3. **One shared channel.** A single ESP32 radio can only listen on one channel.
   Head, WiChat nodes and the M5 bridge must use the same channel.
   *Claude's call:* **channel 6** (the live repeater's current bridge channel),
   set as the WiChat default (`WICHAT_DEFAULT_CH`) and used as the WiChat
   repeater idle-fallback channel; configurable on the head. Changing the M5
   instead (`set bridge.channel`) remains possible.
4. Raw 802.11 (promiscuous) WiChat transport is untouched on WiChat devices; the
   head only needs the ESP-NOW side.
5. `wichat/gateway` (wrapped-format WiChat↔M5 gateway, COM9, never deployed)
   and `wichat/lora_term` are superseded once the M5 runs `bridge.format raw`.

### Screens (v1, simple)

1. **Repeater** (home): name, battery, uptime, noise floor, last RSSI/SNR,
   packets RX/TX, neighbors count; logged-in state; refresh button.
2. **Messages**: recent channel and direct messages, newest first; tap to compose.
3. **Heard** (*Claude's call*): two sections. (a) **Neighbors** — the
   repeater's own zero-hop neighbor list with SNR and age, fetched via admin
   when logged in. (b) **Adverts** — adverts the head itself receives through
   the bridge, with age and hop count only. ESP-NOW reports no RSSI/SNR
   (`ESPNOWRadio::getLastRSSI/SNR` return 0) and zero-hop neighbor adverts are
   not forwarded under `bridge.source tx`, so no per-packet signal is shown and
   no repeater-side change is made for it in v1. Likewise there is no
   "heard by repeater" echo confirmation for sent channel messages (the bridge
   marks the head's own packets as seen); delivery status for DMs comes from
   normal MeshCore ACKs.
4. **Admin**: login (password entry), a short list of one-tap common commands
   (e.g. `ver`, `neighbors`, `get radio`, `advert`), and a free-text command
   line with the reply shown below.
5. **Compose** (from Messages): channel or contact, touch keyboard, "Send as"
   toggle for channel posts.
6. **Settings**: head name, paired repeater, ESP-NOW channel, brightness,
   "Advert (zero-hop / flood)".

Exact layout is the builder's call; must be readable at arm's length on 2.8".

## Constraints

- **Clock:** the CYD has no RTC. Remote admin and CLI requests are rejected if
  their timestamp is not newer than the last accepted one, so the head's clock
  must be correct and monotonic: take time from the repeater (its no-login
  clock request or the login reply), and persist a floor so a reboot never goes
  backwards. *Claude's call:* if the repeater's time looks unset (earlier than
  the head's build date), the head asks for manual time entry and can then
  offer to set the repeater's clock (`time <epoch>`) once logged in. The USB
  terminal's `time` command also works.
- **Fit on classic ESP32 (4 MB, no PSRAM).** Trim contacts/channels/history as
  needed; report flash/RAM per target. If the CYD cannot fit essentials, say so
  with numbers rather than silently cutting goals 1–3.
- **Repo rules:** Cascade `AGENTS.md` — one PlatformIO process at a time per
  checkout. Windows builds use `PLATFORMIO_CORE_DIR=C:\pio`.
- **Secrets** (admin password, bridge secret) are entered on the device or set
  via CLI; never hardcoded in committed source.
- **Hardware safety:** opening COM ports can reset boards; the M5 (COM19) is a
  live repeater — configuration changes to it are announced to the owner first.
- Nothing is committed or pushed without the owner's explicit OK.

## Decided by the owner

- Head = wireless portable head for a headless repeater, over ESP-NOW.
- Essential functionality, very simple GUI.
- Targets: CYD ESP32 preferred; ESP32-S3 (Freenove) also.
- Own identity is acceptable; hidden-vs-visible left to Claude within MeshCore's
  ethos.
- The ESP-NOW layer must handle both WiChat and MeshCore traffic.
- Two builds from the same code: one MeshCore-only, one MeshCore + WiChat
  (2026-10-28). WiChat is a plus, not a must.
- Approved to build after consolidating the review (2026-10-28).

## Claude's calls (owner can override)

- Own identity, no automatic adverts (companion convention).
- "Send as repeater" toggle for channel posts keeps the original "hack".
- Built as a Cascade variant on the companion + `ESPNOWRadio`, not a standalone
  firmware.
- `bridge.format raw`, `bridge.source tx` as the starting repeater config.
- One paired repeater in v1.

## Left to the builder

- Which companion UI to extend; page layout; touch keyboard design.
- Contact/channel/history limits per target.
- Variant naming and environment names.
- How the head is "paired" to its repeater (e.g. pick from heard repeaters, or
  enter/paste public key).

## Open items

- Multiple repeaters / a few relay ESP32s extending ESP-NOW range between head
  and repeater — the owner mentioned "a couple ESP32s"; out of v1, but don't
  design it out.
- Whether to propose any resulting repeater-side change (e.g. bridge "both")
  upstream — owner decides after v1.
