"""Persistent BETA packaging: menu install, status/verdict and a multi-boot
host run of the real guard source against the installed fixture tree.

Authored fixtures only. The host-built guard (MX5DR_GUARD_TESTING) stands in
for the ARM helper that the fixture installer never executes.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import test_trial_menu

HERE = Path(__file__).resolve().parents[2]
BOOT = test_trial_menu.BOOT.strip()


def boot_id(n):
    return '%08x' % (0x87650000 + n) + BOOT[8:]


class PersistentBetaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.guard_exe = Path(cls.build.name) / 'guard'
        subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-DMX5DR_GUARD_TESTING',
                        str(HERE / 'src/guard/guard.cpp'), str(HERE / 'src/runtime/sha256.cpp'),
                        '-o', str(cls.guard_exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.menu_fixture = test_trial_menu.TrialMenuTests()
        self.addCleanup(self.menu_fixture.doCleanups)
        self.menu_fixture.setUp()
        m = self.menu_fixture
        self.root, self.usb, self.base, self.logs = m.root, m.usb, m.base, m.logs
        self.fixture = m.fixture
        (self.root / 'data').mkdir()
        (self.root / 'data/dmesg.out').write_text('[ 1451.0] an old reset before installation\n')
        self.genv = dict(os.environ, MX5DR_GUARD_ROOT=str(self.root))
        # trial_status asks the guard (`status`); fixtures supply the host build.
        self.menu_fixture.env['MX5DR_FIXTURE_GUARD'] = str(self.guard_exe)

    def menu(self, keys):
        return self.menu_fixture.menu(keys)

    def set_boot(self, n):
        (self.root / 'proc/sys/kernel/random/boot_id').write_text(boot_id(n) + '\n')

    def guard(self, *args):
        return subprocess.run([str(self.guard_exe)] + list(args), env=self.genv, text=True, capture_output=True)

    def install_beta(self):
        (self.usb / 'bundle-default-mode').write_text('BETA\n')
        result = self.menu('1\n0\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def menu1(self):
        """Menu 1 on the vehicle: install, then the target guard's enable."""
        result = self.install_beta()
        enabled = self.guard('enable')
        self.assertEqual(enabled.returncode, 0, enabled.stderr)
        return result

    def autostart_select(self, confirm=True):
        """The owned autostart block: select, then (90 s later) confirm while
        the SM runs with the trial (authored /proc entry, vehicle ps form)."""
        result = self.guard('select', '/jci/sm/sm.conf')
        if result.returncode != 0:
            return None
        trial = result.stdout.strip()
        if confirm:
            proc = self.root / 'proc/266'
            proc.mkdir(parents=True, exist_ok=True)
            (proc / 'cmdline').write_bytes(b'\0'.join([b'/jci/sm/sm', b'-f', trial[len(str(self.root)):].encode(),
                                                       b'-e', b'/tmp/smevents.txt']) + b'\0')
            confirmed = self.guard('confirm')
            self.assertEqual(confirmed.returncode, 0, confirmed.stderr)
        return trial

    def status(self):
        result = self.menu('2\n0\n')
        text = result.stdout
        block = text[text.index('---- GO / NO-GO ----'):]
        verdict = block[:block.index('\n0 Exit')].strip().splitlines()
        for line in verdict:
            self.assertLessEqual(len(line), 40, line)
        return text, verdict

    def test_beta_install_is_persistent_and_shadow_stays_one_boot(self):
        result = self.install_beta()
        self.assertIn('Automatic fallback: after 2 boots in a row that end in a CMU reset', result.stdout)
        self.assertIn('Choose 5 to start the first BETA boot', result.stdout)
        installed = (self.base / 'installed.txt').read_text()
        self.assertIn('mode=BETA\npolicy=persistent\n', installed)
        # The always-on product journals with the quiet profile and a 16 MiB
        # trace ring (validation/PERSISTENT_LOGGING_2026-10-06.md).
        self.assertEqual((self.base / 'mx5dr.conf').read_text(),
                         'mode=BETA\nmax_log_bytes=8388608\nmax_log_files=2\nsample_ms=1000\n'
                         'log_profile=persistent\n')
        # The one-boot arming marker belongs to the one-boot policy only.
        self.assertFalse((self.base / 'guard/armed-boot').exists())
        self.assertFalse((self.base / 'guard/arm').exists())
        self.assertIn('ONE-BOOT BEGIN SMCFG_NORMALMODE', self.fixture.autostart.read_text())
        self.menu('4\n')
        (self.usb / 'bundle-default-mode').write_text('SHADOW\n')
        self.assertEqual(self.menu('1\n0\n').returncode, 0)
        self.assertIn('policy=one-boot', (self.base / 'installed.txt').read_text())
        # SHADOW trial bundles keep the full journal profile (no profile key).
        self.assertEqual((self.base / 'mx5dr.conf').read_text(),
                         'mode=SHADOW\nmax_log_bytes=41943040\nmax_log_files=3\nsample_ms=1000\n')
        self.assertTrue((self.base / 'guard/armed-boot').exists())

    def test_explicit_one_boot_beta_keeps_the_trial_policy(self):
        self.fixture.run_script('install.sh', '--mode=BETA', '--one-boot')
        self.assertIn('policy=one-boot', (self.base / 'installed.txt').read_text())
        self.assertTrue((self.base / 'guard/armed-boot').exists())

    def test_reinstall_clears_enablement_and_beta_acknowledges_runtime_disable(self):
        self.install_beta()
        (self.base / 'guard/persist').write_text('stale enablement\n')
        (self.logs / 'disable-next-start').write_text('x')
        result = self.install_beta()
        self.assertIn('Cleared the runtime disable marker', result.stdout)
        self.assertFalse((self.logs / 'disable-next-start').exists())
        # The fixture installer cannot run the ARM guard; a stale manifest never survives.
        self.assertFalse((self.base / 'guard/persist').exists())
        # A one-boot SHADOW install never clears the runtime's stop marker.
        (self.logs / 'disable-next-start').write_text('x')
        (self.usb / 'bundle-default-mode').write_text('SHADOW\n')
        self.assertEqual(self.menu('1\n0\n').returncode, 0)
        self.assertTrue((self.logs / 'disable-next-start').exists())

    def test_uninstall_and_one_boot_arm_remove_the_enablement(self):
        self.menu1()
        self.assertTrue((self.base / 'guard/persist').exists())
        result = self.menu('4\n')
        self.assertIn('the persistent enablement', result.stdout)
        self.assertFalse((self.base / 'guard/persist').exists())
        self.assertEqual(self.fixture.autostart.read_bytes(), self.fixture.original_autostart)
        # Evidence stays for export.
        self.assertTrue((self.base / 'guard/persist-state').exists())
        self.menu1()
        self.fixture.run_script('arm.sh', '--mode=SHADOW')
        self.assertFalse((self.base / 'guard/persist').exists())

    def test_multi_boot_product_trip_and_reenable(self):
        self.menu1()
        # Installing boot: status says reboot with 5; the guard never selects here.
        text, verdict = self.status()
        self.assertIn('guard_policy=persistent persist=enabled', text)
        self.assertIn('persist_enabled_this_boot=yes', text)
        self.assertIn('NO   BOOT  installed; choose 5', verdict)
        self.assertIsNone(self.autostart_select())
        for n in range(1, 4):
            self.set_boot(n)
            path = self.autostart_select()
            self.assertIsNotNone(path)
            self.assertIn('libmx5dr-vimtap.so', Path(path).read_text())
            text, verdict = self.status()
            self.assertIn('startup_state=guard_committed_persistent', text)
            self.assertIn('one_boot=persistent_this_boot', text)
            self.assertIn('guard_config_binding=matched', text)
            self.assertEqual(verdict[1:8], ['PERSIST enabled (every boot)',
                                            'fail count 0 of 2, unconfirmed 0 of 3',
                                            'recent boots ' + ('none' if n == 1 else 'C' * (n - 1)),
                                            'attempts since healthy %d (rule off)' % n,
                                            'healthy previous boot ' + ('none' if n == 1 else 'no'),
                                            'this boot selected yes',
                                            'this boot confirmed yes'])
            for row in ('ok   BOOT  product this boot', 'ok   CONF  confirmed', 'ok   GUARD committed',
                        'ok   PERS  enabled'):
                self.assertIn(row, verdict)
            self.assertFalse(any(line.startswith('NO   ONCE') for line in verdict))
        # Boot 3 ends in an SM reset: the reports change.
        (self.root / 'data/dmesg.out').write_text('[ 1452.7] reset during boot 3\n')
        self.set_boot(4)
        self.assertIsNotNone(self.autostart_select())
        text, verdict = self.status()
        self.assertIn('persist_fail_count=1', text)
        self.assertIn('persist_previous=failed_reset', text)
        self.assertIn('fail count 1 of 2, unconfirmed 0 of 3', verdict)
        # Boot 4 also resets: two in a row trip to stock.
        (self.root / 'data/thread_info.out').write_text('stacks during boot 4\n')
        self.set_boot(5)
        self.assertIsNone(self.autostart_select())
        text, verdict = self.status()
        self.assertIn('persist=tripped persist_reason=reset_reports', text)
        self.assertIn('startup_state=persistent_tripped', text)
        self.assertEqual(verdict[1], 'PERSIST tripped (reset_reports)')
        self.assertIn('this boot selected no', verdict)
        self.assertIn('NO   PERS  tripped reset_reports', verdict)
        self.assertIn('NO   BOOT  stock this boot', verdict)
        self.assertIn(' (tripped:reset_reports)', verdict)
        self.assertEqual(verdict[-2:], ['NO-GO: tripped, stock runs, BETA off.',
                                        'Find the cause, then run menu 1 again.'])
        for n in (6, 7):
            self.set_boot(n)
            self.assertIsNone(self.autostart_select())
        # Logs and reports stay; menu 3 still exports them.
        (self.logs / 'trace.0.jsonl').write_text('retained raw evidence\n')
        (self.logs / 'capture.done').write_text(boot_id(7) + '\n')
        exported = self.menu('3\n')
        self.assertIn('export_exit=0', exported.stdout)
        report = self.menu_fixture.assert_export()
        self.assertIn('guard.persist_state.line_14=tripped=reset_reports', report)
        self.assertIn('guard.persist.line_3=healthy_rule=off', report)
        self.assertIn('guard.inventory.status=known_entries_only', report)
        # Owner re-enables with menu 1 in boot 7; boot 8 is product again and
        # the previous boot's capture freeze does not carry over.
        self.menu1()
        text, verdict = self.status()
        self.assertIn('persist=enabled', text)
        self.assertIn('persist_fail_count=0', text)
        (self.logs / 'capture.stop').mkdir()
        (self.logs / 'capture.done').write_text(boot_id(7) + '\n')
        self.set_boot(8)
        self.assertIsNotNone(self.autostart_select())
        self.assertFalse((self.logs / 'capture.stop').exists())
        text, verdict = self.status()
        self.assertIn('ok   BOOT  product this boot', verdict)

    def test_status_and_guard_share_one_parser(self):
        """A state the guard rejects is never displayed as enabled (parity)."""
        self.menu1()
        self.set_boot(1)
        self.assertIsNotNone(self.autostart_select())
        state = self.base / 'guard/persist-state'
        good = state.read_text()
        variants = {
            'noncanonical count': good.replace('fail_count=0', 'fail_count=00'),
            'count above cap': good.replace('unconfirmed_count=0', 'unconfirmed_count=3'),
            'unknown trip': good.replace('tripped=no', 'tripped=maybe'),
            'old schema': good.replace('mx5dr-persist-state-v2', 'mx5dr-persist-state-v1'),
            'extra line': good + 'x=1\n',
            'bad recent': good.replace('recent=-', 'recent=CX'),
            'foreign confirm': good.replace('confirmed_boot=' + boot_id(1), 'confirmed_boot=' + boot_id(9)),
            'foreign trial path': good.replace('/tmp/mx5dr-trial-', '/tmp/other-trial-'),
        }
        for label, text in variants.items():
            with self.subTest(label=label):
                self.assertNotEqual(text, good)
                state.write_text(text)
                status = self.guard('status')
                self.assertIn('persist=invalid\n', status.stdout)
                screen, verdict = self.status()
                self.assertIn('guard_policy=persistent persist=invalid', screen)
                self.assertNotIn('PERSIST enabled', screen)
                self.assertIn('PERSIST invalid', verdict)
                self.assertEqual(verdict[-1], 'NO-GO')
                # The guard's own selection rejects the same state.
                self.set_boot(2)
                self.assertIsNone(self.autostart_select(confirm=False))
                self.set_boot(1)
        state.write_text(good)
        status = dict(line.split('=', 1) for line in self.guard('status').stdout.splitlines())
        screen, verdict = self.status()
        self.assertIn('persist_fail_count=%s persist_trip_at=2 persist_unconfirmed_count=%s'
                      % (status['fail_count'], status['unconfirmed_count']), screen)
        self.assertIn('PERSIST enabled (every boot)', verdict)
        # Without a runnable guard nothing is claimed.
        self.menu_fixture.env['MX5DR_FIXTURE_GUARD'] = str(self.root / 'missing')
        screen, verdict = self.status()
        self.assertIn('persist=unavailable', screen)
        self.assertIn('PERSIST unavailable', verdict)
        self.assertEqual(verdict[-1], 'NO-GO')

    def test_binding_change_is_visible_in_status(self):
        self.menu1()
        self.set_boot(1)
        self.assertIsNotNone(self.autostart_select())
        screen, verdict = self.status()
        self.assertIn('ok   PERS  enabled', verdict)
        # Another tool edits the stock SM config in this boot.
        sm = self.root / 'jci/sm/sm.conf'
        sm.write_text(sm.read_text() + '<!-- other tool -->\n')
        screen, verdict = self.status()
        self.assertIn('persist_verify=baseline_edited:sm.conf', screen)
        self.assertEqual(verdict[1], 'PERSIST enabled but bindings changed')
        message = ' '.join(line.strip() for line in verdict[2:5])
        self.assertIn('bindings changed: sm.conf edited by another tool: run menu 1', message)
        self.assertIn('NO   PERS  bindings changed', verdict)
        self.assertEqual(verdict[-1], 'NO-GO')
        # The next boot runs stock and says why.
        self.set_boot(2)
        self.assertIsNone(self.autostart_select())
        screen, verdict = self.status()
        self.assertIn('persist_last_decision=baseline_edited:sm.conf', screen)
        self.assertIn('NO   BOOT  stock this boot', verdict)
        self.assertIn(' (baseline_edited:sm.conf)', verdict)
        # A short reason fits on the row.
        self.assertIsNone(self.autostart_select())  # same boot again
        screen, verdict = self.status()

    def test_pending_confirmation_waits(self):
        self.menu1()
        self.set_boot(1)
        self.assertIsNotNone(self.autostart_select(confirm=False))
        screen, verdict = self.status()
        self.assertIn('this boot confirmed pending', verdict)
        self.assertIn('wait CONF  confirm after 90 s', verdict)
        self.assertEqual(verdict[-1], 'WAIT 60 s, then run 2 again')

    def test_reenable_keeps_trip_and_disable_evidence(self):
        self.menu1()
        for n in (1, 2):
            self.set_boot(n)
            self.assertIsNotNone(self.autostart_select())
            (self.root / 'data/dmesg.out').write_text('reset during boot %d\n' % n)
        self.set_boot(3)
        self.assertIsNone(self.autostart_select())
        tripped_state = (self.base / 'guard/persist-state').read_text()
        self.assertIn('tripped=reset_reports', tripped_state)
        (self.logs / 'disable-next-start').write_text('runtime reason\n')
        result = self.install_beta()
        self.assertIn('Kept the previous BETA state in backups/persist-evidence/1 (tripped: reset_reports).',
                      result.stdout)
        self.assertIn('BETA was tripped (reset_reports). Run menu 3 export first', result.stdout)
        self.assertIn('Cleared the runtime disable marker', result.stdout)
        kept = self.base / 'backups/persist-evidence/1'
        self.assertEqual((kept / 'persist-state').read_text(), tripped_state)
        self.assertEqual((kept / 'disable-next-start').read_text(), 'runtime reason\n')
        self.assertIn('reason=tripped:reset_reports', (kept / 'last-decision').read_text())
        self.assertFalse((self.logs / 'disable-next-start').exists())
        # Bounded: only the last three numbered copies remain.
        for _ in range(4):
            self.guard('enable')
            self.install_beta()
        names = sorted(int(p.name) for p in (self.base / 'backups/persist-evidence').iterdir())
        self.assertEqual(names, [3, 4, 5])

    def test_upgrade_from_published_one_boot_beta_state(self):
        """v1.0.0-beta.1 installed BETA as one guarded boot: arm, consumed,
        armed-boot and last-boot may all be present."""
        self.fixture.run_script('install.sh', '--mode=BETA', '--one-boot')
        guard = self.base / 'guard'
        manifest = 'mx5dr-one-boot-v3\n' + ('a' * 64 + '\n') * 8
        for name in ('arm', 'consumed'):
            (guard / name).write_text(manifest)
            (guard / name).chmod(0o600)
        (guard / 'last-boot').write_text(test_trial_menu.BOOT)
        (guard / 'last-boot').chmod(0o600)
        self.set_boot(1)
        self.menu1()
        self.assertFalse((guard / 'arm').exists())
        self.assertTrue((guard / 'consumed').exists())  # old evidence kept
        self.assertIn('policy=persistent', (self.base / 'installed.txt').read_text())
        screen, verdict = self.status()
        self.assertIn('guard_policy=persistent persist=enabled', screen)
        self.assertIn('NO   BOOT  installed; choose 5', verdict)
        self.set_boot(2)
        self.assertIsNotNone(self.autostart_select())
        screen, verdict = self.status()
        self.assertIn('ok   BOOT  product this boot', verdict)
        self.assertIn('ok   CONF  confirmed', verdict)

    def test_damaged_state_is_not_reported_as_selected(self):
        self.menu1()
        self.set_boot(1)
        self.assertIsNotNone(self.autostart_select())
        state = self.base / 'guard/persist-state'
        state.write_text(state.read_text().replace('fail_count=0', 'fail_count=x'))
        text, verdict = self.status()
        self.assertIn('persist=invalid', text)
        self.assertIn('startup_state=persistent_state_invalid', text)
        self.assertIn('PERSIST invalid', verdict)
        self.assertEqual(verdict[-1], 'NO-GO')


if __name__ == '__main__':
    unittest.main()
