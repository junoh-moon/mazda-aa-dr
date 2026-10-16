"""tools/pimu_eval.py and the analyzer's phone_imu section on authored rows.

A synthetic boot: a known heading path, the car's yaw digests with a constant
zero error and phone bins with a different constant bias. The evaluation must
recover each source's heading error over a GPS-lost episode and the synthetic
windows. validation/PHONE_IMU_CHANNEL_2026-10-14.md.
"""
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
import pimu_eval  # noqa: E402

BOOT = '12345678-1234-1234-1234-123456789abc'
RATE = 10.0                 # yaw samples per second
CAR_BIAS = 0.10             # deg/s of the car's yaw (zero error)
PHONE_BIAS = 0.01           # deg/s of the phone


def true_rate(t):
    return 3.0 if 200 <= t < 230 else 0.0      # one 90 degree turn


def heading(t):
    return 10.0 + sum(true_rate(s) for s in range(int(t)))


def write_trip(root, phone=True, lost=(400, 460)):
    logs = root / 'logs'
    logs.mkdir(parents=True)
    rows = [dict(kind='boot', schema=1, pid=1, mono_ns=0, boot_id=BOOT)]
    for k in range(0, 60):
        since = 100 + 10 * k
        y1 = []
        for j in range(10):
            t = since + j
            deg = true_rate(t) + CAR_BIAS
            # course change = -K * y1/rate  ->  y1 = -radians(deg) * rate / K
            y1.append(int(round(-math.radians(deg) * RATE / pimu_eval.YAW_RAD_PER_COUNT)))
        gc = []
        for j in range(10):
            t = since + j - pimu_eval.GPS_DELAY_S   # a fix received at since+j describes t
            if not lost[0] <= since + j <= lost[1]:
                gc.append(int(j * 10) * 1000 + int(round(heading(t))) % 360)
        rows.append(dict(kind='log_digest', schema=1, mono_ns=(since + 10) * 10**9, since_ns=since * 10**9,
                         y1=y1, y1n=[int(RATE)] * 10, yst=0, ystn=0, gc=gc, gq=3, gv=50, speed_kmh_mean=50.0))
    rows.append(dict(kind='beta_state', mono_ns=lost[0] * 10**9, to='GPS_LOST'))
    rows.append(dict(kind='beta_state', mono_ns=lost[1] * 10**9, to='ARMED'))
    (logs / 'trace.0.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in rows))
    if phone:
        prow = [dict(kind='pimu_boot', schema=1, m=0, boot_id=BOOT, svc=15, budget_bph=1, cap=1)]
        for start in range(100, 700, 10):
            d = [[50, int(round(math.radians(true_rate(start + i) + PHONE_BIAS) * 1e6)), 1000, 0, 5] for i in range(10)]
            prow.append(dict(kind='pimu', m=start * 1000, s0=0, s1=1, b=0, pt=0, ce=0, bv=0, sk=0, dr=0, fr=0, d=d))
        prow.append(dict(kind='pimu_sync', m=150000, q=1, rtt_us=30000, off_us=123456789))
        prow.append(dict(kind='pimu_sync', m=155000, q=2, rtt_us=40000, off_us=123456799))
        prow.append(dict(kind='pimu_sync', m=160000, q=3, rtt_us=20000, off_us=123456809))
        prow.append(dict(kind='pimu_link', m=100000, ev='connect', why='raw', path='/dev/pts/3', svc=15, cs=-1, st=-1, err=0))
        prow.append(dict(kind='bt_watch', m=90000, ev='ready', why='flag', pid=42, after_ms=1200))
        (logs / 'pimu.0.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in prow) + '{"kind":"pimu","m":')
    return logs


class PimuEval(unittest.TestCase):
    def test_episode_and_windows_recover_each_bias(self):
        with tempfile.TemporaryDirectory() as tmp:
            logs = write_trip(Path(tmp))
            out = subprocess.run([sys.executable, str(REPO / 'tools/pimu_eval.py'), '--json', str(logs)],
                                 capture_output=True, text=True, check=True).stdout
            report = json.loads(out)
            (boot,) = report['boots']
            (ep,) = boot['episodes']
            span = ep['seconds']
            self.assertGreater(span, 55)
            self.assertAlmostEqual(ep['car']['heading_error_deg'], CAR_BIAS * span, delta=1.2)
            self.assertAlmostEqual(ep['phone']['heading_error_deg'], PHONE_BIAS * span, delta=1.2)
            self.assertGreater(ep['car']['exit_jump_m'], 5 * ep['phone']['exit_jump_m'])
            s60 = report['synthetic_summary']['60']
            self.assertGreater(s60['car']['n'], 10)
            self.assertEqual(s60['phone']['n'], s60['car']['n'])
            self.assertLess(s60['phone']['heading_abs_median_deg'], s60['car']['heading_abs_median_deg'])
            # Windows through the 90 degree turn are followed by both sources.
            turn = [w for w in boot['synthetic']['60'] if w['start_s'] <= 199 and w['start_s'] + w['seconds'] >= 231]
            self.assertTrue(turn and all(abs(w['gps_turn_deg']) > 80 for w in turn))
            for w in turn:
                self.assertLess(abs(w['phone']['heading_error_deg']), 2.5)

    def test_missing_phone_bins_are_reported_not_imputed(self):
        with tempfile.TemporaryDirectory() as tmp:
            logs = write_trip(Path(tmp), phone=False)
            report = json.loads(subprocess.run([sys.executable, str(REPO / 'tools/pimu_eval.py'), '--json', str(logs)],
                                               capture_output=True, text=True, check=True).stdout)
            self.assertEqual(report['episode_summary']['phone']['n'], 0)
            self.assertEqual(report['episode_summary']['phone_missing'], 1)
            self.assertEqual(report['episode_summary']['car']['n'], 1)



# ---- Stage 2: OBD 0x079 bins (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md, section 15) ----

OBD_STOPS = ((100, 130), (400, 430), (700, 730), (1000, 1030))
OBD_END = 1300
CAR_ZEROS = (2046.0, 2049.0, 2045.0, 2048.0)


def obd_true_rate(t):
    """deg/s clockwise: turns of +90, +45, +90 and -30 degrees between the stops."""
    if 200 <= t < 230 or 800 <= t < 830 or 500 <= t < 515:
        return 3.0
    if 1100 <= t < 1110:
        return -3.0
    return 0.0


def obd_heading(t):
    return 10.0 + sum(obd_true_rate(s) for s in range(int(t)))


def stopped(t):
    return any(a <= t < b for a, b in OBD_STOPS)


def write_obd_trip(root, zeros=(2047.6,) * 4, frames=50, stops=OBD_STOPS, car_zeros=CAR_ZEROS, edit=None):
    """A boot with GPS fixes while moving, four stops (yaw_stop rows with the car's 0x078 zero), the
    car's yaw with a +0.1 deg/s error and OBD bins (flag bit 4) whose 0x079 zero from stop k on is
    zeros[k] counts on the 2047-based scale; `frames` per second (window 20 ms each)."""
    logs = root / 'logs'
    logs.mkdir(parents=True)
    rows = [dict(kind='boot', schema=1, pid=1, mono_ns=0, boot_id=BOOT)]
    for since in range(100, OBD_END, 10):
        y1, y1n, ystn, gc = [], [], 0, []
        for j in range(10):
            t = since + j
            if stopped(t):
                y1.append(0)
                y1n.append(0)
                ystn += int(RATE)
                continue
            deg = obd_true_rate(t) + CAR_BIAS
            y1.append(int(round(-math.radians(deg) * RATE / pimu_eval.YAW_RAD_PER_COUNT)))
            y1n.append(int(RATE))
            # a fix received 0.2 s into second t describes the whole second t - 1 (delay 1.2 s), so the
            # harness's 1 s bins line up with the fixes and turns leave no partial-second error
            if not stopped(t - 1):
                gc.append((j * 10 + 2) * 1000 + int(round(obd_heading(t - 1))) % 360)
        moving = not all(stopped(since + j) for j in range(10))
        rows.append(dict(kind='log_digest', schema=1, mono_ns=(since + 10) * 10**9, since_ns=since * 10**9, y1=y1,
                         y1n=y1n, yst=0, ystn=ystn, gc=gc, gq=3, gv=50 if moving else 0,
                         speed_kmh_mean=50.0 if moving else 0.0))
    for (a, b), z in zip(stops, car_zeros):
        yn = (b - a) * 10
        rows.append(dict(kind='yaw_stop', schema=1, mono_ns=b * 10**9, boot_s=float(a), dur_ms=(b - a) * 1000,
                         ys=int(round((z - 2048) * yn)), yn=yn, sd=20, first=None, last=None, pre=None, pre_kmh=None))
    (logs / 'trace.0.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in rows))

    def zero_at(t):
        k = 0
        for i, (a, _) in enumerate(OBD_STOPS):
            if t >= a:
                k = i
        return zeros[k]
    bins = {}
    for t in range(100, OBD_END):
        rate_urad = math.radians(obd_true_rate(t)) * 1e6 - (zero_at(t) - 2047) * 658.615
        cov = frames / 50.0
        bins[t] = [frames, int(round(rate_urad * cov)), int(round(1000 * cov)), 0, 16 | (32 if frames < 50 else 0)]
    bins[100][4] |= 8                       # first OBD frame after the source switch
    if edit:
        edit(bins)
    prow = [dict(kind='pimu_boot', schema=1, m=0, boot_id=BOOT, svc=15, budget_bph=1, cap=1)]
    for start in range(100, OBD_END, 10):
        prow.append(dict(kind='pimu', m=start * 1000, s0=0, s1=1, b=0, pt=0, ce=0, bv=0, sk=0, dr=0, fr=0,
                         d=[bins[start + i] for i in range(10)]))
    (logs / 'pimu.0.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in prow))
    return logs


def run_eval(logs, *extra):
    return json.loads(subprocess.run([sys.executable, str(REPO / 'tools/pimu_eval.py'), '--json',
                                      *[str(x) for x in extra], str(logs)],
                                     capture_output=True, text=True, check=True).stdout)


class ObdEval(unittest.TestCase):
    def test_stop_zero_learned_and_compared_with_0x078(self):
        with tempfile.TemporaryDirectory() as tmp:
            zeros = (2047.60, 2047.70, 2047.55, 2047.65)
            report = run_eval(write_obd_trip(Path(tmp), zeros=zeros))
            (boot,) = report['boots']
            stops = boot['obd_stops']
            self.assertEqual(len(stops), 4)
            for st, z, car in zip(stops, zeros, CAR_ZEROS):
                # 30 s stop, 1 s margins; in the first stop bins 100 (frame_reset) and 101 are excluded.
                self.assertGreaterEqual(st['obd_bins'], 26)
                self.assertAlmostEqual(st['zero_obd'], z, delta=0.002)
                self.assertAlmostEqual(st['zero_078'], car, delta=0.01)
                self.assertLess(st['sd_obd'], 0.002)
                self.assertAlmostEqual(st['drift_obd'], 0.0, delta=0.002)
                self.assertAlmostEqual(st['obd_minus_078'], z - car, delta=0.01)
            z = report['obd_zero']
            self.assertEqual(z['stops'], 4)
            self.assertAlmostEqual(z['obd']['range_counts'], 0.15, delta=0.003)
            self.assertAlmostEqual(z['car_078']['range_counts'], 4.0, delta=0.01)
            self.assertAlmostEqual(z['range_ratio_078_over_obd'], 26.7, delta=0.6)
            self.assertAlmostEqual(z['lattice_step_dps'], 1.52751423 * math.degrees(0.000658615), places=4)
            self.assertLessEqual(report['obd_totals']['count_sum_residual_max'], 0.5)

    def test_within_stop_drift_and_spread(self):
        def edit(bins):
            for t in range(401, 430):           # second stop: the zero ramps 2047.0 -> 2048.0
                bins[t][1] = int(round(-(t - 401) / 28.0 * 658.615))
        with tempfile.TemporaryDirectory() as tmp:
            stops = run_eval(write_obd_trip(Path(tmp), edit=edit))['boots'][0]['obd_stops']
            self.assertGreater(stops[1]['drift_obd'], 0.6)
            self.assertGreater(stops[1]['slope_obd_per_min'], 1.5)
            self.assertGreater(stops[1]['sd_obd'], 0.2)

    def test_stop_edges_are_left_out(self):
        """Bins within 1 s of the stop's wheel-zero edges (receipt time, Bluetooth delay) can still turn."""
        def edit(bins):
            for t in (700, 729):
                bins[t][1] += 10**5
        with tempfile.TemporaryDirectory() as tmp:
            st = run_eval(write_obd_trip(Path(tmp), edit=edit))['boots'][0]['obd_stops'][2]
            self.assertEqual(st['obd_bins'], 28)
            self.assertAlmostEqual(st['zero_obd'], 2047.6, delta=0.002)

    def test_short_stop_gives_no_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            stops = run_eval(write_obd_trip(Path(tmp), stops=((100, 130), (400, 406)), car_zeros=CAR_ZEROS[:2]))
            stops = stops['boots'][0]['obd_stops']
            self.assertEqual(stops[1]['obd_bins'], 4)          # 6 s minus the 1 s margins
            self.assertNotIn('zero_obd', stops[1])

    def test_reset_neighbours_and_mixed_bins_are_not_integrated(self):
        def edit(bins):
            bins[415][4] |= 8                    # frame_reset inside the second stop ...
            for t in (414, 415, 416):            # ... its bin and neighbours carry garbage: not in the zero
                bins[t][1] += 10**6
            bins[420][4] |= 4                    # gravity_valid: a mixed second, used for neither source
            bins[420][1] += 10**7
            bins[620][4] |= 8                    # a switch while driving: windows across it get no OBD value
        with tempfile.TemporaryDirectory() as tmp:
            report = run_eval(write_obd_trip(Path(tmp), edit=edit))
            (boot,) = report['boots']
            self.assertAlmostEqual(boot['obd_stops'][1]['zero_obd'], 2047.6, delta=0.002)
            self.assertEqual(boot['obd']['mixed_bins'], 1)
            self.assertEqual(boot['obd']['reset_bins'], 3)
            self.assertEqual(boot['obd_excluded_s'], [100, 101, 414, 415, 416, 619, 620, 621])
            across = [w for w in boot['synthetic']['60'] if w['start_s'] <= 619 and w['start_s'] + w['seconds'] >= 622]
            self.assertTrue(across)
            for w in across:
                self.assertIsNone(w['obd'])
                self.assertIsNotNone(w['car'])
            self.assertGreater(report['synthetic_summary']['60']['obd_missing'], 0)

    def test_heading_error_with_learned_zero_beats_the_biased_car(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = run_eval(write_obd_trip(Path(tmp)))
            straight = report['synthetic_straight_summary']['60']
            turning = report['synthetic_turning_summary']['60']
            self.assertGreaterEqual(straight['obd']['n'], 20)
            self.assertGreaterEqual(turning['obd']['n'], 10)
            # learned zero: only the integer GPS course remains
            self.assertLessEqual(straight['obd']['heading_abs_p90_deg'], 1.0)
            self.assertLessEqual(turning['obd']['heading_abs_p90_deg'], 1.0)
            self.assertGreater(straight['car_on_obd_windows']['heading_abs_median_deg'], 4.0)   # 0.1 deg/s x 60 s
            self.assertEqual(straight['car_on_obd_windows']['n'], straight['obd']['n'])
            # OBD bins are never counted as phone data.
            self.assertEqual(report['synthetic_summary']['60']['phone']['n'], 0)
            self.assertEqual(report['boots'][0]['obd']['bins'], OBD_END - 100)
            w = next(w for w in report['boots'][0]['synthetic']['60'] if w.get('obd'))
            self.assertIn('obd_zero', w)

    def test_batched_seconds_are_not_missing(self):
        """Bluetooth batches: 30 then 70 frames in neighbouring receipt seconds keep every frame and
        all rotation; such windows count (the phone's 40-per-second rule would drop them)."""
        def edit(bins):
            for t in range(131, 399, 2):
                a, b = bins[t], bins[t + 1]
                moved = (a[1] * 2) // 5            # 20 of 50 frames' rotation moves to the next second
                bins[t] = [30, a[1] - moved, 600, 0, a[4]]
                bins[t + 1] = [70, b[1] + moved, 1400, 0, b[4]]
        with tempfile.TemporaryDirectory() as tmp:
            report = run_eval(write_obd_trip(Path(tmp), edit=edit))
            ws = [w for w in report['boots'][0]['synthetic']['60'] if 135 <= w['start_s'] and w['start_s'] + w['seconds'] <= 395]
            self.assertTrue(ws)
            for w in ws:
                self.assertIsNotNone(w['obd'])
                # a pair split by a window edge moves 0.4 s of a 3 deg/s turn across it (1.2 deg)
                self.assertLessEqual(abs(w['obd']['heading_error_deg']), 1.7)

    def test_nominal_zero_error_is_removed_by_the_learned_zero(self):
        """A 0x079 zero 1.6 counts off nominal (0.06 deg/s) would be 3.6 deg per 60 s without learning."""
        with tempfile.TemporaryDirectory() as tmp:
            s = run_eval(write_obd_trip(Path(tmp), zeros=(2048.6,) * 4))['synthetic_straight_summary']['60']
            self.assertLessEqual(s['obd']['heading_abs_p90_deg'], 1.0)

    def test_without_a_previous_stop_there_is_no_obd_value(self):
        """The zero is causal: windows that start before the end of the first stop get no OBD value."""
        with tempfile.TemporaryDirectory() as tmp:
            report = run_eval(write_obd_trip(Path(tmp), stops=OBD_STOPS[2:], car_zeros=CAR_ZEROS[2:]))
            windows = report['boots'][0]['synthetic']['60']
            self.assertTrue(any(w['start_s'] < 700 for w in windows))
            for w in windows:
                if w['start_s'] < 730:
                    self.assertIsNone(w['obd'])
                    self.assertNotIn('obd_zero', w)
            self.assertTrue(any(w['obd'] for w in windows if w['start_s'] >= 730))

    def test_segments_between_stops(self):
        with tempfile.TemporaryDirectory() as tmp:
            segs = run_eval(write_obd_trip(Path(tmp)))['boots'][0]['obd_segments']
            self.assertEqual(len(segs), 3)
            self.assertAlmostEqual(segs[0]['obd_deg'], 90.0, delta=0.05)     # the +90 turn between stops 1 and 2
            self.assertAlmostEqual(segs[1]['obd_deg'], 45.0, delta=0.05)
            self.assertTrue(all(s['obd_complete'] for s in segs))
            # the car's +0.1 deg/s over 270 moving seconds with 2048 as its zero; with the stop's 0x078
            # zero (2046 = -2 counts = -0.075 deg/s while moving) the car's own error changes accordingly
            self.assertAlmostEqual(segs[0]['car_2048_deg'] - segs[0]['obd_deg'], 27.0, delta=1.0)  # y1 is rounded per second
            self.assertAlmostEqual(segs[0]['car_stop_zero_deg'] - segs[0]['car_2048_deg'],
                                   -2 * math.degrees(pimu_eval.YAW_RAD_PER_COUNT) * 270, delta=0.05)

    def test_holes_and_loss_lower_coverage(self):
        def edit(bins):
            for t in range(600, 605):
                bins[t] = [0, 0, 0, 0, 0]       # a stall: empty seconds between OBD bins
        with tempfile.TemporaryDirectory() as tmp:
            obd = run_eval(write_obd_trip(Path(tmp), edit=edit))['obd_totals']
            self.assertEqual(obd['hole_bins'], 5)
            self.assertEqual(obd['bins'], OBD_END - 100)
            self.assertAlmostEqual(obd['coverage'], 1 - 5.0 / (OBD_END - 100), places=4)
        with tempfile.TemporaryDirectory() as tmp:
            report = run_eval(write_obd_trip(Path(tmp), frames=40))
            obd = report['obd_totals']
            # 80 % of the frames: lost rotation is not imputed, so no window reaches the 90 % bound
            self.assertEqual(report['synthetic_summary']['60']['obd']['n'], 0)
            self.assertGreater(report['synthetic_summary']['60']['obd_missing'], 50)
            self.assertAlmostEqual(obd['coverage'], 0.8, places=3)
            self.assertAlmostEqual(obd['frames_per_s'], 40.0)
            self.assertEqual(obd['gap_bin_rate'], 1.0)


class ObdDecision(unittest.TestCase):
    """The pre-registered rule (design note section 15) on authored trips."""

    def decide(self, **kw):
        with tempfile.TemporaryDirectory() as tmp:
            return run_eval(write_obd_trip(Path(tmp), **kw))['obd_decision']

    def test_wire_candidate(self):
        d = self.decide()
        self.assertEqual(d['outcome'], 'wire_candidate', d)
        self.assertEqual({k: v['result'] for k, v in d['checks'].items()},
                         dict(C1_coverage='pass', C2_zero='pass', C3_heading='pass'))

    def test_unstable_zero_is_record_only(self):
        d = self.decide(zeros=(2047.0, 2048.0, 2047.0, 2048.0))
        self.assertEqual(d['outcome'], 'record_only')
        self.assertIn('C2_zero', d['reasons'])

    def test_zero_no_better_than_0x078_is_record_only(self):
        # 0.6 counts is inside the absolute bound but not three times smaller than 0x078's 1.6 counts
        d = self.decide(zeros=(2047.0, 2047.6, 2047.0, 2047.6), car_zeros=(2046.0, 2047.6, 2046.0, 2047.6))
        self.assertEqual(d['outcome'], 'record_only', d)
        self.assertEqual(d['checks']['C2_zero']['relative_test'], False)

    def test_marginal_coverage_is_record_only(self):
        d = self.decide(frames=40)               # 80 %: above the adapter bound, below the wire bound
        self.assertEqual(d['outcome'], 'record_only')
        self.assertIn('C1_coverage', d['reasons'])

    def test_low_coverage_is_adapter_inadequate(self):
        d = self.decide(frames=25)
        self.assertEqual(d['outcome'], 'adapter_inadequate')
        self.assertEqual(d['reasons'], ['coverage 0.5 < 0.60'])

    def test_two_stops_are_not_decidable(self):
        d = self.decide(stops=OBD_STOPS[:2], car_zeros=CAR_ZEROS[:2])
        self.assertEqual(d['outcome'], 'not_decidable')
        self.assertEqual(d['reasons'], ['C2_zero'])

    def test_no_obd_bins_and_the_app_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(run_eval(write_trip(Path(tmp)))['obd_decision']['outcome'], 'no_obd_data')
            app = Path(tmp) / 'diag.txt'
            app.write_text('2026-10-20T10:00:00.000+0900 t=5ms obd_check_end verdict=BAD wall_coverage=0.412 restarts=7 '
                           '| frames=1236 elapsed_s=59.0 fps=20.9 loss_pct=58.2\n')
            report = run_eval(write_trip(Path(tmp) / 'b'), '--app-log', app)
            self.assertEqual(report['app_check'], dict(verdict='BAD', wall_coverage=0.412, restarts=7, frames=1236.0,
                                                       fps=20.9, loss_pct=58.2))
            self.assertEqual(report['obd_decision']['outcome'], 'adapter_inadequate')

class AnalyzerSection(unittest.TestCase):
    def analyze(self, logs):
        r = subprocess.run([sys.executable, str(REPO / 'tools/analyze_logs.py'), '--json', str(logs)],
                           capture_output=True, text=True)
        return json.loads(r.stdout)

    def test_phone_imu_section(self):
        with tempfile.TemporaryDirectory() as tmp:
            logs = write_trip(Path(tmp))
            report = self.analyze(logs)
            imu = report['phone_imu']
            self.assertEqual(imu['data']['frames'], 60 * 10 * 50)
            self.assertEqual(imu['data']['frames_per_s'], 50.0)
            self.assertEqual(imu['data']['seq_loss_fraction'], 0.0)
            self.assertEqual(imu['links'], {'connect': 1})
            self.assertEqual(imu['bt_watch'], ['ready:flag'])
            self.assertEqual(imu['malformed_rows'], 1)          # the cut last line
            self.assertEqual(imu['sync']['pongs'], 3)
            self.assertAlmostEqual(imu['sync']['offset_ms_median'], 123456.799, places=3)
            self.assertAlmostEqual(imu['sync']['clock_drift_ppm'], 2.0, places=3)  # 20 us over 10 s
            # The pimu journal never enters AA trace identity.
            codes = [i['code'] for i in report['issues']]
            self.assertNotIn('unknown_record_kind', codes)

    def test_obd_source_bins(self):
        """Stage 2: bins whose flags carry bit 4 (OBD 0x079 yaw) are summarised separately."""
        with tempfile.TemporaryDirectory() as tmp:
            logs = write_trip(Path(tmp))
            path = logs / 'pimu.0.jsonl'
            rows = path.read_text().splitlines()
            d = [[50, 0, 1000, 0, 16], [45, 0, 900, 0, 48], [50, 0, 1000, 0, 21]]
            rows.insert(1, json.dumps(dict(kind='pimu', m=50000, s0=0, s1=1, b=0, pt=0, ce=0, bv=0, sk=0, dr=0, fr=0, d=d)))
            path.write_text('\n'.join(rows))
            obd = self.analyze(logs)['phone_imu']['obd']
            self.assertEqual(obd['bins'], 3)
            self.assertEqual(obd['frames'], 145)
            self.assertEqual(obd['gap_bins'], 1)
            self.assertEqual(obd['mixed_bins'], 1)
            self.assertAlmostEqual(obd['coverage'], 0.9667, places=4)
            self.assertNotIn('obd', self.analyze(write_trip(Path(tmp) / 'b'))['phone_imu'])

    def test_obd_reset_bins_excluded(self):
        """Bins with frame_reset (flag bit 3) and their neighbours are marked for exclusion."""
        with tempfile.TemporaryDirectory() as tmp:
            logs = write_trip(Path(tmp))
            path = logs / 'pimu.0.jsonl'
            rows = path.read_text().splitlines()
            # 8 bins from m=50000: phone, phone->OBD switch (mixed + reset), OBD x4, OBD->phone
            # (OBD bin, then a phone bin with frame_reset), phone.
            d = [[50, 0, 1000, 0, 4], [50, 0, 1000, 0, 28], [50, 0, 1000, 0, 16], [50, 0, 1000, 0, 16],
                 [50, 0, 1000, 0, 16], [50, 0, 1000, 0, 16], [50, 0, 1000, 0, 16], [50, 0, 1000, 0, 12]]
            rows.insert(1, json.dumps(dict(kind='pimu', m=50000, s0=0, s1=1, b=0, pt=0, ce=0, bv=0, sk=0, dr=0, fr=2, d=d)))
            path.write_text('\n'.join(rows))
            obd = self.analyze(logs)['phone_imu']['obd']
            self.assertEqual(obd['bins'], 6)
            self.assertEqual(obd['reset_bins'], 1)
            # 51000 (reset) and 52000 (after it); 56000 (before the phone reset bin at 57000)
            self.assertEqual(obd['eval_excluded_m'], [51000, 52000, 56000])
            self.assertEqual(obd['eval_excluded_bins'], 3)
            self.assertEqual(obd['eval_bins'], 3)

    def test_reports_without_pimu_files_have_no_section(self):
        with tempfile.TemporaryDirectory() as tmp:
            logs = write_trip(Path(tmp), phone=False)
            self.assertNotIn('phone_imu', self.analyze(logs))


if __name__ == '__main__':
    unittest.main()
