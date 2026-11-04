"""-Wl,--wrap=esp_wifi_set_ps: no modem sleep reaches the driver while the bridge runs.

Links a stand-in "driver" (the real esp_wifi_set_ps), a stand-in "Arduino"
object calling it (as WiFi.setSleep and the STA_START handler do), and the
real wrapper source with GNU ld's --wrap, the same mechanism the xtensa
linker uses for the M5 _udp image.
"""
from pathlib import Path
import configparser
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

FAKE_ESP_WIFI = r'''
#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
typedef enum { WIFI_PS_NONE = 0, WIFI_PS_MIN_MODEM = 1, WIFI_PS_MAX_MODEM = 2 } wifi_ps_type_t;
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_wifi_set_ps(wifi_ps_type_t type);
#ifdef __cplusplus
}
#endif
'''
FAKE_POLICY = r'''
#pragma once
#include <atomic>
#include <stdint.h>
namespace mesh { namespace wifi {
inline std::atomic<uint8_t>& bridgeEspNowChannel() { static std::atomic<uint8_t> c{0}; return c; }
}}
'''
DRIVER = r'''
#include "esp_wifi.h"
wifi_ps_type_t driver_ps = WIFI_PS_MIN_MODEM;
extern "C" esp_err_t esp_wifi_set_ps(wifi_ps_type_t type) { driver_ps = type; return ESP_OK; }
'''
ARDUINO = r'''
#include "esp_wifi.h"
// WiFiGenericClass::setSleep / the STA_START branch: a plain call.
void arduino_set_sleep(wifi_ps_type_t type) { (void)esp_wifi_set_ps(type); }
'''
MAIN = r'''
#include <cassert>
#include "esp_wifi.h"
#include <helpers/esp32/WiFiRadioPolicy.h>
extern wifi_ps_type_t driver_ps;
void arduino_set_sleep(wifi_ps_type_t type);
int main() {
  arduino_set_sleep(WIFI_PS_MIN_MODEM);            // no bridge: the owner's choice
  assert(driver_ps == WIFI_PS_MIN_MODEM);
  mesh::wifi::bridgeEspNowChannel() = 6;           // bridge running
  arduino_set_sleep(WIFI_PS_MIN_MODEM);  assert(driver_ps == WIFI_PS_NONE);
  arduino_set_sleep(WIFI_PS_MAX_MODEM);  assert(driver_ps == WIFI_PS_NONE);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);    assert(driver_ps == WIFI_PS_NONE);
  mesh::wifi::bridgeEspNowChannel() = 0;           // bridge stopped
  arduino_set_sleep(WIFI_PS_MAX_MODEM);  assert(driver_ps == WIFI_PS_MAX_MODEM);
  return 0;
}
'''


class EspNowAwakeWrapTests(unittest.TestCase):
    def test_wrapped_set_ps_never_lets_the_radio_doze_under_the_bridge(self):
        cxx = shutil.which("g++")
        if not cxx:
            self.skipTest("g++ required")
        with tempfile.TemporaryDirectory(prefix="awake-wrap-") as tmp:
            d = Path(tmp)
            (d / "esp_wifi.h").write_text(FAKE_ESP_WIFI)
            (d / "helpers/esp32").mkdir(parents=True)
            (d / "helpers/esp32/WiFiRadioPolicy.h").write_text(FAKE_POLICY)
            for name, text in (("driver.cpp", DRIVER), ("arduino.cpp", ARDUINO), ("main.cpp", MAIN)):
                (d / name).write_text(text)
            objs = []
            sources = [d / "driver.cpp", d / "arduino.cpp", d / "main.cpp",
                       ROOT / "src/helpers/esp32/EspNowAwakeWrap.cpp"]
            for src in sources:
                obj = d / (src.stem + ".o")
                r = subprocess.run([cxx, "-std=c++17", "-c", "-DESP32=1", "-DMESH_WRAP_ESP_WIFI_SET_PS=1",
                                    "-I", str(d), "-I", str(ROOT / "src"), str(src), "-o", str(obj)],
                                   capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)
                objs.append(str(obj))
            # the driver lives in an archive, as libnet80211/libesp_wifi do
            r = subprocess.run(["ar", "rcs", str(d / "libdriver.a"), objs[0]], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            exe = d / "t"
            r = subprocess.run([cxx, "-Wl,--wrap=esp_wifi_set_ps", *objs[1:], str(d / "libdriver.a"),
                                "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_udp_env_links_the_wrapper(self):
        ini = configparser.ConfigParser(strict=False, interpolation=None)
        ini.read(ROOT / "variants/thinknode_m5/platformio.ini")
        env = ini["env:ThinkNode_M5_Repeater_bridge_espnow_udp"]
        self.assertIn("-Wl,--wrap=esp_wifi_set_ps", env["build_flags"])
        self.assertIn("-D MESH_WRAP_ESP_WIFI_SET_PS=1", env["build_flags"])
        self.assertIn("+<helpers/esp32/EspNowAwakeWrap.cpp>", env["build_src_filter"])


if __name__ == "__main__":
    unittest.main()
