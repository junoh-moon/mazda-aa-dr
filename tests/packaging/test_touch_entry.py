"""Touch route (packaging/touch): every choice is a jci-dialog button and each
action feeds the numeric trial menu's own lines.

Host tests use a fixture root, a stub jci-dialog and a stub trial. They prove
the button-to-action mapping across the three stick states (fresh, installed,
residue) and the fail-closed paths. They do not run the CMU update scanner,
jci-dialog on the CMU, or any vehicle step.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

PACK = Path(__file__).resolve().parents[2] / 'packaging'
TOUCH = PACK / 'touch'
TOUCH_SH_PATH = TOUCH / 'touch.sh'
TOUCH_SH = TOUCH_SH_PATH.read_text()
GUIDE = (PACK / 'TOUCH_START_KO.md').read_text()
sys.path.insert(0, str(TOUCH))
from make_entry import ENTRY_NAME  # noqa: E402

INSTALL_OK = 'Installation finished. Choose 5 to start the first BETA boot; later boots start it by themselves.\n'
REBOOT_REFUSAL = 'Refused, nothing deleted: guard/arm present; run 4, then 5, first\n'
DIALOG_STUB = '''#!/bin/sh
printf '%s\\n' "$*" >> "$DIALOG_LOG"
case "$1" in --info) exit 0;; esac
answer=$(sed -n '1p' "$DIALOG_ANSWERS")
[ -n "$answer" ] || exit 9
sed -i '1d' "$DIALOG_ANSWERS"
exit "$answer"
'''
TRIAL_STUB = '''#!/bin/sh
lines=$(cat)
printf '%s' "$lines" | tr '\\n' ' ' >> "$TRIAL_LOG"
printf '\\n' >> "$TRIAL_LOG"
key=$(printf '%s\\n' "$lines" | head -n 1)
if [ -f "$TRIAL_OUT/$key" ]; then cat "$TRIAL_OUT/$key"; fi
rc=0
if [ -f "$TRIAL_RC/$key" ]; then rc=$(cat "$TRIAL_RC/$key"); fi
exit "$rc"
'''


def write_exec(path, text):
    path.write_text(text)
    path.chmod(0o755)


class Run:
    def __init__(self, trial_calls, blocking, log):
        self.trial_calls = trial_calls
        self.blocking = blocking
        self.log = log


class TouchRouteTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.root = base / 'fixture'
        self.usb = base / 'usb'
        self.dialog_log = base / 'dialog.log'
        self.answers = base / 'answers.txt'
        self.trial_log = base / 'trial.log'
        self.trial_out = base / 'trial-out'
        self.trial_rc = base / 'trial-rc'
        self.package = self.root / 'data_persist/mx5-aa-dr'
        for path in (self.root / 'tmp', self.root / 'jci/tools',
                     self.package, self.usb, self.trial_out, self.trial_rc):
            path.mkdir(parents=True)
        (self.root / '.mx5dr-fixture').write_text('fixture\n')
        self.dialog = self.root / 'jci/tools/jci-dialog'
        write_exec(self.dialog, DIALOG_STUB)
        shutil.copy(TOUCH_SH_PATH, self.usb / 'touch.sh')
        shutil.copy(PACK / 'common.sh', self.usb / 'common.sh')
        write_exec(self.usb / 'trial', TRIAL_STUB)
        self.env = dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root),
                        DIALOG_LOG=str(self.dialog_log), DIALOG_ANSWERS=str(self.answers),
                        TRIAL_LOG=str(self.trial_log), TRIAL_OUT=str(self.trial_out),
                        TRIAL_RC=str(self.trial_rc))

    def tearDown(self):
        self.tmp.cleanup()

    def run_touch(self, answers, *, installed=False, residue=False, outputs=None,
                  codes=None, dialog=True, expect_rc=0):
        for log in (self.trial_log, self.dialog_log):
            log.unlink(missing_ok=True)
        self.answers.write_text(''.join('%d\n' % answer for answer in answers))
        for key, text in (outputs or {}).items():
            (self.trial_out / key).write_text(text)
        for key, code in (codes or {}).items():
            (self.trial_rc / key).write_text('%d\n' % code)
        if installed:
            (self.package / 'installed.txt').write_text('mode=BETA\n')
        if residue:
            # Uninstall keeps the mapped library and the logs; purge removes them.
            (self.package / 'libmx5dr.so').write_bytes(b'\x7fELF residue')
            (self.package / 'logs').mkdir(exist_ok=True)
        if not dialog:
            self.dialog.unlink()
        result = subprocess.run(['sh', str(self.usb / 'touch.sh')], env=self.env,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, expect_rc, result.stderr)
        calls = self.trial_log.read_text().splitlines() if self.trial_log.exists() else []
        dialog_lines = self.dialog_log.read_text().splitlines() if self.dialog_log.exists() else []
        blocking = [line for line in dialog_lines if not line.startswith('--info')]
        log_path = self.usb / 'touch-log.txt'
        return Run(calls, blocking, log_path.read_text() if log_path.exists() else '')

    # --- FRESH ------------------------------------------------------------
    def test_fresh_cancel_changes_nothing(self):
        run = self.run_touch([0])
        self.assertEqual(run.trial_calls, [])
        self.assertEqual(len(run.blocking), 1)
        self.assertIn('--cancel-label=INSTALL', run.blocking[0])
        self.assertIn('state: fresh', run.log)

    def test_fresh_install_then_restart_feeds_one_and_five(self):
        run = self.run_touch([1, 1], outputs={'1': INSTALL_OK})
        self.assertEqual(run.trial_calls, ['1 0', '5'])
        self.assertIn('--cancel-label=RESTART', run.blocking[1])

    def test_fresh_install_later_does_not_reboot(self):
        run = self.run_touch([1, 0], outputs={'1': INSTALL_OK})
        self.assertEqual(run.trial_calls, ['1 0'])

    def test_install_failure_offers_no_restart(self):
        run = self.run_touch([1, 0], codes={'1': 1})
        self.assertEqual(run.trial_calls, ['1 0'])
        self.assertIn('Install did not finish', run.blocking[1])

    def test_install_missing_finished_line_is_failure(self):
        run = self.run_touch([1, 0], outputs={'1': 'mx5dr: no space\n'})
        self.assertEqual(run.trial_calls, ['1 0'])
        self.assertIn('Install did not finish', run.blocking[1])

    # --- INSTALLED: main (check / save logs / more) -----------------------
    def test_installed_check_go(self):
        run = self.run_touch([0, 0], installed=True, outputs={'2': 'GO\n'})
        self.assertEqual(run.trial_calls, ['2 0'])
        self.assertIn('Check: GO', run.blocking[1])
        self.assertIn('state: installed', run.log)

    def test_installed_check_not_go_points_to_reinstall(self):
        run = self.run_touch([0, 0], installed=True,
                             outputs={'2': 'NO-GO: tripped, stock runs, BETA off.\n'})
        self.assertEqual(run.trial_calls, ['2 0'])
        self.assertIn('REINSTALL to re-enable', run.blocking[1])

    def test_installed_save_logs_feeds_three(self):
        run = self.run_touch([1, 1], installed=True)
        self.assertEqual(run.trial_calls, ['3'])

    def test_installed_save_cancel_changes_nothing(self):
        run = self.run_touch([1, 0], installed=True)
        self.assertEqual(run.trial_calls, [])

    # --- INSTALLED: MORE (reinstall / restart / remove) -------------------
    def test_installed_reinstall_then_restart_feeds_one_and_five(self):
        run = self.run_touch([2, 0, 1, 1], installed=True, outputs={'1': INSTALL_OK})
        self.assertEqual(run.trial_calls, ['1', '5'])

    def test_installed_reinstall_later_feeds_one_only(self):
        run = self.run_touch([2, 0, 1, 0], installed=True, outputs={'1': INSTALL_OK})
        self.assertEqual(run.trial_calls, ['1'])

    def test_installed_reinstall_cancel_changes_nothing(self):
        run = self.run_touch([2, 0, 0], installed=True)
        self.assertEqual(run.trial_calls, [])

    def test_installed_restart_needs_confirm(self):
        self.assertEqual(self.run_touch([2, 1, 1], installed=True).trial_calls, ['5'])
        self.assertEqual(self.run_touch([2, 1, 0], installed=True).trial_calls, [])

    def test_installed_remove_then_restart_feeds_four_and_five(self):
        run = self.run_touch([2, 2, 1, 1], installed=True)
        self.assertEqual(run.trial_calls, ['4', '5'])

    def test_installed_remove_later_feeds_four_only(self):
        run = self.run_touch([2, 2, 1, 0], installed=True)
        self.assertEqual(run.trial_calls, ['4'])

    def test_installed_remove_cancel_changes_nothing(self):
        run = self.run_touch([2, 2, 0], installed=True)
        self.assertEqual(run.trial_calls, [])

    # --- RESIDUE: save logs / reinstall / delete files --------------------
    def test_residue_state_is_detected(self):
        run = self.run_touch([3], residue=True)  # 3 is no valid button -> closed
        self.assertIn('state: residue', run.log)
        self.assertEqual(run.trial_calls, [])

    def test_residue_save_logs_feeds_three(self):
        run = self.run_touch([0, 1], residue=True)
        self.assertEqual(run.trial_calls, ['3'])

    def test_residue_reinstall_then_restart_feeds_one_and_five(self):
        run = self.run_touch([1, 1, 1], residue=True, outputs={'1': INSTALL_OK})
        self.assertEqual(run.trial_calls, ['1', '5'])

    def test_residue_delete_two_confirms_feeds_six_six(self):
        run = self.run_touch([2, 1, 1], residue=True)
        self.assertEqual(run.trial_calls, ['6 6'])
        self.assertIn('Really delete everything?', run.blocking[2])

    def test_residue_delete_cancel_at_either_confirm_changes_nothing(self):
        self.assertEqual(self.run_touch([2, 0], residue=True).trial_calls, [])
        self.assertEqual(self.run_touch([2, 1, 0], residue=True).trial_calls, [])

    def test_residue_delete_refused_while_live_offers_restart(self):
        run = self.run_touch([2, 1, 1, 1], residue=True, codes={'6': 1},
                             outputs={'6': REBOOT_REFUSAL})
        self.assertEqual(run.trial_calls, ['6 6', '5'])
        self.assertIn('still live', run.blocking[-1])

    def test_residue_delete_other_failure_is_reported(self):
        run = self.run_touch([2, 1, 1], residue=True, codes={'6': 1},
                             outputs={'6': 'mx5dr: unexpected\n'})
        self.assertEqual(run.trial_calls, ['6 6'])
        self.assertIn('Delete did not run', run.blocking[-1])

    # --- fail closed ------------------------------------------------------
    def test_missing_dialog_logs_and_exits_nonzero(self):
        run = self.run_touch([], dialog=False, expect_rc=1)
        self.assertEqual(run.trial_calls, [])
        self.assertIn('jci-dialog is missing', run.log)

    def test_dialog_failure_is_not_a_choice(self):
        run = self.run_touch([])
        self.assertEqual(run.trial_calls, [])
        self.assertIn('install cancelled', run.log)

    def test_touch_route_never_reads_the_keyboard(self):
        for line in TOUCH_SH.splitlines():
            code = line.split('#', 1)[0]
            self.assertIsNone(re.search(r'(^|[\s;&|(])read(\s|$)', code), line)
        self.assertNotIn('/dev/tty', TOUCH_SH)

    def test_touch_route_takes_no_arguments(self):
        result = subprocess.run(['sh', str(self.usb / 'touch.sh'), 'extra'], env=self.env,
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.trial_log.exists())

    def test_destructive_buttons_sit_on_the_right_of_every_confirm(self):
        # The left button returns 0; a stray tap there must never start an action.
        destructive = {'INSTALL', 'REINSTALL', 'REMOVE', 'RESTART', 'SAVE', 'DELETE'}
        safe_left = {'CANCEL', 'LATER', 'OK'}
        calls = re.findall(r"ask_two '[^']*' (\S+) (\S+) \|\|", TOUCH_SH)
        self.assertTrue(calls)
        for left, right in calls:
            self.assertNotIn(left, destructive, (left, right))
            if right in destructive:
                self.assertIn(left, safe_left, (left, right))


class TouchEntryTests(unittest.TestCase):
    def test_entry_name_is_one_plain_file_name(self):
        self.assertNotIn('/', ENTRY_NAME)
        self.assertTrue(ENTRY_NAME.endswith(').up'))
        self.assertIn('MX5DR-TOUCH.ID', ENTRY_NAME)
        self.assertIn('$I', ENTRY_NAME)

    def test_entry_expression_runs_touch_only_on_the_one_identified_stick(self):
        cases = {
            'one identified stick': (['sdb1'], ['sdb1']),
            'two identified sticks': (['sdb1', 'sdc1'], []),
            'no identified stick': ([], []),
        }
        for label, (identified, ran) in cases.items():
            with self.subTest(label), tempfile.TemporaryDirectory() as base:
                base_path = Path(base)
                for dev in ('sda1', 'sdb1', 'sdc1', 'sdd1'):
                    mount = base_path / 'tmp/mnt' / dev
                    mount.mkdir(parents=True)
                    (mount / 'touch.sh').write_text(
                        ': > "%s/ran-%s"\n' % (base_path, dev))
                    if dev in identified:
                        (mount / 'MX5DR-TOUCH.ID').write_text('mx5dr-touch-v1\n')
                # HOME=/root gives ${HOME%root} = "/" as on the CMU; MX5DR_ROOT
                # stands in for the mount base, which is empty on the CMU.
                expression = ENTRY_NAME[:-len('.up')]
                subprocess.run(['sh', '-c', 'printf "%s\\n" ' + expression],
                               env={'HOME': '/root', 'PATH': os.environ['PATH'],
                                    'MX5DR_ROOT': str(base_path)},
                               check=True, capture_output=True)
                self.assertEqual(sorted(p.name for p in base_path.glob('ran-*')),
                                 sorted('ran-' + dev for dev in ran))

    def test_entry_archive_carries_only_the_two_scanner_files(self):
        with tempfile.TemporaryDirectory() as out:
            subprocess.run([sys.executable, str(TOUCH / 'make_entry.py'), out], check=True)
            entries = list(Path(out).iterdir())
            self.assertEqual([p.name for p in entries], [ENTRY_NAME])
            with zipfile.ZipFile(entries[0]) as archive:
                self.assertIsNone(archive.testzip())
                self.assertEqual(archive.namelist(), ['main_instructions.ini', 'versions.ini'])
                for name in archive.namelist():
                    self.assertEqual(archive.read(name), (TOUCH / 'launcher' / name).read_bytes())

    def test_full_bundle_carries_the_touch_route_and_both_guides(self):
        with tempfile.TemporaryDirectory() as base:
            build = Path(base) / 'build'
            build.mkdir()
            for name in ('libmx5dr.so', 'mx5dr-collector', 'mx5dr-guard',
                         'libmx5dr-vimtap.so', 'libmx5dr-ldstap.so', 'mx5dr-sha256'):
                (build / name).write_bytes(b'\x7fELF' + name.encode())
            dest = Path(base) / 'bundle'
            result = subprocess.run(['sh', str(PACK / 'make_bundle.sh'), '--default-mode=SHADOW',
                                     str(build / 'libmx5dr.so'), str(dest)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            names = {p.name for p in dest.iterdir()}
            self.assertLessEqual({'touch.sh', 'MX5DR-TOUCH.ID', 'jci-autoupdate', ENTRY_NAME,
                                  'INSTALL_KO.md', 'SHELL_INSTALL_KO.md', 'common.sh', 'trial',
                                  'mp3', 'js'}, names)
            self.assertEqual((dest / 'jci-autoupdate').stat().st_size, 0)
            self.assertEqual((dest / 'touch.sh').read_bytes(), TOUCH_SH_PATH.read_bytes())
            self.assertEqual((dest / 'INSTALL_KO.md').read_bytes(), (PACK / 'TOUCH_START_KO.md').read_bytes())
            self.assertEqual((dest / 'SHELL_INSTALL_KO.md').read_bytes(), (PACK / 'USB_START_KO.md').read_bytes())

    def test_shell_only_bundle_has_no_touch_route(self):
        with tempfile.TemporaryDirectory() as base:
            build = Path(base) / 'build'
            build.mkdir()
            for name in ('libmx5dr.so', 'mx5dr-collector', 'mx5dr-guard',
                         'libmx5dr-vimtap.so', 'libmx5dr-ldstap.so', 'mx5dr-sha256'):
                (build / name).write_bytes(b'\x7fELF' + name.encode())
            dest = Path(base) / 'bundle'
            result = subprocess.run(['sh', str(PACK / 'make_bundle.sh'), '--shell-only',
                                     '--default-mode=SHADOW', str(build / 'libmx5dr.so'), str(dest)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            names = {p.name for p in dest.iterdir()}
            self.assertFalse({'touch.sh', 'MX5DR-TOUCH.ID', 'jci-autoupdate', 'SHELL_INSTALL_KO.md'} & names)
            self.assertFalse(any(name.endswith(').up') for name in names))


class TouchGuideTests(unittest.TestCase):
    def test_guide_types_nothing_and_quotes_only_real_screen_texts(self):
        self.assertNotIn('```', GUIDE)
        for quoted in ('INSTALL', 'RESTART', 'LATER', 'CHECK', 'REMOVE', 'MORE', 'SAVE LOGS',
                       'REINSTALL', 'DELETE FILES', 'Check: GO', 'Installed.',
                       'MX5 AA DR is installed.', 'Install MX5 AA DR (BETA)?',
                       'Installing MX5 AA DR.', 'NO-GO', 'WAIT 60 s, then run 2 again',
                       'startup-result.txt', 'touch-log.txt'):
            self.assertIn('`%s`' % quoted, GUIDE, quoted)
            self.assertIn(quoted, TOUCH_SH, quoted)

    def test_guide_names_the_stick_files_and_the_shell_fallback(self):
        for name in ('touch.sh', 'trial', 'MX5DR-TOUCH.ID', 'jci-autoupdate', 'SHELL_INSTALL_KO.md'):
            self.assertIn(name, GUIDE)


if __name__ == '__main__':
    unittest.main()
