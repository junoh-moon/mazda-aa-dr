"""Synthetic PC runner checks against production navigation, no vehicle claims."""
import json
from pathlib import Path
import subprocess
import sys
import unittest

RUNNER = Path(sys.argv.pop(1)).resolve()
BASE = 1_000_000_000


def inputs(end_ms, stationary_ms=0, scale=1.0, yaw=2047, gps_until=None,
           gap_at=None, shifted_after=None, stop_at=None):
    records = []
    seq = 0
    for ms in range(0, end_ms + 1, 100):
        ns = BASE + ms * 1_000_000
        moving = ms >= stationary_ms and (stop_at is None or ms < stop_at)
        if gps_until is None or ms <= gps_until:
            mode = 0 if gap_at is not None and ms >= gap_at else 1
            north = max(0, min(ms, stop_at or ms) - stationary_ms) * 0.01 * scale
            if shifted_after is not None and ms >= shifted_after:
                north += 1.0
            records.append(f'P {ns} {mode} {1700000000 + ms // 1000} '
                           f'{north / 111320:.17g} 135 0 {36 * scale if moving else 0}\n')
        wheel = 13600 if moving else 10000
        for kind in (1, 3, 2):
            seq += 1
            raw = wheel if kind == 1 else yaw if kind == 2 else 0
            count = 1 if kind == 2 else 0
            records.append(f'M {kind} 1 {seq} {ns} 0 {raw} '
                           f'{wheel if kind == 1 else 0} {wheel if kind == 1 else 0} '
                           f'{wheel if kind == 1 else 0} {count} 0\n')
    return ''.join(records)


def run(data, valid=True):
    result = subprocess.run([str(RUNNER)], input=data, text=True, capture_output=True)
    if not valid:
        return result
    if result.returncode:
        raise AssertionError(result.stderr)
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    assert rows[-1]['kind'] == 'final'
    return rows


def summaries(rows):
    return {r['variant']: r for r in rows if r['kind'] == 'summary'}


class ReplayCpp(unittest.TestCase):
    def test_straight_completed_holdout_and_exact_receipt_alignment(self):
        rows = run(inputs(12_300))
        compared = [r for r in rows if r.get('event') == 'COMPARED']
        self.assertGreater(len(compared), 100)
        self.assertEqual(sum(r.get('event') == 'END' for r in rows), 2)
        self.assertFalse(rows[-1]['inconclusive'])
        for r in compared:
            self.assertEqual(r['frontier_ns'], r['reference_ns'])
            self.assertLess(r['position_error_m'], 1.0)  # core uses its own ellipsoid
            self.assertEqual(r['domain'], 'model')
            self.assertFalse(r['assist_ready'])
            self.assertTrue(r['model_valid'])
        self.assertNotIn('lat', rows[0])

    def test_main_gap_distance_and_stop(self):
        rows = run(inputs(4_000, gps_until=2_000, gap_at=2_000))
        for r in summaries(rows).values():
            self.assertEqual(r['state'], 2)  # ACTIVE
            self.assertTrue(r['model_valid'])
            self.assertFalse(r['valid'])
            self.assertGreater(r['distance_m'], 19)
            self.assertGreater(r['intervals'], 10)
        stopped = run(inputs(5_000, gps_until=2_000, gap_at=2_000, stop_at=3_000))
        for r in summaries(stopped).values():
            self.assertLess(r['distance_m'], summaries(rows)[r['variant']]['distance_m'] - 5)

    def test_nonzero_learning_fixed_baseline_and_second_holdout(self):
        rows = run(inputs(58_000, stationary_ms=4_000, scale=1.03, yaw=2055))
        s = summaries(rows)
        self.assertEqual(s['fixed']['yaw_zero'], 2047)
        self.assertEqual(s['fixed']['calibration_version'], 0)
        self.assertEqual(s['fixed']['wheel_scale'], 1)
        self.assertEqual(s['fixed']['wheel_scale_version'], 0)
        self.assertEqual(s['adaptive']['yaw_zero'], 2055)
        self.assertGreater(s['adaptive']['calibration_version'], 0)
        self.assertAlmostEqual(s['adaptive']['wheel_scale'], 1.03, places=8)
        self.assertGreater(s['adaptive']['wheel_scale_version'], 0)
        completed = [r for r in rows if r.get('event') == 'END']
        self.assertGreaterEqual(len(completed), 4)
        self.assertTrue(any(r['variant'] == 'adaptive' and r['wheel_scale_version'] > 0
                            for r in completed))
        # This proves the two models actually change prediction, not accuracy.
        compared = [r for r in rows if r.get('event') == 'COMPARED']
        keyed = {(r['variant'], r['anchor_ns'], r['reference_ns']): r for r in compared}
        deltas = [abs(r['position_error_m'] - keyed[('fixed', r['anchor_ns'], r['reference_ns'])]['position_error_m'])
                  for r in compared if r['variant'] == 'adaptive' and
                  ('fixed', r['anchor_ns'], r['reference_ns']) in keyed]
        self.assertGreater(max(deltas), 1)
        self.assertFalse(rows[-1]['inconclusive'])

    def test_same_gate_without_learning_and_null_rejection(self):
        data = inputs(2_100)
        # Opposite GPS heading at every fix is incompatible with north travel.
        data = '\n'.join(line.rsplit(' 0 36', 1)[0] + ' 180 36'
                         if line.startswith('P ') else line for line in data.splitlines()) + '\n'
        rows = run(data)
        self.assertFalse(any(r.get('event') == 'BEGIN' for r in rows))
        for r in summaries(rows).values():
            self.assertNotEqual(r['state'], 1)  # never READY
            self.assertEqual(r['gps_anchor_gate'], summaries(rows)['fixed']['gps_anchor_gate'])
        rows = run('P 1000000000 1 1700000000 null 135 0 36\n')
        self.assertTrue(rows[-1]['inconclusive'])
        self.assertTrue(any(r.get('reason') == 'bad_gps' for r in rows))

    def test_holdout_reference_isolation(self):
        a = run(inputs(11_300))
        b = run(inputs(11_300, shifted_after=1_100))
        # During RUNNING a 1m north reference shift changes only comparison.
        ca = [r for r in a if r.get('event') == 'COMPARED']
        cb = [r for r in b if r.get('event') == 'COMPARED']
        self.assertEqual(len(ca), len(cb))
        for x, y in zip(ca, cb):
            self.assertEqual(x['frontier_ns'], y['frontier_ns'])
            self.assertEqual(x['yaw_zero'], y['yaw_zero'])
            self.assertAlmostEqual(x['position_error_m'] + y['position_error_m'], 1, places=6)

    def test_group_watermark_tail_and_uint64(self):
        rows = run(inputs(200))
        self.assertEqual(rows[-1]['groups'], 3)
        self.assertEqual(rows[-1]['watermark_ns'], BASE + 100_000_000)
        self.assertFalse(rows[-1]['inconclusive'])
        large = 2**53 + 123
        rows = run(f'P {large} 0 0 null null null null\n')
        self.assertEqual(rows[-1]['last_received_ns'], large)
        self.assertEqual(rows[-1]['watermark_ns'], large - 100_000_000)

    def test_fault_is_reported_and_no_silent_success(self):
        data = inputs(300)
        last = data.splitlines()[-1].split()
        last[2] = '2'  # source epoch change
        rows = run(data + ' '.join(last) + '\n')
        self.assertTrue(rows[-1]['inconclusive'])
        self.assertGreater(rows[-1]['faults'], 0)

    def test_protocol_rejects_malformed_bounds_nonfinite_extra_and_regression(self):
        good = 'P 1000000000 1 1700000000 0 135 0 36'
        cases = ['', 'x', good + ' extra', good.replace('1000000000', '-1', 1),
                 good.replace('1000000000', str(2**64), 1),
                 good.replace(' 0 135', ' nan 135'), good.replace(' 0 135', ' 0x1p1 135'),
                 good.replace(' 0 135', ' 1e999 135'), good + '\0', 'x' * 1025,
                 'M 1 1 1 1000000000 0 65536 0 0 0 0 0',
                 'M 1 1 1 1000000000 9223372036854775808 0 0 0 0 0 0',
                 good + '\n' + good.replace('1000000000', '999999999', 1)]
        for case in cases:
            with self.subTest(case=case[:30]):
                result = run(case + '\n', valid=False)
                self.assertEqual(result.returncode, 2)
                self.assertNotIn('135', result.stderr)
        empty = run('')
        self.assertTrue(empty[-1]['inconclusive'])


if __name__ == '__main__':
    unittest.main()
