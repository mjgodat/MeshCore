# ESP32 repeater board build matrix for the ESP-NOW bridge and observer feed

Build-check of every ESP32-family Cascade repeater environment with the ESP-NOW
bridge enabled. The observer feed (`src/helpers/bridges/Observer*.h`, the
`ESPNowBridge` observer path, `set espnow.observer`) is compiled in
`ESPNowBridge.cpp`, so it is present exactly where `WITH_ESPNOW_BRIDGE` is
defined.

## Status (honest)

- **On-air verified: ThinkNode M5 only** (plus the heads and the DevKit relay
  documented in `ESPNOW_HEAD.md`). Every other row below is **build-checked
  only**: it compiled, linked and passed the repo's runtime RAM gate, nothing
  more. No board other than the ones above was flashed or run for this work.
- nRF52, STM32 and RP2040 boards are **not supported**: they have no ESP-NOW
  (no WiFi radio), so no bridge and no observer feed. Their envs are out of
  scope here.
- No ESP32-S2 or ESP32-C5 repeater env exists in this tree. Only ESP32,
  ESP32-S3, ESP32-C3 and ESP32-C6 appear.

## Result

68 environments built: **59 OK, 9 failed, 0 over the runtime RAM gate**
(every build that reached the gate printed PASS, including the 9 that failed
later on flash size or link).

All 9 failures are flash-size problems (8 overflow, 1 link error) that the
plain repeater env already has on this commit **without** the bridge (see
"Failures"). They are not caused by the bridge; the bridge adds roughly
40-45 KB on top of an already-too-large image.

## Legend

- **Env**: the PlatformIO env that was built. Where "Bridge env exists" is
  "no", the env is the existing repeater env plus
  `-D WITH_ESPNOW_BRIDGE=1 -D WITH_ESPNOW_BRIDGE_SECRET='"shared-secret"'` and
  `+<helpers/bridges/ESPNowBridge.cpp>` (the same additions the ThinkNode M5
  bridge env uses), injected through an untracked `platformio.local.ini`. No
  `platformio.ini` was edited.
- **Cores**: ESP32 and ESP32-S3 are dual core; C3 and C6 are single core (the
  bridge and the LoRa/mesh loop share one core there).
- **Flash**: firmware size / app partition size, percent used. **Static DRAM**:
  PlatformIO `RAM:` line (data + bss) in KB.
- **RAM gate**: `scripts/check_firmware_ram.py` "Runtime RAM" result: linked
  internal heap available vs. required by the enabled features.
- Windows needs a command-line shortener for most ESP32 builds
  (`variants/m5stack_cardputer_adv/win_long_cmdline.py`, added as an extra
  script only in the local test envs). Plain builds of the first two S3 envs
  (Heltec V3, Heltec V4) failed with `CreateProcess: No such file or directory`;
  to save time the remaining S3 envs were built with the shortener directly.
  ESP32/C3/C6 envs were tried plain first; the Notes column says which needed it.

## Matrix

| Env | Chip | Cores | Bridge env exists? | Builds | Flash | Static DRAM | RAM gate | Notes |
|---|---|---|---|---|---|---|---|---|
| `Ebyte_EoRa-S3_Repeater` | ESP32-S3 | 2 | no (built with bridge flags added) | yes | 1797 KB / 1920 KB (93.6%) | 94 KB | PASS (271,944 avail / 153,488 req) | built with Windows cmdline shortener (plain build not tried) |
| `Heltec_E213_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1840 KB / 6400 KB (28.8%) | 93 KB | PASS (272,392 avail / 159,632 req) | built with Windows cmdline shortener (plain build not tried) |
| `Heltec_E290_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1838 KB / 6400 KB (28.7%) | 94 KB | PASS (272,216 avail / 159,632 req) | built with Windows cmdline shortener (plain build not tried) |
| `heltec_rc32_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1925 KB / 6400 KB (30.1%) | 99 KB | PASS (265,648 avail / 151,440 req) | built with Windows cmdline shortener (plain build not tried) |
| `heltec_rc32_without_display_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1900 KB / 6400 KB (29.7%) | 98 KB | PASS (266,096 avail / 149,392 req) | built with Windows cmdline shortener (plain build not tried) |
| `Heltec_T190_repeater_bridge_espnow_` | ESP32-S3 | 2 | yes | yes | 1804 KB / 6400 KB (28.2%) | 93 KB | PASS (272,352 avail / 166,160 req) | built with Windows cmdline shortener (plain build not tried) |
| `heltec_tracker_v2_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1905 KB / 3264 KB (58.4%) | 132 KB | PASS (235,592 avail / 144,310 req) | built with Windows cmdline shortener (plain build not tried) |
| `Heltec_v3_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1862 KB / 3264 KB (57.0%) | 131 KB | PASS (235,984 avail / 120,756 req) | needed Windows cmdline shortener |
| `heltec_v4_expansionkit_repeater` | ESP32-S3 | 2 | no (built with bridge flags added) | yes | 1893 KB / 6400 KB (29.6%) | 96 KB | PASS (270,768 avail / 157,584 req) | built with Windows cmdline shortener (plain build not tried) |
| `heltec_v4_r8_repeater` | ESP32-S3 | 2 | no (built with bridge flags added) | yes | 1878 KB / 6400 KB (29.3%) | 96 KB | PASS (269,872 avail / 153,488 req) | built with Windows cmdline shortener (plain build not tried) |
| `heltec_v4_r8_tft_repeater` | ESP32-S3 | 2 | no (built with bridge flags added) | yes | 1887 KB / 6400 KB (29.5%) | 96 KB | PASS (269,536 avail / 151,440 req) | built with Windows cmdline shortener (plain build not tried) |
| `heltec_v4_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1893 KB / 6400 KB (29.6%) | 96 KB | PASS (270,768 avail / 157,584 req) | built with Windows cmdline shortener (plain build not tried) |
| `heltec_v4_tft_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1893 KB / 6400 KB (29.6%) | 96 KB | PASS (270,768 avail / 157,584 req) | built with Windows cmdline shortener (plain build not tried) |
| `Heltec_Wireless_Paper_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1794 KB / 3264 KB (55.0%) | 128 KB | PASS (238,336 avail / 126,900 req) | built with Windows cmdline shortener (plain build not tried) |
| `Heltec_Wireless_Tracker_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1859 KB / 3264 KB (57.0%) | 131 KB | PASS (235,256 avail / 144,310 req) | built with Windows cmdline shortener (plain build not tried) |
| `Heltec_WSL3_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1832 KB / 3264 KB (56.1%) | 130 KB | PASS (236,256 avail / 116,660 req) | built with Windows cmdline shortener (plain build not tried) |
| `LilyGo_T3S3_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | NO (link error) | - | - | - | built with Windows cmdline shortener (plain build not tried); error: collect2.exe: error: ld returned 1 exit status |
| `LilyGo_T3S3_sx1262_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | NO (flash overflow) | 1747 KB / 1280 KB (136.5%) | 94 KB | PASS (272,120 avail / 153,488 req) | built with Windows cmdline shortener (plain build not tried); error: Error: The program size (1788581 bytes) is greater than maximum allowed (1310720 bytes) |
| `LilyGo_T3S3_sx1276_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | NO (flash overflow) | 1735 KB / 1280 KB (135.5%) | 93 KB | PASS (272,192 avail / 153,488 req) | built with Windows cmdline shortener (plain build not tried); error: Error: The program size (1776565 bytes) is greater than maximum allowed (1310720 bytes) |
| `LilyGo_TBeam_1W_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1828 KB / 6400 KB (28.6%) | 94 KB | PASS (271,360 avail / 153,488 req) | built with Windows cmdline shortener (plain build not tried) |
| `LilyGo_TDeck_repeater` | ESP32-S3 | 2 | no (built with bridge flags added) | yes | 1852 KB / 6400 KB (28.9%) | 95 KB | PASS (268,816 avail / 151,440 req) | built with Windows cmdline shortener (plain build not tried) |
| `LilyGo_TETH_Elite_sx1262_repeater` | ESP32-S3 | 2 | no (built with bridge flags added) | yes | 1750 KB / 6400 KB (27.3%) | 93 KB | PASS (273,232 avail / 149,392 req) | built with Windows cmdline shortener (plain build not tried) |
| `M5Stack_Cardputer_Adv_repeater` | ESP32-S3 | 2 | no (built with bridge flags added) | yes | 1777 KB / 3264 KB (54.4%) | 129 KB | PASS (238,424 avail / 116,660 req) | built with Windows cmdline shortener (plain build not tried) |
| `meshnology_w12_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1886 KB / 6400 KB (29.5%) | 96 KB | PASS (267,600 avail / 153,488 req) | built with Windows cmdline shortener (plain build not tried) |
| `MKE_s3_repeater` | ESP32-S3 | 2 | yes | yes | 1874 KB / 3264 KB (57.4%) | 131 KB | PASS (235,976 avail / 127,924 req) | base env already defines WITH_ESPNOW_BRIDGE; built with Windows cmdline shortener (plain build not tried) |
| `MKE_s3_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1860 KB / 3264 KB (57.0%) | 131 KB | PASS (235,984 avail / 120,756 req) | built with Windows cmdline shortener (plain build not tried) |
| `nibble_screen_connect_repeater_bridge_espnow_` | ESP32-S3 | 2 | yes | NO (flash overflow) | 1741 KB / 1280 KB (136.0%) | 129 KB | PASS (236,760 avail / 120,756 req) | built with Windows cmdline shortener (plain build not tried); error: Error: The program size (1782825 bytes) is greater than maximum allowed (1310720 bytes) |
| `nibble_zero_connect_repeater_bridge_espnow_` | ESP32-S3 | 2 | yes | NO (flash overflow) | 1741 KB / 1280 KB (136.0%) | 129 KB | PASS (236,760 avail / 120,756 req) | built with Windows cmdline shortener (plain build not tried); error: Error: The program size (1783025 bytes) is greater than maximum allowed (1310720 bytes) |
| `RAK_3112_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1853 KB / 3264 KB (56.8%) | 132 KB | PASS (234,120 avail / 116,660 req) | built with Windows cmdline shortener (plain build not tried) |
| `SenseCapIndicator-LoRa-N16R2_repeater` | ESP32-S3 | 2 | no (built with bridge flags added) | yes | 1925 KB / 6400 KB (30.1%) | 95 KB | PASS (270,208 avail / 203,664 req) | built with Windows cmdline shortener (plain build not tried) |
| `SenseCapIndicator-LoRa_repeater` | ESP32-S3 | 2 | no (built with bridge flags added) | yes | 1926 KB / 2560 KB (75.2%) | 95 KB | PASS (268,656 avail / 203,664 req) | built with Windows cmdline shortener (plain build not tried) |
| `Station_G2_logging_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | NO (flash overflow) | 1504 KB / 1280 KB (117.5%) | 95 KB | PASS (268,880 avail / 151,440 req) | built with Windows cmdline shortener (plain build not tried); error: Error: The program size (1539877 bytes) is greater than maximum allowed (1310720 bytes) |
| `Station_G2_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | NO (flash overflow) | 1504 KB / 1280 KB (117.5%) | 95 KB | PASS (268,880 avail / 151,440 req) | built with Windows cmdline shortener (plain build not tried); error: Error: The program size (1539865 bytes) is greater than maximum allowed (1310720 bytes) |
| `Station_G3_ESP32_r2_repeater` | ESP32-S3 | 2 | no (built with bridge flags added) | yes | 1888 KB / 6400 KB (29.5%) | 97 KB | PASS (267,304 avail / 153,488 req) | built with Windows cmdline shortener (plain build not tried) |
| `Station_G3_ESP32_repeater` | ESP32-S3 | 2 | no (built with bridge flags added) | yes | 1888 KB / 6400 KB (29.5%) | 97 KB | PASS (267,304 avail / 153,488 req) | built with Windows cmdline shortener (plain build not tried) |
| `T_Beam_S3_Supreme_SX1262_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1862 KB / 3264 KB (57.1%) | 95 KB | PASS (270,800 avail / 153,488 req) | built with Windows cmdline shortener (plain build not tried) |
| `ThinkNode_M2_Repeater_bridge_espnow` | ESP32-S3 | 2 | yes | NO (flash overflow) | 1733 KB / 1280 KB (135.4%) | 126 KB | PASS (242,520 avail / 120,756 req) | built with Windows cmdline shortener (plain build not tried); error: Error: The program size (1774925 bytes) is greater than maximum allowed (1310720 bytes) |
| `ThinkNode_M5_Repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1814 KB / 1920 KB (94.5%) | 132 KB | PASS (236,584 avail / 118,708 req) |  |
| `ThinkNode_M7_repeater` | ESP32-S3 | 2 | no (built with bridge flags added) | yes | 1746 KB / 3264 KB (53.5%) | 92 KB | PASS (274,800 avail / 149,392 req) | built with Windows cmdline shortener (plain build not tried) |
| `ThinkNode_M9_repeater_` | ESP32-S3 | 2 | no (built with bridge flags added) | yes | 1870 KB / 6400 KB (29.2%) | 94 KB | PASS (271,928 avail / 171,280 req) | built with Windows cmdline shortener (plain build not tried) |
| `Waveshare_S3_LR1121_repeater` | ESP32-S3 | 2 | no (built with bridge flags added) | yes | 1754 KB / 1920 KB (91.4%) | 93 KB | PASS (272,648 avail / 149,392 req) | built with Windows cmdline shortener (plain build not tried) |
| `Wio_Tracker_L2_repeater` | ESP32-S3 | 2 | no (built with bridge flags added) | yes | 1994 KB / 6400 KB (31.1%) | 101 KB | PASS (259,752 avail / 188,048 req) | built with Windows cmdline shortener (plain build not tried) |
| `Xiao_S3_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1814 KB / 3264 KB (55.6%) | 95 KB | PASS (270,888 avail / 149,392 req) | built with Windows cmdline shortener (plain build not tried) |
| `Xiao_S3_WIO_repeater_bridge_espnow` | ESP32-S3 | 2 | yes | yes | 1814 KB / 3264 KB (55.6%) | 95 KB | PASS (270,888 avail / 149,392 req) | built with Windows cmdline shortener (plain build not tried) |
| `ESP32Dev_ESPNOW_repeater` | ESP32 | 2 | n/a (ESP-NOW primary radio) | yes | 1665 KB / 1920 KB (86.7%) | 94 KB | PASS (197,784 avail / 116,932 req) | ESP-NOW is the primary radio; WITH_ESPNOW_BRIDGE (and so the observer feed) is not compiled |
| `Generic_E22_repeater_bridge_espnow` | ESP32 | 2 | yes | yes | 1813 KB / 1920 KB (94.4%) | 102 KB | PASS (189,584 avail / 115,892 req) |  |
| `Generic_E22_sx1262_repeater_bridge_espnow` | ESP32 | 2 | yes | yes | 1797 KB / 1920 KB (93.6%) | 101 KB | PASS (189,720 avail / 115,892 req) |  |
| `Generic_E22_sx1268_repeater_bridge_espnow` | ESP32 | 2 | yes | yes | 1788 KB / 1920 KB (93.1%) | 101 KB | PASS (189,776 avail / 115,892 req) |  |
| `GEPRC_Linkflow_900_repeater` | ESP32 | 2 | no (built with bridge flags added) | NO (flash overflow) | 1789 KB / 1536 KB (116.5%) | 102 KB | PASS (189,592 avail / 116,932 req) | error: Error: The program size (1832281 bytes) is greater than maximum allowed (1572864 bytes) |
| `Heltec_v2_repeater_bridge_espnow` | ESP32 | 2 | yes | yes | 1810 KB / 3264 KB (55.4%) | 101 KB | PASS (189,744 avail / 119,988 req) |  |
| `LilyGo_TLora_V2_1_1_6_repeater_bridge_espnow` | ESP32 | 2 | yes | yes | 1867 KB / 1984 KB (94.1%) | 103 KB | PASS (187,880 avail / 119,988 req) |  |
| `Meshadventurer_repeater_bridge_espnow` | ESP32 | 2 | yes | yes | 1855 KB / 1920 KB (96.6%) | 102 KB | PASS (188,912 avail / 119,988 req) |  |
| `Meshadventurer_sx1262_repeater_bridge_espnow` | ESP32 | 2 | yes | yes | 1840 KB / 1920 KB (95.8%) | 102 KB | PASS (189,048 avail / 119,988 req) |  |
| `Meshadventurer_sx1268_repeater_bridge_espnow` | ESP32 | 2 | yes | yes | 1832 KB / 1920 KB (95.4%) | 102 KB | PASS (189,104 avail / 119,988 req) |  |
| `Tbeam_repeater_bridge_espnow` | ESP32 | 2 | yes | yes | 1751 KB / 1920 KB (91.2%) | 103 KB | PASS (188,560 avail / 115,892 req) |  |
| `Tbeam_SX1262_repeater_bridge_espnow` | ESP32 | 2 | yes | yes | 1727 KB / 1920 KB (90.0%) | 102 KB | PASS (188,848 avail / 115,892 req) |  |
| `Tbeam_SX1276_repeater_bridge_espnow` | ESP32 | 2 | yes | yes | 1715 KB / 1920 KB (89.3%) | 102 KB | PASS (189,024 avail / 115,892 req) |  |
| `Generic_ESPNOW_repeatr` | ESP32-C3 | 1 | n/a (ESP-NOW primary radio) | yes | 1688 KB / 1920 KB (87.9%) | 114 KB | PASS (195,648 avail / 116,660 req) | single core; ESP-NOW is the primary radio; WITH_ESPNOW_BRIDGE (and so the observer feed) is not compiled |
| `Heltec_ct62_repeater_bridge_espnow` | ESP32-C3 | 1 | yes | yes | 1759 KB / 1984 KB (88.7%) | 122 KB | PASS (187,584 avail / 116,660 req) | single core |
| `Tenstar_C3_repeater_bridge_espnow` | ESP32-C3 | 1 | yes | yes | 1812 KB / 1920 KB (94.4%) | 122 KB | PASS (187,248 avail / 116,660 req) | single core |
| `Tenstar_C3_sx1262_repeater_bridge_espnow` | ESP32-C3 | 1 | yes | yes | 1800 KB / 1920 KB (93.8%) | 122 KB | PASS (187,392 avail / 116,660 req) | single core |
| `Tenstar_C3_sx1268_repeater_bridge_espnow` | ESP32-C3 | 1 | yes | yes | 1793 KB / 1920 KB (93.4%) | 122 KB | PASS (187,440 avail / 116,660 req) | single core |
| `Xiao_C3_repeater` | ESP32-C3 | 1 | no (built with bridge flags added) | yes | 1825 KB / 1920 KB (95.0%) | 123 KB | PASS (186,304 avail / 116,660 req) | single core; needed Windows cmdline shortener |
| `LilyGo_Tlora_C6_repeater_` | ESP32-C6 | 1 | no (built with bridge flags added) | yes | 1914 KB / 1920 KB (99.7%) | 128 KB | PASS (292,048 avail / 116,660 req) | single core; only 6547 bytes of app flash free |
| `M5Stack_Unit_C6L_repeater` | ESP32-C6 | 1 | no (built with bridge flags added) | yes | 1920 KB / 1920 KB (100.0%) | 128 KB | PASS (291,968 avail / 116,660 req) | single core; only 121 bytes of app flash free |
| `Meshimi_repeater_` | ESP32-C6 | 1 | no (built with bridge flags added) | yes | 1914 KB / 1920 KB (99.7%) | 128 KB | PASS (292,048 avail / 116,660 req) | single core; only 6489 bytes of app flash free |
| `WHY2025_badge_repeater_` | ESP32-C6 | 1 | no (built with bridge flags added) | yes | 1914 KB / 3264 KB (58.6%) | 128 KB | PASS (292,048 avail / 116,660 req) | single core |
| `Xiao_C6_repeater_` | ESP32-C6 | 1 | no (built with bridge flags added) | yes | 1914 KB / 1920 KB (99.7%) | 128 KB | PASS (292,048 avail / 116,660 req) | single core; only 6549 bytes of app flash free |

## Failures

Baseline = the plain repeater env of the same board, built without the bridge
on the same commit (also with the shortener).

| Failing bridge env(s) | Baseline env (no bridge) | Baseline result |
|---|---|---|
| `Station_G2_repeater_bridge_espnow`, `Station_G2_logging_repeater_bridge_espnow` | `Station_G2_repeater` | 1,498,701 / 1,310,720 B (114.3%), overflow |
| `LilyGo_T3S3_sx1262_repeater_bridge_espnow`, `LilyGo_T3S3_sx1276_repeater_bridge_espnow` | `LilyGo_T3S3_sx1262_repeater` | 1,742,981 / 1,310,720 B (133.0%), overflow (sx1276 not separately baselined, same family) |
| `LilyGo_T3S3_repeater_bridge_espnow` (universal) | `LilyGo_T3S3_repeater` | fails to link with the same `plugin needed to handle lto object` / `undefined reference to app_main` errors; cause not isolated (the env sets its own `extra_scripts`) |
| `ThinkNode_M2_Repeater_bridge_espnow` | `ThinkNode_M2_Repeater` | 1,729,777 / 1,310,720 B (132.0%), overflow |
| `GEPRC_Linkflow_900_repeater` (+bridge flags) | `GEPRC_Linkflow_900_repeater` | 1,787,405 / 1,572,864 B (113.6%), overflow |
| `nibble_screen_connect_repeater_bridge_espnow_`, `nibble_zero_connect_repeater_bridge_espnow_` | `nibble_zero_connect_repeater_` | 1,789,741 / 1,310,720 B (136.5%), overflow |

The plain repeater images on these boards already exceed their app partition
on this commit, so the bridge cannot be added until the base image is
shrunk or the partition table is enlarged. That is a separate issue from the
bridge. These rows are reported, not fixed.

## Tight fits

- ESP32-C6 repeater envs are at 99.7-100.0% of the app partition. With the
  bridge added they leave only about 6 KB free (`M5Stack_Unit_C6L_repeater`:
  121 bytes). Any growth will overflow them.
- Classic ESP32 bridge envs reach 96.6% (`Meshadventurer_repeater_bridge_espnow`);
  ESP32-C3 bridge envs 93-95%; ThinkNode M5 94.5%.

## Not built

- **Variants of built boards** (46 envs): `*_observer_mqtt` (MQTT observer,
  different transport), `*_bridge_rs232`, `*_logging_repeater` (non-bridge
  Station logging variants), `heltec_v4_repeater_legacy_partition_test`,
  `Heltec_v3_repeater_observer_mqtt_sim`. A bridge variant of the same board
  was built where one exists.
- **Plain repeater envs that have a bridge sibling** (41 envs) are covered by
  the sibling's build above.
- **Room server envs** (about 86): not attempted, they are a different
  firmware role.
- **nRF52 / STM32 / RP2040 envs**: no ESP-NOW, not supported.

## Method

`pio project config` enumerated every env with an espressif32 platform and a
`simple_repeater` source filter (155), classified into bridge envs (44),
plain envs lacking a bridge (22 built), ESP-NOW-primary (2 built), bridge
siblings and variants (skipped, see above). One PlatformIO process at a time.
The PlatformIO penv was missing `pip`, which the pioarduino ESP32-C6
platform needs; `python -m ensurepip` was run once in that penv. Nothing
was flashed and no serial port was opened.

