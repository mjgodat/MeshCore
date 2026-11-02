"""Repeater user-button long press: opt-in page actions instead of power off.

Compiles the real LONG_PRESS branch of examples/simple_repeater UITask::loop()
and the real UITask::handleUserButtonHold() against host fakes, once without
REPEATER_UI_HOLD_ACTIONS (power-off path unchanged) and once with it (home page
hold sends one rate-limited flood advert, never powers off). Source contracts
pin the build flag to the ThinkNode M5 repeater envs only.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

from test_replay_reset_integration import extract_braced

ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / 'examples/simple_repeater/UITask.cpp'
UI_HEADER = ROOT / 'examples/simple_repeater/UITask.h'
BUTTON = ROOT / 'src/helpers/ui/MomentaryButton.h'
HOLD = ROOT / 'src/helpers/ui/RepeaterHoldAction.h'
CLI = ROOT / 'src/helpers/CommonCLI.cpp'
FIXTURE = ROOT / 'test/fixtures/repeater_hold_actions.cpp'
M5_INI = ROOT / 'variants/thinknode_m5/platformio.ini'
FLAG = '-D REPEATER_UI_HOLD_ACTIONS=1'
LONG_PRESS = '} else if (ev == BUTTON_EVENT_LONG_PRESS) {'


def read(path):
    return path.read_text(encoding='utf-8').replace('\r\n', '\n')


def long_press_branch(source):
    start = source.index(LONG_PRESS) + len(LONG_PRESS)
    end = source.index('#ifdef WITH_WEBCONFIG\n  } else if (ev == BUTTON_EVENT_TRIPLE_CLICK)', start)
    return source[start:end]


def harness():
    source = read(UI)
    events = '\n'.join(re.findall(r'^#define BUTTON_EVENT_\w+\s+\d+$', read(BUTTON), re.MULTILINE))
    replacements = {
        '@BUTTON_EVENTS@': events,
        '@MILLIS_REACHED@': extract_braced(source, 'static inline bool millisReached('),
        '@FEEDBACK_ACTIVE@': extract_braced(source, 'bool UITask::holdFeedbackActive()'),
        '@HOLD@': extract_braced(source, 'void UITask::handleUserButtonHold()'),
        '@BRANCH@': long_press_branch(source),
    }
    text = read(FIXTURE)
    for key, value in replacements.items():
        if text.count(key) != 1:
            raise AssertionError('fixture placeholder ' + key)
        text = text.replace(key, value)
    return text


def ini_sections(text):
    sections = {}
    for match in re.finditer(r'^\[([^\]]+)\]\n(.*?)(?=^\[|\Z)', text, re.MULTILINE | re.DOTALL):
        sections[match.group(1)] = match.group(2)
    return sections


class RepeaterHoldActionsTest(unittest.TestCase):
    def build_and_run(self, enabled):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            self.skipTest('a host C++17 compiler is required')
        sanitize = [] if os.name == 'nt' else [
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        with tempfile.TemporaryDirectory(prefix='repeater-hold-') as directory:
            work = Path(directory)
            source = work / 'hold.cpp'
            source.write_text(harness(), encoding='utf-8')
            binary = work / ('hold' + ('.exe' if os.name == 'nt' else ''))
            macros = ['-DREPEATER_UI_HOLD_ACTIONS=1'] if enabled else []
            built = subprocess.run([
                compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                *sanitize, *macros, '-I', str(ROOT / 'src'), str(source), '-o', str(binary),
            ], capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            return ran.stdout

    def test_flag_off_long_press_still_powers_off(self):
        self.assertRegex(self.build_and_run(False), r'PASS flag-off: \d+ checks')

    def test_flag_on_home_hold_sends_one_rate_limited_advert(self):
        self.assertRegex(self.build_and_run(True), r'PASS flag-on: \d+ checks')

    def test_power_off_branch_is_the_unflagged_alternative(self):
        branch = long_press_branch(read(UI))
        flagged, _, rest = branch.partition('#else')
        unflagged = rest.rsplit('#endif', 1)[0]
        self.assertIn('#if defined(REPEATER_UI_HOLD_ACTIONS) && REPEATER_UI_HOLD_ACTIONS', flagged)
        self.assertIn('handleUserButtonHold();', flagged)
        self.assertNotIn('_powering_off_at', flagged)
        self.assertIn('_powering_off_at = millis() + POWEROFF_DELAY;', unflagged)
        self.assertIn('mesh::usbConsolePort().printf("Powering Off\\r\\n");', unflagged)

    def test_hold_reuses_cli_advert_call(self):
        cli = read(CLI)
        self.assertIn('_callbacks->sendSelfAdvertisement(1500, true);', cli)
        hold = extract_braced(read(UI), 'void UITask::handleUserButtonHold()')
        self.assertIn('the_mesh.sendSelfAdvertisement(mesh::ui::REPEATER_HOLD_ADVERT_DELAY_MILLIS, true);', hold)
        self.assertNotIn('createSelfAdvert', hold)
        self.assertIn('constexpr int REPEATER_HOLD_ADVERT_DELAY_MILLIS = 1500;', read(HOLD))
        self.assertIn('constexpr uint32_t REPEATER_HOLD_ADVERT_MIN_INTERVAL_MILLIS = 30000;', read(HOLD))

    def test_hint_and_feedback_are_drawn_only_with_flag(self):
        source = read(UI)
        self.assertIn('drawHoldFeedback();', source)
        self.assertIn('mesh::ui::repeaterHoldHint(currentHoldAction())', source)
        self.assertIn('#if defined(REPEATER_UI_HOLD_ACTIONS) && REPEATER_UI_HOLD_ACTIONS\n'
                      '        drawHoldFeedback();\n#endif', source)
        self.assertIn('#if defined(REPEATER_UI_HOLD_ACTIONS) && REPEATER_UI_HOLD_ACTIONS\n'
                      '  #include <helpers/ui/RepeaterHoldAction.h>\n#endif', read(UI_HEADER))

    def test_flag_set_for_m5_repeater_envs_only(self):
        sections = ini_sections(read(M5_INI))
        with_flag = sorted(name for name, body in sections.items() if FLAG in body)
        self.assertEqual(with_flag, ['env:ThinkNode_M5_Repeater',
                                     'env:ThinkNode_M5_Repeater_bridge_espnow'])
        others = [path for path in (ROOT / 'variants').rglob('platformio.ini')
                  if path != M5_INI and 'REPEATER_UI_HOLD_ACTIONS' in read(path)]
        self.assertEqual(others, [])
        self.assertNotIn('REPEATER_UI_HOLD_ACTIONS', read(ROOT / 'platformio.ini'))


if __name__ == '__main__':
    unittest.main()
