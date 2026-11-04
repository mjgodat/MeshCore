"""Visual head transport selection: espnow (default) / udp / both / auto.

Host tests of helpers/HeadTransport.h: mode parsing, the saved config (NVS
image) and its masking, the CLI text including the channel-rule refusal, the
auto fail-over / fail-back state machine with hysteresis, and the
de-duplication of observer frames that arrive on both transports.
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
#include <initializer_list>
#include <string>
#include <helpers/HeadTransport.h>
#include <helpers/bridges/ObserverFrame.h>
using namespace mesh::headlink;

static char reply[160];
static Status st;
static Result run(Config& c, const char* cmd) {
  strcpy(reply, "untouched");
  return handleCommand(c, cmd, reply, 157, st);
}

static void modes() {
  Mode m = Mode::Udp;
  const char* names[] = {"espnow", "udp", "both", "auto"};
  for (int i = 0; i < 4; ++i) { assert(parseMode(names[i], m) && (int)m == i && !strcmp(modeName(m), names[i])); }
  for (const char* bad : {"", "ESPNOW", "wifi", "auto ", "udp,both"}) { m = Mode::Udp; assert(!parseMode(bad, m) && m == Mode::Udp); }
  assert(!usesWiFi(Mode::EspNow) && usesWiFi(Mode::Udp) && usesWiFi(Mode::Both) && usesWiFi(Mode::Auto));
  assert(!needsSharedChannel(Mode::Udp) && needsSharedChannel(Mode::Both) && needsSharedChannel(Mode::Auto));
  Config def;
  assert(def.mode == Mode::EspNow && def.local_port == 4404 && def.failover_s == 30 && !udpConfigured(def));
}

static Config full() {
  Config c; c.mode = Mode::Auto;
  strcpy(c.ssid, "Home Net"); strcpy(c.password, "hunter22pass"); strcpy(c.secret, "s3cret");
  c.repeater.ip[0] = 192; c.repeater.ip[1] = 168; c.repeater.ip[2] = 1; c.repeater.ip[3] = 50; c.repeater.port = 4404;
  c.local_port = 4405; c.failover_s = 45;
  return c;
}

static void storage() {
  uint8_t image[kImageSize];
  const Config c = full();
  assert(udpConfigured(c));
  encodeConfig(image, c);
  Config d;
  assert(decodeConfig(image, sizeof(image), d));
  assert(d.mode == Mode::Auto && !strcmp(d.ssid, "Home Net") && !strcmp(d.password, "hunter22pass")
         && !strcmp(d.secret, "s3cret") && d.repeater.port == 4404 && d.repeater.ip[3] == 50
         && d.local_port == 4405 && d.failover_s == 45);
  // every bit flip and every short read is rejected; the output is untouched
  for (size_t bit = 0; bit < sizeof(image) * 8; ++bit) {
    uint8_t bad[kImageSize]; memcpy(bad, image, sizeof(bad)); bad[bit / 8] ^= uint8_t(1U << (bit % 8));
    Config e; assert(!decodeConfig(bad, sizeof(bad), e) && e.mode == Mode::EspNow && !e.ssid[0]);
  }
  for (size_t n = 0; n < sizeof(image); ++n) { Config e; assert(!decodeConfig(image, n, e)); }
  // a record with a valid CRC but bad contents is rejected too
  Config bad = c; bad.failover_s = 5; encodeConfig(image, bad); { Config e; assert(!decodeConfig(image, sizeof(image), e)); }
  bad = c; bad.mode = (Mode)4; encodeConfig(image, bad); { Config e; assert(!decodeConfig(image, sizeof(image), e)); }
  bad = c; bad.local_port = 0; encodeConfig(image, bad); { Config e; assert(!decodeConfig(image, sizeof(image), e)); }
  bad = c; bad.repeater.ip[0] = 127; encodeConfig(image, bad); { Config e; assert(!decodeConfig(image, sizeof(image), e)); }
  // the default config round-trips (espnow, nothing configured)
  encodeConfig(image, Config()); { Config e; e.mode = Mode::Udp; assert(decodeConfig(image, sizeof(image), e) && e.mode == Mode::EspNow); }
}

static void masking() {
  Config c = full();
  assert(run(c, "get udp") == Result::Handled);
  assert(!strstr(reply, "hunter22pass") && !strstr(reply, "s3cret"));
  assert(!strcmp(reply, "> ssid=Home Net password=set repeater=192.168.1.50:4404 port=4405 secret=set failover=45s"));
  assert(run(c, "get udp.password") == Result::Handled && !strcmp(reply, "> set"));
  assert(run(c, "get udp.secret") == Result::Handled && !strcmp(reply, "> set"));
  assert(run(c, "get udp.ssid") == Result::Handled && !strcmp(reply, "> Home Net"));
  assert(run(c, "get udp.peer") == Result::Handled && !strcmp(reply, "> 192.168.1.50:4404"));
  assert(run(c, "get udp.port") == Result::Handled && !strcmp(reply, "> 4405"));
  Config e;
  assert(run(e, "get udp") == Result::Handled
         && !strcmp(reply, "> ssid=- password=- repeater=off port=4404 secret=- failover=30s"));
  assert(run(e, "get udp.password") == Result::Handled && !strcmp(reply, "> -"));
}

static void commands() {
  Config c;
  st = Status(); st.espnow_channel = 6;
  // not ours
  for (const char* other : {"get espnow.channel", "get transports", "set transportx udp", "get udpx", "set udp.enabled on", "ver"})
    assert(run(c, other) == Result::NotMine && !strcmp(reply, "untouched"));
  // transport
  assert(run(c, "set transport udp") == Result::Changed && c.mode == Mode::Udp);
  assert(strstr(reply, "set udp.ssid"));   // reminder while unconfigured
  for (const char* bad : {"set transport", "set transport wifi", "set transport UDP", "get transport x"}) {
    assert(run(c, bad) == Result::Handled && !strncmp(reply, "Error:", 6));
  }
  assert(c.mode == Mode::Udp);
  // channel rule: both/auto refused while the AP is on another channel; udp/espnow accepted
  st.ap_channel = 11;
  for (const char* m : {"set transport both", "set transport auto"}) {
    assert(run(c, m) == Result::Handled && !strncmp(reply, "Error: ", 7) && strstr(reply, "refused")
           && strstr(reply, "AP on ch 11") && strstr(reply, "router to ch 6"));
    assert(c.mode == Mode::Udp);
  }
  assert(run(c, "set transport espnow") == Result::Changed && c.mode == Mode::EspNow);
  st.ap_channel = 0;   // unknown: accepted, the link reports wrong-channel later
  assert(run(c, "set transport auto") == Result::Changed && c.mode == Mode::Auto);
  st.ap_channel = 6;
  assert(run(c, "set transport both") == Result::Changed && c.mode == Mode::Both);
  // failover
  assert(run(c, "get transport.failover") == Result::Handled && !strcmp(reply, "> 30"));
  assert(run(c, "set transport.failover 90") == Result::Changed && c.failover_s == 90);
  for (const char* bad : {"set transport.failover 9", "set transport.failover 601", "set transport.failover x",
                          "set transport.failover", "set transport.failover -5"}) {
    assert(run(c, bad) == Result::Handled && !strncmp(reply, "Error:", 6) && c.failover_s == 90);
  }
  // udp.*
  assert(run(c, "set udp.ssid My Wifi 5") == Result::Changed && !strcmp(c.ssid, "My Wifi 5"));
  assert(run(c, "set udp.ssid 123456789012345678901234567890123") == Result::Handled && !strcmp(c.ssid, "My Wifi 5"));
  // clearing: empty or off, the whole buffer zeroed
  assert(run(c, "set udp.ssid off") == Result::Changed && !strcmp(reply, "OK - udp.ssid cleared"));
  for (char ch : c.ssid) assert(ch == 0);
  assert(run(c, "set udp.ssid My Wifi 5") == Result::Changed);
  assert(run(c, "set udp.ssid") == Result::Changed && !c.ssid[0]);
  assert(run(c, "set udp.ssid My Wifi 5") == Result::Changed);
  assert(run(c, "set udp.password short") == Result::Handled && !strncmp(reply, "Error:", 6) && !c.password[0]);
  assert(run(c, "set udp.password long enough pw") == Result::Changed && !strcmp(c.password, "long enough pw"));
  assert(!strstr(reply, "long enough"));   // never echoed
  assert(run(c, "set udp.password") == Result::Changed && !c.password[0] && strstr(reply, "cleared"));
  assert(run(c, "set udp.secret 1234567890123456") == Result::Handled && !c.secret[0]);
  assert(run(c, "set udp.secret abcdefgh") == Result::Changed && !strcmp(c.secret, "abcdefgh") && !strstr(reply, "abcdefgh"));
  assert(run(c, "set udp.secret abc") == Result::Changed && !strcmp(c.secret, "abc") && !strstr(reply, "abc"));
  for (size_t i = 4; i < sizeof(c.secret); ++i) assert(c.secret[i] == 0);   // no tail of the longer one
  assert(run(c, "set udp.secret off") == Result::Changed && !strcmp(reply, "OK - udp.secret cleared"));
  for (char ch : c.secret) assert(ch == 0);
  assert(run(c, "set udp.secret abc") == Result::Changed);
  assert(run(c, "set udp.secret") == Result::Changed && !c.secret[0]);
  assert(run(c, "set udp.secret abc") == Result::Changed);
  assert(run(c, "set udp.password long enough pw") == Result::Changed);
  assert(run(c, "set udp.password off") == Result::Changed && !c.password[0] && strstr(reply, "cleared"));
  for (char ch : c.password) assert(ch == 0);
  for (const char* bad : {"set udp.peer", "set udp.peer 10.0.0.1", "set udp.peer 127.0.0.1:4404", "set udp.peer 10.0.0.1:0",
                          "set udp.peer 224.0.0.1:4404", "set udp.peer 10.0.0.256:1"})
    assert(run(c, bad) == Result::Handled && !strncmp(reply, "Error:", 6) && !c.repeater.enabled());
  assert(run(c, "set udp.peer 10.0.0.7:4404") == Result::Changed && c.repeater.enabled() && c.repeater.ip[3] == 7);
  assert(run(c, "set udp.peer off") == Result::Changed && !c.repeater.enabled());
  assert(run(c, "set udp.port 0") == Result::Handled && c.local_port == 4404);
  assert(run(c, "set udp.port 5000") == Result::Changed && c.local_port == 5000);
  // replies always fit the terminal buffer
  char tiny[8] = "xxxxxxx";
  assert(handleCommand(c, "get udp", tiny, 4, st) == Result::Handled && strlen(tiny) == 3);
}

static void status() {
  Config c; Status s; s.espnow_channel = 6;
  char out[160];
  formatStatus(c, s, out, sizeof(out));
  assert(!strcmp(out, "> espnow; ch 6 vitals -"));
  s.espnow_vitals_age_s = 3; formatStatus(c, s, out, sizeof(out));
  assert(!strcmp(out, "> espnow; ch 6 vitals 3s"));
  c = full(); s.udp = LinkState::Ready; s.sending_udp = true; s.udp_vitals_age_s = 4;
  s.local_ip[0] = 192; s.local_ip[1] = 168; s.local_ip[2] = 1; s.local_ip[3] = 60;
  formatStatus(c, s, out, sizeof(out));
  assert(!strcmp(out, "> auto via udp; espnow ch 6 vitals 3s; udp ready 192.168.1.60:4405 vitals 4s"));
  s.sending_udp = false; s.udp = LinkState::WrongChannel; s.ap_channel = 11;
  formatStatus(c, s, out, sizeof(out));
  assert(!strcmp(out, "> auto via espnow; espnow ch 6 vitals 3s; udp wrong-channel (AP on ch 11)"));
  c.mode = Mode::Both; s.udp = LinkState::Joining; formatStatus(c, s, out, sizeof(out));
  assert(!strcmp(out, "> both via both; espnow ch 6 vitals 3s; udp joining"));
  assert(!strcmp(linkStateName(LinkState::NotConfigured), "not-configured"));
}

static void autoSelector() {
  const uint32_t failover = 30000;
  AutoSelector a; uint32_t t = 1000;
  a.begin(t);
  // ESP-NOW vitals every 5 s: never fails over
  for (int i = 0; i < 40; ++i) { t += 5000; a.noteEspNowVitals(t); assert(!a.update(t, failover, true) && !a.onUdp()); }
  // vitals stop: still ESP-NOW until the fail-over time has passed
  const uint32_t last = t;
  for (; t < last + failover - 1000; t += 1000) assert(!a.update(t, failover, true) && !a.onUdp());
  // no UDP session: stays on ESP-NOW however long
  for (int i = 0; i < 100; ++i) { t += 1000; assert(!a.update(t, failover, false) && !a.onUdp()); }
  assert(a.update(t, failover, true) && a.onUdp() && a.failovers() == 1);
  // a single stray ESP-NOW vitals frame does not fail back (hysteresis)
  t += 5000; a.noteEspNowVitals(t);
  for (int i = 0; i < 60; ++i) { t += 1000; assert(!a.update(t, failover, true) && a.onUdp()); }
  // two in a row, then a gap: the streak restarts
  a.noteEspNowVitals(t); t += 5000; a.noteEspNowVitals(t);
  t += 20000; assert(!a.update(t, failover, true) && a.onUdp());
  a.noteEspNowVitals(t); t += 5000; assert(!a.update(t, failover, true));
  a.noteEspNowVitals(t); t += 5000; assert(!a.update(t, failover, true));
  // the third in a row (and >= 30 s on UDP): fail back
  a.noteEspNowVitals(t);
  assert(a.update(t, failover, true) && !a.onUdp() && a.failbacks() == 1);
  // vitals keep coming: stays on ESP-NOW
  for (int i = 0; i < 20; ++i) { t += 5000; a.noteEspNowVitals(t); assert(!a.update(t, failover, true)); }
  // fails over again, and fail-back waits for the minimum dwell even with a streak
  t += failover; assert(a.update(t, failover, true) && a.onUdp());
  const uint32_t switched = t;
  for (int i = 0; i < 4; ++i) { t += 5000; a.noteEspNowVitals(t); if (t - switched < AutoSelector::kMinUdpDwellMs) assert(!a.update(t, failover, true)); }
  t = switched + AutoSelector::kMinUdpDwellMs; a.noteEspNowVitals(t);
  assert(a.update(t, failover, true) && !a.onUdp());
  // UDP lost while on UDP: back to ESP-NOW at once (nothing better)
  t += failover + 5000; assert(a.update(t, failover, true) && a.onUdp());
  t += 1000; assert(a.update(t, failover, false) && !a.onUdp());
  // F6: UDP comes straight back with still no ESP-NOW vitals: no re-fail-over
  // for 30 s (hysteresis both ways), then over again; no flapping in between.
  const uint32_t back = t;
  for (int i = 0; i < 29; ++i) { t += 1000; assert(!a.update(t, failover, i % 2 == 0) && !a.onUdp()); }
  t = back + AutoSelector::kMinEspNowDwellMs; assert(a.update(t, failover, true) && a.onUdp());
  // a UDP session toggling every second while ESP-NOW is dead switches at most
  // once per 30 s
  { AutoSelector f; uint32_t u = 0; f.begin(u); int switches = 0;
    for (int i = 0; i < 600; ++i) { u += 1000; if (f.update(u, failover, i % 2 == 0)) ++switches; }
    assert(switches <= 2 * 600 / 30 + 2); }
  // millis() wrap: fail-over timing is wrap-safe
  AutoSelector w; uint32_t u = 0xFFFFF000u; w.begin(u); w.noteEspNowVitals(u);
  u += 10000; assert(!w.update(u, failover, true));
  u += 25000; assert(w.update(u, failover, true) && w.onUdp());
}

static std::string frameOf(uint32_t id, bool vitals) {
  uint8_t buf[256]; size_t n;
  if (vitals) { mesh::observer::Vitals v; memset(&v, 0, sizeof(v)); v.uptime_s = id; n = mesh::observer::encodeVitals(v, buf, sizeof(buf)); }
  else { mesh::observer::PacketRecord r; memset(&r, 0, sizeof(r)); r.rx_ms = id; r.len = 8; memcpy(r.raw, &id, 4); n = mesh::observer::encodePacket(r, buf, sizeof(buf)); }
  return std::string((const char*)buf, n);
}

static void dedupe() {
  FrameDeduper d;
  // the same record over ESP-NOW then UDP (any order, 3 apart) passes once
  for (uint32_t i = 0; i < 100; ++i) {
    const std::string f = frameOf(i, i % 5 == 0);
    assert(!d.duplicate((const uint8_t*)f.data(), f.size()));
    if (i >= 3) { const std::string g = frameOf(i - 3, (i - 3) % 5 == 0); assert(d.duplicate((const uint8_t*)g.data(), g.size())); }
  }
  assert(d.dropped() == 97);
  // F7: realistic skew under load: one transport 40 frames (a full burst)
  // behind the other, interleaved; every frame still passes exactly once.
  { FrameDeduper k; int passed = 0;
    for (uint32_t i = 0; i < 2000; ++i) {
      const std::string a = frameOf(10000 + i, i % 7 == 0);
      if (!k.duplicate((const uint8_t*)a.data(), a.size())) ++passed;
      if (i >= 40) { const std::string b = frameOf(10000 + i - 40, (i - 40) % 7 == 0);
                     if (!k.duplicate((const uint8_t*)b.data(), b.size())) ++passed; }
    }
    assert(passed == 2000); }
  // distinct records (different receive time or uptime) are never merged
  FrameDeduper e;
  for (uint32_t i = 0; i < 1000; ++i) { const std::string f = frameOf(i, false); assert(!e.duplicate((const uint8_t*)f.data(), f.size())); }
  // a record repeated after more than 64 others is passed again (bounded memory)
  assert(FrameDeduper::kDepth == 64);
  const std::string first = frameOf(5000, false);
  assert(!e.duplicate((const uint8_t*)first.data(), first.size()));
  for (uint32_t i = 0; i < 64; ++i) { const std::string f = frameOf(6000 + i, false); e.duplicate((const uint8_t*)f.data(), f.size()); }
  assert(!e.duplicate((const uint8_t*)first.data(), first.size()));
  assert(!e.duplicate(nullptr, 0));
  e.reset(); assert(!e.duplicate((const uint8_t*)first.data(), first.size()));
}

static void diagnosis() {
  const uint32_t f = 30000;
  // ESP-NOW-only and both: never leave the channel
  assert(!allChannelDiagnosisAllowed(Mode::EspNow, false, 0, f));
  assert(!allChannelDiagnosisAllowed(Mode::Both, false, 0, f));
  assert(!allChannelDiagnosisAllowed(Mode::Both, true, 999999, f));
  // udp / auto: only while ESP-NOW is silent
  for (Mode m : {Mode::Udp, Mode::Auto}) {
    assert(allChannelDiagnosisAllowed(m, false, 0, f));          // never heard
    assert(!allChannelDiagnosisAllowed(m, true, 3000, f));       // healthy
    assert(!allChannelDiagnosisAllowed(m, true, f - 1, f));
    assert(allChannelDiagnosisAllowed(m, true, f, f));           // silent for the fail-over time
  }
}

int main() {
  modes(); storage(); masking(); commands(); status(); autoSelector(); dedupe(); diagnosis();
  return 0;
}
'''


class HeadTransportTests(unittest.TestCase):
    def test_modes_config_cli_auto_and_dedupe(self):
        build_and_run(self, CHECKS)

    def test_header_compiles_as_esp32_gnu11(self):
        build_and_run(self, '#include <helpers/HeadTransport.h>\nint main() { return 0; }\n')


class HeadTransportWiringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.radio = (ROOT / "src/helpers/esp32/HeadLinkRadio.cpp").read_text()

    def test_espnow_mode_allocates_nothing_and_never_joins_wifi(self):
        apply = method(self.radio, "void HeadLinkRadio::apply()")
        self.assertIn("if (!hl::usesWiFi(_cfg.mode)) return;", apply)
        self.assertLess(apply.index("if (!hl::usesWiFi(_cfg.mode)) return;"), apply.index("new (std::nothrow) HeadUdpLink"))
        loop = method(self.radio, "void HeadLinkRadio::loop()")
        self.assertIn("if (!_link) return;", loop)

    def test_station_joins_only_through_the_channel_safe_async_join(self):
        self.assertIn("_join.poll(_driver, now, _cfg.ssid, _cfg.password);", self.radio)
        self.assertIn("_join(kDiagnoseEveryMs)", self.radio)
        for blocking in ("beginStation(", "WiFi.begin(", "scanNetworks(", "setAutoReconnect(true"):
            self.assertNotIn(blocking, self.radio)

    def test_link_teardown_drops_any_association(self):
        # stop() disconnects even an adopted link while Idle (StationJoin.h);
        # the head owns its WiFi, so leaving UDP always leaves the network.
        dtor = self.radio[self.radio.index("~HeadUdpLink()"):]
        dtor = dtor[:dtor.index("}")]
        self.assertIn("_join.stop(_driver);", dtor)
        join = (ROOT / "src/helpers/StationJoin.h").read_text()
        self.assertIn("if (_state != JoinState::Idle || d.linkUp()) { d.disconnect(); d.restoreChannel(); }", join)

    def test_wifi_task_reads_the_mode_under_the_lock(self):
        deliver = method(self.radio, "void HeadLinkRadio::deliverObserver(")
        self.assertNotIn("_cfg.", deliver)
        self.assertLess(deliver.index("portENTER_CRITICAL(&_mux);"), deliver.index("_rx_mode"))
        apply = method(self.radio, "void HeadLinkRadio::apply()")
        self.assertIn("_rx_mode = _cfg.mode;", apply)

    def test_secrets_never_logged(self):
        import re
        self.assertIsNone(re.search(r"(?<!sn)printf|println|Serial|logf", self.radio))

    def test_all_channel_diagnosis_follows_the_espnow_health(self):
        loop = method(self.radio, "void HeadLinkRadio::loop()")
        self.assertIn("_link->setDiagnoseAllowed(hl::allChannelDiagnosisAllowed(", loop)
        self.assertLess(loop.index("setDiagnoseAllowed"), loop.index("_link->loop();"))

    def test_dedupe_only_when_both_transports_feed_the_ui(self):
        deliver = method(self.radio, "void HeadLinkRadio::deliverObserver(")
        self.assertIn("if (mode == hl::Mode::Both || mode == hl::Mode::Auto) duplicate = _dedupe.duplicate(frame, len);", deliver)


if __name__ == "__main__":
    unittest.main()
