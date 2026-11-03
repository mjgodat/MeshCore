import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "helpers/bridges/ObserverFrame.h"
using namespace mesh::observer;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static PacketRecord makePacket() {
  PacketRecord p; std::memset(&p, 0, sizeof(p));
  p.rx_ms = 0x01020304u; p.snr_q4 = (int8_t)(-7.25f * 4); p.rssi = -104;
  p.disp = Disposition::Forwarded; p.len = 184;
  for (int i = 0; i < 184; i++) p.raw[i] = (uint8_t)(i * 7 + 1);
  return p;
}
static Vitals makeVitals(bool pos) {
  Vitals v; std::memset(&v, 0, sizeof(v));
  v.uptime_s = 86400u * 3; v.noise_floor = -118; v.rx_per_min = 12; v.tx_per_min = 5; v.dup_per_min = 3;
  v.airtime_permille = 42; v.espnow_nodes = 4; v.batt_mv = 4012;
  v.has_pos = pos;
  if (pos) { v.lat_e6 = 47606200; v.lon_e6 = -122332100; }
  v.freq = 910.525f; v.bw = 62.5f; v.sf = 7; v.cr = 5; v.preset_slot = 2;
  return v;
}

int main() {
  uint8_t buf[256];
  std::printf("section roundtrip_packet\n");
  // roundtrip_packet
  {
    PacketRecord p = makePacket();
    CHECK(p.snr_q4 == -29);
    size_t n = encodePacket(p, buf, sizeof(buf));
    CHECK(n > 0 && n <= 250);
    CHECK(isObserverFrame(buf, n));
    PacketRecord q; std::memset(&q, 0xEE, sizeof(q));
    CHECK(decodePacket(buf, n, q));
    CHECK(q.rx_ms == p.rx_ms && q.snr_q4 == -29 && q.rssi == -104);
    CHECK(q.disp == Disposition::Forwarded && q.len == 184 && !q.truncated);
    CHECK(std::memcmp(q.raw, p.raw, 184) == 0);
    { Vitals tv; CHECK(!decodeVitals(buf, n, tv)); }
    // short packet
    PacketRecord s = makePacket(); s.len = 5;
    size_t m = encodePacket(s, buf, sizeof(buf));
    CHECK(m < n);
    CHECK(decodePacket(buf, m, q) && q.len == 5 && std::memcmp(q.raw, s.raw, 5) == 0);
    // cap too small
    CHECK(encodePacket(p, buf, n - 1) == 0);
  }
  std::printf("section truncated_flag\n");
  // truncated_flag: MAX_RAW = 235 fills one ESP-NOW frame; longer packets keep
  // their first 235 bytes and set bit 7 of the disposition byte.
  {
    CHECK(MAX_RAW == 235 && MAX_PACKET_FRAME == 250);
    uint8_t pkt[255]; for (int i = 0; i < 255; i++) pkt[i] = (uint8_t)(i * 13 + 5);
    PacketRecord p = makePacket(), q;
    CHECK(setPacketBytes(p, pkt, 255) && p.truncated && p.len == 235);
    size_t n = encodePacket(p, buf, sizeof(buf));
    CHECK(n == 250 && isObserverFrame(buf, n));
    CHECK(buf[6 + 4 + 1 + 2] == (0x80 | (uint8_t)Disposition::Forwarded));
    std::memset(&q, 0xEE, sizeof(q));
    CHECK(decodePacket(buf, n, q) && q.truncated && q.len == 235 && q.disp == Disposition::Forwarded);
    CHECK(std::memcmp(q.raw, pkt, 235) == 0);
    const size_t exact[] = {1, 200, 235};
    for (size_t len : exact) {
      PacketRecord e = makePacket();
      CHECK(setPacketBytes(e, pkt, len) && !e.truncated && e.len == len);
      size_t m = encodePacket(e, buf, sizeof(buf));
      CHECK(m == 15 + len && buf[6 + 4 + 1 + 2] == (uint8_t)Disposition::Forwarded);
      std::memset(&q, 0xEE, sizeof(q));
      CHECK(decodePacket(buf, m, q) && !q.truncated && q.len == len && std::memcmp(q.raw, pkt, len) == 0);
    }
    CHECK(!setPacketBytes(p, pkt, 0) && !setPacketBytes(p, nullptr, 5));
    // A truncated flag on a short record is inconsistent: not encoded, not decoded.
    PacketRecord bad = makePacket(); bad.len = 10; bad.truncated = true;
    CHECK(encodePacket(bad, buf, sizeof(buf)) == 0);
    bad.truncated = false; size_t m = encodePacket(bad, buf, sizeof(buf));
    buf[6 + 4 + 1 + 2] |= 0x80; CHECK(!decodePacket(buf, m, q));
    // Bad disposition values stay rejected with the flag set.
    p = makePacket(); CHECK(setPacketBytes(p, pkt, 255)); n = encodePacket(p, buf, sizeof(buf));
    buf[6 + 4 + 1 + 2] = 0x84; CHECK(!decodePacket(buf, n, q));
  }
  std::printf("section roundtrip_vitals\n");
  // roundtrip_vitals
  for (int pos = 0; pos < 2; pos++) {
    Vitals v = makeVitals(pos != 0);
    size_t n = encodeVitals(v, buf, sizeof(buf));
    CHECK(n > 0 && n <= 250);
    CHECK(isObserverFrame(buf, n));
    Vitals r; std::memset(&r, 0xEE, sizeof(r));
    CHECK(decodeVitals(buf, n, r));
    CHECK(r.uptime_s == v.uptime_s && r.noise_floor == -118 && r.rx_per_min == 12 && r.tx_per_min == 5);
    CHECK(r.dup_per_min == 3 && r.airtime_permille == 42 && r.espnow_nodes == 4 && r.batt_mv == 4012);
    CHECK(r.has_pos == (pos != 0));
    CHECK(r.lat_e6 == v.lat_e6 && r.lon_e6 == v.lon_e6);
    CHECK(r.freq == 910.525f && r.bw == 62.5f && r.sf == 7 && r.cr == 5 && r.preset_slot == 2);
    PacketRecord pr;
    CHECK(!decodePacket(buf, n, pr));
    CHECK(encodeVitals(v, buf, n - 1) == 0);
  }
  std::printf("section decode_rejects_truncated\n");
  // decode_rejects_truncated
  {
    PacketRecord p = makePacket();
    size_t n = encodePacket(p, buf, sizeof(buf));
    PacketRecord q;
    for (size_t k = 0; k < n; k++) CHECK(!decodePacket(buf, k, q));
    CHECK(decodePacket(buf, n, q));
    Vitals v = makeVitals(true);
    size_t nv = encodeVitals(v, buf, sizeof(buf));
    Vitals r;
    for (size_t k = 0; k < nv; k++) CHECK(!decodeVitals(buf, k, r));
    CHECK(decodeVitals(buf, nv, r));
    CHECK(!isObserverFrame(nullptr, 0));
    CHECK(!isObserverFrame(buf, 3));
  }
  std::printf("section rejects_wrong_magic_and_version\n");
  // rejects_wrong_magic_and_version (and kind, and bad field values)
  {
    Vitals v = makeVitals(true), r;
    size_t n = encodeVitals(v, buf, sizeof(buf));
    uint8_t t[256];
    for (int i = 0; i < 5; i++) {
      std::memcpy(t, buf, n); t[i] ^= 0x01;
      CHECK(!isObserverFrame(t, n));
      CHECK(!decodeVitals(t, n, r));
    }
    std::memcpy(t, buf, n); t[4] = VERSION + 1; CHECK(!decodeVitals(t, n, r));
    std::memcpy(t, buf, n); t[4] = 0; CHECK(!isObserverFrame(t, n));
    std::memcpy(t, buf, n); t[5] = 9; CHECK(!isObserverFrame(t, n));
    PacketRecord p = makePacket(), q;
    n = encodePacket(p, buf, sizeof(buf));
    std::memcpy(t, buf, n); t[5] = (uint8_t)Kind::Vitals; CHECK(!decodePacket(t, n, q));
    // oversize len field is rejected even if bytes are present
    std::memcpy(t, buf, n); t[6 + 4 + 1 + 2 + 1] = 236; CHECK(!decodePacket(t, n, q));
    // bad disposition
    std::memcpy(t, buf, n); t[6 + 4 + 1 + 2] = 4; CHECK(!decodePacket(t, n, q));
  }
  std::printf("section no_collision_with_meshcore_raw\n");
  // no_collision_with_meshcore_raw
  {
    // Today only MeshCore PAYLOAD_VER_1 (header bits 7:6 == 00) is valid, so valid first
    // bytes are 0x00..0x3F; Packet::deserialize rejects the rest. MAGIC0 is outside that
    // range and is not 0xFE (MCF/MCD).
    CHECK(MAGIC0 >= 0x40 && MAGIC0 != 0xFE);
    // Any valid-looking MeshCore prefix followed by anything is never an observer frame.
    for (int h = 0; h < 0x40; h++)
      for (int b = 0; b < 256; b++) {
        uint8_t f[8] = {(uint8_t)h, (uint8_t)b, 0x53, 0x4F, 1, 2, 0, 0};
        CHECK(!isObserverFrame(f, sizeof(f)));
      }
    // Even with a future-version first byte, the 4-byte magic + exact length must match.
    for (int h = 0x40; h < 256; h++)
      for (int b = 0; b < 256; b++) {
        uint8_t f[8] = {(uint8_t)h, (uint8_t)b, 1, 1, 0, 0, 0, 0};
        CHECK(!isObserverFrame(f, sizeof(f)));
      }
    // Right 2-byte prefix but wrong length / wrong tail is rejected.
    { Vitals v = makeVitals(false); size_t n = encodeVitals(v, buf, sizeof(buf));
      buf[n] = 0; CHECK(!isObserverFrame(buf, n + 1)); CHECK(!isObserverFrame(buf, n - 1)); CHECK(isObserverFrame(buf, n));
      Vitals r; CHECK(!decodeVitals(buf, n + 1, r)); }
    // raw fragment FE 4D 43 46
    uint8_t frag[16] = {0xFE, 0x4D, 0x43, 0x46, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(!isObserverFrame(frag, sizeof(frag)));
    // WiChat frame: BSSID 57:43.. at offset 16, magic 0x5743 at offset 24
    uint8_t w[64]; std::memset(w, 0, sizeof(w));
    w[16] = 0x57; w[17] = 0x43; w[24] = 0x57; w[25] = 0x43;
    CHECK(!isObserverFrame(w, sizeof(w)));
  }
  std::printf("section fits_espnow_mtu\n");
  // fits_espnow_mtu
  {
    PacketRecord p = makePacket();
    CHECK(encodePacket(p, buf, sizeof(buf)) <= 250);
    CHECK(encodeVitals(makeVitals(true), buf, sizeof(buf)) <= 250);
    CHECK(MAX_PACKET_FRAME <= 250 && VITALS_FRAME <= 250);
  }
  if (failures) return 1;
  std::printf("PASS observer\n");
  return 0;
}
'''


class ObserverFrameTest(unittest.TestCase):
    def test_observer_frame_codec(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            self.skipTest('a host C++17 compiler is required')
        sanitize = [] if os.name == 'nt' else [
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        with tempfile.TemporaryDirectory(prefix='observer-frame-') as directory:
            work = Path(directory)
            cpp = work / 'observer.cpp'
            cpp.write_text(HARNESS, encoding='ascii')
            binary = work / ('observer' + ('.exe' if os.name == 'nt' else ''))
            built = subprocess.run([
                compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize,
                '-I', str(ROOT / 'src'), '-I', str(ROOT / 'test'), str(cpp), '-o', str(binary),
            ], capture_output=True, text=True, timeout=120, cwd=str(ROOT / 'test'))
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('PASS observer', ran.stdout)


if __name__ == '__main__':
    unittest.main()
