"""espnow.observer: opt-in repeater -> visual head feed (settings, rate limit, bridge, wiring)."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_espnow_repeat import build_and_run, compiler
from test_radio_receive_contract import method

ROOT = Path(__file__).resolve().parents[1]


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
static bool run(const char* command) {
  strcpy(reply, "untouched");
  return handleEspNowObserverCommand(command, reply, 157);
}
int main(int argc, char** argv) {
  assert(argc == 2);
  MemoryFS fs; const char* path = "/espnow_observer";
  const std::vector<uint8_t> legacy = {1, 8, 4, 7, 9}; fs.files["/com_prefs"] = legacy;
  // observer_off_by_default: before and after loading with no saved record.
  assert(!espNowObserverEnabled());
  loadEspNowObserverSettings(&fs);
  assert(!espNowObserverEnabled() && espNowObserverSettingsHealthy() && fs.files.size() == 1);
  if (!strcmp(argv[1], "default")) {
    assert(run("get espnow.observer") && !strcmp(reply, "> off"));
    assert(saveEspNowObserver(false) && fs.files.size() == 1);   // unchanged: no write
    loadEspNowObserverSettings(nullptr); assert(!espNowObserverEnabled());
    assert(!saveEspNowObserver(true));
  } else if (!strcmp(argv[1], "storage")) {
    // observer_setting_persists: round trip through the settings file.
    assert(saveEspNowObserver(true) && espNowObserverEnabled());
    const auto good = fs.files.at(path);
    assert(good.size() == 9 && !memcmp(good.data(), "ENO\1", 4));
    loadEspNowObserverSettings(&fs); assert(espNowObserverEnabled() && espNowObserverSettingsHealthy());
    assert(saveEspNowObserver(false)); loadEspNowObserverSettings(&fs); assert(!espNowObserverEnabled());
    assert(saveEspNowObserver(true) && fs.files.at(path) == good);
    // Failed writes keep the previous record and live value.
    fs.fail_write = true; assert(!saveEspNowObserver(false) && espNowObserverEnabled());
    assert(fs.files.at(path) == good); fs.fail_write = false;
    // Every corruption falls back to off.
    for (size_t bit = 0; bit < good.size() * 8; ++bit) {
      fs.files[path] = good; fs.files[path][bit / 8] ^= uint8_t(1U << (bit % 8));
      loadEspNowObserverSettings(&fs);
      assert(!espNowObserverSettingsHealthy() && !espNowObserverEnabled());
    }
    for (size_t size = 0; size < good.size(); ++size) {
      fs.files[path] = {good.begin(), good.begin() + size};
      loadEspNowObserverSettings(&fs); assert(!espNowObserverSettingsHealthy() && !espNowObserverEnabled());
    }
    // A classified malformed record can be repaired explicitly.
    assert(saveEspNowObserver(true) && espNowObserverSettingsHealthy() && fs.files.at(path) == good);
    // An unreadable record cannot be overwritten blindly.
    fs.fail_read_open = true; loadEspNowObserverSettings(&fs);
    assert(!espNowObserverSettingsHealthy() && !espNowObserverEnabled());
    fs.fail_read_open = false; assert(!saveEspNowObserver(false) && fs.files.at(path) == good);
    loadEspNowObserverSettings(&fs); assert(espNowObserverEnabled());
  } else {
    for (const char* other : {"get espnow.observerX", "set espnow.observers on", "get espnow.peer",
                              "get espnow.repeat", "get espnow.stats", "set espnow.repeat 2"})
      assert(!run(other) && !strcmp(reply, "untouched"));
    for (const char* bad : {"set espnow.observer", "set espnow.observer 1", "set espnow.observer ON",
                            "set espnow.observer on x", "get espnow.observer on"})
      assert(run(bad) && !strncmp(reply, "Error:", 6));
    assert(fs.files.size() == 1 && !espNowObserverEnabled());
    assert(run("set espnow.observer on") && !strcmp(reply, "OK - espnow.observer saved"));
    assert(espNowObserverEnabled());
    assert(run("set espnow.observer\ton") && !strcmp(reply, "OK - espnow.observer unchanged"));
    assert(run("get espnow.observer") && !strcmp(reply, "> on"));
    loadEspNowObserverSettings(&fs); assert(espNowObserverEnabled());
    assert(run("set espnow.observer off") && !strcmp(reply, "OK - espnow.observer saved"));
    assert(run("get espnow.observer") && !strcmp(reply, "> off"));
    fs.files[path][4] ^= 1; loadEspNowObserverSettings(&fs);
    assert(run("get espnow.observer") && strstr(reply, "observer off") && !strncmp(reply, "Error:", 6));
    char bounded[] = {'x', 'x', 'x', 'z'};
    assert(handleEspNowObserverCommand("get espnow.observer", bounded, 3) && !bounded[2] && bounded[3] == 'z');
  }
  assert(fs.files.at("/com_prefs") == legacy);
  return 0;
}
'''


OUTBOX_CHECKS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <helpers/bridges/ObserverRateLimiter.h>
using namespace mesh::observer;
static std::vector<uint8_t> record(uint8_t id) {
  PacketRecord r{}; r.rx_ms = id; r.len = 3; r.raw[0] = id; r.raw[1] = 1; r.raw[2] = 2;
  std::vector<uint8_t> out(MAX_PACKET_FRAME); out.resize(encodePacket(r, out.data(), out.size()));
  assert(!out.empty()); return out;
}
static std::vector<uint8_t> vitals(uint32_t up) {
  Vitals v{}; v.uptime_s = up;
  std::vector<uint8_t> out(VITALS_FRAME); assert(encodeVitals(v, out.data(), out.size()) == VITALS_FRAME);
  return out;
}
static uint8_t frame[MAX_PACKET_FRAME];
static bool drain(Outbox& box, std::vector<std::vector<uint8_t>>* into = nullptr) {
  const size_t n = box.pop(frame, sizeof(frame));
  if (n && into) into->emplace_back(frame, frame + n);
  return n != 0;
}
int main() {
  // observer_rate_limit_drops_oldest_keeps_vitals: 60 records in 1 s -> <= 40 sent.
  {
    Outbox box; unsigned sent = 0;
    for (unsigned i = 0; i < 60; ++i) {
      auto r = record(uint8_t(i));
      box.offer(r.data(), r.size(), 1000 + i / 4, false);   // 60 records within 15 ms
      while (drain(box)) ++sent;
    }
    assert(sent == 40);
    assert(box.stats().rate_drops == 20);
    auto v = vitals(7);                                       // same second: still sent
    assert(box.offer(v.data(), v.size(), 1015, false) == Offer::Queued);
    std::vector<std::vector<uint8_t>> out; assert(drain(box, &out) && out[0] == v);
    // Spread over one full second the bucket refills at 20/s.
    auto r = record(99);
    assert(box.offer(r.data(), r.size(), 1015, false) == Offer::RateLimited);
    assert(box.offer(r.data(), r.size(), 1065, false) == Offer::Queued);   // +50 ms = 1 token
    assert(box.offer(r.data(), r.size(), 1065, false) == Offer::RateLimited);
    drain(box);
    // Long idle refills to the burst only, never beyond.
    unsigned burst = 0;
    for (unsigned i = 0; i < 100; ++i) { if (box.offer(r.data(), r.size(), 900000, false) == Offer::Queued) ++burst; drain(box); }
    assert(burst == 40);
    // Refill is exactly 20 tokens per second.
    { TokenBucket full; for (unsigned i = 0; i < 40; ++i) assert(full.take(5000)); assert(!full.take(5000));
      unsigned refilled = 0; while (full.take(6000)) ++refilled; assert(refilled == 20); }
    { TokenBucket early; for (unsigned i = 0; i < 40; ++i) assert(early.take(5000));
      unsigned refilled = 0; while (early.take(5999)) ++refilled; assert(refilled == 19); }
    // millis() wrap.
    TokenBucket b(20, 2); assert(b.take(0xFFFFFFF0u) && b.take(0xFFFFFFF0u) && !b.take(0xFFFFFFF0u));
    assert(!b.take(0xFFFFFFF0u + 49) && b.take(0x00000022u));
  }
  // Under load without a consumer the oldest packet records are dropped; vitals kept.
  {
    Outbox box;
    auto v = vitals(1);
    assert(box.offer(v.data(), v.size(), 0, false) == Offer::Queued);
    for (uint8_t i = 1; i <= 6; ++i) { auto r = record(i); assert(box.offer(r.data(), r.size(), 0, false) == Offer::Queued); }
    assert(box.size() == kOutboxDepth && box.stats().overflow_drops == 3);
    std::vector<std::vector<uint8_t>> out; while (drain(box, &out)) {}
    assert(out.size() == 4 && out[0] == v && out[1] == record(4) && out[2] == record(5) && out[3] == record(6));
    // All-vitals outbox: the oldest vitals frame goes.
    for (uint32_t up = 1; up <= 5; ++up) { auto x = vitals(up); box.offer(x.data(), x.size(), 0, false); }
    out.clear(); while (drain(box, &out)) {}
    assert(out.size() == 4 && out[0] == vitals(2) && out[3] == vitals(5));
  }
  // mesh_frames_preempt_observer (admission half): pending mesh drops records, not vitals.
  {
    Outbox box; auto r = record(1); auto v = vitals(3);
    assert(box.offer(r.data(), r.size(), 0, true) == Offer::Preempted && box.empty());
    assert(box.stats().preempt_drops == 1);
    assert(box.offer(v.data(), v.size(), 0, true) == Offer::Queued && box.size() == 1);
  }
  // Only well-formed observer frames are admitted.
  {
    Outbox box; const uint8_t junk[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    assert(box.offer(junk, sizeof(junk), 0, false) == Offer::Invalid);
    assert(box.offer(nullptr, 10, 0, false) == Offer::Invalid && box.empty());
    auto r = record(1); r.push_back(0);
    assert(box.offer(r.data(), r.size(), 0, false) == Offer::Invalid);
    uint8_t tiny[4]; auto ok = record(2); box.offer(ok.data(), ok.size(), 0, false);
    assert(box.pop(tiny, sizeof(tiny)) == 0 && box.size() == 1);
  }
  // Vitals helpers.
  assert(perMinute(10, 5000) == 120 && perMinute(0, 5000) == 0 && perMinute(5, 0) == 0);
  assert(perMinute(0xFFFFFFFFu, 1) == 0xFFFF);
  assert(permille(250, 5000) == 50 && permille(9000, 5000) == 1000 && permille(1, 0) == 0);
  assert(snrQuarterDb(-7.25f) == -29 && snrQuarterDb(10.0f) == 40 && snrQuarterDb(-50.0f) == -128);
  assert(snrQuarterDb(40.0f) == 127 && snrQuarterDb(0.0f / 0.0f) == -128);
  assert(clampRssi(-112.4f) == -112 && clampRssi(-1e9f) == -32768);
  std::printf("PASS outbox\n");
  return 0;
}
'''


BRIDGE_CHECKS = r'''
#include <helpers/bridges/ObserverFrame.h>
static std::vector<uint8_t> record(uint8_t id){
 mesh::observer::PacketRecord r{};r.len=3;r.raw[0]=id;
 std::vector<uint8_t> out(mesh::observer::MAX_PACKET_FRAME);out.resize(mesh::observer::encodePacket(r,out.data(),out.size()));return out;
}
static std::vector<uint8_t> vitals(uint32_t up){
 mesh::observer::Vitals v{};v.uptime_s=up;
 std::vector<uint8_t> out(mesh::observer::VITALS_FRAME);mesh::observer::encodeVitals(v,out.data(),out.size());return out;
}
int main(){
 Backend backend;backend.wifi=true;mesh::wireless::control().begin(backend);
 sdk_initialized=sdk_started=true;sdk_mode=WIFI_STA;
 NodePrefs prefs;mesh::PacketManager mgr;mesh::RTCClock rtc;
 auto& config=mesh::espnow::espNowRepeatConfig();
 ESPNowBridge bridge(&prefs,&mgr,&rtc);
 auto r0=record(9);assert(!bridge.sendObserverFrame(r0.data(),r0.size()));   // not running
 bridge.begin();assert(bridge._initialized);
 // observer off (never called): today's bytes and stats exactly.
 now_ms=1000;auto a=packet(1),b=packet(2,40);
 bridge.sendPacket(&a);bridge.loop();complete(bridge);bridge.sendPacket(&b);bridge.loop();complete(bridge);
 assert(sent.size()==2&&sent[0]==std::vector<uint8_t>(a.data,a.data+a.length)
        &&sent[1]==std::vector<uint8_t>(b.data,b.data+b.length));
 char reply[160];bridge.formatStats("tx",reply,157);
 assert(!strcmp(reply,"> req=2 queued=2 start=2 ok=2 err=0 cbfail=0 timeout=0 rep=0"));
 // Idle: an observer record goes out next loop, unwrapped, never counted as mesh.
 sent.clear();auto r1=record(1);
 assert(bridge.sendObserverFrame(r1.data(),r1.size()));bridge.loop();
 assert(sent.size()==1&&sent[0]==r1&&bridge._tx_waiting&&bridge._tx_count==0);
 complete(bridge);assert(!bridge._tx_waiting);
 bridge.formatStats("tx",reply,157);
 assert(!strcmp(reply,"> req=2 queued=2 start=2 ok=2 err=0 cbfail=0 timeout=0 rep=0"));
 assert(bridge._observer.stats().sent_ok==1);
 // mesh_frames_preempt_observer: a pending mesh frame drops the record; mesh frame sent.
 sent.clear();auto c=packet(3);bridge.sendPacket(&c);assert(bridge._tx_count==1);
 auto r2=record(2);assert(!bridge.sendObserverFrame(r2.data(),r2.size()));
 auto v1=vitals(5);assert(bridge.sendObserverFrame(v1.data(),v1.size()));   // vitals wait
 bridge.loop();assert(sent.size()==1&&sent[0][0]==3);
 complete(bridge);assert(sent.size()==2&&sent[1]==v1);complete(bridge);
 // Queued records yield to mesh traffic that arrives later.
 sent.clear();auto r3=record(3),r4=record(4);
 assert(bridge.sendObserverFrame(r3.data(),r3.size())&&bridge.sendObserverFrame(r4.data(),r4.size()));
 auto d=packet(4);bridge.sendPacket(&d);bridge.loop();
 assert(sent.size()==1&&sent[0][0]==4);complete(bridge);
 assert(sent.size()==2&&sent[1]==r3);
 // Mesh traffic queued while a record is on air is not misattributed.
 auto e=packet(5);bridge.sendPacket(&e);
 complete(bridge);assert(sent.size()==3&&sent[2][0]==5&&bridge._tx_count==1);
 complete(bridge);assert(sent.size()==4&&sent[3]==r4&&bridge._tx_count==0);complete(bridge);
 bridge.formatStats("tx",reply,157);
 assert(!strcmp(reply,"> req=5 queued=5 start=5 ok=5 err=0 cbfail=0 timeout=0 rep=0"));
 // Observer frames are never repeated, never retried.
 config.count=3;config.gap_ms=30;sent.clear();
 auto r5=record(5);bridge.sendObserverFrame(r5.data(),r5.size());bridge.loop();
 complete(bridge,ESP_NOW_SEND_FAIL);
 for(unsigned i=0;i<20;++i){now_ms+=10;bridge.loop();}
 assert(sent.size()==1&&bridge._observer.stats().sent_fail==1&&bridge._stats.tx.repeats==0);
 // A failed start drops the record and leaves the queue idle.
 send_result=ESP_ERR_ESPNOW_NO_MEM;auto r6=record(6);bridge.sendObserverFrame(r6.data(),r6.size());
 bridge.loop();assert(!bridge._tx_waiting&&bridge._observer.empty());send_result=ESP_OK;
 // A lost observer callback restarts the session like any other frame.
 sent.clear();auto r7=record(7);bridge.sendObserverFrame(r7.data(),r7.size());bridge.loop();
 assert(sent.size()==1&&bridge._tx_waiting);now_ms+=1000;bridge.loop();
 assert(bridge._initialized&&!bridge._tx_waiting&&bridge._stats.tx.callback_timeout==0);
 assert(bridge._observer.stats().timeouts==1&&bridge._stats.error.stage!=mesh::bridge::diag::Stage::TxTimeout);
 // Then mesh traffic still flows, still repeated as configured.
 auto f=packet(6);bridge.sendPacket(&f);bridge.loop();complete(bridge);
 now_ms+=30;bridge.loop();assert(sent.size()==3&&sent[2][0]==6);complete(bridge);
 config.count=1;
 // Observer frames from other repeaters are not parsed as MeshCore packets.
 const uint8_t mac[6]={2,9,9,9,9,9};auto foreign=record(8);
 const auto bad_before=bridge._stats.rx.packet_bad;
 bridge.queueReceivedFrame(mac,foreign.data(),int(foreign.size()));bridge.loop();
 assert(bridge._stats.rx.packet_bad==bad_before&&mgr.received.empty());
 // Stopping the bridge drops queued observer frames: none go out after a restart.
 { auto q1=vitals(11),q2=vitals(12);sent.clear();   // vitals queue even behind owed mesh copies
   assert(bridge.sendObserverFrame(q1.data(),q1.size())&&bridge.sendObserverFrame(q2.data(),q2.size()));
   assert(bridge._observer.size()==2);bridge.end();assert(bridge._observer.empty());
   bridge.begin();for(unsigned i=0;i<5;++i){now_ms+=10;bridge.loop();}assert(sent.empty());
   // `set espnow.observer off` discards what is still queued.
   assert(bridge.sendObserverFrame(q1.data(),q1.size()));bridge.discardObserverFrames();
   bridge.loop();assert(sent.empty()&&bridge._observer.empty()); }
 bridge.end();
 // Unicast peer: observer frames follow espnow.peer like mesh frames.
 const uint8_t peer[]={2,16,32,48,64,80};memcpy(mesh::bridge::configured_peer,peer,6);
 { ESPNowBridge unicast(&prefs,&mgr,&rtc);unicast.begin();sent_peers.clear();
   auto r9=record(9);unicast.sendObserverFrame(r9.data(),r9.size());unicast.loop();
   assert(sent_peers.size()==1&&!memcmp(sent_peers[0].data(),peer,6));complete(unicast);unicast.end(); }
}
'''


def settings_folder(tmp):
    folder = Path(tmp)
    (folder / "helpers").mkdir()
    mock = (ROOT / "test/fixtures/radio_profiles/mocks/helpers/IdentityStore.h").read_text()
    (folder / "IdentityStore.h").write_text(mock)
    (folder / "helpers/IdentityStore.h").write_text('#include "../IdentityStore.h"\n')
    (folder / "FilePresence.h").write_text('''#pragma once
#include "IdentityStore.h"
namespace mesh {template<class FS> bool filePresence(FS* fs,const char* path,bool& present) {
  if(!fs||!path)return false;present=fs->exists(path);return true;
}}
''')
    for name in ("ObserverSettings.h", "ContactFileTransaction.h", "CheckedSettingsFile.h"):
        (folder / name).write_bytes((ROOT / "src/helpers" / name).read_bytes())
    (folder / "settings_under_test.cpp").write_bytes(
        (ROOT / "src/helpers/ObserverSettings.cpp").read_bytes())
    return folder


class ObserverSettingsTests(unittest.TestCase):
    def run_settings(self, mode):
        with tempfile.TemporaryDirectory(prefix="espnow-observer-settings-") as tmp:
            folder = settings_folder(tmp)
            build_and_run(self, SETTINGS_CHECKS, includes=[folder, ROOT / "src/helpers"],
                          defines=["ESP32_PLATFORM=1", "WITH_ESPNOW_BRIDGE=1"], args=[mode], strict=False)

    def test_observer_off_by_default(self):
        self.run_settings("default")

    def test_observer_setting_persists(self):
        self.run_settings("storage")

    def test_observer_cli_get_set(self):
        self.run_settings("cli")

    def test_non_bridge_build_has_no_observer_symbols(self):
        with tempfile.TemporaryDirectory(prefix="no-espnow-observer-") as tmp:
            obj = Path(tmp) / "observer.o"
            result = subprocess.run([compiler(), "-std=c++17", "-DNRF52_PLATFORM=1", "-c",
                str(ROOT / "src/helpers/ObserverSettings.cpp"), "-o", str(obj)],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            symbols = subprocess.check_output(["nm", "-C", str(obj)], text=True)
            self.assertNotRegex(symbols, r"Observer|settings_fs|healthy")


VITALS_WINDOW_CHECKS = r'''
#include <cassert>
#include <cstdio>
#include <helpers/bridges/ObserverRateLimiter.h>
using namespace mesh::observer;
int main() {
  // A quiet mesh: about 13 packets a minute, vitals every kVitalsIntervalMs.
  // A per-interval rate reads 0 or 12/min in turn; the window reads ~13.
  VitalsWindow w;
  VitalsRates r{};
  uint32_t rx = 0, tx = 0, dup = 0, air = 0, next_rx = 0, zeros = 0, reports = 0;
  for (uint32_t t = 0; t <= 180000; t += 100) {
    if (t >= next_rx) { rx++; dup += rx % 4 == 0; next_rx += 4615; }
    if (t % 1000 == 0) air += 30;
    if (t % kVitalsIntervalMs != 0) continue;
    if (t % 15000 == 0) tx += 3;
    const bool got = w.sample(VitalsCounters{t, rx, tx, dup, air}, r);
    assert(got == (t > 0));
    if (t >= 60000) {
      reports++;
      zeros += r.rx_per_min == 0;
      assert(r.rx_per_min >= 11 && r.rx_per_min <= 15);
      assert(r.tx_per_min == 12 && r.dup_per_min >= 2 && r.dup_per_min <= 5);
      assert(r.airtime_permille == 30);
    }
  }
  assert(reports > 20 && zeros == 0);
  // The window is one minute: traffic that stops reads 0 a minute later.
  uint32_t t = 185000;
  for (int i = 0; i < 13; i++, t += kVitalsIntervalMs) w.sample(VitalsCounters{t, rx, tx, dup, air}, r);
  assert(r.rx_per_min == 0 && r.tx_per_min == 0 && r.dup_per_min == 0);
  // Counters that go backwards (stats reset) restart the window: no 0xFFFF.
  assert(!w.sample(VitalsCounters{t, 0, 0, 0, 0}, r));
  assert(w.sample(VitalsCounters{t + kVitalsIntervalMs, 1, 0, 0, 0}, r) && r.rx_per_min == 12);
  // reset(): the next sample starts over.
  w.reset();
  assert(!w.started() && !w.sample(VitalsCounters{t + 10000, 5, 0, 0, 0}, r) && w.started());
  std::printf("PASS vitals_window\n");
  return 0;
}
'''


class ObserverVitalsWindowTests(unittest.TestCase):
    def test_vitals_rates_over_a_minute_not_one_interval(self):
        build_and_run(self, VITALS_WINDOW_CHECKS)

    def test_repeater_vitals_use_the_window(self):
        repeater = (ROOT / "examples/simple_repeater/MyMesh.cpp").read_text()
        vitals = method(repeater, "void MyMesh::serviceObserverVitals()")
        self.assertIn("w.rates.sample(", vitals)
        self.assertNotIn("perMinute(rx - w.rx", vitals)


class ObserverRateLimitTests(unittest.TestCase):
    def test_observer_rate_limit_drops_oldest_keeps_vitals(self):
        build_and_run(self, OUTBOX_CHECKS)

    def test_headers_compile_as_esp32_gnu11(self):
        # The ESP32 Arduino toolchain builds this code as gnu++11.
        with tempfile.TemporaryDirectory(prefix="observer-gnu11-") as tmp:
            cpp = Path(tmp) / "use.cpp"
            cpp.write_text(OUTBOX_CHECKS)
            result = subprocess.run([compiler(), "-std=gnu++11", "-fsyntax-only", "-Wall", "-Wextra",
                "-Werror", "-I" + str(ROOT / "src"), str(cpp)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_rate_limiter_header_is_arduino_free(self):
        header = (ROOT / "src/helpers/bridges/ObserverRateLimiter.h").read_text()
        for forbidden in ("Arduino.h", "esp_now", "millis(", "WiFi"):
            self.assertNotIn(forbidden, header)


class ObserverBridgeTests(unittest.TestCase):
    def test_mesh_frames_preempt_observer(self):
        from test_bridge_recovery import code, COMMON
        source = code() + COMMON + BRIDGE_CHECKS
        for version in (40400, 50200, 50500):
            with self.subTest(idf=version):
                build_and_run(self, source, strict=False, defines=[
                    "ESP32=1", "WITH_ESPNOW_BRIDGE=1", "WITH_RS232_BRIDGE=1",
                    f"ESP_IDF_VERSION={version}", f"ESP_IDF_VERSION_MAJOR={version // 10000}"])


class ObserverWiringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repeater = (ROOT / "examples/simple_repeater/MyMesh.cpp").read_text()
        cls.common = (ROOT / "src/helpers/CommonCLI.cpp").read_text()

    def test_log_rx_raw_calls_observer_only_when_enabled(self):
        body = method(self.repeater, "void MyMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len)")
        guard = body.index("if (mesh::espnow::espNowObserverEnabled())")
        self.assertLess(guard, body.index("emitObserverPacket(snr, rssi, raw, len);"))
        self.assertEqual(self.repeater.count("emitObserverPacket("), 2)   # definition + one call
        emit = method(self.repeater, "void MyMesh::emitObserverPacket(")
        self.assertIn("observerFeedActive()", emit)   # any transport (observer.transport)
        self.assertIn("Disposition::Unknown", emit)
        self.assertIn("mesh::observer::setPacketBytes(record, raw, (size_t)len)", emit)
        self.assertNotIn("MAX_RAW", emit.split("setPacketBytes")[1])
        self.assertIn("offerObserverFrame(frame, frame_len)", emit)

    def test_vitals_timer_in_loop_only_when_enabled(self):
        loop = method(self.repeater, "void __attribute__((noinline)) MyMesh::servicePostMeshLoop()")
        # Before the bridge pump, so a disable never lets a queued record out.
        self.assertIn("#if defined(WITH_ESPNOW_BRIDGE)\n  serviceObserverVitals();\n", loop)
        self.assertLess(loop.index("serviceObserverVitals();"), loop.index("espnow_bridge.loop();"))
        self.assertEqual(self.repeater.count("serviceObserverVitals();"), 1)
        vitals = method(self.repeater, "void MyMesh::serviceObserverVitals()")
        self.assertLess(vitals.index("observerActive()"), vitals.index("offerObserverFrame("))
        self.assertIn("kVitalsIntervalMs", vitals)
        self.assertIn("#if MESH_ENABLE_RADIO_PRESETS", vitals)
        disabled = vitals[:vitals.index("const uint32_t now = millis();")]
        self.assertIn("diagnosticEspNowBridge().discardObserverFrames();", disabled)

    def test_cli_loads_and_dispatches_admin_only(self):
        load = method(self.common, "void CommonCLI::loadPrefs(FILESYSTEM* fs)")
        self.assertIn("#if defined(MESH_ESPNOW_OBSERVER_SETTINGS)\n  mesh::espnow::loadEspNowObserverSettings(fs);", load)
        handle = method(self.common, "void CommonCLI::handleCommand(uint32_t sender_timestamp, char* command, char* reply)")
        self.assertIn("mesh::espnow::handleEspNowObserverCommand(command, reply, 157, &observer_runtime)", handle)

    def test_manager_allow_lists_exclude_observer(self):
        for name in ("static bool isRegionMgrAllowed(const char* cmd)",
                     "static bool isFilterMgrAllowed(const char* cmd)",
                     "static bool isCommonManagerReadOnlyAllowed(const char* cmd)"):
            body = method(self.repeater, name)
            self.assertNotIn("espnow", body, name)
            self.assertNotIn('"get"', body, name)
            self.assertNotIn('"set"', body, name)

    def test_manager_allow_lists_reject_observer_commands_at_runtime(self):
        names = ("static bool commandFamilyMatches(", "static bool commandListMatches(",
                 "static bool isCommonManagerReadOnlyAllowed(", "static bool isRegionMgrAllowed(",
                 "static bool isFilterMgrAllowed(")
        functions = "\n".join(method(self.repeater, name) for name in names)
        source = ("#include <cassert>\n#include <cstring>\n#include <helpers/FloodCommandAliases.h>\n"
                  "#define MESH_ENABLE_FLOOD_RULE_ENGINE 1\n" + functions + r'''
int main() {
  const char* observer[] = {"get espnow.observer", "set espnow.observer on", "set espnow.observer off",
                            "  set espnow.observer on", "set espnow.observer\ton",
                            "set observer on", "get observer", "set observer.transport both",
                            "get observer.transport"};
  for (const char* c : observer)
    assert(!isRegionMgrAllowed(c) && !isFilterMgrAllowed(c) && !isCommonManagerReadOnlyAllowed(c));
  // Controls: the matchers are live.
  assert(isFilterMgrAllowed("set repeat on") && isRegionMgrAllowed("region") && isCommonManagerReadOnlyAllowed("ver"));
  return 0;
}
''')
        build_and_run(self, source, strict=False)

    def test_bridge_send_path_marks_observer_frames_separately(self):
        source = (ROOT / "src/helpers/bridges/ESPNowBridge.cpp").read_text()
        send = method(source, "bool ESPNowBridge::sendObserverFrame(")
        self.assertIn("_observer.offer(", send)
        self.assertNotIn("queueTransmitFrames", send)
        self.assertNotIn("_seen_packets", send)


if __name__ == "__main__":
    unittest.main()
