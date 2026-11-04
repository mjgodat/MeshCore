"""Repeater discovery, auto-pairing and the bridged-ingress rate limit.

- DiscoveryFrame.h: announce/reply codec with the HMAC-SHA256 tag (checked
  against Python's hmac), strictness, freshness, the auto-paired head table
  (slots, expiry, rate limit) and bridge.rxlimit's token bucket.
- BridgeDiscoverySettings: bridge.announce / bridge.rxlimit / bridge.autopeers.
- The real UDPBridge.cpp (test_udp_bridge fixture): discovery frames from any
  address reach the role, announces go to the subnet broadcast and the peers,
  an auto peer completes the authenticated session and carries mesh traffic,
  manual peers win, a restart forgets auto peers, the rate limit drops floods.
- The real ESPNowBridge.cpp (test_bridge_recovery fixture): an announce goes
  out only into an idle link (wrapped when the link is), received replies go
  to the role and never into the mesh, the rate limit applies.
"""
from pathlib import Path
import hashlib
import hmac
import shutil
import subprocess
import tempfile
import unittest

from test_espnow_repeat import build_and_run
from test_observer_feed import settings_folder

ROOT = Path(__file__).resolve().parents[1]


def tag(key, data):
    return hmac.new(key, b"MeshCore discovery v1\0" + data, hashlib.sha256).digest()[:8]


CODEC_CHECKS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <SHA256.h>
#include <helpers/bridges/DiscoveryFrame.h>
using namespace mesh::discovery;
static bool mac(void* ctx, const uint8_t* data, size_t len, uint8_t digest[32]) {
  const char* key = static_cast<const char*>(ctx);
  if (!key || !*key) return false;
  SHA256 sha; sha.resetHMAC(reinterpret_cast<const uint8_t*>(key), strlen(key));
  sha.update(kDomain, sizeof(kDomain)); sha.update(data, len);
  sha.finalizeHMAC(reinterpret_cast<const uint8_t*>(key), strlen(key), digest, 32);
  return true;
}
static char key[] = "fleet-test";
static char other[] = "other";
int main() {
  Announce a; a.boot = 0x11223344; a.counter = 7;
  const uint8_t prefix[6] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6};
  memcpy(a.prefix, prefix, 6); strcpy(a.name, "CaptCallahan-Base");   // 17 chars: cut to 16
  a.espnow_channel = 6; a.espnow_format = 1; a.caps = CAP_ESPNOW | CAP_UDP | CAP_AUTOPAIR;
  const uint8_t ip[4] = {192, 168, 1, 60}; memcpy(a.ip, ip, 4); a.port = 4404;
  uint8_t f[MAX_FRAME];
  const size_t n = encodeAnnounce(a, mac, key, f, sizeof(f));
  assert(n == ANNOUNCE_FRAME && isDiscoveryFrame(f, n) && isAnnounce(f, n) && !isReply(f, n));
  printf("ANNOUNCE ");
  for (size_t i = 0; i < n; ++i) printf("%02x", f[i]);
  printf("\n");
  Announce b;
  assert(decodeAnnounce(f, n, mac, key, b));
  assert(b.boot == a.boot && b.counter == 7 && !memcmp(b.prefix, prefix, 6) && !strcmp(b.name, "CaptCallahan-Bas"));
  assert(b.espnow_channel == 6 && b.espnow_format == 1 && b.caps == a.caps && !memcmp(b.ip, ip, 4) && b.port == 4404);
  // every bit flip, every short or long length, the wrong key: rejected, out untouched
  for (size_t bit = 0; bit < n * 8; ++bit) {
    f[bit / 8] ^= uint8_t(1U << (bit % 8));
    Announce u; u.counter = 99;
    assert(!decodeAnnounce(f, n, mac, key, u) && u.counter == 99);
    f[bit / 8] ^= uint8_t(1U << (bit % 8));
  }
  for (size_t len = 0; len < n; ++len) assert(!isDiscoveryFrame(f, len) && !decodeAnnounce(f, len, mac, key, b));
  assert(!decodeAnnounce(f, n, mac, other, b));
  assert(!decodeAnnounce(f, n, nullptr, key, b));
  assert(encodeAnnounce(a, mac, const_cast<char*>(""), f, sizeof(f)) == 0);   // no secret: nothing sent
  assert(encodeAnnounce(a, mac, key, f, n - 1) == 0);
  Announce zero = a; zero.boot = 0; assert(encodeAnnounce(zero, mac, key, f, sizeof(f)) == 0);
  // not confused with MeshCore, observer or wrapped frames
  const uint8_t observer[6] = {0xB0, 0x42, 0x53, 0x4F, 1, 2};
  assert(!isDiscoveryFrame(observer, 6));

  Reply r; r.boot = a.boot; r.counter = 7; memcpy(r.prefix, prefix, 6); strcpy(r.name, "CYD head");
  r.flags = 0; const uint8_t hip[4] = {192, 168, 1, 77}; memcpy(r.ip, hip, 4); r.port = 4404;
  uint8_t g[MAX_FRAME];
  const size_t m = encodeReply(r, mac, key, g, sizeof(g));
  assert(m == REPLY_FRAME && isReply(g, m));
  Reply q; assert(decodeReply(g, m, mac, key, q) && q.port == 4404 && q.flags == 0 && !strcmp(q.name, "CYD head"));
  assert(!decodeReply(g, m, mac, other, q));
  assert(!decodeAnnounce(g, m, mac, key, b));
  // freshness: this boot, one of the last 12 counters
  assert(freshReply(q, a.boot, 7) && freshReply(q, a.boot, 18) && !freshReply(q, a.boot, 19));
  assert(!freshReply(q, a.boot, 6) && !freshReply(q, a.boot + 1, 7));

  // AutoPeers: 4 slots, 5 min, 4 new heads per minute
  AutoPeers peers; uint32_t now = 1000;
  Reply h[6];
  for (int i = 0; i < 6; ++i) {
    h[i] = r; h[i].prefix[0] = uint8_t(i); h[i].ip[3] = uint8_t(10 + i);
  }
  assert(peers.offer(h[0], now) == PairResult::Added && peers.count() == 1);
  // review F2: a recorded reply (same or older counter) cannot be replayed
  assert(peers.offer(h[0], now + 5) == PairResult::Replay && peers.replays() == 1);
  h[0].counter = 8;
  assert(peers.offer(h[0], now + 10) == PairResult::Refreshed && peers.count() == 1);
  Reply old = h[0]; old.counter = 7; old.ip[3] = 66;
  assert(peers.offer(old, now + 11) == PairResult::Replay && peers.slot(0).ip[3] == 10);
  Reply moved = h[0]; moved.ip[3] = 99; moved.counter = 9;
  assert(peers.offer(moved, now + 20) == PairResult::Moved && peers.slot(peers.last()).ip[3] == 99);
  assert(peers.offer(h[1], now) == PairResult::Added && peers.offer(h[2], now) == PairResult::Added);
  assert(peers.offer(h[3], now) == PairResult::Added && peers.count() == 4);
  assert(peers.offer(h[4], now) == PairResult::Full && peers.full() == 1);
  Reply bad = h[4]; bad.ip[0] = 127; assert(peers.offer(bad, now) == PairResult::Invalid);
  bad = h[4]; bad.port = 0; assert(peers.offer(bad, now) == PairResult::Invalid);
  // expiry: refreshed ones live on
  h[1].counter = 8;
  assert(peers.offer(h[1], now + 200000) == PairResult::Refreshed);
  const uint8_t gone = peers.expire(now + kAutoPeerTtlMs + 100);
  assert(peers.count() == 1 && (gone & 0x0F) == 0x0D);
  // new heads after expiry: the rate limit (4 per minute, all spent) holds back the 5th
  now += kAutoPeerTtlMs + 200;   // > 5 min later: tokens are back
  assert(peers.offer(h[4], now) == PairResult::Added && peers.offer(h[5], now) == PairResult::Added);
  assert(peers.offer(h[2], now) == PairResult::Added);
  assert(peers.count() == 4 && peers.offer(h[3], now) == PairResult::Full);
  peers.clear(); AutoPeers fresh;
  for (int i = 0; i < 4; ++i) assert(fresh.offer(h[i], 5000) == PairResult::Added);
  fresh.expire(5000 + kAutoPeerTtlMs);
  assert(fresh.count() == 0 && fresh.offer(h[4], 5001 + kAutoPeerTtlMs) == PairResult::Added);  // tokens refilled
  // one token back every 15 s once all four are spent
  AutoPeers rl2;
  for (int i = 0; i < 4; ++i) { assert(rl2.offer(h[i], 100) == PairResult::Added); }
  rl2.clear();
  assert(rl2.offer(h[4], 200) == PairResult::RateLimited && rl2.limited() == 1);
  assert(rl2.offer(h[4], 100 + 15000) == PairResult::Added);
  assert(rl2.offer(h[5], 100 + 15001) == PairResult::RateLimited);

  // RxLimiter: default 0 (no limit, review F1); suggested 300/min, burst 60
  RxLimiter l;
  assert(kRxLimitDefault == 0 && l.rate() == 0 && l.burst() == 0);
  for (int i = 0; i < 1000; ++i) assert(l.admit(5));
  l.configure(kRxLimitSuggested);
  assert(l.rate() == 300 && l.burst() == 60);
  int passed = 0;
  for (int i = 0; i < 100; ++i) passed += l.admit(10) ? 1 : 0;
  assert(passed == 60 && l.dropped() == 40);
  assert(!l.admit(10 + 199) && l.admit(10 + 200 + 1));   // 5 per second
  l.configure(0);
  for (int i = 0; i < 1000; ++i) assert(l.admit(20));
  l.configure(3); assert(l.burst() == 1 && l.admit(0) && !l.admit(1) && l.admit(20001));
  l.configure(10); assert(l.burst() == 2);
  l.configure(9000); assert(l.rate() == kRxLimitMax);
  assert(&rxLimiter() == &rxLimiter());
  return 0;
}
'''


SETTINGS_CHECKS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
#include "BridgeDiscoverySettings.h"
#include "settings_under_test.cpp"
using namespace mesh::discovery;
static char reply[160];
static void peers(void*, char* out, size_t cap) { snprintf(out, cap, "> 1 10.0.0.5:4404/A1B2/290s"); }
static bool run(const char* command, const DiscoveryRuntime* rt = nullptr) {
  strcpy(reply, "untouched");
  return handleBridgeDiscoveryCommand(command, reply, 157, rt);
}
int main() {
  MemoryFS fs; const char* path = "/bridge_discovery";
  loadBridgeDiscoverySettings(&fs);
  // defaults: announce auto (only while UDP runs), rxlimit 0 = off (review F1, F3)
  assert(bridgeAnnounceMode() == AnnounceMode::Auto && bridgeRxLimit() == 0 && fs.files.empty());
  assert(!bridgeAnnounceActive(false) && bridgeAnnounceActive(true));
  assert(rxLimiter().rate() == 0);
  assert(run("get bridge.announce") && !strcmp(reply, "> auto"));
  assert(run("get bridge.rxlimit") && !strncmp(reply, "> 0 (no limit) passed=", 22));
  // udp.secret: keys UDP and discovery; off = bridge.secret; never printed (review F6)
  assert(run("get udp.secret") && !strcmp(reply, "> off (bridge.secret)") && !udpSecretOverride()[0]);
  assert(!strcmp(udpSessionSecret("bridge-key"), "bridge-key"));
  assert(run("set udp.secret lan-key") && !strcmp(reply, "OK - udp.secret saved; UDP sessions restart"));
  assert(!strcmp(udpSessionSecret("bridge-key"), "lan-key"));
  assert(run("set udp.secret lan-key") && !strcmp(reply, "OK - udp.secret saved; UDP sessions restart"));
  assert(run("get udp.secret") && !strcmp(reply, "> set"));
  for (const char* bad : {"set udp.secret", "set udp.secret 0123456789abcdef"})
    assert(run(bad) && !strncmp(reply, "Error:", 6));
  loadBridgeDiscoverySettings(&fs); assert(!strcmp(udpSessionSecret("x"), "lan-key"));
  assert(run("set udp.secret off") && !strcmp(udpSessionSecret("bridge-key"), "bridge-key"));
  fs.files.clear(); loadBridgeDiscoverySettings(&fs);
  assert(run("get bridge.autopeers") && !strcmp(reply, "> none"));
  DiscoveryRuntime rt; rt.describe_peers = peers;
  assert(run("get bridge.autopeers", &rt) && !strcmp(reply, "> 1 10.0.0.5:4404/A1B2/290s"));
  for (const char* bad : {"set bridge.announce", "set bridge.announce 1", "set bridge.announce AUTO", "set bridge.rxlimit",
                          "set bridge.rxlimit -1", "set bridge.rxlimit 6001", "set bridge.rxlimit 10x",
                          "get bridge.rxlimit x", "get bridge.autopeers x"})
    assert(run(bad) && !strncmp(reply, "Error:", 6));
  assert(fs.files.empty());
  assert(run("set bridge.rxlimit 600") && !strcmp(reply, "OK - bridge.rxlimit saved"));
  assert(bridgeRxLimit() == 600 && rxLimiter().rate() == 600 && fs.files.at(path).size() == 27);
  assert(run("set bridge.announce off") && !strcmp(reply, "OK - bridge.announce saved"));
  assert(bridgeAnnounceMode() == AnnounceMode::Off && !bridgeAnnounceActive(true));
  assert(run("set bridge.announce off") && !strcmp(reply, "OK - bridge.announce unchanged"));
  assert(run("set bridge.rxlimit 0") && !strcmp(reply, "OK - bridge.rxlimit saved"));
  assert(run("get bridge.rxlimit") && !strncmp(reply, "> 0 (no limit)", 14));
  loadBridgeDiscoverySettings(&fs);
  assert(bridgeAnnounceMode() == AnnounceMode::Off && bridgeRxLimit() == 0 && rxLimiter().rate() == 0);
  fs.files[path][5] ^= 1; loadBridgeDiscoverySettings(&fs);
  assert(bridgeAnnounceMode() == AnnounceMode::Auto && bridgeRxLimit() == 0);
  assert(run("get bridge.announce") && strstr(reply, "unavailable"));
  assert(run("set bridge.announce on") && !strcmp(reply, "OK - bridge.announce saved") && bridgeAnnounceActive(false));
  fs.fail_write = true;
  assert(run("set bridge.rxlimit 5") && !strcmp(reply, "Error: bridge.rxlimit not saved") && bridgeRxLimit() == 0);
  fs.fail_write = false;
  for (const char* other : {"get bridge.format", "set bridge.secret x", "get bridge.announces", "set bridge.rxlimits 4"})
    assert(!run(other) && !strcmp(reply, "untouched"));
  return 0;
}
'''


ESPNOW_ONLY_SETTINGS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
#include "BridgeDiscoverySettings.h"
#include "settings_under_test.cpp"
#ifdef MESH_BRIDGE_DISCOVERY
#error discovery must be compiled out without the UDP bridge
#endif
using namespace mesh::discovery;
int main() {
  MemoryFS fs; loadBridgeDiscoverySettings(&fs);
  char reply[160];
  assert(handleBridgeDiscoveryCommand("set bridge.rxlimit 300", reply, 157) && !strcmp(reply, "OK - bridge.rxlimit saved"));
  for (const char* c : {"get bridge.announce", "set bridge.announce on", "get bridge.autopeers", "get udp.secret",
                        "set udp.secret x"}) {
    strcpy(reply, "untouched"); assert(!handleBridgeDiscoveryCommand(c, reply, 157) && !strcmp(reply, "untouched"));
  }
  return 0;
}
'''


UDP_CHECKS = r'''
#include <helpers/bridges/DiscoveryFrame.h>
struct Seen { std::vector<std::vector<uint8_t>> frames; std::vector<udp::Peer> from; };
static void sink(void* ctx, const uint8_t* f, size_t n, const uint8_t* ip, uint16_t port) {
  Seen* s = static_cast<Seen*>(ctx); s->frames.emplace_back(f, f + n);
  udp::Peer p; memcpy(p.ip, ip, 4); p.port = port; s->from.push_back(p);
}
static std::vector<uint8_t> fake_reply() {
  std::vector<uint8_t> f(mesh::discovery::REPLY_FRAME, 0);
  f[0] = 0xB0; f[1] = 0x42; f[2] = 0x44; f[3] = 0x43; f[4] = 1; f[5] = 2;   // tag checked by the role
  return f;
}
static void discovery_runtime() {
  // Repeater A: no manual peer. Head B: manual peer = A (as a head's udp.peer).
  Node a(1, 2), b(2, 1);
  a.config.peers[0] = udp::Peer();
  a.start(); b.start();
  assert(a.bridge.isRunning() && b.bridge.isRunning() && udp_test::broadcast_enabled);
  Seen seen; a.bridge.setDiscoverySink(sink, &seen);
  // a discovery frame from an unknown address reaches the role with its source
  udp_test::Datagram d; d.source.sin_family = AF_INET; d.source.sin_port = htons(5555);
  d.source.sin_addr.s_addr = address(77); d.bytes = fake_reply();
  udp_test::inject(a.fd, d);
  run(a, b, 2);
  assert(seen.frames.size() == 1 && seen.from[0].ip[3] == 77 && seen.from[0].port == 5555);
  assert(a.stats("drops").find("foreign=0") != std::string::npos);
  // B cannot reach A yet (A does not know B): no session
  run(a, b, 30);
  assert(a.status().find("ready=0") != std::string::npos);
  // announce: subnet broadcast on udp.port, plus every configured peer
  uint8_t ann[mesh::discovery::ANNOUNCE_FRAME] = {0xB0, 0x42, 0x44, 0x43, 1, 1};
  const size_t before = udp_test::sent.size();
  assert(a.bridge.sendDiscovery(ann, sizeof(ann)));
  assert(udp_test::sent.size() == before + 1);
  const uint8_t* target = reinterpret_cast<const uint8_t*>(&udp_test::sent.back().target.sin_addr.s_addr);
  assert(target[0] == 10 && target[3] == 255 && ntohs(udp_test::sent.back().target.sin_port) == 4404);
  assert(!a.bridge.sendDiscovery(ann, sizeof(ann) - 1));        // not a discovery frame
  // B receives the broadcast (any address) on its socket
  Seen at_b; b.bridge.setDiscoverySink(sink, &at_b); run(a, b, 1);
  assert(at_b.frames.size() == 1 && at_b.from[0].ip[3] == 1);
  // auto peer: A adds B at run time; the session completes and mesh flows both ways
  const uint8_t bip[4] = {10, 11, 12, 2};
  assert(a.bridge.addAutoPeer(bip, 4404) == 0 && a.bridge.autoPeerCount() == 1);
  assert(a.bridge.addAutoPeer(bip, 4404) == 0 && a.bridge.autoPeerCount() == 1);   // idempotent
  run(a, b, 60);
  assert(a.status().find("auto=1 ready=1") != std::string::npos);
  assert(b.status().find("ready=1") != std::string::npos);
  mesh::Packet packet; packet.payload = {0x31, 1, 2};
  a.bridge.sendPacket(&packet); run(a, b, 3);
  assert(b.manager.received.size() == 1);
  mesh::Packet back; back.payload = {0x32, 1, 2};
  b.bridge.sendPacket(&back); run(a, b, 3);
  assert(a.manager.received.size() == 1);
  uint8_t lip[4]; uint16_t lport = 0;
  assert(a.bridge.localAddress(lip, lport) && lip[3] == 1 && lport == 4404);
  // unusable addresses, our own address: refused
  const uint8_t self_ip[4] = {10, 11, 12, 1}, bcast[4] = {10, 11, 12, 255}, loop_ip[4] = {127, 0, 0, 1};
  assert(a.bridge.addAutoPeer(self_ip, 4404) == -1 && a.bridge.addAutoPeer(bcast, 4404) == -1);
  assert(a.bridge.addAutoPeer(loop_ip, 4404) == -1 && a.bridge.addAutoPeer(bip, 0) == -1);
  // remove: gone, no session
  a.bridge.removeAutoPeer(bip, 4404);
  assert(a.bridge.autoPeerCount() == 0 && a.status().find("ready=0") != std::string::npos);
  // a restart forgets auto peers (the role re-adds live ones)
  assert(a.bridge.addAutoPeer(bip, 4404) == 0);
  a.bridge.end(); a.start();
  assert(a.bridge.autoPeerCount() == 0 && a.status().find("peers=0") != std::string::npos);
}
static void manual_wins() {
  Node a(1, 2), b(2, 1);
  a.start(); b.start();
  const uint8_t bip[4] = {10, 11, 12, 2};
  assert(a.bridge.addAutoPeer(bip, 4404) == -2 && a.bridge.autoPeerCount() == 0);
  // fill the free slots: the ninth address finds no room
  for (uint8_t i = 0; i < 7; ++i) { const uint8_t ip[4] = {10, 11, 12, uint8_t(20 + i)}; assert(a.bridge.addAutoPeer(ip, 4404) == i + 1); }
  const uint8_t ninth[4] = {10, 11, 12, 40};
  assert(a.bridge.addAutoPeer(ninth, 4404) == -1);
}
static void session_key() {
  // review F6: udp.secret on the repeater keys the UDP session instead of bridge.secret
  Node a(1, 2), b(2, 1);
  strcpy(mesh::discovery::udpSecretOverride(), "lan-key");
  a.start(); run(a, b, 1);
  strcpy(mesh::discovery::udpSecretOverride(), "");
  strcpy(b.prefs.bridge_secret, "lan-key"); b.start();
  // a and b run in one process: give each loop its own key
  for (unsigned i = 0; i < 60; ++i) {
    test_now += 100;
    strcpy(mesh::discovery::udpSecretOverride(), "lan-key"); a.bridge.loop();
    strcpy(mesh::discovery::udpSecretOverride(), ""); b.bridge.loop();
  }
  assert(a.status().find("ready=1") != std::string::npos && b.status().find("ready=1") != std::string::npos);
  // a key change ends the session at once
  strcpy(mesh::discovery::udpSecretOverride(), "other");
  a.bridge.serviceNetwork(true, address(1), 0x00ffffff);
  assert(a.status().find("ready=0") != std::string::npos);
  strcpy(mesh::discovery::udpSecretOverride(), "");
}
static void rx_limit() {
  Node a(1, 2), b(2, 1);
  a.start(); b.start(); run(a, b, 60);
  mesh::discovery::rxLimiter().configure(4);   // burst 1
  for (int i = 0; i < 5; ++i) { mesh::Packet p; p.payload = {uint8_t(0x40 + i), 1, 2}; b.bridge.sendPacket(&p); run(a, b, 2); }
  assert(a.manager.received.size() == 1);
  assert(mesh::discovery::rxLimiter().dropped() == 4);
  mesh::discovery::rxLimiter().configure(0);
  mesh::Packet p; p.payload = {0x50, 1, 2}; b.bridge.sendPacket(&p); run(a, b, 2);
  assert(a.manager.received.size() == 2);
  mesh::discovery::rxLimiter().configure(mesh::discovery::kRxLimitDefault);
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string name = argv[1];
  if (name == "discovery") discovery_runtime();
  else if (name == "manual") manual_wins();
  else if (name == "rxlimit") rx_limit();
  else if (name == "sessionkey") session_key();
  else return 2;
  printf("%s passed\n", argv[1]);
  return 0;
}
'''


ESPNOW_CHECKS = r'''
#include <helpers/bridges/DiscoveryFrame.h>
static std::vector<std::vector<uint8_t>> got;
static void sink(void*, const uint8_t* f, size_t n, const uint8_t* ip, uint16_t) { assert(ip == nullptr); got.emplace_back(f, f + n); }
int main(int argc, char** argv) {
  assert(argc == 2);
  const bool wrapped = argv[1][0] == 'w';
  Backend backend; backend.wifi = true; mesh::wireless::control().begin(backend);
  sdk_initialized = sdk_started = true; sdk_mode = WIFI_STA;
  NodePrefs prefs; mesh::PacketManager mgr; mesh::RTCClock rtc;
  prefs.bridge_format = wrapped ? mesh::bridge::ESPNOW_FORMAT_WRAPPED : mesh::bridge::ESPNOW_FORMAT_RAW;
  ESPNowBridge bridge(&prefs, &mgr, &rtc); bridge.begin(); assert(bridge._initialized);
  bridge.setDiscoverySink(sink, nullptr);
  uint8_t ann[mesh::discovery::ANNOUNCE_FRAME] = {0xB0, 0x42, 0x44, 0x43, 1, 1, 9};
  assert(!bridge.sendAuxFrame(ann, sizeof(ann) - 1));
  // mesh first: an announce waits for the queued packet
  auto p = packet(0x11); bridge.sendPacket(&p);
  assert(bridge.sendAuxFrame(ann, sizeof(ann)));
  bridge.loop(); assert(sent.size() == 1 && sent[0][0] == (wrapped ? 0xC0 : 0x11));
  complete(bridge);
  assert(sent.size() == 2 && bridge._aux_in_flight);
  if (wrapped) {
    uint8_t plain[250]; size_t n = 0;
    assert(mesh::bridge::unwrapFrame(sent[1].data(), sent[1].size(), prefs.bridge_secret, sizeof(prefs.bridge_secret),
                                     plain, sizeof(plain), n) == mesh::bridge::Unwrap::Ok);
    assert(n == sizeof(ann) && !memcmp(plain, ann, n));
  } else {
    assert(sent[1] == std::vector<uint8_t>(ann, ann + sizeof(ann)));
  }
  complete(bridge);
  assert(bridge.auxSent() == 1 && !bridge._aux_in_flight && bridge._observer.stats().sent_ok == 0);
  assert(bridge._stats.tx.callback_ok == 1);   // only the mesh frame counts as mesh
  // a newer announce replaces one not yet sent; end() discards it
  bridge.sendAuxFrame(ann, sizeof(ann)); ann[6] = 10; bridge.sendAuxFrame(ann, sizeof(ann));
  bridge.loop(); assert(sent.size() == 3); complete(bridge);
  bridge.sendAuxFrame(ann, sizeof(ann)); bridge.end(); bridge.begin(); bridge.loop(); assert(sent.size() == 3);
  // received discovery frames go to the sink, never into the mesh
  const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  uint8_t reply[mesh::discovery::REPLY_FRAME] = {0xB0, 0x42, 0x44, 0x43, 1, 2};
  uint8_t frame[250]; size_t flen = sizeof(reply);
  if (wrapped) flen = mesh::bridge::wrapFrame(reply, sizeof(reply), prefs.bridge_secret, sizeof(prefs.bridge_secret), frame, sizeof(frame));
  else memcpy(frame, reply, sizeof(reply));
  bridge.queueReceivedFrame(mac, frame, (int)flen); bridge.loop();
  assert(got.size() == 1 && got[0].size() == sizeof(reply) && mgr.received.empty() && bridge.discoveryReceived() == 1);
  // a raw discovery frame on a wrapped link is foreign (strict)
  if (wrapped) { bridge.queueReceivedFrame(mac, reply, sizeof(reply)); bridge.loop(); assert(got.size() == 1); }
  // review F1: duplicates (repeat copies, relay echoes) never spend the
  // budget: one fresh packet, five copies of it, then a fresh one still passes.
  mesh::discovery::rxLimiter().configure(10);   // burst 2
  {
    const size_t before = mgr.received.size();
    auto send = [&](uint8_t id) {
      auto q = packet(id); uint8_t buf[250]; size_t len = q.length;
      if (wrapped) len = mesh::bridge::wrapFrame(q.data, q.length, prefs.bridge_secret, sizeof(prefs.bridge_secret), buf, sizeof(buf));
      else memcpy(buf, q.data, q.length);
      bridge.queueReceivedFrame(mac, buf, (int)len); bridge.loop();
    };
    send(0x61); for (int i = 0; i < 5; ++i) send(0x61);
    send(0x62);
    assert(mgr.received.size() == before + 2 && mesh::discovery::rxLimiter().dropped() == 0);
    send(0x63);   // the third new packet within the burst is dropped
    assert(mgr.received.size() == before + 2 && mesh::discovery::rxLimiter().dropped() == 1);
  }
  // bridge.rxlimit: packets from ESP-NOW share the budget
  mesh::discovery::rxLimiter().configure(0);
  mesh::discovery::rxLimiter().resetCounters();
  mesh::discovery::rxLimiter().configure(4);
  for (int i = 0; i < 3; ++i) {
    auto q = packet(uint8_t(0x20 + i)); uint8_t buf[250]; size_t len = q.length;
    if (wrapped) len = mesh::bridge::wrapFrame(q.data, q.length, prefs.bridge_secret, sizeof(prefs.bridge_secret), buf, sizeof(buf));
    else memcpy(buf, q.data, q.length);
    bridge.queueReceivedFrame(mac, buf, (int)len); bridge.loop();
  }
  assert(mgr.received.size() == 3 && mesh::discovery::rxLimiter().dropped() == 2 && mgr.allocs == mgr.frees);
  bridge.end();
  return 0;
}
'''


KEY_CHECKS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <helpers/bridges/DiscoveryKey.h>
using namespace mesh::discovery;
int main() {
  uint8_t key[KEY_SIZE];
  assert(deriveKey("fleet-test", 10, key));
  printf("KEY ");
  for (uint8_t b : key) printf("%02x", b);
  printf("\n");
  KeyCache cache;
  const uint8_t* k1 = cache.get("fleet-test");
  assert(k1 && !memcmp(k1, key, KEY_SIZE) && cache.derivations() == 1);
  assert(cache.get("fleet-test") == k1 && cache.derivations() == 1);       // cached, not per frame
  assert(cache.get("other") && cache.derivations() == 2);
  assert(!cache.get("") && !cache.get(nullptr) && !cache.get("0123456789abcdef"));
  Announce a; a.boot = 1; a.counter = 2; a.caps = CAP_UDP; a.port = 4404; a.ip[0] = 10; a.ip[3] = 5;
  uint8_t f[MAX_FRAME];
  const size_t n = encodeAnnounce(a, keyedMac, key, f, sizeof(f));
  assert(n == ANNOUNCE_FRAME);
  printf("FRAME ");
  for (size_t i = 0; i < n; ++i) printf("%02x", f[i]);
  printf("\n");
  Announce b; assert(decodeAnnounce(f, n, keyedMac, key, b) && b.counter == 2);
  uint8_t wrong[KEY_SIZE]; deriveKey("fleet-tesu", 10, wrong);
  assert(!decodeAnnounce(f, n, keyedMac, wrong, b));
  return 0;
}
'''


class DiscoveryKeyTests(unittest.TestCase):
    def test_pbkdf2_key_and_tag_match_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / "t.cpp").write_text(KEY_CHECKS)
            from test_espnow_repeat import compiler, sanitizer_flags
            built = subprocess.run([compiler(), "-std=c++11", "-O1", "-g", "-Wall", "-Wextra", *sanitizer_flags(),
                                    "-I", str(ROOT / "test/mocks"), "-I", str(ROOT / "src"),
                                    str(folder / "t.cpp"), "-o", str(folder / "t")],
                                   capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([str(folder / "t")], capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        key = hashlib.pbkdf2_hmac("sha256", b"fleet-test", b"MeshCore discovery key v1", 2048, 32)
        self.assertEqual(run.stdout.split("KEY ", 1)[1].split()[0], key.hex())
        frame = bytes.fromhex(run.stdout.split("FRAME ", 1)[1].split()[0])
        self.assertEqual(frame[-8:], tag(key, frame[:-8]))
        self.assertNotEqual(frame[-8:], tag(b"fleet-test", frame[:-8]))   # never the raw secret


class DiscoveryCodecTests(unittest.TestCase):
    def test_codec_table_and_limiter(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / "t.cpp").write_text(CODEC_CHECKS)
            from test_espnow_repeat import compiler, sanitizer_flags
            built = subprocess.run([compiler(), "-std=c++11", "-O1", "-g", "-Wall", "-Wextra", *sanitizer_flags(),
                                    "-I", str(ROOT / "test/mocks"), "-I", str(ROOT / "src"),
                                    str(folder / "t.cpp"), "-o", str(folder / "t")],
                                   capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([str(folder / "t")], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        frame = bytes.fromhex(run.stdout.split("ANNOUNCE ", 1)[1].split()[0])
        self.assertEqual(len(frame), 53)
        self.assertEqual(frame[:6], bytes([0xB0, 0x42, 0x44, 0x43, 1, 1]))
        # The tag is HMAC-SHA256 keyed with the secret (checked with Python's hmac).
        self.assertEqual(frame[-8:], tag(b"fleet-test", frame[:-8]))
        self.assertEqual(frame[6 + 4 + 4 + 6:6 + 4 + 4 + 6 + 16], b"CaptCallahan-Bas")
        self.assertNotIn(b"fleet-test", frame)


class DiscoverySettingsTests(unittest.TestCase):
    def test_cli_and_saved_record(self):
        with tempfile.TemporaryDirectory(prefix="bridge-discovery-") as tmp:
            folder = settings_folder(tmp)
            (folder / "BridgeDiscoverySettings.h").write_bytes((ROOT / "src/helpers/BridgeDiscoverySettings.h").read_bytes())
            (folder / "settings_under_test.cpp").write_bytes((ROOT / "src/helpers/BridgeDiscoverySettings.cpp").read_bytes())
            build_and_run(self, SETTINGS_CHECKS, includes=[folder, ROOT / "src/helpers", ROOT / "src"],
                          defines=["ESP32_PLATFORM=1", "WITH_ESPNOW_BRIDGE=1", "WITH_UDP_BRIDGE=1"], strict=False)

    def test_without_the_udp_bridge_only_rxlimit_exists(self):
        # review F3: no announce, autopeers or udp.secret in ESP-NOW-only builds
        with tempfile.TemporaryDirectory(prefix="bridge-discovery-") as tmp:
            folder = settings_folder(tmp)
            (folder / "BridgeDiscoverySettings.h").write_bytes((ROOT / "src/helpers/BridgeDiscoverySettings.h").read_bytes())
            (folder / "settings_under_test.cpp").write_bytes((ROOT / "src/helpers/BridgeDiscoverySettings.cpp").read_bytes())
            build_and_run(self, ESPNOW_ONLY_SETTINGS, includes=[folder, ROOT / "src/helpers", ROOT / "src"],
                          defines=["ESP32_PLATFORM=1", "WITH_ESPNOW_BRIDGE=1"], strict=False)


class UdpDiscoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        cls.tmp = tempfile.TemporaryDirectory(prefix="udp-discovery-")
        folder = Path(cls.tmp.name)
        fixtures = ROOT / "test/fixtures/udp_bridge"
        runtime = (ROOT / "test/fixtures/udp_bridge_runtime.cpp").read_text()
        driver = runtime[:runtime.index("static void codec()")] + UDP_CHECKS
        (folder / "driver.cpp").write_text(driver)
        flags = [compiler, "-Wall", "-Wextra", "-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-fno-pie", "-no-pie", "-DARDUINO=1", "-DESP32=1", "-DWITH_UDP_BRIDGE=1",
                 "-DMESH_ESP32_USB_CONSOLE_COOPERATIVE=0",
                 "-I" + str(fixtures), "-I" + str(ROOT / "test/mocks"), "-I" + str(ROOT / "src")]
        core = folder / "core.o"
        for command in (flags + ["-std=c++11", "-pedantic-errors", "-Werror", "-c",
                                 str(ROOT / "src/helpers/bridges/UDPBridge.cpp"), "-o", str(core)],
                        flags + ["-std=c++17", str(folder / "driver.cpp"), str(core), "-o", str(folder / "t")]):
            built = subprocess.run(command, capture_output=True, text=True, timeout=120)
            if built.returncode:
                cls.tmp.cleanup()
                raise AssertionError(built.stdout + built.stderr)
        cls.binary = folder / "t"

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def check(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(name + " passed", result.stdout)

    def test_discovery_frames_broadcast_and_auto_peer_session(self):
        self.check("discovery")

    def test_manual_peers_win_and_slots_are_bounded(self):
        self.check("manual")

    def test_rx_limit_drops_udp_floods(self):
        self.check("rxlimit")

    def test_udp_secret_keys_the_session(self):
        self.check("sessionkey")


class EspNowDiscoveryTests(unittest.TestCase):
    def test_announce_idle_link_and_replies_to_the_role(self):
        from test_bridge_recovery import BridgeRecoveryTests, code, COMMON
        import os
        for mode in ("raw", "wrapped"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                cpp, exe = Path(tmp) / "t.cpp", Path(tmp) / "t"
                cpp.write_text(code() + COMMON + ESPNOW_CHECKS)
                built = subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-O1", "-Wall", "-Wextra",
                                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
                                        "-DESP32=1", "-DWITH_ESPNOW_BRIDGE=1", "-DWITH_RS232_BRIDGE=1",
                                        "-DESP_IDF_VERSION=50200", "-DESP_IDF_VERSION_MAJOR=5",
                                        "-I", str(ROOT / "src"), str(cpp), "-o", str(exe)],
                                       capture_output=True, text=True, timeout=300)
                self.assertEqual(built.returncode, 0, built.stderr)
                run = subprocess.run([str(exe), mode], capture_output=True, text=True, timeout=60)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


RADIO_CHECKS = r"""
std::vector<std::vector<uint8_t>> air_log;
static std::vector<std::vector<uint8_t>> heard;
static void on_discovery(const uint8_t* f, size_t n, void*) { heard.emplace_back(f, f + n); }
static void deliver(const uint8_t* bytes, int len) {
  static uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  esp_now_recv_info_t info{mac}; OnDataRecv(&info, bytes, len);
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const bool wrapped = argv[1][0] == 'w';
  if (wrapped) assert(mesh::espnow::activateLink(true, "fleet-test"));
  ESPNOWRadio radio; radio.init(); assert(radio.isEnabled());
  uint8_t ann[mesh::discovery::ANNOUNCE_FRAME] = {0xB0, 0x42, 0x44, 0x43, 1, 1, 7};
  uint8_t frame[250]; size_t n = sizeof(ann);
  if (wrapped) n = mesh::bridge::wrapFrame(ann, sizeof(ann), "fleet-test", 16, frame, sizeof(frame));
  else memcpy(frame, ann, sizeof(ann));
  uint8_t out[255];
  // no handler: the frame takes the old path (MeshCore rejects it later)
  deliver(frame, (int)n); assert(radio.recvRaw(out, sizeof(out)) == (int)sizeof(ann));
  ESPNOWRadio::setDiscoveryHandler(on_discovery, nullptr);
  deliver(frame, (int)n);
  assert(heard.size() == 1 && heard[0] == std::vector<uint8_t>(ann, ann + sizeof(ann)));
  assert(radio.recvRaw(out, sizeof(out)) == 0);
  // a wrapped head ignores a raw announce (strict format)
  if (wrapped) { deliver(ann, sizeof(ann)); assert(heard.size() == 1); }
  ESPNOWRadio::setDiscoveryHandler(nullptr, nullptr);
  radio.end();
  return 0;
}
"""


class RadioDiscoveryTests(unittest.TestCase):
    def test_primary_radio_hands_announces_to_the_head(self):
        from test_espnow_wrapped_primary import radio_source
        from test_espnow_repeat import compiler, sanitizer_flags
        for mode in ("raw", "wrapped"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                cpp, exe = Path(tmp) / "t.cpp", Path(tmp) / "t"
                cpp.write_text(radio_source(RADIO_CHECKS))
                built = subprocess.run([compiler(), "-std=c++17", "-O1", "-g", *sanitizer_flags(),
                                        "-DMESH_PRIMARY_ESPNOW=1", "-DMESH_ESPNOW_RADIO=1",
                                        "-DESP_ARDUINO_VERSION_MAJOR=3", "-DESP_IDF_VERSION=50200",
                                        "-I", str(ROOT / "src"), str(cpp), "-o", str(exe)],
                                       capture_output=True, text=True, timeout=300)
                self.assertEqual(built.returncode, 0, built.stderr)
                run = subprocess.run([str(exe), mode], capture_output=True, text=True, timeout=60)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


class RepeaterWiringTests(unittest.TestCase):
    def test_repeater_announces_and_pairs_only_after_the_tag_verifies(self):
        source = (ROOT / "examples/simple_repeater/MyMesh.cpp").read_text()
        handler = source[source.index("void MyMesh::onDiscoveryFrame("):source.index("void MyMesh::serviceDiscovery()")]
        verify = handler.index("decodeReply(")
        fresh = handler.index("freshReply(")
        spoof = handler.index("memcmp(ip, reply.ip, 4) || port != reply.port")   # review F2
        manual = handler.index("equalPeer(manual, wanted)")
        self.assertLess(fresh, spoof)
        self.assertLess(spoof, manual)
        self.assertNotIn("memcpy(reply.ip, ip, 4)", handler)
        self.assertIn("keyedMac", handler)
        offer = handler.index("d.peers.offer(")
        add = handler.index("addAutoPeer(")
        self.assertLess(verify, fresh)
        self.assertLess(fresh, manual)
        self.assertLess(manual, offer)
        self.assertLess(offer, add)
        loop = source[source.index("serviceUDPBridge();\n#endif\n#if defined(MESH_BRIDGE_DISCOVERY)"):]
        self.assertIn("serviceDiscovery();", loop[:200])
        service = source[source.index("void MyMesh::serviceDiscovery()"):source.index("void MyMesh::describeAutoPeers(")]
        self.assertIn("bridgeAnnounceActive(udp_on)", service)
        self.assertIn("discoveryKey()", service)
        self.assertIn("#if defined(MESH_BRIDGE_DISCOVERY)\n  serviceDiscovery();", source)
        self.assertNotIn("#if defined(MESH_BRIDGE_DISCOVERY_SETTINGS)\n  serviceDiscovery();", source)
        self.assertIn("sendAuxFrame(frame, n)", service)
        self.assertIn("sendDiscovery(frame, n)", service)
        cli = (ROOT / "src/helpers/CommonCLI.cpp").read_text()
        self.assertIn("handleBridgeDiscoveryCommand(", cli)
        # udp.secret is answered before Cascade's udp.* parser refuses it
        self.assertLess(cli.index("handleBridgeDiscoveryCommand("), cli.index("handleUDPBridgeCommand(sender_timestamp"))
        self.assertIn("loadBridgeDiscoverySettings(", cli)


if __name__ == "__main__":
    unittest.main()
