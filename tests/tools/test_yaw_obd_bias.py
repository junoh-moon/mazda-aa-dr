"""tools/yaw_obd_bias.py: stage 3 OBD yaw-zero estimator prototype (offline research tool).

Synthetic inputs only; the public ND log is used only when MX5DR_ND_CAN_CSV names it
(validation/YAW_OBD_BIAS_DESIGN_2026-10-18.md).
"""
import os
from pathlib import Path
import sys
import unittest

try:
    import numpy as np
except ImportError:  # the research tool needs numpy; the product build does not
    raise unittest.SkipTest('numpy not installed')

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
import yaw_obd_bias as y  # noqa: E402


def scene(T=200.0, still_until=30.0, rate_dps=0.0, bias=3.0, z79=2047.6):
    t = np.arange(0.0, T, y.PERIOD)
    still = t < still_until
    w = np.where(still, 0.0, rate_dps * np.sin(t / 7.0)) / y.DEG_PER_COUNT
    raw79 = np.round(w + z79)
    raw78 = np.round(w + 2048.0 + bias)
    return t, raw78, raw79, still


class Estimator(unittest.TestCase):
    def test_removes_constant_bias_without_lag(self):
        t, r78, r79, still = scene(bias=4.0, rate_dps=5.0)
        rng = np.random.default_rng(1)
        tr, _, _ = y.deliver(t, rng, L=0.3, jitter_sd=0.0, P=0.02, loss=0.0)
        bh, d = y.run_estimator(t, r78, r79, still, tr)
        self.assertGreater(d['valid_bins'], 100)
        self.assertAlmostEqual(float(np.median(bh[t > 100])), 4.0, delta=0.4)
        self.assertAlmostEqual(d['z79'], 2048.0, delta=0.6)   # integer raw at rest rounds 2047.6 up

    def test_fail_closed_without_obd(self):
        t, r78, r79, still = scene()
        tr = np.full(t.size, np.nan)
        bh, d = y.run_estimator(t, r78, r79, still, tr)
        self.assertEqual(float(np.abs(bh).max()), 0.0)
        self.assertEqual(d['valid_bins'], 0)

    def test_no_standstill_means_no_zero_and_no_correction(self):
        t, r78, r79, still = scene(still_until=0.0, rate_dps=5.0)
        rng = np.random.default_rng(2)
        tr, _, _ = y.deliver(t, rng, L=0.2, jitter_sd=0.0, P=0.02, loss=0.0)
        bh, d = y.run_estimator(t, r78, r79, still, tr)
        self.assertIsNone(d['z79'])
        self.assertEqual(float(np.abs(bh).max()), 0.0)

    def test_scale_gate_blocks_large_mismatch(self):
        t = np.arange(0.0, 240.0, y.PERIOD)
        still = t < 30.0
        w = np.where(still, 0.0, 15.0 * np.sin(t / 5.0)) / y.DEG_PER_COUNT
        r79 = np.round(1.2 * w + 2047.6)
        r78 = np.round(w + 2050.0)
        rng = np.random.default_rng(3)
        tr, _, _ = y.deliver(t, rng, L=0.2, jitter_sd=0.0, P=0.02, loss=0.0)
        _, d = y.run_estimator(t, r78, r79, still, tr)
        self.assertIsNotNone(d['scale'])
        self.assertLess(d['scale'], 0.95)
        self.assertGreater(d['reasons'].get('scale', 0) + d['reasons'].get('blocked', 0)
                           + d['reasons'].get('rms', 0), 0)

    def test_hold_then_ramp_to_zero_after_dropout(self):
        t, r78, r79, still = scene(T=200.0, bias=5.0)
        rng = np.random.default_rng(4)
        tr, _, _ = y.deliver(t, rng, L=0.2, jitter_sd=0.0, P=0.02, loss=0.0, dropouts=[(80.0, 200.0)])
        bh, _ = y.run_estimator(t, r78, r79, still, tr)
        self.assertAlmostEqual(float(bh[np.searchsorted(t, 85.0)]), 5.0, delta=0.5)   # held
        self.assertEqual(float(bh[np.searchsorted(t, 150.0)]), 0.0)                   # released

    def test_huber_lag_tap_is_clamped(self):
        g = np.linspace(-5, 5, 20)
        r = 2.0 + 3.0 * g
        b, f, _ = y.huber_fit(r, g, np.ones(20), y.Params)
        self.assertEqual(f, y.Params.F_MAX)

    def test_owner_bias_changes_only_while_moving(self):
        t = np.arange(0.0, 600.0, y.PERIOD)
        moving = np.zeros(t.size, bool)
        rng = np.random.default_rng(5)
        b = y.owner_bias(t, moving, rng)
        self.assertLessEqual(len(set(np.round(b, 9))), 2)   # start level and the one +2.6 step


class StageThreeVariants(unittest.TestCase):
    def turning(self, T=240.0, bias=4.0):
        t = np.arange(0.0, T, y.PERIOD)
        still = t < 30.0
        w = np.where(still, 0.0, 12.0 * np.sin(t / 6.0)) / y.DEG_PER_COUNT
        return t, np.round(w + 2048.0 + bias), np.round(w + 2047.0), still

    def test_restore_times_removes_jitter_and_bursts(self):
        t = np.arange(0.0, 120.0, y.PERIOD)
        rng = np.random.default_rng(6)
        tr, L, _ = y.deliver(t, rng, L=0.6, jitter_sd=0.1, P=0.3, loss=0.0)
        th, chain = y.restore_times(t, tr)
        self.assertEqual(int(chain.max()), 0)
        d = th[t > 20.0] - t[t > 20.0]
        self.assertLess(float(d.max() - d.min()), 0.05)
        self.assertAlmostEqual(float(np.median(d)), 0.6, delta=0.05)

    def test_fill_losses_recreates_frames_and_flags(self):
        t = np.arange(0.0, 1.0, y.PERIOD)
        raw = np.arange(t.size, dtype=float)
        tr = t + 0.1
        tr[5:7] = np.nan
        r2, t2, flag = y.fill_losses(t, raw, tr)
        self.assertTrue(np.all(np.isfinite(t2)))
        self.assertEqual(list(r2[5:7]), [4.0, 4.0])
        self.assertTrue(flag[5] and flag[6] and flag[7])
        r3, t3, flag3 = y.fill_losses(t, raw, tr, fill=False)
        self.assertTrue(np.isnan(t3[5]) and flag3[7] and not flag3[5])

    def test_e3_estimates_lag_and_removes_bias(self):
        t, r78, r79, still = self.turning(bias=4.0)
        rng = np.random.default_rng(7)
        tr, _, _ = y.deliver(t, rng, L=0.6, jitter_sd=0.05, P=0.2, loss=0.0)
        bh, d = y.run_estimator(t, r78, r79, still, tr, kind='E3')
        self.assertAlmostEqual(d['f'], 0.58, delta=0.06)
        self.assertAlmostEqual(float(np.median(bh[t > 150])), 4.0, delta=0.5)

    def test_e3_without_standstill_never_corrects(self):
        t, r78, r79, _ = self.turning()
        still = np.zeros(t.size, bool)
        rng = np.random.default_rng(10)
        tr, _, _ = y.deliver(t, rng, L=0.4, jitter_sd=0.0, P=0.02, loss=0.0)
        bh, d = y.run_estimator(t, r78, r79, still, tr, kind='E3')
        self.assertIsNone(d['z79'])
        self.assertEqual(float(np.abs(bh).max()), 0.0)

    def test_e3_unfilled_loss_disables(self):
        t, r78, r79, still = self.turning()
        rng = np.random.default_rng(8)
        tr, _, _ = y.deliver(t, rng, L=0.3, jitter_sd=0.0, P=0.02, loss=0.10)
        r79f, trf, flag = y.fill_losses(t, r79, tr, fill=False)
        bh, d = y.run_estimator(t, r78, r79f, still, trf, kind='E3', unfilled=flag)
        self.assertEqual(d['valid_bins'], 0)
        self.assertEqual(float(np.abs(bh).max()), 0.0)

    def test_e2_segment_between_quiet_endpoints(self):
        t = np.arange(0.0, 200.0, y.PERIOD)
        still = t < 30.0
        w = np.where((t > 60) & (t < 70), 15.0, 0.0) / y.DEG_PER_COUNT   # one turn between straights
        r78, r79 = np.round(w + 2051.0), np.round(w + 2047.0)
        rng = np.random.default_rng(9)
        tr, _, _ = y.deliver(t, rng, L=0.1, jitter_sd=0.05, P=0.02, loss=0.0)
        bh, d = y.run_estimator(t, r78, r79, still, tr, kind='E2')
        self.assertAlmostEqual(float(np.median(bh[t > 100])), 3.0, delta=0.3)
        # Known E2 weakness (recorded): with 0.8 s lag and 0.25 s bursts its receipt-time scale
        # fit reads about 0.91 and the scale gate blocks the correction (fail-closed, b = 0).
        rng = np.random.default_rng(9)
        tr, _, _ = y.deliver(t, rng, L=0.8, jitter_sd=0.05, P=0.25, loss=0.0)
        bh, d = y.run_estimator(t, r78, r79, still, tr, kind='E2')
        self.assertLess(d['scale'], 0.95)
        self.assertEqual(float(np.median(bh[t > 100])), 0.0)


@unittest.skipUnless(os.environ.get('MX5DR_ND_CAN_CSV'), 'public ND log not given (MX5DR_ND_CAN_CSV)')
class PublicLog(unittest.TestCase):
    def test_loads_pairs_and_stops(self):
        pub = y.load_public(os.environ['MX5DR_ND_CAN_CSV'])
        self.assertEqual(pub['t'].size, 14411)
        self.assertGreater(int(pub['still'].sum()), 1000)


if __name__ == '__main__':
    unittest.main()
