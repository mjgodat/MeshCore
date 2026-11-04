"""The ESP-NOW bridge keeps the radio awake (WIFI_PS_NONE) in every order of events.

2026-11-03 hardware finding: after a reboot with UDP saved on and the router
not on the bridge channel, the UDP station had already chosen modem sleep
(power save picked before the bridge ran) and the bridge, starting on the
shared station, kept it: `ps=1 awake=0` until the next bridge restart.

These checks run the real ESPNowBridge.cpp and the real applyStationPowerSave
(WiFiStationPower.h) on the SDK/Arduino model of test_bridge_recovery (the
Arduino 2.0.17 cached-sleep and STA_START code is copied verbatim there).
Every ordering ends with the radio awake while the bridge runs, and the
station owner's own preference applies again once the bridge stops.
"""
from pathlib import Path
import unittest

from test_bridge_recovery import BridgeRecoveryTests

ROOT = Path(__file__).resolve().parents[1]


def station_power():
    source = (ROOT / "src/helpers/esp32/WiFiStationPower.h").read_text()
    return source[source.index("namespace mesh {"):]


PRELUDE = r'''
using wifi_power_t = int;
constexpr int WIFI_POWER_11dBm = 44, WIFI_POWER_19_5dBm = 78;
''' + station_power() + r'''
// A station owner (MQTT, WebConfig, the UDP station join) applying its
// configured modem-sleep preference the way all of them do.
static void owner_applies(wifi_ps_type_t mode) {
  (void)WiFi.setSleep(mode);
  (void)mesh::wifi::applyStationPowerSave(mode);
}
static void shared_station_up() {
  static Backend backend; backend.wifi = true; mesh::wireless::control().begin(backend);
  sdk_initialized = sdk_started = true; sdk_mode = WIFI_STA;
}
static void run_loops(ESPNowBridge& bridge, uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 10) { now_ms += 10; bridge.loop(); }
}
'''


class BridgeAwakeTests(unittest.TestCase):
    run_fixture = BridgeRecoveryTests.run_fixture

    def scenario(self, body):
        self.run_fixture(PRELUDE + "int main(){\n NodePrefs prefs;mesh::PacketManager mgr;mesh::RTCClock rtc;\n"
                         + body + "\n return 0;\n}\n")

    def test_station_first_then_bridge_on_the_shared_station(self):
        # Boot with UDP on: the station (or MQTT/WebConfig) chose modem sleep
        # before the bridge started on its radio.
        self.scenario(r'''
 shared_station_up(); owner_applies(WIFI_PS_MIN_MODEM);
 assert(wifi_ps == WIFI_PS_MIN_MODEM);
 ESPNowBridge bridge(&prefs,&mgr,&rtc); bridge.begin();
 assert(bridge._initialized);
 assert(wifi_ps == WIFI_PS_NONE);                 // awake at once
 assert(WiFi.getSleep() == WIFI_PS_NONE);         // and Arduino's cache too
 bridge.end();''')

    def test_bridge_first_then_any_station_owner(self):
        # Boot with UDP off, enabled later; router present, absent or on
        # another channel: whatever the owner asks, the radio stays awake.
        self.scenario(r'''
 ESPNowBridge bridge(&prefs,&mgr,&rtc); bridge.begin();
 assert(bridge._initialized && wifi_ps == WIFI_PS_NONE);
 for (wifi_ps_type_t mode : {WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM, WIFI_PS_MIN_MODEM}) {
   owner_applies(mode);                           // join, drop, rejoin, UDP toggles
   run_loops(bridge, 260);
   assert(wifi_ps == WIFI_PS_NONE);
 }
 // the gate itself: nothing waits for the watch
 assert(mesh::wifi::applyStationPowerSave(WIFI_PS_MIN_MODEM) == ESP_OK && wifi_ps == WIFI_PS_NONE);
 bridge.end();''')

    def test_sleep_set_behind_the_bridge_is_undone_within_the_watch_period(self):
        # A raw esp_wifi_set_ps or Arduino replaying its cached preference
        # on STA_START (a station reconnect) cannot keep modem sleep on.
        self.scenario(r'''
 shared_station_up();
 ESPNowBridge bridge(&prefs,&mgr,&rtc); bridge.begin();
 assert(wifi_ps == WIFI_PS_NONE);
 esp_wifi_set_ps(WIFI_PS_MIN_MODEM);              // behind the bridge's back
 run_loops(bridge, ESPNowBridge::kAwakeCheckMs + 20);
 assert(wifi_ps == WIFI_PS_NONE && bridge._awake_repairs == 1);
 // Arduino cache set behind the bridge, then a STA_START replays it
 WiFi._sleepEnabled = WIFI_PS_MAX_MODEM; WiFi.deliverStart();
 assert(wifi_ps == WIFI_PS_MAX_MODEM);
 run_loops(bridge, ESPNowBridge::kAwakeCheckMs + 20);
 assert(wifi_ps == WIFI_PS_NONE && WiFi.getSleep() == WIFI_PS_NONE && bridge._awake_repairs == 2);
 WiFi.deliverStart();                             // the next STA_START keeps it awake
 assert(wifi_ps == WIFI_PS_NONE);
 run_loops(bridge, 1000);
 assert(bridge._awake_repairs == 2);              // nothing to repair when nothing changed
 bridge.end();''')

    def test_refused_awake_backs_off_and_is_counted(self):
        # L4: when the driver refuses WIFI_PS_NONE (Bluetooth coexistence)
        # the watch backs off 500 ms .. 10 s instead of retrying every 250 ms.
        self.scenario(r'''
 shared_station_up();
 ESPNowBridge bridge(&prefs,&mgr,&rtc); bridge.begin();
 wifi_ps = WIFI_PS_MIN_MODEM; failure = 19;       // esp_wifi_set_ps now refused
 const unsigned writes = ps_writes;
 run_loops(bridge, 60000);
 const unsigned attempts = ps_writes - writes;
 assert(bridge._awake_refusals >= 6 && bridge._awake_refusals <= 12);   // not 240
 assert(attempts <= 2 * bridge._awake_refusals + 2);
 assert(bridge._awake_backoff_ms == 10000);
 char reply[160]; bridge.formatStats("awake", reply, sizeof(reply));
 assert(strstr(reply, "refused=") && strstr(reply, "backoff_ms=10000"));
 failure = 0;                                     // accepted again: back to 250 ms
 run_loops(bridge, 10500);
 assert(wifi_ps == WIFI_PS_NONE && bridge._awake_backoff_ms == 0);
 bridge.end();''')

    def test_powersave_replies_while_the_bridge_runs(self):
        # L5: the saved setting and what the radio does differ while the bridge runs.
        self.scenario(r'''
 char r[160];
 strcpy(r, "OK - WiFi power save saved; reboot to apply");
 mesh::wifi::annotatePowerSaveReply(r, sizeof(r), true, true);
 assert(!strcmp(r, "OK - saved (radio kept awake while the ESP-NOW bridge runs)"));
 strcpy(r, "Error: power save must be none, min, or max");
 mesh::wifi::annotatePowerSaveReply(r, sizeof(r), true, true); assert(!strncmp(r, "Error", 5));
 strcpy(r, "OK - WiFi power save saved; reboot to apply");
 mesh::wifi::annotatePowerSaveReply(r, sizeof(r), false, true); assert(strstr(r, "reboot to apply"));
 strcpy(r, "> min"); mesh::wifi::annotatePowerSaveReply(r, sizeof(r), true, false);
 assert(!strcmp(r, "> min (saved; effective none while the ESP-NOW bridge runs)"));
 strcpy(r, "> none"); mesh::wifi::annotatePowerSaveReply(r, sizeof(r), true, false); assert(!strcmp(r, "> none"));
 strcpy(r, "> min"); mesh::wifi::annotatePowerSaveReply(r, sizeof(r), false, false); assert(!strcmp(r, "> min"));
 char tiny[8] = "> min"; mesh::wifi::annotatePowerSaveReply(tiny, sizeof(tiny), true, false);
 assert(strlen(tiny) < sizeof(tiny));''')

    def test_bridge_restart_and_release(self):
        # Bridge restarted (channel change) while the station owner keeps
        # asking for sleep; once the bridge stops the owner's choice applies.
        self.scenario(r'''
 shared_station_up(); owner_applies(WIFI_PS_MIN_MODEM);
 ESPNowBridge bridge(&prefs,&mgr,&rtc);
 for (int i = 0; i < 3; ++i) {
   bridge.begin(); assert(bridge._initialized && wifi_ps == WIFI_PS_NONE);
   owner_applies(WIFI_PS_MIN_MODEM); assert(wifi_ps == WIFI_PS_NONE);
   bridge.end();
   // L3: the owner's preference is back at once, cache included
   assert(wifi_ps == WIFI_PS_MIN_MODEM && WiFi.getSleep() == WIFI_PS_MIN_MODEM);
 }
 // an owner that changed its mind while the bridge ran gets the newer choice
 bridge.begin(); owner_applies(WIFI_PS_MAX_MODEM); assert(wifi_ps == WIFI_PS_NONE);
 bridge.end(); assert(wifi_ps == WIFI_PS_MAX_MODEM && WiFi.getSleep() == WIFI_PS_MAX_MODEM);
 // an owner that wants no sleep keeps none
 owner_applies(WIFI_PS_NONE); bridge.begin(); bridge.end(); assert(wifi_ps == WIFI_PS_NONE);''')


if __name__ == "__main__":
    unittest.main()
