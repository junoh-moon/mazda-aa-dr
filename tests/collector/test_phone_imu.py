"""Golden vectors of PHONE IMU CHANNEL v1 against the C++ decoder/encoder.

tests/phone_imu/golden_frames.json is shared with the phone implementer (byte
identical in the end). make_golden.py is an independent encoder (struct +
zlib.crc32); build/test_phone_imu decodes with src/collector/phone_imu.h.
"""
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import unittest

REPO = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get('MX5DR_TEST_BUILD', REPO / 'build'))
TOOL = BUILD / 'test_phone_imu'
GOLDEN = REPO / 'tests/phone_imu/golden_frames.json'


def run(mode, lines):
    out = subprocess.run([str(TOOL), mode], input=''.join(l + '\n' for l in lines), text=True,
                         capture_output=True, check=True).stdout
    return [json.loads(l) for l in out.splitlines()]


class GoldenTests(unittest.TestCase):
    def setUp(self):
        self.vectors = json.loads(GOLDEN.read_text())

    def test_golden_file_matches_generator(self):
        subprocess.run([sys.executable, str(REPO / 'tests/phone_imu/make_golden.py'), '--check'], check=True)
        kinds = [v['kind'] for v in self.vectors]
        self.assertGreaterEqual(kinds.count('data'), 10)
        self.assertGreaterEqual(kinds.count('ping') + kinds.count('pong'), 4)
        self.assertGreaterEqual(kinds.count('corrupt'), 3)
        data = [v['fields'] for v in self.vectors if v['kind'] == 'data']
        self.assertTrue(any(f['dtheta_urad'] < 0 for f in data))
        seqs = [f['seq'] for f in data]
        self.assertIn(4294967295, seqs)
        self.assertEqual(seqs[seqs.index(4294967295) + 1], 0)        # wrap
        self.assertGreaterEqual(len({f['flags'] for f in data}), 4)  # flag combinations
        self.assertEqual({v['fields']['expect'] for v in self.vectors if v['kind'] == 'corrupt'},
                         {'crc_error', 'unknown_version', 'bad_type'})
        self.assertEqual(len({v['name'] for v in self.vectors}), len(self.vectors))
        for v in self.vectors:
            self.assertEqual(set(v), {'name', 'kind', 'hex', 'fields'})
            n = len(bytes.fromhex(v['hex']))
            self.assertEqual(n, 32 if bytes.fromhex(v['hex'])[:2] == b'MX' else 28)

    def test_each_frame_decodes_and_reencodes(self):
        out = run('decode-frames', [v['hex'] for v in self.vectors])
        self.assertEqual(len(out), 2 * len(self.vectors))
        for i, v in enumerate(self.vectors):
            event, counts = out[2 * i], out[2 * i + 1]['counts']
            with self.subTest(name=v['name']):
                if v['kind'] == 'corrupt':
                    self.assertEqual(event['kind'], 'none')
                    self.assertEqual(counts['frames'] + counts['syncs'], 0)
                    self.assertEqual(counts[v['fields']['expect']], 1)
                    continue
                self.assertEqual(event.pop('reencoded'), v['hex'])
                self.assertEqual(event.pop('kind'), v['kind'])
                fields = dict(v['fields'])
                self.assertEqual(fields.pop('magic'), 'MX' if v['kind'] == 'data' else 'MP')
                self.assertEqual(event, fields)
                self.assertEqual(counts['crc_error'] + counts['unknown_version'] + counts['bad_type'] + counts['skipped'], 0)

    def test_stream_resync_over_random_chunks_and_garbage(self):
        rng = random.Random(20261014)
        blob, expect = b'', []
        for v in self.vectors:
            garbage = bytes(rng.choice(b'MXP\x00\x01\xff\r\n') for _ in range(rng.randrange(0, 9)))
            blob += garbage + bytes.fromhex(v['hex'])
            if v['kind'] != 'corrupt':
                f = dict(v['fields'], kind=v['kind'])
                f.pop('magic')
                expect.append(f)
        for trial in range(20):
            cuts = sorted(rng.sample(range(1, len(blob)), 40))
            chunks = [blob[a:b] for a, b in zip([0] + cuts, cuts + [len(blob)])]
            out = run('decode-stream', [c.hex() for c in chunks])
            events, counts = out[:-1], out[-1]['counts']
            for e in events:
                e.pop('reencoded')
            # Random garbage can contain 'M','X'/'P' + version byte 1 and cost a
            # CRC check, never a frame: every golden frame must survive intact.
            self.assertEqual(events, expect, trial)
            self.assertGreaterEqual(counts['crc_error'], 4)
            self.assertGreaterEqual(counts['unknown_version'], 2)
            self.assertEqual(counts['bad_type'], 1)

    def test_unit_checks(self):
        result = subprocess.run([str(TOOL)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('PASS test_phone_imu', result.stdout)


if __name__ == '__main__':
    unittest.main()
