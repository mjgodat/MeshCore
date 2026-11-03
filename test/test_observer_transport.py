"""observer.transport: the observer feed over ESP-NOW, UDP or both (repeater side).

Covers the transport setting (parse, saved record, CLI incl. the channel rule),
the shared rate limiter fanning one frame out to both transports with
per-transport drop counters, and the UDP bridge framing of observer frames
(Observer / Subscribe kinds).
"""
from pathlib import Path
import tempfile
import unittest

from test_espnow_repeat import build_and_run
from test_observer_feed import settings_folder
from test_radio_receive_contract import method

ROOT = Path(__file__).resolve().parents[1]


TRANSPORT_CHECKS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <helpers/ObserverTransport.h>
using namespace mesh::observer;
int main() {
  // parse / names
  Transport t = Transport::Udp;
  assert(parseTransport("espnow", t) && t == Transport::EspNow);
  assert(parseTransport("udp", t) && t == Transport::Udp);
  assert(parseTransport("both", t) && t == Transport::Both);
  for (const char* bad : {"", "ESPNOW", "auto", "udp ", " both", "espnow,udp"}) {
    t = Transport::Udp; assert(!parseTransport(bad, t) && t == Transport::Udp);
  }
  assert(!parseTransport(nullptr, t));
  assert(!strcmp(transportName(Transport::EspNow), "espnow") && !strcmp(transportName(Transport::Both), "both"));
  assert(usesEspNow(Transport::EspNow) && !usesUdp(Transport::EspNow));
  assert(!usesEspNow(Transport::Udp) && usesUdp(Transport::Udp));
  assert(usesEspNow(Transport::Both) && usesUdp(Transport::Both));
  // saved record: round trip; every bit flip and every short read falls back
  uint8_t image[kTransportImageSize];
  for (Transport v : {Transport::EspNow, Transport::Udp, Transport::Both}) {
    encodeTransport(image, v);
    Transport out = Transport::EspNow;
    assert(decodeTransport(image, sizeof(image), out) && out == v);
  }
  encodeTransport(image, Transport::Both);
  for (size_t bit = 0; bit < sizeof(image) * 8; ++bit) {
    uint8_t bad[kTransportImageSize]; memcpy(bad, image, sizeof(bad));
    bad[bit / 8] ^= uint8_t(1U << (bit % 8));
    Transport out = Transport::EspNow;
    assert(!decodeTransport(bad, sizeof(bad), out) && out == Transport::EspNow);
  }
  for (size_t n = 0; n < sizeof(image); ++n) { Transport out; assert(!decodeTransport(image, n, out)); }
  // channel rule
  assert(checkChannel(6, 0) == ChannelCheck::Unknown && checkChannel(0, 6) == ChannelCheck::Unknown);
  assert(checkChannel(6, 6) == ChannelCheck::Ok && checkChannel(6, 11) == ChannelCheck::Mismatch);
  char reply[157];
  assert(checkTransportChange(Transport::Both, 6, 6, reply, sizeof(reply)));
  assert(checkTransportChange(Transport::Both, 6, 0, reply, sizeof(reply)));
  assert(checkTransportChange(Transport::Udp, 6, 11, reply, sizeof(reply)));
  assert(checkTransportChange(Transport::EspNow, 6, 11, reply, sizeof(reply)));
  assert(!checkTransportChange(Transport::Both, 6, 11, reply, sizeof(reply)));
  assert(!strncmp(reply, "Error: both refused.", 20));
  assert(strstr(reply, "AP on ch 11") && strstr(reply, "ESP-NOW on ch 6") && strstr(reply, "router to ch 6")
         && strstr(reply, "ch 11 on all ESP-NOW nodes"));
  assert(strlen(reply) < 157);
  return 0;
}
'''


SETTINGS_CHECKS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
#include "ObserverSettings.h"
#include "settings_under_test.cpp"
using namespace mesh::espnow;
static char reply[160];
static ObserverRuntime rt;
static void describe(void*, char* out, size_t cap) { snprintf(out, cap, "(espnow ch 6 running)"); }
static bool run(const char* command, const ObserverRuntime* runtime = &rt) {
  strcpy(reply, "untouched");
  return handleEspNowObserverCommand(command, reply, 157, runtime);
}
int main() {
  MemoryFS fs; const char* path = "/observer_transport";
  loadEspNowObserverSettings(&fs);
  // default espnow, nothing written; an upgraded repeater behaves as before
  assert(observerTransport() == mesh::observer::Transport::EspNow && fs.files.empty());
  rt.espnow_channel = 6; rt.describe = describe;
  assert(run("get observer.transport") && !strcmp(reply, "> espnow (espnow ch 6 running)"));
  // no UDP bridge compiled: udp and both refused, nothing saved
  for (const char* c : {"set observer.transport udp", "set observer.transport both"}) {
    assert(run(c) && !strcmp(reply, "Error: no UDP bridge in this firmware") && fs.files.empty());
  }
  assert(run("set observer.transport udp", nullptr) && !strncmp(reply, "Error:", 6));
  rt.udp_available = true;
  for (const char* bad : {"set observer.transport", "set observer.transport auto", "set observer.transport UDP",
                          "set observer.transport udp x", "get observer.transport x"})
    assert(run(bad) && !strncmp(reply, "Error:", 6));
  assert(fs.files.empty());
  // channel rule: both refused while the AP is on another channel
  rt.ap_channel = 11;
  assert(run("set observer.transport both") && !strncmp(reply, "Error: both refused.", 20));
  assert(fs.files.empty() && observerTransport() == mesh::observer::Transport::EspNow);
  assert(run("set observer.transport udp") && !strcmp(reply, "OK - observer.transport saved"));
  assert(observerTransport() == mesh::observer::Transport::Udp && fs.files.at(path).size() == 9);
  rt.ap_channel = 6;
  assert(run("set observer.transport both") && !strcmp(reply, "OK - observer.transport saved"));
  assert(run("set observer.transport\tboth") && !strcmp(reply, "OK - observer.transport unchanged"));
  loadEspNowObserverSettings(&fs);
  assert(observerTransport() == mesh::observer::Transport::Both);
  // corrupt record: back to espnow, explicit repair allowed
  fs.files[path][4] ^= 3; loadEspNowObserverSettings(&fs);
  assert(observerTransport() == mesh::observer::Transport::EspNow);
  assert(run("get observer.transport") && strstr(reply, "settings unavailable"));
  assert(run("set observer.transport espnow") && !strcmp(reply, "OK - observer.transport saved"));
  loadEspNowObserverSettings(&fs); assert(observerTransport() == mesh::observer::Transport::EspNow);
  // failed write keeps the previous value
  fs.fail_write = true;
  assert(run("set observer.transport udp") && !strcmp(reply, "Error: observer.transport not saved"));
  assert(observerTransport() == mesh::observer::Transport::EspNow); fs.fail_write = false;
  // `observer` is the transport-neutral alias of `espnow.observer` (one flag)
  assert(run("get observer") && !strcmp(reply, "> off"));
  assert(run("set observer on") && !strcmp(reply, "OK - observer saved") && espNowObserverEnabled());
  assert(run("get espnow.observer") && !strcmp(reply, "> on"));
  assert(run("set espnow.observer on") && !strcmp(reply, "OK - espnow.observer unchanged"));
  assert(run("set observer off") && !strcmp(reply, "OK - observer saved") && !espNowObserverEnabled());
  for (const char* bad : {"set observer", "set observer 1", "get observer on"})
    assert(run(bad) && !strncmp(reply, "Error:", 6));
  for (const char* other : {"get observers", "set observer.transports udp", "get observer.status", "set observerx on"})
    assert(!run(other) && !strcmp(reply, "untouched"));
  return 0;
}
'''


FANOUT_CHECKS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <helpers/bridges/ObserverRateLimiter.h>
using namespace mesh::observer;

struct Sink : FanoutSink {
  bool active = true, pending = false;
  Outbox box;
  bool observerActive() const override { return active; }
  bool observerMeshPending() const override { return pending; }
  Outbox& observerOutbox() override { return box; }
  size_t drain() { uint8_t f[MAX_PACKET_FRAME]; size_t n = 0; while (box.pop(f, sizeof(f))) ++n; return n; }
};

static std::vector<uint8_t> record(uint32_t id) {
  PacketRecord r; memset(&r, 0, sizeof(r)); r.rx_ms = id; r.len = 4; memcpy(r.raw, &id, 4);
  std::vector<uint8_t> f(MAX_PACKET_FRAME); f.resize(encodePacket(r, f.data(), f.size())); return f;
}
static std::vector<uint8_t> vitals(uint32_t up) {
  Vitals v; memset(&v, 0, sizeof(v)); v.uptime_s = up;
  std::vector<uint8_t> f(VITALS_FRAME); encodeVitals(v, f.data(), f.size()); return f;
}

int main() {
  // 1) both transports: one shared bucket (burst 40), each admitted record on both
  {
    Sink a, b; FanoutSink* sinks[] = {&a, &b}; Fanout fan;
    size_t queued_a = 0, queued_b = 0;
    for (uint32_t i = 0; i < 60; ++i) {
      auto f = record(i);
      const uint8_t n = fan.offer(f.data(), f.size(), 1000, sinks, 2);
      assert(n == (i < 40 ? 2 : 0));
      queued_a += a.drain(); queued_b += b.drain();
    }
    assert(queued_a == 40 && queued_b == 40);
    assert(a.box.stats().rate_drops == 20 && b.box.stats().rate_drops == 20);  // per transport
    // 20/s refill: 1 s later 20 more on both, never 40
    size_t later = 0;
    for (uint32_t i = 0; i < 40; ++i) { auto f = record(100 + i); later += fan.offer(f.data(), f.size(), 2000, sinks, 2); a.drain(); b.drain(); }
    assert(later == 40);   // 20 records x 2 transports
  }
  // 2) mesh pending on one transport: that one counts a preempt, the other still
  //    gets the record, and only one token is spent
  {
    Sink a, b; FanoutSink* sinks[] = {&a, &b}; Fanout fan;
    b.pending = true;
    for (uint32_t i = 0; i < 40; ++i) { auto f = record(i); assert(fan.offer(f.data(), f.size(), 5, sinks, 2) == 1); a.drain(); }
    assert(b.box.stats().preempt_drops == 40 && b.box.empty() && a.box.stats().queued == 40);
    auto f = record(99); assert(fan.offer(f.data(), f.size(), 5, sinks, 2) == 0);  // bucket empty now
    assert(a.box.stats().rate_drops == 1 && b.box.stats().rate_drops == 0);
    // both pending: no token spent at all
    Fanout fan2; a.pending = true;
    for (int i = 0; i < 100; ++i) { auto g = record(i); assert(fan2.offer(g.data(), g.size(), 7, sinks, 2) == 0); }
    a.pending = b.pending = false;
    for (uint32_t i = 0; i < 40; ++i) { auto g = record(i); assert(fan2.offer(g.data(), g.size(), 7, sinks, 2) == 2); a.drain(); b.drain(); }
  }
  // 3) vitals bypass the bucket and mesh-pending, go to every active transport
  {
    Sink a, b; FanoutSink* sinks[] = {&a, &b}; Fanout fan;
    for (uint32_t i = 0; i < 40; ++i) { auto f = record(i); fan.offer(f.data(), f.size(), 1, sinks, 2); a.drain(); b.drain(); }
    a.pending = true;
    auto v = vitals(5); assert(fan.offer(v.data(), v.size(), 1, sinks, 2) == 2);
  }
  // 4) inactive transports get nothing and count nothing; no active sink spends no token
  {
    Sink a, b; FanoutSink* sinks[] = {&a, &b}; Fanout fan; b.active = false;
    auto f = record(1); assert(fan.offer(f.data(), f.size(), 1, sinks, 2) == 1);
    assert(b.box.empty() && b.box.stats().queued == 0 && b.box.stats().rate_drops == 0);
    a.active = false;
    for (int i = 0; i < 100; ++i) assert(fan.offer(f.data(), f.size(), 1, sinks, 2) == 0);
    a.active = true; a.drain();
    for (int i = 0; i < 39; ++i) assert(fan.offer(f.data(), f.size(), 1, sinks, 2) == 1);   // 40 - 1 spent above
    assert(fan.offer(f.data(), f.size(), 1, sinks, 2) == 0);
    // null and invalid frames
    FanoutSink* none[] = {nullptr, &a}; uint8_t junk[6] = {1, 2, 3, 4, 5, 6};
    assert(fan.offer(junk, sizeof(junk), 1, none, 2) == 0 && fan.offer(nullptr, 0, 1, none, 2) == 0);
  }
  // 5) one transport (the default): identical to Outbox::offer() step by step
  {
    Sink a; FanoutSink* sinks[] = {&a}; Fanout fan; Outbox ref;
    uint32_t now = 0, seed = 12345;
    for (int i = 0; i < 4000; ++i) {
      seed = seed * 1103515245u + 12345u;
      now += (seed >> 16) % 120;
      const bool pending = ((seed >> 8) & 7) == 0, is_vitals = ((seed >> 4) & 15) == 0;
      a.pending = pending;
      auto f = is_vitals ? vitals(uint32_t(i)) : record(uint32_t(i));
      const bool q1 = fan.offer(f.data(), f.size(), now, sinks, 1) == 1;
      const bool q2 = ref.offer(f.data(), f.size(), now, pending) == Offer::Queued;
      assert(q1 == q2);
      if (((seed >> 12) & 3) == 0) {
        uint8_t x[MAX_PACKET_FRAME], y[MAX_PACKET_FRAME];
        const size_t nx = a.box.pop(x, sizeof(x)), ny = ref.pop(y, sizeof(y));
        assert(nx == ny && !memcmp(x, y, nx));
      }
    }
    const auto& s1 = a.box.stats(); const auto& s2 = ref.stats();
    assert(s1.queued == s2.queued && s1.rate_drops == s2.rate_drops && s1.preempt_drops == s2.preempt_drops
           && s1.overflow_drops == s2.overflow_drops);
  }
  return 0;
}
'''


UDP_FRAME_CHECKS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <helpers/bridges/UDPBridgeProtocol.h>
#include <helpers/bridges/ObserverFrame.h>
using namespace mesh::bridge::udp;
// Stand-in MAC: any keyed digest exercises the framing; the bridge uses HMAC-SHA256.
static bool mac(void* ctx, const uint8_t* data, size_t size, uint8_t digest[32]) {
  const uint8_t key = *static_cast<uint8_t*>(ctx);
  uint32_t h = 2166136261u ^ key;
  for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 16777619u;
  for (int i = 0; i < 32; ++i) { h = h * 16777619u + key; digest[i] = uint8_t(h >> 24); }
  return true;
}
int main() {
  uint8_t key = 7, other = 8;
  // An observer frame inside a UDP Observer frame decodes to the same record.
  mesh::observer::PacketRecord r; memset(&r, 0, sizeof(r));
  r.rx_ms = 1234; r.snr_q4 = -20; r.rssi = -101; r.len = 30;
  for (int i = 0; i < 30; ++i) r.raw[i] = uint8_t(i * 3);
  uint8_t obs[mesh::observer::MAX_PACKET_FRAME];
  const size_t obs_len = mesh::observer::encodePacket(r, obs, sizeof(obs));
  Frame f; f.kind = Kind::Observer; f.sender = 0x1111; f.recipient = 0x2222; f.sequence = 9;
  f.payload = obs; f.length = uint16_t(obs_len);
  uint8_t wire[MAX_FRAME];
  const size_t n = encode(wire, sizeof(wire), f, mac, &key);
  assert(n == HEADER_SIZE + obs_len + TAG_SIZE && wire[5] == 6);
  Frame g;
  assert(decode(wire, n, g, mac, &key) == Decode::Ok && g.kind == Kind::Observer && g.length == obs_len);
  mesh::observer::PacketRecord back;
  assert(mesh::observer::decodePacket(g.payload, g.length, back) && back.rx_ms == 1234 && back.len == 30
         && !memcmp(back.raw, r.raw, 30));
  assert(decode(wire, n, g, mac, &other) == Decode::Authentication);
  wire[HEADER_SIZE + 3] ^= 1; assert(decode(wire, n, g, mac, &key) == Decode::Authentication);
  // The largest observer frame (250) fits one UDP bridge frame.
  assert(mesh::observer::MAX_PACKET_FRAME <= MAX_PAYLOAD);
  // Observer needs a recipient and at least an observer header.
  f.recipient = 0; assert(encode(wire, sizeof(wire), f, mac, &key) == 0);
  f.recipient = 0x2222; f.length = 5; assert(encode(wire, sizeof(wire), f, mac, &key) == 0);
  // Subscribe: exactly one byte, 0 or 1.
  for (uint8_t v : {0, 1}) {
    uint8_t p = v; Frame s; s.kind = Kind::Subscribe; s.sender = 3; s.recipient = 4; s.sequence = 1;
    s.payload = &p; s.length = 1;
    const size_t m = encode(wire, sizeof(wire), s, mac, &key);
    assert(m == HEADER_SIZE + 1 + TAG_SIZE && decode(wire, m, g, mac, &key) == Decode::Ok
           && g.kind == Kind::Subscribe && g.payload[0] == v);
  }
  { uint8_t p = 2; Frame s; s.kind = Kind::Subscribe; s.sender = 3; s.recipient = 4; s.sequence = 1;
    s.payload = &p; s.length = 1; assert(encode(wire, sizeof(wire), s, mac, &key) == 0);
    uint8_t q[2] = {1, 1}; s.payload = q; s.length = 2; assert(encode(wire, sizeof(wire), s, mac, &key) == 0); }
  // Unknown kinds stay malformed (8+).
  { uint8_t p[2] = {1, 2}; Frame m; m.kind = Kind::Mesh; m.sender = 3; m.recipient = 4; m.sequence = 1;
    m.payload = p; m.length = 2; const size_t k = encode(wire, sizeof(wire), m, mac, &key);
    assert(k); wire[5] = 8; assert(decode(wire, k, g, mac, &key) == Decode::Malformed); }
  return 0;
}
'''


class ObserverTransportSettingTests(unittest.TestCase):
    def test_transport_parse_record_and_channel_rule(self):
        build_and_run(self, TRANSPORT_CHECKS)

    def test_transport_cli_and_storage(self):
        with tempfile.TemporaryDirectory(prefix="observer-transport-") as tmp:
            folder = settings_folder(tmp)
            (folder / "ObserverTransport.h").write_bytes((ROOT / "src/helpers/ObserverTransport.h").read_bytes())
            build_and_run(self, SETTINGS_CHECKS, includes=[folder, ROOT / "src/helpers"],
                          defines=["ESP32_PLATFORM=1", "WITH_ESPNOW_BRIDGE=1"], strict=False)


class ObserverFanoutTests(unittest.TestCase):
    def test_shared_rate_limit_and_per_transport_drops(self):
        build_and_run(self, FANOUT_CHECKS)


class ObserverUdpFramingTests(unittest.TestCase):
    def test_observer_and_subscribe_frames(self):
        build_and_run(self, UDP_FRAME_CHECKS)


class ObserverTransportWiringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repeater = (ROOT / "examples/simple_repeater/MyMesh.cpp").read_text()
        cls.udp = (ROOT / "src/helpers/bridges/UDPBridge.cpp").read_text()

    def test_repeater_offers_every_frame_through_the_fanout(self):
        self.assertEqual(self.repeater.count("offerObserverFrame(frame, frame_len)"), 2)
        self.assertNotIn("sendObserverFrame(", self.repeater)
        offer = method(self.repeater, "void MyMesh::offerObserverFrame(")
        self.assertIn("_observer_fanout.offer(", offer)

    def test_udp_observer_only_to_live_subscribers_after_mesh(self):
        loop = method(self.udp, "void UDPBridge::loop()")
        self.assertLess(loop.index("pumpTransmit();"), loop.index("pumpObserver();"))
        pump = method(self.udp, "void UDPBridge::pumpObserver()")
        self.assertIn("if (_tx_mask || !_observer || _observer->empty()) return;", pump)
        self.assertIn("observerMask()", pump)
        mask = method(self.udp, "uint8_t UDPBridge::observerMask() const")
        self.assertIn("_sessions[i].remote", mask)
        self.assertIn("!udp::due(now, _observer_until[i])", mask)
        receive = method(self.udp, "void UDPBridge::receiveFrame(")
        # Subscribe is honoured only after the replay-checked admit().
        self.assertLess(receive.index("session.admit("), receive.index("Kind::Subscribe"))


if __name__ == "__main__":
    unittest.main()
