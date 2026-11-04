"""espnow.format wrapped on primary ESP-NOW nodes (heads, relays, Generic_ESPNOW).

The wrapped frame is the LoRa repeater bridge's `bridge.format wrapped`
(magic C0 3E, Fletcher-16, XOR with the secret), produced and parsed by one
codec (ESPNowWrappedFrame.h). These checks run:

- the codec against an independent Python model of the legacy bridge
  wrapper (byte-for-byte golden frames, both directions, wrong secret);
- the real ESPNowBridge.cpp (fixture of test_bridge_recovery) and the real
  ESPNOWRadio.cpp (fixture of test_espnow_radio_lifecycle) against each
  other: frames sent by one are accepted by the other in wrapped mode, the
  observer feed is wrapped too, a wrong secret or a raw frame is refused,
  and raw mode is unchanged;
- the espnow.format / espnow.secret settings (saved record, CLI, the secret
  never printed, the format applying only at the next boot).
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

from test_espnow_repeat import build_and_run, compiler, sanitizer_flags
from test_observer_feed import settings_folder

ROOT = Path(__file__).resolve().parents[1]


def fletcher16(data):
    s1 = s2 = 0
    for b in data:
        s1 = (s1 + b) % 255
        s2 = (s2 + s1) % 255
    return (s2 << 8) | s1


def legacy_wrap(payload, secret):
    """The bridge's original sendPacket wrapper (before the shared codec)."""
    chk = fletcher16(payload)
    body = bytes([chk >> 8, chk & 0xFF]) + payload
    key = secret.encode()
    return b"\xc0\x3e" + bytes(b ^ key[i % len(key)] for i, b in enumerate(body))


def packets():
    # MeshCore packets as the fixtures build them: first byte distinct, the
    # rest a pattern; lengths 4 .. 246 (the wrapped maximum).
    out = []
    for i, length in enumerate((4, 37, 100, 200, 246)):
        out.append(bytes([0x10 + i]) + bytes((i * 7 + j) & 0xFF for j in range(1, length)))
    return out


def hexlines(frames):
    return "\n".join(f.hex() for f in frames) + "\n"


def parse(text, tag):
    return [bytes.fromhex(line.split(" ", 1)[1]) for line in text.splitlines()
            if line.startswith(tag + " ") and len(line.split(" ", 1)) == 2]


CODEC_CHECKS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <helpers/bridges/ESPNowWrappedFrame.h>
#include <helpers/bridges/ObserverFrame.h>
using namespace mesh::bridge;
int main(int argc, char** argv) {
  assert(argc == 2);
  // argv[1]: hex payloads separated by ':', wrapped with "fleet-test".
  char secret[16] = "fleet-test";
  const char* p = argv[1];
  while (*p) {
    uint8_t payload[260]; size_t n = 0;
    while (*p && *p != ':') { unsigned v; sscanf(p, "%2x", &v); payload[n++] = (uint8_t)v; p += 2; }
    if (*p == ':') ++p;
    uint8_t frame[250]; uint8_t back[250]; size_t back_len = 99;
    const size_t size = wrapFrame(payload, n, secret, sizeof(secret), frame, sizeof(frame));
    printf("W ");
    for (size_t i = 0; i < size; ++i) printf("%02x", frame[i]);
    printf("\n");
    if (!size) continue;
    assert(unwrapFrame(frame, size, secret, sizeof(secret), back, sizeof(back), back_len) == Unwrap::Ok);
    assert(back_len == n && !memcmp(back, payload, n));
    char other[16] = "Fleet-test";
    assert(unwrapFrame(frame, size, other, sizeof(other), back, sizeof(back), back_len) == Unwrap::Checksum
           && back_len == 0);
  }
  // Strictness and bounds.
  uint8_t out[250]; size_t out_len = 7;
  const uint8_t raw_packet[] = {0x11, 1, 2, 3, 4};
  assert(unwrapFrame(raw_packet, sizeof(raw_packet), secret, sizeof(secret), out, sizeof(out), out_len)
         == Unwrap::Foreign && out_len == 0);
  const uint8_t short_frame[] = {0xC0, 0x3E, 1};
  assert(unwrapFrame(short_frame, 3, secret, sizeof(secret), out, sizeof(out), out_len) == Unwrap::TooShort);
  const uint8_t header_only[] = {0xC0, 0x3E, 1, 2};
  assert(unwrapFrame(header_only, 4, secret, sizeof(secret), out, sizeof(out), out_len) == Unwrap::TooShort);
  uint8_t big[247] = {1};
  assert(wrapFrame(big, 246, secret, sizeof(secret), out, sizeof(out)) == 250);
  assert(wrapFrame(big, 247, secret, sizeof(secret), out, sizeof(out)) == 0);   // > 246: refused
  assert(wrapFrame(big, 0, secret, sizeof(secret), out, sizeof(out)) == 0);
  assert(wrapFrame(big, 10, secret, sizeof(secret), out, 13) == 0);             // out too small
  char empty[16] = {};
  char unterminated[16]; memset(unterminated, 'x', sizeof(unterminated));
  assert(wrapFrame(big, 10, empty, sizeof(empty), out, sizeof(out)) == 0);
  assert(wrapFrame(big, 10, unterminated, sizeof(unterminated), out, sizeof(out)) == 0);
  const size_t ok = wrapFrame(big, 10, secret, sizeof(secret), out, sizeof(out));
  assert(unwrapFrame(out, ok, empty, sizeof(empty), big, sizeof(big), out_len) == Unwrap::NoSecret);
  // Observer frames fit a wrapped frame: a long packet record is cut to
  // MAX_RAW_WRAPPED (231) bytes and marked truncated; it still decodes.
  using namespace mesh::observer;
  PacketRecord r; memset(&r, 0, sizeof(r));
  uint8_t pkt[255]; for (int i = 0; i < 255; ++i) pkt[i] = (uint8_t)i;
  assert(setPacketBytes(r, pkt, 255) && r.len == MAX_RAW);
  uint8_t f[MAX_PACKET_FRAME]; size_t fl = encodePacket(r, f, sizeof(f));
  assert(fl == 250 && fitWrappedPacketFrame(f, fl) == 246);
  PacketRecord q;
  assert(decodePacket(f, 246, q) && q.truncated && q.len == MAX_RAW_WRAPPED && !memcmp(q.raw, pkt, 231));
  assert(wrapFrame(f, 246, secret, sizeof(secret), out, sizeof(out)) == 250);
  // records that fit and vitals are unchanged; a short truncated record is still invalid
  assert(setPacketBytes(r, pkt, 100)); fl = encodePacket(r, f, sizeof(f));
  assert(fitWrappedPacketFrame(f, fl) == fl && decodePacket(f, fl, q) && !q.truncated);
  f[13] |= 0x80; assert(!decodePacket(f, fl, q));
  Vitals v; memset(&v, 0, sizeof(v)); uint8_t vf[VITALS_FRAME];
  assert(encodeVitals(v, vf, sizeof(vf)) == VITALS_FRAME && fitWrappedPacketFrame(vf, VITALS_FRAME) == VITALS_FRAME);
  uint8_t raw_copy[5]; memcpy(raw_copy, raw_packet, 5); assert(fitWrappedPacketFrame(raw_copy, 5) == 0);
  // activeLink: wrapped needs a usable secret; raw by default.
  mesh::espnow::ActiveLink& link = mesh::espnow::activeLink();
  assert(!link.wrapped.load());
  assert(!mesh::espnow::activateLink(true, "") && !link.wrapped.load());
  assert(mesh::espnow::activateLink(true, "abc") && link.wrapped.load() && !strcmp(link.secret, "abc"));
  assert(mesh::espnow::activateLink(false, nullptr) && !link.wrapped.load());
  return 0;
}
'''


# --- the real bridge (test_bridge_recovery fixture) --------------------------
BRIDGE_DRIVER = r'''
#include <fstream>
#include <sstream>
static std::vector<std::vector<uint8_t>> read_frames(const char* path) {
  std::ifstream in(path); std::string line; std::vector<std::vector<uint8_t>> out;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::vector<uint8_t> f;
    for (size_t i = 0; i + 1 < line.size(); i += 2) f.push_back((uint8_t)std::stoul(line.substr(i, 2), nullptr, 16));
    out.push_back(f);
  }
  return out;
}
static void dump(const char* tag, const std::vector<uint8_t>& f) {
  printf("%s ", tag); for (uint8_t b : f) printf("%02x", b); printf("\n");
}
int main(int argc, char** argv) {
  assert(argc >= 4);
  Backend backend; backend.wifi = true; mesh::wireless::control().begin(backend);
  sdk_initialized = sdk_started = true; sdk_mode = WIFI_STA;
  NodePrefs prefs; mesh::PacketManager mgr; mesh::RTCClock rtc;
  prefs.bridge_format = !strcmp(argv[2], "raw") ? mesh::bridge::ESPNOW_FORMAT_RAW
                                               : mesh::bridge::ESPNOW_FORMAT_WRAPPED;
  memset(prefs.bridge_secret, 0, sizeof(prefs.bridge_secret));
  strcpy(prefs.bridge_secret, argv[2]);
  ESPNowBridge bridge(&prefs, &mgr, &rtc); bridge.begin(); assert(bridge._initialized);
  if (!strcmp(argv[1], "send")) {
    // argv[3]: file of packets to send; then one observer packet record.
    uint8_t id = 0;
    for (const auto& bytes : read_frames(argv[3])) {
      mesh::Packet p; p.length = (uint16_t)bytes.size(); memcpy(p.data, bytes.data(), bytes.size());
      bridge.sendPacket(&p); bridge.loop(); complete(bridge); ++id;
    }
    mesh::observer::PacketRecord r; memset(&r, 0, sizeof(r));
    uint8_t raw[255]; for (int i = 0; i < 255; ++i) raw[i] = (uint8_t)(255 - i);
    mesh::observer::setPacketBytes(r, raw, 255); r.rx_ms = 77;
    uint8_t frame[mesh::observer::MAX_PACKET_FRAME];
    const size_t n = mesh::observer::encodePacket(r, frame, sizeof(frame));
    assert(n == 250 && bridge.sendObserverFrame(frame, n));
    bridge.loop(); complete(bridge);
    for (const auto& f : sent) dump("TX", f);
  } else {
    const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    for (const auto& f : read_frames(argv[3])) { bridge.queueReceivedFrame(mac, f.data(), (int)f.size()); bridge.loop(); }
    for (const auto& f : mgr.full) dump("RX", f);
    printf("STATS checksum_bad=%lu foreign=%lu badframe=%lu\n", (unsigned long)bridge._stats.rx.checksum_bad,
           (unsigned long)bridge._stats.rx.foreign, (unsigned long)bridge._stats.rx.badframe);
  }
  bridge.end();
  return 0;
}
'''


# --- the real primary radio (test_espnow_radio_lifecycle fixture) ------------
RADIO_DRIVER = r'''
#include <fstream>
#include <string>
std::vector<std::vector<uint8_t>> air_log;
static std::vector<std::vector<uint8_t>> observer_frames;
static void on_observer(const uint8_t* f, size_t n, void*) { observer_frames.emplace_back(f, f + n); }
static std::vector<std::vector<uint8_t>> read_frames(const char* path) {
  std::ifstream in(path); std::string line; std::vector<std::vector<uint8_t>> out;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::vector<uint8_t> f;
    for (size_t i = 0; i + 1 < line.size(); i += 2) f.push_back((uint8_t)std::stoul(line.substr(i, 2), nullptr, 16));
    out.push_back(f);
  }
  return out;
}
static void dump(const char* tag, const std::vector<uint8_t>& f) {
  printf("%s ", tag); for (uint8_t b : f) printf("%02x", b); printf("\n");
}
static void deliver(const uint8_t* bytes, int len) {
  static uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_now_recv_info_t info{mac}; OnDataRecv(&info, bytes, len);
#else
  OnDataRecv(mac, bytes, len);
#endif
}
int main(int argc, char** argv) {
  assert(argc >= 4);
  if (strcmp(argv[2], "raw")) assert(mesh::espnow::activateLink(true, argv[2]));
  ESPNOWRadio::setObserverHandler(on_observer, nullptr);
  ESPNOWRadio radio; radio.init(); assert(radio.isEnabled());
  if (!strcmp(argv[1], "boot")) {
    // Raw frames that arrive before the saved wrapped format is applied at
    // boot (radio_init runs before the settings load) must never reach
    // MeshCore once the node is wrapped.
    assert(!strcmp(argv[2], "raw"));
    const auto frames = read_frames(argv[3]);
    for (size_t i = 0; i + 1 < frames.size(); ++i) deliver(frames[i].data(), (int)frames[i].size());
    assert(mesh::espnow::activateLink(true, "fleet-test"));
    deliver(frames.back().data(), (int)frames.back().size());
    uint8_t out[255]; int n;
    while ((n = radio.recvRaw(out, sizeof(out))) > 0) dump("RX", std::vector<uint8_t>(out, out + n));
    uint32_t foreign = 0, checksum = 0; ESPNOWRadio::linkRejects(foreign, checksum);
    printf("STATS foreign=%lu checksum=%lu\n", (unsigned long)foreign, (unsigned long)checksum);
  } else if (!strcmp(argv[1], "send")) {
    for (const auto& p : read_frames(argv[3])) {
      const size_t before = air_log.size();
      const bool started = radio.startSendRaw(p.data(), (int)p.size());
      if (!started) { printf("REFUSED %u\n", (unsigned)p.size()); continue; }
      OnDataSent(nullptr, ESP_NOW_SEND_SUCCESS);
      while (!radio.isSendComplete()) OnDataSent(nullptr, ESP_NOW_SEND_SUCCESS);
      radio.onSendFinished();
      assert(air_log.size() > before);
    }
    for (const auto& f : air_log) dump("TX", f);
  } else {
    for (const auto& f : read_frames(argv[3])) {
      deliver(f.data(), (int)f.size());
      uint8_t out[255]; int n;
      while ((n = radio.recvRaw(out, sizeof(out))) > 0) dump("RX", std::vector<uint8_t>(out, out + n));
    }
    for (const auto& f : observer_frames) dump("OBS", f);
    uint32_t foreign = 0, checksum = 0; ESPNOWRadio::linkRejects(foreign, checksum);
    printf("STATS foreign=%lu checksum=%lu errors=%lu\n", (unsigned long)foreign, (unsigned long)checksum,
           (unsigned long)radio.getPacketsRecvErrors());
  }
  radio.end();
  return 0;
}
'''


def bridge_source():
    from test_bridge_recovery import code, COMMON
    source = code() + COMMON
    hook = "void queueInbound(Packet* p,uint32_t){"
    assert hook in source
    source = source.replace(hook, "std::vector<std::vector<uint8_t>> full;" + hook
                            + "full.emplace_back(p->data,p->data+p->length);")
    return source + BRIDGE_DRIVER


def radio_source():
    from test_espnow_radio_lifecycle import STUBS
    header = (ROOT / "src/helpers/esp32/ESPNOWRadio.h").read_text()
    source = (ROOT / "src/helpers/esp32/ESPNOWRadio.cpp").read_text()
    header = header[:header.index("#if ESPNOW_DEBUG_LOGGING")]
    header = re.sub(r"^\s*#(?:include|pragma).*$", "", header, flags=re.MULTILINE)
    header = "#include <helpers/bridges/ObserverDemux.h>\n" + header
    header = header.replace(" : public mesh::Radio", "").replace(" override", "")
    source = re.sub(r"^\s*#include.*$", "", source, flags=re.MULTILINE)
    stubs = STUBS.replace(
        "int esp_now_send(const uint8_t*,const uint8_t*,size_t){assert(sdk_active);++send_calls;return 0;}",
        "#include <vector>\nextern std::vector<std::vector<uint8_t>> air_log;\n"
        "int esp_now_send(const uint8_t* d,const uint8_t* b,size_t n){assert(sdk_active);"
        "(void)d;++send_calls;air_log.emplace_back(b,b+n);return 0;}")
    assert "air_log" in stubs
    return stubs + header + source + RADIO_DRIVER


class Programs:
    """Builds the bridge and radio drivers once per test class."""

    def __init__(self, tmp):
        self.tmp = Path(tmp)
        self.bridge = self.build("bridge", bridge_source(),
                                 ["-DESP32=1", "-DWITH_ESPNOW_BRIDGE=1", "-DWITH_RS232_BRIDGE=1",
                                  "-DESP_IDF_VERSION=50200", "-DESP_IDF_VERSION_MAJOR=5"])
        self.radio = self.build("radio", radio_source(),
                                ["-DMESH_PRIMARY_ESPNOW=1", "-DMESH_ESPNOW_RADIO=1",
                                 "-DESP_ARDUINO_VERSION_MAJOR=3", "-DESP_IDF_VERSION=50200"])

    def build(self, name, source, defines):
        cpp, exe = self.tmp / f"{name}.cpp", self.tmp / name
        cpp.write_text(source)
        result = subprocess.run([compiler(), "-std=c++17", "-O1", "-g", *sanitizer_flags(), *defines,
                                 "-I", str(ROOT / "src"), str(cpp), "-o", str(exe)],
                                capture_output=True, text=True, timeout=300)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return exe

    def run(self, exe, mode, secret, frames):
        path = self.tmp / f"in-{exe.name}-{mode}.txt"
        path.write_text(hexlines(frames))
        result = subprocess.run([str(exe), mode, secret, str(path)], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result.stdout


class WrappedCodecTests(unittest.TestCase):
    def test_codec_matches_the_legacy_bridge_wrapper(self):
        payloads = packets() + [b"\x01", bytes(range(200)), b"\xff" * 247]
        arg = ":".join(p.hex() for p in payloads)
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "codec.cpp"
            src.write_text(CODEC_CHECKS)
            exe = Path(tmp) / "codec"
            result = subprocess.run([compiler(), "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                     *sanitizer_flags(), "-I", str(ROOT / "src"), str(src), "-o", str(exe)],
                                    capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run([str(exe), arg], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        frames = [line[2:] for line in run.stdout.splitlines() if line.startswith("W ")]
        self.assertEqual(len(frames), len(payloads))
        for payload, frame in zip(payloads, frames):
            expected = legacy_wrap(payload, "fleet-test").hex() if len(payload) <= 246 else ""
            self.assertEqual(frame, expected)

    def test_bridge_and_primary_radio_share_the_codec(self):
        bridge = (ROOT / "src/helpers/bridges/ESPNowBridge.cpp").read_text()
        radio = (ROOT / "src/helpers/esp32/ESPNOWRadio.cpp").read_text()
        self.assertIn("mesh::bridge::wrapFrame(", bridge)
        self.assertIn("mesh::bridge::unwrapFrame(", bridge)
        self.assertNotIn("xorCrypt", bridge)
        self.assertIn("mesh::bridge::wrapFrame(", radio)
        self.assertIn("mesh::bridge::unwrapFrame(", radio)


class WrappedInteropTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix="espnow-wrapped-")
        cls.p = Programs(cls._tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_bridge_frames_are_accepted_by_a_wrapped_primary_radio(self):
        out = self.p.run(self.p.bridge, "send", "fleet-test", packets())
        tx = parse(out, "TX")
        self.assertEqual(len(tx), len(packets()) + 1)
        for frame, packet in zip(tx, packets()):
            self.assertEqual(frame, legacy_wrap(packet, "fleet-test"))   # unchanged on the wire
        observer = tx[-1]
        self.assertEqual(len(observer), 250)                             # 246 + wrapper
        self.assertEqual(observer[:2], b"\xc0\x3e")
        got = self.p.run(self.p.radio, "recv", "fleet-test", tx)
        self.assertEqual(parse(got, "RX"), packets())
        obs = parse(got, "OBS")
        self.assertEqual(len(obs), 1)
        self.assertEqual(obs[0][:4], b"\xb0\x42\x53\x4f")
        self.assertEqual(len(obs[0]), 246)
        self.assertEqual(obs[0][13] & 0x80, 0x80)                        # truncated
        self.assertEqual(obs[0][14], 231)
        self.assertIn("STATS foreign=0 checksum=0", got)

    def test_primary_radio_frames_are_accepted_by_a_wrapped_bridge(self):
        out = self.p.run(self.p.radio, "send", "fleet-test", packets())
        tx = parse(out, "TX")
        self.assertEqual(tx, [legacy_wrap(p, "fleet-test") for p in packets()])
        got = self.p.run(self.p.bridge, "recv", "fleet-test", tx)
        self.assertEqual(parse(got, "RX"), packets())
        self.assertIn("checksum_bad=0 foreign=0 badframe=0", got)

    def test_wrong_secret_is_refused_both_ways(self):
        from_bridge = parse(self.p.run(self.p.bridge, "send", "fleet-test", packets()), "TX")
        got = self.p.run(self.p.radio, "recv", "other-secret", from_bridge)
        self.assertEqual(parse(got, "RX"), [])
        self.assertEqual(parse(got, "OBS"), [])
        self.assertIn(f"STATS foreign=0 checksum={len(from_bridge)}", got)
        from_radio = parse(self.p.run(self.p.radio, "send", "fleet-test", packets()), "TX")
        got = self.p.run(self.p.bridge, "recv", "other-secret", from_radio)
        self.assertEqual(parse(got, "RX"), [])
        self.assertIn(f"checksum_bad={len(from_radio)}", got)

    def test_strict_format_no_dual_receive(self):
        # A wrapped radio refuses raw frames (and raw observer frames); a raw
        # radio passes wrapped frames to MeshCore, which cannot parse them.
        raw_frames = parse(self.p.run(self.p.radio, "send", "raw", packets()), "TX")
        self.assertEqual(raw_frames, packets())                        # raw is unchanged
        got = self.p.run(self.p.radio, "recv", "fleet-test", raw_frames)
        self.assertEqual(parse(got, "RX"), [])
        self.assertIn(f"STATS foreign={len(raw_frames)} checksum=0", got)
        raw_bridge = parse(self.p.run(self.p.bridge, "send", "raw", packets()), "TX")
        self.assertEqual(raw_bridge[:-1], packets())
        self.assertEqual(len(raw_bridge[-1]), 250)                    # raw observer frame, unwrapped
        got = self.p.run(self.p.radio, "recv", "fleet-test", raw_bridge)
        self.assertEqual(parse(got, "OBS"), [])
        wrapped = parse(self.p.run(self.p.radio, "send", "fleet-test", packets()), "TX")
        got = self.p.run(self.p.radio, "recv", "raw", wrapped)
        self.assertTrue(all(f[:2] == b"\xc0\x3e" for f in parse(got, "RX")))

    def test_frames_queued_before_the_boot_format_applies_are_dropped(self):
        # Review F4: two raw packets (one a raw fragment) queued while the
        # node was still raw at boot, then wrapped activates.
        big = bytes([0x15]) + bytes(range(254))
        raw_frames = parse(self.p.run(self.p.radio, "send", "raw", [packets()[0], big]), "TX")
        self.assertEqual(len(raw_frames), 3)
        wrapped = legacy_wrap(packets()[1], "fleet-test")
        got = self.p.run(self.p.radio, "boot", "raw", raw_frames + [wrapped])
        self.assertEqual(parse(got, "RX"), [packets()[1]])
        self.assertIn("STATS foreign=3 checksum=0", got)

    def test_raw_mode_keeps_two_frame_packets(self):
        big = [bytes([0x15]) + bytes(range(254))]
        tx = parse(self.p.run(self.p.radio, "send", "raw", big), "TX")
        self.assertEqual(len(tx), 2)                                    # unchanged raw fragmentation
        self.assertEqual(parse(self.p.run(self.p.radio, "recv", "raw", tx), "RX"), big)
        out = self.p.run(self.p.radio, "send", "fleet-test", big)
        self.assertIn("REFUSED 255", out)                              # wrapped max is 246, as on the bridge


SETTINGS_CHECKS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
#include "ESPNowLinkSettings.h"
#include "settings_under_test.cpp"
using namespace mesh::espnow;
static char reply[160];
static bool run(const char* command, const LinkCounters* c = nullptr) {
  strcpy(reply, "untouched");
  return handleEspNowLinkCommand(command, reply, 157, c);
}
int main() {
  MemoryFS fs; const char* path = "/espnow_link";
  // default raw, nothing written; this boot is raw
  loadEspNowLinkSettings(&fs);
  assert(espNowLinkSettingsHealthy() && fs.files.empty() && !activeLink().wrapped.load());
  assert(run("get espnow.format") && !strcmp(reply, "> raw"));
  assert(run("get espnow.secret") && !strcmp(reply, "> not set"));
  // wrapped needs a secret first
  assert(run("set espnow.format wrapped") && !strncmp(reply, "Error: set espnow.secret first", 30));
  for (const char* bad : {"set espnow.format", "set espnow.format WRAPPED", "set espnow.format auto",
                          "set espnow.secret", "set espnow.secret 0123456789abcdef", "get espnow.format x"})
    assert(run(bad) && !strncmp(reply, "Error:", 6));
  assert(fs.files.empty());
  assert(run("set espnow.secret fleet-test") && !strcmp(reply, "OK - espnow.secret saved"));
  // Review F9: no equality oracle; the same secret again still says saved.
  assert(run("set espnow.secret fleet-test") && !strcmp(reply, "OK - espnow.secret saved"));
  assert(run("set espnow.secret other") && !strcmp(reply, "OK - espnow.secret saved"));
  assert(run("set espnow.secret fleet-test") && !strcmp(reply, "OK - espnow.secret saved"));
  assert(run("get espnow.secret") && !strcmp(reply, "> set"));               // never printed
  assert(fs.files.at(path).size() == 25);
  assert(run("set espnow.format wrapped") && !strcmp(reply, "OK - espnow.format saved; reboot to apply"));
  assert(!activeLink().wrapped.load());                                    // not before the reboot
  assert(run("get espnow.format") && !strcmp(reply, "> wrapped (saved; raw until reboot)"));
  assert(run("set espnow.format\twrapped") && !strcmp(reply, "OK - espnow.format unchanged"));
  assert(run("set espnow.secret off") && !strcmp(reply, "Error: set espnow.format raw first"));
  // a reload does not change this boot's format either
  loadEspNowLinkSettings(&fs);
  assert(savedEspNowLinkWrapped() && !activeLink().wrapped.load());
  char secret[16]; assert(savedEspNowLinkSecret(secret) && !strcmp(secret, "fleet-test"));
  // nothing in any reply contains the secret
  for (const char* c : {"get espnow.format", "get espnow.secret", "set espnow.secret fleet-test"})
    assert(run(c) && !strstr(reply, "fleet-test"));
  // corrupt record: raw, explicit repair allowed
  fs.files[path][6] ^= 1; loadEspNowLinkSettings(&fs);
  assert(!espNowLinkSettingsHealthy() && !savedEspNowLinkWrapped());
  assert(run("get espnow.format") && strstr(reply, "unavailable"));
  assert(run("set espnow.secret s2") && !strcmp(reply, "OK - espnow.secret saved"));
  assert(espNowLinkSettingsHealthy());
  // failed write keeps the previous value
  fs.fail_write = true;
  assert(run("set espnow.secret s3") && !strcmp(reply, "Error: espnow.secret not saved"));
  fs.fail_write = false;
  assert(savedEspNowLinkSecret(secret) && !strcmp(secret, "s2"));
  for (const char* other : {"get espnow.formats", "set espnow.secretx a", "get espnow.channel", "get espnow.repeat"})
    assert(!run(other) && !strcmp(reply, "untouched"));
  return 0;
}
'''

BOOT_CHECKS_2 = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "ESPNowLinkSettings.h"
#include "settings_under_test.cpp"
using namespace mesh::espnow;
int main(int argc, char** argv) {
  assert(argc == 2);
  MemoryFS fs;
  // Image produced by a previous boot (argv[1]: hex).
  std::vector<uint8_t> image;
  for (const char* p = argv[1]; *p; p += 2) { unsigned v; sscanf(p, "%2x", &v); image.push_back((uint8_t)v); }
  fs.files["/espnow_link"] = image;
  loadEspNowLinkSettings(&fs);
  assert(activeLink().wrapped.load() && !strcmp(activeLink().secret, "fleet-test"));
  char r[160]; LinkCounters c; c.foreign = 3; c.checksum = 4;
  assert(handleEspNowLinkCommand("get espnow.format", r, 157, &c));
  assert(!strcmp(r, "> wrapped (refused: not wrapped 3, wrong secret 4)"));
  assert(handleEspNowLinkCommand("set espnow.format raw", r, 157) && !strcmp(r, "OK - espnow.format saved; reboot to apply"));
  assert(handleEspNowLinkCommand("get espnow.format", r, 157) && !strcmp(r, "> raw (saved; wrapped until reboot)"));
  assert(activeLink().wrapped.load());
  return 0;
}
'''


def link_settings_folder(tmp):
    folder = settings_folder(tmp)
    (folder / "ESPNowLinkSettings.h").write_bytes((ROOT / "src/helpers/ESPNowLinkSettings.h").read_bytes())
    (folder / "settings_under_test.cpp").write_bytes((ROOT / "src/helpers/ESPNowLinkSettings.cpp").read_bytes())
    return folder


class LinkSettingsTests(unittest.TestCase):
    def test_cli_and_saved_record(self):
        with tempfile.TemporaryDirectory(prefix="espnow-link-") as tmp:
            folder = link_settings_folder(tmp)
            build_and_run(self, SETTINGS_CHECKS, includes=[folder, ROOT / "src/helpers"],
                          defines=["ESP32_PLATFORM=1", "MESH_PRIMARY_ESPNOW=1"], strict=False)

    def test_saved_wrapped_applies_at_the_next_boot(self):
        image = b"ENL\x01\x01" + b"fleet-test".ljust(16, b"\0")
        import zlib
        image += zlib.crc32(image).to_bytes(4, "little")
        with tempfile.TemporaryDirectory(prefix="espnow-link-") as tmp:
            folder = link_settings_folder(tmp)
            build_and_run(self, BOOT_CHECKS_2, includes=[folder, ROOT / "src/helpers"],
                          defines=["ESP32_PLATFORM=1", "MESH_PRIMARY_ESPNOW=1"], args=[image.hex()], strict=False)

    def test_wired_on_primary_espnow_roles(self):
        common = (ROOT / "src/helpers/CommonCLI.cpp").read_text()
        companion = (ROOT / "examples/companion_radio/MyMesh.cpp").read_text()
        for source in (common, companion):
            self.assertIn("loadEspNowLinkSettings(", source)
            self.assertIn("handleEspNowLinkCommand(", source)
        # companion: local (USB/BLE app) clients only
        at = companion.index("handleEspNowLinkCommand(")
        self.assertIn("if (sender_timestamp == 0) {", companion[at - 300:at])


if __name__ == "__main__":
    unittest.main()
