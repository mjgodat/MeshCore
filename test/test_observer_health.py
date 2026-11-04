"""Observer health frame (kind 3): the repeater's heap and reset reason.

- ObserverFrame.h: encodeHealth/decodeHealth golden bytes, forward
  compatibility (a longer, newer body decodes; version 0 and short frames do
  not), the vitals frame keeps its exact length (deployed heads), and the
  health frame cannot be mistaken for a MeshCore packet.
- ObserverRateLimiter.h: health frames are status frames: they bypass the
  packet-record token bucket and survive eviction like vitals.
- HeapHealth.h: `stats-core` gains heap_free / heap_min / heap_max_block,
  and only when they fit the 160-byte reply.
- The repeater sends a health frame with the first vitals after a start and
  then every kHealthEveryVitals ticks (source contract).
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

CODEC = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include "helpers/bridges/ObserverFrame.h"
#include "helpers/bridges/ObserverRateLimiter.h"
using namespace mesh::observer;
int main() {
  Health h; h.uptime_s = 0x00015180; h.free_heap = 0x00012345; h.min_free_heap = 0x0000F00D; h.largest_block = 0x00008000; h.reset_reason = 6;
  uint8_t f[260];
  assert(encodeHealth(h, f, HEALTH_FRAME_V1 - 1) == 0);
  const size_t n = encodeHealth(h, f, sizeof(f));
  assert(n == HEALTH_FRAME_V1 && n == 24);
  printf("HEALTH ");
  for (size_t i = 0; i < n; ++i) printf("%02x", f[i]);
  printf("\n");
  assert(isObserverFrame(f, n) && isStatusFrame(f, n));
  Health d; memset(&d, 0xEE, sizeof(d));
  assert(decodeHealth(f, n, d) && d.uptime_s == 86400 && d.free_heap == h.free_heap && d.min_free_heap == h.min_free_heap
         && d.largest_block == h.largest_block && d.reset_reason == 6);
  Vitals v; assert(!decodeVitals(f, n, v));
  PacketRecord p; assert(!decodePacket(f, n, p));
  // the same heap 30 s later is a different frame (a head on transport
  // both drops byte-identical frames as the other transport's copy)
  { Health later = h; later.uptime_s += 30; uint8_t f2[64];
    assert(encodeHealth(later, f2, sizeof(f2)) == n && memcmp(f, f2, n) != 0); }
  // forward compatible: a newer body (version 2, appended fields) decodes
  uint8_t g[64]; memcpy(g, f, n); g[6] = 2; for (int i = 0; i < 8; ++i) g[n + i] = 0x5A;
  assert(isObserverFrame(g, n + 8) && decodeHealth(g, n + 8, d) && d.free_heap == h.free_heap);
  // up to a wrapped ESP-NOW frame, no longer
  uint8_t big[MAX_WRAPPED_FRAME + 1]; memset(big, 0, sizeof(big)); memcpy(big, f, n);
  assert(isObserverFrame(big, MAX_WRAPPED_FRAME) && !isObserverFrame(big, MAX_WRAPPED_FRAME + 1));
  // too short, version 0, wrong frame version
  assert(!isObserverFrame(f, n - 1) && !decodeHealth(f, n - 1, d));
  memcpy(g, f, n); g[6] = 0; assert(!decodeHealth(g, n, d));
  memcpy(g, f, n); g[4] = 2; assert(!isObserverFrame(g, n));
  // the vitals frame keeps its exact length: deployed heads check it
  Vitals vv; memset(&vv, 0, sizeof(vv));
  uint8_t vf[64]; const size_t vn = encodeVitals(vv, vf, sizeof(vf));
  assert(vn == VITALS_FRAME && VITALS_FRAME == 43 && isStatusFrame(vf, vn));
  vf[vn] = 0; assert(!isObserverFrame(vf, vn + 1));
  // MeshCore rejects the first byte (ver bits 0b10)
  assert((f[0] >> 6) == 2);
  assert(!strcmp(resetReasonName(1), "power-on") && !strcmp(resetReasonName(6), "task wdt")
         && !strcmp(resetReasonName(9), "brownout") && !strcmp(resetReasonName(200), "other"));
  // status frames bypass the packet bucket and outlive packet records
  Outbox box;
  PacketRecord r; memset(&r, 0, sizeof(r)); r.len = 10;
  uint8_t pf[MAX_PACKET_FRAME]; const size_t pn = encodePacket(r, pf, sizeof(pf));
  for (int i = 0; i < 60; ++i) box.offer(pf, pn, 1000, false);   // drain the bucket
  assert(box.offer(pf, pn, 1000, false) == Offer::RateLimited);
  assert(box.offer(f, n, 1000, false) == Offer::Queued);
  assert(box.offer(f, n, 1000, true) == Offer::Queued);    // not preempted by mesh traffic
  for (int i = 0; i < 4; ++i) box.offer(vf, vn, 1000, false);
  uint8_t out[MAX_PACKET_FRAME];
  int health = 0, vitals = 0;
  while (!box.empty()) {
    const size_t m = box.pop(out, sizeof(out));
    assert(m);
    health += out[5] == 3; vitals += out[5] == 2;
  }
  assert(health + vitals == kOutboxDepth && vitals >= 1);
  // the shared fanout admits health without a token
  struct Sink : FanoutSink {
    Outbox ob;
    bool observerActive() const override { return true; }
    bool observerMeshPending() const override { return false; }
    Outbox& observerOutbox() override { return ob; }
  } a, b;
  FanoutSink* sinks[2] = {&a, &b};
  Fanout fan;
  for (int i = 0; i < 60; ++i) fan.offer(pf, pn, 2000, sinks, 2);
  assert(fan.offer(pf, pn, 2000, sinks, 2) == 0);
  assert(fan.offer(f, n, 2000, sinks, 2) == 2);
  static_assert(kHealthEveryVitals * kVitalsIntervalMs == 30000, "one health frame per 30 s");
  printf("PASS health\n");
  return 0;
}
'''

STATS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
size_t free_size = 123456, min_size = 81234, largest = 65524;
int reason = 4;
#include "helpers/esp32/HeapHealth.h"
int main() {
  char reply[160];
  strcpy(reply, "{\"battery_mv\":4131,\"uptime_secs\":74,\"errors\":0,\"queue_len\":0}");
  mesh::esp32::appendHeapStats(reply, sizeof(reply));
  assert(!strcmp(reply, "{\"battery_mv\":4131,\"uptime_secs\":74,\"errors\":0,\"queue_len\":0,"
                        "\"heap_free\":123456,\"heap_min\":81234,\"heap_max_block\":65524}"));
  const mesh::observer::Health h = mesh::esp32::heapHealth(77);
  assert(h.uptime_s == 77 && h.free_heap == 123456 && h.min_free_heap == 81234 && h.largest_block == 65524 && h.reset_reason == 4);
  // left alone when it would not fit, or when it is not a JSON object
  std::string longer(120, 'x'); longer = "{\"" + longer + "\":1}";
  char small[160]; strcpy(small, longer.c_str());
  mesh::esp32::appendHeapStats(small, sizeof(small));
  assert(longer == small);
  char text[160] = "Err - busy";
  mesh::esp32::appendHeapStats(text, sizeof(text));
  assert(!strcmp(text, "Err - busy"));
  printf("PASS stats\n");
  return 0;
}
'''


def compile_and_run(test, source, includes):
    compiler = shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        test.skipTest("a host C++ compiler is required")
    sanitize = [] if os.name == "nt" else ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    with tempfile.TemporaryDirectory(prefix="observer-health-") as tmp:
        folder = Path(tmp)
        (folder / "t.cpp").write_text(source)
        command = [compiler, "-std=c++11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *sanitize]
        for include in includes(folder):
            command += ["-I", str(include)]
        built = subprocess.run(command + [str(folder / "t.cpp"), "-o", str(folder / "t")],
                               capture_output=True, text=True, timeout=120)
        test.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        ran = subprocess.run([str(folder / "t")], capture_output=True, text=True, timeout=60)
        test.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        return ran.stdout


class HealthFrameTests(unittest.TestCase):
    def test_codec_compatibility_and_outbox(self):
        out = compile_and_run(self, CODEC, lambda folder: [ROOT / "src"])
        self.assertIn("PASS health", out)
        frame = bytes.fromhex(out.split("HEALTH ", 1)[1].split()[0])
        # B0 42 53 4F, frame version 1, kind 3, body version 1, then LE fields:
        # uptime 86400 s, free, min free, largest block, reset reason 6
        self.assertEqual(frame.hex(), "b042534f0103" "01" "80510100" "45230100" "0df00000" "00800000" "06")

    def test_stats_core_heap_fields(self):
        def includes(folder):
            (folder / "esp_heap_caps.h").write_text(
                "#pragma once\n#include <cstddef>\n#define MALLOC_CAP_8BIT 4\n"
                "extern size_t free_size, min_size, largest;\n"
                "inline size_t heap_caps_get_free_size(int c) { return c == MALLOC_CAP_8BIT ? free_size : 0; }\n"
                "inline size_t heap_caps_get_minimum_free_size(int c) { return c == MALLOC_CAP_8BIT ? min_size : 0; }\n"
                "inline size_t heap_caps_get_largest_free_block(int c) { return c == MALLOC_CAP_8BIT ? largest : 0; }\n")
            (folder / "esp_system.h").write_text(
                "#pragma once\nextern int reason;\ninline int esp_reset_reason() { return reason; }\n")
            return [folder, ROOT / "src"]
        self.assertIn("PASS stats", compile_and_run(self, STATS, includes))


class RepeaterSendsHealthTests(unittest.TestCase):
    def test_vitals_pump_sends_health_and_stats_core_appends_heap(self):
        source = (ROOT / "examples/simple_repeater/MyMesh.cpp").read_text().replace("\r\n", "\n")
        pump = source[source.index("void MyMesh::serviceObserverVitals()"):]
        pump = pump[:pump.index("\n}\n")]
        self.assertIn("w.health_wait = 0;", pump)                       # at once after a (re)start
        self.assertIn("encodeHealth(h, frame, sizeof(frame))", pump)
        self.assertIn("mesh::esp32::heapHealth((uint32_t)(uptime_millis / 1000))", pump)
        self.assertIn("w.health_wait = mesh::observer::kHealthEveryVitals;", pump)
        self.assertLess(pump.index("encodeHealth"), pump.index("w.started = true;"))
        stats = source[source.index("void MyMesh::formatStatsReply(char *reply) {"):]
        stats = stats[:stats.index("\n}\n")]
        self.assertRegex(stats, r"#if defined\(ESP32_PLATFORM\)\s+mesh::esp32::appendHeapStats\(reply, 160\);")


if __name__ == "__main__":
    unittest.main()
