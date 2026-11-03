"""Observer demux on the ESP-NOW receive path: observer frames are handed to
a registered handler before the MeshCore receive queue; every other frame is
unchanged.

Host harness (g++, ASan/UBSan off Windows) plus source contracts for the
device glue in ESPNOWRadio.cpp.
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "helpers/bridges/ObserverDemux.h"
using namespace mesh::observer;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

typedef std::vector<uint8_t> Bytes;

struct Sink { int calls = 0; std::vector<Bytes> frames; };
static void onObserver(const uint8_t* d, size_t n, void* ctx) {
  Sink* s = static_cast<Sink*>(ctx);
  s->calls++;
  s->frames.push_back(Bytes(d, d + n));
}

// The receive path as OnDataRecv runs it: the demux first, then the MeshCore
// receive queue (byte copies) for everything it does not take.
static void receive(const Bytes& f, FrameHandler h, void* ctx, std::vector<Bytes>& meshQueue) {
  if (!f.empty() && demuxObserverFrame(f.data(), f.size(), h, ctx)) return;
  meshQueue.push_back(f);
}

static Bytes packetFrame(uint8_t len, uint8_t seed) {
  PacketRecord p; std::memset(&p, 0, sizeof(p));
  p.rx_ms = 1234u + seed; p.snr_q4 = (int8_t)(seed - 20); p.rssi = -100; p.disp = Disposition::Forwarded;
  p.len = len;
  for (int i = 0; i < len; i++) p.raw[i] = (uint8_t)(i * 3 + seed);
  uint8_t buf[256];
  size_t n = encodePacket(p, buf, sizeof(buf));
  return Bytes(buf, buf + n);
}
static Bytes vitalsFrame() {
  Vitals v; std::memset(&v, 0, sizeof(v));
  v.uptime_s = 99; v.noise_floor = -117; v.rx_per_min = 4; v.batt_mv = 4000; v.freq = 910.525f; v.bw = 62.5f; v.sf = 7; v.cr = 5;
  uint8_t buf[256];
  size_t n = encodeVitals(v, buf, sizeof(buf));
  return Bytes(buf, buf + n);
}

// Frames the head's MeshCore/WiChat parsers must keep receiving untouched.
static std::vector<Bytes> meshCorpus() {
  std::vector<Bytes> c;
  uint32_t rng = 0x12345678u;
  for (int first = 0; first < 256; first++) {
    const size_t lens[] = {1, 2, 15, 16, 41, 64, 120, 184, 235, 249, 250};
    for (size_t len : lens) {
      Bytes f(len);
      for (size_t i = 0; i < len; i++) { rng = rng * 1103515245u + 12345u; f[i] = (uint8_t)(rng >> 16); }
      f[0] = (uint8_t)first;
      c.push_back(f);
    }
  }
  // Raw-mode fragments (FE 4D 43 46 ...) and WiChat-like frames (magic at 16/24).
  for (int k = 0; k < 8; k++) {
    Bytes frag(40 + k * 20, (uint8_t)k); frag[0] = 0xFE; frag[1] = 0x4D; frag[2] = 0x43; frag[3] = 0x46;
    c.push_back(frag);
    Bytes wichat(60 + k, 0x11); wichat[16] = 0xB0; wichat[17] = 0x42; wichat[24] = 0x53;
    c.push_back(wichat);
  }
  // Near misses of the observer header: right magic, wrong version / kind / length.
  Bytes pkt = packetFrame(30, 7), vit = vitalsFrame();
  Bytes badVersion = pkt; badVersion[4] = 2; c.push_back(badVersion);
  Bytes badKind = pkt; badKind[5] = 9; c.push_back(badKind);
  Bytes longer = pkt; longer.push_back(0); c.push_back(longer);
  Bytes shorter = pkt; shorter.pop_back(); c.push_back(shorter);
  Bytes vitLong = vit; vitLong.push_back(0); c.push_back(vitLong);
  Bytes vitShort = vit; vitShort.pop_back(); c.push_back(vitShort);
  Bytes headerOnly(pkt.begin(), pkt.begin() + 6); c.push_back(headerOnly);
  Bytes magicOnly(pkt.begin(), pkt.begin() + 4); c.push_back(magicOnly);
  return c;
}

static int observer_frames_bypass_meshcore_parser() {
  Sink sink; std::vector<Bytes> mesh;
  std::vector<Bytes> sent;
  const uint8_t lens[] = {1, 30, 184, 235};
  for (uint8_t len : lens) sent.push_back(packetFrame(len, len));
  sent.push_back(vitalsFrame());
  for (const Bytes& f : sent) {
    CHECK(isObserverFrame(f.data(), f.size()));
    receive(f, onObserver, &sink, mesh);
  }
  CHECK(mesh.empty());                       // MeshCore never saw one
  CHECK(sink.calls == (int)sent.size());
  CHECK(sink.frames == sent);                // handed over byte for byte, in order

  // The UI-side queue: bounded, drop-oldest, whole frames.
  FrameRing<4> ring;
  CHECK(ring.count() == 0 && ring.dropped() == 0);
  uint8_t out[FrameRing<4>::MAX_FRAME]; size_t n = 0;
  CHECK(!ring.pop(out, sizeof(out), n));
  for (int i = 0; i < 6; i++) CHECK(ring.push(packetFrame((uint8_t)(10 + i), (uint8_t)i).data(), packetFrame((uint8_t)(10 + i), (uint8_t)i).size()));
  CHECK(ring.count() == 4 && ring.dropped() == 2 && ring.pushed() == 6);
  for (int i = 2; i < 6; i++) {
    Bytes want = packetFrame((uint8_t)(10 + i), (uint8_t)i);
    CHECK(ring.pop(out, sizeof(out), n));
    CHECK(n == want.size() && std::memcmp(out, want.data(), n) == 0);
  }
  CHECK(!ring.pop(out, sizeof(out), n) && ring.count() == 0);
  // Invalid sizes are refused, not truncated.
  Bytes big(251, 1);
  CHECK(!ring.push(big.data(), big.size()));
  CHECK(!ring.push(big.data(), 0));
  CHECK(!ring.push(nullptr, 10));
  CHECK(ring.count() == 0 && ring.dropped() == 2 && ring.pushed() == 6);
  // A max-size frame round-trips; a too-small output buffer drops it (counted).
  Bytes full = packetFrame(235, 3);
  CHECK(full.size() == 250 && ring.push(full.data(), full.size()));
  CHECK(ring.pop(out, sizeof(out), n) && n == 250 && std::memcmp(out, full.data(), 250) == 0);
  CHECK(ring.push(full.data(), full.size()));
  uint8_t small[16];
  CHECK(!ring.pop(small, sizeof(small), n) && ring.count() == 0 && ring.dropped() == 3);
  // Wrap-around keeps order over many cycles.
  for (int i = 0; i < 50; i++) {
    Bytes a = packetFrame((uint8_t)(1 + i % 200), (uint8_t)i);
    CHECK(ring.push(a.data(), a.size()));
    CHECK(ring.pop(out, sizeof(out), n) && n == a.size() && std::memcmp(out, a.data(), n) == 0);
  }
  return 0;
}

static int meshcore_frames_unchanged_with_observer_registered() {
  const std::vector<Bytes> corpus = meshCorpus();
  Sink sink;
  std::vector<Bytes> withHandler, without;
  for (const Bytes& f : corpus) {
    receive(f, onObserver, &sink, withHandler);
    receive(f, nullptr, nullptr, without);
  }
  CHECK(sink.calls == 0);
  CHECK(withHandler == corpus);   // every frame, same order, byte for byte
  CHECK(without == corpus);
  // Without a registered handler even observer frames stay on the old path
  // (builds without a visual head behave exactly as before).
  std::vector<Bytes> plain;
  Bytes pkt = packetFrame(50, 1), vit = vitalsFrame();
  receive(pkt, nullptr, nullptr, plain);
  receive(vit, nullptr, nullptr, plain);
  CHECK(plain.size() == 2 && plain[0] == pkt && plain[1] == vit);
  CHECK(!demuxObserverFrame(nullptr, 0, onObserver, &sink) && sink.calls == 0);
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  std::string name = argv[1];
  if (name == "observer_frames_bypass_meshcore_parser") observer_frames_bypass_meshcore_parser();
  else if (name == "meshcore_frames_unchanged_with_observer_registered") meshcore_frames_unchanged_with_observer_registered();
  else { std::printf("unknown scenario %s\n", argv[1]); return 2; }
  if (failures) return 1;
  std::printf("PASS %s\n", argv[1]);
  return 0;
}
'''

def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def function_body(text, signature_regex):
    m = re.search(signature_regex, text)
    if m is None:
        raise AssertionError('function not found: ' + signature_regex)
    start = text.index('{', m.end())
    depth = 0
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise AssertionError('unbalanced body')


class ObserverDemuxTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            raise unittest.SkipTest('a host C++17 compiler is required')
        cls.compiler = compiler
        cls.sanitize = [] if os.name == 'nt' else [
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        cls.temp = tempfile.TemporaryDirectory(prefix='observer-demux-')
        work = Path(cls.temp.name)
        cls.exe = cls._build(work, 'demux', HARNESS, [])

    @classmethod
    def _build(cls, work, name, source, sources):
        cpp = work / (name + '.cpp')
        cpp.write_text(source, encoding='ascii')
        binary = work / (name + ('.exe' if os.name == 'nt' else ''))
        built = subprocess.run([
            cls.compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', *cls.sanitize,
            '-isystem', str(ROOT / 'src'), '-isystem', str(ROOT / 'test' / 'mocks'),
            str(cpp), *[str(s) for s in sources], '-o', str(binary),
        ], capture_output=True, text=True, timeout=300, cwd=str(ROOT / 'test'))
        if built.returncode != 0:
            raise AssertionError(built.stdout + built.stderr)
        return binary

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_scenario(self, name):
        ran = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=60)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('PASS ' + name, ran.stdout)

    def test_observer_frames_bypass_meshcore_parser(self):
        self.run_scenario('observer_frames_bypass_meshcore_parser')

    def test_meshcore_frames_unchanged_with_observer_registered(self):
        self.run_scenario('meshcore_frames_unchanged_with_observer_registered')




class ReceivePathContractTest(unittest.TestCase):
    """The device glue the host harness cannot run."""

    def test_on_data_recv_demuxes_before_meshcore_queue(self):
        raw = (ROOT / 'src' / 'helpers' / 'esp32' / 'ESPNOWRadio.cpp').read_bytes()
        self.assertNotIn(b'\n', raw.replace(b'\r\n', b''), 'ESPNOWRadio.cpp must stay CRLF')
        src = strip_comments(raw.decode('ascii'))
        self.assertIn('#include <helpers/bridges/ObserverDemux.h>', src)
        # Two signatures (Arduino 2/3) share one body: start at the last one.
        starts = [m.start() for m in re.finditer(r'static void OnDataRecv\(', src)]
        self.assertTrue(starts)
        body = function_body(src[starts[-1]:], r'static void OnDataRecv\(')
        accept = body.index('accepting_packets.load()')
        demux = body.index('demuxObserverFrame(')
        queue = body.index('rx_frames[')
        self.assertLess(accept, demux)
        self.assertLess(demux, queue, 'observer frames must be taken before the MeshCore queue')
        # The demux branch returns: observer frames never reach the queue.
        branch = body[demux:queue]
        self.assertRegex(branch, r'\)\s*\)\s*\{?\s*return\s*;')
        # Handler and context are published with release/acquire ordering.
        self.assertIn('observer_handler.load(std::memory_order_acquire)', body)
        setter = function_body(src, r'void ESPNOWRadio::setObserverHandler\(')
        self.assertLess(setter.index('observer_ctx.store'), setter.index('observer_handler.store'))

    def test_header_declares_handler_api(self):
        raw = (ROOT / 'src' / 'helpers' / 'esp32' / 'ESPNOWRadio.h').read_bytes()
        self.assertNotIn(b'\n', raw.replace(b'\r\n', b''), 'ESPNOWRadio.h must stay CRLF')
        self.assertRegex(raw.decode('ascii'),
                         r'static void setObserverHandler\(mesh::observer::FrameHandler handler,\s*void\* ctx\);')


if __name__ == '__main__':
    unittest.main()
