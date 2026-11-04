# Post-build guard for the ThinkNode M5 bridge images: fail the build when
# the application image uses more than FLASH_BUDGET of its app partition, so
# growth is noticed before a release no longer fits (or leaves no room for a
# fix). The slot comes from the env's board_build.partitions table (the
# project's own CSV, or Arduino-ESP32's tools/partitions one).
import csv
import os

FLASH_BUDGET = 0.97


def parse_size(text):
    text = text.strip().upper()
    if not text:
        return 0
    if text.endswith("K"):
        return int(text[:-1], 0) * 1024
    if text.endswith("M"):
        return int(text[:-1], 0) * 1024 * 1024
    return int(text, 0)


def app_slot_size(lines):
    """Size of the first app partition in a partition table (CSV lines)."""
    rows = [r for r in csv.reader(lines) if r and not r[0].strip().startswith("#")]
    for row in rows:
        if len(row) >= 5 and row[1].strip() == "app":
            return parse_size(row[4])
    return 0


def check(size, slot, budget=FLASH_BUDGET):
    """(ok, message) for an image of `size` bytes in a `slot`-byte partition."""
    if slot <= 0:
        return True, "flash budget: app slot unknown, not checked"
    used = size / slot
    message = "flash budget: %d of %d bytes (%.1f%%, limit %.0f%%)" % (size, slot, used * 100, budget * 100)
    return used <= budget, message


def _partition_path(env):
    name = env.GetProjectOption("board_build.partitions", "default.csv")
    candidates = [os.path.join(env.subst("$PROJECT_DIR"), name)]
    try:
        framework = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
        if framework:
            candidates.append(os.path.join(framework, "tools", "partitions", name))
    except Exception:  # pragma: no cover - platform API differences
        pass
    for path in candidates:
        if os.path.isfile(path):
            return path
    return None


def _after_build(source, target, env):
    image = target[0].get_abspath()
    path = _partition_path(env)
    slot = 0
    if path:
        with open(path, newline="") as table:
            slot = app_slot_size(table)
    ok, message = check(os.path.getsize(image), slot)
    print(message)
    if not ok:
        env.Exit(1)


try:
    Import("env")  # noqa: F821 (PlatformIO SCons)
except NameError:
    env = None

if env is not None:
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _after_build)
