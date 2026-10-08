"""Menu 6 complete removal on synthetic fixtures (host shell, not the CMU).

Every refusal must delete nothing; a successful run removes the package
directory and the package's own leftovers outside it, never OEM files or
another tool's files, and a second run has nothing to delete.
"""
import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import stat
import subprocess
import unittest

import test_synthetic_install

BOOT = '12345678-1234-1234-1234-123456789abc\n'
MENU6 = '6 Delete everything this package left on the CMU (run 4, then 5, first)'
CONFIRM = ('Menu 6 deletes the collected logs too.\nRun 3 first to save them to this USB.\n'
           'Enter 6 again to delete everything,\nanything else cancels.\n')


def tree(root, skip):
    """paths -> (kind, mode, sha256 | link target); directory mtimes excluded."""
    found = {}
    for top, dirs, files in os.walk(root):
        rel_top = os.path.relpath(top, root)
        dirs[:] = sorted(d for d in dirs if os.path.normpath(os.path.join(rel_top, d)) not in skip)
        for name in dirs + files:
            path = Path(top) / name
            rel = os.path.normpath(os.path.join(rel_top, name))
            if rel in skip:
                continue
            info = path.lstat()
            mode = stat.S_IMODE(info.st_mode)
            if stat.S_ISLNK(info.st_mode):
                found[rel] = ('link', mode, os.readlink(path))
            elif stat.S_ISDIR(info.st_mode):
                found[rel] = ('dir', mode, '')
            else:
                try:
                    digest = hashlib.sha256(path.read_bytes()).hexdigest()
                except PermissionError:
                    digest = 'unreadable %d %d' % (info.st_size, info.st_mtime_ns)
                found[rel] = ('file', mode, digest)
    return found


class PurgeTests(unittest.TestCase):
    def setUp(self):
        self.fixture = test_synthetic_install.SyntheticPackagingTests()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.root = self.fixture.root
        self.usb = self.root / 'tmp/mnt/sdb1'
        self.usb.parent.mkdir(parents=True)
        shutil.move(str(self.fixture.bundle), self.usb)
        self.fixture.bundle = self.usb
        self.base = self.root / 'data_persist/mx5-aa-dr'
        (self.root / 'proc/sys/kernel/random/boot_id').write_text(BOOT)
        (self.root / 'proc/uptime').write_text('100.00 1.00\n')
        (self.root / 'proc/self').mkdir()
        (self.root / 'proc/self/mounts').write_text(f'/dev/sdb1 {self.usb} vfat rw 0 0\n')
        (self.root / 'proc/mounts').symlink_to('self/mounts')
        # Someone else's files that menu 6 must never touch.
        oem = self.root / 'data_persist/oem-aa-mod'
        oem.mkdir()
        (oem / 'libpatch-blmjciaapa.so').write_bytes(b'third-party touch patch\n')
        (oem / 'libpatch.conf').write_text('touch=1\n')
        (self.root / 'data').mkdir()
        (self.root / 'data/dmesg.out').write_text('SM reset report\n')
        (self.root / 'tmp/smevents.txt').write_text('stock\n')
        # A stock process that uses only OEM files.
        self.process(266, maps='00008000-0000a000 r-xp 00000000 1f:02 77 /jci/sm/sm\n',
                     fds={'0': '/dev/null', '3': '/tmp/smevents.txt'},
                     cmdline='/jci/sm/sm\0-f\0/jci/sm/sm.conf\0')
        self.env = dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root))
        self.skip = {'proc', 'tmp/mnt/sdb1'}

    def process(self, pid, maps='', fds=None, cmdline=''):
        proc = self.root / 'proc' / str(pid)
        proc.mkdir(parents=True)
        (proc / 'maps').write_text(maps)
        (proc / 'cmdline').write_text(cmdline)
        (proc / 'fd').mkdir()
        for number, target in (fds or {}).items():
            (proc / 'fd' / number).symlink_to(target)
        return proc

    def menu(self, keys):
        return subprocess.run(['sh', str(self.usb / 'trial')], input=keys, text=True,
                              capture_output=True, env=self.env, cwd=self.root, timeout=60)

    def purge(self):
        return subprocess.run(['sh', str(self.usb / 'purge.sh'), str(self.usb)], text=True,
                              capture_output=True, env=self.env, cwd=self.root, timeout=60)

    def install_and_uninstall(self, *args):
        self.fixture.run_script('install.sh', *args)
        (self.base / 'logs/trace.0.jsonl').write_text('raw evidence\n')
        self.fixture.run_script('uninstall.sh')

    def assert_refused(self, result, code, reason):
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        self.assertIn('Refused, nothing deleted: ' + reason, result.stdout)

    def test_menu_lists_six_and_runs_purge(self):
        clean = tree(self.root, self.skip)
        result = self.menu('0\n')
        self.assertIn(MENU6, result.stdout)
        self.install_and_uninstall('--mode=SHADOW')
        result = self.menu('6\n6\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(CONFIRM, result.stdout)
        self.assertIn('Package directory absent', result.stdout)
        self.assertIn('OEM files: identical to pre-install', result.stdout)
        for line in result.stdout[result.stdout.index('---- DELETE RESULT ----'):].splitlines():
            self.assertLessEqual(len(line), 40, line)
        self.assertFalse(self.base.exists())
        self.assertEqual(tree(self.root, self.skip), clean)
        report = (self.usb / 'purge-result.txt').read_text()
        self.assertIn('status=finished', report)
        self.assertIn('verdict: identical to all 1 recorded pre-install states', report)
        self.assertIn('after_package_directory=absent', report)
        self.assertIn('/data_persist/mx5-aa-dr/logs/trace.0.jsonl', report)
        # Idempotent: the second run has nothing to delete and changes nothing.
        again = self.menu('6\n6\n')
        self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
        self.assertIn('Nothing to delete', again.stdout)
        self.assertEqual((self.usb / 'purge-result.txt').read_text(), report)
        self.assertEqual(tree(self.root, self.skip), clean)

    def test_menu_six_needs_a_second_six(self):
        # Only a second line that is exactly 6 deletes. Anything else (another
        # digit, an empty line, a word, 6 with a space, end of input) cancels
        # with nothing deleted and no report.
        self.install_and_uninstall('--mode=BETA')
        before = tree(self.root, self.skip)
        for keys in ('6\n7\n0\n', '6\n\n0\n', '6\nsix\n0\n', '6\n6 \n0\n', '6\n 6\n0\n', '6\n66\n0\n',
                     '6\n3\n0\n', '6\n'):
            with self.subTest(keys=keys):
                result = self.menu(keys)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(CONFIRM, result.stdout)
                self.assertIn('Cancelled; nothing deleted.', result.stdout)
                self.assertNotIn('DELETE RESULT', result.stdout)
                self.assertEqual(tree(self.root, self.skip), before)
                self.assertFalse((self.usb / 'purge-result.txt').exists())
        # The statement comes before the second prompt, and every line fits
        # the 40-column screen.
        result = self.menu('6\n0\n')
        self.assertLess(result.stdout.index(CONFIRM), result.stdout.rindex('Number, then Enter: '))
        for line in CONFIRM.splitlines():
            self.assertLessEqual(len(line), 40, line)
        result = self.menu('6\n6\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.base.exists())

    def test_unexpected_entry_in_the_lock_is_reported_not_aborted(self):
        # The final rmdir of the install lock fails when something else put a
        # file into it; set -e must not abort before the result (2026-10-08
        # menu 6 re-review, LOW).
        self.install_and_uninstall()
        lock = self.root / 'data_persist/.mx5dr-install-lock'
        hook = self.root.parent / 'after-lock-extra.sh'
        hook.write_text(f": > '{lock}/..extra'\n")
        result = subprocess.run(['sh', str(self.usb / 'purge.sh'), str(self.usb)], text=True, capture_output=True,
                                env=dict(self.env, MX5DR_FIXTURE_AFTER_LOCK=str(hook)), cwd=self.root, timeout=60)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('---- DELETE RESULT ----', result.stdout)
        self.assertIn('Package directory absent', result.stdout)
        self.assertIn('Install lock STILL PRESENT', result.stdout)
        self.assertIn('Deletion incomplete; see purge-result.txt', result.stderr)
        self.assertFalse(self.base.exists())
        self.assertTrue((lock / '..extra').exists())
        self.assertFalse((lock / 'pid').exists())
        report = (self.usb / 'purge-result.txt').read_text()
        self.assertTrue(report.startswith('purge_schema=1\nstatus=incomplete\n'), report[:80])
        self.assertIn('after_install_lock=present', report)
        self.assertIn('result=incomplete', report)

    def test_totals_match_the_deleted_files(self):
        self.install_and_uninstall('--mode=BETA')
        files = [p for p in self.base.rglob('*') if p.is_file() and not p.is_symlink()]
        total = sum(p.stat().st_size for p in files)
        result = self.purge()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f'Deleted {len(files)} files, {total} bytes', result.stdout)
        report = (self.usb / 'purge-result.txt').read_text()
        self.assertIn(f'delete_total_files={len(files)} delete_total_bytes={total}', report)

    def refusal_keeps_everything(self, prepare, code, reason):
        prepare()
        before = tree(self.root, self.skip)
        result = self.purge()
        self.assert_refused(result, code, reason)
        self.assertEqual(tree(self.root, self.skip), before)
        self.assertFalse((self.usb / 'purge-result.txt').exists())
        return result

    def test_refuses_while_installed(self):
        self.refusal_keeps_everything(lambda: self.fixture.run_script('install.sh', '--mode=BETA'),
                                      3, 'autostart has mx5dr blocks')

    def test_refuses_while_persist_or_arm_present(self):
        for marker in ('persist', 'arm'):
            with self.subTest(marker=marker):
                def prepare():
                    self.install_and_uninstall('--mode=BETA')
                    (self.base / 'guard' / marker).write_text('mx5dr-one-boot-v3\n')
                self.refusal_keeps_everything(prepare, 3, f'guard/{marker} present')
                shutil.rmtree(self.base)

    def test_refuses_with_tokens_in_either_service_config(self):
        for name in ('sm.conf', 'sm_WCP.conf'):
            with self.subTest(name=name):
                config = self.root / 'jci/sm' / name
                original = config.read_bytes()

                def prepare():
                    self.install_and_uninstall()
                    config.write_text(config.read_text().replace(
                        '</service>', '<environ_var env_name="LD_PRELOAD" '
                        'env_value="/data_persist/mx5-aa-dr/libmx5dr-vimtap.so"/>\n</service>', 1))
                self.refusal_keeps_everything(prepare, 3, name + ' has mx5dr tokens')
                config.write_bytes(original)
                shutil.rmtree(self.base)

    def test_refuses_when_a_process_maps_or_opens_a_package_file(self):
        cases = {
            'maps': dict(maps='b6f00000-b6f80000 r-xp 00000000 1f:05 12 '
                              '/tmp/mnt/data_persist/mx5-aa-dr/libmx5dr.so (deleted)\n'),
            'fd': dict(fds={'4': '/tmp/mnt/data_persist/mx5-aa-dr/logs/trace.0.jsonl'}),
            'cmdline': dict(cmdline='/jci/sm/sm\0-f\0/tmp/mx5dr-trial-AbC123/sm.conf\0'),
        }
        for entry, kwargs in cases.items():
            with self.subTest(entry=entry):
                def prepare():
                    self.install_and_uninstall()
                    self.process(900, **kwargs)
                self.refusal_keeps_everything(prepare, 4, f'process 900 uses a package file ({entry})')
                shutil.rmtree(self.root / 'proc/900')
                shutil.rmtree(self.base)

    def test_processes_are_checked_again_under_the_lock(self):
        self.install_and_uninstall()
        hook = self.root.parent / 'after-lock.sh'
        proc = self.root / 'proc/903'
        hook.write_text(f'mkdir -p {proc}/fd\n: > {proc}/cmdline\n'
                        f"printf '%s\\n' 'b6000000-b6080000 r-xp 0 1f:05 9 /data_persist/mx5-aa-dr/libmx5dr.so' > {proc}/maps\n")
        before = tree(self.root, self.skip | {'proc'})
        result = subprocess.run(['sh', str(self.usb / 'purge.sh'), str(self.usb)], text=True, capture_output=True,
                                env=dict(self.env, MX5DR_FIXTURE_AFTER_LOCK=str(hook)), cwd=self.root, timeout=60)
        self.assert_refused(result, 4, 'process 903 uses a package file (maps)')
        self.assertEqual(tree(self.root, self.skip | {'proc'}), before)
        self.assertFalse((self.root / 'data_persist/.mx5dr-install-lock').exists())
        # The report written before the lock now says refused.
        report = (self.usb / 'purge-result.txt').read_text()
        self.assertIn('status=refused', report)
        self.assertIn('result=refused, nothing deleted: process 903 uses a package file (maps)', report)

    def test_unwritable_usb_aborts_before_any_deletion(self):
        self.install_and_uninstall()
        before = tree(self.root, self.skip)
        self.usb.chmod(0o555)
        try:
            result = self.purge()
        finally:
            self.usb.chmod(0o755)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('Cannot write purge-result.txt on the USB (full, read-only or removed?); nothing deleted',
                      result.stderr)
        self.assertEqual(tree(self.root, self.skip), before)
        self.assertFalse((self.usb / 'purge-result.txt').exists())

    def test_final_report_failure_says_the_deletion_completed(self):
        self.install_and_uninstall()
        result = subprocess.run(['sh', str(self.usb / 'purge.sh'), str(self.usb)], text=True, capture_output=True,
                                env=dict(self.env, MX5DR_FIXTURE_FAIL_REPORT='finished'), cwd=self.root, timeout=60)
        self.assertEqual(result.returncode, 1)
        self.assertIn('---- DELETE RESULT ----\nDeleted ', result.stdout)
        self.assertIn('Package directory absent', result.stdout)
        self.assertIn('the deletion DID complete', result.stderr)
        self.assertFalse(self.base.exists())
        self.assertIn('status=started', (self.usb / 'purge-result.txt').read_text())

    def test_first_report_failure_changes_nothing(self):
        # The report is attempted before the lock is reclaimed or taken.
        self.install_and_uninstall()
        result = subprocess.run(['sh', str(self.usb / 'purge.sh'), str(self.usb)], text=True, capture_output=True,
                                env=dict(self.env, MX5DR_FIXTURE_FAIL_REPORT='started'), cwd=self.root, timeout=60)
        self.assertEqual(result.returncode, 1)
        self.assertIn('nothing deleted', result.stderr)
        self.assertTrue((self.base / 'logs/trace.0.jsonl').exists())
        self.assertFalse((self.root / 'data_persist/.mx5dr-install-lock').exists())

    def test_refuses_when_an_fd_directory_cannot_be_read(self):
        def prepare():
            self.install_and_uninstall()
            proc = self.process(904)
            shutil.rmtree(proc / 'fd')
            (proc / 'fd').write_text('not a directory\n')
        self.refusal_keeps_everything(prepare, 4, 'cannot read /proc/904/fd')

    def test_refuses_when_process_entries_cannot_be_read(self):
        def prepare():
            self.install_and_uninstall()
            (self.process(901) / 'maps').unlink()
        self.refusal_keeps_everything(prepare, 4, 'cannot read /proc/901/maps')

    def test_refuses_without_any_process_entry(self):
        def prepare():
            self.install_and_uninstall()
            shutil.rmtree(self.root / 'proc/266')
        self.refusal_keeps_everything(prepare, 4, 'no readable /proc process entries')

    def test_refuses_while_collector_holds_its_lock(self):
        self.install_and_uninstall()
        lock = self.base / 'logs/collector.lock'
        lock.touch()
        with open(lock, 'rb') as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            before = tree(self.root, self.skip)
            self.assert_refused(self.purge(), 5, 'collector is running')
            self.assertEqual(tree(self.root, self.skip), before)
        result = self.purge()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_refuses_while_install_lock_is_held(self):
        def prepare():
            self.install_and_uninstall()
            lock = self.root / 'data_persist/.mx5dr-install-lock'
            lock.mkdir()
            (lock / 'pid').write_text('4242\n')
        self.refusal_keeps_everything(prepare, 6, 'install lock is held')

    def test_refuses_a_file_system_mounted_inside_the_package(self):
        def prepare():
            self.install_and_uninstall()
            (self.base / 'logs/usb').mkdir()
            mounts = self.root / 'proc/self/mounts'
            mounts.write_text(mounts.read_text() + f'/dev/sdc1 {self.base.resolve()}/logs/usb vfat rw 0 0\n')
        self.refusal_keeps_everything(prepare, 8, 'a file system is mounted inside the package directory')

    def test_refuses_when_a_service_config_is_not_a_regular_file(self):
        for name in ('sm.conf', 'sm_WCP.conf'):
            with self.subTest(name=name):
                config = self.root / 'jci/sm' / name
                original = config.read_bytes()
                staged = self.root / 'jci/sm' / (name + '.mx5dr-remove.55')

                def prepare():
                    self.install_and_uninstall()
                    staged.write_bytes(original)  # possibly the only copy left
                    config.unlink()
                self.refusal_keeps_everything(prepare, 7, f'/jci/sm/{name} is not a regular file')
                self.assertEqual(staged.read_bytes(), original)
                staged.unlink()
                config.write_bytes(original)
                shutil.rmtree(self.base)

    def test_unreadable_autostart_or_config_refuses(self):
        # Stock BusyBox 1.19.2 grep returns 1 (as for "no match") for an
        # unreadable file or a directory, so reads go through cat instead.
        if os.geteuid() == 0:
            self.skipTest('root reads mode 000 files')
        for name in ('usr/bin/autostart', 'jci/sm/sm.conf', 'jci/sm/sm_WCP.conf'):
            with self.subTest(name=name):
                path = self.root / name

                def prepare():
                    self.install_and_uninstall()
                    path.chmod(0)
                try:
                    self.refusal_keeps_everything(prepare, 7, 'cannot read /' + name)
                finally:
                    path.chmod(0o644)
                shutil.rmtree(self.base)

    def test_refuses_symlinked_package_directory(self):
        elsewhere = self.root / 'elsewhere'
        elsewhere.mkdir()
        (elsewhere / 'keep').write_text('not ours to follow\n')
        self.refusal_keeps_everything(lambda: self.base.symlink_to(elsewhere), 7, 'the package directory is a symlink')

    def test_touch_mod_edit_is_reported_not_restored(self):
        self.fixture.add_touch()
        self.install_and_uninstall('--mode=BETA')
        edited = self.fixture.sm.read_text().replace('libpatch-blmjciaapa.so', 'libpatch-blmjciaapa.so:/data_persist/oem-aa-mod/extra.so')
        self.fixture.sm.write_text(edited)
        result = self.purge()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('OEM files differ from pre-install:', result.stdout)
        self.assertIn('  sm.conf (kept as it is)', result.stdout)
        self.assertEqual(self.fixture.sm.read_text(), edited)
        report = (self.usb / 'purge-result.txt').read_text()
        self.assertIn('verdict: differs from the pre-install state: /jci/sm/sm.conf (changed after installation', report)
        self.assertRegex(report, r'compare \S+ /jci/sm/sm_WCP.conf identical')
        self.assertRegex(report, r'compare \S+ /usr/bin/autostart identical')
        self.assertNotIn('identical to all', report)

    def test_upgrade_leftovers_and_external_staging_files_are_deleted(self):
        # A v0.3.12-shadow.5 one-boot tree, several installs, one interrupted.
        self.install_and_uninstall('--mode=SHADOW')
        self.install_and_uninstall('--mode=BETA')
        guard = self.base / 'guard'
        for name in ('consumed', 'armed-boot', 'armed-boot.previous', 'last-boot'):
            (guard / name).write_text(BOOT)
        (guard / '.persist-state.321').write_text('torn\n')
        (guard / 'lock').write_text('')
        (self.base / 'pending').write_text('one-boot install: baseline configs and autostart\n')
        sets = sorted(p.name for p in (self.base / 'backups').iterdir() if p.name[0].isdigit())
        self.assertEqual(len(sets), 2)
        partial = self.base / 'backups/19700101T002334-12846'
        partial.mkdir()
        shutil.copy(self.fixture.sm, partial / 'sm.conf.before')
        # A record taken while owned blocks were present is not the original.
        blocked = self.base / 'backups/19700101T000001-1'
        shutil.copytree(self.base / 'backups' / sets[0], blocked)
        text = (blocked / 'autostart.before').read_text() + '# MX5DR ONE-BOOT BEGIN SMCFG_NORMALMODE\n'
        (blocked / 'autostart.before').write_text(text)
        (blocked / 'autostart.before.sha256').write_text(hashlib.sha256(text.encode()).hexdigest() + '\n')
        evidence = self.base / 'backups/persist-evidence/1'
        evidence.mkdir(parents=True, exist_ok=True)
        (evidence / 'persist-state').write_text('old\n')
        ours = [self.root / 'usr/bin/autostart.mx5dr-remove.77', self.root / 'usr/bin/autostart.mx5dr-new.78',
                self.root / 'jci/sm/sm.conf.mx5dr-remove.77', self.root / 'jci/sm/sm_WCP.conf.mx5dr-remove.77.tap.77',
                self.root / 'jci/sm/sm.conf.mx5dr-remove.79.lds.79', self.root / 'tmp/mx5-lds-association-Qw3rTy']
        for path in ours:
            path.write_text('staged\n')
        for directory, inner in (('tmp/mx5dr-trial-1u7LnL', 'sm.conf'), ('tmp/mx5dr-hash.aB3dEf', 'hash')):
            (self.root / directory).mkdir()
            (self.root / directory / inner).write_text('tmp\n')
        not_ours = [self.root / 'jci/sm/sm.conf.mx5dr-remove.x7', self.root / 'jci/sm/sm.conf.bak',
                    self.root / 'usr/bin/autostart.orig', self.root / 'tmp/mx5dr-trial-zzzzzz/extra']
        not_ours[-1].parent.mkdir()
        for path in not_ours:
            path.write_text('someone else\n')
        result = self.purge()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.base.exists())
        for path in ours + [self.root / 'tmp/mx5dr-trial-1u7LnL', self.root / 'tmp/mx5dr-hash.aB3dEf']:
            self.assertFalse(path.exists(), path)
        for path in not_ours:
            self.assertEqual(path.read_text(), 'someone else\n')
        self.assertEqual((self.root / 'data_persist/oem-aa-mod/libpatch.conf').read_text(), 'touch=1\n')
        self.assertEqual((self.root / 'data/dmesg.out').read_text(), 'SM reset report\n')
        self.assertEqual(self.fixture.autostart.read_bytes(), self.fixture.original_autostart)
        self.assertFalse((self.root / 'data_persist/.mx5dr-install-lock').exists())
        report = (self.usb / 'purge-result.txt').read_text()
        self.assertIn(f'compare_records= {sets[0]} {sets[1]}', report)
        self.assertIn('verdict: identical to all 2 recorded pre-install states', report)
        self.assertIn('compare_complete_records=2', report)
        self.assertIn('compare_records_agree=yes', report)
        self.assertIn('compare_incomplete_or_not_original= 19700101T000001-1 19700101T002334-12846', report)
        for name in ('guard/consumed', 'guard/.persist-state.321', 'pending', 'backups/persist-evidence/1/persist-state'):
            self.assertIn('/data_persist/mx5-aa-dr/' + name, report)
        self.assertIn('file 7 /usr/bin/autostart.mx5dr-remove.77', report)
        self.assertIn('dir 0 /tmp/mx5dr-trial-1u7LnL', report)
        self.assertIn('unexpected, left as is: /tmp/mx5dr-trial-zzzzzz', report)
        self.assertIn('unexpected, left as is: /jci/sm/sm.conf.mx5dr-remove.x7', report)
        self.assertIn('Nothing to delete', self.purge().stdout)

    def test_decoy_names_and_symlinks_are_never_followed_or_expanded(self):
        # Review 2026-10-07: names were joined into one word list and expanded
        # again, so "x jci1" deleted ./jci1 and "a * bc" globbed the cwd.
        self.install_and_uninstall('--mode=BETA')
        victim = self.root / 'jci1'
        victim.mkdir()
        (victim / 'oemfile').write_text('OEM\n')
        decoys = ['mx5dr-trial-x jci1', 'mx5dr-trial-a * bc', 'mx5dr-trial-ab?def', 'mx5dr-trial-ab\ncde',
                  'mx5dr-trial--abcde', 'mx5dr-trial-abcdefg', 'mx5dr-trial-abcd', 'mx5dr-hash.ab*def']
        for name in decoys:
            (self.root / 'tmp' / name).mkdir()
            (self.root / 'tmp' / name / 'sm.conf').write_text('decoy\n')
        (self.root / 'tmp/mx5dr-hash.xxxxxx').symlink_to(self.root / 'jci')
        (self.root / 'tmp/mx5-lds-association-abcdef').symlink_to(self.root / 'data/dmesg.out')
        (self.root / 'tmp/mx5dr-trial-Lnk123').symlink_to(self.root / 'jci/sm')
        for name in ('jci/sm/sm.conf.mx5dr-remove.1 2', 'jci/sm/sm.conf.mx5dr-remove.*',
                     'usr/bin/autostart.mx5dr-new.-1', 'usr/bin/autostart.mx5dr-new.7.tap.x'):
            (self.root / name).write_text('decoy\n')
        (self.root / 'usr/bin/autostart.mx5dr-remove.8').symlink_to(self.root / 'usr/bin/autostart')
        # Links inside the package directory point at OEM data; rm -rf removes
        # the links only.
        (self.base / 'logs/evil').symlink_to(self.root / 'jci')
        (self.base / 'evil2').symlink_to(self.root / 'data/dmesg.out')
        owned = {'tmp/mx5dr-trial-Ab12Z9', 'tmp/mx5dr-trial-Ab12Z9/sm.conf'}
        (self.root / 'tmp/mx5dr-trial-Ab12Z9').mkdir()
        (self.root / 'tmp/mx5dr-trial-Ab12Z9/sm.conf').write_text('ours\n')
        before = tree(self.root, self.skip)
        result = self.purge()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        after = tree(self.root, self.skip)
        package = 'data_persist/mx5-aa-dr'
        gone = {k for k in before if k not in after}
        self.assertEqual({k for k in gone if k != package and not k.startswith(package + '/')}, owned)
        self.assertEqual({k: v for k, v in after.items()},
                         {k: v for k, v in before.items() if k not in gone})
        self.assertEqual((victim / 'oemfile').read_text(), 'OEM\n')
        report = (self.usb / 'purge-result.txt').read_text()
        self.assertIn('status=finished', report)
        for name in ('mx5dr-trial-x jci1', 'mx5dr-trial-a * bc', 'mx5dr-hash.xxxxxx', 'mx5dr-trial-Lnk123'):
            self.assertIn('unexpected, left as is: /tmp/' + name, report.replace(str(self.root), ''))
        self.assertIn('unexpected, left as is: /usr/bin/autostart.mx5dr-remove.8', report.replace(str(self.root), ''))

    def test_disagreeing_records_are_reported(self):
        # Review 2026-10-07: original install (A), another tool edits
        # sm_WCP.conf, reinstall (B) whose name sorts first after a clock
        # restart, uninstall. The verdict must not claim "identical".
        self.install_and_uninstall()
        self.fixture.wcp.write_text(self.fixture.wcp.read_text() + '<!-- edit -->\n')
        self.install_and_uninstall()
        backups = self.base / 'backups'
        first, second = sorted(p.name for p in backups.iterdir() if p.name[0].isdigit())
        (backups / second).rename(backups / '19700101T000001-9')
        result = self.purge()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('identical', result.stdout)
        self.assertIn('Pre-install records disagree.\nNothing restored. Current files match:\n'
                      '  19700101T000001-9\n', result.stdout)
        report = (self.usb / 'purge-result.txt').read_text()
        self.assertIn('compare_records_agree=no', report)
        self.assertIn('compare_complete_records=2', report)
        self.assertIn(f'verdict: recorded pre-install states disagree: 19700101T000001-9 {first}; '
                      'current files match: 19700101T000001-9 (nothing restored)', report)
        self.assertIn(f'compare {first} /jci/sm/sm_WCP.conf differs', report)
        self.assertNotIn('verdict: identical', report)
        # Neither record matches after a further edit.
        self.assertEqual(self.purge().returncode, 0)

    def test_disagreeing_records_with_no_match(self):
        self.install_and_uninstall()
        self.fixture.wcp.write_text(self.fixture.wcp.read_text() + '<!-- edit -->\n')
        self.install_and_uninstall()
        self.fixture.sm.write_text(self.fixture.sm.read_text() + '<!-- later -->\n')
        result = self.purge()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Nothing restored. Current files match:\n  none\n', result.stdout)
        self.assertIn('current files match: none (nothing restored)', (self.usb / 'purge-result.txt').read_text())

    def test_report_keeps_an_interrupted_earlier_report(self):
        self.install_and_uninstall()
        (self.usb / 'purge-result.txt').write_text('purge_schema=1\nstatus=started\nverdict: identical to the pre-install state\n')
        result = self.purge()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = (self.usb / 'purge-result.txt').read_text()
        self.assertTrue(report.startswith('purge_schema=1\nstatus=finished\n'), report[:80])
        self.assertIn('---- previous purge-result.txt ----\npurge_schema=1\nstatus=started\n', report)

    def test_partial_directory_after_power_loss_is_completed(self):
        self.install_and_uninstall('--mode=BETA')
        # Power cut after the guard directory went away: inert, menu 6 finishes.
        shutil.rmtree(self.base / 'guard')
        result = self.purge()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.base.exists())


if __name__ == '__main__':
    unittest.main()
