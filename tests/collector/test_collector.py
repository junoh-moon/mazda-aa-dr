"""Independent-process host fixtures; no OEM firmware or vehicle is executed."""
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get('MX5DR_TEST_BUILD', REPO / 'build'))
COLLECTOR = BUILD / 'test_collector'


def polling_imports(report):
    # nm suffixes symbol versions with @. Registering a child cleanup callback
    # is not an import of fork(): match functions, not substrings of the report.
    symbols = {line.split()[-1].split('@', 1)[0]
               for line in report.splitlines() if line.split()}
    calls = {'fork', 'vfork', '__fork', '__libc_fork', 'posix_spawn',
             'posix_spawnp', 'waitpid', '__waitpid'}
    return {name for name in symbols if name.startswith('dbus_') or name in calls}


class CollectorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'logs').mkdir()
        self.config = self.root / 'mx5dr.conf'
        self.config.write_text('mode=OBSERVE\nmax_log_bytes=65536\nmax_log_files=2\nsample_ms=500\n')
        self.cmd = [str(COLLECTOR), '--root', str(self.root), '--bus-address',
                    'unix:path=' + str(self.root / 'missing-bus')]

    def rows(self):
        result = []
        for path in sorted((self.root / 'logs').glob('collector.*.jsonl'), reverse=True):
            result.extend(json.loads(line) for line in path.read_text().splitlines())
        return result

    def wait_boot(self, process):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if (self.root / 'logs/collector.0.jsonl').exists():
                if 'collector_boot' in (self.root / 'logs/collector.0.jsonl').read_text():
                    return
            self.assertIsNone(process.poll())
            time.sleep(.02)
        self.fail('collector did not boot')

    def spawn(self, *args):
        process = subprocess.Popen(self.cmd + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        def cleanup():
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=3)
        self.addCleanup(cleanup)
        return process

    def test_bus_disconnected_records_unknown_producer_time(self):
        p = self.spawn('--samples', '1')
        p.communicate(timeout=3)
        self.assertEqual(p.returncode, 0)
        rows = self.rows()
        self.assertEqual(rows[0]['kind'], 'collector_boot')
        self.assertEqual(rows[0]['boot_id'], Path('/proc/sys/kernel/random/boot_id').read_text().strip())
        self.assertEqual(rows[-1]['kind'], 'collector_stop')
        for row in rows:
            self.assertEqual(row['stream'], 'collector')
            self.assertEqual(row['collector_pid'], p.pid)
            self.assertGreater(row['observed_at_mono_ns'], 0)
            self.assertIsNone(row['producer_mono_ns'])
            self.assertEqual(row['producer_time_status'], 'unknown')
        poll = next(row for row in rows if row['kind'] == 'poll')
        self.assertEqual(poll['speed_raw'], 'smdb_disabled')
        self.assertEqual(poll['yaw_raw'], 'smdb_disabled')
        self.assertEqual(poll['gear_raw'], 'smdb_disabled')
        self.assertEqual(poll['freshness'], 'unproven_poll')
        self.assertIn('position_poll_error', [row['kind'] for row in rows])
        self.assertFalse(list((self.root / 'logs').glob('trace.*')))

    def test_invalid_off_and_production_no_test_overrides(self):
        for config in ('mode=OFF\n', 'mode=ASSIST\n'):
            self.config.write_text(config)
            self.assertEqual(subprocess.run(self.cmd).returncode, 78)
            self.assertFalse((self.root / 'logs/collector.0.jsonl').exists())
        result = subprocess.run([str(BUILD / 'mx5dr-collector-host'), '--root', str(self.root)], capture_output=True)
        self.assertEqual(result.returncode, 64)

    def test_singleton_and_cooperative_stop(self):
        p = self.spawn()
        self.wait_boot(p)
        self.assertEqual(subprocess.run(self.cmd, timeout=2).returncode, 73)
        (self.root / 'logs/collector.stop').mkdir()
        p.communicate(timeout=3)
        self.assertEqual(p.returncode, 0)
        self.assertEqual(self.rows()[-1]['reason'], 'stop_marker')
        self.assertFalse((self.root / 'logs/collector.pid').exists())
        # Kernel lock is released and stale stop marker cleared on explicit restart.
        again = self.spawn('--samples', '1')
        again.communicate(timeout=3)
        self.assertEqual(again.returncode, 0)

    def test_config_off_stops_running_collector(self):
        p = self.spawn()
        self.wait_boot(p)
        self.config.write_text('mode=OFF\n')
        p.communicate(timeout=3)
        self.assertEqual(p.returncode, 0)
        self.assertEqual(self.rows()[-1]['reason'], 'config_disabled')

    def test_hang_and_crash_do_not_block_aa_send_or_writer(self):
        p = self.spawn()
        self.wait_boot(p)
        p.send_signal(signal.SIGSTOP)
        try:
            for test in ([str(BUILD / 'test_journal')], [str(BUILD / 'test_adapter'), 'observe']):
                result = subprocess.run(test, timeout=2, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
        finally:
            p.kill()
            p.communicate(timeout=3)
        self.assertLess(p.returncode, 0)
        for test in ([str(BUILD / 'test_journal')], [str(BUILD / 'test_adapter'), 'scrub']):
            self.assertEqual(subprocess.run(test, timeout=2, capture_output=True).returncode, 0)

    def test_collector_never_touches_the_stock_smdb_or_other_processes(self):
        # A killed smdb-read can leak libjcismdb's named semaphore and stall
        # jciblmVdt (vehicle reset 2026-10-04). The collector must not read the
        # SMDB, fork, exec, signal or wait on any process.
        imports = subprocess.check_output(['nm', '-u', str(COLLECTOR)], text=True)
        names = {line.split()[-1].split('@')[0] for line in imports.splitlines() if line.strip()}
        for name in ('fork', 'vfork', '__fork', '__libc_fork', 'posix_spawn', 'posix_spawnp',
                     'execv', 'execve', 'execvp', 'execl', 'execlp', 'system', 'popen',
                     'waitpid', '__waitpid', 'wait', 'wait4', 'kill', 'killpg', 'sem_open',
                     'sem_wait', 'sem_post', 'shm_open'):
            with self.subTest(name=name):
                self.assertNotIn(name, names)
        strings = subprocess.check_output(['strings', '-a', str(COLLECTOR)], text=True)
        self.assertNotIn('smdb-read', strings)
        self.assertNotIn('/jci/smdb', strings)
        self.assertNotIn('vdm_vdt_current_data', strings)
        p = self.spawn('--samples', '1')
        p.communicate(timeout=3)
        self.assertEqual(p.returncode, 0)
        poll = next(row for row in self.rows() if row['kind'] == 'poll')
        self.assertEqual(poll['speed_raw'], 'smdb_disabled')

    def test_symlink_log_file_and_pid_are_rejected(self):
        target = self.root / 'untouched'
        target.write_text('sentinel')
        (self.root / 'logs/collector.pid').symlink_to(target)
        self.assertEqual(subprocess.run(self.cmd).returncode, 73)
        self.assertEqual(target.read_text(), 'sentinel')
        (self.root / 'logs/collector.pid').unlink()
        (self.root / 'logs/collector.0.jsonl').symlink_to(target)
        self.assertEqual(subprocess.run(self.cmd, timeout=3).returncode, 74)
        self.assertEqual(target.read_text(), 'sentinel')

    def test_preload_runtime_has_no_polling_imports(self):
        imports = subprocess.check_output(['nm', '-u', str(BUILD / 'test_journal')], text=True)
        self.assertEqual(polling_imports(imports), set())
        needed = subprocess.check_output(['readelf', '-d', str(BUILD / 'test_journal')], text=True)
        self.assertNotIn('libdbus', needed)
        collector_needed = subprocess.check_output(['readelf', '-d', str(COLLECTOR)], text=True)
        self.assertIn('libdbus', collector_needed)

    def test_atfork_registration_does_not_hide_process_or_dbus_imports(self):
        allowed = ' U __register_atfork@GLIBC_2.3.2\n U pthread_atfork@GLIBC_2.2.5\n'
        self.assertEqual(polling_imports(allowed), set())
        for name in ('fork', 'vfork', '__fork', '__libc_fork', 'posix_spawn',
                     'posix_spawnp', 'waitpid', '__waitpid', 'dbus_connection_send'):
            with self.subTest(name=name):
                self.assertEqual(polling_imports(allowed + ' U ' + name + '@GLIBC_2.4\n'),
                                 {name})

    def test_separate_collector_trace_does_not_create_fake_aa_session(self):
        p = self.spawn('--samples', '1')
        p.communicate(timeout=3)
        result = subprocess.run(['python3', str(REPO / 'tools/analyze_logs.py'), '--json', str(self.root / 'logs')],
                                capture_output=True, text=True)
        report = json.loads(result.stdout)
        codes = [issue['code'] for issue in report['issues']]
        self.assertNotIn('missing_boot', codes)
        self.assertNotIn('missing_health', codes)
        self.assertNotIn('unknown_record_kind', codes)
        self.assertIn('no_location_samples', codes)  # Polls do not replace AA evidence.
        self.assertEqual(report['collector']['pids'], [p.pid])

    def test_parked_helpers_request_one_launch_and_cooperative_stop(self):
        fixture = self.root / 'fixture'
        fixture.mkdir()
        (fixture / '.mx5dr-fixture').touch()
        base = fixture / 'data_persist/mx5-aa-dr'
        (base / 'logs').mkdir(parents=True)
        (base / 'mx5dr.conf').write_text('mode=OBSERVE\n')
        tools = base / 'tools'
        tools.mkdir()
        for name in ('start_collector.sh', 'stop_collector.sh', 'common.sh'):
            shutil.copyfile(REPO / 'packaging' / name, tools / name)
        # Fixture executable records launch arguments/environment, never touches AA.
        executable = base / 'mx5dr-collector'
        executable.write_text('#!/bin/sh\nprintf "%s|%s|%s\\n" "${LD_PRELOAD-unset}" '
                              '"${LD_AUDIT-unset}" "$*" > "' + str(base / 'logs/launched') + '"\n')
        executable.chmod(0o700)
        environment = dict(os.environ, MX5DR_FIXTURE_ROOT=str(fixture), LD_PRELOAD='libm.so.6')
        result = subprocess.run(['sh', str(tools / 'start_collector.sh'), '60'],
                                env=environment, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        deadline = time.monotonic() + 2
        launched = base / 'logs/launched'
        while not launched.exists() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertEqual(launched.read_text().strip(), 'unset|unset|--session-seconds 60')
        result = subprocess.run(['sh', str(tools / 'stop_collector.sh')], env=environment, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((base / 'logs/collector.stop').is_dir())
        result = subprocess.run(['sh', str(tools / 'start_collector.sh'), '0'], env=environment, capture_output=True)
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
