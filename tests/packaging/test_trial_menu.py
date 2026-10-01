"""Numeric USB workflow with real helpers and synthetic firmware fixtures."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import unittest

import test_synthetic_install

BOOT = '12345678-1234-1234-1234-123456789abc\n'


class TrialMenuTests(unittest.TestCase):
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
        self.logs = self.base / 'logs'
        bootfile = self.root / 'proc/sys/kernel/random/boot_id'
        bootfile.parent.mkdir(parents=True)
        bootfile.write_text(BOOT)
        (self.root / 'proc/uptime').write_text('100.00 1.00\n')
        # Linux exposes /proc/mounts as a kernel-owned relative symlink. The
        # earlier regular-file fixture missed the actual menu3 startup failure.
        (self.root / 'proc/self').mkdir()
        self.mounts = self.root / 'proc/self/mounts'
        self.mounts.write_text(f'/dev/sdb1 {self.usb} vfat rw 0 0\n')
        (self.root / 'proc/mounts').symlink_to('self/mounts')
        self.env = dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root),
                        MX5DR_FIXTURE_FINISH_WAIT='0')

    def menu(self, keys):
        return subprocess.run(['sh', str(self.usb / 'trial')], input=keys,
                              text=True, capture_output=True, env=self.env,
                              cwd=self.root, timeout=20)

    def prepare_logs(self, acknowledged=True):
        self.fixture.run_script('install.sh')
        (self.logs / 'trace.0.jsonl').write_text('retained raw evidence\n')
        if acknowledged:
            (self.logs / 'capture.done').write_text(BOOT)

    def assert_export(self):
        archives = list(self.usb.glob('mx5dr-logs-*.tar'))
        self.assertEqual(len(archives), 1)
        archive = archives[0]
        checksum = archive.with_suffix('.tar.sha256').read_text().split()[0]
        self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(), checksum)
        with tarfile.open(archive) as tar:
            self.assertEqual(tar.extractfile('logs/trace.0.jsonl').read(),
                             b'retained raw evidence\n')
        self.assertEqual((self.logs / 'trace.0.jsonl').read_text(),
                         'retained raw evidence\n')
        return (self.usb / 'trial-result.txt').read_text()

    def test_eof_blank_and_invalid_input_never_install(self):
        for keys in ('', '\n0\n', 'bad\n0\n', '$(touch unexpected)\n0\n'):
            result = self.menu(keys)
            self.assertIn('1 Install', result.stdout)
            self.assertFalse(self.base.exists())
            self.assertEqual(self.fixture.autostart.read_bytes(),
                             self.fixture.original_autostart)
            self.assertFalse((self.root / 'unexpected').exists())

    def test_install_uses_bundle_mode_without_more_typed_commands(self):
        (self.usb / 'bundle-default-mode').write_text('SHADOW\n')
        result = self.menu('1\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('mode=SHADOW', (self.base / 'mx5dr.conf').read_text())
        self.assertIn('ONE-BOOT BEGIN', self.fixture.autostart.read_text())
        self.assertFalse(list(self.usb.glob('mx5dr-logs-*')))

    def test_install_failure_is_not_reported_as_success(self):
        (self.usb / 'libmx5dr.so').write_bytes(b'damaged')
        result = self.menu('1\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Truncated ELF header', result.stderr)
        self.assertEqual(self.fixture.autostart.read_bytes(),
                         self.fixture.original_autostart)

    def test_status_is_read_only_and_failure_remains_visible(self):
        self.prepare_logs()
        before = self.fixture.autostart.read_bytes()
        result = self.menu('2\n0\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('capture_active=unavailable', result.stdout)
        self.assertFalse((self.logs / 'capture.stop').exists())
        self.assertEqual(self.fixture.autostart.read_bytes(), before)
        self.assertFalse(list(self.usb.glob('mx5dr-logs-*')))

    def test_finish_exports_to_the_launching_usb_and_records_outcome(self):
        self.prepare_logs()
        self.assertTrue((self.root / 'proc/mounts').is_symlink())
        result = self.menu('3\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = self.assert_export()
        self.assertIn('finish_exit=0', report)
        self.assertIn('export_exit=0', report)
        self.assertTrue((self.logs / 'capture.stop').is_dir())

    def test_regular_mount_table_remains_compatible(self):
        mounts_alias = self.root / 'proc/mounts'
        mounts_alias.unlink()
        mounts_alias.write_bytes(self.mounts.read_bytes())
        self.prepare_logs()
        result = self.menu('3\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('export_exit=0', self.assert_export())

    def test_status_failure_does_not_block_finish_and_export(self):
        self.prepare_logs()
        result = self.menu('2\n3\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_export()

    def previous_boot_retrieval(self, acknowledged):
        self.prepare_logs(acknowledged=acknowledged)
        rows = [dict(kind='boot', boot_id=BOOT.strip(), mono_ns=1000000000, mode=4),
                dict(kind='position', mono_ns=99000000000, mode=1),
                dict(kind='motion_batch', schema=1, epoch=1, events=[[1, 1, 99000000000, 1, 0, 0, 0, 0, 1, 0]])]
        payload = ''.join(json.dumps(row, separators=(',', ':')) + '\n' for row in rows).encode()
        (self.logs / 'trace.0.jsonl').write_bytes(payload)
        (self.base / 'guard/last-boot').write_text(BOOT)
        (self.root / 'proc/sys/kernel/random/boot_id').write_text(BOOT.replace('12345678', '87654321'))
        (self.root / 'proc/uptime').write_text('2.00 1.00\n')
        before = self.fixture.autostart.read_bytes()
        # No menu2/arm/install command is issued on return; AA occupied the
        # single USB port during the authored previous-boot recording.
        result = self.menu('3\n')
        self.assertNotEqual(result.returncode, 0)  # Current-boot finish is unproved.
        report = (self.usb / 'trial-result.txt').read_text()
        self.assertIn('status_scope=current_boot', report)
        self.assertIn('export_scope=all_retained_boots', report)
        self.assertIn('retained_runtime_last_boot=previous_boot boot_id=' + BOOT.strip(), report)
        self.assertIn('position_records=1 motion_batches=1', report)
        self.assertIn('status_exit=1', report)
        self.assertIn('finish_exit=1', report)
        self.assertIn('export_exit=0', report)
        self.assertIn('Current status is not a verdict on the retained trial', result.stdout)
        archive, = self.usb.glob('mx5dr-logs-*.tar')
        self.assertEqual(archive.with_suffix('.tar.sha256').read_text().split()[0],
                         hashlib.sha256(archive.read_bytes()).hexdigest())
        with tarfile.open(archive) as tar:
            self.assertEqual(tar.extractfile('logs/trace.0.jsonl').read(), payload)
        self.assertEqual((self.logs / 'trace.0.jsonl').read_bytes(), payload)
        self.assertFalse((self.base / 'guard/arm').exists())
        self.assertEqual(self.fixture.autostart.read_bytes(), before)

    def test_single_port_reboot_return_exports_without_previous_ack(self):
        self.previous_boot_retrieval(acknowledged=False)

    def test_single_port_reboot_return_exports_without_accepting_old_ack(self):
        self.previous_boot_retrieval(acknowledged=True)

    def test_unconfirmed_finish_still_exports_and_retains_failure(self):
        self.prepare_logs(acknowledged=False)
        result = self.menu('3\n')
        self.assertNotEqual(result.returncode, 0)
        report = self.assert_export()
        self.assertIn('finish_exit=1', report)
        self.assertIn('export_exit=0', report)
        self.assertIn('Freeze not confirmed', report)

    def test_missing_usb_mount_does_not_freeze_or_fill_cmu_tmp(self):
        self.prepare_logs()
        self.mounts.write_text('rootfs / rootfs rw 0 0\n')
        result = self.menu('3\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('mounted USB', result.stdout + result.stderr)
        self.assertFalse((self.logs / 'capture.stop').exists())
        self.assertFalse(list(self.usb.glob('mx5dr-logs-*')))

    def test_other_mount_does_not_make_the_launching_usb_mounted(self):
        self.prepare_logs()
        for row in (f'/dev/sda1 {self.usb.parent}/sda1 vfat rw 0 0\n',
                    f'/dev/sdb1 {self.usb}/nested vfat rw 0 0\n',
                    f'/dev/mmcblk0p1 {self.usb} ext4 rw 0 0\n'):
            with self.subTest(mount=row):
                self.mounts.write_text(row)
                result = self.menu('3\n')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('mounted USB', result.stdout + result.stderr)
                self.assertFalse((self.logs / 'capture.stop').exists())
                self.assertFalse(list(self.usb.glob('mx5dr-logs-*')))

    def test_missing_mount_table_does_not_freeze_or_export(self):
        self.prepare_logs()
        self.mounts.unlink()  # The real /proc/mounts alias is now dangling.
        result = self.menu('3\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.logs / 'capture.stop').exists())
        self.assertFalse(list(self.usb.glob('mx5dr-logs-*')))

    def test_kernel_alias_exception_does_not_allow_a_symlink_usb_report(self):
        self.prepare_logs()
        sentinel = self.root / 'untouched-report'
        sentinel.write_bytes(b'preserve original report target\n')
        (self.usb / 'trial-result.txt').symlink_to(sentinel)
        result = self.menu('3\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Not a regular non-symlink file: ' + str(self.usb / 'trial-result.txt'),
                      result.stderr)
        self.assertEqual(sentinel.read_bytes(), b'preserve original report target\n')

    def test_export_after_remove_preserves_raw_logs_without_reinstall_or_rearm(self):
        self.prepare_logs(acknowledged=False)
        removed = self.menu('4\n')
        self.assertEqual(removed.returncode, 0, removed.stdout + removed.stderr)
        self.assertIn('mode=OFF', (self.base / 'mx5dr.conf').read_text())
        before = {path: path.read_bytes() for path in
                  (self.fixture.autostart, self.fixture.sm,
                   self.root / 'jci/sm/sm_WCP.conf', self.base / 'mx5dr.conf')}
        result = self.menu('3\n')  # No install, arm or diagnostic command.
        self.assertNotEqual(result.returncode, 0)  # Current-boot finish unproved.
        self.assertIn('export_exit=0', result.stdout)
        report = self.assert_export()
        self.assertIn('status_exit=1', report)
        self.assertIn('finish_exit=1', report)
        self.assertIn('export_scope=all_retained_boots', report)
        archive, = self.usb.glob('mx5dr-logs-*.tar')
        with tarfile.open(archive) as tar:
            self.assertEqual(tar.extractfile('mx5dr.conf').read(),
                             before[self.base / 'mx5dr.conf'])
        for path, contents in before.items():
            self.assertEqual(path.read_bytes(), contents)
        self.assertFalse((self.base / 'guard/arm').exists())
        self.assertFalse((self.base / 'pending').exists())
        self.assertFalse((self.base / 'installed.txt').exists())

    def test_export_failure_is_visible_and_logged(self):
        self.prepare_logs()
        (self.base / 'mx5dr.conf').unlink()
        result = self.menu('3\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.usb / 'trial-result.txt').is_file())
        report = (self.usb / 'trial-result.txt').read_text()
        self.assertIn('finish_exit=0', report)
        self.assertIn('export_exit=1', report)
        self.assertIn('Export failed', report)

    def test_remove_keeps_original_settings_and_logs(self):
        self.prepare_logs()
        result = self.menu('4\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.fixture.autostart.read_bytes(),
                         self.fixture.original_autostart)
        self.assertTrue((self.logs / 'trace.0.jsonl').exists())


if __name__ == '__main__':
    unittest.main()
