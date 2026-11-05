"""Repeater lockup evidence (reliability round).

- ResetHistory.h: how the previous run ended and when it was last alive,
  exact from the RTC copy after a software/panic/watchdog reset, from the
  last NVS checkpoint after a power-on reset; boots and abnormal ends; the
  `powerlog` text fits the 160-byte reply.
- LoopWatchdog.h: subscribes loopTask with the configured timeout, pauses
  around long work and restores the subscription, caps sleeps (mocked
  ESP-IDF task-WDT API).
- Wiring (source contract): the ThinkNode M5 builds enable a 30 s loop
  watchdog; the repeater starts it last in setup(), refreshes the history
  every loop, caps and feeds around sleep, and long operations (OTA apply,
  OTA download wait, filesystem format) pause it; `powerlog` prints the
  history through a weak hook.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HISTORY = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include "helpers/ResetHistory.h"
#include "helpers/bridges/ObserverFrame.h"
using namespace mesh::resethistory;
static Record fresh() { Record r; memset(&r, 0xA5, sizeof(r)); return r; }
int main() {
  RtcCopy rtc; memset(&rtc, 0x5A, sizeof(rtc));   // RTC garbage after power-up
  char out[160];
  // first boot ever: nothing loaded, power-on
  Record r = fresh();
  onBoot(r, false, rtc, PowerOn);
  assert(r.magic == kMagic && r.boots == 1 && !r.prev_known && r.abnormal_ends == 0);
  format(r, out, sizeof(out), mesh::observer::resetReasonName);
  assert(!strcmp(out, "\r\nPrev run not recorded\r\nBoots 1, abnormal 0"));
  // checkpoints: first after 60 s, then every 10 min
  assert(!checkpointDue(59, 0, false) && checkpointDue(60, 0, false));
  assert(!checkpointDue(659, 60, true) && checkpointDue(660, 60, true));
  checkpoint(r, 7260, 1762230720u, 81234);   // 2025-11-04 04:32Z
  rtcUpdate(rtc, 7300, 1762230760u, 80000);
  assert(rtcValid(rtc));
  // a power loss (power-on reset): RTC lost, the last checkpoint is used
  Record saved = r;
  onBoot(saved, true, rtc, PowerOn);
  assert(saved.prev_known && !saved.prev_exact && saved.prev_uptime_s == 7260 && saved.prev_epoch == 1762230720u);
  assert(saved.prev_min_heap == 81234 && saved.prev_end_reason == PowerOn && saved.boots == 2 && saved.abnormal_ends == 0);
  assert(saved.alive_uptime_s == 0 && !rtcValid(rtc));   // RTC copy consumed
  format(saved, out, sizeof(out), mesh::observer::resetReasonName);
  assert(!strcmp(out, "\r\nPrev run 2h1m~, alive 2025-11-04 04:32Z, heap>=81234\r\nBoots 2, abnormal 0"));
  // a task-watchdog reset: the RTC copy survived and is exact
  checkpoint(saved, 600, 0, 90000);
  rtcUpdate(rtc, 3 * 86400 + 5 * 3600 + 7, 1762490000u, 70000);
  onBoot(saved, true, rtc, TaskWdt);
  assert(saved.prev_exact && saved.prev_uptime_s == 3 * 86400 + 5 * 3600 + 7 && saved.prev_min_heap == 70000);
  assert(saved.abnormal_ends == 1 && saved.last_abnormal_reason == TaskWdt && saved.last_abnormal_epoch == 1762490000u);
  format(saved, out, sizeof(out), mesh::observer::resetReasonName);
  assert(!strcmp(out, "\r\nPrev run 3d5h, alive 2025-11-07 04:33Z, heap>=70000\r\nBoots 3, abnormal 1 (task wdt @2025-11-07 04:33Z)"));
  // a damaged RTC copy is not trusted even after a software reset
  checkpoint(saved, 1200, 0, 60000);
  rtcUpdate(rtc, 5000, 0, 1); rtc.uptime_s ^= 1;
  onBoot(saved, true, rtc, Sw);
  assert(!saved.prev_exact && saved.prev_uptime_s == 1200 && saved.abnormal_ends == 1);
  format(saved, out, sizeof(out), mesh::observer::resetReasonName);
  assert(strstr(out, "alive clock unset"));
  // a run that died before its first checkpoint, then a power-on reset:
  // says so instead of "0m0s" (review L4)
  { Record early = saved; RtcCopy lost; memset(&lost, 0, sizeof(lost));
    onBoot(early, true, lost, PowerOn);       // alive fields were cleared at the previous boot
    format(early, out, sizeof(out), mesh::observer::resetReasonName);
    assert(strstr(out, "\r\nPrev run <1m, not checkpointed\r\nBoots ") == out); }
  // brownout and panic count; deep sleep and software resets do not
  Record b = saved;
  onBoot(b, true, rtc, Brownout); assert(b.abnormal_ends == 2 && b.last_abnormal_reason == Brownout);
  onBoot(b, true, rtc, Panic); assert(b.abnormal_ends == 3);
  onBoot(b, true, rtc, IntWdt); assert(b.abnormal_ends == 4);
  onBoot(b, true, rtc, DeepSleep); assert(b.abnormal_ends == 4);
  // a record with another magic is replaced
  Record bad = saved; bad.magic = 1;
  onBoot(bad, true, rtc, PowerOn);
  assert(bad.boots == 1 && !bad.prev_known);
  // the whole powerlog reply fits 160 bytes in the longest case
  Record w; memset(&w, 0, sizeof(w)); w.magic = kMagic; w.prev_known = 1;
  w.prev_uptime_s = 0xFFFFFFFFu; w.prev_epoch = 0xFFFFFFF0u; w.prev_min_heap = 0xFFFFFFFFu;
  w.boots = w.abnormal_ends = 0xFFFFFFFFu; w.last_abnormal_reason = 255; w.last_abnormal_epoch = 0xFFFFFFF0u;
  char reply[160];
  snprintf(reply, sizeof(reply), "Last reset reason: %s", "Unknown reset reason (255)");
  const size_t used = strlen(reply);
  const size_t n = format(w, reply + used, sizeof(reply) - used, mesh::observer::resetReasonName);
  assert(used + n < sizeof(reply) && strlen(reply) == used + n);
  printf("LONGEST %zu\n", used + n);
  // truncation stays inside the buffer
  char tiny[12]; format(w, tiny, sizeof(tiny), mesh::observer::resetReasonName); assert(strlen(tiny) == 11);
  printf("PASS history\n");
  return 0;
}
'''

WATCHDOG = r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_STATE 0x103
typedef void* TaskHandle_t;
static bool initialized = false, subscribed = false;
static uint32_t timeout = 0; static bool panic = false; static int resets = 0, adds = 0, deletes = 0;
esp_err_t esp_task_wdt_init(uint32_t t, bool p) { initialized = true; timeout = t; panic = p; return ESP_OK; }
esp_err_t esp_task_wdt_status(TaskHandle_t) { return !initialized ? ESP_ERR_INVALID_STATE : subscribed ? ESP_OK : ESP_ERR_NOT_FOUND; }
esp_err_t esp_task_wdt_add(TaskHandle_t) { ++adds; subscribed = true; return ESP_OK; }
esp_err_t esp_task_wdt_delete(TaskHandle_t) { ++deletes; if (!subscribed) return ESP_ERR_NOT_FOUND; subscribed = false; return ESP_OK; }
esp_err_t esp_task_wdt_reset() { if (!subscribed) return ESP_ERR_NOT_FOUND; ++resets; return ESP_OK; }
bool loopTaskWDTEnabled = false;
void enableLoopWDT() { if (esp_task_wdt_add(nullptr) == ESP_OK) loopTaskWDTEnabled = true; }
#define MESH_LOOP_WATCHDOG_SECS 30
#include "helpers/esp32/LoopWatchdog.h"
using namespace mesh::esp32;
int main() {
  // off: no subscription, pause and feed are no-ops, sleep uncapped
  { LoopWatchdogPause p; } assert(!subscribed && adds == 0);
  feedLoopWatchdog(); assert(resets == 0 && loopWatchdogSleepCapMs() == UINT32_MAX);
  assert(!beginLoopWatchdog(0) && !initialized);
  // on: the timeout is applied with panic, loopTask subscribed
  assert(beginLoopWatchdog(30) && initialized && timeout == 30 && panic && subscribed && loopTaskWDTEnabled);
  assert(beginLoopWatchdog(30) && adds == 1);   // idempotent
  feedLoopWatchdog(); assert(resets == 1);
  assert(loopWatchdogSleepCapMs() == 15000);
  {
    LoopWatchdogPause p; assert(!subscribed && deletes == 1);
    { LoopWatchdogPause nested; assert(!subscribed); }   // nested: no double add
    assert(!subscribed && adds == 1);
  }
  assert(subscribed && adds == 2);
  printf("PASS watchdog\n");
  return 0;
}
'''


WATCHDOG_IDF5 = r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_STATE 0x103
#define CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 1
typedef void* TaskHandle_t;
typedef struct { uint32_t timeout_ms; uint32_t idle_core_mask; bool trigger_panic; } esp_task_wdt_config_t;
static bool running = false, subscribed = false; static int inits = 0, reconfigs = 0;
static esp_task_wdt_config_t applied = {0, 0, false};
esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t* c) {
  if (running) { return ESP_ERR_INVALID_STATE; } ++inits; running = true; applied = *c; return ESP_OK; }
esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t* c) {
  if (!running) { return ESP_ERR_INVALID_STATE; } ++reconfigs; applied = *c; return ESP_OK; }
esp_err_t esp_task_wdt_status(TaskHandle_t) { return !running ? ESP_ERR_INVALID_STATE : subscribed ? ESP_OK : ESP_ERR_NOT_FOUND; }
esp_err_t esp_task_wdt_add(TaskHandle_t) { subscribed = true; return ESP_OK; }
esp_err_t esp_task_wdt_delete(TaskHandle_t) { if (!subscribed) return ESP_ERR_NOT_FOUND; subscribed = false; return ESP_OK; }
esp_err_t esp_task_wdt_reset() { return subscribed ? ESP_OK : ESP_ERR_NOT_FOUND; }
void enableLoopWDT() { esp_task_wdt_add(nullptr); }
#define MESH_LOOP_WATCHDOG_SECS 30
#include "helpers/esp32/LoopWatchdog.h"
int main() {
  // the SDK already runs the TWDT (CONFIG_ESP_TASK_WDT_INIT): reconfigured
  running = true;
  assert(mesh::esp32::applyTaskWatchdogTimeout(30) == ESP_OK && reconfigs == 1 && inits == 0);
  assert(applied.timeout_ms == 30000 && applied.trigger_panic && applied.idle_core_mask == 1);
  // not running: started with the same config
  running = false;
  assert(mesh::esp32::applyTaskWatchdogTimeout(20) == ESP_OK && inits == 1 && applied.timeout_ms == 20000);
  // the full start subscribes loopTask
  running = true; reconfigs = 0;
  assert(mesh::esp32::beginLoopWatchdog(30) && subscribed && reconfigs == 1);
  printf("PASS idf5\n");
  return 0;
}
'''


def compile_and_run(test, source, extra=()):
    compiler = shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        test.skipTest("a host C++ compiler is required")
    sanitize = [] if os.name == "nt" else ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    with tempfile.TemporaryDirectory(prefix="reset-history-") as tmp:
        folder = Path(tmp)
        (folder / "t.cpp").write_text(source)
        for name, text in extra:
            (folder / name).write_text(text)
        built = subprocess.run([compiler, "-std=c++11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *sanitize,
                                "-I", str(folder), "-I", str(ROOT / "src"), str(folder / "t.cpp"),
                                "-o", str(folder / "t")], capture_output=True, text=True, timeout=120)
        test.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        ran = subprocess.run([str(folder / "t")], capture_output=True, text=True, timeout=60)
        test.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        return ran.stdout


def text(path):
    return (ROOT / path).read_text().replace("\r\n", "\n")


class ResetHistoryTests(unittest.TestCase):
    def test_previous_run_summary(self):
        out = compile_and_run(self, HISTORY)
        self.assertIn("PASS history", out)


class LoopWatchdogTests(unittest.TestCase):
    def test_subscribe_pause_feed(self):
        stubs = (("Arduino.h", "#pragma once\nvoid enableLoopWDT();\n"),
                 ("esp_task_wdt.h", "#pragma once\n"),
                 ("esp_idf_version.h", "#pragma once\n#define ESP_IDF_VERSION_MAJOR 4\n"))
        self.assertIn("PASS watchdog", compile_and_run(self, WATCHDOG, stubs))

    def test_idf5_config_struct(self):
        # Arduino-ESP32 3.x / IDF 5 (pioarduino C6 and RC32 envs): config
        # struct, reconfigure a running TWDT, init otherwise (re-review).
        stubs = (("Arduino.h", "#pragma once\nvoid enableLoopWDT();\n"),
                 ("esp_task_wdt.h", "#pragma once\n"),
                 ("esp_idf_version.h", "#pragma once\n#define ESP_IDF_VERSION_MAJOR 5\n"))
        self.assertIn("PASS idf5", compile_and_run(self, WATCHDOG_IDF5, stubs))
        # the IDF 4 call (the M5) is unchanged
        header = text("src/helpers/esp32/LoopWatchdog.h")
        self.assertIn("return esp_task_wdt_init(timeout_s, true);", header)
        self.assertIn("#if ESP_IDF_VERSION_MAJOR >= 5", header)


class WiringTests(unittest.TestCase):
    def test_m5_repeater_builds_enable_a_30_s_loop_watchdog(self):
        ini = text("variants/thinknode_m5/platformio.ini")
        base = ini[ini.index("[ThinkNode_M5]"):ini.index("[env:ThinkNode_M5_Repeater]")]
        self.assertNotIn("MESH_LOOP_WATCHDOG_SECS", base)   # only the repeater acts on it (review L3)
        for env in ("[env:ThinkNode_M5_Repeater]", "[env:ThinkNode_M5_Repeater_bridge_espnow]"):
            section = ini[ini.index(env) + 1:]
            section = section[:section.index("\n[")]
            self.assertRegex(section, r"-D MESH_LOOP_WATCHDOG_SECS=30\b")
        udp = ini[ini.index("[env:ThinkNode_M5_Repeater_bridge_espnow_udp]") + 1:]
        self.assertIn("${env:ThinkNode_M5_Repeater_bridge_espnow.build_flags}", udp[:udp.index("\n[")])
        self.assertIn("#define MESH_LOOP_WATCHDOG_SECS 0", text("src/helpers/esp32/LoopWatchdog.h"))

    def test_repeater_main(self):
        main = text("examples/simple_repeater/main.cpp")
        setup = main[main.index("void setup() {"):main.index("\nvoid loop() {")]
        setup = setup[:setup.index("\n}\n")]
        tail = setup[setup.index("board.onBootComplete();"):]
        self.assertLess(tail.index("reset_history.begin();"), tail.index("beginLoopWatchdog(MESH_LOOP_WATCHDOG_SECS);"))
        loop = main[main.index("\nvoid loop() {"):]
        self.assertLess(loop.index("reset_history.service("), loop.index("#if MESH_ENABLE_EMERGENCY_POWER"))
        self.assertIn("esp_timer_get_time() / 1000000LL", loop)
        sleep = loop[loop.index("if (can_power_save) {"):]
        cap = sleep.index("loopWatchdogSleepCapMs()")
        self.assertLess(cap, sleep.index("feedLoopWatchdog();"))
        self.assertLess(sleep.index("feedLoopWatchdog();"), sleep.index("board.sleepMilliseconds(sleep_ms);"))
        self.assertIn("size_t meshFormatResetHistory(char* out, size_t cap) { return reset_history.format(out, cap); }", main)

    def test_long_operations_pause_the_watchdog(self):
        apply = text("src/helpers/ota/OtaApply.cpp")
        body = apply[apply.index("bool ota_apply_detools_mota(OtaStoreFlashEsp32& store"):]
        self.assertIn("mesh::esp32::LoopWatchdogPause watchdog_pause;", body[:body.index("st = ApplyState();")])
        board = text("src/helpers/ESP32Board.cpp")
        wait = board[board.index("bool ESP32Board::otaFromManifest("):]
        wait = wait[wait.index("while (!args.done)"):]
        # review M1: fed while waiting and bounded (test_observer_ota_channel runs it)
        self.assertIn("mesh::esp32::feedLoopWatchdog();", wait[:wait.index("delay(50)")])
        self.assertNotIn("LoopWatchdogPause", board[board.index("// ota worker begin"):board.index("// ota worker end")])
        # review L1: bounded waits outside loop() feed through serviceWatchdog()
        self.assertIn("void serviceWatchdog() override;", text("src/helpers/ESP32Board.h"))
        sw = board[board.index("void ESP32Board::serviceWatchdog() {"):]
        self.assertIn("mesh::esp32::feedLoopWatchdog();", sw[:sw.index("}")])
        mesh_cpp = text("examples/simple_repeater/MyMesh.cpp")
        fmt = mesh_cpp[mesh_cpp.index("bool MyMesh::formatFileSystem() {"):]
        self.assertLess(fmt.index("LoopWatchdogPause watchdog_pause;"), fmt.index("return SPIFFS.format();"))

    def test_powerlog_prints_the_history(self):
        cli = text("src/helpers/CommonCLI.cpp")
        self.assertIn("size_t meshFormatResetHistory(char* out, size_t cap) __attribute__((weak));", cli)
        powerlog = cli[cli.index('memcmp(command, "powerlog", 8) == 0'):]
        powerlog = powerlog[:powerlog.index("#if defined(NRF52_PLATFORM)")]
        self.assertIn("if (meshFormatResetHistory) {", powerlog)
        self.assertIn("meshFormatResetHistory(reply + used, 160 - used);", powerlog)


if __name__ == "__main__":
    unittest.main()
