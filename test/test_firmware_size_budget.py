"""ThinkNode M5 bridge images: flash budget guard (review F10).

variants/thinknode_m5/flash_budget.py fails a build whose image uses more
than 97% of its app partition. These checks cover its arithmetic and table
parsing, that both M5 bridge envs run it, and, when a build of those envs is
present in .pio/build, the actual images.
"""
from pathlib import Path
import importlib.util
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "variants/thinknode_m5/flash_budget.py"

MIN_SPIFFS = """# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x1E0000,
app1,     app,  ota_1,   0x1F0000,0x1E0000,
spiffs,   data, spiffs,  0x3D0000,0x20000,
coredump, data, coredump,0x3F0000,0x10000,
"""


def load():
    spec = importlib.util.spec_from_file_location("flash_budget", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FlashBudgetTests(unittest.TestCase):
    def test_arithmetic_and_tables(self):
        m = load()
        self.assertEqual(m.app_slot_size(MIN_SPIFFS.splitlines()), 1966080)
        with open(ROOT / "variants/dual_ota_full_4MB.csv", newline="") as table:
            self.assertEqual(m.app_slot_size(table), 1984 * 1024)
        self.assertEqual(m.parse_size("1984K"), 1984 * 1024)
        self.assertEqual(m.parse_size("0x1E0000"), 1966080)
        self.assertTrue(m.check(1876089, 1966080)[0])            # 95.4%: fine
        self.assertTrue(m.check(int(1966080 * 0.97), 1966080)[0])
        self.assertFalse(m.check(int(1966080 * 0.97) + 1, 1966080)[0])
        self.assertTrue(m.check(10, 0)[0])                        # unknown slot: not checked

    def test_both_m5_bridge_envs_run_it(self):
        ini = (ROOT / "variants/thinknode_m5/platformio.ini").read_text()
        default = ini[ini.index("[env:ThinkNode_M5_Repeater_bridge_espnow]"):]
        default = default[:default.index("\n[env:", 1)]
        self.assertIn("post:variants/thinknode_m5/flash_budget.py", default)
        udp = ini[ini.index("[env:ThinkNode_M5_Repeater_bridge_espnow_udp]"):]
        udp = udp[:udp.index("\n[env:", 1)]
        self.assertIn("${env:ThinkNode_M5_Repeater_bridge_espnow.extra_scripts}", udp)

    def test_built_images_fit(self):
        m = load()
        slots = {"ThinkNode_M5_Repeater_bridge_espnow": 1966080,
                 "ThinkNode_M5_Repeater_bridge_espnow_udp": 1984 * 1024}
        checked = 0
        for env, slot in slots.items():
            image = ROOT / ".pio/build" / env / "firmware.bin"
            if not image.is_file():
                continue
            ok, message = m.check(image.stat().st_size, slot)
            self.assertTrue(ok, f"{env}: {message}")
            checked += 1
        if not checked:
            self.skipTest("no M5 bridge build in .pio/build")


if __name__ == "__main__":
    unittest.main()
