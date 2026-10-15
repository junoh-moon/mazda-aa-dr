"""Independent Python implementation of PHONE IMU CHANNEL v1 (encoder and resynchronising decoder).

Written from the shared spec only (not from the Java sources) so that the Java phone encoder and this
decoder cross-check each other. Little-endian; CRC-32 (zlib) over all preceding bytes.
"""
import struct
import zlib

DATA_LEN, PING_LEN, VERSION = 32, 28, 1
DATA_MAGIC, PING_MAGIC = b'MX', b'MP'


def encode_data(flags, seq, t_end_ns, dtheta_urad, window_us, bias_urad_s, version=VERSION):
    body = DATA_MAGIC + struct.pack('<BBIqiIi', version, flags, seq & 0xFFFFFFFF, t_end_ns,
                                    dtheta_urad, window_us & 0xFFFFFFFF, bias_urad_s)
    return body + struct.pack('<I', zlib.crc32(body))


def encode_ping(type_, seq, t_cmu_ns, t_phone_ns, version=VERSION):
    body = PING_MAGIC + struct.pack('<BBIqq', version, type_, seq & 0xFFFFFFFF, t_cmu_ns, t_phone_ns)
    return body + struct.pack('<I', zlib.crc32(body))


def fields_of(frame):
    """Field dict of one CRC-valid frame (no checks beyond layout)."""
    if frame[:2] == DATA_MAGIC:
        v, fl, seq, t, d, w, b, c = struct.unpack('<BBIqiIiI', frame[2:32])
        return {'magic': 'MX', 'version': v, 'flags': fl, 'seq': seq, 't_end_ns': t, 'dtheta_urad': d,
                'window_us': w, 'bias_urad_s': b, 'crc32': c}
    v, ty, seq, tc, tp, c = struct.unpack('<BBIqqI', frame[2:28])
    return {'magic': 'MP', 'version': v, 'type': ty, 'seq': seq, 't_cmu_ns': tc, 't_phone_ns': tp, 'crc32': c}


class Decoder:
    """Resynchronise on magic + CRC; bad CRC drops one byte; other versions are counted and dropped."""

    def __init__(self):
        self.buf = b''
        self.counts = {'frames': 0, 'crc_error': 0, 'unknown_version': 0, 'bad_type': 0, 'skipped_bytes': 0}

    def feed(self, data):
        self.buf += data
        out, pos, buf = [], 0, self.buf
        while len(buf) - pos >= 2:
            magic = buf[pos:pos + 2]
            if magic == DATA_MAGIC:
                need = DATA_LEN
            elif magic == PING_MAGIC:
                need = PING_LEN
            else:
                pos += 1
                self.counts['skipped_bytes'] += 1
                continue
            if len(buf) - pos < need:
                break
            frame = buf[pos:pos + need]
            if zlib.crc32(frame[:-4]) != struct.unpack('<I', frame[-4:])[0]:
                self.counts['crc_error'] += 1
                self.counts['skipped_bytes'] += 1
                pos += 1
                continue
            pos += need
            if frame[2] != VERSION:
                self.counts['unknown_version'] += 1
                continue
            f = fields_of(frame)
            if f['magic'] == 'MP' and f['type'] not in (0, 1):
                self.counts['bad_type'] += 1
                continue
            self.counts['frames'] += 1
            out.append(f)
        self.buf = buf[pos:]
        return out
