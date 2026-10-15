#!/usr/bin/env python3
"""End-to-end host check of the Java phone encoder against the independent Python decoder.

  e2e_check.py golden FILE                 Python decoder agrees with tests/phone_imu/golden_frames.json
  e2e_check.py pings OUT.bin OUT.json      write a CMU->phone byte stream (pings, garbage, one corrupt ping)
  e2e_check.py check STREAM.bin PINGS.json SAMPLES.csv
      decode the phone->CMU stream written by the Java E2EDump, recompute every window's dtheta and
      bias from the raw synthetic samples per the spec, and check seq, pongs and stream rules.
"""
import csv
import json
import math
import random
import sys

from imu_wire import Decoder, encode_ping, fields_of


def fail(msg):
    print('FAIL:', msg)
    sys.exit(1)


def golden(path):
    entries = json.load(open(path))
    kinds = [e['kind'] for e in entries]
    if kinds.count('data') < 10 or kinds.count('ping') + kinds.count('pong') < 4 or kinds.count('corrupt') < 3:
        fail('golden file too small')
    for e in entries:
        d = Decoder()
        got = d.feed(bytes.fromhex(e['hex']))
        if e['kind'] == 'corrupt':
            if got or d.counts[e['fields']['expect']] < 1:
                fail(e['name'] + ': corrupt frame not rejected as ' + e['fields']['expect'])
        elif len(got) != 1 or got[0] != e['fields'] or d.buf:
            fail(e['name'] + ': decode mismatch')
    print('golden ok', len(entries))


def pings(out_bin, out_json):
    rnd = random.Random(20261014)
    stream, sent = b'', []
    for i in range(6):
        seq = (0xFFFFFFFD + i) & 0xFFFFFFFF
        t = 5_000_000_000 * (i + 1) + 777
        stream += bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 9))).replace(b'M', b'm')
        if i == 3:
            bad = bytearray(encode_ping(0, 99, 1, 0))
            bad[10] ^= 4
            stream += bytes(bad)
        stream += encode_ping(0, seq, t, 0)
        sent.append({'seq': seq, 't_cmu_ns': t})
    open(out_bin, 'wb').write(stream)
    json.dump(sent, open(out_json, 'w'))


def recompute(samples):
    """Spec integration over the raw samples, cut into windows of >= 20 ms of sensor time."""
    windows, prev, start, acc, n, down, bias, bias_t = [], None, None, 0.0, 0, None, None, None
    for row in samples:
        kind, t = row[0], int(row[1])
        x, y, z = (float(v) for v in row[2:5])
        if kind == 'g':
            g = math.sqrt(x * x + y * y + z * z)
            down = (-x / g, -y / g, -z / g)  # TYPE_GRAVITY points up; the spec axis is downward
            continue
        if kind == 'b':
            bias, bias_t = (x, y, z), t
            continue
        if prev is None:
            prev = start = t
            continue
        acc += (x * down[0] + y * down[1] + z * down[2]) * (t - prev) * 1e-9
        n += 1
        prev = t
        if t - start >= 20_000_000:
            b = round(sum(a * c for a, c in zip(bias, down)) * 1e6)
            windows.append({'t_end_ns': t, 'window_us': (t - start) // 1000, 'dtheta': acc, 'bias': b})
            start, acc, n = t, 0.0, 0
    return windows


def check(stream_bin, pings_json, samples_csv):
    d = Decoder()
    data = open(stream_bin, 'rb').read()
    frames = []
    for i in range(0, len(data), 7):  # arbitrary chunking exercises resynchronisation buffers
        frames += d.feed(data[i:i + 7])
    if d.buf or d.counts['crc_error'] or d.counts['unknown_version'] or d.counts['skipped_bytes']:
        fail('phone stream not clean: %r' % d.counts)
    if not frames or frames[0]['magic'] != 'MP':
        fail('the phone sent DATA before the first PONG (spec: no DATA before the first PING)')
    dataf = [f for f in frames if f['magic'] == 'MX']
    pongs = [f for f in frames if f['magic'] == 'MP']
    if any(f['type'] != 1 for f in pongs):
        fail('phone sent a frame other than DATA or PONG')
    sent = json.load(open(pings_json))
    if [(p['seq'], p['t_cmu_ns']) for p in pongs] != [(p['seq'], p['t_cmu_ns']) for p in sent]:
        fail('pongs do not echo the valid pings in order')
    if any(p['t_phone_ns'] <= 0 for p in pongs) or sorted(p['t_phone_ns'] for p in pongs) != [p['t_phone_ns'] for p in pongs]:
        fail('pong t_phone_ns not positive and monotonic')
    for a, b in zip(dataf, dataf[1:]):
        if b['seq'] != (a['seq'] + 1) & 0xFFFFFFFF:
            fail('seq not contiguous at %d' % a['seq'])
    if not any(a['seq'] > b['seq'] for a, b in zip(dataf, dataf[1:])):
        fail('test stream did not wrap seq')
    want = recompute(list(csv.reader(open(samples_csv))))
    if len(want) != len(dataf):
        fail('window count %d != recomputed %d' % (len(dataf), len(want)))
    for i, (f, w) in enumerate(zip(dataf, want)):
        if f['t_end_ns'] != w['t_end_ns'] or f['window_us'] != w['window_us']:
            fail('window %d timing mismatch' % i)
        if abs(f['dtheta_urad'] - w['dtheta'] * 1e6) > 1.0:
            fail('window %d dtheta %d vs %.2f' % (i, f['dtheta_urad'], w['dtheta'] * 1e6))
        if f['flags'] & 1 and abs(f['bias_urad_s'] - w['bias']) > 1:
            fail('window %d bias mismatch' % i)
        if f['flags'] & 0xF0:
            fail('reserved flag bits set')
    if dataf[0]['flags'] & 8 == 0 or any(f['flags'] & 8 for f in dataf[1:]):
        fail('frame_reset must be set on the first frame only')
    total = sum(f['dtheta_urad'] for f in dataf) * 1e-6
    print('e2e ok frames=%d pongs=%d total_rad=%.6f' % (len(dataf), len(pongs), total))


if __name__ == '__main__':
    cmd = sys.argv[1]
    {'golden': golden, 'pings': pings, 'check': check}[cmd](*sys.argv[2:])
