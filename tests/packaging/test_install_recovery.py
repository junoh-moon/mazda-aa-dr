"""Interrupted installation and removal (power loss) recover with number menus.

v1.0 stable (validation/V1_STABLE_AUDIT_2026-10-18.md risk 6): a power cut
during menu 1 left the install lock and/or the pending transaction marker,
and only menu 6 (which refuses while owned blocks exist) could reclaim the
lock. Menu 4 now reclaims a lock no live run owns (proof: it holds the kernel
mount lock every installer holds while its lock directory exists) and then
removes the package as usual; menu 1 succeeds afterwards.

Power loss is simulated by SIGKILL (no exit trap, no cleanup) injected before
every top-level statement of install.sh and uninstall.sh in a copy of the
script. Host shells (dash, and BusyBox ash when present) on synthetic
fixtures (SYNTHETIC, NOT OEM); the stock BusyBox emulation covers the CMU
shell separately. SIGKILL keeps every completed write, so this does not model
lost unsynced data; the atomic rename properties are what it exercises.
"""
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import unittest

import test_synthetic_install

SHELLS = [['sh']] + ([['busybox', 'sh']] if shutil.which('busybox') else [])
CLOSERS = ('fi', 'done', 'esac', 'else', 'elif', 'then', 'do', '}', ';;', ')')


def kill_points(text):
    """Line indexes before which a top-level `kill -KILL $$` is valid."""
    lines = text.split('\n')
    points = []
    for i, line in enumerate(lines):
        if i == 0 or not line or line[0] in ' \t#':
            continue
        if line.split()[0] in CLOSERS:
            continue
        previous = lines[i - 1].rstrip()
        if previous.endswith(('\\', '|', '&&', '||')):
            continue
        points.append(i)
    return points


class InstallRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.fixture = test_synthetic_install.SyntheticPackagingTests()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.root = self.fixture.root
        self.bundle = self.fixture.bundle
        (self.root / 'tmp').mkdir(exist_ok=True)
        self.base = self.root / 'data_persist/mx5-aa-dr'
        self.lock = self.root / 'data_persist/.mx5dr-install-lock'
        self.env = dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root), MX5DR_FIXTURE_MOUNT_LOCK='1')
        self.install_text = (self.bundle / 'install.sh').read_text()
        self.uninstall_text = (self.bundle / 'uninstall.sh').read_text()

    def script(self, name, *args, shell=('sh',), env=None):
        return subprocess.run(list(shell) + [str(self.bundle / name), *args], env=env or self.env,
                              capture_output=True, text=True, timeout=60)

    def killed_copy(self, name, text, index):
        lines = text.split('\n')
        lines.insert(index, 'kill -KILL $$')
        path = self.bundle / name
        path.write_text('\n'.join(lines))
        if subprocess.run(['sh', '-n', str(path)], capture_output=True).returncode != 0:
            return False
        return True

    def restore(self):
        (self.bundle / 'install.sh').write_text(self.install_text)
        (self.bundle / 'uninstall.sh').write_text(self.uninstall_text)

    def assert_stock_configs(self, context):
        self.assertEqual(self.fixture.sm.read_bytes(), self.fixture.original, context)
        self.assertEqual(self.fixture.wcp.read_bytes(), self.fixture.original_wcp, context)

    def reset_root(self):
        """Back to a never-installed CMU (stock files as before)."""
        shutil.rmtree(self.base, ignore_errors=True)
        if self.lock.exists():
            shutil.rmtree(self.lock)
        self.fixture.autostart.write_bytes(self.fixture.original_autostart)
        for path in list((self.root / 'usr/bin').glob('autostart.mx5dr-*')) + \
                list((self.root / 'jci/sm').glob('*.mx5dr-remove.*')):
            path.unlink()

    def test_install_killed_at_every_stage_recovers_with_menu_four_then_one(self):
        points = kill_points(self.install_text)
        self.assertGreater(len(points), 100)
        for shell in SHELLS:
            stages = 0
            # Every stage under the host sh; every fourth under BusyBox ash.
            for index in (points if shell == ['sh'] else points[::4]):
                context = '%s kill before install.sh line %d: %s' % (
                    shell[-1], index + 1, self.install_text.split('\n')[index][:60])
                with self.subTest(context=context):
                    self.reset_root()
                    if not self.killed_copy('install.sh', self.install_text, index):
                        continue
                    killed = self.script('install.sh', '--mode=BETA', shell=shell)
                    self.assertEqual(killed.returncode, -9, context + killed.stderr)
                    stages += 1
                    self.restore()
                    # Stock service configs are never edited by an install;
                    # autostart is either the original or the complete new one.
                    self.assert_stock_configs(context)
                    autostart = self.fixture.autostart.read_bytes()
                    self.assertTrue(autostart == self.fixture.original_autostart or
                                    b'MX5DR ONE-BOOT BEGIN' in autostart, context)
                    self.assertFalse((self.base / 'guard/persist').exists(), context)
                    # Menu 1 straight away either works or names menu 4.
                    retry = self.script('install.sh', '--mode=BETA', shell=shell)
                    if retry.returncode != 0:
                        self.assertTrue('choose 4, then 1 again' in retry.stderr, context + retry.stderr)
                        self.assertEqual(self.fixture.autostart.read_bytes(), autostart, context)
                    removed = self.script('uninstall.sh', shell=shell)
                    self.assertEqual(removed.returncode, 0, context + removed.stderr + removed.stdout)
                    self.assertEqual(self.fixture.autostart.read_bytes(), self.fixture.original_autostart, context)
                    self.assert_stock_configs(context)
                    self.assertFalse(self.lock.exists(), context)
                    self.assertFalse((self.base / 'pending').exists(), context)
                    installed = self.script('install.sh', '--mode=BETA', shell=shell)
                    self.assertEqual(installed.returncode, 0, context + installed.stderr)
                    self.assertTrue((self.base / 'installed.txt').exists(), context)
                    for name in ('bt-off', 'g3-garage-off'):
                        self.assertEqual((self.base / name).read_bytes(), b'off\n', context)
                    self.assertFalse(self.lock.exists(), context)
                    self.assertIn(b'MX5DR ONE-BOOT BEGIN', self.fixture.autostart.read_bytes(), context)
                    self.assert_stock_configs(context)
            self.assertGreater(stages, 100 if shell == ['sh'] else 25, shell)

    def test_uninstall_killed_at_every_stage_recovers_with_menu_four(self):
        points = kill_points(self.uninstall_text)
        for shell in SHELLS:
            stages = 0
            for index in points:
                context = '%s kill before uninstall.sh line %d: %s' % (
                    shell[-1], index + 1, self.uninstall_text.split('\n')[index][:60])
                with self.subTest(context=context):
                    self.reset_root()
                    self.assertEqual(self.script('install.sh', '--mode=BETA', shell=shell).returncode, 0)
                    (self.base / 'diag').write_text('on\n')
                    if not self.killed_copy('uninstall.sh', self.uninstall_text, index):
                        continue
                    killed = self.script('uninstall.sh', shell=shell)
                    self.assertEqual(killed.returncode, -9, context + killed.stderr)
                    stages += 1
                    self.restore()
                    self.assert_stock_configs(context)
                    removed = self.script('uninstall.sh', shell=shell)
                    self.assertEqual(removed.returncode, 0, context + removed.stderr + removed.stdout)
                    self.assertEqual(self.fixture.autostart.read_bytes(), self.fixture.original_autostart, context)
                    self.assertFalse(self.lock.exists(), context)
                    for name in ('bt-off', 'g3-garage-off', 'diag'):
                        self.assertFalse((self.base / name).exists(), context)
                    self.assertEqual(self.script('install.sh', '--mode=BETA', shell=shell).returncode, 0, context)
            self.assertGreater(stages, 15, shell)

    def leave_lock(self, pid=True):
        self.lock.mkdir()
        if pid:
            (self.lock / 'pid').write_text('4242\n')

    def test_menu_four_reclaims_an_empty_or_pid_only_lock_and_pending(self):
        for pid in (False, True):
            with self.subTest(pid=pid):
                self.reset_root()
                self.assertEqual(self.script('install.sh', '--mode=BETA').returncode, 0)
                self.leave_lock(pid)
                (self.base / 'pending').write_text('persistent install: baseline configs and autostart\n')
                blocked = self.script('install.sh', '--mode=BETA')
                self.assertNotEqual(blocked.returncode, 0)
                self.assertIn('Install lock present', blocked.stderr)
                self.assertIn('choose 4, then 1 again', blocked.stderr)
                removed = self.script('uninstall.sh')
                self.assertEqual(removed.returncode, 0, removed.stderr)
                self.assertIn('Removed an install lock left by an interrupted run.', removed.stdout)
                self.assertIn('Recovered an interrupted run', removed.stdout)
                self.assertFalse(self.lock.exists())
                self.assertFalse((self.base / 'pending').exists())
                self.assertEqual(self.fixture.autostart.read_bytes(), self.fixture.original_autostart)
                self.assertEqual(self.script('install.sh', '--mode=BETA').returncode, 0)

    def test_pending_alone_names_menu_four(self):
        self.assertEqual(self.script('install.sh', '--mode=BETA').returncode, 0)
        (self.base / 'pending').write_text('persistent install: baseline configs and autostart\n')
        blocked = self.script('install.sh', '--mode=BETA')
        self.assertNotEqual(blocked.returncode, 0)
        self.assertIn('Incomplete transaction (an earlier install was interrupted): choose 4, then 1 again', blocked.stderr)
        self.assertEqual(self.script('uninstall.sh').returncode, 0)
        self.assertEqual(self.script('install.sh', '--mode=BETA').returncode, 0)

    def test_lock_of_a_live_run_is_never_reclaimed(self):
        # The live run holds the kernel mount lock: menu 4 stops before any
        # change (autostart keeps the installed blocks, the lock stays).
        self.assertEqual(self.script('install.sh', '--mode=BETA').returncode, 0)
        installed = self.fixture.autostart.read_bytes()
        self.leave_lock()
        mount_lock = self.root / 'tmp/.mx5dr-mount.lock'
        mount_lock.touch()
        with open(mount_lock, 'rb') as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            removed = self.script('uninstall.sh')
        self.assertNotEqual(removed.returncode, 0)
        self.assertIn('Another installer or USB export is active', removed.stderr)
        self.assertTrue((self.lock / 'pid').exists())
        self.assertEqual(self.fixture.autostart.read_bytes(), installed)

    def test_lock_without_proof_or_with_foreign_entries_is_refused(self):
        self.assertEqual(self.script('install.sh', '--mode=BETA').returncode, 0)
        installed = self.fixture.autostart.read_bytes()
        self.leave_lock()
        # A fixture without the kernel lock hook cannot prove the owner gone.
        env = dict(self.env)
        del env['MX5DR_FIXTURE_MOUNT_LOCK']
        removed = self.script('uninstall.sh', env=env)
        self.assertNotEqual(removed.returncode, 0)
        self.assertIn('owner cannot be checked', removed.stderr)
        self.assertTrue((self.lock / 'pid').exists())
        self.assertEqual(self.fixture.autostart.read_bytes(), installed)
        for extra in ('other', '..hidden', '.x'):
            with self.subTest(extra=extra):
                (self.lock / extra).write_text('not ours\n')
                removed = self.script('uninstall.sh')
                self.assertNotEqual(removed.returncode, 0)
                self.assertIn('Install lock holds unexpected entries; nothing changed', removed.stderr)
                self.assertTrue((self.lock / extra).exists())
                self.assertEqual(self.fixture.autostart.read_bytes(), installed)
                (self.lock / extra).unlink()
        (self.lock / 'pid').unlink()
        (self.lock / 'pid').mkdir()
        removed = self.script('uninstall.sh')
        self.assertNotEqual(removed.returncode, 0)
        self.assertIn('unexpected entries', removed.stderr)
        (self.lock / 'pid').rmdir()
        self.lock.rmdir()
        self.lock.symlink_to(self.root / 'tmp')
        removed = self.script('uninstall.sh')
        self.assertNotEqual(removed.returncode, 0)
        self.assertIn('Install lock is not a directory', removed.stderr)
        self.assertEqual(self.fixture.autostart.read_bytes(), installed)
        self.assertTrue((self.root / 'tmp').is_dir())


if __name__ == '__main__':
    unittest.main()
