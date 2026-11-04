"""Firmware version strings the way build.sh (build_legacy.sh) makes them.

build.sh embeds FIRMWARE_VERSION as "<version>-<commit8>", where <version>
is the latest "<tag prefix>-v*" git tag without its prefix plus "-dev"
(derive_default_firmware_version), or "dev-YYYY-MM-DD-HH-MM" when there is
no tag; FIRMWARE_BUILD_DATE is the UTC date "DD-Mon-YYYY" and
FIRMWARE_BUILD_EPOCH the UTC epoch. A FIRMWARE_VERSION environment variable
overrides the version, as with build.sh --firmware-version.
Pure: test/test_firmware_version_scheme.py.
"""
import datetime


def default_version(tags, now, prefix):
    """tags: tag names sorted newest first (git tag --sort=-version:refname)."""
    for tag in tags:
        if tag.startswith(prefix + "-v"):
            base = tag[len(prefix) + 1:]
            break
    else:
        return "dev-" + now.strftime("%Y-%m-%d-%H-%M")
    if base.endswith("-dev") or base.startswith("dev-"):
        return base
    return base + "-dev"


def embedded_version(version, commit):
    return "%s-%s" % (version, commit[:8])


def build_date(now):
    return now.strftime("%d-%b-%Y")


def scheme(tags, commit, now, prefix, explicit_version=None):
    """(FIRMWARE_VERSION, FIRMWARE_BUILD_DATE, FIRMWARE_BUILD_EPOCH)."""
    version = explicit_version or default_version(tags, now, prefix)
    epoch = int(now.replace(tzinfo=datetime.timezone.utc).timestamp()) if now.tzinfo is None \
        else int(now.timestamp())
    return embedded_version(version, commit), build_date(now), epoch
