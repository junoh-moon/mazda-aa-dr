"""Explicit reboot requests; every executable endpoint is an authored witness."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


PACK = Path(__file__).resolve().parents[2] / 'packaging'
BOOT = '12345678-1234-1234-1234-123456789abc'


class RebootCmuTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'root'
        self.root.mkdir()
        (self.root / '.mx5dr-fixture').touch()
        self.usb = self.root / 'tmp/mnt/sdb1'
        self.usb.mkdir(parents=True)
        shutil.copyfile(PACK / 'common.sh', self.usb / 'common.sh')
        if (PACK / 'reboot_cmu.sh').exists():
            shutil.copyfile(PACK / 'reboot_cmu.sh', self.usb / 'reboot_cmu.sh')
        self.proc = self.root / 'proc'
        (self.proc / 'self').mkdir(parents=True)
        self.mounts = self.proc / 'self/mounts'
        self.mounts.write_text(f'/dev/sdb1 {self.usb} vfat rw 0 0\n')
        (self.proc / 'mounts').symlink_to('self/mounts')
        self.boot = self.proc / 'sys/kernel/random/boot_id'
        self.boot.parent.mkdir(parents=True)
        self.boot.write_text(BOOT + '\n')
        (self.proc / 'uptime').write_text('123.45 88.00\n')
        self.base = self.root / 'data_persist/mx5-aa-dr'
        (self.base / 'guard').mkdir(parents=True)
        (self.base / 'guard/arm').write_bytes(b'preserved arm\n')
        (self.base / 'logs').mkdir()
        (self.base / 'logs/trace.0.jsonl').write_bytes(b'preserved raw\n')
        self.order = Path(self.tmp.name) / 'order.log'
        self.commands = Path(self.tmp.name) / 'commands'
        self.commands.mkdir()
        self.write_command(self.commands / 'sync',
                           'printf "sync\\n" >> "$ORDER_LOG"\nexit "${SYNC_EXIT:-0}"\n')
        self.write_command(self.commands / 'mount',
                           'printf "mount %s\\n" "$*" >> "$ORDER_LOG"\n'
                           'case "$*" in *remount,ro*) exit "${RESTORE_EXIT:-0}";; esac\n')
        self.write_command(self.commands / 'reboot',
                           'printf "FORBIDDEN_PATH_REBOOT\\n" >> "$ORDER_LOG"\nexit 99\n')
        (self.root / 'sbin').mkdir()
        self.reboot = self.root / 'sbin/reboot'
        self.write_command(self.reboot,
                           'printf "reboot argc=%s args=%s\\n" "$#" "$*" >> "$ORDER_LOG"\n'
                           'cat "$RECEIPT" > "$REBOOT_SNAPSHOT"\nexit "${REBOOT_EXIT:-0}"\n')
        self.env = dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root),
                        PATH=str(self.commands) + os.pathsep + os.environ['PATH'],
                        ORDER_LOG=str(self.order), RECEIPT=str(self.usb / 'reboot-request.txt'),
                        REBOOT_SNAPSHOT=str(Path(self.tmp.name) / 'receipt-at-reboot'))

    def write_command(self, path, body):
        path.write_text('#!/bin/sh\n' + body)
        path.chmod(0o755)

    def run_helper(self, *args, changes=None):
        env = dict(self.env)
        if changes:
            env.update(changes)
        return subprocess.run(['sh', str(self.usb / 'reboot_cmu.sh'), *args],
                              env=env, capture_output=True, text=True, timeout=5)

    def events(self):
        return self.order.read_text().splitlines() if self.order.exists() else []

    def receipt(self):
        data = (self.usb / 'reboot-request.txt').read_bytes()
        self.assertLessEqual(len(data), 512)
        lines = data.decode().splitlines()
        values = dict(line.split('=', 1) for line in lines)
        self.assertEqual(len(values), len(lines))
        self.assertEqual(values['reboot_schema'], '1')
        self.assertEqual(values['request'], 'normal')
        self.assertEqual(values['completion'], 'unconfirmed')
        self.assertFalse(list(self.usb.glob('reboot-request.txt.tmp.*')))
        return values

    def remount_seam(self, body=None):
        # Host fixture common.mount_rw deliberately does nothing. This authored
        # seam tests the helper's transaction/EXIT-trap ordering; original stock
        # common.mount_rw execution remains a separate target integration check.
        if body is None:
            body = 'mount -o remount,rw "$1" || return $?; REMOUNTED="$1"'
        with (self.usb / 'common.sh').open('a') as stream:
            stream.write('\nmount_rw() { ' + body + '; }\n')

    def test_saves_bounded_receipt_syncs_then_requests_normal_reboot_once(self):
        before = {p.relative_to(self.base): p.read_bytes()
                  for p in (self.base / 'guard/arm', self.base / 'logs/trace.0.jsonl')}
        result = self.run_helper()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        values = self.receipt()
        self.assertEqual(values['boot_id_status'], 'valid')
        self.assertEqual(values['boot_id'], BOOT)
        self.assertEqual(values['uptime_status'], 'valid')
        self.assertEqual(values['uptime_seconds'], '123.45')
        events = self.events()
        self.assertEqual(events[-1], 'reboot argc=0 args=')
        self.assertEqual(sum(e.startswith('reboot ') for e in events), 1)
        self.assertIn('sync', events[:-1])
        self.assertEqual(Path(self.env['REBOOT_SNAPSHOT']).read_bytes(),
                         (self.usb / 'reboot-request.txt').read_bytes())
        self.assertIn('reboot_command_exit=0', result.stdout)
        self.assertIn('reboot_completion=unconfirmed', result.stdout)
        self.assertEqual({p: (self.base / p).read_bytes() for p in before}, before)

    def test_missing_or_bad_boot_information_does_not_block_reboot(self):
        for data in (None, b'bad\n', BOOT.encode() + b'\nextra\n', b'x' * 4096):
            with self.subTest(data=None if data is None else len(data)):
                if self.boot.exists():
                    self.boot.unlink()
                if data is not None:
                    self.boot.write_bytes(data)
                result = self.run_helper()
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                values = self.receipt()
                self.assertEqual(values['boot_id_status'], 'unavailable')
                self.assertEqual(values['boot_id'], 'unavailable')
                self.assertEqual(values['uptime_seconds'], '123.45')

    def test_fifo_boot_and_invalid_uptime_remain_diagnostic_only(self):
        self.boot.unlink()
        os.mkfifo(self.boot)
        (self.proc / 'uptime').write_text('bad unsafe value\n')
        result = self.run_helper()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        values = self.receipt()
        self.assertEqual(values['boot_id_status'], 'unavailable')
        self.assertEqual(values['uptime_status'], 'unavailable')
        self.assertEqual(values['uptime_seconds'], 'unavailable')

    def test_mount_table_allows_normal_kernel_alias_and_regular_file(self):
        alias = self.proc / 'mounts'
        alias.unlink()
        alias.write_bytes(self.mounts.read_bytes())
        result = self.run_helper()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.receipt()['boot_id'], BOOT)

    def test_usb_absent_wrong_device_or_shadowed_mount_never_writes_tmpfs(self):
        for table in ('tmpfs /tmp tmpfs rw 0 0\n',
                      f'/dev/mmcblk0p2 {self.usb} relfs rw 0 0\n',
                      f'/dev/sdb1 {self.usb} vfat rw 0 0\ntmpfs {self.usb} tmpfs rw 0 0\n'):
            with self.subTest(table=table):
                self.mounts.write_text(table)
                result = self.run_helper()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('mounted USB', result.stderr)
                self.assertFalse((self.usb / 'reboot-request.txt').exists())
                self.assertFalse(self.events())

    def test_detached_usb_during_remount_is_rechecked_before_receipt(self):
        self.remount_seam('printf "tmpfs /tmp tmpfs rw 0 0\\n" > "$ROOT/proc/self/mounts"')
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('mounted USB', result.stderr)
        self.assertFalse((self.usb / 'reboot-request.txt').exists())
        self.assertFalse(self.events())

    def test_original_readonly_mount_is_restored_before_reboot(self):
        self.remount_seam()
        result = self.run_helper()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        events = self.events()
        readonly = events.index('mount -o remount,ro ' + str(self.usb))
        reboot = events.index('reboot argc=0 args=')
        self.assertLess(readonly, reboot)
        self.assertIn('sync', events[:readonly])
        self.assertEqual(reboot, len(events) - 1)

    def test_sync_or_readonly_restoration_failure_prevents_reboot(self):
        self.remount_seam()
        for changes in ({'SYNC_EXIT': '9'}, {'RESTORE_EXIT': '8'}):
            with self.subTest(changes=changes):
                self.order.unlink(missing_ok=True)
                result = self.run_helper(changes=changes)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(e.startswith('reboot ') for e in self.events()))
                self.assertIn('mount -o remount,ro ' + str(self.usb), self.events())

    def test_failure_of_original_reboot_is_propagated(self):
        result = self.run_helper(changes={'REBOOT_EXIT': '23'})
        self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
        self.assertEqual(self.receipt()['completion'], 'unconfirmed')
        self.assertIn('reboot_command_exit=23', result.stdout)
        self.assertEqual(sum(e.startswith('reboot ') for e in self.events()), 1)

    def test_repeated_explicit_request_replaces_one_receipt(self):
        first = self.run_helper()
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        new_boot = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa'
        self.boot.write_text(new_boot + '\n')
        second = self.run_helper()
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(self.receipt()['boot_id'], new_boot)
        self.assertEqual(len(list(self.usb.glob('reboot-request*'))), 1)

    def test_receipt_symlink_and_directory_do_not_modify_other_storage(self):
        target = self.base / 'logs/trace.0.jsonl'
        receipt = self.usb / 'reboot-request.txt'
        receipt.symlink_to(target)
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(target.read_bytes(), b'preserved raw\n')
        self.assertFalse(any(e.startswith('reboot ') for e in self.events()))
        receipt.unlink()
        receipt.mkdir()
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(e.startswith('reboot ') for e in self.events()))

    def test_fixture_requires_explicit_regular_reboot_witness(self):
        self.reboot.unlink()
        missing = self.run_helper()
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn('fixture reboot witness', missing.stderr)
        self.assertFalse(self.events())
        self.reboot.symlink_to(self.commands / 'reboot')
        link = self.run_helper()
        self.assertNotEqual(link.returncode, 0)
        self.assertIn('fixture reboot witness', link.stderr)
        self.assertFalse(self.events())
        self.assertFalse((self.usb / 'reboot-request.txt').exists())

    def test_fixture_rejects_redirected_sbin_parent(self):
        # This safe external witness stands for any directory outside ROOT;
        # the helper must not follow a fixture sbin alias into host programs.
        external = Path(self.tmp.name) / 'external-sbin'
        (self.root / 'sbin').rename(external)
        (self.root / 'sbin').symlink_to(external)
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('fixture reboot witness', result.stderr)
        self.assertFalse(self.events())
        self.assertFalse((self.usb / 'reboot-request.txt').exists())

    def test_arguments_and_unmarked_fixture_never_request_reboot(self):
        bad_args = self.run_helper('-f')
        self.assertNotEqual(bad_args.returncode, 0)
        self.assertIn('Usage:', bad_args.stderr)
        (self.root / '.mx5dr-fixture').unlink()
        bad_root = self.run_helper()
        self.assertNotEqual(bad_root.returncode, 0)
        self.assertIn('marked fixture root', bad_root.stderr)
        self.assertFalse(self.events())
        self.assertFalse((self.usb / 'reboot-request.txt').exists())


if __name__ == '__main__':
    unittest.main()
