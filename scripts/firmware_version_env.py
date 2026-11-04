# PlatformIO pre-script: give an env the same FIRMWARE_VERSION /
# FIRMWARE_BUILD_DATE / FIRMWARE_BUILD_EPOCH as the shipped build.sh image
# (scripts/firmware_version_scheme.py). The tag prefix comes from the env's
# custom_firmware_tag_prefix option (e.g. "repeater"). Macros build.sh (or
# the env) already defines are left alone, so nothing is defined twice.
import datetime
import os
import subprocess
import sys

Import("env")  # noqa: F821  (SCons)

sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))  # noqa: F821
import firmware_version_scheme as scheme  # noqa: E402


def git(*args):
    try:
        return subprocess.check_output(["git", *args], cwd=env.subst("$PROJECT_DIR"),  # noqa: F821
                                       text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return ""


prefix = env.GetProjectOption("custom_firmware_tag_prefix", "repeater")  # noqa: F821
tags = git("tag", "--list", prefix + "-v*", "--sort=-version:refname").splitlines()
commit = git("rev-parse", "HEAD") or "00000000"
now = datetime.datetime.now(datetime.timezone.utc)
version, date, epoch = scheme.scheme(tags, commit, now, prefix, os.environ.get("FIRMWARE_VERSION"))
existing = " ".join([os.environ.get("PLATFORMIO_BUILD_FLAGS", ""),
                     " ".join(str(f) for f in env.GetProjectOption("build_flags", "").split())])  # noqa: F821
flags = scheme.missing_flags(existing, version, date, epoch)
if flags:
    env.Append(BUILD_FLAGS=flags)  # noqa: F821
print("firmware_version_env: %s" % (" ".join(flags) if flags else "already defined by the build"))
