"""Independent-process host fixtures; no OEM firmware or vehicle is executed."""
import json
import os
from pathlib import Path
import signal
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest

REPO = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get('MX5DR_TEST_BUILD', REPO / 'build'))
COLLECTOR = BUILD / 'test_collector'
FAKE_BCA = BUILD / 'fake_bca'
BUS_CONF = '''<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:path={path}</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/></policy>
</busconfig>
'''


class StuckBus:
    """A unix socket in place of the service-bus dbus-daemon: it accepts and
    then never answers SASL ('silent'), or answers SASL and never answers
    Hello ('auth_only'). Records the duration of every connection."""
    def __init__(self, path, kind):
        self.kind, self.conns = kind, []
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(str(path))
        self.srv.listen(64)
        threading.Thread(target=self.accept_loop, daemon=True).start()

    def accept_loop(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            rec = {'accept': time.monotonic(), 'eof': None, 'sock': c}
            self.conns.append(rec)
            threading.Thread(target=self.serve, args=(rec,), daemon=True).start()

    def serve(self, rec):
        c, buf, authed = rec['sock'], b'', False
        try:
            while True:
                chunk = c.recv(4096)
                if not chunk:
                    break
                if self.kind != 'auth_only' or authed:
                    continue
                buf += chunk
                while b'\r\n' in buf:
                    line, buf = buf.split(b'\r\n', 1)
                    line = line.lstrip(b'\0')
                    if line.startswith(b'AUTH'):
                        c.sendall(b'OK ' + b'0123456789abcdef' * 2 + b'\r\n')
                    elif line.startswith(b'NEGOTIATE_UNIX_FD'):
                        c.sendall(b'AGREE_UNIX_FD\r\n')
                    elif line.startswith(b'BEGIN'):
                        authed = True
                        break
                    else:
                        c.sendall(b'ERROR\r\n')
        except OSError:
            pass
        rec['eof'] = time.monotonic()

    def close(self):
        self.srv.close()
        for rec in self.conns:
            try:
                rec['sock'].close()
            except OSError:
                pass


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

    # ---- bounded service-bus registration (validation/V1_A_PRODUCT_HARDENING_2026-10-18.md) ----
    # dbus_bus_register() waited without limit on a socket that accepts but
    # never answers SASL, and 25 s for an unanswered Hello: position polling
    # and row writing froze. Now 2 s handshake + 1 s Hello per sample.
    def stuck_service_bus(self, kind, attempt_max_s):
        sock = self.root / 'stuckbus'
        stuck = StuckBus(sock, kind)
        self.addCleanup(stuck.close)
        cmd = [str(COLLECTOR), '--root', str(self.root), '--bus-address', 'unix:path=' + str(sock), '--samples', '3']
        started = time.monotonic()
        result = subprocess.run(cmd, capture_output=True, timeout=20)
        elapsed = time.monotonic() - started
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = self.rows()
        errors = [r for r in rows if r['kind'] == 'position_poll_error']
        self.assertEqual(len(errors), 3, rows)
        self.assertTrue(all(r['reason'] == 'unavailable' for r in errors))
        self.assertEqual(len([r for r in rows if r['kind'] == 'poll']), 3)
        self.assertEqual(rows[-1]['kind'], 'collector_stop')
        self.assertEqual(rows[-1]['samples'], 3)
        self.assertGreaterEqual(len(stuck.conns), 3, 'a new connection per sample')
        deadline = time.monotonic() + 2       # the server threads see EOF after the exit
        while any(rec['eof'] is None for rec in stuck.conns) and time.monotonic() < deadline:
            time.sleep(.02)
        for rec in stuck.conns:
            self.assertIsNotNone(rec['eof'], 'every attempt closed its socket')
            self.assertLessEqual(rec['eof'] - rec['accept'], attempt_max_s)
        self.assertLess(elapsed, 3 * attempt_max_s + 3)

    def test_silent_service_bus_times_out_and_polling_continues(self):
        self.stuck_service_bus('silent', 2.6)

    def test_unanswered_hello_times_out_and_polling_continues(self):
        self.stuck_service_bus('auth_only', 1.6)

    def test_stop_signal_interrupts_a_stuck_handshake(self):
        sock = self.root / 'stuckbus'
        stuck = StuckBus(sock, 'silent')
        self.addCleanup(stuck.close)
        p = subprocess.Popen([str(COLLECTOR), '--root', str(self.root), '--bus-address', 'unix:path=' + str(sock)],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(lambda: (p.poll() is None and p.kill(), p.communicate(timeout=3)))
        self.wait_boot(p)
        deadline = time.monotonic() + 3
        while not stuck.conns:
            self.assertLess(time.monotonic(), deadline, 'no connection attempt')
            time.sleep(.02)
        p.send_signal(signal.SIGTERM)
        p.communicate(timeout=1.5)
        self.assertEqual(p.returncode, 0)
        self.assertEqual(self.rows()[-1]['reason'], 'signal')

    # ---- coordinate rounding (validation/V1_STABLE_PRIVACY_2026-10-18.md) ----
    @unittest.skipUnless(shutil.which('dbus-daemon') and FAKE_BCA.exists(), 'dbus-daemon or build/fake_bca missing')
    def test_position_poll_coordinates_rounded_to_three_decimals(self):
        bus = self.root / 'bus'
        conf = self.root / 'bus.conf'
        conf.write_text(BUS_CONF.format(path=bus))
        daemon = subprocess.Popen(['dbus-daemon', '--nofork', '--config-file', str(conf)],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: (daemon.kill(), daemon.wait(timeout=5)))
        deadline = time.monotonic() + 5
        while not bus.exists():
            self.assertLess(time.monotonic(), deadline, 'dbus-daemon did not start')
            time.sleep(.02)
        # Synthetic values (not a place): the fourth decimal decides rounding.
        lds = subprocess.Popen([str(FAKE_BCA), 'unix:path=' + str(bus), '--lds', '12.3456789', '-45.6784999'],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.addCleanup(lambda: (lds.kill(), lds.communicate(timeout=5)))
        self.assertTrue(lds.stdout.readline().startswith('ready '))
        cmd = [str(COLLECTOR), '--root', str(self.root), '--bus-address', 'unix:path=' + str(bus), '--samples', '2']
        result = subprocess.run(cmd, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        text = (self.root / 'logs/collector.0.jsonl').read_text()
        polls = [r for r in self.rows() if r['kind'] == 'position_poll']
        self.assertEqual(len(polls), 2, text[-600:])
        for row in polls:
            self.assertEqual((row['lat'], row['lon']), (12.346, -45.678))
            self.assertEqual((row['mode'], row['heading'], row['kmh']), (2, 87.25, 42.5))
        self.assertIn('"lat":12.346,"lon":-45.678,', text)
        self.assertNotIn('12.3456789', text)
        self.assertNotIn('45.6784999', text)
        # The analyzer still accepts the rounded rows.
        report = json.loads(subprocess.run(['python3', str(REPO / 'tools/analyze_logs.py'), '--json',
                                            str(self.root / 'logs')], capture_output=True, text=True).stdout)
        self.assertNotIn('partial_record', [i['code'] for i in report['issues']])
        self.assertEqual(report['position_poll_modes'], {'2': 2})

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
