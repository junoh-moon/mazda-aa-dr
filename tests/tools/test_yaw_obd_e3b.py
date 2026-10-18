"""tools/yaw_obd_e3b.py: E3b reference (OBD 0x079 correction of the 0x078 yaw zero), offline.

Synthetic inputs only; validation/YAW_OBD_BIAS_E3B_2026-10-19.md. The golden vector check replays
tests/golden/yaw_obd_e3b_vectors.json through the Python reference (the C++ port must reproduce
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
import yaw_obd_e3b as z  # noqa: E402

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

    def test_standstill_zero_fault_blocks_and_relearns(self):
        col = z.Collector(BOOT)
        feed_steady(col, 100 * NS, 30 * 50, 2047)
        n = feed_steady(col, 130 * NS, 30 * 50, 2050, seq0=100 + 30 * 50, flags_first=z.FLAG_SOURCE_OBD)
        y78_bins(col, 99, 162, -4.0, True)
        recs = {k: z.decode_ob(col.process(k)) for k in range(100, 158)}
        self.assertEqual(col.relearns, 1)
        self.assertTrue(any(recs[k]['health'] & z.H_STILL_BLOCK for k in range(130, 158)))
        self.assertTrue(all(recs[k]['status'] != z.ST_VALID for k in range(133, 158)))


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

    def test_standstill_disagreement(self):
        w = z.Worker(BOOT)
        for k in range(1000, 1012):                                        # 12 still bins, offset -1 count
            for c in range(10):
                w.on_window(k * NS + c * 100_000_000 + 1, 5 * 2047, 5, True, None)
        w.on_window(1012 * NS + 1, 5 * 2047, 5, True, None)
        r = w.on_ob(1012 * NS + 5, self.rec(1, 1009, b=-64), 1001)         # b -4 vs observed -1
        self.assertEqual(r, 'still_disagree')
        self.assertEqual(w.b_at(1012 * NS + 6), 0)
        self.assertEqual(w.on_ob(1013 * NS, self.rec(2, 1010, b=-16), 1001), 'disabled')

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


class EndToEnd(unittest.TestCase):
    def test_tunnel_run_removes_most_of_the_bias(self):
        r = z.run_one('S04_tunnel', 424242)
        e = np.array([[abs(x[2]), abs(x[3])] for x in r['errs']])
        self.assertGreater(r['valid_share'], 0.5)
        self.assertLess(e[:, 0].max(), max(0.5 * e[:, 1].max(), 3.0))

    def test_garage_without_obd_standstill_never_corrects(self):
        r = z.run_one('S14_garage_no_stop', 7)
        self.assertFalse(r['b_nonzero'])

    def test_old_app_never_corrects(self):
        r = z.run_one('F5_old_app', 8)
        self.assertFalse(r['b_nonzero'])


GOLDEN = REPO / 'tests' / 'golden' / 'yaw_obd_e3b_vectors.json'


@unittest.skipUnless(GOLDEN.exists(), 'golden vectors not generated')
class Golden(unittest.TestCase):
    def test_reference_reproduces_golden_vectors(self):
        import make_yaw_obd_e3b_golden as g
        doc = json.loads(GOLDEN.read_text())
        self.assertEqual(doc['format'], 'mx5dr.yaw_obd_e3b.golden.v1')
        for case in doc['cases']:
            out = g.replay(case)
            self.assertEqual(out, case['expect'], case['name'])


if __name__ == '__main__':
    unittest.main()
