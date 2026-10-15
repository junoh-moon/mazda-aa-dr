#!/usr/bin/env python3
"""Phone IMU channel end-to-end interop rig (validation/PHONE_IMU_CHANNEL_2026-10-14.md).

Runs the REAL phone Java core (phone/imu-sender core: Link, ReconnectPolicy, Session, Integrator,
FrameParser, Protocol; driven by tests/phone_imu/interop/PhoneSim.java) against the REAL collector
receiver (src/collector/phone_imu_rx.h in build/test_collector) through a fake bds relay:

  PhoneSim --TCP ("RFCOMM")--> FakeBds relay --host PTY master--> PTY slave <-- collector receiver
                                     |                                               |
                                     +-- fake BCA (build/fake_bca) on a private dbus-daemon -+
                                     +-- authored fake /proc entry of the bds process

The relay copies bds statePassThru as described in the feasibility records (one PTY per RFCOMM
connection; each received chunk written to the PTY master in full, then usleep(writeDelay = 3 ms);
the other direction read in 1024-byte units). It can kill and restart "bds" (new /proc pid, new PTY)
and drop the radio ACL. Nothing here is OEM code; RFCOMM/BlueGo timing, pairing and the real BCA
are not modelled, so a PASS is host interop evidence only, never vehicle evidence.

Usage: rig.py [--build DIR] [--out DIR] [scenario ...]
Scenarios: normal, firstping, firstping_poll, crash, flap, garbage, slow (default: all).
Each scenario prints CHECK lines (assertions; any failure makes the exit status 1) and MEASURE lines
(measurements, never a verdict). A JSON summary is written to <out>/summary.json.
Load-sensitive: the "relay PTY writes never blocked" (< 200 ms) and reconnect/first-PING timing checks
measure host scheduling as well; run on a quiet host (1-minute load below 4) and rerun a timing
failure that coincides with a load spike (the load at each scenario start is recorded).
"""
import argparse
import fcntl
import json
import math
import os
from pathlib import Path
import random
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import tty
import zlib

REPO = Path(__file__).resolve().parents[3]
PHONE_SRC = REPO / 'phone/imu-sender/src/me/sixtyfive/mx5imu/core'
SIM_SRC = Path(__file__).resolve().with_name('PhoneSim.java')
XML = '/data_persist/mx5-aa-dr/bt/BdsConfiguration.xml'
SVC = 15
CS_CONNECTED, CS_DISCONNECTED = 3, 6
BUS_CONF = '''<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:path={path}</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/></policy>
</busconfig>
'''


def mono_ms():
    return time.monotonic() * 1000.0   # CLOCK_MONOTONIC: same clock as the collector rows and Java nanoTime


# ---------------------------------------------------------------- results
class Results:
    def __init__(self):
        self.items = []
        self.scenario = None

    def check(self, ok, what, detail=''):
        self.items.append({'scenario': self.scenario, 'type': 'CHECK', 'ok': bool(ok), 'what': what, 'detail': str(detail)[:600]})
        print(f"  CHECK {'PASS' if ok else 'FAIL'}: {what}" + (f' ({str(detail)[:300]})' if detail != '' else ''), flush=True)

    def measure(self, what, value):
        self.items.append({'scenario': self.scenario, 'type': 'MEASURE', 'what': what, 'value': value})
        print(f'  MEASURE {what}: {json.dumps(value)}', flush=True)

    def failed(self):
        return any(i['type'] == 'CHECK' and not i['ok'] for i in self.items)


# ---------------------------------------------------------------- wire helpers (independent of C++/Java)
def ping_frames(stream):
    """(offset, frame) of every CRC-valid v1 MP frame in a byte string (used for the CMU->phone side)."""
    out, i = [], 0
    while True:
        i = stream.find(b'MP', i)
        if i < 0 or len(stream) - i < 28:
            return out
        f = stream[i:i + 28]
        if f[2] == 1 and zlib.crc32(f[:24]) == struct.unpack('<I', f[24:])[0]:
            out.append((i, f))
            i += 28
        else:
            i += 1


def read_tee(path):
    """PhoneSim tee: records (t_ns big-endian i64, len big-endian i32, bytes) of every phone write."""
    data = Path(path).read_bytes() if Path(path).exists() else b''
    recs, p = [], 0
    while p + 12 <= len(data):
        t, n = struct.unpack('>qi', data[p:p + 12])
        recs.append((t, data[p + 12:p + 12 + n]))
        p += 12 + n
    return recs


def tee_frames(recs):
    """Phone writes are whole frames (Session writes one frame per call)."""
    out = []
    for t, b in recs:
        if b[:2] == b'MX' and len(b) == 32:
            v, fl, seq, tend, d, w, bias, crc = struct.unpack('<BBIqiIiI', b[2:32])
            out.append({'t_ns': t, 'kind': 'data', 'flags': fl, 'seq': seq, 't_end_ns': tend, 'dth': d, 'wus': w, 'bias': bias})
        elif b[:2] == b'MP' and len(b) == 28:
            v, ty, seq, tc, tp, crc = struct.unpack('<BBIqqI', b[2:28])
            out.append({'t_ns': t, 'kind': 'pong' if ty == 1 else 'mp%d' % ty, 'seq': seq, 't_cmu': tc, 't_phone': tp})
        else:
            out.append({'t_ns': t, 'kind': 'other', 'len': len(b)})
    return out


# ---------------------------------------------------------------- environment: dbus, fake BCA, fake /proc, collector
class Env:
    def __init__(self, build, out, name, signals=True):
        self.build, self.name = build, name
        self.dir = Path(tempfile.mkdtemp(prefix=f'pimu-{name}-', dir=out))
        self.root = self.dir / 'pkg'
        (self.root / 'logs').mkdir(parents=True)
        (self.root / 'bt').mkdir()
        (self.root / 'bt/BdsConfiguration.xml').write_text('<bdsConfig/>\n')
        (self.root / 'mx5dr.conf').write_text('mode=BETA\nmax_log_bytes=65536\nmax_log_files=2\nsample_ms=500\n')
        self.proc = self.dir / 'proc'
        self.ready = self.dir / 'bds_ready_flag'
        self.ready.write_text('')
        self.signals = signals
        self.procs = []
        self.bds_pid, self.bds_start = 4242, 1000
        self.set_bds(self.bds_pid)
        # unix socket paths are limited to 107 bytes: the bus socket lives in a short temporary directory
        self.busdir = Path(tempfile.mkdtemp(prefix='pimu-bus-'))
        bus = self.busdir / 'bus'
        conf = self.dir / 'bus.conf'
        conf.write_text(BUS_CONF.format(path=bus))
        self.daemon = self.spawn(['dbus-daemon', '--nofork', '--config-file', str(conf)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.address = 'unix:path=' + str(bus)
        deadline = time.monotonic() + 5
        while not bus.exists():
            if time.monotonic() > deadline:
                raise RuntimeError('dbus-daemon did not start')
            time.sleep(.02)
        self.bca = self.spawn([str(build / 'fake_bca'), self.address], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        line = self.bca.stdout.readline()
        if not line.startswith('ready '):
            raise RuntimeError('fake_bca: ' + line)
        self.bca_lines = []   # (mono_ms, line)
        self.bca_lock = threading.Lock()
        threading.Thread(target=self._bca_reader, daemon=True).start()
        self.collector = None

    def spawn(self, cmd, **kw):
        p = subprocess.Popen(cmd, **kw)
        self.procs.append(p)
        return p

    def _bca_reader(self):
        for line in iter(self.bca.stdout.readline, ''):
            with self.bca_lock:
                self.bca_lines.append((mono_ms(), line.strip()))

    def bca_cmd(self, text):
        with self.bca_lock:
            try:
                self.bca.stdin.write(text + '\n')
                self.bca.stdin.flush()
            except (BrokenPipeError, ValueError):
                pass

    def queries(self):
        with self.bca_lock:
            return [t for t, l in self.bca_lines if l == f'query {SVC}']

    # The stock BCA would publish this: QueryConnectStatus(15) state and (if broadcast) the signal.
    def bca_status(self, connected, path):
        cs = CS_CONNECTED if connected else CS_DISCONNECTED
        self.bca_cmd(f"state {SVC} {cs} 0 {path if connected else '-'}")
        if self.signals:
            self.bca_cmd(f"signal {SVC} {cs} 3 0 {path if path else '-'}")

    def set_bds(self, pid):
        """Authored /proc entry of a bds-shaped process (never an OEM process)."""
        if self.proc.exists():
            shutil.rmtree(self.proc)
        (self.proc / 'self').mkdir(parents=True)
        if pid:
            d = self.proc / str(pid)
            d.mkdir()
            d.joinpath('cmdline').write_bytes(b'\0'.join(x.encode() for x in ['/jci/bds/bds', '-interface', 'dbus', '-configFilePath', XML]) + b'\0')
            fields = ['S', '1'] + ['0'] * 18 + [str(self.bds_start)] + ['0'] * 10
            d.joinpath('stat').write_text('%d (bds) %s\n' % (pid, ' '.join(fields)))

    def start_collector(self, *extra):
        cmd = [str(self.build / 'test_collector'), '--root', str(self.root), '--bus-address', self.address,
               '--pimu-proc-root', str(self.proc), '--pimu-ready-flag', str(self.ready), *extra]
        self.collector = self.spawn(['nice', '-n', '19', *cmd], stdout=subprocess.DEVNULL, stderr=open(self.dir / 'collector.err', 'w'))

    def wait_ready(self, timeout=15):
        return self.wait_row(lambda r: r['kind'] == 'bt_watch' and r['ev'] == 'ready', timeout)

    def stop_collector(self):
        if self.collector and self.collector.poll() is None:
            try:
                os.kill(self.collector.pid, signal.SIGCONT)
            except OSError:
                pass
            (self.root / 'logs/collector.stop').mkdir(exist_ok=True)
            try:
                self.collector.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.collector.kill()
                self.collector.wait(timeout=5)

    def rows(self, kind=None):
        out = []
        for name in ('pimu.1.jsonl', 'pimu.0.jsonl'):
            p = self.root / 'logs' / name
            if p.exists():
                for line in p.read_text(errors='replace').splitlines():
                    if line.strip():
                        try:
                            out.append(json.loads(line))
                        except ValueError:
                            pass
        return [r for r in out if kind is None or r['kind'] == kind]

    def wait_row(self, pred, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            found = [r for r in self.rows() if pred(r)]
            if found:
                return found
            time.sleep(.1)
        return []

    def close(self):
        self.stop_collector()
        try:
            self.bca_cmd('quit')
        except Exception:
            pass
        for p in reversed(self.procs):
            if p.poll() is None:
                p.kill()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
        shutil.rmtree(self.busdir, ignore_errors=True)


# ---------------------------------------------------------------- fake bds relay
class Conn:
    def __init__(self, sock, master, slave, path, n):
        self.sock, self.master, self.slave, self.path, self.n = sock, master, slave, path, n
        self.closed = False
        self.t_open = mono_ms()
        self.first_ping_ms = None
        self.pings = 0
        self.up_bytes = 0


class FakeBds:
    """bds statePassThru stand-in. One PTY per RFCOMM (TCP) connection, noOfInstances = 1."""

    def __init__(self, env, chunk_max=127, write_delay=0.003, keep_slave=True, seed=7, on_acl=None):
        self.env = env
        self.chunk_max, self.write_delay, self.keep_slave = chunk_max, write_delay, keep_slave
        self.rnd = random.Random(seed)
        self.on_acl = on_acl or (lambda up: None)
        self.lock = threading.RLock()
        self.alive = True
        self.acl_up = True
        self.listener = None
        self.port = None
        self.conn = None
        self.conns = []
        self.events = []        # (mono_ms, text)
        self.writes = []        # (mono_ms at start, blocked_ms, nbytes) of every PTY master write (one bds event)
        self.lags = []          # recv -> fully written to the PTY, ms
        self.crashes = 0
        self.accepts = 0
        self.crash_on_connect = False
        self.restart_s = 8.0
        self.mangle_up = None   # function(frame bytes) -> bytes, applied per phone frame (garbage scenario)
        self.mangle_down = None
        self.stop = False
        self.reserve_names = []
        self.eof_linger = 0.5   # rig only: lets the receiver drain the last frames before the PTY closes
        self._listen()
        threading.Thread(target=self._accept_loop, daemon=True).start()

    def log(self, text):
        self.events.append((mono_ms(), text))

    def _listen(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(('127.0.0.1', self.port or 0))
        s.listen(1)
        s.settimeout(.2)
        self.port = s.getsockname()[1]
        self.listener = s

    def _unlisten(self):
        if self.listener:
            self.listener.close()
            self.listener = None

    def accepting(self):
        return self.alive and self.acl_up

    def _accept_loop(self):
        while not self.stop:
            with self.lock:
                lst = self.listener if self.accepting() else None
            if lst is None:
                time.sleep(.05)
                continue
            try:
                sock, _ = lst.accept()
            except (socket.timeout, OSError):
                continue
            with self.lock:
                if not self.accepting() or (self.conn and not self.conn.closed):
                    sock.close()            # one instance; a dead stack refuses
                    self.log('refused')
                    continue
                self.accepts += 1
                if self.crash_on_connect:
                    self.log('connect_then_crash')
                    sock.close()
                    self._crash_locked('on_connect')
                    continue
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                master, slave = os.openpty()
                path = os.ttyname(slave)
                os.set_blocking(master, False)
                if not self.keep_slave:
                    os.close(slave)
                    slave = None
                c = Conn(sock, master, slave, path, len(self.conns))
                self.conn = c
                self.conns.append(c)
                self.log(f'connect {path}')
            self.env.bca_status(True, path)
            threading.Thread(target=self._up, args=(c,), daemon=True).start()
            threading.Thread(target=self._down, args=(c,), daemon=True).start()

    # phone -> PTY master (statePassThru: write the whole event, then usleep(writeDelay))
    def _write_all(self, c, data):
        t0 = time.monotonic()
        done = 0
        while done < len(data) and not c.closed:
            try:
                done += os.write(c.master, data[done:])
            except BlockingIOError:
                select.select([], [c.master], [], .05)
            except OSError:
                break
        self.writes.append((t0 * 1000.0, (time.monotonic() - t0) * 1000.0, len(data)))

    def _up(self, c):
        buf = b''
        while not c.closed:
            try:
                data = c.sock.recv(4096)
            except OSError:
                data = b''
            t_rx = time.monotonic()
            if not data:
                time.sleep(self.eof_linger)
                self.disconnect(c, 'phone_eof')
                return
            c.up_bytes += len(data)
            if self.mangle_up:
                buf += data
                out = b''
                while len(buf) >= 2:
                    n = 32 if buf[:2] == b'MX' else 28 if buf[:2] == b'MP' else 1
                    if len(buf) < n:
                        break
                    out += self.mangle_up(buf[:n]) if n > 1 else buf[:1]
                    buf = buf[n:]
                data = out
            p = 0
            while p < len(data) and not c.closed:
                k = self.rnd.randint(1, self.chunk_max)
                self._write_all(c, data[p:p + k])
                p += k
                time.sleep(self.write_delay)
            self.lags.append((time.monotonic() - t_rx) * 1000.0)

    # PTY master -> phone (ptyReadHandler: up to 1024 bytes per read)
    def _down(self, c):
        while not c.closed:
            try:
                r, _, _ = select.select([c.master], [], [], .2)
            except (OSError, ValueError):
                return
            if not r:
                continue
            try:
                data = os.read(c.master, 1024)
            except BlockingIOError:
                continue
            except OSError:
                time.sleep(.05)      # EIO: no slave reader at the moment (only without keep_slave)
                continue
            if not data:
                time.sleep(.05)
                continue
            for _, f in ping_frames(data):
                c.pings += 1
                if c.first_ping_ms is None:
                    c.first_ping_ms = mono_ms()
            if self.mangle_down:
                data = self.mangle_down(data)
            try:
                c.sock.sendall(data)
            except OSError:
                self.disconnect(c, 'phone_gone')
                return

    def disconnect(self, c, why):
        with self.lock:
            if c.closed:
                return
            c.closed = True
            self.log(f'disconnect {c.path} {why}')
            try:
                c.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            c.sock.close()
            os.close(c.master)       # bds event -2: the PTY is closed
            if c.slave is not None:
                os.close(c.slave)
            if self.conn is c:
                self.conn = None
        self.env.bca_status(False, c.path)

    def drop_current(self, why):
        c = self.conn
        if c:
            self.disconnect(c, why)

    def acl(self, up):
        with self.lock:
            self.acl_up = up
            self.log('acl ' + ('up' if up else 'down'))
        if not up:
            self.drop_current('acl_down')
        self.on_acl(up)

    def _crash_locked(self, why):
        """bds dies: RFCOMM and ACL drop, the PTY closes, the /proc entry goes; SM restarts it later."""
        self.crashes += 1
        self.alive = False
        self.log(f'crash {why}')
        c = self.conn
        self._unlisten()
        self.env.set_bds(0)
        threading.Thread(target=self._after_crash, args=(c,), daemon=True).start()

    def _after_crash(self, c):
        if c:
            self.disconnect(c, 'crash')
        self.acl(False)
        time.sleep(self.restart_s)
        with self.lock:
            self.env.bds_pid += 17
            self.env.bds_start += 5000
            self.env.set_bds(self.env.bds_pid)
            # a restarted bds gets a fresh PTY name: hold one name so the next openpty differs
            m, s = os.openpty()
            self.reserve_names.append((m, s))
            self._listen()
            self.alive = True
            self.log(f'restart pid {self.env.bds_pid}')
        self.acl(True)

    def crash(self, why):
        with self.lock:
            self._crash_locked(why)

    def close(self):
        self.stop = True
        for c in list(self.conns):
            self.disconnect(c, 'rig_end')
        self._unlisten()
        for m, s in self.reserve_names:
            os.close(m)
            os.close(s)

    def write_stats(self, since_ms=None):
        w = [x for x in self.writes if since_ms is None or x[0] >= since_ms]
        if not w:
            return {'events': 0}
        b = sorted(x[1] for x in w)
        return {'events': len(w), 'bytes': sum(x[2] for x in w), 'max_blocked_ms': round(b[-1], 2),
                'p99_blocked_ms': round(b[int(.99 * (len(b) - 1))], 3), 'over_50ms': sum(1 for x in b if x > 50),
                'max_lag_ms': round(max(self.lags), 1) if self.lags else None}


# ---------------------------------------------------------------- fake phone process
class Phone:
    def __init__(self, cp, out_dir, port, offset_ns=987654321012345, accel=1.0, profile='60:0', tilt='0,0',
                 noise=0.0, breaker=None, tag='phone'):
        self.tee = out_dir / f'{tag}.tee'
        self.events = []
        self.offset_ns = offset_ns
        cmd = ['nice', '-n', '19', 'java', '-Xmx96m', '-XX:+UseSerialGC', '-cp', str(cp), 'me.sixtyfive.mx5imu.core.PhoneSim',
               '--port', str(port), '--offset-ns', str(offset_ns), '--accel', str(accel), '--profile', profile,
               '--tilt-deg', tilt, '--noise-rad-s', str(noise), '--tee', str(self.tee)]
        if breaker:
            cmd += ['--breaker', breaker]
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.lock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for line in iter(self.p.stdout.readline, ''):
            try:
                e = json.loads(line)
            except ValueError:
                e = {'ev': 'raw', 'text': line.strip(), 't_ms': mono_ms()}
            with self.lock:
                self.events.append(e)

    def cmd(self, text):
        try:
            self.p.stdin.write(text + '\n')
            self.p.stdin.flush()
        except (BrokenPipeError, ValueError, OSError):
            pass

    def ev(self, name):
        with self.lock:
            return [e for e in self.events if e['ev'] == name]

    def wait_ev(self, name, n=1, timeout=30, pred=lambda e: True):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            got = [e for e in self.ev(name) if pred(e)]
            if len(got) >= n:
                return got
            time.sleep(.05)
        return [e for e in self.ev(name) if pred(e)]

    def quit(self):
        self.cmd('quit')
        try:
            self.p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait(timeout=5)


# ---------------------------------------------------------------- receiver row helpers
def pimu_totals(rows):
    pimu = [r for r in rows if r['kind'] == 'pimu']
    t = {k: sum(r[k] for r in pimu) for k in ('ce', 'bv', 'sk', 'dr', 'fr')}
    t['frames'] = sum(b[0] for r in pimu for b in r['d'])
    t['dth'] = sum(b[1] for r in pimu for b in r['d'])
    t['gaps'] = sum(b[3] for r in pimu for b in r['d'])
    t['flags_or'] = 0
    for r in pimu:
        for b in r['d']:
            t['flags_or'] |= b[4]
    t['rows'] = len(pimu)
    return t


def drops(rows):
    d = [r for r in rows if r['kind'] == 'pimu_drop']
    return d[-1]['d'] if d else [0] * 9


def sync_check(res, rows, offset_ns, what):
    sy = [r for r in rows if r['kind'] == 'pimu_sync']
    bad = [s for s in sy if abs(s['off_us'] - offset_ns / 1000.0) > s['rtt_us'] / 2.0 + 1000]
    res.check(sy and not bad, f'{what}: every pimu_sync offset within rtt/2 + 1 ms of the simulated clock offset',
              {'syncs': len(sy), 'bad': bad[:3]})
    if sy:
        errs = sorted(abs(s['off_us'] - offset_ns / 1000.0) for s in sy)
        rtts = sorted(s['rtt_us'] for s in sy)
        res.measure(f'{what}: sync offset error us (median, max) and rtt us (median, max)',
                    [errs[len(errs) // 2], errs[-1], rtts[len(rtts) // 2], rtts[-1]])


def build_phone_classes(out):
    cp = out / 'classes'
    if cp.exists():
        shutil.rmtree(cp)
    cp.mkdir(parents=True)
    srcs = sorted(str(p) for p in PHONE_SRC.glob('*.java')) + [str(SIM_SRC)]
    subprocess.run(['nice', '-n', '19', 'javac', '--release', '11', '-Xlint:all', '-Werror', '-d', str(cp), *srcs], check=True)
    return cp


_PROFILES = {}


def profile_rate(profile, s):
    if profile not in _PROFILES:
        segs = [(float(a), math.radians(float(b))) for a, b in (x.split(':') for x in profile.split(','))]
        _PROFILES[profile] = (segs, sum(d for d, _ in segs))
    segs, total = _PROFILES[profile]
    x = s % total
    for d, r in segs:
        if x < d:
            return r
        x -= d
    return segs[-1][1]


def window_truth(profile, base_ns, frame, step_ns=5_000_000):
    """The rotation the simulated sensor reported inside one DATA window (samples in (start, end])."""
    t_end = frame['t_end_ns'] - base_ns
    t_start = t_end - frame['wus'] * 1000
    k0, k1 = t_start // step_ns + 1, t_end // step_ns
    return sum(profile_rate(profile, (k * step_ns) / 1e9) * step_ns / 1e9 for k in range(k0, k1 + 1))


def tilt_down(tilt):
    r, q = (math.radians(float(x)) for x in tilt.split(','))
    rx = [[1, 0, 0], [0, math.cos(r), -math.sin(r)], [0, math.sin(r), math.cos(r)]]
    ry = [[math.cos(q), 0, math.sin(q)], [0, 1, 0], [-math.sin(q), 0, math.cos(q)]]
    m = [[sum(ry[i][k] * rx[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    return [-m[i][2] for i in range(3)]     # m * (0,0,-1)


# ---------------------------------------------------------------- scenarios
PROFILE_A = ('30:0,6:15,20:0,9:10,40:0,15:-12,10:0,18:20,25:0,3:-30,12:0,'
             '30:2,8:0,20:-4.5,14:0')   # clockwise turns, straights, slow drifts, counter-clockwise turns


def scenario_normal(ctx, res):
    """(a) 10 simulated minutes at 3x real time through the relay, tilted mount, no noise."""
    env = Env(ctx.build, ctx.out, 'normal')
    ctx.cleanup.append(env.close)
    env.start_collector()
    res.check(env.wait_ready(), 'collector saw the bds with our config and the ready flag')
    bds = FakeBds(env)
    ctx.cleanup.append(bds.close)
    accel, sim_s, tilt = 3.0, 600.0, '27,-11'
    phone = Phone(ctx.cp, env.dir, bds.port, accel=accel, profile=PROFILE_A, tilt=tilt, tag='phone')
    ctx.cleanup.append(phone.quit)
    phone.cmd('acl up')
    st = phone.wait_ev('sensor_start', timeout=20)
    conn = phone.wait_ev('connected', timeout=20)
    res.check(conn, 'phone connected through the relay')
    t_conn = time.monotonic()
    time.sleep(sim_s / accel)
    stream_real_s = time.monotonic() - t_conn
    # stop the phone first (the sensor stops), let the pipeline drain, then close the link
    phone.quit()
    time.sleep(1.5)
    bds.close()
    time.sleep(1.0)
    env.stop_collector()
    rows = env.rows()
    frames = [f for f in tee_frames(read_tee(phone.tee)) if f['kind'] == 'data']
    pongs = [f for f in tee_frames(read_tee(phone.tee)) if f['kind'] == 'pong']
    t = pimu_totals(rows)
    disc = phone.ev('disconnected')
    res.measure('normal: real seconds streamed, phone DATA frames, receiver frames', [round(stream_real_s, 1), len(frames), t['frames']])
    res.check(len(frames) > 0.95 * (stream_real_s * accel * 50) - 300, 'phone sent ~50 Hz of simulated time',
              (len(frames), stream_real_s * accel * 50))
    res.check(t['frames'] == len(frames), 'receiver counted every DATA frame the phone wrote', (t['frames'], len(frames)))
    res.check(t['dth'] == sum(f['dth'] for f in frames), 'receiver dtheta sum == phone dtheta sum (exact, urad)',
              (t['dth'], sum(f['dth'] for f in frames)))
    seqs = [f['seq'] for f in frames]
    res.check(all(b - a == 1 for a, b in zip(seqs, seqs[1:])) and t['gaps'] == 0, 'seq continuous on the phone and no receiver gap',
              {'gaps': t['gaps']})
    res.check(t['ce'] == 0 and t['bv'] == 0 and t['sk'] == 0 and t['dr'] == 0, 'no CRC/version/skip/overload counts', t)
    base = st[0]['sensor_base_ns'] if st else 0
    per = [window_truth(PROFILE_A, base, f) for f in frames]
    truth_disc = sum(per)
    worst = max(abs(f['dth'] - w * 1e6) for f, w in zip(frames, per)) if frames else None
    res.check(worst is not None and worst <= 1.0, 'every phone DATA window equals the known rotation of its samples within 1 urad (Integrator, tilted mount)',
              worst)
    t_first = (frames[0]['t_end_ns'] - frames[0]['wus'] * 1000 - base) / 1e9
    t_last = (frames[-1]['t_end_ns'] - base) / 1e9
    rx_rad = t['dth'] * 1e-6
    tol = 0.5e-6 * len(frames) + 1e-6
    res.measure('normal: simulated span s, receiver total deg, sampled-profile truth deg', [round(t_last - t_first, 2), round(math.degrees(rx_rad), 4),
                                                                                         round(math.degrees(truth_disc), 4)])
    res.check(abs(rx_rad - truth_disc) <= tol, f'receiver total rotation == known profile (tilted mount {tilt}) within 0.5 urad per window',
              {'err_urad': round((rx_rad - truth_disc) * 1e6, 1), 'tol_urad': round(tol * 1e6, 1)})
    # per 10 s row: receiver rows are by CMU receipt time; check the 1 Hz bins sum to the row content in time order
    bins = [b for r in rows if r['kind'] == 'pimu' for b in r['d']]
    res.measure('normal: 1 Hz bins, frames per bin (min, median, max)', [len(bins)] + ([sorted(b[0] for b in bins)[0], sorted(b[0] for b in bins)[len(bins) // 2],
                                                                                        max(b[0] for b in bins)] if bins else []))
    down = tilt_down(tilt)
    bias_expect = round((0.0021 * down[0] - 0.0013 * down[1] + 0.0007 * down[2]) * 1e6)
    last_b = [r['b'] for r in rows if r['kind'] == 'pimu' and r['s0'] >= 0]
    res.check(last_b and all(abs(b - bias_expect) <= 1 for b in last_b), 'bias projection on the tilted gravity axis reaches the receiver',
              (bias_expect, last_b[:3]))
    res.check((t['flags_or'] & 0b0111) == 0b0111, 'bias_valid, stationary and gravity_valid flags seen', t['flags_or'])
    fr_phone = sum(1 for f in frames if f['flags'] & 8)
    res.measure('normal: frame_reset flags (phone-sent, receiver-counted)', [fr_phone, t['fr']])
    res.check(fr_phone >= 1 and t['fr'] == fr_phone and frames and frames[0]['flags'] & 8,
              'frame_reset (bit3) on the first DATA frame actually sent in the session reaches the receiver (rig F1)', [fr_phone, t['fr']])
    sync_check(res, rows, phone.offset_ns, 'normal')
    res.check(len(pongs) >= int(stream_real_s / 5) - 2, 'phone answered the 5 s PINGs', (len(pongs), stream_real_s))
    ws = bds.write_stats()
    res.measure('normal: relay PTY writes', ws)
    res.check(ws['max_blocked_ms'] < 200, 'relay PTY master writes never blocked (>= 200 ms; host scheduling noise is tens of ms)', ws)
    res.check(drops(rows)[:5] == [0] * 5, 'no rate/ring row drops', drops(rows))
    res.measure('normal: phone session end', disc[-1] if disc else None)


def scenario_firstping(ctx, res, poll_only=False):
    """(b) the phone connects before the CMU knows the PTY."""
    name = 'firstping_poll' if poll_only else 'firstping'
    env = Env(ctx.build, ctx.out, name, signals=not poll_only)
    ctx.cleanup.append(env.close)
    bds = FakeBds(env)
    ctx.cleanup.append(bds.close)
    breaker = '3,60000,60000,30000'   # open 60 s instead of 60 min (only shifts time; see the report)
    if not poll_only:
        # b1: phone first, collector started 6 s later; b2: then a reconnect with the receiver running (signal path)
        phone = Phone(ctx.cp, env.dir, bds.port, breaker=breaker, tag='phone')
        ctx.cleanup.append(phone.quit)
        phone.cmd('acl up')
        c = phone.wait_ev('connected', timeout=20)
        res.check(c, 'phone connected before the collector ran')
        time.sleep(6)
        env.start_collector()
        deadline = time.monotonic() + 20
        while not [x for x in bds.conns if x.first_ping_ms] and time.monotonic() < deadline:
            time.sleep(.1)
        c0 = bds.conns[0]
        lat = (c0.first_ping_ms - c0.t_open) if c0.first_ping_ms else None
        res.measure('firstping: collector started 6 s after the phone connected: connect -> first PING ms', lat)
        res.check(lat is not None and lat < 15000, 'first PING inside the phone 15 s window', lat)
        time.sleep(6)
        recs = tee_frames(read_tee(phone.tee))
        first_data = next((r for r in recs if r['kind'] == 'data'), None)
        first_pong = next((r for r in recs if r['kind'] == 'pong'), None)
        res.check(first_data and first_pong and first_pong['t_ns'] <= first_data['t_ns'], 'no DATA before the first PING (pong precedes the first DATA)',
                  (first_pong and first_pong['t_ns'], first_data and first_data['t_ns']))
        # reconnect with the receiver idle: ACL drop and return
        bds.acl(False)
        phone.cmd('acl down')
        time.sleep(3)
        bds.acl(True)
        phone.cmd('acl up')
        deadline = time.monotonic() + 20
        while len([x for x in bds.conns if x.first_ping_ms]) < 2 and time.monotonic() < deadline:
            time.sleep(.1)
        c1 = bds.conns[-1] if len(bds.conns) > 1 else None
        lat2 = (c1.first_ping_ms - c1.t_open) if c1 and c1.first_ping_ms else None
        res.measure('firstping: reconnect with signals: connect -> first PING ms', lat2)
        res.check(lat2 is not None and lat2 < 2000, 'signal path opens the new PTY and PINGs within 2 s', lat2)
        dd = [e for e in phone.ev('disconnected')]
        res.check(sum(e['frames'] for e in dd) >= 0 and all(e['reason'] != 'no_ping' for e in dd), 'no no_ping session', [e['reason'] for e in dd])
        phone.quit()
        bds.close()
        env.stop_collector()
        return
    # b3: QueryConnectStatus only (the BCA never broadcasts for service 15): v3 keeps the period at 5 s
    # until the first PTY of the boot was opened (the phone waits only 15 s for its first PING).
    env.start_collector()
    res.check(env.wait_ready(), 'collector ready')
    time.sleep(140)                      # v2 had grown to 60 s by now (0, 5, 15, 35, 75, 135 s)
    q = env.queries()
    gaps = [round((b - a) / 1000.0, 1) for a, b in zip(q, q[1:])]
    res.measure('firstping_poll: QueryConnectStatus intervals before the phone came (s)', gaps)
    res.check(gaps and max(gaps) <= 6.5, 'query period stays 5 s until the first PTY of the boot', gaps)
    phone = Phone(ctx.cp, env.dir, bds.port, breaker=breaker, tag='phone')
    ctx.cleanup.append(phone.quit)
    phone.cmd('acl up')
    t_start = time.monotonic()
    # watch up to 6 minutes or until the phone locks or streams
    while time.monotonic() - t_start < 360:
        if [e for e in phone.ev('breaker') if e['state'] == 'LOCKED']:
            break
        if [x for x in bds.conns if x.pings >= 3]:
            break
        time.sleep(.5)
    q = env.queries()
    sess = phone.ev('disconnected')
    lat = [round((x.first_ping_ms - x.t_open) / 1000.0, 1) if x.first_ping_ms else None for x in bds.conns]
    res.measure('firstping_poll: per connection: seconds to first PING (None = none)', lat)
    res.measure('firstping_poll: phone session end reasons', [e['reason'] for e in sess])
    res.measure('firstping_poll: breaker events', [(e['state']) for e in phone.ev('breaker')])
    res.measure('firstping_poll: query intervals while the phone tried (s)',
                [round((b - a) / 1000.0, 1) for a, b in zip(q, q[1:]) if a >= t_start * 1000 - 60000])
    no_ping = [e for e in sess if e['reason'] == 'no_ping']
    res.check(all(e['frames'] == 0 for e in no_ping), 'no DATA was sent in no_ping sessions', [e['frames'] for e in no_ping])
    res.check(not no_ping and not phone.ev('breaker') and lat and lat[0] is not None and lat[0] < 15,
              'poll only: the first PING arrives inside the 15 s window (no no_ping session, no breaker trip)', lat)
    phone.quit()
    bds.close()
    env.stop_collector()


def scenario_crash(ctx, res):
    """(c) bds dies after the PTY was opened, SM restarts it with the same args, new PTY name."""
    out = {}
    for model, first_s in (('restart_healthy', 20), ('crash_every_connect', 20), ('crash_every_connect_long_first', 70)):
        env = Env(ctx.build, ctx.out, 'crash-' + model)
        ctx.cleanup.append(env.close)
        env.start_collector()
        res.check(env.wait_ready(), f'{model}: collector ready')
        phone_box = []
        bds = FakeBds(env, on_acl=lambda up: phone_box and phone_box[0].cmd('acl up' if up else 'acl down'))
        ctx.cleanup.append(bds.close)
        breaker = '3,60000,60000,30000'
        phone = Phone(ctx.cp, env.dir, bds.port, breaker=breaker, tag='phone')
        phone_box.append(phone)
        ctx.cleanup.append(phone.quit)
        phone.cmd('acl up')
        deadline = time.monotonic() + 20
        while not [x for x in bds.conns if x.first_ping_ms] and time.monotonic() < deadline:
            time.sleep(.1)
        res.check(bds.conns and bds.conns[0].first_ping_ms, f'{model}: first session streaming')
        time.sleep(first_s)
        old_path = bds.conns[0].path
        if model != 'restart_healthy':
            bds.crash_on_connect = True
        bds.crash('after_pty_open')
        t0 = time.monotonic()
        while time.monotonic() - t0 < 420:
            if [e for e in phone.ev('breaker') if e['state'] == 'LOCKED']:
                break
            time.sleep(.5)
        time.sleep(3)
        rows = env.rows()
        bt = [(r['ev'], r['why']) for r in rows if r['kind'] == 'bt_watch']
        links = [(r['ev'], r['why'], r['path']) for r in rows if r['kind'] == 'pimu_link']
        opened_after = [x for x in links if x[0] in ('connect', 'replace') and x[2] != old_path]
        later_pings = sum(x.pings for x in bds.conns[1:])
        locked = bool([e for e in phone.ev('breaker') if e['state'] == 'LOCKED'])
        # v3: any change of the known bds pids quiesces at once; bt-fail says restart (or vanished)
        res.check(('quiesce', 'bds_gone') in bt and (('fail', 'restart') in bt or ('fail', 'vanished') in bt),
                  f'{model}: collector quiesced and wrote bt-fail (restart or vanished)', bt)
        res.check((env.root / 'logs/bt-fail').exists(), f'{model}: logs/bt-fail exists')
        res.check(not opened_after and later_pings == 0, f'{model}: no PTY of the restarted bds opened, no PING after the crash',
                  {'opened': opened_after, 'pings': later_pings})
        res.check(locked, f"{model}: the phone breaker locked (no further attempts until 'retry now')",
                  [e['state'] for e in phone.ev('breaker')])
        out[model] = {'crashes': bds.crashes, 'connections': bds.accepts, 'phone_sessions': [e['reason'] for e in phone.ev('disconnected')],
                      'connect_failed': len(phone.ev('connect_failed')), 'breaker': [e['state'] for e in phone.ev('breaker')],
                      'rig_seconds': round(time.monotonic() - t0, 1)}
        res.measure(f'crash/{model}', out[model])
        phone.quit()
        bds.close()
        env.stop_collector()
    res.measure('crash: bds crashes per app start before LOCKED (24 h bound: no attempt after LOCKED)',
                {k: v['crashes'] for k, v in out.items()})
    # v3 bound in one CMU boot: a session with PINGs is never a failure, the CMU quiesces after the first
    # crash, then every no-PING session counts: 1 + 3 + 3 (OPEN, then LOCKED). Record 3a.
    res.check(all(v['crashes'] <= 7 for v in out.values()), 'crashes per app start before LOCKED <= 7 (record 3a bound)',
              {k: v['crashes'] for k, v in out.items()})


def scenario_flap(ctx, res):
    """(d) ACL flapping: (d1) 2 s down every ~75 s, (d2) 2 s down every 20 s."""
    for model, up_s, cycles in (('slow_flap', 75, 3), ('fast_flap', 20, 9)):
        env = Env(ctx.build, ctx.out, 'flap-' + model)
        ctx.cleanup.append(env.close)
        env.start_collector()
        res.check(env.wait_ready(), f'{model}: collector ready')
        bds = FakeBds(env)
        ctx.cleanup.append(bds.close)
        phone = Phone(ctx.cp, env.dir, bds.port, breaker='3,60000,60000,30000', tag='phone')
        ctx.cleanup.append(phone.quit)
        phone.cmd('acl up')
        reconnect = []
        for i in range(cycles):
            n_before = len(bds.conns)
            t_up = time.monotonic()
            time.sleep(up_s)
            bds.acl(False)
            phone.cmd('acl down')
            time.sleep(2)
            bds.acl(True)
            phone.cmd('acl up')
            t_back = mono_ms()
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                newc = [x for x in bds.conns if x.t_open >= t_back]
                if newc and newc[0].first_ping_ms:
                    reconnect.append(round(newc[0].first_ping_ms - t_back))
                    break
                time.sleep(.1)
            else:
                reconnect.append(None)
        time.sleep(up_s)
        phone.quit()
        time.sleep(1)
        recs = tee_frames(read_tee(phone.tee))
        data = [r for r in recs if r['kind'] == 'data']
        bds.close()
        time.sleep(1)
        env.stop_collector()
        rows = env.rows()
        t = pimu_totals(rows)
        sess = phone.ev('disconnected')
        res.measure(f'{model}: ACL up -> first PING ms per cycle (None = no reconnect within 8 s)', reconnect)
        res.measure(f'{model}: phone sessions / breaker', {'reasons': [e['reason'] for e in sess],
                                                           'breaker': [e['state'] for e in phone.ev('breaker')]})
        res.measure(f'{model}: phone DATA vs receiver frames, receiver frame_reset count, row drops',
                    [len(data), t['frames'], t['fr'], drops(rows)])
        lost = len(data) - t['frames']
        drops_n = cycles
        res.check(0 <= lost <= 5 * drops_n, f'{model}: receiver counted every DATA frame except at most 5 in flight per ACL drop',
                  {'phone': len(data), 'receiver': t['frames'], 'lost': lost})
        seq_jumps = sum(1 for a, b in zip(data, data[1:]) if b['seq'] != (a['seq'] + 1) & 0xFFFFFFFF)
        res.check(seq_jumps == 0 and t['gaps'] == 0, f'{model}: seq continuous across sessions (Link-wide counter), no receiver gap',
                  {'phone_jumps': seq_jumps, 'rx_gaps': t['gaps']})
        # v3: a session that received a PING is never a breaker failure (rig F4): flapping never trips
        res.check(not phone.ev('breaker') and all(r is not None and r < 5000 for r in reconnect),
                  f'{model}: PINGed sessions: no breaker trip, every return reconnected and PINGed within 5 s', reconnect)
        ws = bds.write_stats()
        res.check(ws.get('max_blocked_ms', 0) < 200, f'{model}: relay PTY writes never blocked', ws)


def scenario_garbage(ctx, res):
    """(e) corrupt and foreign bytes in both directions; resynchronisation."""
    env = Env(ctx.build, ctx.out, 'garbage')
    ctx.cleanup.append(env.close)
    env.start_collector()
    res.check(env.wait_ready(), 'collector ready')
    rnd = random.Random(11)
    ledger = {'data_ok': 0, 'dth_ok': 0, 'data_bad': 0, 'pong_bad': 0, 'garbage_bytes': 0, 'ping_bad': 0, 'pings_seen': 0}
    active = {'on': False}

    def garbage():
        n = rnd.randint(1, 40)
        alpha = b'MXP\x01\x00\x02\xff\r\n\x03\x04' + bytes(range(32, 127))
        g = bytes(rnd.choice(alpha) for _ in range(n))
        ledger['garbage_bytes'] += n
        return g

    def up(frame):
        if not active['on']:
            if frame[:2] == b'MX':
                ledger['data_ok'] += 1
                ledger['dth_ok'] += struct.unpack('<i', frame[16:20])[0]
            return frame
        pre = garbage() if rnd.random() < 0.08 else b''
        if rnd.random() < 0.02:
            b = bytearray(frame)
            b[rnd.randrange(len(b))] ^= 1 << rnd.randrange(8)
            if frame[:2] == b'MX':
                ledger['data_bad'] += 1
            else:
                ledger['pong_bad'] += 1
            return pre + bytes(b)
        if frame[:2] == b'MX':
            ledger['data_ok'] += 1
            ledger['dth_ok'] += struct.unpack('<i', frame[16:20])[0]
        return pre + frame

    def down(data):
        if not active['on']:
            return data
        out = bytearray()
        pf = ping_frames(data)
        if pf:
            ledger['pings_seen'] += 1
        if pf and ledger['pings_seen'] == 4:
            b = bytearray(data)
            i = ping_frames(data)[0][0] + rnd.randrange(28)
            b[i] ^= 0x10
            ledger['ping_bad'] += 1
            data = bytes(b)
        if rnd.random() < 0.5:
            out += garbage()
        return bytes(out) + data

    bds = FakeBds(env)
    bds.mangle_up, bds.mangle_down = up, down
    ctx.cleanup.append(bds.close)
    phone = Phone(ctx.cp, env.dir, bds.port, profile='5:0,3:30,4:0,3:-30', tag='phone')
    ctx.cleanup.append(phone.quit)
    phone.cmd('acl up')
    deadline = time.monotonic() + 20
    while not [x for x in bds.conns if x.first_ping_ms] and time.monotonic() < deadline:
        time.sleep(.1)
    time.sleep(3)
    active['on'] = True
    time.sleep(90)
    active['on'] = False
    time.sleep(8)
    phone.quit()
    time.sleep(1.5)
    bds.close()
    time.sleep(1)
    env.stop_collector()
    rows = env.rows()
    t = pimu_totals(rows)
    sess = phone.ev('disconnected')
    res.measure('garbage: relay ledger', ledger)
    res.measure('garbage: receiver totals', t)
    res.measure('garbage: phone sessions', [{k: e[k] for k in ('reason', 'crc_errors', 'pings', 'pongs', 'unexpected')} for e in sess])
    res.check(t['frames'] == ledger['data_ok'] and t['dth'] == ledger['dth_ok'],
              'receiver counted exactly the DATA frames the relay delivered intact (none lost to resync)', (t['frames'], ledger['data_ok']))
    res.check(t['ce'] > 0 and t['sk'] > 0, 'corrupt frames counted (CRC) and garbage bytes skipped', t)
    res.check(sess and sess[0]['crc_errors'] >= 1, 'phone parser counted the corrupted PING and resynchronised', sess[:1])
    early = [e for e in sess[:-1]]
    # v3: the phone closes after 15 s (three PING periods) without a PING: one lost PING never closes it
    res.check(not any(e['reason'] == 'silence' for e in sess), 'one corrupted PING never closes the phone session',
              [e['reason'] for e in sess])
    sy = [r for r in rows if r['kind'] == 'pimu_sync']
    res.check(len(sy) >= 10, 'PING/PONG sync rows kept arriving through the garbage', len(sy))
    sync_check(res, rows, phone.offset_ns, 'garbage')


def pty_capacity(raw=True):
    m, s = os.openpty()
    if raw:
        tty.setraw(s)
    os.set_blocking(m, False)
    n = 0
    try:
        while True:
            n += os.write(m, b'\x55' * 256)
    except BlockingIOError:
        pass
    os.close(m)
    os.close(s)
    return n


def scenario_slow(ctx, res):
    """(f) the receiver blocked (SIGSTOP of the collector) or slow (duty-cycled stops)."""
    cap_raw, cap_cooked = pty_capacity(True), pty_capacity(False)
    res.measure('slow: host PTY master->slave capacity without a reader, raw / cooked bytes', [cap_raw, cap_cooked])
    res.measure('slow: seconds of 50 Hz DATA (1600 B/s) a stopped reader can absorb before bds blocks (host kernel)', round(cap_raw / 1600.0, 1))
    env = Env(ctx.build, ctx.out, 'slow')
    ctx.cleanup.append(env.close)
    env.start_collector()
    res.check(env.wait_ready(), 'collector ready')
    bds = FakeBds(env)
    ctx.cleanup.append(bds.close)
    phone = Phone(ctx.cp, env.dir, bds.port, tag='phone')
    ctx.cleanup.append(phone.quit)
    phone.cmd('acl up')
    deadline = time.monotonic() + 20
    while not [x for x in bds.conns if x.first_ping_ms] and time.monotonic() < deadline:
        time.sleep(.1)
    time.sleep(10)
    # f1: slow receiver, 60 s of 300 ms stop / 200 ms run
    t_f1 = mono_ms()
    end = time.monotonic() + 60
    while time.monotonic() < end:
        os.kill(env.collector.pid, signal.SIGSTOP)
        time.sleep(.3)
        os.kill(env.collector.pid, signal.SIGCONT)
        time.sleep(.2)
    ws1 = bds.write_stats(since_ms=t_f1)
    res.measure('slow/f1 duty-cycled receiver: relay PTY writes', ws1)
    res.check(ws1['max_blocked_ms'] < 200, 'f1: relay never blocked while the receiver ran 40 % of the time', ws1)
    s1 = phone.ev('disconnected')
    res.check(not s1, 'f1: phone session survived', s1)
    # f2: receiver stopped for 25 s
    time.sleep(5)
    t_f2 = mono_ms()
    conn_before = bds.conn
    os.kill(env.collector.pid, signal.SIGSTOP)
    time.sleep(25)
    os.kill(env.collector.pid, signal.SIGCONT)
    ws2 = bds.write_stats(since_ms=t_f2)
    res.measure('slow/f2 receiver stopped 25 s: relay PTY writes', ws2)
    res.check(ws2['max_blocked_ms'] < 200, 'f2: relay never blocked: the phone paused DATA 11 s after the last PING, before the PTY filled', ws2)
    s2 = phone.ev('disconnected')
    res.measure('slow/f2: phone session end', [{k: e[k] for k in ('reason', 'frames', 'ping_span_ms')} for e in s2])
    res.check(s2 and s2[0]['reason'] in ('silence', 'tx_stall'), 'f2: the phone closed on PING silence', s2)
    # recovery
    deadline = time.monotonic() + 40
    while time.monotonic() < deadline and not [x for x in bds.conns if x is not conn_before and x.first_ping_ms]:
        time.sleep(.2)
    rec = [x for x in bds.conns if x is not conn_before and x.first_ping_ms]
    res.check(rec, 'f2: the phone reconnected and the receiver PINGed the new PTY', [(x.path, x.pings) for x in bds.conns])
    time.sleep(8)
    phone.quit()
    time.sleep(1.5)
    bds.close()
    time.sleep(1)
    env.stop_collector()
    rows = env.rows()
    t = pimu_totals(rows)
    data = [r for r in tee_frames(read_tee(phone.tee)) if r['kind'] == 'data']
    res.measure('slow: phone DATA frames, receiver frames, receiver discarded bytes (overload)', [len(data), t['frames'], t['dr']])
    lost = len(data) - t['frames']
    res.check(lost >= 0, 'receiver never counted more frames than sent', lost)
    if lost:
        res.measure('slow: frames lost to the backlog burst after SIGCONT (overload discard or closed PTY)', lost)
    links = [(r['ev'], r['why']) for r in rows if r['kind'] == 'pimu_link']
    res.measure('slow: receiver link rows', links)


SCENARIOS = {
    'normal': scenario_normal,
    'firstping': scenario_firstping,
    'firstping_poll': lambda ctx, res: scenario_firstping(ctx, res, poll_only=True),
    'crash': scenario_crash,
    'flap': scenario_flap,
    'garbage': scenario_garbage,
    'slow': scenario_slow,
}


class Ctx:
    pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', default=os.environ.get('MX5DR_TEST_BUILD', str(REPO / 'build')))
    ap.add_argument('--out', default=None)
    ap.add_argument('scenarios', nargs='*')
    a = ap.parse_args()
    build = Path(a.build)
    for need in ('test_collector', 'fake_bca'):
        if not (build / need).exists():
            print(f'SKIP: {build / need} missing (make {build}/{need})')
            return 77
    for tool in ('javac', 'java', 'dbus-daemon'):
        if not shutil.which(tool):
            print(f'SKIP: {tool} not found')
            return 77
    out = Path(a.out or tempfile.mkdtemp(prefix='pimu-interop-'))
    out.mkdir(parents=True, exist_ok=True)
    ctx = Ctx()
    ctx.build, ctx.out = build, out
    ctx.cp = build_phone_classes(out)
    res = Results()
    names = a.scenarios or list(SCENARIOS)
    for n in names:
        res.scenario = n
        ctx.cleanup = []
        print(f'== {n}', flush=True)
        # The fake bds is a Python relay with a 3 ms sleep per event: a host load spike (load average far above
        # the core count) can stall it for seconds, which the phone then sees as a stalled CMU (tx_stall) and the
        # receiver as an overload burst. Record the load so such a run can be judged; rerun on an idle host.
        res.measure(f'{n}: load average at start', open('/proc/loadavg').read().split()[:3])
        t0 = time.monotonic()
        try:
            SCENARIOS[n](ctx, res)
        except Exception as e:   # a crashed scenario is a failure, never a skip
            import traceback
            traceback.print_exc()
            res.check(False, f'{n}: scenario raised', repr(e))
        finally:
            for f in reversed(ctx.cleanup):
                try:
                    f()
                except Exception:
                    pass
        res.measure(f'{n}: wall seconds', round(time.monotonic() - t0, 1))
    (out / 'summary.json').write_text(json.dumps(res.items, indent=1))
    nf = sum(1 for i in res.items if i['type'] == 'CHECK' and not i['ok'])
    print(f'interop rig: {sum(1 for i in res.items if i["type"] == "CHECK")} checks, {nf} failed; {out}/summary.json')
    return 1 if res.failed() else 0


if __name__ == '__main__':
    sys.exit(main())
