"""Read-only recovery facts from real helpers and explicitly authored filesystems."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import test_synthetic_install

REPO = Path(__file__).resolve().parents[2]
BOOT = '12345678-1234-1234-1234-123456789abc\n'
MANIFEST = 'mx5dr-one-boot-v2\n' + ''.join(f'{i:064x}\n' for i in range(1, 8))


class StartupDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        self.fixture = test_synthetic_install.SyntheticPackagingTests()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.fixture.run_script('install.sh', '--mode=SHADOW')
        self.root = self.fixture.root
        (self.root / 'tmp/mnt').mkdir(parents=True)
        (self.root / 'data_persist').rename(self.root / 'tmp/mnt/data_persist')
        (self.root / 'data_persist').symlink_to('/mnt/data_persist')
        (self.root / 'mnt').symlink_to('/tmp/mnt')
        self.base = self.root / 'tmp/mnt/data_persist/mx5-aa-dr'
        self.guard = self.base / 'guard'
        self.proc = self.root / 'proc'
        (self.proc / 'self').mkdir(parents=True)
        (self.proc / 'sys/kernel/random').mkdir(parents=True)
        (self.proc / 'sys/kernel/random/boot_id').write_text(BOOT)
        self.mounts = self.proc / 'self/mounts'
        self.mounts.write_text('rootfs / rootfs rw 0 0\n'
            '/dev/root / relfs ro 0 0\ntmpfs /tmp tmpfs rw 0 0\n'
            '/dev/mmcblk0p2 /tmp/mnt/data_persist relfs rw,noatime 0 0\n')
        (self.proc / 'mounts').symlink_to('self/mounts')
        self.mountinfo = self.proc / 'self/mountinfo'
        self.mountinfo.write_text('1 0 0:1 / / rw - rootfs rootfs rw\n'
            '2 1 0:2 / /tmp rw - tmpfs tmpfs rw\n'
            '3 2 179:2 / /tmp/mnt/data_persist rw,noatime - relfs /dev/mmcblk0p2 rw,noatime\n')
        (self.guard / 'last-boot').write_text(BOOT)
        (self.guard / 'consumed').write_text(MANIFEST)
        self.env = dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root))

    def run_diagnostics(self, env=None):
        result = subprocess.run(['/bin/sh', str(self.fixture.bundle / 'startup_diagnostics.sh')],
            env=env or self.env, text=True, capture_output=True, timeout=15)
        values = {}
        for line in result.stdout.splitlines():
            self.assertIn('=', line, line)
            key, value = line.split('=', 1)
            self.assertNotIn(key, values, 'duplicate schema key: ' + key)
            values[key] = value
        self.assertLessEqual(len(result.stdout.encode()), 32768)
        return result, values

    def test_stock_alias_and_kernel_mounts_link_produce_read_only_facts(self):
        targets = [self.base / 'mx5dr.conf', self.guard / 'consumed',
                   self.guard / 'last-boot', self.fixture.autostart,
                   self.guard / 'normal.trial']
        before = {p: p.read_bytes() for p in targets}
        result, values = self.run_diagnostics()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(values['diagnostic_schema'], '1')
        self.assertEqual(values['scope'], 'current_recovery_boot')
        self.assertEqual(values['persist_resolved'], '/tmp/mnt/data_persist')
        self.assertEqual(values['persist_mount.source'], '/dev/mmcblk0p2')
        self.assertEqual(values['persist_mount.target'], '/tmp/mnt/data_persist')
        self.assertEqual(values['persist_mount.fs'], 'relfs')
        self.assertEqual(values['persist_mount.access'], 'rw')
        self.assertEqual(values['persist_mount.noexec'], 'false')
        self.assertEqual(values['persist_mount.coverage'], 'exact')
        self.assertEqual(values['persist_mount.table'], 'mountinfo')
        self.assertEqual(values['path.data_persist.type'], 'symlink')
        self.assertEqual(values['path.mnt.type'], 'symlink')
        self.assertEqual(values['guard.last_boot.value'], BOOT.strip())
        self.assertEqual(values['guard.arm.status'], 'missing')
        self.assertEqual(values['guard.consumed.status'], 'valid')
        for index in range(1, 8):
            self.assertEqual(values[f'guard.consumed.input_{index}'], f'{index:064x}')
        self.assertEqual({p: p.read_bytes() for p in targets}, before)
        self.assertFalse((self.guard / 'arm').exists())
        self.assertFalse((self.base / 'logs/capture.stop').exists())
        self.assertFalse((self.base.parent / '.mx5dr-install-lock').exists())

    def test_hashes_cover_all_seven_guard_inputs_without_printing_bodies(self):
        config = self.base / 'mx5dr.conf'
        config.write_text('PRIVATE_CONFIG_SENTINEL\n')
        self.fixture.autostart.write_text('PRIVATE_OEM_STARTUP_SENTINEL\n')
        result, values = self.run_diagnostics()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        inputs = dict(libmx5dr=self.base / 'libmx5dr.so', config=config,
            sm_normal=self.fixture.sm, template_normal=self.guard / 'normal.trial',
            sm_wcp=self.fixture.wcp, template_wcp=self.guard / 'wcp.trial',
            vimtap=self.base / 'libmx5dr-vimtap.so')
        for key, path in inputs.items():
            self.assertEqual(values[f'hash.{key}.sha256'], hashlib.sha256(path.read_bytes()).hexdigest())
        for key in ('autostart', 'collector', 'guard', 'tool_common_sh', 'tool_mx5dr_sha256'):
            self.assertEqual(values[f'hash.{key}.status'], 'ok')
        self.assertNotIn('PRIVATE_', result.stdout + result.stderr)

    def test_marker_and_binary_permissions_are_observed_without_claiming_guard_admission(self):
        (self.guard / 'arm').write_text(MANIFEST)
        (self.guard / 'arm').chmod(0o666)
        (self.base / 'libmx5dr.so').chmod(0o664)
        result, values = self.run_diagnostics()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(values.get('path.guard.arm.type'), 'file')
        self.assertEqual(values.get('path.guard.arm.mode'), '666')
        self.assertEqual(values.get('path.guard.arm.uid'), str(os.getuid()))
        self.assertEqual(values.get('path.hash.libmx5dr.mode'), '664')
        self.assertEqual(values['guard.arm.status'], 'valid')  # format only
        self.assertEqual((self.guard / 'arm').stat().st_mode & 0o777, 0o666)

    def test_no_flash_mount_reports_actual_tmpfs_ancestor_without_claiming_persistence(self):
        self.mountinfo.write_text('1 0 0:1 / / rw - rootfs rootfs rw\n'
            '2 1 0:2 / /tmp rw,noexec - tmpfs tmpfs rw\n')
        result, values = self.run_diagnostics()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(values['persist_mount.target'], '/tmp')
        self.assertEqual(values['persist_mount.fs'], 'tmpfs')
        self.assertEqual(values['persist_mount.coverage'], 'ancestor')
        self.assertEqual(values['persist_mount.noexec'], 'true')
        self.assertEqual(values['persist_mount.persistence'], 'not_inferred')

    def test_mounts_fallback_follows_normal_symlink_and_last_equal_length_entry(self):
        self.mountinfo.unlink()
        self.mounts.write_text('rootfs / rootfs rw 0 0\n/dev/root / relfs ro 0 0\n'
            'tmpfs /tmp tmpfs rw 0 0\n'
            '/dev/old /tmp/mnt/data_persist relfs rw 0 0\n'
            '/dev/mmcblk0p2 /tmp/mnt/data_persist relfs ro,noexec 0 0\n'
            '/dev/unrelated /tmp/mnt/data_persist-other relfs rw 0 0\n')
        result, values = self.run_diagnostics()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(values['mountinfo.status'], 'missing')
        self.assertEqual(values['persist_mount.table'], 'mounts')
        self.assertEqual(values['persist_mount.source'], '/dev/mmcblk0p2')
        self.assertEqual(values['persist_mount.access'], 'ro')
        self.assertEqual(values['persist_mount.noexec'], 'true')

    def test_missing_owned_state_is_reported_without_creating_it(self):
        shutil.rmtree(self.base)
        result, values = self.run_diagnostics()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(values['path.base.type'], 'missing')
        self.assertEqual(values['guard.arm.status'], 'parent_unavailable')
        self.assertEqual(values['boot_id.value'], BOOT.strip())
        self.assertEqual(values['hash.autostart.status'], 'ok')
        self.assertFalse(self.base.exists())

    def test_guard_parent_symlink_does_not_read_external_marker(self):
        target = self.root / 'external-private'
        self.guard.rename(target)
        self.guard.symlink_to(target)
        result, values = self.run_diagnostics()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(values['path.guard.type'], 'symlink')
        self.assertEqual(values['guard.consumed.status'], 'parent_unavailable')
        self.assertNotIn('guard.consumed.input_1', values)
        self.assertEqual((target / 'consumed').read_text(), MANIFEST)

    def test_fifo_marker_is_not_opened_and_does_not_hide_other_facts(self):
        (self.guard / 'consumed').unlink()
        os.mkfifo(self.guard / 'consumed')
        result, values = self.run_diagnostics()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(values['guard.consumed.status'], 'not_regular')
        self.assertEqual(values['guard.last_boot.value'], BOOT.strip())
        self.assertEqual(values['hash.autostart.status'], 'ok')

    def test_bad_manifest_shape_and_oversize_are_partial_without_body_output(self):
        cases = [b'PRIVATE_SECRET\n', MANIFEST.replace('0001\n', '000A\n').encode(),
                 (MANIFEST + '0' * 64 + '\n').encode(), b'x' * 4096,
                 MANIFEST.rstrip('\n').encode(), MANIFEST.encode() + b'\x00']
        for data in cases:
            with self.subTest(length=len(data)):
                (self.guard / 'consumed').write_bytes(data)
                result, values = self.run_diagnostics()
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(values['guard.consumed.status'], ('malformed', 'oversized'))
                self.assertNotIn('guard.consumed.input_1', values)
                self.assertNotIn('PRIVATE_SECRET', result.stdout + result.stderr)

    def test_bad_boot_and_source_sha_do_not_echo_untrusted_values(self):
        (self.guard / 'last-boot').write_text('PRIVATE_BOOT\n')
        (self.guard / 'normal.source.sha256').write_text('PRIVATE_SHA\n')
        result, values = self.run_diagnostics()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(values['guard.last_boot.status'], 'malformed')
        self.assertEqual(values['guard.normal_source.status'], 'malformed')
        self.assertNotIn('PRIVATE_', result.stdout + result.stderr)

    def test_unexpected_guard_entry_is_counted_without_name_or_contents(self):
        (self.guard / 'PRIVATE_NAME').write_text('PRIVATE_BODY')
        result, values = self.run_diagnostics()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(values['guard.inventory.status'], 'unexpected_entries')
        self.assertEqual(values['guard.inventory.unexpected_count'], '1')
        self.assertNotIn('PRIVATE_', result.stdout + result.stderr)

    def test_newline_in_unexpected_name_cannot_impersonate_two_allowed_names(self):
        (self.guard / 'last-boot\narm').write_text('PRIVATE_BODY')
        result, values = self.run_diagnostics()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(values['guard.inventory.status'], 'unexpected_entries')
        self.assertEqual(values['guard.inventory.unexpected_count'], '1')
        self.assertNotIn('PRIVATE_BODY', result.stdout + result.stderr)

    def test_unexpected_entry_count_is_bounded_and_other_diagnostics_survive(self):
        for index in range(70):
            (self.guard / f'unexpected-{index}').touch()
        result, values = self.run_diagnostics()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(values['guard.inventory.status'], 'too_many_entries')
        self.assertEqual(values['guard.inventory.unexpected_count'], 'at_least_65')
        self.assertEqual(values['hash.autostart.status'], 'ok')

    def test_failed_inventory_reader_is_not_reported_as_an_empty_directory(self):
        commands = self.root / 'failed-inventory-bin'
        commands.mkdir()
        fake = commands / 'find'
        fake.write_text('#!/bin/sh\nexit 3\n')
        fake.chmod(0o755)
        result, values = self.run_diagnostics(dict(self.env,
            PATH=str(commands) + os.pathsep + os.environ['PATH']))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(values['guard.inventory.status'], 'unavailable')
        self.assertNotIn('guard.inventory.unexpected_count', values)
        self.assertEqual(values['hash.autostart.status'], 'ok')

    def test_malformed_and_oversized_mount_tables_keep_other_facts(self):
        for body in ('PRIVATE_MOUNT_SECRET\n', 'x' * 70000):
            with self.subTest(length=len(body)):
                self.mountinfo.write_text(body)
                result, values = self.run_diagnostics()
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(values['mountinfo.status'], ('malformed', 'oversized'))
                self.assertEqual(values['guard.last_boot.value'], BOOT.strip())
                self.assertNotIn('PRIVATE_MOUNT_SECRET', result.stdout + result.stderr)

    def test_hash_target_programs_are_never_executed(self):
        marker = self.root / 'PROGRAM_RAN'
        for path in (self.guard / 'mx5dr-guard', self.base / 'mx5dr-collector', self.fixture.autostart):
            path.write_text('#!/bin/sh\ntouch ' + str(marker) + '\n')
            path.chmod(0o755)
        result, values = self.run_diagnostics()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(values['hash.guard.status'], 'ok')
        self.assertFalse(marker.exists())

    def test_native_sha_missing_uses_existing_bounded_temporary_helper_then_cleans_it(self):
        build = tempfile.TemporaryDirectory()
        self.addCleanup(build.cleanup)
        helper = Path(build.name) / 'mx5dr-sha256'
        subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
            str(REPO / 'src/tools/sha256_main.cpp'), str(REPO / 'src/runtime/sha256.cpp'),
            '-o', str(helper)], check=True)
        shutil.copyfile(helper, self.fixture.bundle / 'mx5dr-sha256')
        (self.fixture.bundle / 'mx5dr-sha256').chmod(0o644)
        commands = self.root / 'limited-bin'
        commands.mkdir()
        for command in ('dirname', 'readlink', 'stat', 'awk', 'dd', 'find', 'head',
                        'mktemp', 'cp', 'chmod', 'rm', 'rmdir', 'wc', 'tr'):
            (commands / command).symlink_to(shutil.which(command))
        before = set(Path('/tmp').glob('mx5dr-hash.*'))
        result, values = self.run_diagnostics(dict(self.env, PATH=str(commands)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(values['hash_backend'], 'temporary_bundled_helper')
        self.assertEqual(values['hash.config.sha256'],
                         hashlib.sha256((self.base / 'mx5dr.conf').read_bytes()).hexdigest())
        self.assertEqual(set(Path('/tmp').glob('mx5dr-hash.*')), before)

    def test_failed_sha_backend_leaves_metadata_and_reports_partial(self):
        commands = self.root / 'failed-hash-bin'
        commands.mkdir()
        fake = commands / 'sha256sum'
        fake.write_text('#!/bin/sh\nexit 9\n')
        fake.chmod(0o755)
        result, values = self.run_diagnostics(dict(self.env,
            PATH=str(commands) + os.pathsep + os.environ['PATH']))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(values['hash.config.status'], 'hash_failed')
        self.assertEqual(values['guard.last_boot.value'], BOOT.strip())
        self.assertEqual(values['diagnostic_status'], 'partial')


if __name__ == '__main__':
    unittest.main()
