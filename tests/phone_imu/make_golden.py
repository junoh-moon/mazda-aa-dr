#!/usr/bin/env python3
"""The single canonical golden-vector generator of PHONE IMU CHANNEL v1
(validation/PHONE_IMU_CHANNEL_2026-10-14.md): writes tests/phone_imu/golden_frames.json,
read by the CMU parser tests (tests/collector/test_phone_imu.py) and the phone app tests
(phone/imu-sender/test.sh: JVM tests and the independent Python decoder).

Deterministic (no randomness). Its own encoder (struct + zlib.crc32), independent of
the C++ (CMU) and Java (phone) encoders under test.
Schema: a JSON list of {"name", "kind", "hex", "fields"}; kind is data, ping, pong or
corrupt. Valid entries: fields are the decoded wire fields (magic, version, ..., crc32)
and encoding them must reproduce hex. Corrupt entries: fields = {"expect": <counter>};
a decoder fed hex alone must output no frame and count it as crc_error,
unknown_version or bad_type.
Usage: make_golden.py [--check | OUTPUT]
"""
import json
import pathlib
import struct
import sys
import zlib

OUT = pathlib.Path(__file__).resolve().with_name('golden_frames.json')


def encode_data(flags, seq, t_end_ns, dtheta_urad, window_us, bias_urad_s, version=1):
    body = struct.pack('<2sBBIqiIi', b'MX', version, flags, seq & 0xFFFFFFFF, t_end_ns,
                       dtheta_urad, window_us & 0xFFFFFFFF, bias_urad_s)
    return body + struct.pack('<I', zlib.crc32(body) & 0xFFFFFFFF)


def encode_sync(type_, seq, t_cmu_ns, t_phone_ns, version=1):
    body = struct.pack('<2sBBIqq', b'MP', version, type_, seq & 0xFFFFFFFF, t_cmu_ns, t_phone_ns)
    return body + struct.pack('<I', zlib.crc32(body) & 0xFFFFFFFF)


def fields_of(frame):
    if frame[:2] == b'MX':
        v, fl, seq, t, d, w, b, c = struct.unpack('<BBIqiIiI', frame[2:32])
        return {'magic': 'MX', 'version': v, 'flags': fl, 'seq': seq, 't_end_ns': t, 'dtheta_urad': d,
                'window_us': w, 'bias_urad_s': b, 'crc32': c}
    v, ty, seq, tc, tp, c = struct.unpack('<BBIqqI', frame[2:28])
    return {'magic': 'MP', 'version': v, 'type': ty, 'seq': seq, 't_cmu_ns': tc, 't_phone_ns': tp, 'crc32': c}


T0 = 1_234_567_890_123  # phone elapsedRealtimeNanos-like value
DATA = [
    # phone implementer's cases
    ('basic_gravity_bias', 0x05, 0, T0, 1234, 20000, 15),
    ('negative_dtheta', 0x05, 1, T0 + 20_000_000, -98765, 20000, -15),
    ('frame_reset_first', 0x0D, 7, T0 + 5_000_000, 5000, 19980, 0),
    ('stationary_hint', 0x07, 8, T0 + 25_000_000, 0, 20010, 3),
    ('no_flags_gravity_invalid', 0x00, 9, T0 + 45_000_000, 0, 20000, 0),
    ('seq_max_before_wrap', 0x05, 0xFFFFFFFF, T0 + 65_000_000, 345, 20000, 12),
    ('seq_wrapped_to_zero', 0x05, 0, T0 + 85_000_000, -345, 20000, 12),
    ('all_flags_negative_bias', 0x0F, 0x7FFFFFFF, T0 + 105_000_000, 1, 20000, -12345),
    ('dtheta_int32_min', 0x04, 0x80000000, T0 + 125_000_000, -2**31, 25000, 2**31 - 1),
    ('dtheta_int32_max', 0x04, 0x80000001, T0 + 145_000_000, 2**31 - 1, 20000, -2**31),
    ('window_u32_max_large_t', 0x0C, 42, 2**62 + 17, -1, 0xFFFFFFFF, 0),
    ('slow_turn_tilted', 0x05, 43, T0 + 165_000_000, 87266, 20000, -523),
    # CMU implementer's cases
    ('cmu_first_after_connect', 0b1101, 0, 86400123456789, 1745, 20000, 12),
    ('cmu_negative_small', 0b0101, 2, 86400163456789, -3490, 20000, 12),
    ('cmu_bias_invalid_zero', 0b0100, 4, 86400203456789, 52360, 19876, 0),
    ('cmu_t_end_max', 0b0101, 4294967294, 9223372036854775807, -1, 20000, -2**31),
    ('cmu_seq_last_before_wrap', 0b0101, 4294967295, 1, -20943, 20001, 3),
    ('cmu_seq_wrapped', 0b0101, 0, 2, 20943, 19999, 3),
    ('cmu_t_end_zero', 0b1111, 17, 0, 1, 40000, -1),
    ('cmu_negative_phone_time', 0b0100, 18, -1, -1745329, 1000000, 0),
]
SYNC = [
    ('ping_basic', 0, 1, 987_654_321_000, 0),
    ('pong_basic', 1, 1, 987_654_321_000, T0 + 1_000_000),
    ('ping_seq_max', 0, 0xFFFFFFFF, 2**40, 0),
    ('pong_seq_max', 1, 0xFFFFFFFF, 2**40, 2**62),
    ('ping_zero', 0, 0, 0, 0),
    ('cmu_ping', 0, 1, 123456789000, 0),
    ('cmu_pong', 1, 1, 123456789000, 86400987654321),
    ('cmu_ping_t_max', 0, 4294967295, 9223372036854775807, 0),
    ('cmu_pong_negative_phone', 1, 4294967295, 9223372036854775807, -5),
]


def entries():
    out = []
    for name, *a in DATA:
        h = encode_data(*a)
        out.append({'name': name, 'kind': 'data', 'hex': h.hex(), 'fields': fields_of(h)})
    for name, *a in SYNC:
        h = encode_sync(*a)
        out.append({'name': name, 'kind': 'ping' if a[0] == 0 else 'pong', 'hex': h.hex(), 'fields': fields_of(h)})

    def corrupt(name, raw, expect):
        out.append({'name': name, 'kind': 'corrupt', 'hex': bytes(raw).hex(), 'fields': {'expect': expect}})
    flipped = bytearray(encode_data(0x05, 3, T0, 1000, 20000, 5))
    flipped[16] ^= 0x01                      # one bit of dtheta
    corrupt('corrupt_data_bitflip', flipped, 'crc_error')
    bad_crc = bytearray(encode_sync(0, 5, 1_000_000, 0))
    bad_crc[-1] ^= 0xFF
    corrupt('corrupt_ping_crc', bad_crc, 'crc_error')
    corrupt('corrupt_data_version2', encode_data(0x05, 4, T0, 1000, 20000, 5, version=2), 'unknown_version')
    corrupt('corrupt_ping_type2', encode_sync(2, 6, 1_000_000, 0), 'bad_type')
    crc_bit = bytearray(encode_data(0b0101, 7, 86400263456789, 1000, 20000, 12))
    crc_bit[28] ^= 0x01                      # one bit of the CRC itself
    corrupt('cmu_corrupt_data_crc_bit', crc_bit, 'crc_error')
    pong_bit = bytearray(encode_sync(1, 2, 223456789000, 86400987654999))
    pong_bit[12] ^= 0x80                     # t_cmu_ns of a pong
    corrupt('cmu_corrupt_pong_payload', pong_bit, 'crc_error')
    corrupt('cmu_corrupt_ping_version2', encode_sync(0, 9, 5, 0, version=2), 'unknown_version')
    return out


def main():
    text = json.dumps(entries(), indent=1) + '\n'
    if '--check' in sys.argv:
        if OUT.read_text() != text:
            sys.exit('tests/phone_imu/golden_frames.json differs from make_golden.py output')
        return
    (pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else OUT).write_text(text)


if __name__ == '__main__':
    main()
