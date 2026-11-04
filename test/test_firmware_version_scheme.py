"""The _udp M5 env embeds the same version scheme as the shipped build.sh image."""
from pathlib import Path
import configparser
import datetime
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import firmware_version_scheme as scheme  # noqa: E402


class FirmwareVersionSchemeTests(unittest.TestCase):
    now = datetime.datetime(2026, 11, 2, 9, 41, tzinfo=datetime.timezone.utc)

    def test_matches_the_shipped_m5_string(self):
        # The owner's M5 runs "v1.17.1-dev-51138a2e (Build: 02-Nov-2026)".
        tags = ["repeater-v1.17.1", "repeater-v1.17.0", "repeater-v1.16.0"]
        version, date, epoch = scheme.scheme(tags, "51138a2e0123456789", self.now, "repeater")
        self.assertEqual(version, "v1.17.1-dev-51138a2e")
        self.assertEqual(date, "02-Nov-2026")
        self.assertEqual(epoch, int(self.now.timestamp()))

    def test_build_sh_rules(self):
        self.assertEqual(scheme.default_version([], self.now, "repeater"), "dev-2026-11-02-09-41")
        self.assertEqual(scheme.default_version(["repeater-v2.0.0-dev"], self.now, "repeater"), "v2.0.0-dev")
        self.assertEqual(scheme.default_version(["companion-v9.0.0", "repeater-v1.0.0"], self.now, "repeater"),
                         "v1.0.0-dev")
        version, _, _ = scheme.scheme(["repeater-v1.17.1"], "abcdef0123", self.now, "repeater", "v9.9.9")
        self.assertEqual(version, "v9.9.9-abcdef01")   # FIRMWARE_VERSION override, as --firmware-version

    def test_udp_env_uses_the_scheme(self):
        ini = configparser.ConfigParser(strict=False, interpolation=None)
        ini.read(ROOT / "variants/thinknode_m5/platformio.ini")
        env = ini["env:ThinkNode_M5_Repeater_bridge_espnow_udp"]
        self.assertIn("pre:scripts/firmware_version_env.py", env["extra_scripts"])
        self.assertEqual(env["custom_firmware_tag_prefix"].strip(), "repeater")
        script = (ROOT / "scripts/firmware_version_env.py").read_text()
        for flag in ("FIRMWARE_VERSION", "FIRMWARE_BUILD_DATE", "FIRMWARE_BUILD_EPOCH"):
            self.assertIn("-D%s=" % flag, script)


if __name__ == "__main__":
    unittest.main()
