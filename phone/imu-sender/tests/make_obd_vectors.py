#!/usr/bin/env python3
"""Decode vectors of the stage-2 OBD yaw source (CAN 0x079 through an ELM327 monitor),
validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md. Writes phone/imu-sender/tests/obd_079_vectors.json,
read by the JVM tests (CoreTests.obdVectors); this script is an independent Python decoder.

Where the numbers come from (the 'can' vectors are CONSTRUCTED; the public ND log stays outside
the repository, validation/ND_CAN_LOG_2026-10-03.md; only the five 'observed' pairs below are
captured bytes copied from it):
  layout  12-bit yaw = ((b3&3)<<10)|(b4<<2)|(b5>>6) (DBC Motorola start 25, length 12), b3>>2 == 0,
          counter = b6 & 0x0F (equal to 0x078 b6>>4 in all 14,411 pairs), Qf = b0>>6,
          lateral acceleration 13 bits ((b0&0x3F)<<7)|(b1>>1) with 0 G near 3988
          (validation/YAW_DECODE_2026-10-10 in the owner's tree, VIP_0X079_GHIDRA_2026-10-12,
          ND_CAN_LOG_2026-10-03, VIM_CHANNEL_CAPTURE_2026-10-10)
  values  on the integer lattice round(2047.00190 + 1.52751423*n) that reproduces every observed
          0x078 and 0x079 value of the public log; 1737 and 2925 are the observed 0x078 extremes
          (the same lattice); the six stop means of 0x079 were 2047.50..2047.69
  units   the CMU product constant for 0x078: rad/s = -(raw-2047)*0.000658615 (src/navigation/
          pipeline.cpp), clockwise positive; the count rises in LEFT turns (public log)
  DBC     berumiya MX5ND_6thGenMazda_HSCAN.dbc: YawRate_Corr 31|16@0+ (0.223437,-114.4) deg/s,
          i.e. bytes 3..4 only (cross-check field, its own sign: positive = count rising = left)
Bytes the sources do not define (b2, b6 high nibble, b7, b5 low six bits) are filled with fixed
non-zero patterns so that masking is tested; they carry no claimed meaning.
OBSERVED pairs: five real 0x079 frames with their 0x078 partner (k-th 0x079 with the k-th 0x078 of
the public log, sha256 18983a366b46d8bea62ea426652798ca24cfef23f932e3751442485938aab54f, CSV
columns time,addr,bus,data; k and the logger time are given). They show that the 0x079 layout
applied to CAPTURED bytes gives values on the same integer lattice as the firmware-derived 0x078
layout, with a lattice difference in 0..3 and paired counters. They do NOT prove the scale, the
sign, the meaning of the other bytes, or that the owner's ND2 sends the same.
Usage: make_obd_vectors.py [--check | OUTPUT]
       make_obd_vectors.py observed RAW_CAN.csv   re-derive the observed pairs from the public log
       make_obd_vectors.py stream FRAMES.bin   check the v1 frames the JVM ObdLink produced from
                                               this file's lines (ObdTests.linkEndToEnd)
"""
import json
import math
import pathlib
import sys

OUT = pathlib.Path(__file__).resolve().with_name('obd_079_vectors.json')
URAD_S_PER_COUNT = 658.615
ZERO = 2047
PERIOD_S = 0.020


def lattice(n):
    return math.floor(2047.00190 + 1.52751423 * n + 0.5)


def frame(raw12, counter, qf=3, lat13=3988, b2=0x5A, b5_low=0x15, b6_high=0x3, b7=0xA5, b3_high=0):
    b0 = (qf << 6) | (lat13 >> 7)
    b1 = (lat13 & 0x7F) << 1
    b3 = (b3_high << 2) | ((raw12 >> 10) & 3)
    b4 = (raw12 >> 2) & 0xFF
    b5 = ((raw12 & 3) << 6) | b5_low
    b6 = (b6_high << 4) | counter
    return bytes([b0, b1, b2, b3, b4, b5, b6, b7])


def decode(can_id, d):
    """Independent decoder: returns (status, raw12, counter, qf)."""
    if can_id != 0x079:
        return 'WRONG_ID', -1, -1, -1
    if len(d) < 6:
        return 'SHORT', -1, -1, -1
    counter = d[6] & 0x0F if len(d) >= 7 else -1
    qf = d[0] >> 6
    if d[3] >> 2:
        return 'RESERVED_BITS', -1, counter, qf
    raw = ((d[3] & 3) << 10) | (d[4] << 2) | (d[5] >> 6)
    if raw in (0, 0xFFF):
        return 'SENTINEL', raw, counter, qf
    return 'OK', raw, counter, qf


# (k, logger time s, 0x078 bytes, 0x079 bytes, why chosen): k-th frame of each id in log order.
OBSERVED = [
    (100, 6.081432440667413, 'DF58FFA0ABC3C0C9', 'DF4C0001FFDD8C62', 'early stop, lattice difference 1'),
    (13499, 278.0864685792476, 'E0416DA098C340FB', 'E0940002DBDE844D', 'largest 0x079 value (left turn)'),
    (9752, 199.63661909732036, 'DEEAD92098C3504F', 'DDEC0001B25E054F', 'smallest 0x079 value (right turn)'),
    (8233, 168.59392795327585, 'DF80FFA0A7C310C4', 'DF4E0002005E0167', 'lattice difference 2'),
    (6264, 129.04312583559658, 'E016FFA099C37080', 'DF3A0002009E076A', 'lattice difference 3'),
]
LOG_SHA256 = '18983a366b46d8bea62ea426652798ca24cfef23f932e3751442485938aab54f'


def raw078(d):
    """Firmware 0x078 yaw layout (VIP map; ND_CAN_LOG_2026-10-03)."""
    return ((d[1] & 1) << 11) | (d[2] << 3) | (d[3] >> 5)


def lattice_index(raw):
    n = round((raw - 2047.00190) / 1.52751423)
    if lattice(n) != raw:
        raise SystemExit('value %d is not on the lattice' % raw)
    return n


def observed_vectors():
    out = []
    for k, t, h78, h79, why in OBSERVED:
        d78, d79 = bytes.fromhex(h78), bytes.fromhex(h79)
        status, raw, counter, qf = decode(0x079, d79)
        if status != 'OK' or counter != d78[6] >> 4:
            raise SystemExit('observed pair %d: status %s counter %d vs %d' % (k, status, counter, d78[6] >> 4))
        r78 = raw078(d78)
        diff = lattice_index(raw) - lattice_index(r78)
        if not 0 <= diff <= 3:
            raise SystemExit('observed pair %d: lattice difference %d' % (k, diff))
        out.append({'name': 'observed_k%d' % k, 'why': why, 'k': k, 'logger_time_s': t,
                    'data078': h78, 'data079': h79, 'raw078': r78, 'raw079': raw, 'counter': counter, 'qf': qf,
                    'lattice_diff': diff, 'cw_dps': round(cw_dps(raw), 9)})
    return out


def check_observed(path):
    """Re-derives OBSERVED from the public log (outside the repository)."""
    import csv
    import hashlib
    data = pathlib.Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != LOG_SHA256:
        sys.exit('not the public ND log (sha256 differs)')
    f78, f79 = [], []
    for row in csv.reader(data.decode('ascii').splitlines()[1:]):
        try:
            t, addr, d = float(row[0]), int(row[1]), row[3].upper()
        except (ValueError, IndexError):
            continue
        if addr == 0x078:
            f78.append(d)
        elif addr == 0x079:
            f79.append((t, d))
    for k, t, h78, h79, _ in OBSERVED:
        if f78[k] != h78 or f79[k][1] != h79 or abs(f79[k][0] - t) > 1e-9:
            sys.exit('observed pair %d differs from the log' % k)
    print('observed pairs ok', len(OBSERVED), 'of', len(f79))


def cw_dps(raw):
    return -(raw - ZERO) * URAD_S_PER_COUNT * 1e-6 * 180 / math.pi


def elm_line(can_id, d, spaces=False, dlc=False, lower=False):
    parts = ['%03X' % can_id] + (['%d' % len(d)] if dlc else []) + ['%02X' % x for x in d]
    s = (' ' if spaces else '').join(parts)
    return s.lower() if lower else s


def can_vectors():
    out = []
    cases = [
        ('centre_n0', lattice(0), 0, {}),
        ('lattice_n1_left', lattice(1), 1, {}),
        ('lattice_n-1_right', lattice(-1), 2, {}),
        ('lattice_n2_left', lattice(2), 3, {}),
        ('lattice_n-2_right', lattice(-2), 4, {}),
        ('observed_min_1737_right_turn', 1737, 5, {}),
        ('observed_max_2925_left_turn', 2925, 6, {}),
        ('counter_15_wrap_before', lattice(0), 15, {}),
        ('qf_not_3', lattice(1), 0, {'qf': 1}),
        ('all_low_bits_set', 0xFFE, 7, {'b5_low': 0x3F}),
        ('one_count_above_sentinel', 1, 8, {}),
    ]
    for name, raw, ctr, kw in cases:
        d = frame(raw, ctr, **kw)
        out.append((name, 0x079, d, {}))
    out.append(('reserved_bits_set', 0x079, frame(lattice(0), 9, b3_high=1), {}))
    out.append(('sentinel_zero', 0x079, frame(0, 10), {}))
    out.append(('sentinel_fff', 0x079, frame(0xFFF, 11), {}))
    out.append(('wrong_id_078', 0x078, frame(lattice(0), 12), {}))
    out.append(('short_5_bytes', 0x079, frame(lattice(0), 13)[:5], {}))
    out.append(('six_bytes_no_counter', 0x079, frame(lattice(3), 0)[:6], {}))
    vec = []
    fmts = [dict(), dict(spaces=True), dict(spaces=True, dlc=True), dict(lower=True), dict(dlc=True)]
    for i, (name, can_id, d, _) in enumerate(out):
        status, raw, ctr, qf = decode(can_id, d)
        e = {'name': name, 'line': elm_line(can_id, d, **fmts[i % len(fmts)]), 'id': can_id, 'data': d.hex(),
             'status': status, 'raw12': raw, 'counter': ctr, 'qf': qf}
        if status == 'OK':
            e['cw_dps'] = round(cw_dps(raw), 9)
            e['dtheta_urad_20ms'] = round(-(raw - ZERO) * URAD_S_PER_COUNT * PERIOD_S, 6)
        if len(d) >= 5:
            e['dbc_yawrate_corr_dps'] = round(((d[3] << 8) | d[4]) * 0.223437 - 114.4, 6)
        vec.append(e)
    return vec


def line_vectors():
    """Classification of adapter lines (kind names of ElmLineParser.Kind)."""
    return [
        {'text': 'OK', 'kind': 'OK'},
        {'text': '?', 'kind': 'UNKNOWN_COMMAND'},
        {'text': 'BUFFER FULL', 'kind': 'BUFFER_FULL'},
        {'text': 'STOPPED', 'kind': 'STOPPED'},
        {'text': 'CAN ERROR', 'kind': 'ERROR'},
        {'text': '079DF285A01FFD5A3A5<RX ERROR', 'kind': 'ERROR'},
        {'text': 'ELM327 v1.5', 'kind': 'TEXT'},
        {'text': 'NO DATA', 'kind': 'TEXT'},
        {'text': 'SEARCHING...', 'kind': 'TEXT'},
        {'text': 'ATE0', 'kind': 'TEXT'},
        {'text': 'LV RESET', 'kind': 'TEXT'},
        {'text': '07', 'kind': 'MALFORMED'},
        {'text': '079DF2', 'kind': 'MALFORMED'},
        {'text': '8001122', 'kind': 'MALFORMED'},
        {'text': '0799DF285A01FFD5A3A5', 'kind': 'MALFORMED'},
        {'text': '0798DF28', 'kind': 'MALFORMED'},
        {'text': '079 2 DF 28', 'kind': 'CAN'},
        {'text': '079', 'kind': 'CAN'},
        {'text': '079 DF 28', 'kind': 'CAN'},
        {'text': '079DF285A01FFD5A3A5AA', 'kind': 'MALFORMED'},
    ]


def stream_vectors(can):
    """A monitor sequence: OK frames in order with counters, used for the dtheta running sum."""
    seq = [e for e in can if e['status'] == 'OK' and e['counter'] >= 0][:8]
    total = sum(e['dtheta_urad_20ms'] for e in seq)
    return {'names': [e['name'] for e in seq], 'sum_dtheta_urad': round(total, 6)}


def build():
    can = can_vectors()
    return {'schema': 'obd_079_vectors v1', 'source': __doc__.split('Usage:')[0].strip(),
            'zero_nominal': ZERO, 'urad_s_per_count': URAD_S_PER_COUNT, 'period_us': 20000,
            'can': can, 'lines': line_vectors(), 'stream': stream_vectors(can), 'observed': observed_vectors()}


def check_stream(path):
    """Independent check of the Java OBD windows: v1 decoder from imu_wire, sums from this decoder."""
    from imu_wire import Decoder
    frames = Decoder().feed(pathlib.Path(path).read_bytes())
    can = build()['can']
    ok = [e for e in can if e['status'] == 'OK' and e['id'] == 0x079]
    want_n = len(ok) + 30
    exact = sum(e['dtheta_urad_20ms'] for e in ok)   # the 30 trailing frames are at the zero
    errors = []
    if len(frames) != want_n:
        errors.append('frames %d want %d' % (len(frames), want_n))
    for f in frames:
        if f['flags'] & 0x10 == 0 or f['flags'] & 0x07 or f['window_us'] != 20000 or f['bias_urad_s'] != 0:
            errors.append('bad OBD frame %r' % f)
    resets = sum(1 for f in frames if f['flags'] & 0x08)
    if resets != 2:
        errors.append('frame_reset %d want 2' % resets)
    got = sum(f['dtheta_urad'] for f in frames)
    if abs(got - exact) > 0.5:
        errors.append('dtheta sum %d vs exact %.4f' % (got, exact))
    for i, e in enumerate(ok):
        step = round(e['dtheta_urad_20ms'])
        if abs(frames[i]['dtheta_urad'] - step) > 1:
            errors.append('%s: dtheta %d vs %.4f' % (e['name'], frames[i]['dtheta_urad'], e['dtheta_urad_20ms']))
    if errors:
        sys.exit('FAIL: ' + '; '.join(errors[:5]))
    print('obd stream ok', len(frames), 'frames, dtheta sum', got, 'exact %.4f' % exact)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == 'stream':
        check_stream(sys.argv[2])
        return
    if len(sys.argv) == 3 and sys.argv[1] == 'observed':
        check_observed(sys.argv[2])
        return
    text = json.dumps(build(), indent=1, ensure_ascii=False) + '\n'
    if '--check' in sys.argv:
        if OUT.read_text() != text:
            sys.exit('phone/imu-sender/tests/obd_079_vectors.json differs from make_obd_vectors.py output')
        print('obd vectors ok', len(build()['can']))
        return
    (pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else OUT).write_text(text)


if __name__ == '__main__':
    main()
