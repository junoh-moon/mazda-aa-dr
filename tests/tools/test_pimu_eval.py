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

    def test_reports_without_pimu_files_have_no_section(self):
        with tempfile.TemporaryDirectory() as tmp:
            logs = write_trip(Path(tmp), phone=False)
            self.assertNotIn('phone_imu', self.analyze(logs))


if __name__ == '__main__':
    unittest.main()
