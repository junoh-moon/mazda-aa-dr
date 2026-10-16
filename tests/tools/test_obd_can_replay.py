"""tools/obd_can_replay.py: raw CAN 0x079/0x078 frames -> OBD-flagged v1 bins and digests for pimu_eval.

An authored CSV with known 0x079 and 0x078 zeros checks the replay and the evaluation end to end
(validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md, section 15). The public ND log (outside the
repository, sha256 18983a366b46...) is used only when MX5DR_ND_CAN_CSV names it; otherwise that
test is skipped.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
import obd_can_replay  # noqa: E402
import make_obd_vectors  # noqa: E402  (path set up by obd_can_replay)

STOPS = ((0.0, 20.0, 2046), (50.0, 80.0, 2049), (100.0, 120.0, 2045))   # logger s, 0x078 raw at rest
ZERO_PATTERN = (2047, 2048, 2048, 2047, 2048)                            # 0x079 at rest: mean 2047.6
DROP_079 = (60.0, 60.7)          # a 0.7 s hole inside the second stop: frame_reset after it
SKIP_079_AT = 85.001             # one lost frame while driving: obd_gap


def frame078(raw, counter):
    return bytes([0, (raw >> 11) & 1, (raw >> 3) & 0xFF, (raw & 7) << 5, 0, 0, counter << 4, 0])


def write_csv(path):
    lines = ['time,addr,bus,data']
    k = 0
    t = 0.001
    while t < 120.0:
        still = any(a <= t < b for a, b, _ in STOPS)
        z078 = next((z for a, b, z in STOPS if a <= t < b), 2048)
        raw79 = ZERO_PATTERN[k % 5]
        raw78 = z078
        if 30.0 <= t < 40.0:                 # a left turn: counts rise (about 10 deg/s)
            raw79 += 265
            raw78 += 265
        counter = k & 15
        drop = DROP_079[0] <= t < DROP_079[1] or abs(t - SKIP_079_AT) < 0.005
        if not drop:
            lines.append('%.6f,121,0,%s' % (t, make_obd_vectors.frame(raw79, counter).hex()))
        lines.append('%.6f,120,0,%s' % (t + 0.00001, frame078(raw78, counter).hex()))
        for half in (0.0, 0.01):
            w = 10000 if still else 15000
            lines.append('%.6f,533,0,%s' % (t + half + 0.00002, (w.to_bytes(2, 'big') * 4).hex()))
            lines.append('%.6f,514,0,%s' % (t + half + 0.00003, (bytes(2) + (0 if still else 5000).to_bytes(2, 'big')
                                                                  + bytes(4)).hex()))
        k += 1
        t = 0.001 + k * 0.02
    path.write_text('\n'.join(lines) + '\n')


def run(*args):
    return subprocess.run([sys.executable, *map(str, args)], capture_output=True, text=True, check=True).stdout


class Replay(unittest.TestCase):
    def test_authored_log_end_to_end(self):
        with tempfile.TemporaryDirectory() as tmp:
            csv_path = Path(tmp) / 'can.csv'
            write_csv(csv_path)
            with self.assertRaises(SystemExit):              # only the public log without --any-csv
                obd_can_replay.replay(csv_path, Path(tmp) / 'x')
            info = json.loads(run(REPO / 'tools/obd_can_replay.py', '--any-csv', csv_path, Path(tmp) / 'trip'))
            self.assertFalse(info['public_log'])
            self.assertEqual(info['decode'], {'OK': info['frames_079']})
            self.assertEqual(info['v1_frames'], info['frames_079'])
            self.assertEqual(len(info['stops']), 3)
            logs = Path(tmp) / 'trip' / 'logs'
            pimu = [json.loads(x) for x in (logs / 'pimu.0.jsonl').read_text().splitlines()]
            bins = [b for r in pimu if r['kind'] == 'pimu' for b in r['d']]
            self.assertEqual(sum(b[0] for b in bins), info['frames_079'])
            self.assertTrue(all(b[4] & 16 for b in bins if b[0]))
            # The bins carry exactly what the app's integrator produced: -(raw - 2047) * 13.1723 urad per frame.
            raws = [make_obd_vectors.decode(0x079, bytes.fromhex(line.split(',')[3]))[1]
                    for line in csv_path.read_text().splitlines()[1:] if line.split(',')[1] == '121']
            exact = -sum(r - 2047 for r in raws) * 658.615 * 0.020
            self.assertLessEqual(abs(sum(b[1] for b in bins) - exact), 1.0 * 3)   # carry reset at each frame_reset
            report = json.loads(run(REPO / 'tools/pimu_eval.py', '--json', Path(tmp) / 'trip'))
            tot = report['obd_totals']
            self.assertEqual(tot['frames'], info['frames_079'])
            self.assertEqual(tot['reset_bins'], 2)            # first frame and after the 0.7 s hole
            self.assertEqual(tot['gap_bins'], 2)              # the hole and the skipped counter
            self.assertLessEqual(tot['count_sum_residual_max'], 0.08)
            (boot,) = report['boots']
            stops = boot['obd_stops']
            self.assertEqual([round(s['zero_078']) for s in stops], [2046, 2049, 2045])
            for s in stops:
                self.assertAlmostEqual(s['zero_obd'], 2047.6, delta=0.03)
            z = report['obd_zero']
            self.assertEqual(z['stops'], 3)
            self.assertLess(z['obd']['range_counts'], 0.05)
            self.assertAlmostEqual(z['car_078']['range_counts'], 4.0, delta=0.01)
            # the left turn (about -100 deg) is seen by both sources in the first segment
            seg = boot['obd_segments'][0]
            self.assertLess(seg['obd_deg'], -90)
            self.assertAlmostEqual(seg['obd_deg'], seg['car_2048_deg'], delta=2.0)

    @unittest.skipUnless(os.environ.get('MX5DR_ND_CAN_CSV'), 'public ND CAN log not provided (MX5DR_ND_CAN_CSV)')
    def test_public_nd_log(self):
        """Real 0x079 bytes: 0x079 stop zeros versus 0x078 on the same stops (YAW_DECODE_2026-10-10:
        stop-mean range 0.189 counts versus 3.343 over six frame-index stops)."""
        with tempfile.TemporaryDirectory() as tmp:
            info = obd_can_replay.replay(os.environ['MX5DR_ND_CAN_CSV'], tmp)
            self.assertTrue(info['public_log'])
            self.assertEqual(info['frames_079'], 14411)
            self.assertEqual(info['v1_frames'], 14411)
            report = json.loads(run(REPO / 'tools/pimu_eval.py', '--json', tmp))
            z = report['obd_zero']
            self.assertGreaterEqual(z['stops'], 4)
            self.assertLess(z['obd']['range_counts'], 0.3)
            self.assertGreater(z['car_078']['range_counts'], 3.0)
            self.assertGreater(z['range_ratio_078_over_obd'], 10)
            self.assertLessEqual(report['obd_totals']['count_sum_residual_max'], 0.08)


if __name__ == '__main__':
    unittest.main()
