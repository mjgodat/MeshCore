"""ESP32 station join next to the ESP-NOW bridge (StationJoinDriver.h, ESPNowBridge.cpp).

Root cause of the 2026-11-03 M5 panic: the bridge started the WiFi driver
through ESP-IDF (no netif); Arduino's WiFi.mode(WIFI_STA) then created the
default STA netif after STA_START had passed, so the netif was never started
(no lwIP input; "dhcp client start failed"; null call in esp_netif_receive).

Fix: in UDP builds the bridge starts the driver through Arduino's facade (as
the heads do), so the netif exists before esp_wifi_start; the station driver
refuses a driver started without the facade instead of creating a late netif.

The fakes model Arduino 2.0.17 / IDF 4.4 closely enough to catch the earlier
attempts: events are queued and delivered in order by an event task, the
default STA netif has no AUTOUP (it is "up" only after association), the
IDF default STA_START handler starts the netif (a second start of a started
netif is a fault), and Arduino's own handler sets STA_STARTED_BIT.
"""
from pathlib import Path
import re
import tempfile
import unittest

from test_espnow_repeat import build_and_run
from test_radio_receive_contract import method

ROOT = Path(__file__).resolve().parents[1]

FAKES = {
    'WiFi.h': r'''
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include "esp_wifi.h"
enum wl_status_t { WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_CONNECTED = 3, WL_CONNECT_FAILED = 4 };
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_OFF WIFI_MODE_NULL
#define WIFI_STA WIFI_MODE_STA
struct IPAddress { uint32_t v = 0; operator uint32_t() const { return v; } };
struct FakeArduinoWiFi {
  wifi_mode_t getMode();
  bool mode(wifi_mode_t m);
  void persistent(bool) {}
  wifi_ps_type_t sleep_cache = WIFI_PS_MIN_MODEM;
  bool setSleep(wifi_ps_type_t m) { sleep_cache = m; return true; }
  wl_status_t status();
  IPAddress localIP() { IPAddress a; return a; }
  int32_t channel() { return 6; }
  int16_t scanComplete() { return -2; }
  int16_t scanNetworks(bool, bool, bool, uint32_t, uint8_t, const char*, const uint8_t*);
  void scanDelete() {}
  std::string SSID(int) { return ""; }
  int32_t RSSI(int) { return 0; }
  int32_t channel(int) { return 0; }
  const uint8_t* BSSID(int) { return nullptr; }
  bool disconnect(bool, bool) { return true; }
};
extern FakeArduinoWiFi WiFi;
''',
    'esp_wifi.h': r'''
#pragma once
#include <cstdint>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_WIFI_NOT_INIT 0x3001
typedef enum { WIFI_MODE_NULL = 0, WIFI_MODE_STA = 1, WIFI_MODE_AP = 2, WIFI_MODE_APSTA = 3 } wifi_mode_t;
typedef enum { WIFI_PS_NONE = 0, WIFI_PS_MIN_MODEM = 1, WIFI_PS_MAX_MODEM = 2 } wifi_ps_type_t;
typedef enum { WIFI_SECOND_CHAN_NONE = 0 } wifi_second_chan_t;
typedef enum { WIFI_IF_STA = 0 } wifi_interface_t;
esp_err_t esp_wifi_get_mode(wifi_mode_t* m);
esp_err_t esp_wifi_get_channel(uint8_t* c, wifi_second_chan_t* s);
esp_err_t esp_wifi_set_channel(uint8_t c, wifi_second_chan_t s);
''',
    'helpers/esp32/WiFiRadioPolicy.h': r'''
#pragma once
#include <atomic>
#include "esp_wifi.h"
namespace mesh { namespace wifi {
std::atomic<uint8_t>& bridgeEspNowChannel();
bool espNowChannelConstrained();
uint8_t activeEspNowChannel();
bool stationMutationAllowed();
esp_err_t applyProtocolMask(wifi_interface_t);
esp_err_t restoreEspNowChannel();
}}
''',
    'helpers/esp32/WiFiStationPolicy.h': r'''
#pragma once
#include <WiFi.h>
namespace mesh { namespace wifi {
void setStationAutoReconnect(bool);
void beginStationDiagnostics(const char*);
wl_status_t beginStationWithSecurity(const char*, const char*, int32_t, const uint8_t*);
}}
''',
    'helpers/esp32/WiFiStationPower.h': r'''
#pragma once
#include "esp_wifi.h"
namespace mesh { namespace wifi { esp_err_t applyStationPowerSave(wifi_ps_type_t); }}
''',
}

CHECKS = r'''
#define ESP32 1
#include <cassert>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>
#include <helpers/esp32/StationJoinDriver.h>

// ---------------------------------------------------------------- the radio
enum Event { STA_START, STA_STOP, STA_CONNECTED };
static std::deque<Event> queue;          // the default event loop (FIFO)
static std::vector<std::string> log_;
static bool drv_init = false, drv_started = false, associated = false;
static wifi_mode_t drv_mode = WIFI_MODE_NULL;
static bool netif = false, netif_started = false, netif_up = false;   // no AUTOUP
static int faults = 0;                   // double start, DHCP on a stopped netif
static bool sta_started_bit = false;     // Arduino STA_STARTED_BIT
static bool ard_low_level = false, ard_started = false;   // Arduino caches
static uint8_t radio_channel = 6;
static int posts_by_driver_code = 0;
FakeArduinoWiFi WiFi;
static void L(const std::string& s) { log_.push_back(s); }

static void deliver(Event e) {
  switch (e) {
    case STA_START:
      if (netif) {   // IDF default handler: esp_netif_action_start
        if (netif_started) { ++faults; L("FAULT netif started twice"); }
        netif_started = true;
      }
      sta_started_bit = true;   // Arduino handler
      break;
    case STA_STOP:
      if (netif) { netif_started = false; netif_up = false; }
      sta_started_bit = false;
      break;
    case STA_CONNECTED:
      if (!netif_started) { ++faults; L("FAULT dhcp client start failed"); }
      else { netif_up = true; L("dhcp"); }
      break;
  }
}
static void pump() { while (!queue.empty()) { Event e = queue.front(); queue.pop_front(); deliver(e); } }

// ESP-IDF
static esp_err_t idf_init() { L("esp_wifi_init"); drv_init = true; return ESP_OK; }
static esp_err_t idf_start() {
  L("esp_wifi_start");
  if (!drv_started) { drv_started = true; queue.push_back(STA_START); }
  return ESP_OK;
}
static esp_err_t idf_stop() { L("esp_wifi_stop"); if (drv_started) { drv_started = false; queue.push_back(STA_STOP); } return ESP_OK; }
static esp_err_t idf_deinit() { L("esp_wifi_deinit"); drv_init = false; return ESP_OK; }
esp_err_t esp_wifi_get_mode(wifi_mode_t* m) { if (!drv_init) return ESP_ERR_WIFI_NOT_INIT; *m = drv_mode; return ESP_OK; }
esp_err_t esp_wifi_get_channel(uint8_t* c, wifi_second_chan_t*) { *c = radio_channel; return ESP_OK; }
esp_err_t esp_wifi_set_channel(uint8_t c, wifi_second_chan_t) { radio_channel = c; return ESP_OK; }
static wifi_ps_type_t applied_ps = WIFI_PS_MIN_MODEM;

// Arduino 2.0.17 WiFiGenericClass::getMode()/mode()
wifi_mode_t FakeArduinoWiFi::getMode() {
  if (!ard_low_level || !ard_started) return WIFI_MODE_NULL;
  wifi_mode_t m; return esp_wifi_get_mode(&m) == ESP_OK ? m : WIFI_MODE_NULL;
}
bool FakeArduinoWiFi::mode(wifi_mode_t m) {
  const wifi_mode_t cm = getMode();
  if (cm == m) return true;
  if (!cm && m) {
    if (!ard_low_level) {
      ard_low_level = true;
      if (!netif) { netif = true; L("create_default_sta_netif"); }
      idf_init();
    }
  } else if (cm && !m) {   // espWiFiStop + low-level deinit
    ard_started = false; idf_stop(); ard_low_level = false; idf_deinit(); return true;
  }
  drv_mode = m; L("esp_wifi_set_mode");
  if (!ard_started) { ard_started = true; idf_start(); }
  return true;
}
wl_status_t FakeArduinoWiFi::status() { return associated && netif_up ? WL_CONNECTED : WL_IDLE_STATUS; }
int16_t FakeArduinoWiFi::scanNetworks(bool, bool, bool, uint32_t, uint8_t, const char*, const uint8_t*) {
  L("scan"); return WIFI_SCAN_RUNNING;
}

// the bridge's two ways of starting the radio
static void bridge_start_raw() { idf_init(); drv_mode = WIFI_MODE_STA; idf_start(); }
static void bridge_start_arduino() { WiFi.mode(WIFI_STA); }

namespace mesh { namespace wifi {
std::atomic<uint8_t>& bridgeEspNowChannel() { static std::atomic<uint8_t> c{0}; return c; }
bool espNowChannelConstrained() { return true; }
uint8_t activeEspNowChannel() { return 6; }
bool stationMutationAllowed() { return true; }
esp_err_t applyProtocolMask(wifi_interface_t) { return ESP_OK; }
esp_err_t restoreEspNowChannel() { return ESP_OK; }
void setStationAutoReconnect(bool) {}
void beginStationDiagnostics(const char*) {}
wl_status_t beginStationWithSecurity(const char*, const char*, int32_t, const uint8_t*) {
  L("join"); associated = true; queue.push_back(STA_CONNECTED); return WL_IDLE_STATUS;
}
esp_err_t applyStationPowerSave(wifi_ps_type_t m) { applied_ps = m; return ESP_OK; }
}}

static void reset_all() {
  queue.clear(); log_.clear(); drv_init = drv_started = associated = false; drv_mode = WIFI_MODE_NULL;
  netif = netif_started = netif_up = false; faults = 0; sta_started_bit = false;
  ard_low_level = ard_started = false; radio_channel = 6; posts_by_driver_code = 0;
}
static int count(const char* what) { int n = 0; for (auto& s : log_) n += s == what; return n; }

int main() {
  uint8_t bssid[6] = {1, 2, 3, 4, 5, 6};
  // 1) UDP build: the bridge starts the radio through Arduino. The netif
  //    exists before esp_wifi_start and is started by the queued STA_START.
  reset_all();
  bridge_start_arduino();
  pump();
  assert(netif && netif_started && sta_started_bit && !netif_up);   // no AUTOUP: not up yet
  {
    mesh::wifi::StationJoinDriver d;
    assert(d.startScan(6, "net"));            // prepare: facade already STA -> nothing to do
    assert(d.begin("net", "pw", 6, bssid));
    pump();
    assert(faults == 0 && count("dhcp") == 1 && netif_up);
    assert(count("esp_wifi_init") == 1 && count("esp_wifi_start") == 1);
    assert(count("esp_wifi_stop") == 0 && count("esp_wifi_deinit") == 0);
  }
  // 2) A driver started through ESP-IDF alone (non-UDP bridge image): the
  //    station driver refuses it; no late netif, no event replay, no join.
  reset_all();
  bridge_start_raw();
  pump();
  {
    mesh::wifi::StationJoinDriver d;
    const size_t queued_before = queue.size();
    assert(!d.startScan(6, "net"));
    assert(!d.begin("net", "pw", 6, bssid));
    assert(!netif && queue.size() == queued_before && count("join") == 0 && faults == 0);
    assert(count("esp_wifi_stop") == 0 && count("esp_wifi_deinit") == 0 && count("esp_wifi_init") == 1);
  }
  // 3) The facade started WiFi earlier (WebConfig/OTA), stopped it, and the
  //    radio was then restarted through ESP-IDF: the netif exists and was
  //    started by the raw STA_START. A join must neither start it twice nor
  //    join on a radio Arduino does not own.
  reset_all();
  WiFi.mode(WIFI_STA); pump();
  WiFi.mode(WIFI_OFF); pump();
  bridge_start_raw(); pump();
  assert(netif && netif_started && !netif_up);
  {
    mesh::wifi::StationJoinDriver d;
    (void)d.startScan(6, "net"); pump();
    (void)d.begin("net", "pw", 6, bssid); pump();
    assert(faults == 0);
  }
  // 4b) power save: the station's own preference only while no ESP-NOW
  //     bridge is enabled. Boot with UDP saved on loads that preference
  //     before the bridge runs; the join must still ask for no sleep when
  //     the bridge is running or merely enabled (pinned, mid channel change).
  reset_all();
  bridge_start_arduino(); pump();
  {
    mesh::wifi::StationJoinDriver d; d.power_save = WIFI_PS_MIN_MODEM;
    assert(d.begin("net", "pw", 6, bssid));
    assert(applied_ps == WIFI_PS_MIN_MODEM && WiFi.sleep_cache == WIFI_PS_MIN_MODEM);   // no bridge
    mesh::wifi::bridgeEspNowChannel() = 6;                                           // bridge running
    assert(d.begin("net", "pw", 6, bssid));
    assert(applied_ps == WIFI_PS_NONE && WiFi.sleep_cache == WIFI_PS_NONE);
    mesh::wifi::bridgeEspNowChannel() = 0; d.pinned_channel = 6;                     // enabled, restarting
    assert(d.begin("net", "pw", 6, bssid));
    assert(applied_ps == WIFI_PS_NONE && WiFi.sleep_cache == WIFI_PS_NONE);
    d.pinned_channel = 0;
  }
  // 4) pinned channel (repeater): the configured bridge channel wins while
  //    the bridge is momentarily stopped; restore puts the radio there.
  reset_all();
  {
    mesh::wifi::StationJoinDriver r; r.pinned_channel = 2; radio_channel = 11;
    assert(r.espNowChannel() == 2);
    r.restoreChannel(); assert(radio_channel == 2);
  }
  return 0;
}
'''


class StationJoinDriverTests(unittest.TestCase):
    def test_station_netif_is_started_once_and_before_dhcp(self):
        with tempfile.TemporaryDirectory(prefix="station-join-driver-") as tmp:
            folder = Path(tmp)
            for name, text in FAKES.items():
                path = folder / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            build_and_run(self, CHECKS, includes=[folder], strict=False)

    def test_join_path_has_no_radio_restart_and_no_event_replay(self):
        driver = (ROOT / "src/helpers/esp32/StationJoinDriver.h").read_text()
        for call in ("esp_wifi_stop(", "esp_wifi_deinit(", "esp_wifi_init(", "esp_now_deinit(",
                     "WiFi.mode(WIFI_OFF", "esp_event_post(", "esp_netif_is_netif_up("):
            self.assertNotIn(call, driver)
        begin = driver[driver.index("bool begin("):]
        self.assertLess(begin.index("prepare()"), begin.index("beginStationWithSecurity("))

    def test_udp_bridge_starts_the_radio_through_arduino(self):
        source = (ROOT / "src/helpers/bridges/ESPNowBridge.cpp").read_text()
        begin = method(source, "void ESPNowBridge::begin()")
        udp = begin[begin.index("#if defined(WITH_UDP_BRIDGE)\n    // UDP builds"):]
        udp_branch, raw_branch = udp.split("#else", 1)
        raw_branch = raw_branch[:raw_branch.index("#endif")]
        self.assertIn("WiFi.mode(WIFI_STA)", udp_branch)
        for call in ("esp_wifi_init(", "esp_wifi_start(", "esp_wifi_set_mode("):
            self.assertNotIn(call, udp_branch)
            self.assertIn(call, raw_branch)   # non-UDP images unchanged


if __name__ == "__main__":
    unittest.main()
