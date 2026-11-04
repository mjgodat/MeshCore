"""ESP32 station join on a radio the ESP-NOW bridge already started (StationJoinDriver.h).

Root cause of the 2026-11-03 M5 panic: the bridge starts the WiFi driver via
ESP-IDF (no netif); Arduino's WiFi.mode(WIFI_STA) then creates the default
STA netif on the already started driver, whose STA_START event has passed,
so the netif is never started and DHCP / lwIP input fail. The fakes below
model exactly that Arduino 2.0.17 / IDF 4.4 behaviour and check that the
driver brings the station up on the existing radio init, once: the netif is
started before any join/DHCP, and the radio is never stopped, de-initialised
or restarted.
"""
from pathlib import Path
import tempfile
import unittest

from test_espnow_repeat import build_and_run

ROOT = Path(__file__).resolve().parents[1]

FAKES = {
    'WiFi.h': r'''
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "esp_wifi.h"
#include "esp_netif.h"
enum wl_status_t { WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_CONNECTED = 3, WL_CONNECT_FAILED = 4 };
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_OFF WIFI_MODE_NULL
#define WIFI_STA WIFI_MODE_STA
struct IPAddress { uint32_t v = 0; operator uint32_t() const { return v; } };
struct FakeArduinoWiFi {
  bool low_level = false, started_flag = false;   // Arduino's private caches
  int mode_calls = 0;
  wl_status_t status_v = WL_IDLE_STATUS;
  uint32_t ip = 0;
  wifi_mode_t getMode();
  bool mode(wifi_mode_t m);
  void persistent(bool) {}
  bool setSleep(wifi_ps_type_t) { return true; }
  wl_status_t status() { return status_v; }
  IPAddress localIP() { IPAddress a; a.v = ip; return a; }
  int32_t channel() { return 6; }
  int16_t scanComplete() { return -2; }
  int16_t scanNetworks(bool, bool, bool, uint32_t, uint8_t, const char*, const uint8_t*) { return WIFI_SCAN_RUNNING; }
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
    'esp_netif.h': r'''
#pragma once
struct esp_netif_obj;
typedef struct esp_netif_obj esp_netif_t;
esp_netif_t* esp_netif_get_handle_from_ifkey(const char* key);
bool esp_netif_is_netif_up(esp_netif_t* n);
''',
    'esp_event.h': r'''
#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_wifi.h"
typedef const char* esp_event_base_t;
extern esp_event_base_t WIFI_EVENT;
enum { WIFI_EVENT_STA_START = 2 };
esp_err_t esp_event_post(esp_event_base_t base, int32_t id, const void* data, size_t size, uint32_t ticks);
''',
    'freertos/FreeRTOS.h': '#pragma once\n#include <cstdint>\n#define pdMS_TO_TICKS(x) (x)\n',
    'freertos/task.h': '#pragma once\nvoid vTaskDelay(uint32_t);\n',
    'helpers/esp32/WiFiRadioPolicy.h': r'''
#pragma once
#include "esp_wifi.h"
namespace mesh { namespace wifi {
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
#include <string>
#include <vector>
#include <helpers/esp32/StationJoinDriver.h>

// ---- the radio: IDF driver + netif + event loop, as on the M5 ------------
static std::vector<std::string> calls;
static bool driver_init = false, driver_started = false;
static wifi_mode_t driver_mode = WIFI_MODE_NULL;
static bool netif_created = false, netif_started = false;
static uint8_t radio_channel = 6;
esp_event_base_t WIFI_EVENT = "WIFI_EVENT";
FakeArduinoWiFi WiFi;
static void ev(const std::string& c) { calls.push_back(c); }

static esp_err_t idf_init() {   // esp_wifi_init: OK if already initialised
  ev("esp_wifi_init"); driver_init = true; return ESP_OK;
}
static void post_sta_start() { if (netif_created) netif_started = true; }   // default handler
static esp_err_t idf_start() {
  ev("esp_wifi_start");
  if (!driver_started) { driver_started = true; post_sta_start(); }   // event only on a real start
  return ESP_OK;
}
esp_err_t esp_wifi_get_mode(wifi_mode_t* m) {
  if (!driver_init) return ESP_ERR_WIFI_NOT_INIT;
  *m = driver_mode; return ESP_OK;
}
esp_err_t esp_wifi_get_channel(uint8_t* c, wifi_second_chan_t*) { *c = radio_channel; return ESP_OK; }
esp_err_t esp_wifi_set_channel(uint8_t c, wifi_second_chan_t) { ev("set_channel"); radio_channel = c; return ESP_OK; }
esp_netif_t* esp_netif_get_handle_from_ifkey(const char*) {
  return netif_created ? reinterpret_cast<esp_netif_t*>(&netif_created) : nullptr;
}
bool esp_netif_is_netif_up(esp_netif_t*) { return netif_started; }
esp_err_t esp_event_post(esp_event_base_t, int32_t id, const void*, size_t, uint32_t) {
  ev("post_sta_start");
  if (id == WIFI_EVENT_STA_START) post_sta_start();
  return ESP_OK;
}
void vTaskDelay(uint32_t) {}

// Arduino 2.0.17 WiFiGenericClass::mode()/getMode(), reduced to what matters.
wifi_mode_t FakeArduinoWiFi::getMode() {
  if (!low_level || !started_flag) return WIFI_MODE_NULL;
  return driver_mode;
}
bool FakeArduinoWiFi::mode(wifi_mode_t m) {
  ++mode_calls;
  if (getMode() == m) return true;
  if (!low_level) {                      // wifiLowLevelInit
    low_level = true;
    if (!netif_created) { netif_created = true; ev("create_default_sta_netif"); }
    if (idf_init() != ESP_OK) return false;
  }
  ev("esp_wifi_set_mode"); driver_mode = m;
  if (!started_flag) { started_flag = true; idf_start(); }   // espWiFiStart
  return true;
}

namespace mesh { namespace wifi {
bool espNowChannelConstrained() { return true; }
uint8_t activeEspNowChannel() { return 6; }
bool stationMutationAllowed() { return true; }
esp_err_t applyProtocolMask(wifi_interface_t) { return ESP_OK; }
esp_err_t restoreEspNowChannel() { return ESP_OK; }
void setStationAutoReconnect(bool) {}
void beginStationDiagnostics(const char*) {}
wl_status_t beginStationWithSecurity(const char*, const char*, int32_t, const uint8_t*) {
  ev("join");
  // Association, then DHCP on the station netif.
  if (!netif_started) { ev("dhcp client start failed"); return WL_CONNECT_FAILED; }
  ev("dhcp_start");
  return WL_IDLE_STATUS;
}
esp_err_t applyStationPowerSave(wifi_ps_type_t) { return ESP_OK; }
}}

static int index(const char* c) {
  for (size_t i = 0; i < calls.size(); ++i) if (calls[i] == c) return (int)i;
  return -1;
}
static void reset_radio() {
  calls.clear(); driver_init = driver_started = false; driver_mode = WIFI_MODE_NULL;
  netif_created = netif_started = false; WiFi = FakeArduinoWiFi(); radio_channel = 6;
}

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  // 1) The M5 case: the ESP-NOW bridge started the driver through IDF.
  reset_radio();
  driver_init = true; driver_started = true; driver_mode = WIFI_MODE_STA;
  mesh::wifi::StationJoinDriver d;
  assert(d.startScan(6, "net"));
  assert(netif_created && netif_started);                     // the late netif is started
  assert(index("post_sta_start") > index("create_default_sta_netif"));
  uint8_t bssid[6] = {1, 2, 3, 4, 5, 6};
  assert(d.begin("net", "pw", 6, bssid));
  assert(index("dhcp_start") > index("post_sta_start") && index("dhcp client start failed") < 0);
  // never stops / de-initialises / restarts the radio; STA_START replayed once
  int posts = 0; for (auto& c : calls) posts += c == "post_sta_start";
  assert(posts == 1);
  for (auto& c : calls) assert(c != "esp_wifi_stop" && c != "esp_wifi_deinit");
  // a later prepare (facade now consistent) does nothing more
  const size_t before = calls.size();
  assert(d.startScan(6, "net"));
  for (size_t i = before; i < calls.size(); ++i) assert(calls[i] != "post_sta_start" && calls[i] != "esp_wifi_init");
  // 2) Without the replay the old code path fails exactly like the M5 did.
  reset_radio();
  driver_init = true; driver_started = true; driver_mode = WIFI_MODE_STA;
  assert(WiFi.mode(WIFI_STA));
  assert(netif_created && !netif_started);
  assert(mesh::wifi::beginStationWithSecurity("net", "pw", 6, bssid) == WL_CONNECT_FAILED);
  // 3) A head / plain station: Arduino starts the driver itself; the netif
  //    starts from the real event and nothing is replayed.
  reset_radio();
  mesh::wifi::StationJoinDriver h;
  assert(h.startScan(6, "net") && netif_started && index("post_sta_start") < 0);
  assert(h.begin("net", "pw", 6, bssid) && index("dhcp_start") >= 0);
  // 4) pinned channel: the repeater's configured bridge channel wins while
  //    the bridge is momentarily stopped; restore puts the radio there.
  reset_radio();
  mesh::wifi::StationJoinDriver r; r.pinned_channel = 2; radio_channel = 11;
  assert(r.espNowChannel() == 2);
  r.restoreChannel(); assert(radio_channel == 2);
  const size_t n = calls.size(); r.restoreChannel(); assert(calls.size() == n);   // no rewrite when already there
  return 0;
}
'''


class StationJoinDriverTests(unittest.TestCase):
    def test_station_comes_up_on_the_bridge_radio_without_restart(self):
        with tempfile.TemporaryDirectory(prefix="station-join-driver-") as tmp:
            folder = Path(tmp)
            for name, text in FAKES.items():
                path = folder / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            build_and_run(self, CHECKS, includes=[folder], strict=False)

    def test_join_path_has_no_radio_restart(self):
        driver = (ROOT / "src/helpers/esp32/StationJoinDriver.h").read_text()
        for call in ("esp_wifi_stop(", "esp_wifi_deinit(", "esp_wifi_init(", "esp_now_deinit(", "WiFi.mode(WIFI_OFF"):
            self.assertNotIn(call, driver)
        prepare = driver[driver.index("bool prepare()"):driver.index("static bool startLateStationNetif()")]
        self.assertLess(prepare.index("WiFi.mode(WIFI_STA)"), prepare.index("startLateStationNetif()"))
        begin = driver[driver.index("bool begin("):]
        self.assertLess(begin.index("prepare()"), begin.index("beginStationWithSecurity("))


if __name__ == "__main__":
    unittest.main()
