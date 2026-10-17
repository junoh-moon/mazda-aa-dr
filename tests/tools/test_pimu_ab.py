"""tools/pimu_ab.py on authored phone app exports (design note section 16).

Synthetic A/B rows in the exact RollingLog format of the app: a known rate
profile (parked, turns of several rates, straights), a gyro and an OBD reading
with their own zero, scale, noise and flags. The analyzer must recover the
zeros, the scale and the sign, and the pre-registered outcome of each case.
"""
import json
import math
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
import pimu_ab  # noqa: E402

T0_MS = 4302243885   # phone elapsedRealtime of the order seen in the owner's export


def profile():
    """True mean rate (deg/s) of each 10 s row: parked, turns, straights, parked, ..., parked."""
    rates = [0.0] * 6
    rnd = random.Random(5)
    for k in range(3):
        for _ in range(25):
            r = rnd.choice([0.5, 1.0, 3.0, 5.0, 9.0, 12.0, 18.0])
            rates.append(r * rnd.choice([-1, 1]))
        rates += [0.0] * 6
    return rates


def row_line(t_ms, fields):
    return '2026-10-17T19:00:00.000+0900 t=%dms ab %s\n' % (t_ms, ' '.join('%s=%s' % kv for kv in fields))


def make_log(path, g_scale=1.0, o_scale=1.0, g_zero=0.004, o_zero=0.02, g_noise=0.01, o_noise=0.01, g_sign=1,
             g_grav0_every=0, o_gap_every=0, obd_missing=(), o_zero_steps=None, g_scale_fn=None, rates=None,
             extra_lines=(), seed=1, stationary=True, outliers=0, outlier_every=37):
    rnd = random.Random(seed)
    rates = profile() if rates is None else rates
    lines = ['2026-10-17T18:54:07.979+0900 t=4302243170ms service_start why=user device=AA:BB:CC:00:00:01\n']
    lines += list(extra_lines)
    t = T0_MS
    for i, true in enumerate(rates):
        iv = 10000 + rnd.randint(0, 900)
        t += iv
        gs = g_scale_fn(true) if g_scale_fn else g_scale
        g = g_sign * true * gs + g_zero + rnd.gauss(0, g_noise)
        oz = o_zero if o_zero_steps is None else o_zero_steps(i)
        o = true * o_scale + oz + rnd.gauss(0, o_noise)
        if outliers and true != 0.0 and i % outlier_every == 11 % outlier_every:
            o += 25.0
        g_dur = iv * 1000 - rnd.randint(0, 20000)
        o_n = 500 - rnd.randint(0, 5)
        o_dur = o_n * 20000
        moving = true != 0.0
        f = [('iv_ms', iv), ('sel', 'OBD')]
        f += [('g_n', 500), ('g_dur_us', g_dur), ('g_dth_urad', int(round(math.radians(g) * g_dur))),
              ('g_grav', 0 if g_grav0_every and i % g_grav0_every == 0 else 1),
              ('g_st', 500 if (stationary and not moving) else 3)]
        if i in obd_missing:
            f.append(('miss', 'obd'))
        else:
            f += [('o_n', o_n), ('o_dur_us', o_dur), ('o_dth_urad', int(round(math.radians(o) * o_dur))),
                  ('o_gap', 1 if o_gap_every and i % o_gap_every == 0 else 0)]
        lines.append(row_line(t, f))
        if i % 7 == 3:
            lines.append('2026-10-17T19:00:00.000+0900 t=%dms obd_stats source=OBD switches=1 | x\n' % (t + 5))
    Path(path).write_text(''.join(lines))
    return rates


class PimuAbTest(unittest.TestCase):
    def run_case(self, **kw):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'mx5imu-log.txt'
            make_log(p, **kw)
            rows, bad = pimu_ab.load([p])
            return pimu_ab.analyse(rows, bad)

    def test_agree_recovers_zeros_scale_sign(self):
        rep = self.run_case()
        self.assertEqual(rep['ab_decision']['outcome'], 'agree', rep['ab_decision'])
        m = rep['moving']
        self.assertEqual(m['sign_estimate'], 1)
        self.assertGreater(m['corr'], 0.999)
        self.assertAlmostEqual(m['scale_obd_per_gyro'], 1.0, delta=0.005)
        self.assertAlmostEqual(m['obd_on_gyro']['intercept_dps'], 0.016, delta=0.01)
        self.assertEqual(len(rep['parked']), 4)
        for i in rep['parked']:
            self.assertEqual(i['rows'], 6)
            self.assertAlmostEqual(i['gyro']['zero_dps'], 0.004, delta=0.01)
            self.assertAlmostEqual(i['obd']['zero_dps'], 0.02, delta=0.01)
            # 0.02 deg/s = 0.53 counts below the nominal 2047 (clockwise positive = count below the zero)
            self.assertAlmostEqual(i['obd']['zero_count'], 2047 - math.radians(i['obd']['zero_dps']) * 1e6 / 658.615, places=3)
        self.assertLess(rep['parked_across']['obd']['range'], 0.03)
        self.assertEqual(rep['present']['both'], rep['rows'])
        self.assertEqual(rep['malformed'], 0)
        self.assertAlmostEqual(rep['coverage']['o'], 0.98, delta=0.03)

    def test_scale_disagree(self):
        rep = self.run_case(o_scale=1.06)
        self.assertEqual(rep['ab_decision']['outcome'], 'scale_disagree', rep['ab_decision'])
        self.assertAlmostEqual(rep['moving']['scale_obd_per_gyro'], 1.06, delta=0.01)
        # 2.5 % is inside the 3 % tolerance
        self.assertEqual(self.run_case(o_scale=1.025)['ab_decision']['outcome'], 'agree')

    def test_sign_mismatch_estimated(self):
        rep = self.run_case(g_sign=-1)
        self.assertEqual(rep['ab_decision']['outcome'], 'sign_mismatch', rep['ab_decision'])
        self.assertEqual(rep['moving']['sign_estimate'], -1)
        self.assertEqual(rep['moving']['expected_sign'], 1)

    def test_projection_rate_dependent(self):
        # gyro under-reads only in fast turns (a gravity estimate tilting under lateral acceleration)
        rep = self.run_case(g_scale_fn=lambda r: 1.0 if abs(r) < 8 else 0.90)
        self.assertEqual(rep['ab_decision']['outcome'], 'phone_projection_unreliable', rep['ab_decision'])
        # OBD per gyro rises in the fast bin: the gyro reads low there
        self.assertGreater(rep['moving']['rate_dependence'], 0.05)

    def test_projection_gravity_invalid_share(self):
        rep = self.run_case(g_grav0_every=5)   # 20 % of the gyro rows
        self.assertEqual(rep['ab_decision']['outcome'], 'phone_projection_unreliable', rep['ab_decision'])
        self.assertGreater(rep['excluded']['g_grav0'], 0)

    def test_gyro_unusable(self):
        rep = self.run_case(g_grav0_every=1)
        self.assertEqual(rep['ab_decision']['outcome'], 'gyro_unusable', rep['ab_decision'])

    def test_obd_unusable_gap_and_zero(self):
        rep = self.run_case(o_gap_every=1)
        self.assertEqual(rep['ab_decision']['outcome'], 'obd_unusable', rep['ab_decision'])
        self.assertEqual(rep['pair_rows'], 0)
        # a zero that jumps 0.2 deg/s between parked intervals
        rep = self.run_case(o_zero_steps=lambda i: 0.02 + 0.2 * (i // 40))
        self.assertEqual(rep['ab_decision']['outcome'], 'obd_unusable', rep['ab_decision'])
        self.assertGreater(rep['parked_across']['obd']['range'], 0.1)

    def test_not_decidable_parked_only(self):
        rep = self.run_case(rates=[0.0] * 30)
        self.assertEqual(rep['ab_decision']['outcome'], 'not_decidable', rep['ab_decision'])
        self.assertEqual(len(rep['parked']), 1)
        self.assertEqual(rep['parked'][0]['rows'], 30)

    def test_poor_correlation(self):
        rep = self.run_case(o_noise=3.0)
        self.assertEqual(rep['ab_decision']['outcome'], 'poor_correlation', rep['ab_decision'])
        # disturbed rows are trimmed from corr, but more than 10 % of them is poor_correlation
        rep = self.run_case(outliers=1, outlier_every=7)
        self.assertEqual(rep['ab_decision']['outcome'], 'poor_correlation', rep['ab_decision'])
        self.assertGreater(rep['moving']['outlier_share'], 0.10)
        self.assertGreater(rep['moving']['corr'], 0.98)

    def test_exclusions_and_one_sided(self):
        rates = profile()
        rep = self.run_case(o_gap_every=10, obd_missing={7, 8, 9})
        self.assertEqual(rep['present']['only_g'], 3)
        self.assertEqual(rep['excluded']['o_gap'], len([i for i in range(len(rates)) if i % 10 == 0 and i not in (7, 8, 9)]))
        self.assertEqual(rep['pair_rows'], len(rates) - 3 - rep['excluded']['o_gap'])
        self.assertEqual(rep['ab_decision']['outcome'], 'agree', rep['ab_decision'])

    def test_robust_to_outlier_rows(self):
        rep = self.run_case(outliers=1)
        self.assertEqual(rep['ab_decision']['outcome'], 'agree', rep['ab_decision'])
        self.assertGreater(rep['moving']['outliers'], 0)
        self.assertLess(rep['moving']['corr_all'], 0.98)
        self.assertAlmostEqual(rep['moving']['obd_on_gyro']['slope'], 1.0, delta=0.01)

    def test_parked_needs_stationary_hint_and_consecutive(self):
        rep = self.run_case(rates=[0.0] * 10, stationary=False)
        self.assertEqual(rep['parked'], [])
        # a phone reboot (t restarts) splits a parked run
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'a.txt'
            make_log(p, rates=[0.0] * 5)
            q = Path(d) / 'b.txt'
            make_log(q, rates=[0.0] * 5)
            rows, bad = pimu_ab.load([p, q])
            rep = pimu_ab.analyse(rows, bad)
            self.assertEqual([i['rows'] for i in rep['parked']], [5, 5])

    def test_parse_format(self):
        lines = [
            '2026-10-17T18:53:29.976+0900 t=4302205167ms obd_check_start device=AA:BB:CC:00:00:02\n',
            '2026-10-17T19:00:10.000+0900 t=100ms ab iv_ms=10000 sel=MIX g_n=500 g_dur_us=10000000 g_dth_urad=174533 '
            'g_grav=1 g_st=0 o_n=499 o_dur_us=9980000 o_dth_urad=-5 o_gap=1\n',
            '2026-10-17T19:00:20.000+0900 t=200ms ab iv_ms=10000 sel=OBD o_n=1 o_dur_us=20000 o_dth_urad=-7 o_gap=0 miss=gyro\n',
            '2026-10-17T19:00:30.000+0900 t=300ms ab iv_ms=x sel=OBD\n',
            '2026-10-17T19:00:40.000+0900 t=400ms ab broken\n',
            'garbage ab iv_ms=1\n',
        ]
        rows, bad = pimu_ab.parse_lines(lines)
        self.assertEqual(len(rows), 2)
        self.assertEqual(bad, 2)
        self.assertEqual(rows[0]['sel'], 'MIX')
        self.assertEqual(rows[0]['g_dth_urad'], 174533)
        self.assertEqual(rows[1]['miss'], 'gyro')
        self.assertNotIn('g_n', rows[1])
        rep = pimu_ab.analyse(rows, bad)
        self.assertEqual(rep['present'], dict(g=1, o=2, both=1, only_g=0, only_o=1))
        self.assertEqual(rep['excluded']['o_gap'], 1)
        # 174533 urad over 10 s = 1.0000 deg/s
        self.assertAlmostEqual(rep['per_row'][0]['gyro_dps'], 1.0, places=3)
        self.assertIsNone(rep['per_row'][0]['obd_dps'])

    def test_long_interval_excluded(self):
        lines = ['2026-10-17T19:00:10.000+0900 t=%dms ab iv_ms=%d sel=OBD g_n=500 g_dur_us=%d g_dth_urad=0 g_grav=1 '
                 'g_st=500 o_n=500 o_dur_us=10000000 o_dth_urad=0 o_gap=0\n' % (t, iv, iv * 1000)
                 for t, iv in ((10000, 10000), (40000, 30000))]
        rep = pimu_ab.analyse(*pimu_ab.parse_lines(lines))
        self.assertEqual(rep['excluded']['long_interval'], 1)
        self.assertEqual(rep['pair_rows'], 1)

    def test_cli_json_and_text(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'mx5imu-log.txt'
            make_log(p)
            out = subprocess.run([sys.executable, str(REPO / 'tools' / 'pimu_ab.py'), '--json', str(p)],
                                 check=True, capture_output=True, text=True).stdout
            rep = json.loads(out)
            self.assertEqual(rep['ab_decision']['outcome'], 'agree')
            self.assertEqual(rep['rule'], pimu_ab.RULE)
            txt = subprocess.run([sys.executable, str(REPO / 'tools' / 'pimu_ab.py'), str(p)],
                                 check=True, capture_output=True, text=True).stdout
            self.assertIn('A/B decision (pre-registered): agree', txt)
            self.assertIn('sign estimate 1 (expected +1)', txt)
            empty = Path(d) / 'empty.txt'
            empty.write_text('2026-10-17T18:54:07.979+0900 t=1ms connected seq=0\n')
            r = subprocess.run([sys.executable, str(REPO / 'tools' / 'pimu_ab.py'), str(empty)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 2)
            self.assertIn('no ab rows', r.stdout)

    def test_rule_matches_design_note(self):
        note = (REPO / 'validation' / 'OBD_YAW_CHANNEL_DESIGN_2026-10-15.md').read_text()
        block = note.split('<!-- ab-rule:begin -->')[1].split('<!-- ab-rule:end -->')[0]
        listed = {}
        for line in block.splitlines():
            if line.startswith('| `'):
                cells = [c.strip() for c in line.strip('|').split('|')]
                listed[cells[0].strip('`')] = float(cells[1])
        self.assertEqual(listed, {k: float(v) for k, v in pimu_ab.RULE.items()})


if __name__ == '__main__':
    unittest.main()
