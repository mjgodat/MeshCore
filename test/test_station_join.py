"""Non-blocking station join that never leaves the ESP-NOW channel (StationJoin.h).

Simulated clock and radio: every driver call is charged its real-world cost
(an asynchronous scan start or WiFi.begin's async start take a few ms; a
synchronous scan would take 300 ms and does not exist in the driver), and the
test asserts that no single poll() of the mesh loop costs more than 5 ms.
"""
from pathlib import Path
import unittest

from test_espnow_repeat import build_and_run
from test_radio_receive_contract import method

ROOT = Path(__file__).resolve().parents[1]

CHECKS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <helpers/StationJoin.h>
using namespace mesh::wifi;

struct Ap { std::string ssid; uint8_t channel; int rssi; uint8_t bssid; };

struct Sim {
  // world
  std::vector<Ap> aps;
  uint8_t espnow = 6;            // 0 = unconstrained
  bool allowed = true;
  // radio
  uint8_t radio_channel = 6;
  bool scanning = false; uint8_t scan_channel = 0; uint32_t scan_done_at = 0; bool scan_fails = false;
  std::vector<Ap> results; bool have_results = false;
  bool joining = false; uint32_t join_at = 0; uint8_t join_channel = 0; uint8_t join_bssid = 0;
  bool linked = false; uint8_t link_channel = 0; uint8_t link_bssid = 0; bool join_never = false;
  // accounting
  uint32_t now = 0; double cost = 0; int begins = 0, scans = 0, all_channel_scans = 0, disconnects = 0;
  uint32_t off_channel_ms = 0;   // time ESP-NOW spent off its channel

  // ---- driver ----
  uint8_t espNowChannel() { cost += 0.05; return espnow; }
  bool mutationAllowed() { cost += 0.05; return allowed; }
  bool startScan(uint8_t ch, const char*) {
    cost += 2.0; ++scans; if (!ch) ++all_channel_scans;
    if (scanning) return false;
    scanning = true; scan_channel = ch; have_results = false;
    scan_done_at = now + (ch ? 300 : 1500);
    return true;
  }
  int scanResult() {
    cost += 0.05;
    if (scanning && (int32_t)(now - scan_done_at) < 0) return -1;
    if (scanning) {
      scanning = false; results.clear();
      if (scan_fails) { have_results = false; return -2; }
      for (auto& a : aps) if (!scan_channel || a.channel == scan_channel) results.push_back(a);
      have_results = true;
    }
    return have_results ? (int)results.size() : -2;
  }
  bool pick(const char* ssid, uint8_t ch, uint8_t bssid[6], uint8_t& strongest) {
    cost += 0.5;
    int best = -1, any = -1;
    for (size_t i = 0; i < results.size(); ++i) {
      if (results[i].ssid != ssid) continue;
      if (any < 0 || results[i].rssi > results[(size_t)any].rssi) any = (int)i;
      if (ch && results[i].channel != ch) continue;
      if (best < 0 || results[i].rssi > results[(size_t)best].rssi) best = (int)i;
    }
    strongest = any >= 0 ? results[(size_t)any].channel : 0;
    if (best < 0) return false;
    memset(bssid, 0, 6); bssid[5] = results[(size_t)best].bssid;
    return true;
  }
  void scanDelete() { cost += 0.1; results.clear(); have_results = false; scanning = false; }
  bool begin(const char* ssid, const char*, uint8_t ch, const uint8_t* bssid) {
    cost += 3.0; ++begins;
    // the station only ever targets the ESP-NOW channel when constrained
    if (espnow) assert(ch == espnow && bssid != nullptr);
    joining = true; join_at = now + 2000; join_channel = ch; join_bssid = bssid ? bssid[5] : 0;
    (void)ssid;
    return true;
  }
  bool linkUp() { cost += 0.05; return linked; }
  uint8_t linkChannel() { cost += 0.05; return linked ? link_channel : 0; }
  void disconnect() { cost += 1.0; ++disconnects; linked = false; joining = false; }
  void restoreChannel() { cost += 0.2; if (espnow) radio_channel = espnow; }

  // ---- world step (1 ms resolution where it matters) ----
  void world() {
    if (joining && (int32_t)(now - join_at) >= 0 && !join_never) {
      joining = false;
      for (auto& a : aps) if ((!join_channel || a.channel == join_channel) && (!join_bssid || a.bssid == join_bssid)) {
        linked = true; link_channel = a.channel; link_bssid = a.bssid; radio_channel = a.channel; break;
      }
    }
    if (linked) {   // the AP can move: the associated radio follows it (CSA)
      bool still = false;
      for (auto& a : aps) if (a.bssid == link_bssid) { still = true; link_channel = a.channel; radio_channel = a.channel; }
      if (!still) linked = false;
    }
    if (espnow && radio_channel != espnow) ++off_channel_ms;
  }
};

static double worst = 0;
static void run(Sim& s, StationJoin<Sim>& j, uint32_t ms, uint32_t step = 10) {
  for (uint32_t t = 0; t < ms; t += step) {
    for (uint32_t k = 0; k < step; ++k) { s.now++; s.world(); }
    s.cost = 0;
    j.poll(s, s.now, "Home", "password1");
    if (s.cost > worst) worst = s.cost;
    assert(s.cost <= 5.0);   // the mesh loop is never held for more than 5 ms
  }
}

int main() {
  // 1) AP on the ESP-NOW channel: joins via async scan, pinned BSSID
  {
    Sim s; s.aps = {{"Home", 6, -50, 1}, {"Other", 6, -40, 2}}; StationJoin<Sim> j; j.reset(0);
    run(s, j, 5000);
    assert(j.connected() && s.linked && s.link_channel == 6 && s.begins == 1 && s.scans == 1);
    assert(j.reason() == JoinReason::None && j.apChannel() == 6);
    run(s, j, 60000);
    assert(j.connected() && s.begins == 1 && s.off_channel_ms == 0);
  }
  // 2) AP absent: back-off 15, 30, 60, 120, 120 s; never joins, never blocks
  {
    Sim s; StationJoin<Sim> j; j.reset(0);
    std::vector<uint32_t> starts; int last = 0;
    for (int i = 0; i < 14000; ++i) {
      run(s, j, 50, 50);
      if (s.scans != last) { starts.push_back(s.now); last = s.scans; }
    }
    assert(s.begins == 0 && j.reason() == JoinReason::NotOnChannel);
    assert(starts.size() >= 6);
    const uint32_t expect[] = {15000, 30000, 60000, 120000, 120000};
    for (int i = 0; i < 5; ++i) {
      const uint32_t gap = starts[(size_t)i + 1] - starts[(size_t)i];
      assert(gap >= expect[i] + 300 && gap <= expect[i] + 400);   // + the 300 ms scan, 50 ms polls
    }
  }
  // 3) AP only on another channel: never joined, ESP-NOW never leaves ch 6;
  //    repeaters (no diagnosis) never scan other channels
  {
    Sim s; s.aps = {{"Home", 11, -40, 1}}; StationJoin<Sim> j; j.reset(0);
    run(s, j, 400000, 20);
    assert(!j.connected() && s.begins == 0 && s.all_channel_scans == 0 && s.off_channel_ms == 0);
    assert(j.reason() == JoinReason::NotOnChannel && j.apChannel() == 0);
  }
  // 4) heads: diagnosis finds the AP's channel, at most once per interval
  {
    Sim s; s.aps = {{"Home", 11, -40, 1}}; StationJoin<Sim> j(600000); j.reset(0);
    run(s, j, 30000, 20);
    assert(s.all_channel_scans == 1 && j.apChannel() == 11 && !j.connected());
    run(s, j, 500000, 20);
    assert(s.all_channel_scans == 1);
    run(s, j, 200000, 20);
    assert(s.all_channel_scans == 2);
  }
  // 5) F1: joined on ch 6, router moves to ch 11: dropped at once, reported,
  //    rejoined only when the AP is back on ch 6
  {
    Sim s; s.aps = {{"Home", 6, -50, 1}}; StationJoin<Sim> j; j.reset(0);
    run(s, j, 5000);
    assert(j.connected());
    s.aps[0].channel = 11;
    run(s, j, 50, 10);
    assert(!s.linked && !j.connected() && j.reason() == JoinReason::ApMoved && j.apChannel() == 11);
    assert(s.off_channel_ms <= 10 && s.radio_channel == 6);   // one poll at most
    const int begins = s.begins;
    run(s, j, 300000, 20);
    assert(s.begins == begins && !s.linked && s.off_channel_ms <= 10);
    s.aps[0].channel = 6;
    run(s, j, 130000, 20);
    assert(j.connected() && s.linked && s.link_channel == 6 && j.reason() == JoinReason::None);
    assert(s.off_channel_ms <= 10);
  }
  // 6) association made by another owner on the wrong channel is dropped too
  {
    Sim s; s.aps = {{"Home", 11, -50, 1}}; StationJoin<Sim> j; j.reset(0);
    s.linked = true; s.link_channel = 11; s.link_bssid = 1; s.radio_channel = 11;
    run(s, j, 20, 10);
    assert(!s.linked && s.radio_channel == 6 && j.reason() == JoinReason::ApMoved);
  }
  // 7) link lost: retry after 15 s, back-off reset
  {
    Sim s; s.aps = {{"Home", 6, -50, 1}}; StationJoin<Sim> j; j.reset(0);
    run(s, j, 5000);
    assert(j.connected());
    s.aps.clear(); run(s, j, 100);
    assert(!j.connected() && j.backoffMs() == StationJoin<Sim>::kFirstBackoffMs);
    s.aps = {{"Home", 6, -50, 1}};
    run(s, j, 20000);
    assert(j.connected());
  }
  // 8) join timeout, scan failure, busy owner, no credentials
  {
    Sim s; s.aps = {{"Home", 6, -50, 1}}; s.join_never = true; StationJoin<Sim> j; j.reset(0);
    run(s, j, 23000);
    assert(!j.connected() && j.reason() == JoinReason::JoinTimeout && s.disconnects >= 1);
    Sim f; f.scan_fails = true; StationJoin<Sim> k; k.reset(0);
    run(f, k, 1000); assert(k.reason() == JoinReason::ScanFailed);
    Sim b; b.allowed = false; StationJoin<Sim> m; m.reset(0);
    run(b, m, 5000); assert(m.reason() == JoinReason::Busy && b.scans == 0);
    Sim c; StationJoin<Sim> n; n.reset(0);
    c.cost = 0; n.poll(c, 1, "", ""); assert(n.reason() == JoinReason::NoCredentials && c.scans == 0);
    c.cost = 0; n.poll(c, 2, nullptr, nullptr); assert(c.scans == 0);
  }
  // 9) no ESP-NOW to protect: plain async begin, no scan
  {
    Sim s; s.espnow = 0; s.aps = {{"Home", 11, -50, 1}}; StationJoin<Sim> j; j.reset(0);
    run(s, j, 5000);
    assert(j.connected() && s.scans == 0 && s.begins == 1 && s.link_channel == 11);
  }
  // 10) needsCredentials only when a join attempt is due; stop() drops our link
  {
    Sim s; s.aps = {{"Home", 6, -50, 1}}; StationJoin<Sim> j; j.reset(0);
    assert(j.needsCredentials(0));
    run(s, j, 5000);
    assert(!j.needsCredentials(s.now));
    j.stop(s); assert(!s.linked && !j.connected());
    // millis() wrap
    Sim w; w.aps = {{"Home", 6, -50, 1}}; StationJoin<Sim> x; w.now = 0xFFFFF000u; x.reset(w.now);
    run(w, x, 10000); assert(x.connected());
  }
  std::printf("worst poll %.2f ms\n", worst);
  return 0;
}
'''


class StationJoinTests(unittest.TestCase):
    def test_never_blocks_never_leaves_the_espnow_channel(self):
        build_and_run(self, CHECKS)


class StationJoinWiringTests(unittest.TestCase):
    def test_repeater_uses_the_async_join_and_checks_a_linked_station(self):
        repeater = (ROOT / "examples/simple_repeater/MyMesh.cpp").read_text()
        body = method(repeater, "void MyMesh::serviceUDPBridge()")
        self.assertNotIn("beginStation(", body)
        self.assertNotIn("scanNetworks(", body)
        self.assertIn("udp_join.poll(udp_join_driver, now, udp_station_ssid, udp_station_password);", body)
        self.assertIn("const bool station_ready = linked && udp_join.connected();", body)

    def test_driver_scans_asynchronously(self):
        driver = (ROOT / "src/helpers/esp32/StationJoinDriver.h").read_text()
        self.assertIn("WiFi.scanNetworks(true,", driver)
        self.assertNotIn("WiFi.scanNetworks(false", driver)
        self.assertNotIn("beginStation(", driver)


if __name__ == "__main__":
    unittest.main()
