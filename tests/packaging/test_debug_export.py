"""Recovery archive contents and failure behavior with authored filesystems."""
import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import unittest

import test_synthetic_install


class DebugExportTests(unittest.TestCase):
    def setUp(self):
        self.fixture = test_synthetic_install.SyntheticPackagingTests()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.fixture.run_script('install.sh', '--mode=SHADOW')
        self.root = self.fixture.root
        self.base = self.root / 'data_persist/mx5-aa-dr'
        self.dest = Path(self.fixture.tmp.name) / 'usb'
        self.dest.mkdir()
        self.env = dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root))
        (self.base / 'guard/arm').write_bytes(b'retained arm\n')
        (self.base / 'logs/trace.0.jsonl').write_bytes(b'raw input\n')
        (self.root / 'proc/self').mkdir(parents=True)
        (self.root / 'proc/self/mounts').write_text('tmpfs /tmp tmpfs rw 0 0\n')
        (self.root / 'proc/mounts').symlink_to('self/mounts')
        (self.root / 'proc/uptime').write_text('123.45 0.00\n')

    def run_export(self):
        return subprocess.run(['sh', str(self.fixture.bundle / 'export_logs.sh'),
                               str(self.dest)], capture_output=True, text=True,
                              env=self.env, timeout=20)

    def archive(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        archive, = self.dest.glob('mx5dr-logs-*.tar')
        self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(),
                         archive.with_suffix('.tar.sha256').read_text().split()[0])
        self.assertFalse(list(self.dest.glob('mx5dr-diagnostics-*')))
        return tarfile.open(archive)

    def member(self, tar, suffix):
        matches = [m for m in tar.getmembers() if m.name.endswith('/' + suffix)]
        self.assertEqual(len(matches), 1, (suffix, tar.getnames()))
        return matches[0]

    def test_entire_installation_and_current_oem_files_keep_bytes_and_metadata(self):
        (self.base / 'unexpected-owned-file').write_bytes(b'extra evidence')
        (self.base / 'guard/arm').chmod(0o600)
        files = [p for p in self.base.rglob('*') if p.is_file()]
        files += [self.fixture.autostart, self.fixture.sm, self.fixture.wcp]
        before = {p: (p.read_bytes(), p.stat().st_mode & 0o777,
                      p.stat().st_uid, p.stat().st_gid) for p in files}
        with self.archive(self.run_export()) as tar:
            for path, (data, mode, uid, gid) in before.items():
                member = self.member(tar, str(path.relative_to(self.root)))
                self.assertEqual(tar.extractfile(member).read(), data)
                self.assertEqual((member.mode, member.uid, member.gid), (mode, uid, gid))
                self.assertEqual(path.read_bytes(), data)

    def test_kernel_mount_alias_is_copied_as_bytes_with_current_boot_scope(self):
        with self.archive(self.run_export()) as tar:
            mounts = self.member(tar, 'proc/mounts.txt')
            self.assertTrue(mounts.isfile())
            self.assertEqual(tar.extractfile(mounts).read(), b'tmpfs /tmp tmpfs rw 0 0\n')
            report = tar.extractfile(self.member(tar, 'collection.txt')).read().decode()
            self.assertIn('scope=current_recovery_boot', report)
            self.assertIn('snapshot=sequential_nonatomic', report)

    def test_missing_current_sm_does_not_block_retained_raw_or_autostart(self):
        self.fixture.wcp.unlink()
        with self.archive(self.run_export()) as tar:
            self.assertEqual(tar.extractfile(self.member(tar, 'mx5-aa-dr/logs/trace.0.jsonl')).read(), b'raw input\n')
            self.member(tar, 'usr/bin/autostart')
            report = tar.extractfile(self.member(tar, 'collection.txt')).read().decode()
            self.assertIn('jci/sm/sm_WCP.conf=missing', report)

    def test_empty_or_missing_logs_still_collect_installation(self):
        import shutil
        shutil.rmtree(self.base / 'logs')
        with self.archive(self.run_export()) as tar:
            self.member(tar, 'mx5-aa-dr/guard/arm')
            self.member(tar, 'usr/bin/autostart')

    def test_missing_installation_still_collects_current_startup(self):
        import shutil
        shutil.rmtree(self.base)
        with self.archive(self.run_export()) as tar:
            self.member(tar, 'usr/bin/autostart')
            report = tar.extractfile(self.member(tar, 'collection.txt')).read().decode()
            self.assertIn('installation=missing', report)

    def test_owned_symlink_and_fifo_are_preserved_without_reading_targets(self):
        outside = self.root / 'outside-secret'
        outside.write_bytes(b'must not read this')
        (self.base / 'link').symlink_to(outside)
        os.mkfifo(self.base / 'fifo')
        with self.archive(self.run_export()) as tar:
            self.assertTrue(self.member(tar, 'mx5-aa-dr/link').issym())
            self.assertTrue(self.member(tar, 'mx5-aa-dr/fifo').isfifo())
            self.assertFalse(any(m.name.endswith('/outside-secret') for m in tar))

    def test_proc_capture_is_bounded_and_reports_truncation(self):
        (self.root / 'proc/meminfo').write_bytes(b'x' * (256 * 1024))
        with self.archive(self.run_export()) as tar:
            member = self.member(tar, 'proc/meminfo.txt')
            self.assertEqual(member.size, 128 * 1024)
            report = tar.extractfile(self.member(tar, 'collection.txt')).read().decode()
            self.assertIn('proc/meminfo=truncated', report)

    def test_diagnostic_helper_failure_keeps_partial_output_and_raw(self):
        (self.fixture.bundle / 'startup_diagnostics.sh').write_text(
            '#!/bin/sh\necho partial_fact=retained\nexit 7\n')
        with self.archive(self.run_export()) as tar:
            data = tar.extractfile(self.member(tar, 'startup-diagnostics.txt')).read()
            self.assertIn(b'partial_fact=retained', data)
            report = tar.extractfile(self.member(tar, 'collection.txt')).read().decode()
            self.assertIn('startup_diagnostics_exit=7', report)
            self.member(tar, 'mx5-aa-dr/logs/trace.0.jsonl')

    def test_actual_testmode_and_relevant_process_maps_are_collected(self):
        (self.root / 'data_persist/testmode.conf').write_text('TDE_TRIG_TEST_MODE\n')
        process = self.root / 'proc/42'
        process.mkdir()
        (process / 'comm').write_text('sm\n')
        (process / 'cmdline').write_bytes(b'/jci/sm/sm\0-f\0/tmp/mx5dr-trial-abcdef/sm.conf\0')
        (process / 'maps').write_text('authored mapped libmx5dr.so\n')
        with self.archive(self.run_export()) as tar:
            self.assertEqual(tar.extractfile(self.member(tar, 'persist/testmode.conf.txt')).read(),
                             b'TDE_TRIG_TEST_MODE\n')
            self.assertEqual(tar.extractfile(self.member(tar, 'proc/42/maps.txt')).read(),
                             b'authored mapped libmx5dr.so\n')

    def test_real_launcher_comm_names_of_the_three_services_are_collected(self):
        # Real CMU form (2026-10-04): comm L_<svc>, argv "/jci/sm/sm_svclauncher -l <svc> ...".
        for pid, name in ((51, 'jciAAPA'), (52, 'jciLDS'), (53, 'jciVBS'), (54, 'jciAudio')):
            process = self.root / 'proc' / str(pid)
            process.mkdir()
            (process / 'comm').write_text('L_' + name + '\n')
            (process / 'cmdline').write_bytes(
                b'\0'.join(x.encode() for x in ['/jci/sm/sm_svclauncher', '-l', name, '/jci/x.so', '0', '-a']) + b'\0')
            (process / 'maps').write_text('authored maps for ' + name + '\n')
        with self.archive(self.run_export()) as tar:
            for pid in (51, 52, 53):
                self.assertEqual(tar.extractfile(self.member(tar, 'proc/%d/maps.txt' % pid)).read(),
                                 b'authored maps for ' + {51: b'jciAAPA', 52: b'jciLDS', 53: b'jciVBS'}[pid] + b'\n')
            self.assertFalse(any('/proc/54/' in m.name for m in tar))

    def test_sm_reset_reports_in_data_are_collected_when_present(self):
        data = self.root / 'data'
        data.mkdir()
        (data / 'thread_info.out').write_text('authored kernel stacks: jciblmVdt futex_wait\n')
        (data / 'unrelated.bin').write_text('must not be collected\n')
        with self.archive(self.run_export()) as tar:
            self.assertIn(b'jciblmVdt futex_wait',
                          tar.extractfile(self.member(tar, 'sm-reports/thread_info.out.txt')).read())
            report = tar.extractfile(self.member(tar, 'collection.txt')).read().decode()
            self.assertIn('sm-reports/thread_info.out.mtime=', report)
            self.assertIn('sm-reports/ps_info.out=missing', report)
            self.assertFalse(any('unrelated' in m.name for m in tar))

    def test_oem_redirect_fifos_are_metadata_only_and_log_tails_are_bounded(self):
        redirects = self.root / 'tmp/redirlogs'
        redirects.mkdir(parents=True)
        for name in ('stdout', 'stderr'):
            os.mkfifo(redirects / name)
        running = self.root / 'var/log/running_log'
        running.mkdir(parents=True)
        (running / 'boot.log').write_bytes(b'x' * (256 * 1024) + b'last failure\n')
        with self.archive(self.run_export()) as tar:
            report = tar.extractfile(self.member(tar, 'collection.txt')).read().decode()
            self.assertIn('tmp/redirlogs/stdout.metadata=fifo', report)
            self.assertFalse(any(m.name.endswith('redirlogs/stdout.txt') for m in tar))
            member = self.member(tar, 'oem-logs/running/boot.log.txt')
            self.assertEqual(member.size, 128 * 1024)
            self.assertTrue(tar.extractfile(member).read().endswith(b'last failure\n'))

    def test_export_never_creates_archive_or_diagnostics_in_persistent_storage(self):
        before = {str(p.relative_to(self.base)) for p in self.base.rglob('*')}
        with self.archive(self.run_export()):
            pass
        self.assertEqual(before, {str(p.relative_to(self.base)) for p in self.base.rglob('*')})

    def test_detached_destination_never_receives_fallback_writes(self):
        import shutil
        commands = Path(self.fixture.tmp.name) / 'commands'
        commands.mkdir()
        # Model the mount pathname changing after export enters the USB.
        # Holding the directory as cwd must keep all writes on that original
        # filesystem. This is not a physical USB unplug or remount test.
        wrapper = commands / 'mkdir'
        wrapper.write_text('#!/bin/sh\n'
            'if [ ! -e "$DETACH_MARKER" ]; then\n'
            '  touch "$DETACH_MARKER"\n'
            '  mv "$DETACH_DEST" "$DETACH_DEST.detached"\n'
            '  "$REAL_MKDIR" "$DETACH_DEST"\n'
            'fi\nexec "$REAL_MKDIR" "$@"\n')
        wrapper.chmod(0o755)
        self.env.update(PATH=str(commands) + os.pathsep + os.environ['PATH'],
                        REAL_MKDIR=shutil.which('mkdir'), DETACH_DEST=str(self.dest),
                        DETACH_MARKER=str(Path(self.fixture.tmp.name) / 'detached'))
        result = self.run_export()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(self.dest.iterdir()), [])
        detached = self.dest.with_name(self.dest.name + '.detached')
        self.assertTrue(list(detached.glob('mx5dr-logs-*.tar.partial')))
        self.assertTrue((self.base / 'logs/trace.0.jsonl').is_file())


if __name__ == '__main__':
    unittest.main()
