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
  uint32_t scan_ms = 300;        // a stuck driver: longer than the 5 s timeout
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
    scan_done_at = now + (ch ? scan_ms : 1500);
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
  void scanDelete() {
    cost += 0.1;
    assert(!(scanning && (int32_t)(now - scan_done_at) < 0));   // never delete a running scan
    results.clear(); have_results = false; scanning = false;
  }
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
    Sim s; s.aps = {{"Home", 6, -50, 1}, {"Other", 6, -40, 2}}; StationJoin<Sim> j; j.reset(s, 0);
    run(s, j, 5000);
    assert(j.connected() && s.linked && s.link_channel == 6 && s.begins == 1 && s.scans == 1);
    assert(j.reason() == JoinReason::None && j.apChannel() == 6);
    run(s, j, 60000);
    assert(j.connected() && s.begins == 1 && s.off_channel_ms == 0);
  }
  // 2) AP absent: back-off 15, 30, 60, 120, 120 s; never joins, never blocks
  {
    Sim s; StationJoin<Sim> j; j.reset(s, 0);
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
    Sim s; s.aps = {{"Home", 11, -40, 1}}; StationJoin<Sim> j; j.reset(s, 0);
    run(s, j, 400000, 20);
    assert(!j.connected() && s.begins == 0 && s.all_channel_scans == 0 && s.off_channel_ms == 0);
    assert(j.reason() == JoinReason::NotOnChannel && j.apChannel() == 0);
  }
  // 4) heads: diagnosis finds the AP's channel, at most once per interval
  {
    Sim s; s.aps = {{"Home", 11, -40, 1}}; StationJoin<Sim> j(600000); j.reset(s, 0);
    run(s, j, 30000, 20);
    assert(s.all_channel_scans == 1 && j.apChannel() == 11 && !j.connected());
    run(s, j, 500000, 20);
    assert(s.all_channel_scans == 1);
    run(s, j, 200000, 20);
    assert(s.all_channel_scans == 2);
  }
  // 4b) the diagnosis only runs while its owner allows it (ESP-NOW silent):
  //     with ESP-NOW healthy the head never leaves its channel
  {
    Sim s; s.aps = {{"Home", 11, -40, 1}}; StationJoin<Sim> j(600000); j.reset(s, 0);
    j.setDiagnoseAllowed(false);
    run(s, j, 1300000, 20);
    assert(s.all_channel_scans == 0 && s.off_channel_ms == 0 && j.reason() == JoinReason::NotOnChannel);
    j.setDiagnoseAllowed(true);
    run(s, j, 130000, 20);
    assert(s.all_channel_scans == 1 && j.apChannel() == 11);
  }
  // 5) F1: joined on ch 6, router moves to ch 11: dropped at once, reported,
  //    rejoined only when the AP is back on ch 6
  {
    Sim s; s.aps = {{"Home", 6, -50, 1}}; StationJoin<Sim> j; j.reset(s, 0);
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
  // 6) an association made by another owner (MQTT, WebConfig) is never
  //    dropped; off the ESP-NOW channel it is reported and not used
  {
    Sim s; s.aps = {{"Home", 11, -50, 1}}; StationJoin<Sim> j; j.reset(s, 0);
    s.linked = true; s.link_channel = 11; s.link_bssid = 1; s.radio_channel = 11;
    run(s, j, 5000, 10);
    assert(s.linked && s.disconnects == 0 && !j.connected() && !j.owned());
    assert(j.reason() == JoinReason::ApMoved && j.apChannel() == 11 && s.begins == 0);
    // on the ESP-NOW channel it is used, still not owned, and not dropped when it moves
    Sim t; t.aps = {{"Home", 6, -50, 1}}; StationJoin<Sim> k; k.reset(t, 0);
    t.linked = true; t.link_channel = 6; t.link_bssid = 1;
    run(t, k, 50, 10);
    assert(k.connected() && !k.owned());
    t.aps[0].channel = 11; run(t, k, 50, 10);
    assert(t.linked && t.disconnects == 0 && !k.connected() && k.reason() == JoinReason::ApMoved);
    // stop() (a head switching UDP off) drops any association, even in Idle
    k.stop(t); assert(!t.linked && t.disconnects == 1 && t.radio_channel == 6);
  }
  // 11) reset() and stop() during a scan, and the scan timeout, never delete a
  //     running scan (the Sim asserts it); no new scan starts before it ends
  {
    Sim s; s.aps = {{"Home", 6, -50, 1}}; StationJoin<Sim> j; j.reset(s, 0);
    run(s, j, 100);
    assert(j.state() == JoinState::Scanning && s.scans == 1);
    j.reset(s, s.now);                       // settings changed mid-scan
    run(s, j, 150);                          // old scan still running: no new one
    assert(s.scans == 1 && j.state() == JoinState::Idle);
    run(s, j, 5000);                         // old one ends, dropped, fresh scan joins
    assert(s.scans == 2 && j.connected() && j.owned());
    Sim t; t.scan_ms = 8000; t.aps = {{"Home", 6, -50, 1}}; StationJoin<Sim> k; k.reset(t, 0);
    run(t, k, 5100);                         // 5 s timeout while the scan still runs
    assert(k.reason() == JoinReason::ScanFailed && t.scanning);
    run(t, k, 3500);                         // it ends at 8 s: dropped, no crash
    assert(!t.have_results);
    Sim u; u.aps = {{"Home", 6, -50, 1}}; StationJoin<Sim> m; m.reset(u, 0);
    run(u, m, 100); m.stop(u);   // the running scan is left to finish
    assert(!u.linked && u.begins == 0 && u.scanning && m.state() == JoinState::Idle);
  }
  // 7) link lost: retry after 15 s, back-off reset
  {
    Sim s; s.aps = {{"Home", 6, -50, 1}}; StationJoin<Sim> j; j.reset(s, 0);
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
    Sim s; s.aps = {{"Home", 6, -50, 1}}; s.join_never = true; StationJoin<Sim> j; j.reset(s, 0);
    run(s, j, 23000);
    assert(!j.connected() && j.reason() == JoinReason::JoinTimeout && s.disconnects >= 1);
    Sim f; f.scan_fails = true; StationJoin<Sim> k; k.reset(f, 0);
    run(f, k, 1000); assert(k.reason() == JoinReason::ScanFailed);
    Sim b; b.allowed = false; StationJoin<Sim> m; m.reset(b, 0);
    run(b, m, 5000); assert(m.reason() == JoinReason::Busy && b.scans == 0);
    Sim c; StationJoin<Sim> n; n.reset(c, 0);
    c.cost = 0; n.poll(c, 1, "", ""); assert(n.reason() == JoinReason::NoCredentials && c.scans == 0);
    c.cost = 0; n.poll(c, 2, nullptr, nullptr); assert(c.scans == 0);
  }
  // 9) no ESP-NOW to protect: plain async begin, no scan
  {
    Sim s; s.espnow = 0; s.aps = {{"Home", 11, -50, 1}}; StationJoin<Sim> j; j.reset(s, 0);
    run(s, j, 5000);
    assert(j.connected() && s.scans == 0 && s.begins == 1 && s.link_channel == 11);
  }
  // 10) needsCredentials only when a join attempt is due; stop() drops our link
  {
    Sim s; s.aps = {{"Home", 6, -50, 1}}; StationJoin<Sim> j; j.reset(s, 0);
    assert(j.needsCredentials(0));
    run(s, j, 5000);
    assert(!j.needsCredentials(s.now));
    j.stop(s); assert(!s.linked && !j.connected());
    // millis() wrap
    Sim w; w.aps = {{"Home", 6, -50, 1}}; StationJoin<Sim> x; w.now = 0xFFFFF000u; x.reset(w, w.now);
    run(w, x, 10000); assert(x.connected());
  }
  // 12) `set bridge.channel` away from the joined AP: WiFi is dropped
  //     (wrong-channel), the radio goes to the new channel, and the station
  //     rejoins only on that channel (the AP is not there: it stays off)
  {
    Sim s; s.aps = {{"Home", 6, -50, 1}}; StationJoin<Sim> j; j.reset(s, 0);
    run(s, j, 5000);
    assert(j.connected() && s.linked);
    s.espnow = 2;                          // the bridge's new channel
    j.dropForChannelChange(s, s.now);
    assert(!s.linked && !j.connected() && j.reason() == JoinReason::WrongChannel && s.radio_channel == 2);
    const int begins = s.begins;
    run(s, j, 300000, 20);
    assert(s.begins == begins && !s.linked && s.radio_channel == 2);
    s.aps[0].channel = 2;                  // the router follows to channel 2
    run(s, j, 130000, 20);
    assert(j.connected() && s.link_channel == 2);
  }
  std::printf("worst poll %.2f ms\n", worst);
  return 0;
}
'''


class StationJoinTests(unittest.TestCase):
    def test_never_blocks_never_leaves_the_espnow_channel(self):
        build_and_run(self, CHECKS)


class BridgeChannelChangeTests(unittest.TestCase):
    """`set bridge.channel` while the UDP station is joined (2026-11-03 step 6)."""

    @classmethod
    def setUpClass(cls):
        cls.repeater = (ROOT / "examples/simple_repeater/MyMesh.cpp").read_text()
        cls.header = (ROOT / "examples/simple_repeater/MyMesh.h").read_text()
        cls.cli = (ROOT / "src/helpers/CommonCLI.cpp").read_text()
        cls.bridge = (ROOT / "src/helpers/bridges/ESPNowBridge.cpp").read_text()

    def test_every_espnow_bridge_start_drops_an_off_channel_udp_station_first(self):
        for anchor in ("    configureBridgeFilter(active_bridge);\n    active_bridge->begin();",
                       "    configureBridgeFilter(&espnow_bridge);\n    espnow_bridge.begin();"):
            at = self.header.index(anchor)
            self.assertIn("prepareUdpForBridgeChannel();", self.header[at - 160:at])
        prep = method(self.repeater, "void MyMesh::prepareUdpForBridgeChannel()")
        self.assertIn("udp_join.dropForChannelChange(udp_join_driver, millis());", prep)
        self.assertIn("udp_bridge_restart_wanted = true;", prep)
        self.assertIn("udp_join_driver.restoreChannel();", prep)

    def test_failed_restart_is_retried_and_never_saved_off(self):
        body = method(self.cli, "void CommonCLI::handleSetCmd(")
        start = body.index('memcmp(config, "bridge.channel ", 15)')
        handler = body[start:body.index('memcmp(config, "bridge.secret ", 14)')]
        self.assertNotIn("bridge_enabled =", handler)
        self.assertNotIn("bridge_enabled=", handler)
        service = method(self.repeater, "void MyMesh::serviceUDPBridge()")
        self.assertIn("if (udp_bridge_restart_wanted)", service)
        self.assertIn("udp_join_driver.pinned_channel = udpPinnedEspNowChannel();", service)

    def test_idle_station_never_blocks_the_bridge_channel(self):
        begin = method(self.bridge, "void ESPNowBridge::begin()")
        self.assertIn("esp_wifi_sta_get_ap_info(&associated_ap) != ESP_OK", begin)

    def test_dedicated_bridge_image_writes_the_espnow_intent_marker(self):
        for marker in ("const uint8_t bridge_profile = 0xA1;", "const uint8_t bridge_profile = 0xA1;   // see"):
            self.assertIn(marker, self.cli)
        self.assertEqual(self.cli.count("const uint8_t espnow_intent = "), 2)

    def test_wifi_ssid_clear_wipes_the_udp_station_copies(self):
        ssid = method(self.repeater, "bool MyMesh::setWiFiSSID(")
        self.assertIn('!strcmp(value, "off")', ssid)
        self.assertIn("forgetUDPStationCredentials();", ssid)
        forget = method(self.repeater, "void MyMesh::forgetUDPStationCredentials()")
        self.assertIn("memset(udp_station_password, 0, sizeof(udp_station_password));", forget)
        web = (ROOT / "src/helpers/esp32/WebConfigServer.cpp").read_text()
        clear = method(web, "bool WebConfigServer::setStandaloneWiFiSSID(")
        self.assertIn("standalone_clear", clear)
        self.assertIn("memset(credentials.password, 0, sizeof(credentials.password));", clear)


class StationJoinWiringTests(unittest.TestCase):
    def test_repeater_uses_the_async_join_and_checks_a_linked_station(self):
        repeater = (ROOT / "examples/simple_repeater/MyMesh.cpp").read_text()
        body = method(repeater, "void MyMesh::serviceUDPBridge()")
        self.assertNotIn("beginStation(", body)
        self.assertNotIn("scanNetworks(", body)
        self.assertIn("udp_join.poll(udp_join_driver, now, udp_station_ssid, udp_station_password);", body)
        self.assertIn("const bool station_ready = linked && udp_join.connected();", body)
        # The WiFi password is wiped once WiFi.begin() has it (end of the scan).
        poll = body.index("udp_join.poll(")
        self.assertIn("memset(udp_station_password, 0, sizeof(udp_station_password));", body[poll:])

    def test_driver_scans_asynchronously(self):
        driver = (ROOT / "src/helpers/esp32/StationJoinDriver.h").read_text()
        self.assertIn("WiFi.scanNetworks(true,", driver)
        self.assertNotIn("WiFi.scanNetworks(false", driver)
        self.assertNotIn("beginStation(", driver)


if __name__ == "__main__":
    unittest.main()
