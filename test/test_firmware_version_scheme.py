"""The _udp M5 env embeds the same version scheme as the shipped build.sh image.

The expectations come from build_legacy.sh itself: its version functions
(get_latest_version_from_tags, derive_default_firmware_version,
get_firmware_version_string) are extracted and run in bash with a stub git,
and its date formats are read from the script.
"""
from pathlib import Path
import configparser
import datetime
import re
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import firmware_version_scheme as scheme  # noqa: E402

BUILD_SH = (ROOT / "build_legacy.sh").read_text()


def shell_function(name):
    match = re.search(r"^%s\(\) \{\n.*?^\}\n" % re.escape(name), BUILD_SH, re.S | re.M)
    if not match:
        raise AssertionError("build_legacy.sh has no %s()" % name)
    return match.group(0)


def build_sh_version(tags, commit, env_name="ThinkNode_M5_Repeater_bridge_espnow_udp"):
    program = "\n".join([
        'FALLBACK_VERSION_PREFIX="dev"',
        "FALLBACK_VERSION_DATE_FORMAT='+%Y-%m-%d-%H-%M'",
        "get_release_tag_prefix_for_env() { echo repeater; }",
        # git tag --list PATTERN --sort=-version:refname: tags already newest first
        'git() { local pattern=$3 t; for t in $TAGS; do case $t in $pattern) echo "$t";; esac; done; }',
        shell_function("get_latest_version_from_tags"),
        shell_function("derive_default_firmware_version"),
        shell_function("get_firmware_version_string"),
        'v=$(derive_default_firmware_version "$ENV_NAME")',
        'get_firmware_version_string "$ENV_NAME" "$v" "$COMMIT" embedded',
    ])
    result = subprocess.run(["bash", "-c", program], capture_output=True, text=True,
                            env={"TAGS": " ".join(tags), "COMMIT": commit[:8], "ENV_NAME": env_name,
                                 "PATH": "/usr/bin:/bin"})
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    return result.stdout


class FirmwareVersionSchemeTests(unittest.TestCase):
    now = datetime.datetime(2026, 11, 2, 9, 41, tzinfo=datetime.timezone.utc)

    @unittest.skipUnless(shutil.which("bash") and sys.platform.startswith("linux"), "bash (WSL) required")
    def test_versions_equal_build_sh(self):
        for tags in (["repeater-v1.17.1", "repeater-v1.17.0"],
                     ["repeater-v2.0.0-dev"],
                     ["companion-v9.0.0", "repeater-v1.0.0"]):
            with self.subTest(tags=tags):
                expected = build_sh_version(tags, "51138a2e0123")
                version, _, _ = scheme.scheme(tags, "51138a2e0123", self.now, "repeater")
                self.assertEqual(version, expected)
        self.assertEqual(build_sh_version(["repeater-v1.17.1"], "51138a2e0"), "v1.17.1-dev-51138a2e")

    def test_date_formats_are_build_sh_formats(self):
        build_date = re.search(r"firmware_build_date=\$\(date -u '\+([^']+)'\)", BUILD_SH).group(1)
        self.assertEqual(scheme.BUILD_DATE_FORMAT, build_date)
        fallback = re.search(r"^FALLBACK_VERSION_DATE_FORMAT='\+([^']+)'", BUILD_SH, re.M).group(1)
        self.assertEqual(scheme.FALLBACK_DATE_FORMAT, fallback)
        prefix = re.search(r'^FALLBACK_VERSION_PREFIX="([^"]+)"', BUILD_SH, re.M).group(1)
        self.assertEqual(scheme.FALLBACK_PREFIX, prefix)
        self.assertEqual(scheme.build_date(self.now), "02-Nov-2026")

    def test_override_and_no_double_definition(self):
        version, _, _ = scheme.scheme(["repeater-v1.17.1"], "abcdef0123", self.now, "repeater", "v9.9.9")
        self.assertEqual(version, "v9.9.9-abcdef01")   # FIRMWARE_VERSION override, as --firmware-version
        all_flags = scheme.missing_flags("", "v1-dev-x", "02-Nov-2026", 5)
        self.assertEqual(len(all_flags), 3)
        # build.sh exports these in PLATFORMIO_BUILD_FLAGS: never define them twice
        build_sh = "-DFIRMWARE_VERSION='\"v1.17.1-dev-51138a2e\"' -DOTA_VARIANT='\"x\"' " \
                   "-DFIRMWARE_BUILD_DATE='\"02-Nov-2026\"' -DFIRMWARE_BUILD_EPOCH=1793605260"
        self.assertEqual(scheme.missing_flags(build_sh, "v1-dev-x", "02-Nov-2026", 5), [])
        self.assertEqual(scheme.missing_flags("-D FIRMWARE_VERSION='\"v\"'", "v", "d", 5),
                         ["-DFIRMWARE_BUILD_DATE='\"d\"'", "-DFIRMWARE_BUILD_EPOCH=5"])
        self.assertEqual(len(scheme.missing_flags("-DFIRMWARE_VERSION_EXTRA=1", "v", "d", 5)), 3)

    def test_udp_env_uses_the_scheme(self):
        ini = configparser.ConfigParser(strict=False, interpolation=None)
        ini.read(ROOT / "variants/thinknode_m5/platformio.ini")
        env = ini["env:ThinkNode_M5_Repeater_bridge_espnow_udp"]
        self.assertIn("pre:scripts/firmware_version_env.py", env["extra_scripts"])
        self.assertEqual(env["custom_firmware_tag_prefix"].strip(), "repeater")
        script = (ROOT / "scripts/firmware_version_env.py").read_text()
        self.assertIn("scheme.missing_flags(existing", script)
        self.assertIn('os.environ.get("PLATFORMIO_BUILD_FLAGS"', script)


if __name__ == "__main__":
    unittest.main()
