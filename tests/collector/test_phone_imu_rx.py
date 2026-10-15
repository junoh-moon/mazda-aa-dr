"""Phone IMU receiver in the real (testing-build) collector against a host PTY
pair, a private dbus-daemon and a fake BCA (tests/collector/fake_bca.c).

The Python side plays the phone and bds: it owns the PTY master, writes v1
frames (tests/phone_imu/make_golden.py encoder), answers PINGs and checks the
pimu rows. No OEM code, radio or vehicle is involved; RFCOMM/BlueGo timing is
not modelled. validation/PHONE_IMU_CHANNEL_2026-10-14.md.
"""
import json
import os
from pathlib import Path
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import tty
import unittest
import zlib

REPO = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get('MX5DR_TEST_BUILD', REPO / 'build'))
COLLECTOR = BUILD / 'test_collector'
FAKE_BCA = BUILD / 'fake_bca'
XML = '/data_persist/mx5-aa-dr/bt/BdsConfiguration.xml'
BUS_CONF = '''<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:path={path}</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/></policy>
</busconfig>
'''


def data_frame(seq, dtheta, flags=0b0101, t_end=0, window=20000, bias=12):
    body = struct.pack('<2sBBIqiIi', b'MX', 1, flags, seq & 0xFFFFFFFF, t_end, dtheta, window, bias)
    return body + struct.pack('<I', zlib.crc32(body) & 0xFFFFFFFF)


def pong(ping, t_phone):
    magic, ver, typ, seq, t_cmu, _, crc = struct.unpack('<2sBBIqqI', ping)
    assert magic == b'MP' and ver == 1 and typ == 0 and crc == zlib.crc32(ping[:24]) & 0xFFFFFFFF
    body = struct.pack('<2sBBIqq', b'MP', 1, 1, seq, t_cmu, t_phone)
    return body + struct.pack('<I', zlib.crc32(body) & 0xFFFFFFFF), seq, t_cmu


@unittest.skipUnless(shutil.which('dbus-daemon') and COLLECTOR.exists() and FAKE_BCA.exists(),
                     'dbus-daemon, build/test_collector or build/fake_bca missing')
class PhoneImuReceiverTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'logs').mkdir()
        (self.root / 'bt').mkdir()
        (self.root / 'bt/BdsConfiguration.xml').write_text('<bdsConfig/>\n')
        (self.root / 'mx5dr.conf').write_text('mode=BETA\nmax_log_bytes=65536\nmax_log_files=2\nsample_ms=500\n')
        self.proc = self.root / 'proc'
        self.set_bds(4242, ['-interface', 'dbus', '-configFilePath', XML])
        self.ready = self.root / 'bds_ready_flag'
        self.ready.write_text('')
        bus = self.root / 'bus'
        conf = self.root / 'bus.conf'
        conf.write_text(BUS_CONF.format(path=bus))
        self.daemon = subprocess.Popen(['dbus-daemon', '--nofork', '--config-file', str(conf)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop, self.daemon)
        self.address = 'unix:path=' + str(bus)
        deadline = time.monotonic() + 5
        while not bus.exists():
            self.assertLess(time.monotonic(), deadline, 'dbus-daemon did not start')
            time.sleep(.02)
        self.bca = self.spawn_bca()
        self.collector = None
        self.masters = []
        self.addCleanup(self.close_masters)

    def stop(self, process):
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)

    def close_masters(self):
        for fd in self.masters:
            try:
                os.close(fd)
            except OSError:
                pass

    def spawn_bca(self, *extra):
        bca = subprocess.Popen([str(FAKE_BCA), self.address, *extra], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, text=True, bufsize=1)
        self.addCleanup(self.stop, bca)
        line = bca.stdout.readline()
        self.assertTrue(line.startswith('ready '), line)
        self.bca_lines = []
        threading.Thread(target=lambda: self.bca_lines.extend(iter(bca.stdout.readline, '')), daemon=True).start()
        return bca

    def bca_cmd(self, bca, text):
        bca.stdin.write(text + '\n')
        bca.stdin.flush()

    def set_bds(self, pid, args, argv0='/jci/bds/bds', start=None, keep=False):
        """Authored /proc entry of a bds-shaped process (never an OEM process)."""
        if self.proc.exists() and not keep:
            shutil.rmtree(self.proc)
        (self.proc / str(pid)).mkdir(parents=True, exist_ok=True)
        (self.proc / str(pid) / 'cmdline').write_bytes(b'\0'.join(x.encode() for x in [argv0, *args]) + b'\0')
        if start is not None:
            fields = ['S', '1'] + ['0'] * 18 + [str(start)] + ['0'] * 10   # field 3 onwards; 22 = start
            (self.proc / str(pid) / 'stat').write_text('%d (bds) %s\n' % (pid, ' '.join(fields)))
        (self.proc / 'self').mkdir(exist_ok=True)

    def start(self, *extra):
        cmd = [str(COLLECTOR), '--root', str(self.root), '--bus-address', self.address,
               '--pimu-proc-root', str(self.proc), '--pimu-ready-flag', str(self.ready), *extra]
        self.collector = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=getattr(self, 'env', None))
        self.addCleanup(self.finish)

    def finish(self):
        if self.collector and self.collector.poll() is None:
            (self.root / 'logs/collector.stop').mkdir(exist_ok=True)
            try:
                self.collector.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                self.collector.kill()
                self.collector.communicate(timeout=5)

    def rows(self, kind=None):
        out = []
        for name in ('pimu.1.jsonl', 'pimu.0.jsonl'):
            path = self.root / 'logs' / name
            if path.exists():
                for line in path.read_text().splitlines():
                    if line.strip():
                        out.append(json.loads(line))
        return [r for r in out if kind is None or r['kind'] == kind]

    def wait_row(self, pred, timeout=15, what='row'):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            found = [r for r in self.rows() if pred(r)]
            if found:
                return found
            self.assertIsNone(self.collector.poll(), self.collector.stderr.read() if self.collector.poll() is not None else '')
            time.sleep(.05)
        self.fail(f'timeout waiting for {what}: {self.rows()[-8:]}')

    def new_pty(self, raw=False):
        master, slave = os.openpty()
        path = os.ttyname(slave)
        if raw:
            tty.setraw(slave)
        os.close(slave)
        os.set_blocking(master, False)
        self.masters.append(master)
        return master, path

    def connect(self, master, path, via='poll'):
        before = len([r for r in self.rows('pimu_link') if r['ev'] in ('connect', 'replace') and r['path'] == path])
        if via == 'poll':
            self.bca_cmd(self.bca, f'state 15 3 0 {path}')
        else:
            self.bca_cmd(self.bca, f'signal 15 3 3 0 {path}')
        deadline = time.monotonic() + 15
        while len([r for r in self.rows('pimu_link') if r['ev'] in ('connect', 'replace') and r['path'] == path]) <= before:
            self.assertLess(time.monotonic(), deadline, f'no connect to {path}: {self.rows()[-6:]}')
            time.sleep(.05)

    class Phone:
        """Writes 50 Hz frames to the master and answers PINGs (offset fixed)."""
        def __init__(self, master, offset_ns, sent):
            self.master, self.offset, self.sent = master, offset_ns, sent
            self.buf, self.pings = b'', 0

        def service(self):
            try:
                self.buf += os.read(self.master, 4096)
            except BlockingIOError:
                pass
            except OSError:
                return
            while True:
                i = self.buf.find(b'MP')
                if i < 0 or len(self.buf) - i < 28:
                    self.buf = self.buf[i:] if i >= 0 else b''
                    return
                frame, self.buf = self.buf[i:i + 28], self.buf[i + 28:]
                _, _, t_cmu = pong(frame, 0)
                # the phone clock = CMU clock + offset; reception 1 ms later
                answer, _, _ = pong(frame, t_cmu + self.offset + 1000000)
                os.write(self.master, answer)
                self.pings += 1

        def send(self, frame):
            done = 0
            deadline = time.monotonic() + 2
            while done < len(frame):
                try:
                    done += os.write(self.master, frame[done:])
                except BlockingIOError:
                    if time.monotonic() > deadline:
                        raise AssertionError('master write blocked: the receiver stopped reading')
                    time.sleep(.001)
            self.sent.append(frame)

    def test_stream_garbage_burst_ping_silence_and_reconnect(self):
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'config', what='bds config seen')
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        # Cooked PTY on purpose: the receiver must switch it to raw (frames
        # carry CR, LF, ^C and ^D bytes).
        master, path = self.new_pty(raw=False)
        self.connect(master, path, via='poll')
        connect = self.rows('pimu_link')[-1]
        self.assertTrue(connect['why'].startswith('lflag_'), connect)
        self.assertIn('query 15\n', self.bca_lines)
        sent = []
        phone = self.Phone(master, 123456789000, sent)
        dtheta = [((i * 7919) % 4001) - 2000 for i in range(600)]
        t0 = time.monotonic()
        for i, d in enumerate(dtheta[:300]):
            frame = data_frame(i, d, flags=0b1101 if i == 0 else 0b0101, t_end=i * 20000000)
            if i == 100:
                phone.send(b'\x00garbage MX\x01 MP\x01\r\n\x03\x04' * 3)   # junk, fake magics
                sent.pop()
            if i == 150:
                bad = bytearray(frame)
                bad[20] ^= 0xFF
                phone.send(bytes(bad))                                     # corrupt: CRC error
                sent.pop()
                frame = data_frame(i, d, t_end=i * 20000000)
            if i in (200, 201, 202):
                phone.send(frame[:7])                                      # burst: fragments
                phone.send(frame[7:])
                sent.pop(); sent.pop()
                sent.append(frame)
            else:
                phone.send(frame)
            phone.service()
            time.sleep(max(0, t0 + (i + 1) * .02 - time.monotonic()))
        # Skip seq 300..302 (gap 3), send 303..399.
        for i in range(303, 400):
            phone.send(data_frame(i, dtheta[i], t_end=i * 20000000))
            phone.service()
            time.sleep(.02)
        rows = self.wait_row(lambda r: r['kind'] == 'pimu' and r['s1'] >= 300, what='pimu row')
        syncs = self.wait_row(lambda r: r['kind'] == 'pimu_sync', what='pimu_sync')
        for s in [s for s in syncs if s['rtt_us'] < 50000]:
            self.assertLess(abs(s['off_us'] - 123456789), 60000, s)   # loopback rtt << 60 ms
            self.assertGreaterEqual(s['rtt_us'], 0)
        self.assertGreaterEqual(phone.pings, 1)
        # Overload: 100 kB in one burst is read (never blocks the writer) and
        # the excess is discarded and counted.
        phone.send(b'\x55' * 100000)
        sent.pop()
        time.sleep(1.2)  # the rest of the overloaded second is discarded too
        for i in range(400, 450):
            phone.send(data_frame(i, dtheta[i], t_end=i * 20000000))
            phone.service()
            time.sleep(.02)
        time.sleep(11)  # 10 s of silence: the reader reopens the same PTY
        silence = self.wait_row(lambda r: r['kind'] == 'pimu_link' and r['ev'] == 'silence', what='silence')
        self.assertEqual(silence[-1]['path'], path)
        # Reconnect: bds closes the old PTY (master closed), a new one appears by signal.
        os.close(master)
        self.masters.remove(master)
        self.wait_row(lambda r: r['kind'] == 'pimu_link' and r['ev'] == 'disconnect', what='disconnect')
        master2, path2 = self.new_pty(raw=True)
        self.bca_cmd(self.bca, 'state 15 0 0 -')
        self.connect(master2, path2, via='signal')
        phone2 = self.Phone(master2, 5000000, [])
        for i in range(60):
            phone2.send(data_frame(i, 1000, flags=0b1101 if i == 0 else 0b0101))
            phone2.service()
            time.sleep(.02)
        self.finish()
        pimu = self.rows('pimu')
        total = {}
        for r in pimu:
            for k in ('ce', 'bv', 'sk', 'dr', 'fr'):
                total[k] = total.get(k, 0) + r[k]
        frames = sum(b[0] for r in pimu for b in r['d'])
        dsum = sum(b[1] for r in pimu for b in r['d'])
        gaps = sum(b[3] for r in pimu for b in r['d'])
        expect = sum(struct.unpack('<i', f[16:20])[0] for f in sent) + 60 * 1000
        self.assertEqual(frames, len(sent) + 60, [r for r in self.rows() if r["kind"] != "pimu"] + pimu[-3:])
        self.assertEqual(dsum, expect)
        self.assertEqual(gaps, 3)
        self.assertGreaterEqual(total['ce'], 1)
        self.assertGreater(total['sk'], 0)
        self.assertGreater(total['dr'], 50000)
        self.assertEqual(total['fr'], 2)
        for r in pimu:
            self.assertLessEqual(len(r['d']), 10)
            for b in r['d']:
                self.assertLessEqual(b[0], 999)
        for line in (self.root / 'logs/pimu.0.jsonl').read_text().splitlines():
            self.assertLess(len(line) + 1, 768)
        boot = self.rows('pimu_boot')
        self.assertEqual(len(boot), 1)
        self.assertLess(boot[0]['budget_bph'], 400000)
        # The collector journal is unchanged in kind: no pimu rows there.
        kinds = {json.loads(l)['kind'] for l in (self.root / 'logs/collector.0.jsonl').read_text().splitlines()}
        self.assertFalse({k for k in kinds if k.startswith('pimu') or k == 'bt_watch'})

    def test_signal_from_another_sender_is_ignored(self):
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        master, path = self.new_pty(raw=True)
        other = self.spawn_bca('--no-name')
        self.bca_cmd(other, f'signal 15 3 3 0 {path}')
        time.sleep(1.5)
        self.assertFalse([r for r in self.rows('pimu_link') if r['ev'] == 'connect'])
        self.bca_cmd(self.bca, f'signal 14 3 3 0 {path}')    # another serial service: never opened
        time.sleep(1)
        self.assertFalse([r for r in self.rows('pimu_link') if r['ev'] == 'connect'])
        self.connect(master, path, via='signal')
        self.bca_cmd(self.bca, 'signal 15 3 3 0 /tmp/not-a-pty')
        self.wait_row(lambda r: r['kind'] == 'pimu_link' and r['why'] == 'bad_path', what='bad path')
        self.finish()
        drop = self.rows('pimu_drop')
        self.assertTrue(drop and drop[-1]['d'][5] >= 1 and drop[-1]['d'][6] >= 1, drop)

    def test_not_ready_writes_bt_fail(self):
        self.ready.unlink()
        self.start('--pimu-ready-timeout-ms', '500')
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'fail', what='bt fail')
        text = (self.root / 'logs/bt-fail').read_text()
        self.assertRegex(text, r'^mx5dr-bt-fail-v1\nboot_id=[0-9a-f-]{36}\nreason=not_ready\n$')

    def test_restart_writes_bt_fail(self):
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        self.set_bds(4343, ['-interface', 'dbus', '-configFilePath', XML])
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'fail', what='bt fail')
        self.assertIn('reason=restart\n', (self.root / 'logs/bt-fail').read_text())

    def test_stock_arguments_stay_inert(self):
        self.set_bds(4242, ['-interface', 'dbus'])
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'stock', what='stock')
        time.sleep(6)
        self.assertNotIn('query 15\n', self.bca_lines)
        self.finish()
        self.assertFalse((self.root / 'logs/bt-fail').exists())
        self.assertFalse(self.rows('pimu_link'))

    def test_no_bt_config_or_markers_start_nothing(self):
        for setup in ('xml', 'off', 'fail', 'mode'):
            with self.subTest(setup=setup):
                (self.root / 'bt/BdsConfiguration.xml').write_text('<bdsConfig/>\n')
                for marker in ('bt-off', 'logs/bt-fail'):
                    if (self.root / marker).exists():
                        (self.root / marker).unlink()
                (self.root / 'mx5dr.conf').write_text('mode=BETA\nsample_ms=500\n')
                if setup == 'xml':
                    (self.root / 'bt/BdsConfiguration.xml').unlink()
                elif setup == 'off':
                    (self.root / 'bt-off').write_text('off\n')
                elif setup == 'fail':
                    (self.root / 'logs/bt-fail').write_text('x\n')
                else:
                    (self.root / 'mx5dr.conf').write_text('mode=OBSERVE\nsample_ms=500\n')
                result = subprocess.run([str(COLLECTOR), '--root', str(self.root), '--bus-address', self.address,
                                         '--pimu-proc-root', str(self.proc), '--pimu-ready-flag', str(self.ready),
                                         '--samples', '3'], capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertFalse(list((self.root / 'logs').glob('pimu.*')))

    def wait_connect_count(self, n, timeout=15):
        deadline = time.monotonic() + timeout
        while len([r for r in self.rows('pimu_link') if r['ev'] in ('connect', 'replace')]) < n:
            self.assertLess(time.monotonic(), deadline, self.rows()[-6:])
            time.sleep(.05)

    def read_master(self, master, seconds):
        got, end = b'', time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                got += os.read(master, 4096)
            except (BlockingIOError, OSError):
                pass
            time.sleep(.05)
        return got

    def test_stale_or_disconnected_status_never_opens_and_disconnect_closes(self):
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        master, path = self.new_pty(raw=True)
        self.bca_cmd(self.bca, f'state 15 0 0 {path}')    # connStatus IDLE with a valid, stale path
        self.wait_row(lambda r: r['kind'] == 'pimu_link' and r['ev'] == 'poll' and r['why'] == 'not_connected',
                      what='stale poll')
        self.bca_cmd(self.bca, f'signal 15 2 3 0 {path}')  # CONNECTING
        time.sleep(1.5)
        self.assertFalse([r for r in self.rows('pimu_link') if r['ev'] == 'connect'])
        self.bca_cmd(self.bca, f'signal 15 3 3 0 {path}')  # CONNECTED
        self.wait_connect_count(1)
        self.bca_cmd(self.bca, f'signal 15 6 3 0 {path}')  # DISCONNECTED: our reader closes
        self.wait_row(lambda r: r['kind'] == 'pimu_link' and r['ev'] == 'disconnect' and r['why'] == 'bca_status',
                      what='bca disconnect')

    def test_poll_backs_off_while_disconnected(self):
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        time.sleep(25)
        queries = self.bca_lines.count('query 15\n')
        # t = 0, 5, 15 (then 35): three in 25 s; a fixed 5 s period would give five or six.
        self.assertIn(queries, (3, 4), self.bca_lines)
        self.bca_cmd(self.bca, 'signal 15 0 3 0 -')          # any signal resets the period to 5 s
        before = queries
        time.sleep(6.5)
        self.assertGreaterEqual(self.bca_lines.count('query 15\n'), before + 1)

    def test_bds_crash_quiesces_the_service_for_the_boot(self):
        """bds crash on connect -> SM restart -> the phone retries: after the restart the CMU
        never opens the new PTY and sends no PING, so every further phone session ends without
        one and its breaker trips (bound: CoreTests.crashLoop)."""
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        master, path = self.new_pty(raw=True)
        self.connect(master, path, via='signal')
        first = self.read_master(master, 6)
        self.assertIn(b'MP', first)                           # PINGs while healthy
        self.set_bds(4343, ['-interface', 'dbus', '-configFilePath', XML])   # crash + SM restart
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'quiesce', what='quiesce')
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'fail' and r['why'] == 'restart', what='restart')
        self.assertTrue([r for r in self.rows('pimu_link') if r['ev'] == 'disconnect' and r['why'] == 'quiesce'])
        self.assertIn('reason=restart\n', (self.root / 'logs/bt-fail').read_text())
        for attempt in range(3):                              # the phone reconnects three times
            m, p = self.new_pty(raw=True)
            self.bca_cmd(self.bca, f'signal 15 3 3 0 {p}')
            self.assertEqual(self.read_master(m, 6), b'', 'no PING after the quiesce')
        self.assertEqual(len([r for r in self.rows('pimu_link') if r['ev'] in ('connect', 'replace')]), 1)
        self.assertNotIn('query 15\n', self.bca_lines[-3:])

    def test_vanished_and_args_changed(self):
        self.start('--pimu-vanish-ms', '3000')
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        shutil.rmtree(self.proc / '4242')
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'fail', what='vanished')
        self.assertIn('reason=vanished\n', (self.root / 'logs/bt-fail').read_text())
        self.finish()
        (self.root / 'logs/bt-fail').unlink()
        for name in ('pimu.0.jsonl', 'pimu.1.jsonl'):
            if (self.root / 'logs' / name).exists():
                (self.root / 'logs' / name).unlink()
        self.collector = None
        self.set_bds(4242, ['-interface', 'dbus', '-configFilePath', XML])
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready again')
        self.set_bds(4444, ['-interface', 'dbus'])
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'fail', what='args_changed')
        self.assertIn('reason=args_changed\n', (self.root / 'logs/bt-fail').read_text())

    def test_forked_child_is_not_a_restart_and_argv0_basename(self):
        # bds is started as a relative "bds"; at the first scan a younger child with the same
        # cmdline (system() before its exec) also exists.
        self.set_bds(4242, ['-interface', 'dbus', '-configFilePath', XML], argv0='bds', start=100)
        self.set_bds(4300, ['-interface', 'dbus', '-configFilePath', XML], argv0='bds', start=250, keep=True)
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'config', what='config')
        self.assertEqual(self.rows('bt_watch')[-1]['pid'], 4242)
        shutil.rmtree(self.proc / '4300')
        time.sleep(1.5)
        # another short-lived child while bds keeps running
        self.set_bds(4301, ['-interface', 'dbus', '-configFilePath', XML], argv0='bds', start=300, keep=True)
        time.sleep(0.4)
        shutil.rmtree(self.proc / '4301')
        time.sleep(3)
        self.assertFalse((self.root / 'logs/bt-fail').exists())
        self.assertFalse([r for r in self.rows('bt_watch') if r['ev'] in ('fail', 'quiesce')])

    def test_short_lived_bds_shaped_process_is_not_taken_for_bds(self):
        # At the first scan only a short-lived bds-shaped process (a fork before its exec)
        # exists; the real bds appears a moment later. It must not be reported as a restart.
        self.set_bds(4300, ['-interface', 'dbus', '-configFilePath', XML])
        self.start()
        time.sleep(0.6)
        self.set_bds(4242, ['-interface', 'dbus', '-configFilePath', XML])
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'config', what='config')
        time.sleep(3)
        self.assertEqual([r['pid'] for r in self.rows('bt_watch') if r['ev'] == 'config'], [4242])
        self.assertFalse([r for r in self.rows('bt_watch') if r['ev'] in ('fail', 'quiesce')])
        self.assertFalse((self.root / 'logs/bt-fail').exists())

    def test_pty_validation_rejects_other_devices_and_owners(self):
        pts = self.root / 'pts'
        pts.mkdir()
        (pts / '7').symlink_to('/dev/null')                   # a character device, major 1
        # /dev/null belongs to uid 0 like the CMU's PTYs: only the major number rejects it.
        self.start('--pimu-pts-dir', str(pts) + '/', '--pimu-pty-owner', '0')
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        self.bca_cmd(self.bca, f'signal 15 3 3 0 {pts}/7')
        self.wait_row(lambda r: r['kind'] == 'pimu_link' and r['ev'] == 'open_fail' and r['why'] == 'not_pty',
                      what='not a pty')
        self.finish()
        self.collector = None
        for name in ('pimu.0.jsonl', 'pimu.1.jsonl'):
            if (self.root / 'logs' / name).exists():
                (self.root / 'logs' / name).unlink()
        self.start('--pimu-pty-owner', '0')                   # a real pty, but not owned by uid 0
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        master, path = self.new_pty(raw=True)
        self.bca_cmd(self.bca, f'signal 15 3 3 0 {path}')
        self.wait_row(lambda r: r['kind'] == 'pimu_link' and r['ev'] == 'open_fail' and r['why'] == 'not_pty',
                      what='wrong owner')

    def test_silence_reopen_never_leaves_the_pty_without_a_reader(self):
        self.env = dict(os.environ, MX5_TEST_REOPEN_DELAY_MS='400')
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        master, path = self.new_pty(raw=True)                 # the test holds no slave fd
        self.connect(master, path, via='signal')
        hup, end = 0, time.monotonic() + 13
        while time.monotonic() < end:                        # 10 s silence -> reopen (400 ms hook)
            r = select.poll()
            r.register(master, select.POLLIN | select.POLLHUP)
            for _, ev in r.poll(5):
                if ev & select.POLLHUP:
                    hup += 1
            try:
                os.read(master, 4096)
            except (BlockingIOError, OSError):
                pass
        self.assertTrue([r for r in self.rows('pimu_link') if r['ev'] == 'silence'])
        self.assertEqual(hup, 0, 'the PTY master saw a hangup: no reader for a moment')


if __name__ == '__main__':
    unittest.main()
