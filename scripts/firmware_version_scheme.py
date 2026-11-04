"""Firmware version strings the way build.sh (build_legacy.sh) makes them.

build.sh embeds FIRMWARE_VERSION as "<version>-<commit8>", where <version>
is the latest "<tag prefix>-v*" git tag without its prefix plus "-dev"
(derive_default_firmware_version), or "dev-YYYY-MM-DD-HH-MM" when there is
no tag; FIRMWARE_BUILD_DATE is the UTC date "DD-Mon-YYYY" and
FIRMWARE_BUILD_EPOCH the UTC epoch. A FIRMWARE_VERSION environment variable
overrides the version, as with build.sh --firmware-version.
test/test_firmware_version_scheme.py checks these rules against the
functions in build_legacy.sh itself.
"""
import datetime
import re

FALLBACK_PREFIX = "dev"
FALLBACK_DATE_FORMAT = "%Y-%m-%d-%H-%M"   # build.sh FALLBACK_VERSION_DATE_FORMAT
BUILD_DATE_FORMAT = "%d-%b-%Y"            # build.sh: date -u '+%d-%b-%Y'


def default_version(tags, now, prefix):
    """tags: tag names sorted newest first (git tag --sort=-version:refname)."""
    for tag in tags:
        if tag.startswith(prefix + "-v"):
            base = tag[len(prefix) + 1:]
            break
    else:
        return FALLBACK_PREFIX + "-" + now.strftime(FALLBACK_DATE_FORMAT)
    if base.endswith("-dev") or base.startswith("dev-"):
        return base
    return base + "-dev"


def embedded_version(version, commit):
    return "%s-%s" % (version, commit[:8])


def build_date(now):
    return now.strftime(BUILD_DATE_FORMAT)


def scheme(tags, commit, now, prefix, explicit_version=None):
    """(FIRMWARE_VERSION, FIRMWARE_BUILD_DATE, FIRMWARE_BUILD_EPOCH)."""
    version = explicit_version or default_version(tags, now, prefix)
    epoch = int(now.replace(tzinfo=datetime.timezone.utc).timestamp()) if now.tzinfo is None \
        else int(now.timestamp())
    return embedded_version(version, commit), build_date(now), epoch


def missing_flags(existing_flags, version, date, epoch):
    """-D flags for the macros not already defined by `existing_flags` (for
    example build.sh's PLATFORMIO_BUILD_FLAGS), so nothing is defined twice."""
    flags = []
    for name, value in (("FIRMWARE_VERSION", "'\"%s\"'" % version),
                        ("FIRMWARE_BUILD_DATE", "'\"%s\"'" % date),
                        ("FIRMWARE_BUILD_EPOCH", str(epoch))):
        if re.search(r"-D\s*%s(=|\s|$)" % name, existing_flags or ""):
            continue
        flags.append("-D%s=%s" % (name, value))
    return flags
