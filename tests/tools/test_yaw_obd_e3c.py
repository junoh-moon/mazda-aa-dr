"""tools/yaw_obd_e3c.py: E3c reference (OBD 0x079 correction of the 0x078 yaw zero), offline.

Synthetic inputs only; validation/YAW_OBD_BIAS_E3C_2026-10-19.md. The golden vector check replays
tests/golden/yaw_obd_e3c_vectors.json through the Python reference (the C++ port must reproduce
the same file byte for byte).
"""
import json
from pathlib import Path
import struct
import sys
import unittest

try:
    import numpy as np
except ImportError:  # the research tool needs numpy; the product build does not
    raise unittest.SkipTest('numpy not installed')

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
import yaw_obd_e3c as z  # noqa: E402

NS = z.NS
BOOT = bytes(range(8))


def frame_urad(raw):
    return int(np.floor(-(raw - 2047) * 13.1723 + 0.5))


class IntegerHelpers(unittest.TestCase):
    def test_rdiv_rounds_half_up_for_negative_numerators(self):
        self.assertEqual(z.rdiv(5, 2), 3)
        self.assertEqual(z.rdiv(-5, 2), -2)
        self.assertEqual(z.rdiv(-7, 2), -3)
        self.assertEqual(z.rdiv(7, 2), 4)
        self.assertEqual(z.rdiv(-1, 3), 0)

    def test_lattice_tables_are_exact_and_invertible(self):
        on = [r for r in range(4096) if z.LAT_INV[r] is not None]
        self.assertEqual(len(on), 2682)
        for r in on:
            self.assertEqual(z.lattice_raw(z.LAT_INV[r]), r)
        self.assertEqual(z.LAT_INV[2047], 0)
        self.assertIsNone(z.LAT_INV[2048])
        self.assertEqual(z.LAT_LIN[0], 0)
        self.assertEqual(z.LAT_LIN[1], 24)          # 1.52941 counts in Q4
        # same lattice as tools/yaw_obd_bias.py (floating definition)
        import yaw_obd_bias as y
        for n in (-600, -1, 0, 1, 2, 3, 577):
            self.assertEqual(z.lattice_raw(n), int(y.lattice(n)))

    def test_raw_recovered_from_carried_dtheta(self):
        raws = [2047, 2049, 1737, 2925, 2045, 2050, 2047, 3001]
        carry = 0.0
        for r in raws:
            v = -(r - 2047) * 13.1723 + carry
            d = int(np.floor(v + 0.5))
            carry = v - d
            self.assertEqual(z.raw_from_urad(d), r)

    def test_correlation_score_operation_order(self):
        self.assertEqual(z.corr_score(3, 4, 9), 3.0 / 6.0)


class Records(unittest.TestCase):
    def test_mxob_round_trip_and_crc(self):
        b = z.encode_ob(7, BOOT, 5012, z.ST_VALID, z.H_Z79 | z.H_LAG, -73, 9, 340, 18, 40, -12, 3, 50)
        self.assertEqual(len(b), 48)
        r = z.decode_ob(b)
        self.assertEqual((r['seq'], r['bin'], r['b_q'], r['zoff_q'], r['lag_ms'], r['status']),
                         (7, 5012, -73, 9, 340, z.ST_VALID))
        bad = bytearray(b)
        bad[28] ^= 1
        self.assertIsNone(z.decode_ob(bytes(bad)))
        self.assertIsNone(z.decode_ob(b[:47]))

    def test_mxyw_round_trip(self):
        sums = [10, -20, 0, 5, 6, 7, -8, 9, 30000, -30000]
        counts = [5, 5, 4, 6, 5, 5, 5, 5, 15, 15]
        b = z.encode_y78(3, BOOT, 77, z.Y78_STILL, sums, counts, 11)
        self.assertEqual(len(b), 64)
        r = z.decode_y78(b)
        self.assertEqual((r['bin'], r['sums'], r['counts'], r['flags']), (77, sums, counts, z.Y78_STILL))


def feed_steady(col, t0_ns, n, raw, seq0=100, flags_first=z.FLAG_SOURCE_OBD | z.FLAG_FRAME_RESET, lat_ns=300_000_000):
    seq = seq0
    for i in range(n):
        f = flags_first if i == 0 else z.FLAG_SOURCE_OBD
        col.on_frame(t0_ns + i * z.PERIOD_NS + lat_ns, seq & 0xFFFFFFFF, f, frame_urad(raw))
        seq += 1
    return seq


def y78_bins(col, k0, k1, a_counts, still, seq0=1):
    """Feed MXYW records for bins k0..k1 with mean offset a_counts (counts rel. 2048) per sample."""
    seq = seq0
    for k in range(k0, k1 + 1):
        st = still(k) if callable(still) else still
        counts = [5] * 10
        sums = [int(round(a_counts * 5))] * 10
        col.on_y78(z.encode_y78(seq, BOOT, k, z.Y78_STILL if st else 0, sums, counts, 10))
        seq += 1
    return seq


class Collector(unittest.TestCase):
    def test_chain_breaks_on_reset_seq_gap_and_sentinel(self):
        col = z.Collector(BOOT)
        col.on_frame(1_000_000_000, 5, z.FLAG_SOURCE_OBD | z.FLAG_FRAME_RESET, 0)
        col.on_frame(1_020_000_000, 6, z.FLAG_SOURCE_OBD, 0)
        self.assertEqual(col.chain, 0)
        col.on_frame(1_040_000_000, 8, z.FLAG_SOURCE_OBD, 0)            # seq gap
        self.assertEqual(col.chain, 1)
        col.on_frame(1_060_000_000, 9, z.FLAG_SOURCE_OBD, -27000)       # sentinel: dropped
        self.assertEqual(col.chain, -1)
        col.on_frame(1_080_000_000, 10, z.FLAG_SOURCE_OBD, 0)
        self.assertEqual(col.chain, 2)
        self.assertEqual(col.fr_n, 4)

    def test_seq_wrap_is_continuous(self):
        col = z.Collector(BOOT)
        col.on_frame(1_000_000_000, 0xFFFFFFFF, z.FLAG_SOURCE_OBD | z.FLAG_FRAME_RESET, 0)
        col.on_frame(1_020_000_000, 0, z.FLAG_SOURCE_OBD, 0)
        self.assertEqual(col.chain, 0)
        self.assertEqual(col.idx, 1)

    def test_envelope_restores_producer_time_under_bursts(self):
        col = z.Collector(BOOT)
        rng = np.random.default_rng(1)
        seq = 0
        for i in range(3000):
            t_prod = 10 * NS + i * z.PERIOD_NS
            t_rx = int(np.ceil((t_prod + 400_000_000 + abs(rng.normal(0, 30e6))) / 200e6) * 200e6)
            col.on_frame(t_rx, seq, z.FLAG_SOURCE_OBD | (z.FLAG_FRAME_RESET if i == 0 else 0), 0)
            seq += 1
        th, q, first, ch = col._frames()
        lat = th - (10 * NS + np.arange(3000) * z.PERIOD_NS)
        self.assertLess(float(lat.max() - lat.min()), 5e6)                # within 5 ms
        self.assertAlmostEqual(float(np.median(lat)), 0.4e9, delta=25e6)

    def test_old_app_latch_disables_for_the_boot(self):
        col = z.Collector(BOOT)
        col.on_frame(1 * NS, 1, z.FLAG_SOURCE_OBD | z.FLAG_FRAME_RESET, 0)
        col.on_frame(1 * NS + 40_000_000, 2, z.FLAG_SOURCE_OBD | z.FLAG_OBD_GAP, 0)   # unfilled loss
        self.assertTrue(col.old_app)
        r = z.decode_ob(col.process(5))
        self.assertEqual(r['status'], z.ST_INVALID)
        self.assertTrue(r['health'] & z.H_OLD_APP)

    def test_filled_frames_do_not_break_the_chain(self):
        col = z.Collector(BOOT)
        col.on_frame(1 * NS, 1, z.FLAG_SOURCE_OBD | z.FLAG_FRAME_RESET, 0)
        col.on_frame(1 * NS + 60_000_000, 2, z.FLAG_SOURCE_OBD | z.FLAG_OBD_GAP | z.FLAG_OBD_FILLED, 0)
        col.on_frame(1 * NS + 60_000_000, 3, z.FLAG_SOURCE_OBD, 0)
        self.assertFalse(col.old_app)
        self.assertEqual((col.chain, col.idx), (0, 2))

    def standstill_then(self, bias_counts, raw79=2047, secs=40):
        col = z.Collector(BOOT)
        feed_steady(col, 100 * NS, secs * 50, raw79)
        y78_bins(col, 99, 100 + secs + 2, bias_counts, True)
        recs = [z.decode_ob(col.process(k)) for k in range(100, 100 + secs - 2)]
        return col, recs

    def test_standstill_learns_zero_and_bias(self):
        col, recs = self.standstill_then(-4.0)
        self.assertEqual(col.zoff, 0)                                     # raw 2047 = lattice n 0
        valid = [r for r in recs if r['status'] == z.ST_VALID]
        self.assertGreater(len(valid), 10)
        self.assertEqual(valid[-1]['b_q'], -64)

    def test_no_standstill_no_correction(self):
        col = z.Collector(BOOT)
        feed_steady(col, 100 * NS, 2000, 2047)
        y78_bins(col, 99, 142, -4.0, False)
        recs = [z.decode_ob(col.process(k)) for k in range(100, 138)]
        self.assertTrue(all(r['status'] == z.ST_INVALID and r['b_q'] == 0 for r in recs))

    def test_change_point_restarts_the_window(self):
        col = z.Collector(BOOT)
        feed_steady(col, 100 * NS, 80 * 50, 2047)
        y78_bins(col, 99, 140, -4.0, True)
        y78_bins(col, 141, 182, 3.0, True, seq0=100)
        recs = {k: z.decode_ob(col.process(k)) for k in range(100, 178)}
        self.assertEqual(recs[139]['b_q'], -64)
        after = [k for k in range(141, 178) if recs[k]['b_q'] == 48]
        self.assertTrue(after and after[0] <= 147, after[:3])           # 5 consistent bins after the step
        self.assertGreaterEqual(col.cp_count, 1)

    def test_standstill_zero_fault_relearns_without_blind_period(self):
        col = z.Collector(BOOT)
        feed_steady(col, 100 * NS, 30 * 50, 2047)
        feed_steady(col, 130 * NS, 30 * 50, 2050, seq0=100 + 30 * 50, flags_first=z.FLAG_SOURCE_OBD)
        y78_bins(col, 99, 162, -4.0, True)
        recs = {k: z.decode_ob(col.process(k)) for k in range(100, 158)}
        self.assertEqual(col.relearns, 1)
        self.assertFalse(any(recs[k]['health'] & z.H_STILL_BLOCK for k in range(100, 158)))
        # no hold of the value built on the old zero; valid again once the new zero is learned
        k_fault = min(k for k in range(130, 158) if recs[k]['status'] != z.ST_VALID)
        self.assertEqual(recs[k_fault]['status'], z.ST_INVALID)
        back = [k for k in range(k_fault, 158) if recs[k]['status'] == z.ST_VALID]
        self.assertTrue(back and back[0] - k_fault <= 12, back[:2])
        self.assertEqual(recs[157]['b_q'], -64)
        self.assertEqual(col.zoff, z.LAT_LIN[z.LAT_INV[2050]])

    def test_scale_gate_needs_leverage_and_blocks_outside_band(self):
        def run(scale79, amp):
            col = z.Collector(BOOT)
            col.lag_steps = 0
            col.zoff = 0
            col.z79_vals = [0] * 5
            for k in range(1000, 1240):
                o = int(amp * 16 * (1 if (k // 7) % 2 else -1))
                col.usable.append([k, None, False, int(-4 * 16 + o / scale79), o])
            col.usable = col.usable[-z.P.USABLE_KEEP:]
            return col
        # 106 counts sd is the leverage floor; 5 % mismatch with 200 counts amplitude -> blocked
        col = run(1.05, 200)
        r = z.decode_ob(col.process(1240))
        self.assertTrue(r['health'] & z.H_SCALE_BLOCK)
        self.assertEqual(r['status'], z.ST_INVALID)
        # same mismatch with 50 counts amplitude: not judged (no leverage), no block
        col = run(1.05, 50)
        r = z.decode_ob(col.process(1240))
        self.assertFalse(r['health'] & z.H_SCALE_BLOCK)
        # 1 % mismatch with leverage: inside the band
        col = run(1.01, 200)
        r = z.decode_ob(col.process(1240))
        self.assertFalse(r['health'] & z.H_SCALE_BLOCK)


class Worker(unittest.TestCase):
    def rec(self, seq, k, status=z.ST_VALID, b=-64, boot=BOOT):
        return z.encode_ob(seq, boot, k, status, z.H_Z79, b, 0, 300, 20, 20, 0, 0, 50)

    def test_acceptance_checks(self):
        w = z.Worker(BOOT, collector_uid=1001)
        t = 1004 * NS
        self.assertEqual(w.on_ob(t, self.rec(1, 1000), 0), 'uid')
        self.assertEqual(w.on_ob(t, self.rec(1, 1000, boot=b'x' * 8), 1001), 'boot')
        self.assertEqual(w.on_ob(t, self.rec(1, 1000)[:-1] + b'\0', 1001), 'format')
        self.assertEqual(w.on_ob(t, self.rec(1, 990), 1001), 'age')
        self.assertEqual(w.on_ob(t, self.rec(1, 1000, b=300), 1001), 'b_max')
        self.assertEqual(w.on_ob(t, self.rec(5, 1000), 1001), 'ok')
        self.assertEqual(w.on_ob(t, self.rec(5, 1000), 1001), 'seq')
        self.assertEqual(w.on_ob(t, self.rec(4, 1000), 1001), 'seq')
        self.assertEqual(w.b_at(t), -64)

    def test_stale_hold_then_ramp(self):
        w = z.Worker(BOOT)
        w.on_ob(1004 * NS, self.rec(1, 1000, b=-160), 1001)
        self.assertEqual(w.b_at(1018 * NS), -160)                          # held 15 s
        self.assertEqual(w.b_at(int(1026.5 * NS)), -80)                    # half way down the ramp
        self.assertEqual(w.b_at(1035 * NS), 0)

    def test_invalid_ramps_immediately(self):
        w = z.Worker(BOOT)
        w.on_ob(1004 * NS, self.rec(1, 1000, b=-160), 1001)
        w.on_ob(1005 * NS, self.rec(2, 1001, status=z.ST_INVALID, b=0), 1001)
        self.assertEqual(w.b_at(int(1012.5 * NS)), -80)
        self.assertEqual(w.b_at(1021 * NS), 0)

    def still_bins(self, w, k0, k1, raw=2047):
        for k in range(k0, k1):
            for c in range(10):
                w.on_window(k * NS + c * 100_000_000 + 1, 5 * raw, 5, True, None)

    def test_standstill_disagreement_waits_for_agreement(self):
        w = z.Worker(BOOT)
        self.still_bins(w, 1000, 1012)                                     # offset -1 count
        w.on_window(1012 * NS + 1, 5 * 2047, 5, True, None)
        r = w.on_ob(1012 * NS + 5, self.rec(1, 1009, b=-64), 1001)         # b -4 vs observed -1
        self.assertEqual(r, 'still_disagree')
        self.assertEqual(w.b_at(1012 * NS + 6), 0)
        # still disagreeing: stays 0
        self.assertEqual(w.on_ob(1013 * NS, self.rec(2, 1010, b=-60), 1001), 'still_wait')
        self.assertEqual(w.b_at(1013 * NS + 1), 0)
        # a HOLDING record that agrees does not resume; a VALID one does, at once (no 60 s)
        self.assertEqual(w.on_ob(1013 * NS + 2, self.rec(3, 1010, status=z.ST_HOLDING, b=-16), 1001),
                         'still_wait')
        self.assertEqual(w.on_ob(1013 * NS + 3, self.rec(4, 1010, b=-20), 1001), 'ok')
        self.assertEqual(w.b_at(1013 * NS + 4), -20)

    def test_disagreement_reference_follows_a_new_stop(self):
        w = z.Worker(BOOT)
        self.still_bins(w, 1000, 1012)                                     # -1 count
        w.on_window(1012 * NS + 1, 5 * 2047, 5, True, None)
        self.assertEqual(w.on_ob(1012 * NS + 5, self.rec(1, 1009, b=-64), 1001), 'still_disagree')
        for k in range(1012, 1016):                                        # moving
            for c in range(10):
                w.on_window(k * NS + c * 100_000_000 + 2, 5 * 2060, 5, False, None)
        self.still_bins(w, 1016, 1028, raw=2044)                           # new stop: -4 counts
        w.on_window(1028 * NS + 1, 5 * 2044, 5, True, None)
        self.assertEqual(w.on_ob(1028 * NS + 5, self.rec(2, 1026, b=-64), 1001), 'ok')

    def test_window_cells_and_rail(self):
        w = z.Worker(BOOT)
        w.on_window(5 * NS + 50_000_000, 5 * 2050, 5, False, None)
        w.on_window(5 * NS + 150_000_000, 5 * 4095, 5, False, None)       # rail window: not summed
        rec = w.on_window(6 * NS + 1, 5 * 2048, 5, False, None)
        r = z.decode_y78(rec)
        self.assertEqual((r['bin'], r['sums'][0], r['counts'][0], r['counts'][1]), (5, 10, 5, 0))
        self.assertTrue(r['flags'] & z.Y78_RAIL)

    def test_gps_guard_needs_two_hits(self):
        w = z.Worker(BOOT)
        w.on_ob(1004 * NS, self.rec(1, 1000, b=-64), 1001)
        w.gps_check(1005 * NS, -64, 16)
        self.assertFalse(w.boot_disabled)
        w.gps_check(1015 * NS, -64, 16)
        self.assertTrue(w.boot_disabled)


class Clamp(unittest.TestCase):
    def test_episode_clamp_bounds_the_total(self):
        ec = z.EpisodeClamp(128)
        tot = 0
        for i in range(1, 501):                                            # b = 16 counts for 10 s
            tot += ec.apply(256, i)
        self.assertEqual(tot, 128 * 500)
        self.assertEqual(ec.acc, 128 * 500)
        # opposite sign is applied in full until the other bound
        self.assertEqual(ec.apply(-256, 501), -256)

    def test_episode_clamp_identity_below_bound(self):
        ec = z.EpisodeClamp(128)
        self.assertEqual([ec.apply(x, i + 1) for i, x in enumerate([-64, 100, -128, 0])], [-64, 100, -128, 0])

    def test_fast_path_equals_loop(self):
        rng = np.random.default_rng(3)
        for _ in range(50):
            inc = rng.integers(-1200, 1200, size=200)
            el = np.cumsum(rng.integers(1, 8, size=200))
            ec = z.EpisodeClamp(128)
            for x, e in zip(inc.tolist(), el.tolist()):
                ec.apply(x, e)
            self.assertEqual(z.clamp_total_q(inc, el, 128), ec.acc)

    def test_budget_kinds(self):
        e = (0.0, 100.0, 0.0, 0.0, 50.0)
        self.assertAlmostEqual(z.budget(e, True), 0.0023 * 100 * 180 / np.pi + 1.0)
        self.assertAlmostEqual(z.budget(e, False), 0.002 * 100 * 180 / np.pi + 5.0)

    def test_h4_unclamped_bound(self):
        self.assertAlmostEqual(z.h4_bound_deg(110.0), 16 * z.DEG_PER_COUNT * 110.0)


class EndToEnd(unittest.TestCase):
    def test_tunnel_run_removes_most_of_the_bias(self):
        r = z.run_one('S04_tunnel', 424242)
        e = np.array([[abs(x[2]), abs(x[3])] for x in r['errs']])
        self.assertGreater(r['valid_share'], 0.5)
        self.assertLess(e[:, 0].max(), max(0.5 * e[:, 1].max(), 3.0))

    def test_uncatchable_jump_is_bounded_by_the_clamp(self):
        r = z.run_one('F2_jump_tunnel', 31)
        cds = z.clamp_deg_s()
        for x in r['errs']:
            self.assertLessEqual(abs(x[2] - x[3]), cds * x[1] + 1e-6)

    def test_garage_without_obd_standstill_never_corrects(self):
        r = z.run_one('S14_garage_no_stop', 7)
        self.assertFalse(r['b_nonzero'])

    def test_old_app_never_corrects(self):
        r = z.run_one('F5_old_app', 8)
        self.assertFalse(r['b_nonzero'])


GOLDEN = REPO / 'tests' / 'golden' / 'yaw_obd_e3c_vectors.json'


@unittest.skipUnless(GOLDEN.exists(), 'golden vectors not generated')
class Golden(unittest.TestCase):
    def test_reference_reproduces_golden_vectors(self):
        import make_yaw_obd_e3c_golden as g
        doc = json.loads(GOLDEN.read_text())
        self.assertEqual(doc['format'], 'mx5dr.yaw_obd_e3c.golden.v1')
        for case in doc['cases']:
            out = g.replay(case)
            self.assertEqual(out, case['expect'], case['name'])


if __name__ == '__main__':
    unittest.main()
