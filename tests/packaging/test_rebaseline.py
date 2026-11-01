"""rebaseline.sh (docs/INTEROP_KO.md, contract 1): another tool changes
/jci/sm/sm.conf and sm_WCP.conf after a persistent BETA install; the installed
tools/rebaseline.sh re-registers the product against the new files without a
reinstall, and --check reports the state without writing.

Authored fixtures only. The host-built guard (MX5DR_GUARD_TESTING) stands in
for the ARM helper, exactly as in test_persistent_beta.
"""
import os
from pathlib import Path
import shutil
import subprocess
import unittest

import test_persistent_beta

boot_id = test_persistent_beta.boot_id


P = test_persistent_beta.PersistentBetaTests


class RebaselineTests(unittest.TestCase):
    # The persistent BETA fixture and host guard, without re-running its tests.
    setUpClass = P.__dict__['setUpClass']
    tearDownClass = P.__dict__['tearDownClass']
    setUp = P.setUp
    menu = P.menu
    set_boot = P.set_boot
    guard = P.guard
    install_beta = P.install_beta
    menu1 = P.menu1
    autostart_select = P.autostart_select

    def rebaseline(self, *args, env=None):
        script = self.base / 'tools/rebaseline.sh'
        run_env = dict(self.menu_fixture.env, MX5DR_FIXTURE_GUARD=str(self.guard_exe))
        run_env.pop('MX5DR_GUARD_ROOT', None)
        run_env.update(env or {})
        result = subprocess.run(['sh', str(script), *args], env=run_env, text=True, capture_output=True,
                                stdin=subprocess.DEVNULL, timeout=30)
        lines = result.stdout.splitlines()
        if args != ('--version',):
            self.assertTrue(lines and lines[-1].startswith('result='), result.stdout + result.stderr)
            self.assertEqual(sum(1 for line in lines if line.startswith('result=')), 1, result.stdout)
        return result.returncode, (lines[-1] if lines else ''), result

    def other_tool_edit(self, name='sm.conf'):
        """A different tool (mazda-cmu-tune phase 1) changes a pinned file."""
        path = self.root / 'jci/sm' / name
        path.write_text(path.read_text() + '<!-- other tool: service trimmed -->\n')
        return path

    def guard_files(self):
        names = ('normal.trial', 'wcp.trial', 'normal.source.sha256', 'wcp.source.sha256', 'persist', 'persist-state')
        return {n: (self.base / 'guard' / n).read_bytes() for n in names if (self.base / 'guard' / n).exists()}

    def installed_product(self):
        self.menu1()
        self.set_boot(1)
        self.assertIsNotNone(self.autostart_select())

    def test_version_and_usage(self):
        self.install_beta()
        rc, line, result = self.rebaseline('--version')
        self.assertEqual((rc, result.stdout), (0, 'contract=1\n'))
        rc, line, _ = self.rebaseline('--bogus')
        self.assertEqual((rc, line), (1, 'result=precheck_failed:usage'))
        rc, line, _ = self.rebaseline('--check', 'extra')
        self.assertEqual((rc, line), (1, 'result=precheck_failed:usage'))

    def test_installed_version_line_and_tools_copy(self):
        self.install_beta()
        version = (self.usb / 'bundle-version').read_text().strip()
        self.assertRegex(version, r'^\d+\.\d+\.\d+$')
        self.assertIn('version=%s\n' % version, (self.base / 'installed.txt').read_text())
        self.assertEqual((self.base / 'tools/rebaseline.sh').read_bytes(), (self.usb / 'rebaseline.sh').read_bytes())

    def test_invalid_bundle_version_is_rejected_before_changes(self):
        (self.usb / 'bundle-version').write_text('1.0.4; reboot\n')
        (self.usb / 'bundle-default-mode').write_text('BETA\n')
        result = self.menu('1\n0\n')
        self.assertIn('Invalid bundle version', result.stdout + result.stderr)
        self.assertFalse((self.base / 'installed.txt').exists())

    def test_developer_bundle_without_version_has_no_version_line(self):
        (self.usb / 'bundle-version').unlink()
        self.install_beta()
        self.assertNotIn('version=', (self.base / 'installed.txt').read_text())

    def test_matching_files_check_match_and_register_is_a_no_op(self):
        self.installed_product()
        before = self.guard_files()
        self.assertEqual(self.rebaseline('--check')[:2], (0, 'result=match'))
        self.assertEqual(self.rebaseline()[:2], (0, 'result=done'))
        self.assertEqual(self.guard_files(), before)
        self.assertFalse((self.base / 'backups/persist-evidence').exists())

    def test_other_tool_edit_is_declined_then_re_registered(self):
        self.installed_product()
        old_state = (self.base / 'guard/persist-state').read_text()
        edited = self.other_tool_edit()
        rc, line, _ = self.rebaseline('--check')
        self.assertEqual((rc, line), (4, 'result=mismatch:baseline_edited:sm.conf'))
        # Without re-registration the next boot runs stock (fail closed).
        self.set_boot(2)
        self.assertIsNone(self.autostart_select())
        self.assertIn('reason=baseline_edited:sm.conf', (self.base / 'guard/last-decision').read_text())
        rc, line, result = self.rebaseline()
        self.assertEqual((rc, line), (0, 'result=done'), result.stderr)
        self.assertIn('probation boot', result.stdout)
        self.assertEqual(self.rebaseline('--check')[:2], (0, 'result=match'))
        state = (self.base / 'guard/persist-state').read_text()
        self.assertIn('probation=yes', state)
        self.assertIn('enabled_boot=%s' % boot_id(2), state)
        kept = self.base / 'backups/persist-evidence/1'
        self.assertEqual((kept / 'persist-state').read_text(), old_state)
        self.assertIn('verify_before=baseline_edited:sm.conf', (kept / 'rebaseline.txt').read_text())
        # The edited file itself is never touched; the next boot selects a
        # trial built from it, with the product preloads.
        self.assertIn('other tool: service trimmed', edited.read_text())
        self.assertNotIn('libmx5dr', edited.read_text())
        self.set_boot(3)
        trial = self.autostart_select()
        self.assertIsNotNone(trial)
        text = Path(trial).read_text()
        self.assertIn('other tool: service trimmed', text)
        self.assertIn('libmx5dr.so', text)
        self.assertIn('libmx5dr-vimtap.so', text)
        self.assertIn('libmx5dr-ldstap.so', text)

    def test_owner_switches_and_phone_channel_form_are_kept(self):
        self.installed_product()
        for name in ('g3-off', 'acc-cap-off'):
            (self.base / name).write_text('off\n')
        markers = {p.name: p.read_bytes() for p in self.base.iterdir() if p.is_file() and p.name.endswith('-off')}
        had_bt = 'configFilePath' in (self.base / 'guard/normal.trial').read_text()
        self.other_tool_edit('sm_WCP.conf')
        self.assertEqual(self.rebaseline()[:2], (0, 'result=done'))
        self.assertEqual({p.name: p.read_bytes() for p in self.base.iterdir()
                          if p.is_file() and p.name.endswith('-off')}, markers)
        self.assertEqual('configFilePath' in (self.base / 'guard/normal.trial').read_text(), had_bt)
        self.assertEqual('configFilePath' in (self.base / 'guard/wcp.trial').read_text(), had_bt)

    def test_trip_outranks_mismatch_and_is_never_cleared(self):
        self.installed_product()
        (self.root / 'data/dmesg.out').write_text('reset during boot 1\n')
        self.set_boot(2)
        self.assertIsNotNone(self.autostart_select())
        (self.root / 'data/thread_info.out').write_text('reset during boot 2\n')
        self.set_boot(3)
        self.assertIsNone(self.autostart_select())
        self.other_tool_edit()
        before = self.guard_files()
        self.assertEqual(self.rebaseline('--check')[:2], (2, 'result=tripped:reset_reports'))
        self.assertEqual(self.rebaseline()[:2], (2, 'result=tripped:reset_reports'))
        self.assertEqual(self.guard_files(), before)

    def test_prechecks_change_nothing(self):
        self.installed_product()
        self.other_tool_edit()
        before = self.guard_files()

        def expect(reason):
            rc, line, result = self.rebaseline()
            self.assertEqual((rc, line), (1, 'result=precheck_failed:' + reason), result.stderr)
            self.assertEqual(self.guard_files(), before)
            if reason != 'locked':
                self.assertFalse((self.root / 'data_persist/.mx5dr-install-lock').exists())

        # Another menu holds the install lock: no waiting.
        lock = self.root / 'data_persist/.mx5dr-install-lock'
        lock.mkdir()
        expect('locked')
        lock.rmdir()
        # Interrupted installation transaction.
        (self.base / 'pending').write_text('x\n')
        expect('pending')
        (self.base / 'pending').unlink()
        # Preload bytes other than the installed ones are never authorized.
        lib = self.base / 'libmx5dr.so'
        original = lib.read_bytes()
        lib.write_bytes(original + b'tampered')
        expect('payload')
        lib.write_bytes(original)
        conf = self.base / 'mx5dr.conf'
        original_conf = conf.read_text()
        conf.write_text(original_conf.replace('sample_ms=1000', 'sample_ms=10'))
        expect('config')
        conf.write_text(original_conf)
        # Our preload inside the OEM file is an installation error.
        sm = self.root / 'jci/sm/sm.conf'
        original_sm = sm.read_text()
        sm.write_text(original_sm + '<!-- /data_persist/mx5-aa-dr/libmx5dr.so -->\n')
        expect('baseline_has_token')
        sm.write_text(original_sm)
        # A runtime stop request is the owner's to acknowledge (menu 1).
        (self.logs / 'disable-next-start').write_text('x')
        expect('runtime_disabled')
        self.assertEqual(self.rebaseline('--check')[:2], (1, 'result=unknown:runtime_disabled'))
        (self.logs / 'disable-next-start').unlink()
        # Only the persistent BETA installation is handled.
        installed = self.base / 'installed.txt'
        original_installed = installed.read_text()
        installed.write_text(original_installed.replace('policy=persistent', 'policy=one-boot'))
        expect('unsupported_policy')
        self.assertEqual(self.rebaseline('--check')[:2], (1, 'result=unknown:unsupported_policy'))
        installed.write_text(original_installed)
        # Still a clean mismatch afterwards, and registration then works.
        self.assertEqual(self.rebaseline('--check')[:2], (4, 'result=mismatch:baseline_edited:sm.conf'))
        self.assertEqual(self.rebaseline()[:2], (0, 'result=done'))

    def test_not_installed_and_not_enabled(self):
        self.install_beta()  # fixture installer: no guard enable, no persist
        self.assertEqual(self.rebaseline('--check')[:2], (1, 'result=unknown:not_enabled'))
        self.assertEqual(self.rebaseline()[:2], (1, 'result=precheck_failed:not_enabled'))
        (self.base / 'installed.txt').unlink()
        self.assertEqual(self.rebaseline()[:2], (1, 'result=precheck_failed:not_installed'))
        self.assertEqual(self.rebaseline('--check')[:2], (1, 'result=unknown:not_installed'))

    def test_failed_enable_is_partial_fail_closed_and_retryable(self):
        self.installed_product()
        self.other_tool_edit()
        rc, line, _ = self.rebaseline(env={'MX5DR_GUARD_FAIL_FSYNC': 'persist-state'})
        self.assertEqual((rc, line), (3, 'result=partial:enable'))
        # New templates, old enablement: the guard declines (stock).
        self.assertEqual(self.rebaseline('--check')[:2], (4, 'result=mismatch:binding_changed:sm.conf'))
        self.set_boot(2)
        self.assertIsNone(self.autostart_select())
        # A retry finishes the job.
        self.assertEqual(self.rebaseline()[:2], (0, 'result=done'))
        self.set_boot(3)
        self.assertIsNotNone(self.autostart_select())

    def test_concurrent_edit_during_template_build_publishes_nothing(self):
        self.installed_product()
        self.other_tool_edit()
        before = self.guard_files()
        commands = Path(self.menu_fixture.fixture.tmp.name) / 'commands'
        commands.mkdir()
        awk = commands / 'awk'
        awk.write_text('#!/bin/sh\n"$REAL_AWK" "$@"\nstatus=$?\ncase "$*" in *edit_service.awk*)\n'
                       ' if [ ! -e "$EDIT_MARKER" ]; then printf "<!-- again -->\\n" >> "$EDIT_TARGET"; '
                       'touch "$EDIT_MARKER"; fi;;\nesac\nexit "$status"\n')
        awk.chmod(0o755)
        env = dict(PATH=str(commands) + os.pathsep + os.environ['PATH'], REAL_AWK=shutil.which('awk'),
                   EDIT_TARGET=str(self.root / 'jci/sm/sm.conf'), EDIT_MARKER=str(commands / 'done'))
        rc, line, _ = self.rebaseline(env=env)
        self.assertEqual((rc, line), (1, 'result=precheck_failed:concurrent_edit'))
        self.assertEqual(self.guard_files(), before)
        self.assertFalse(list((self.base / 'guard').glob('*.new.*')))


del P  # keep the borrowed TestCase out of this module's discovery


if __name__ == '__main__':
    unittest.main()
