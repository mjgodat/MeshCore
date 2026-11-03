"""ESP-NOW redundant send: pure scheduling, settings, CLI and production send paths."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def compiler():
    return os.environ.get("CXX") or shutil.which("g++") or "c++"


def sanitizer_flags():
    # MinGW has no libasan; the Linux run is the sanitizer authority.
    return (["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
            if sys.platform.startswith("linux") else [])


def build_and_run(test, source, includes=(), defines=(), args=(), strict=True):
    with tempfile.TemporaryDirectory(prefix="espnow-repeat-") as tmp:
        folder = Path(tmp)
        cpp, exe = folder / "test.cpp", folder / "test"
        cpp.write_text(source)
        warnings = ["-Wall", "-Wextra", "-Werror"] if strict else ["-Wall", "-Wextra"]
        result = subprocess.run([compiler(), "-std=c++17", "-O1", "-g", *warnings,
            *sanitizer_flags(), *["-D" + d for d in defines],
            *["-I" + str(p) for p in (*includes, ROOT / "src", ROOT / "test")],
            str(cpp), "-o", str(exe)], capture_output=True, text=True, timeout=120)
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for arg in (args or [None]):
            run = subprocess.run([str(exe)] + ([arg] if arg else []),
                                 capture_output=True, text=True, timeout=30)
            test.assertEqual(run.returncode, 0, (arg or "") + run.stdout + run.stderr)


SCHEDULER_CHECKS = r'''
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <helpers/esp32/EspNowRepeatScheduler.h>
using namespace mesh::espnow;
int main() {
  // Bounds and strict parsing.
  assert(!isValidRepeatCount(0) && isValidRepeatCount(1) && isValidRepeatCount(4));
  assert(!isValidRepeatCount(5) && isValidRepeatGap(0) && isValidRepeatGap(200));
  assert(!isValidRepeatGap(201));
  uint32_t value = 77;
  for (const char* bad : {"", "-1", "+2", " 2", "2 ", "2x", "0x2", "1234", "4294967297"}) {
    value = 77; assert(!parseRepeatDecimal(bad, 999, value) && value == 77);
  }
  assert(!parseRepeatDecimal(nullptr, 9, value) && value == 77);
  assert(parseRepeatDecimal("0", 200, value) && value == 0);
  assert(parseRepeatDecimal("200", 200, value) && value == 200);
  assert(!parseRepeatDecimal("201", 200, value) && value == 200);
  assert(kRepeatCountDefault == 1 && kRepeatGapDefaultMs == 30);
  assert(espNowRepeatConfig().count == 1 && espNowRepeatConfig().gap_ms == 30);

  // repeat=1: never an extra copy, at any time, idle or not.
  EspNowRepeatScheduler s;
  assert(!s.pending() && !s.due(0, true));
  s.begin(1000, 1, 30); s.finished();
  for (uint32_t t = 1000; t < 1500; t += 7) assert(!s.due(t, true) && !s.pending());

  // repeat=3 gap=30: copies at t+30 and t+60 only once idle.
  s.begin(1000, 3, 30);
  assert(s.pending() && s.remaining() == 2);
  assert(!s.due(1030, true));            // original still in flight
  s.finished();
  assert(!s.due(1029, true) && !s.due(1030, false) && s.due(1030, true));
  s.copyStarted(1030); assert(s.remaining() == 1);
  assert(!s.due(1060, true));            // copy still in flight
  s.finished();
  assert(!s.due(1059, true) && s.due(1060, true));
  s.copyStarted(1060); s.finished();
  assert(!s.pending() && !s.due(5000, true));

  // A late idle radio delays the copy; the next gap is measured from it.
  s.begin(2000, 3, 30); s.finished();
  assert(!s.due(2035, false) && s.due(2035, true));
  s.copyStarted(2035); s.finished();
  assert(!s.due(2064, true) && s.due(2065, true));

  // A new frame cancels remaining repeats of the previous one.
  s.begin(3000, 4, 30); s.finished(); assert(s.due(3030, true));
  s.begin(3031, 1, 30);
  assert(!s.pending() && !s.due(9999, true));
  s.begin(3100, 3, 30); s.finished(); s.copyStarted(3130);
  s.begin(3131, 2, 30); assert(s.remaining() == 1 && !s.due(3200, true));
  s.finished(); assert(s.due(3161, true));
  s.cancel(); assert(!s.pending() && !s.due(9999, true));

  // gap=0 sends back to back as soon as the radio is idle.
  s.begin(4000, 4, 0); s.finished();
  for (uint8_t copy = 0; copy < 3; ++copy) {
    assert(!s.due(4000, false) && s.due(4000, true));
    s.copyStarted(4000); assert(!s.due(4000, true)); s.finished();
  }
  assert(!s.pending());

  // Out-of-range settings can never schedule extra or slow copies.
  s.begin(5000, 0, 30); s.finished(); assert(!s.pending());
  s.begin(5000, 5, 30); s.finished(); assert(!s.pending());
  s.begin(5000, 2, 60000); s.finished();
  assert(!s.due(5199, true) && s.due(5200, true));

  // An original that never completed (failure/timeout) arms nothing.
  s.begin(6000, 3, 30); s.abortIfUnfinished(); assert(!s.pending());
  s.begin(6000, 3, 30); s.finished(); s.abortIfUnfinished(); assert(s.pending());
  s.copyStarted(6030); s.abortIfUnfinished(); assert(!s.pending());

  // OTA firmware payloads (type 0x0C) are never repeated.
  { const uint8_t ota[] = {0x0C << 2, 1}, plain[] = {0x11, 1}, ack[] = {0x0D << 2, 1};
    assert(!isRepeatablePacket(ota, 2) && isRepeatablePacket(plain, 2) && isRepeatablePacket(ack, 2));
    assert(!isRepeatablePacket(nullptr, 2) && !isRepeatablePacket(plain, 0)); }

  // millis() wrap.
  s.begin(0xFFFFFFF0u, 2, 30); s.finished();
  assert(!s.due(0x0Du, true) && s.due(0x0Eu, true));

  std::printf("PASS scheduler\n");
  return 0;
}
'''


REASSEMBLY_CHECKS = r'''
#include <cassert>
#include <cstring>
#include <helpers/ESPNowRawFragmentation.h>
using namespace mesh::espnow;
int main() {
  uint8_t packet[255], out[255];
  for (unsigned i = 0; i < sizeof(packet); ++i) packet[i] = uint8_t(i * 7 + 1);
  packet[0] = 0x11;
  ESPNowRawFrames a, b;
  assert(encodeEspNowRawFrames(packet, sizeof(packet), a) && a.count == 2);
  // Re-encoding the stored packet for a copy is byte-identical.
  assert(encodeEspNowRawFrames(packet, sizeof(packet), b) && b.count == 2);
  for (unsigned f = 0; f < 2; ++f)
    assert(a.lengths[f] == b.lengths[f] && !memcmp(a.data[f], b.data[f], a.lengths[f]));
  ESPNowRawReassembler r;
  const uint8_t mac[6] = {2, 1, 1, 1, 1, 1};
  size_t len = 0; unsigned complete = 0, duplicate = 0;
  uint32_t now = 100;
  for (unsigned copy = 0; copy < 4; ++copy, now += 30) {
    for (unsigned f = 0; f < 2; ++f) {
      const auto result = r.acceptFrame(mac, a.data[f], a.lengths[f], now, out, sizeof(out), len);
      if (result == ESPNowRawReassemblyResult::PACKET_COMPLETE) {
        ++complete; assert(len == sizeof(packet) && !memcmp(out, packet, len));
      } else if (result == ESPNowRawReassemblyResult::DUPLICATE) ++duplicate;
      else assert(result == ESPNowRawReassemblyResult::FRAGMENT_STORED && copy == 0 && f == 0);
    }
  }
  assert(complete == 1 && duplicate == 6);
  // A lost first copy of fragment 0 is filled in by a later copy.
  ESPNowRawReassembler late;
  assert(late.acceptFrame(mac, a.data[1], a.lengths[1], 0, out, sizeof(out), len)
         == ESPNowRawReassemblyResult::FRAGMENT_STORED);
  assert(late.acceptFrame(mac, a.data[1], a.lengths[1], 30, out, sizeof(out), len)
         == ESPNowRawReassemblyResult::DUPLICATE);
  assert(late.acceptFrame(mac, a.data[0], a.lengths[0], 30, out, sizeof(out), len)
         == ESPNowRawReassemblyResult::PACKET_COMPLETE);
  return 0;
}
'''


SETTINGS_CHECKS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
#include "ESPNowRepeatSettings.h"
#include "settings_under_test.cpp"
using namespace mesh::espnow;
static char reply[160];
static bool run(const char* command) {
  strcpy(reply, "untouched");
  return handleEspNowRepeatCommand(command, reply, 157);
}
static void expect(uint8_t count, uint16_t gap) {
  assert(espNowRepeatConfig().count == count && espNowRepeatConfig().gap_ms == gap);
}
int main(int argc, char** argv) {
  assert(argc == 2);
  MemoryFS fs; const char* path = "/espnow_repeat";
  const std::vector<uint8_t> legacy = {1, 8, 4, 7, 9}; fs.files["/com_prefs"] = legacy;
  // Defaults before and after loading with no saved record; nothing written.
  expect(1, 30);
  loadEspNowRepeatSettings(&fs); expect(1, 30);
  assert(espNowRepeatSettingsHealthy() && fs.files.size() == 1);
  if (!strcmp(argv[1], "storage")) {
    assert(!saveEspNowRepeat(0, 30) && !saveEspNowRepeat(5, 30) && !saveEspNowRepeat(2, 201));
    assert(fs.files.size() == 1);
    assert(saveEspNowRepeat(1, 30) && fs.files.size() == 1); // unchanged default: no write
    assert(saveEspNowRepeat(3, 45)); expect(3, 45);
    const auto good = fs.files.at(path);
    assert(good.size() == 11 && !memcmp(good.data(), "ENR\1", 4));
    // Persistence round-trip across a reload (reboot).
    espNowRepeatConfig().count = 1; espNowRepeatConfig().gap_ms = 30;
    loadEspNowRepeatSettings(&fs); expect(3, 45); assert(espNowRepeatSettingsHealthy());
    assert(saveEspNowRepeat(4, 0)); loadEspNowRepeatSettings(&fs); expect(4, 0);
    assert(saveEspNowRepeat(2, 200)); loadEspNowRepeatSettings(&fs); expect(2, 200);
    assert(saveEspNowRepeat(3, 45)); assert(fs.files.at(path) == good);
    // Failed writes keep the previous record and live values.
    fs.fail_write = true; assert(!saveEspNowRepeat(2, 40)); expect(3, 45);
    assert(fs.files.at(path) == good); fs.fail_write = false;
    fs.fail_write_after = 4; assert(!saveEspNowRepeat(2, 40)); expect(3, 45);
    assert(fs.files.at(path) == good); fs.fail_write_after = -1;
    // Every corruption falls back to repeat off (never extra copies).
    for (size_t bit = 0; bit < good.size() * 8; ++bit) {
      fs.files[path] = good; fs.files[path][bit / 8] ^= uint8_t(1U << (bit % 8));
      loadEspNowRepeatSettings(&fs);
      assert(!espNowRepeatSettingsHealthy()); expect(1, 30);
    }
    for (size_t size = 0; size < good.size(); ++size) {
      fs.files[path] = {good.begin(), good.begin() + size};
      loadEspNowRepeatSettings(&fs); assert(!espNowRepeatSettingsHealthy()); expect(1, 30);
    }
    fs.files[path] = good; fs.files[path].push_back(0);
    loadEspNowRepeatSettings(&fs); assert(!espNowRepeatSettingsHealthy()); expect(1, 30);
    // A classified malformed record can be repaired explicitly.
    assert(saveEspNowRepeat(3, 45) && espNowRepeatSettingsHealthy() && fs.files.at(path) == good);
    // A well-formed record with out-of-range values is rejected too.
    { uint8_t image[11]; encode(image, 5, 30); fs.files[path].assign(image, image + 11);
      loadEspNowRepeatSettings(&fs); assert(!espNowRepeatSettingsHealthy()); expect(1, 30);
      encode(image, 2, 201); fs.files[path].assign(image, image + 11);
      loadEspNowRepeatSettings(&fs); assert(!espNowRepeatSettingsHealthy()); expect(1, 30); }
    fs.files[path] = good;
    // An unreadable record cannot be overwritten blindly.
    fs.fail_read_open = true; loadEspNowRepeatSettings(&fs);
    assert(!espNowRepeatSettingsHealthy()); expect(1, 30);
    fs.fail_read_open = false; assert(!saveEspNowRepeat(2, 40) && fs.files.at(path) == good);
    loadEspNowRepeatSettings(&fs); expect(3, 45);
    loadEspNowRepeatSettings(nullptr); expect(1, 30); assert(!saveEspNowRepeat(2, 40));
  } else {
    assert(run("get espnow.repeat") && !strcmp(reply, "> 1"));
    assert(run("get espnow.repeat.gap") && !strcmp(reply, "> 30"));
    for (const char* other : {"get espnow.repeatX", "get espnow.repeat.gapX", "set espnow.repeats 2",
                              "get espnow.peer", "get espnow.status", "set espnow.channel 6"}) {
      assert(!run(other) && !strcmp(reply, "untouched"));
    }
    for (const char* bad : {"set espnow.repeat", "set espnow.repeat 0", "set espnow.repeat 5",
                            "set espnow.repeat -1", "set espnow.repeat 2x", "set espnow.repeat.gap",
                            "set espnow.repeat.gap 201", "set espnow.repeat.gap -1",
                            "set espnow.repeat.gap 4.5", "get espnow.repeat 2", "get espnow.repeat.gap x"}) {
      assert(run(bad) && !strncmp(reply, "Error:", 6));
    }
    expect(1, 30); assert(fs.files.size() == 1);
    assert(run("set espnow.repeat 2") && !strcmp(reply, "OK - espnow.repeat saved")); expect(2, 30);
    assert(run("set espnow.repeat\t2") && !strcmp(reply, "OK - espnow.repeat unchanged"));
    assert(run("set espnow.repeat.gap 40") && !strcmp(reply, "OK - espnow.repeat.gap saved")); expect(2, 40);
    assert(run("set espnow.repeat.gap 0") && !strncmp(reply, "OK", 2)); expect(2, 0);
    assert(run("set espnow.repeat.gap 200") && !strncmp(reply, "OK", 2)); expect(2, 200);
    assert(run("set espnow.repeat 4") && run("get espnow.repeat") && !strcmp(reply, "> 4"));
    assert(run("get espnow.repeat.gap") && !strcmp(reply, "> 200"));
    loadEspNowRepeatSettings(&fs); expect(4, 200);
    fs.fail_write = true;
    assert(run("set espnow.repeat 1") && !strncmp(reply, "Error:", 6)); expect(4, 200);
    fs.fail_write = false;
    fs.files[path][4] ^= 1; loadEspNowRepeatSettings(&fs); expect(1, 30);
    assert(run("get espnow.repeat") && strstr(reply, "repeat off") && !strncmp(reply, "Error:", 6));
    assert(run("set espnow.repeat 2") && !strcmp(reply, "OK - espnow.repeat saved")); expect(2, 30);
    char bounded[] = {'x', 'x', 'x', 'z'};
    assert(handleEspNowRepeatCommand("get espnow.repeat", bounded, 3) && !bounded[2] && bounded[3] == 'z');
    espNowRepeatConfig().radio_copies = 7;
#if defined(WITH_ESPNOW_BRIDGE)
    assert(!run("get espnow.stats") && !strcmp(reply, "untouched"));
#else
    assert(run("get espnow.stats") && !strcmp(reply, "> repeat=7"));
    assert(!run("get espnow.stats tx"));
#endif
  }
  assert(fs.files.at("/com_prefs") == legacy);
  return 0;
}
'''


class EspNowRepeatSettingsTests(unittest.TestCase):
    def test_persistence_round_trip_corruption_and_cli(self):
        with tempfile.TemporaryDirectory(prefix="espnow-repeat-settings-") as tmp:
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
            for name in ("ESPNowRepeatSettings.h", "ContactFileTransaction.h", "CheckedSettingsFile.h"):
                (folder / name).write_bytes((ROOT / "src/helpers" / name).read_bytes())
            (folder / "settings_under_test.cpp").write_bytes(
                (ROOT / "src/helpers/ESPNowRepeatSettings.cpp").read_bytes())
            for defines in (["ESP32_PLATFORM=1", "WITH_ESPNOW_BRIDGE=1"],
                            ["ESP32_PLATFORM=1", "MESH_PRIMARY_ESPNOW=1"]):
                with self.subTest(defines=defines):
                    build_and_run(self, SETTINGS_CHECKS, includes=[folder, ROOT / "src/helpers"],
                                  defines=defines, args=["storage", "cli"], strict=False)

    def test_non_espnow_build_has_no_repeat_symbols(self):
        with tempfile.TemporaryDirectory(prefix="no-espnow-repeat-") as tmp:
            obj = Path(tmp) / "repeat.o"
            result = subprocess.run([compiler(), "-std=c++17", "-DNRF52_PLATFORM=1", "-c",
                str(ROOT / "src/helpers/ESPNowRepeatSettings.cpp"), "-o", str(obj)],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            symbols = subprocess.check_output(["nm", "-C", str(obj)], text=True)
            self.assertNotRegex(symbols, r"Repeat|settings_fs|healthy")


RADIO_CHECKS = r'''
#include <vector>
namespace mesh { namespace radioDirection {
Mode current = Mode::Both;
Mode mode() { return current; }
}}
static std::vector<std::vector<uint8_t>> air;
static void cb(int status = ESP_NOW_SEND_SUCCESS) {
  OnDataSent(nullptr, static_cast<esp_now_send_status_t>(status));
}
static std::vector<uint8_t> bytes(uint8_t id, size_t len) {
  std::vector<uint8_t> p(len); for (size_t i = 0; i < len; ++i) p[i] = uint8_t(id + i);
  p[0] = id; return p;
}
// One complete Dispatcher transmission: start, callbacks, completion, finish.
static void dispatch(ESPNOWRadio& radio, const std::vector<uint8_t>& p) {
  assert(radio.startSendRaw(p.data(), int(p.size())));
  for (unsigned guard = 0; !radio.isSendComplete(); ++guard) { assert(guard < 4); cb(); }
  radio.onSendFinished();
}
static void service(ESPNOWRadio& radio, uint32_t until) {
  for (; now_ms <= until; ++now_ms) {
    radio.loop();
    if (!is_send_complete && !radio.isInRecvMode()) { cb(); radio.loop(); }
  }
  now_ms = until;
}
int main() {
  auto& config = mesh::espnow::espNowRepeatConfig();
  ESPNOWRadio radio; radio.init(); assert(radio.isEnabled());
  // repeat=1 (default): today's frames exactly, nothing later.
  assert(config.count == 1);
  now_ms = 1000; dispatch(radio, bytes(0x11, 4)); assert(air.size() == 1);
  dispatch(radio, bytes(0x12, 255)); assert(air.size() == 3);
  service(radio, 3000); assert(air.size() == 3 && config.radio_copies == 0);
  assert(radio.getPacketsSent() == 2 && radio.isInRecvMode());

  // repeat=3 gap=30: identical copies at t+30 and t+60, only when idle.
  config.count = 3; config.gap_ms = 30; air.clear();
  now_ms = 5000; const auto p = bytes(0x21, 40); dispatch(radio, p);
  for (now_ms = 5001; now_ms < 5030; ++now_ms) radio.loop();
  assert(air.size() == 1);
  radio.loop(); assert(air.size() == 2 && air[1] == p && !radio.isInRecvMode());
  radio.loop(); assert(air.size() == 2);       // no copy while one is in flight
  now_ms = 5060; radio.loop(); assert(air.size() == 2);
  cb(); radio.loop(); assert(radio.isInRecvMode());
  radio.loop(); assert(air.size() == 3 && air[2] == p);
  cb(); service(radio, 6000);
  assert(air.size() == 3 && config.radio_copies == 2 && radio.getPacketsSent() == 3);

  // Fragmented packets repeat both byte-identical fragments, in order.
  config.count = 2; air.clear(); now_ms = 7000;
  const auto big = bytes(0x35, 255); dispatch(radio, big); assert(air.size() == 2);
  service(radio, 7100); assert(air.size() == 4 && air[2] == air[0] && air[3] == air[1]);
  {
    mesh::espnow::ESPNowRawReassembler r; uint8_t out[255], mac[6] = {2, 0, 0, 0, 0, 1};
    size_t len = 0; unsigned complete = 0;
    for (auto& f : air) complete += r.acceptFrame(mac, f.data(), f.size(), 7000, out, sizeof(out), len)
        == mesh::espnow::ESPNowRawReassemblyResult::PACKET_COMPLETE;
    assert(complete == 1);
  }

  // A new frame cancels pending copies of the previous one (not yet started).
  config.count = 4; config.gap_ms = 30; air.clear(); now_ms = 9000;
  dispatch(radio, bytes(0x41, 10)); now_ms = 9010;
  const auto fresh = bytes(0x42, 12); config.count = 1;
  dispatch(radio, fresh); assert(air.size() == 2 && air[1] == fresh);
  service(radio, 9500); assert(air.size() == 2);

  // Fresh traffic during an in-flight copy is never accepted-and-deferred:
  // Dispatcher is held (no send deadline running) until the whole copy,
  // including its second fragment, completes; remaining copies are cancelled.
  config.count = 4; air.clear(); now_ms = 10000;
  const auto big2 = bytes(0x51, 255);
  dispatch(radio, big2); now_ms = 10030; radio.loop();
  assert(air.size() == 3 && !is_send_complete);
  assert(radio.holdTransmitStart());          // copy fragment 1 on air
  const auto next = bytes(0x52, 20);
  const int calls = send_calls;
  assert(!radio.startSendRaw(next.data(), int(next.size())) && send_calls == calls);
  assert(radio.getPacketsSent() == 7);        // nothing counted before air
  cb(); radio.loop(); assert(air.size() == 4 && air[3] == air[1]); // fragment 2 kept
  assert(radio.holdTransmitStart());
  cb(ESP_NOW_SEND_FAIL); radio.loop();        // copy done (status irrelevant)
  assert(!radio.holdTransmitStart() && radio.isInRecvMode());
  dispatch(radio, next); assert(air.size() == 5 && air[4] == next);
  service(radio, 10300);                      // only the new frame's 3 copies
  assert(air.size() == 8);
  for (size_t i = 5; i < 8; ++i) assert(air[i] == next);
  // repeat=1 never holds the Dispatcher.
  config.count = 1; dispatch(radio, bytes(0x53, 9)); assert(!radio.holdTransmitStart());
  service(radio, 10600); assert(air.size() == 9);

  // lost_copy_callback_releases_hold: a copy whose callback never arrives
  // releases the Dispatcher hold within the bound; the next frame sends.
  config.count = 3; config.gap_ms = 30; air.clear(); now_ms = 10700;
  dispatch(radio, bytes(0x56, 9)); now_ms = 10730; radio.loop();
  assert(air.size() == 2 && radio.holdTransmitStart());
  now_ms = 10730 + mesh::espnow::kRepeatCopyCallbackTimeoutMs - 1;
  radio.loop(); assert(radio.holdTransmitStart() && config.radio_copy_timeouts == 0);
  ++now_ms; assert(!radio.holdTransmitStart() && config.radio_copy_timeouts == 1);
  assert(radio.isInRecvMode());
  const auto after = bytes(0x57, 9); dispatch(radio, after);
  assert(air.size() == 3 && air[2] == after);
  service(radio, 10900); assert(air.size() == 5);   // its own copies only
  // The bound also applies when only loop() observes it (no fresh traffic).
  air.clear(); now_ms = 11000; dispatch(radio, bytes(0x58, 9));
  now_ms = 11030; radio.loop(); assert(air.size() == 2);
  now_ms = 11080; radio.loop(); assert(config.radio_copy_timeouts == 2);
  assert(!radio.holdTransmitStart());
  now_ms = 11200; radio.loop(); assert(air.size() == 2);   // owed copies cancelled
  // Radio recovery clears an in-flight copy.
  dispatch(radio, bytes(0x59, 9)); now_ms = 11230; radio.loop();
  assert(radio.holdTransmitStart() && radio.recoverRadio(true));
  assert(!radio.holdTransmitStart() && radio.isInRecvMode());
  now_ms = 11500; radio.loop(); assert(air.size() == 4);
  cb();                                 // a late callback is harmless when idle
  assert(radio.isInRecvMode());

  // OTA firmware frames (payload type 0x0C) are never repeated.
  config.count = 4; air.clear(); now_ms = 11000;
  auto ota = bytes(0x31, 60); ota[0] = 0x0C << 2;
  dispatch(radio, ota); service(radio, 11500); assert(air.size() == 1);

  // Copies stop while transmit is disabled by radio direction.
  air.clear(); now_ms = 11600; dispatch(radio, bytes(0x55, 9));
  mesh::radioDirection::current = mesh::radioDirection::Mode::Rx;
  service(radio, 12000); assert(air.size() == 1);
  mesh::radioDirection::current = mesh::radioDirection::Mode::Both;
  service(radio, 12100); assert(air.size() == 1);   // cancelled, not postponed
  config.count = 3;

  // A failed original (Dispatcher timeout path) arms no copies.
  air.clear(); now_ms = 12000;
  const auto lost = bytes(0x61, 8);
  assert(radio.startSendRaw(lost.data(), int(lost.size())));
  cb(ESP_NOW_SEND_FAIL); assert(!radio.isSendComplete());
  radio.onSendFinished(); service(radio, 12500); assert(air.size() == 1);

  // gap=0 copies go back to back as each callback returns.
  config.count = 4; config.gap_ms = 0; air.clear(); now_ms = 13000;
  dispatch(radio, bytes(0x75, 6));
  for (unsigned i = 0; i < 3; ++i) { radio.loop(); assert(air.size() == i + 2); cb(); radio.loop(); }
  radio.loop(); assert(air.size() == 4);

  // Stopping the radio mid-copy cancels everything; restart starts clean.
  config.gap_ms = 30; air.clear(); now_ms = 14000;
  dispatch(radio, bytes(0x81, 6)); now_ms = 14030; radio.loop(); assert(air.size() == 2);
  radio.end(); cb(); radio.loop(); radio.init(); radio.onSendFinished();
  service(radio, 15000); assert(air.size() == 2);
  radio.end();
  return 0;
}
'''


class EspNowRepeatRadioTests(unittest.TestCase):
    def test_production_primary_radio_repeats(self):
        import re
        from test_espnow_radio_lifecycle import STUBS
        header = (ROOT / "src/helpers/esp32/ESPNOWRadio.h").read_text()
        source = (ROOT / "src/helpers/esp32/ESPNOWRadio.cpp").read_text()
        header = header[:header.index("#if ESPNOW_DEBUG_LOGGING")]
        header = re.sub(r"^\s*#(?:include|pragma).*$", "", header, flags=re.MULTILINE)
        # ESPNOWRadio.h needs the observer demux types (visual head).
        header = "#include <helpers/bridges/ObserverDemux.h>\n" + header
        header = header.replace(" : public mesh::Radio", "").replace(" override", "")
        source = re.sub(r"^\s*#include.*$", "", source, flags=re.MULTILINE)
        stubs = STUBS.replace("uint32_t millis(){return 1234;}",
                              "uint32_t now_ms=1234;uint32_t millis(){return now_ms;}")
        stubs = stubs.replace(
            "int esp_now_send(const uint8_t*,const uint8_t*,size_t){assert(sdk_active);++send_calls;return 0;}",
            "#include <vector>\nextern std::vector<std::vector<uint8_t>> air_log;\n"
            "int esp_now_send(const uint8_t* d,const uint8_t* b,size_t n){assert(sdk_active);"
            "assert(d[0]==255);++send_calls;air_log.emplace_back(b,b+n);return 0;}")
        self.assertIn("air_log", stubs)
        self.assertIn("now_ms", stubs)
        checks = RADIO_CHECKS.replace("static std::vector<std::vector<uint8_t>> air;",
                                      "std::vector<std::vector<uint8_t>> air_log;\n"
                                      "static auto& air = air_log;")
        for arduino, idf in [(2, 40400), (3, 50200), (3, 50500)]:
            with self.subTest(arduino=arduino, idf=idf):
                build_and_run(self, stubs + header + source + checks, strict=False,
                              defines=["MESH_PRIMARY_ESPNOW=1", "MESH_ESPNOW_RADIO=1",
                                       "MESH_ENABLE_RADIO_DIRECTION=1",
                                       f"ESP_ARDUINO_VERSION_MAJOR={arduino}",
                                       f"ESP_IDF_VERSION={idf}"])


BRIDGE_CHECKS = r'''
int main(){
 Backend backend;backend.wifi=true;mesh::wireless::control().begin(backend);
 sdk_initialized=sdk_started=true;sdk_mode=WIFI_STA;
 NodePrefs prefs;mesh::PacketManager mgr;mesh::RTCClock rtc;
 auto& config=mesh::espnow::espNowRepeatConfig();
 ESPNowBridge bridge(&prefs,&mgr,&rtc);bridge.begin();assert(bridge._initialized);
 // repeat=1 (default): exactly today's frames, nothing later.
 assert(config.count==1);
 now_ms=1000;auto a=packet(1);bridge.sendPacket(&a);bridge.loop();assert(sent.size()==1);
 complete(bridge);assert(bridge._tx_count==0);
 for(unsigned i=0;i<50;++i){now_ms+=10;bridge.loop();}
 assert(sent.size()==1&&bridge._stats.tx.repeats==0);
 // repeat=3 gap=30: identical copies at t+30 and t+60.
 config.count=3;config.gap_ms=30;now_ms=2000;auto b=packet(2);
 bridge.sendPacket(&b);bridge.loop();assert(sent.size()==2);
 complete(bridge);assert(bridge._tx_count==1);           // held only for its copies
 now_ms=2029;bridge.loop();assert(sent.size()==2);
 now_ms=2030;bridge.loop();assert(sent.size()==3&&sent[2]==sent[1]);
 now_ms=2059;complete(bridge);assert(sent.size()==3);
 now_ms=2060;bridge.loop();assert(sent.size()==4&&sent[3]==sent[1]);
 complete(bridge);assert(bridge._tx_count==0);
 for(unsigned i=0;i<20;++i){now_ms+=10;bridge.loop();}
 assert(sent.size()==4&&bridge._stats.tx.repeats==2);
 assert(bridge._stats.tx.start_ok==2&&bridge._stats.tx.callback_ok==2&&bridge._stats.tx.queued==2);
 char reply[160];bridge.formatStats("tx",reply,157);assert(strstr(reply," rep=2"));
 // Raw 251-255-byte packets repeat both identical fragments in order.
 config.count=2;sent.clear();now_ms=3000;auto big=packet(3,255);
 bridge.sendPacket(&big);bridge.loop();complete(bridge);complete(bridge);
 assert(sent.size()==2&&bridge._tx_count==1);
 now_ms=3030;bridge.loop();assert(sent.size()==3&&sent[2]==sent[0]);
 complete(bridge);assert(sent.size()==4&&sent[3]==sent[1]);complete(bridge);
 assert(bridge._tx_count==0);
 { mesh::espnow::ESPNowRawReassembler r;uint8_t out[255],mac[6]={2,0,0,0,0,1};size_t len=0;unsigned done=0;
   for(auto& f:sent)done+=r.acceptFrame(mac,f.data(),f.size(),now_ms,out,sizeof(out),len)
       ==mesh::espnow::ESPNowRawReassemblyResult::PACKET_COMPLETE;
   assert(done==1); }
 // New traffic cancels owed copies of the previous packet.
 config.count=4;sent.clear();now_ms=4000;auto c=packet(4),d=packet(5);
 bridge.sendPacket(&c);bridge.loop();complete(bridge);assert(bridge._tx_count==1);
 now_ms=4005;bridge.sendPacket(&d);bridge.loop();
 assert(sent.size()==2&&sent[1][0]==5&&bridge._tx_count==1);
 complete(bridge);now_ms=4035;bridge.loop();assert(sent.size()==3&&sent[2][0]==5);
 // ...including while one of its copies is in flight: no queue growth.
 auto e=packet(6);bridge.sendPacket(&e);assert(bridge._tx_count==2);
 complete(bridge);assert(sent.size()==4&&sent[3][0]==6&&bridge._tx_count==1);
 complete(bridge);bridge.end();assert(bridge._tx_count==0);
 // Diagnostic probes are never repeated.
 bridge.begin();sent.clear();config.count=3;
 assert(bridge.sendTest(0x1234));bridge.loop();complete(bridge);assert(bridge._tx_count==0);
 for(unsigned i=0;i<20;++i){now_ms+=10;bridge.loop();}
 assert(sent.size()==1);
 // OTA firmware frames are never repeated.
 { auto ota=packet(0x30);assert(ota.getPayloadType()==PAYLOAD_TYPE_OTA);
   bridge.sendPacket(&ota);bridge.loop();complete(bridge);assert(bridge._tx_count==0);
   for(unsigned i=0;i<20;++i){now_ms+=10;bridge.loop();}
   assert(sent.size()==2); }
 // A missing copy callback restarts the session and drops owed copies.
 auto f=packet(7);bridge.sendPacket(&f);bridge.loop();complete(bridge);
 now_ms+=30;bridge.loop();assert(sent.size()==4&&bridge._tx_waiting);
 now_ms+=1000;bridge.loop();assert(bridge._initialized&&bridge._tx_count==0);
 for(unsigned i=0;i<20;++i){now_ms+=10;bridge.loop();}
 assert(sent.size()==4);
 bridge.end();
 // Acknowledged unicast: every copy goes to the same peer; a failed copy is
 // not retried, but later copies still go out.
 const uint8_t peer[]={2,16,32,48,64,80};memcpy(mesh::bridge::configured_peer,peer,6);
 { ESPNowBridge unicast(&prefs,&mgr,&rtc);unicast.begin();sent.clear();sent_peers.clear();
   config.count=3;now_ms=9000;auto g=packet(8);unicast.sendPacket(&g);unicast.loop();
   complete(unicast);now_ms=9030;unicast.loop();assert(sent.size()==2);
   complete(unicast,ESP_NOW_SEND_FAIL);assert(sent.size()==2);
   now_ms=9059;unicast.loop();assert(sent.size()==2);
   now_ms=9060;unicast.loop();assert(sent.size()==3);complete(unicast);
   assert(unicast._tx_count==0&&unicast._stats.tx.repeats==2&&unicast._stats.tx.callback_fail==0);
   for(auto& peer_used:sent_peers)assert(!memcmp(peer_used.data(),peer,6));
   unicast.end(); }
 config.count=1;
}
'''


class EspNowRepeatBridgeTests(unittest.TestCase):
    def test_production_bridge_repeats(self):
        from test_bridge_recovery import code, COMMON
        source = code() + COMMON + BRIDGE_CHECKS
        for version in (40400, 50200, 50500):
            with self.subTest(idf=version):
                build_and_run(self, source, strict=False, defines=[
                    "ESP32=1", "WITH_ESPNOW_BRIDGE=1", "WITH_RS232_BRIDGE=1",
                    f"ESP_IDF_VERSION={version}", f"ESP_IDF_VERSION_MAJOR={version // 10000}"])


class EspNowRepeatWiringTests(unittest.TestCase):
    def test_repeater_roles_load_and_dispatch_through_common_cli(self):
        from test_radio_receive_contract import method
        common = (ROOT / "src/helpers/CommonCLI.cpp").read_text()
        load = method(common, "void CommonCLI::loadPrefs(FILESYSTEM* fs)")
        self.assertIn("#if defined(MESH_ESPNOW_REPEAT_SETTINGS)\n  mesh::espnow::loadEspNowRepeatSettings(fs);", load)
        handle = method(common, "void CommonCLI::handleCommand(uint32_t sender_timestamp, char* command, char* reply)")
        repeat = handle.index("mesh::espnow::handleEspNowRepeatCommand(command, reply, 157)")
        # Before the bridge diagnostics so primary-radio stats are answered,
        # and it never shadows `get espnow.stats` when the bridge is built.
        self.assertLess(repeat, handle.index("handleEspNowDiagnosticCommand(sender_timestamp"))
        header = (ROOT / "src/helpers/ESPNowRepeatSettings.h").read_text()
        self.assertIn("#if defined(WITH_ESPNOW_BRIDGE) || (defined(MESH_PRIMARY_ESPNOW) && MESH_PRIMARY_ESPNOW)", header)

    def test_companion_head_loads_settings_and_accepts_local_commands_only(self):
        from test_radio_receive_contract import method
        source = (ROOT / "examples/companion_radio/MyMesh.cpp").read_text()
        self.assertIn("mesh::espnow::loadEspNowRepeatSettings(_store->getPrimaryFS());", source)
        handle = method(source, "bool MyMesh::handleCommand(const char* command, uint32_t sender_timestamp,")
        self.assertIn("if (sender_timestamp == 0\n      && mesh::espnow::handleEspNowRepeatCommand(command, reply, reply_capacity)) return true;", handle)
        self.assertEqual(source.count("handleEspNowRepeatCommand("), 1)
        local = method(source, "bool MyMesh::handleLocalControlCommand(const char* command, char* reply,")
        self.assertNotIn("Repeat", local)

    def test_default_repeat_keeps_todays_send_path(self):
        from test_radio_receive_contract import method
        radio = (ROOT / "src/helpers/esp32/ESPNOWRadio.cpp").read_text()
        start = method(radio, "bool ESPNOWRadio::startSendRaw(const uint8_t* bytes, int len)")
        # With repeat=1 and no copy in flight nothing is retained or deferred.
        self.assertIn("if (copies > 1) {", start)
        self.assertNotIn("deferred", start)
        self.assertLess(start.index("tx_repeat.cancel();"), start.index("esp_now_send("))


class EspNowRepeatSchedulerTests(unittest.TestCase):
    def test_scheduler_bounds_timing_cancel_and_back_to_back(self):
        build_and_run(self, SCHEDULER_CHECKS)

    def test_scheduler_header_is_arduino_free(self):
        header = (ROOT / "src/helpers/esp32/EspNowRepeatScheduler.h").read_text()
        for forbidden in ("Arduino.h", "esp_now", "millis(", "WiFi"):
            self.assertNotIn(forbidden, header)

    def test_repeated_fragments_are_identical_and_reassembled_once(self):
        build_and_run(self, REASSEMBLY_CHECKS)


if __name__ == "__main__":
    unittest.main()
