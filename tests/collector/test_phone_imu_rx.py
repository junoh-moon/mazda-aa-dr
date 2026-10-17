"""Phone IMU receiver in the real (testing-build) collector against a host PTY
pair, two private dbus-daemons and fake BCAs (tests/collector/fake_bca.c).

Two buses like the CMU (validation/BCA_QUERY_CONNECT_STATUS_RE_2026-10-17.md):
the SERVICE bus (--bus-address, the collector's position polling) has a fake
that owns com.jci.bca but has no /com/jci/bca object, so every call gets
libdbus's UnknownMethod and no signal is ever sent there (the stock BCA's
service-bus connection); the HMI bus (--bca-bus-address) has the fake that
answers QueryConnectStatus and emits ConnectionStatusResp.

The Python side plays the phone and bds: it owns the PTY master, writes v1
frames (tests/phone_imu/make_golden.py encoder), answers PINGs and checks the
pimu rows. No OEM code, radio or vehicle is involved; RFCOMM/BlueGo timing is
not modelled. validation/PHONE_IMU_CHANNEL_2026-10-14.md.
"""
import errno
import json
import os
from pathlib import Path
import select
import shutil
import socket
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
        self.address = self.start_daemon('bus')          # service bus
        self.hmi_address = self.start_daemon('hmibus')   # HMI bus
        self.service_bca = self.spawn_bca('--no-object', address=self.address)
        self.service_lines = self.bca_lines
        self.bca = self.spawn_bca()
        self.collector = None
        self.masters = []
        self.addCleanup(self.close_masters)

    def start_daemon(self, name):
        bus = self.root / name
        conf = self.root / (name + '.conf')
        conf.write_text(BUS_CONF.format(path=bus))
        daemon = subprocess.Popen(['dbus-daemon', '--nofork', '--config-file', str(conf)],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop, daemon)
        self.daemons = getattr(self, 'daemons', {})
        self.daemons[name] = daemon
        deadline = time.monotonic() + 5
        while not bus.exists():
            self.assertLess(time.monotonic(), deadline, 'dbus-daemon did not start')
            time.sleep(.02)
        return 'unix:path=' + str(bus)

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

    def spawn_bca(self, *extra, address=None):
        bca = subprocess.Popen([str(FAKE_BCA), address or self.hmi_address, *extra], stdin=subprocess.PIPE,
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

    def set_bds(self, pid, args, argv0='/jci/bds/bds', start=None, keep=False, ppid=1):
        """Authored /proc entry of a bds-shaped process (never an OEM process)."""
        if self.proc.exists() and not keep:
            shutil.rmtree(self.proc)
        (self.proc / str(pid)).mkdir(parents=True, exist_ok=True)
        (self.proc / str(pid) / 'cmdline').write_bytes(b'\0'.join(x.encode() for x in [argv0, *args]) + b'\0')
        if start is None:
            start = 1000 + pid       # every authored process has a stat entry, like /proc
        if start is not False:
            fields = ['S', str(ppid)] + ['0'] * 17 + [str(start)] + ['0'] * 10   # fields 3..; 4 = ppid, 22 = start
            (self.proc / str(pid) / 'stat').write_text('%d (bds) %s\n' % (pid, ' '.join(fields)))
        (self.proc / 'self').mkdir(exist_ok=True)

    def start(self, *extra, bca_bus=None):
        cmd = [str(COLLECTOR), '--root', str(self.root), '--bus-address', self.address,
               '--bca-bus-address', bca_bus or self.hmi_address,
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

    def test_not_ready_writes_bt_fail_and_quiesces(self):
        # Not ready within the timeout: bt-fail AND quiesce for the boot (no PTY, no PING, no
        # BCA query, D-Bus closed), even when a CONNECTED signal and the flag come later.
        self.ready.unlink()
        self.start('--pimu-ready-timeout-ms', '500')
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'fail', what='bt fail')
        text = (self.root / 'logs/bt-fail').read_text()
        self.assertRegex(text, r'^mx5dr-bt-fail-v1\nboot_id=[0-9a-f-]{36}\nreason=not_ready\n$')
        self.assertTrue([r for r in self.rows('bt_watch') if r['ev'] == 'quiesce' and r['why'] == 'not_ready'])
        self.ready.write_text('')
        master, path = self.new_pty(raw=True)
        self.bca_cmd(self.bca, f'state 15 3 0 {path}')
        self.bca_cmd(self.bca, f'signal 15 3 3 0 {path}')
        queries = self.bca_lines.count('query 15\n')
        self.assertEqual(self.read_master(master, 7), b'', 'no PING after not_ready')
        self.assertEqual(self.bca_lines.count('query 15\n'), queries, 'no BCA query after not_ready')
        self.assertFalse([r for r in self.rows('pimu_link') if r['ev'] in ('connect', 'replace')])
        # The bds keeps being watched: its later death is still journaled and nothing reopens.
        shutil.rmtree(self.proc / '4242')
        self.set_bds(4343, ['-interface', 'dbus', '-configFilePath', XML], keep=True)
        time.sleep(2.5)
        self.assertFalse([r for r in self.rows('pimu_link') if r['ev'] in ('connect', 'replace')])

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
                                         '--bca-bus-address', self.hmi_address,
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

    def test_poll_stays_at_5_s_until_the_first_pty_then_backs_off(self):
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        time.sleep(21)
        # The phone waits only 15 s for a first PING: every 5 s until a PTY was opened.
        self.assertGreaterEqual(self.bca_lines.count('query 15\n'), 4, self.bca_lines)
        master, path = self.new_pty(raw=True)
        self.connect(master, path, via='poll')
        self.bca_cmd(self.bca, 'state 15 0 0 -')
        os.close(master)
        self.masters.remove(master)
        self.wait_row(lambda r: r['kind'] == 'pimu_link' and r['ev'] == 'disconnect', what='disconnect')
        before = self.bca_lines.count('query 15\n')
        time.sleep(25)
        # after the first PTY: 5, 10, 20 s steps -> at most 3 queries in 25 s
        self.assertLessEqual(self.bca_lines.count('query 15\n') - before, 3, self.bca_lines)
        self.bca_cmd(self.bca, 'signal 15 0 3 0 -')          # a signal resets the period to 5 s
        before = self.bca_lines.count('query 15\n')
        time.sleep(6.5)
        self.assertGreaterEqual(self.bca_lines.count('query 15\n'), before + 1)

    def test_connecting_while_connected_keeps_the_reader(self):
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        master, path = self.new_pty(raw=True)
        self.connect(master, path, via='signal')
        self.bca_cmd(self.bca, f'signal 15 2 3 0 {path}')    # CONNECTING for the open path
        time.sleep(2)
        self.assertFalse([r for r in self.rows('pimu_link') if r['ev'] == 'disconnect'])
        self.assertIn(b'MP', self.read_master(master, 6))     # still reading and PINGing

    def bus_clients(self):
        out = subprocess.run(['dbus-send', '--bus=' + self.hmi_address, '--print-reply', '--dest=org.freedesktop.DBus',
                              '/org/freedesktop/DBus', 'org.freedesktop.DBus.ListNames'], capture_output=True, text=True).stdout
        return out.count('":1.')

    def test_quiesce_is_immediate(self):
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        master, path = self.new_pty(raw=True)
        self.connect(master, path, via='signal')
        clients = self.bus_clients()
        t0 = time.monotonic()
        shutil.rmtree(self.proc / '4242')                    # bds died
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'quiesce', what='quiesce', timeout=5)
        self.assertLess(time.monotonic() - t0, 2.6, 'quiesce within about one watch period')
        m2, p2 = self.new_pty(raw=True)
        self.bca_cmd(self.bca, f'signal 15 3 3 0 {p2}')
        self.assertEqual(self.read_master(m2, 3), b'')
        self.assertEqual(self.bus_clients(), clients - 1, 'the receiver closed its D-Bus connection')

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

    def test_bds_dying_during_discovery_quiesces(self):
        # bds A is seen once and replaced by B before the next scan (a crash right after the
        # start): the first sighting already counts, so this is a restart, never accepted silently.
        self.set_bds(4300, ['-interface', 'dbus', '-configFilePath', XML])
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'config', what='config')
        self.set_bds(4242, ['-interface', 'dbus', '-configFilePath', XML])
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'fail', what='restart')
        self.assertIn('reason=restart\n', (self.root / 'logs/bt-fail').read_text())
        self.assertTrue([r for r in self.rows('bt_watch') if r['ev'] == 'quiesce'])

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
            except BlockingIOError:
                pass
            except OSError as e:                              # EIO: no slave open = no reader
                if e.errno == errno.EIO:
                    hup += 1
        self.assertTrue([r for r in self.rows('pimu_link') if r['ev'] == 'silence'])
        self.assertEqual(hup, 0, 'the PTY master saw a hangup/EIO: no reader for a moment')

    # ---- third-round review probes (Fable r3f, Opus r3o), now regression tests ----

    class Ticks:
        """Counts the watch's opens of /proc/<pid>/stat (one per 1 s tick) with inotify, so a test
        can change the entry exactly between ticks."""
        IN_OPEN = 0x20

        def __init__(self, path):
            import ctypes
            self.libc = ctypes.CDLL(None, use_errno=True)
            self.fd = self.libc.inotify_init1(os.O_NONBLOCK)
            self.wd = self.libc.inotify_add_watch(self.fd, str(path).encode(), self.IN_OPEN)
            assert self.fd >= 0 and self.wd >= 0

        def wait_stat_open(self, timeout=5):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                try:
                    data = os.read(self.fd, 4096)
                except BlockingIOError:
                    time.sleep(.005)
                    continue
                if b'stat' in data:
                    return True
            return False

        def close(self):
            os.close(self.fd)

    def test_transient_unreadable_cmdline_is_not_a_restart(self):
        # Opus r3o: the known bds's cmdline unreadable (EACCES) for exactly two ticks, then readable.
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        ticks = self.Ticks(self.proc / '4242')
        self.addCleanup(ticks.close)
        c = self.proc / '4242' / 'cmdline'
        self.assertTrue(ticks.wait_stat_open())
        time.sleep(.2)
        os.chmod(c, 0)
        for _ in range(2):
            self.assertTrue(ticks.wait_stat_open())             # two ticks see EACCES
        time.sleep(.2)
        os.chmod(c, 0o644)
        time.sleep(2.5)
        self.assertFalse([r for r in self.rows('bt_watch') if r['ev'] in ('fail', 'quiesce')])
        self.assertFalse((self.root / 'logs/bt-fail').exists())

    def test_one_missing_read_is_not_gone(self):
        # The cmdline missing for exactly one tick, then back with the same identity.
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        ticks = self.Ticks(self.proc / '4242')
        self.addCleanup(ticks.close)
        c = self.proc / '4242' / 'cmdline'
        saved = c.read_bytes()
        self.assertTrue(ticks.wait_stat_open())
        time.sleep(.2)
        c.unlink()
        self.assertTrue(ticks.wait_stat_open())                 # one tick sees it missing
        time.sleep(.2)
        c.write_bytes(saved)
        time.sleep(3)
        self.assertFalse([r for r in self.rows('bt_watch') if r['ev'] in ('fail', 'quiesce')])

    def test_second_long_lived_process_does_not_hide_a_crash(self):
        # Fable r3f (b) / Opus r3o: bds P (start 1000) and another long-lived process C with the same
        # cmdline (start 1500). P dies, SM starts Q: a crash, whatever C is.
        self.set_bds(4242, ARGS, start=1000)
        self.set_bds(4300, ARGS, start=1500, keep=True)
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        master, path = self.new_pty(raw=True)
        self.connect(master, path, via='signal')
        self.assertEqual(self.rows('bt_watch')[0]['pid'], 4242)
        shutil.rmtree(self.proc / '4242')
        time.sleep(1.5)
        self.set_bds(4400, ARGS, start=9000, keep=True)
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'quiesce', what='quiesce', timeout=5)
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'fail', what='fail', timeout=5)
        self.read_master(master, 0.3)                           # PINGs written before the quiesce
        self.assertEqual(self.read_master(master, 6), b'', 'no PING on the old PTY after the crash')

    def test_pid_reuse_within_one_tick_is_a_restart(self):
        # Fable r3f (a2): the restarted bds gets the same pid with another start time.
        self.set_bds(4242, ARGS, start=1000)
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        self.set_bds(4242, ARGS, start=7777)
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'fail', what='restart', timeout=6)
        self.assertIn('reason=restart\n', (self.root / 'logs/bt-fail').read_text())

    def test_zombie_is_gone(self):
        # Fable r3f (a3): the bds's entry stays as a zombie (empty cmdline); the restarted bds appears.
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        (self.proc / '4242' / 'cmdline').write_bytes(b'')
        self.set_bds(4300, ARGS, start=5000, keep=True)
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'fail', what='restart', timeout=6)
        self.assertTrue([r for r in self.rows('bt_watch') if r['ev'] == 'quiesce'])

    def test_collector_before_bds(self):
        # Fable r3f (a1): no bds at the collector start, bds with our config appears 3 s later.
        shutil.rmtree(self.proc)
        (self.proc / 'self').mkdir(parents=True)
        self.start()
        time.sleep(3)
        self.assertFalse(self.rows('bt_watch'))
        self.set_bds(4242, ARGS, start=1000)
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        time.sleep(2)
        self.assertFalse([r for r in self.rows('bt_watch') if r['ev'] in ('fail', 'quiesce')])

    # ---- the BCA lives on the HMI bus (validation/BCA_HMI_BUS_FIX_2026-10-17.md) ----

    def ping_seen(self, master, seconds):
        return b'MP' in self.read_master(master, seconds)

    def test_pty_and_ping_only_via_the_hmi_bus(self):
        # Like the car: com.jci.bca owned on both buses, the object and the signal only on HMI.
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        master, path = self.new_pty(raw=True)
        self.connect(master, path, via='poll')
        self.assertTrue(self.ping_seen(master, 6), 'PING over the PTY found via the HMI bus')
        self.assertIn('query 15\n', self.bca_lines)
        self.assertNotIn('query 15\n', self.service_lines, 'no BCA call on the service bus')
        self.assertFalse([r for r in self.rows('pimu_link') if r['ev'] in ('poll_error', 'bus')])
        # The collector's own position polling stays on the service bus and keeps running.
        self.finish()
        kinds = [json.loads(l)['kind'] for l in (self.root / 'logs/collector.0.jsonl').read_text().splitlines()]
        self.assertTrue([k for k in kinds if k.startswith('position_poll')], kinds[-10:])

    def test_signal_via_the_hmi_bus_opens_and_pings(self):
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        master, path = self.new_pty(raw=True)
        self.connect(master, path, via='signal')
        self.assertTrue(self.ping_seen(master, 6))

    def test_service_bus_bca_gives_unknown_method_and_never_opens(self):
        # The pre-fix receiver (BCA asked on the service bus): the car's failure, now with the
        # error name in the row. Even a CONNECTED state on the HMI fake is never seen.
        self.start(bca_bus=self.address)
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        master, path = self.new_pty(raw=True)
        self.bca_cmd(self.bca, f'state 15 3 0 {path}')
        self.bca_cmd(self.bca, f'signal 15 3 3 0 {path}')
        row = self.wait_row(lambda r: r['kind'] == 'pimu_link' and r['ev'] == 'poll_error', what='poll_error')[0]
        self.assertEqual(row['why'], 'error')
        self.assertEqual(row['name'], 'org.freedesktop.DBus.Error.UnknownMethod')
        self.assertTrue(row['msg'].startswith("Method 'QueryConnectStatus' with signature 'u'"), row)
        self.assertLessEqual(len(row['msg']), 80)
        self.assertEqual(row['n'], 1)
        self.assertIn('query 15\n', self.service_lines)
        self.assertFalse(self.ping_seen(master, 6))
        self.assertFalse([r for r in self.rows('pimu_link') if r['ev'] in ('connect', 'replace')])
        # Same error name again: no further row until the 10th failure.
        self.assertEqual(len([r for r in self.rows('pimu_link') if r['ev'] == 'poll_error']), 1)

    def test_poll_error_rows_carry_a_sanitised_capped_name_and_message(self):
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        long_msg = 'quote" back\\slash ' + 'x' * 120
        self.bca_cmd(self.bca, 'error com.jci.bca.Error.Test ' + long_msg)
        row = self.wait_row(lambda r: r['kind'] == 'pimu_link' and r['ev'] == 'poll_error', what='poll_error')[0]
        self.assertEqual(row['name'], 'com.jci.bca.Error.Test')
        self.assertEqual(len(row['msg']), 80)
        self.assertTrue(row['msg'].startswith("quote' back/slash xxx"), row)
        self.bca_cmd(self.bca, 'error org.freedesktop.DBus.Error.Failed Error: unpredictable error')
        row2 = self.wait_row(lambda r: r['kind'] == 'pimu_link' and r['ev'] == 'poll_error'
                             and r['name'] == 'org.freedesktop.DBus.Error.Failed', what='second distinct name')[0]
        self.assertEqual(row2['msg'], 'Error: unpredictable error')
        self.assertGreaterEqual(row2['n'], 2)
        for line in (self.root / 'logs/pimu.0.jsonl').read_text().splitlines():
            self.assertLessEqual(len(line) + 1, 384)

    def test_reply_for_another_service_is_logged_once_and_never_opened(self):
        # The stock BCA answers serviceId 0, connStatus 6 when its table lacks service 15.
        master, path = self.new_pty(raw=True)
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        self.bca_cmd(self.bca, 'raw 0 6 0 -')
        row = self.wait_row(lambda r: r['kind'] == 'pimu_link' and r['ev'] == 'poll' and r['why'] == 'other',
                            what='other svc')[0]
        self.assertEqual((row['svc'], row['cs']), (0, 6))
        self.bca_cmd(self.bca, f'raw 14 3 0 {path}')       # another service CONNECTED: never opened
        self.wait_row(lambda r: r['kind'] == 'pimu_link' and r['why'] == 'other' and r['svc'] == 14, what='svc 14')
        time.sleep(11)                                      # two more polls of each answer
        others = [r for r in self.rows('pimu_link') if r['why'] == 'other']
        self.assertEqual([(r['svc'], r['cs']) for r in others], [(0, 6), (14, 3)])
        self.assertFalse([r for r in self.rows('pimu_link') if r['ev'] in ('connect', 'replace')])
        self.assertFalse(self.ping_seen(master, 1))

    def test_hmi_socket_missing_fails_closed_and_retries(self):
        absent = self.root / 'hmi-late'
        self.start(bca_bus='unix:path=' + str(absent))
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        row = self.wait_row(lambda r: r['kind'] == 'pimu_link' and r['ev'] == 'bus', what='bus row')[0]
        self.assertEqual(row['why'], 'unavailable')
        self.assertTrue(row['name'].startswith('org.freedesktop.DBus.Error.'), row)
        self.assertLessEqual(len(row['msg']), 80)
        self.assertNotIn('query 15\n', self.service_lines + self.bca_lines)
        # The HMI daemon appears later: the 10 s retry connects and the PTY opens.
        late = self.start_daemon('hmi-late')
        bca = self.spawn_bca(address=late)
        master, path = self.new_pty(raw=True)
        self.bca_cmd(bca, f'state 15 3 0 {path}')
        self.wait_row(lambda r: r['kind'] == 'pimu_link' and r['ev'] == 'connect', what='connect after retry', timeout=25)
        self.assertTrue(self.ping_seen(master, 6))
        self.assertEqual(len([r for r in self.rows('pimu_link') if r['ev'] == 'bus']), 1, 'one bus row')
        self.finish()
        kinds = [json.loads(l)['kind'] for l in (self.root / 'logs/collector.0.jsonl').read_text().splitlines()]
        self.assertGreaterEqual(len([k for k in kinds if k.startswith('position_poll')]), 10, 'position polling unaffected')


    # ---- a stuck HMI daemon (validation/BCA_HMI_BUS_FIX_REVIEW_FABLE_2026-10-17.md 1-A) ----
    # The socket accepts but the peer never answers SASL ('silent'), or answers
    # SASL and then never answers Hello ('auth_only'). Before the fix,
    # dbus_bus_register() blocked the receiver thread (the PTY's only reader)
    # without limit in SASL and for 25 s per Hello attempt.

    class StuckBus:
        """A unix socket in place of the HMI dbus-daemon. Records per connection
        the accept time, the time BEGIN arrived (auth_only) and the EOF time."""
        def __init__(self, path, kind):
            self.kind, self.conns, self.path = kind, [], str(path)
            self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.srv.bind(self.path)
            self.srv.listen(64)
            threading.Thread(target=self.accept_loop, daemon=True).start()

        def accept_loop(self):
            while True:
                try:
                    c, _ = self.srv.accept()
                except OSError:
                    return
                rec = {'accept': time.monotonic(), 'begin': None, 'eof': None, 'sock': c}
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
                        continue                      # silent: never a byte back
                    buf += chunk
                    while b'\r\n' in buf:
                        line, buf = buf.split(b'\r\n', 1)
                        line = line.lstrip(b'\0')
                        if line.startswith(b'AUTH'):
                            c.sendall(b'OK ' + b'0123456789abcdef' * 2 + b'\r\n')
                        elif line.startswith(b'NEGOTIATE_UNIX_FD'):
                            c.sendall(b'AGREE_UNIX_FD\r\n')
                        elif line.startswith(b'BEGIN'):
                            rec['begin'] = time.monotonic()
                            authed = True              # Hello follows: never answered
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

    def pump(self, phone, seconds, seq, pings):
        """The phone streams 50 Hz frames and answers PINGs for `seconds`;
        appends PING arrival times. Phone.send fails if the receiver stops
        reading for 2 s."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            phone.send(data_frame(seq[0], 0)); seq[0] += 1
            before = phone.pings
            phone.service()
            if phone.pings > before:
                pings.append(time.monotonic())
            time.sleep(.02)

    def stuck_hmi_daemon_case(self, kind, name, attempt_max_s):
        self.start()
        self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', what='ready')
        master, path = self.new_pty(raw=True)
        self.connect(master, path, via='poll')
        phone, seq, pings = self.Phone(master, 0, []), [0], []
        self.pump(phone, 6, seq, pings)
        self.assertTrue(pings, 'PING before the hang')
        # The HMI daemon dies and a stuck peer takes its socket path.
        sock = self.root / 'hmibus'
        self.stop(self.bca)
        self.stop(self.daemons['hmibus'])
        if sock.exists():
            sock.unlink()
        stuck = self.StuckBus(sock, kind)
        self.addCleanup(stuck.close)
        t_hang = time.monotonic()
        pings = []
        self.pump(phone, 32, seq, pings)
        gaps = [b - a for a, b in zip([t_hang] + pings, pings + [time.monotonic()])]
        attempts = [(r['accept'] - t_hang, (r['eof'] or time.monotonic()) - r['accept'], r['begin'])
                    for r in list(stuck.conns)]
        print(f'\n{kind}: PINGs at', [round(t - t_hang, 1) for t in pings], 'max gap %.2f s' % max(gaps),
              'attempts (start s, duration s):', [(round(a, 1), round(d, 2)) for a, d, _ in attempts], flush=True)
        self.assertGreaterEqual(len(attempts), 2, 'retries every 10 s')
        for _, duration, begin in attempts:
            self.assertLessEqual(duration, attempt_max_s, attempts)   # every attempt ends, the socket is closed
            if kind == 'auth_only':
                self.assertIsNotNone(begin, 'SASL completed, Hello sent')
        self.assertLess(max(gaps), 7.5, ('the receiver kept reading and PINGing', gaps))
        self.assertGreaterEqual(len(pings), 5)
        rows = [r for r in self.rows('pimu_link') if r['ev'] == 'bus']
        self.assertTrue(rows and rows[0]['name'] == name and rows[0]['why'] == 'unavailable', rows)
        self.assertEqual(len(rows), 1, 'one row per distinct error name')
        # A real daemon returns at the same path: the receiver reconnects and
        # re-registers (matches and owner), so a new signal replaces the PTY.
        stuck.close()
        sock.unlink()
        self.hmi_address = self.start_daemon('hmibus')
        self.bca = self.spawn_bca()
        master2, path2 = self.new_pty(raw=True)
        deadline = time.monotonic() + 25
        while not [r for r in self.rows('pimu_link') if r['ev'] == 'replace' and r['path'] == path2]:
            self.assertLess(time.monotonic(), deadline, self.rows()[-8:])
            self.bca_cmd(self.bca, f'signal 15 3 3 0 {path2}')
            self.pump(phone, 1, seq, [])
        self.assertTrue(self.ping_seen(master2, 6))

    def test_silent_hmi_socket_times_out_auth_and_keeps_reading(self):
        self.stuck_hmi_daemon_case('silent', 'mx5dr.AuthTimeout', 2.6)

    def test_hello_never_answered_times_out_and_keeps_reading(self):
        self.stuck_hmi_daemon_case('auth_only', 'org.freedesktop.DBus.Error.NoReply', 1.6)


ARGS = ['-interface', 'dbus', '-configFilePath', XML]

if __name__ == '__main__':
    unittest.main()
