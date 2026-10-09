"""Yaw-zero data rows in tools/analyze_logs.py (validation/YAW_DATA_COLLECTION_2026-10-09.md).

Synthetic rows and digests check validation, the report tables (standstills
with step and time since the previous one, GPS edges, reinits) and the
digest-based driving-zero fit at GPS delays 0 and 1.3 s. An optional smoke
test replays a private recording through the product PersistentLog
(build/yaw_replay) when MX5DR_YAW_REPLAY and MX5DR_YAW_TRIP_DIR are set.
PC-only; not vehicle, phone or DHU evidence; GPS is not ground truth.
"""
import contextlib
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("analyze_logs_yaw", ROOT / "tools" / "analyze_logs.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
K = audit.YAW_RAD_PER_COUNT
S = 1000000000


def boot():
    return dict(kind="boot", schema=1, pid=1, mono_ns=1, mode=5, install="ok", assist_ready=False,
                assist_block="synthetic", wire_timestamp_modified=False, log_profile="persistent")


def stop(boot_s, dur_ms, mean, yn=500, **extra):
    row = dict(kind="yaw_stop", schema=1, mono_ns=int((boot_s + dur_ms / 1000.0 + 5.3) * S), boot_s=boot_s,
               dur_ms=dur_ms, ys=int(round((mean - 2048) * yn)), yn=yn, sd=40, first=-20, last=10, pre=150,
               pre_kmh=12.5)
    row.update(extra)
    return row


def consume(rows):
    a = audit.Auditor()
    for i, row in enumerate(rows):
        a.consume(row, "synthetic:%d" % i)
    return a, a.report()


def codes(report):
    return [i["code"] for i in report["issues"]]


def digest_drive(zero, seconds=120, gps_delay=0.0, rate=50, turn_at=40.0, turn_s=10.0, turn_deg=90.0,
                 kmh=40, hacc=2, since0=100.0):
    """10 s digests of a drive with a constant yaw zero `zero`: straight, one
    turn of turn_deg over turn_s, straight; GPS fixes at 1 Hz whose course is
    the true course gps_delay seconds earlier (integer degrees)."""
    def course(t):   # degrees, clockwise
        if t < turn_at:
            return 10.0
        if t < turn_at + turn_s:
            return 10.0 + turn_deg * (t - turn_at) / turn_s
        return 10.0 + turn_deg
    def raw(t):      # yaw raw count: course rate = -K*(raw - zero)
        r = turn_deg / turn_s if turn_at <= t < turn_at + turn_s else 0.0
        return zero - math.radians(r) / K
    rows = []
    for d in range(seconds // 10):
        since = since0 + 10 * d
        y1, y1n = [], []
        for j in range(10):
            # rate samples per second, midpoint integration per 0.02 s sample
            total = sum(raw(since - since0 + j + (k + 0.5) / rate) - 2048 for k in range(rate))
            y1.append(int(round(total)))
            y1n.append(rate)
        gc = []
        for j in range(10):
            t = since + j + 0.5
            c = int(round(course(t - since0 - gps_delay))) % 360
            gc.append(int(round((t - since) * 10)) * 1000 + c)
        rows.append(dict(kind="log_digest", schema=1, digest="periodic", mono_ns=int((since + 10) * S),
                         since_ns=int(since * S), assist_ready=False, max_receipt_gap_ms=100,
                         y1=y1, y1n=y1n, yst=0, ystn=0, gc=gc, gq=hacc, gv=kmh, dw01=0.1, dw23=-0.05))
    return rows


class YawRows(unittest.TestCase):
    def test_standstill_table_steps_and_time_since_previous(self):
        rows = [boot(), stop(100.0, 12000, 2046.5), stop(130.0, 3000, 2047.25, merged=2, bad=1),
                stop(400.0, 60000, 2051.0, open=1, future_field="tolerated")]
        a, report = consume(rows)
        self.assertNotIn("malformed_yaw_row", codes(report))
        self.assertNotIn("unknown_record_kind", codes(report))
        stops = report["yaw_data"]["stops"]
        self.assertEqual([s["mean"] for s in stops], [2046.5, 2047.25, 2051.0])
        self.assertEqual([s["step"] for s in stops], [None, 0.75, 3.75])
        self.assertEqual([s["since_prev_s"] for s in stops], [None, 18.0, 267.0])
        self.assertEqual((stops[0]["first"], stops[0]["last"], stops[0]["pre"], stops[0]["sd"]),
                         (2047.8, 2048.1, 2049.5, 0.4))
        self.assertEqual((stops[1]["merged"], stops[1]["bad"], stops[2]["open"]), (2, 1, True))
        # pre is null with a reason when the 2 s before were not clean driving.
        _, nulled = consume([boot(), stop(100.0, 2000, 2047.0, pre=None, pre_kmh=None, pre_bad=5)])
        self.assertNotIn("malformed_yaw_row", codes(nulled))
        s = nulled["yaw_data"]["stops"][0]
        self.assertEqual((s["pre"], s["pre_kmh"], s["pre_bad"]), (None, None, 5))
        self.assertEqual(report["yaw_data"]["summary"]["stop_mean_range"], [2046.5, 2051.0])
        # A new boot restarts the step chain.
        _, report = consume(rows[:2] + [boot(), stop(10.0, 2000, 2049.0)])
        self.assertEqual(report["yaw_data"]["stops"][1]["step"], None)

    def test_malformed_rows_are_reported_not_used(self):
        bad = [stop(1.0, 1500, 2048.0, yn=-1), stop(1.0, 1500, 2048.0, first=1.5),
               dict(kind="yaw_edge", schema=1, mono_ns=5, at_s=1.0, ev="sideways", **{"from": "FIX"}),
               dict(kind="yaw_reinit", schema=1, mono_ns=5, at_s=1.0, cause=9, n=1, gap_ms=0, still=0),
               stop(1.0, 1500, 2048.0, pre_bad=-1)]
        _, report = consume([boot()] + bad)
        self.assertEqual(codes(report).count("malformed_yaw_row"), 5)
        self.assertEqual(report["yaw_data"]["summary"]["stops"], 0)
        self.assertEqual(report["issue_counts"].get("violation", 0), 0)

    def test_edges_and_reinits(self):
        edge = dict(kind="yaw_edge", schema=1, mono_ns=20 * S, at_s=14.5, ev="loss", **{"from": "FIX", "to": "LOST"})
        edge.update(yb=150, nb=250, vb=40.0, cb=[90, 95, 40, 5], ya=-50, na=250, va=38.5, ca=None, cut=0)
        ret = dict(edge, at_s=60.0, ev="return", ca=[100, 100, 40, 5], cb=None, cut=1, hist_lost=1,
                   **{"from": "LOST", "to": "FIX"})
        reinit = dict(kind="yaw_reinit", schema=1, mono_ns=90 * S, at_s=70.0, cause=3, n=4, gap_ms=1600, still=1,
                      before=-200, before_age_s=12.5, after=300, after_delay_s=40.0)
        lone = dict(reinit, before=None, before_age_s=None, after=None, after_delay_s=None, cause=1, n=1)
        _, report = consume([boot(), edge, ret, reinit, lone])
        yaw = report["yaw_data"]
        self.assertEqual(len(yaw["edges"]), 2)
        e = yaw["edges"][0]
        self.assertEqual((e["ev"], e["transition"], e["before"]["yaw"], e["after"]["yaw"], e["yaw_step"]),
                         ("loss", "FIX->LOST", 2049.5, 2047.5, -2.0))
        # Course-rate zero: +5 deg in 4 s at mean 2049.5 -> zero = 2049.5 + rate/K.
        self.assertAlmostEqual(e["before"]["course_rate_zero"], 2049.5 + math.radians(5 / 4.0) / K, places=1)
        self.assertIsNone(e["after"]["course_rate_zero"])
        self.assertTrue(yaw["edges"][1]["cut"] and yaw["edges"][1]["history_lost"])
        self.assertFalse(yaw["edges"][0]["history_lost"])
        r = yaw["reinits"]
        self.assertEqual((r[0]["cause"], r[0]["triggers"], r[0]["before"], r[0]["after"], r[0]["step"]),
                         ("gap+invalid", 4, 2046.0, 2051.0, 5.0))
        self.assertEqual((r[1]["cause"], r[1]["before"], r[1]["step"]), ("gap", None, None))

    def test_text_section(self):
        rows = [boot(), stop(100.0, 12000, 2046.5)] + digest_drive(2049.0)
        a, report = consume(rows)
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            audit.print_yaw_data(report["yaw_data"])
        text = buffer.getvalue()
        self.assertIn("Yaw zero data (diagnostic", text)
        self.assertIn("standstills:", text)
        self.assertIn("driving zero fits", text)


class DrivingZeroFit(unittest.TestCase):
    def fit(self, rows):
        _, report = consume([boot()] + rows)
        self.assertNotIn("malformed_digest_yaw", codes(report))
        return report["yaw_data"]

    def test_recovers_a_known_zero_at_the_matching_gps_delay(self):
        for zero in (2045.5, 2048.0, 2051.25):
            yaw = self.fit(digest_drive(zero))
            fits = [f for f in yaw["driving_zero_fits"] if f["lag_0_0"]]
            self.assertGreaterEqual(len(fits), 2)
            for f in fits:
                self.assertLess(abs(f["lag_0_0"]["zero"] - zero), 0.2, (zero, f))
            self.assertAlmostEqual(yaw["samples_per_second"]["1"], 50.0, places=3)
        # GPS courses 1.3 s late: the 1.3 s lag fits the turning window, lag 0 does not.
        yaw = self.fit(digest_drive(2049.0, gps_delay=1.3))
        turning = [f for f in yaw["driving_zero_fits"] if abs(f["turn_deg"]) > 45]
        self.assertTrue(turning)
        for f in turning:
            self.assertLess(abs(f["lag_1_3"]["zero"] - 2049.0), 0.3, f)
            self.assertGreater(abs(f["lag_0_0"]["zero"] - 2049.0), abs(f["lag_1_3"]["zero"] - 2049.0))

    def test_gates_windows_and_malformed_fields(self):
        rows = digest_drive(2048.0)
        # A slow or inaccurate digest splits the windows; none are fitted across it.
        rows[5]["gv"] = 8
        rows[8]["gq"] = 9
        yaw = self.fit(rows)
        for f in yaw["driving_zero_fits"]:
            start = f["start_s"]
            self.assertFalse(start <= 150.0 < start + 10 * f["digests"])
            self.assertFalse(start <= 180.0 < start + 10 * f["digests"])
        broken = digest_drive(2048.0)[:3]
        broken[1]["y1"] = [1, 2, 3]
        broken[2]["gc"] = [1234567]
        _, report = consume([boot()] + broken)
        self.assertEqual(codes(report).count("malformed_digest_yaw"), 2)
        # Digests without the yaw fields (older builds) stay accepted.
        old = [dict(r) for r in digest_drive(2048.0)[:2]]
        for r in old:
            for key in ("y1", "y1n", "yst", "ystn", "gc", "gq", "gv", "dw01", "dw23"):
                del r[key]
        _, report = consume([boot()] + old)
        self.assertNotIn("malformed_digest_yaw", codes(report))
        self.assertEqual(report["yaw_data"]["summary"]["digests"], 0)


def trip_records(trip_dir):
    """Wheel/yaw events and POSITION values of the recording's last boot
    session (older rotated files can hold an earlier boot) in replay input form."""
    files = sorted(Path(trip_dir).rglob("trace.*.jsonl"), key=lambda p: -int(p.name.split(".")[1]))
    events, seen = [], set()
    for path in files:
        with open(path) as stream:
            for line in stream:
                if line.startswith('{"kind":"boot"'):
                    events, seen = [], set()
                    continue
                if '"motion_batch"' in line:
                    row = json.loads(line)
                    for e in row["events"]:
                        key = (row.get("epoch"), e[1])
                        if key in seen:
                            continue
                        seen.add(key)
                        if e[0] == 1:
                            events.append((e[2], "W %d %d %d %d %d" % (e[2], e[4], e[5], e[6], e[7])))
                        elif e[0] == 2:
                            events.append((e[2], "Y %d %d %d" % (e[2], e[4], e[8])))
                elif line.startswith('{"kind":"position"'):
                    row = json.loads(line)
                    if not isinstance(row.get("class"), int):
                        continue
                    key = hash((row.get("lat"), row.get("lon"), row.get("utc_s"))) & 0xFFFFFFFF
                    events.append((row["mono_ns"], "P %d %d %d %d %r %r %r %d" % (
                        row["mono_ns"], row["class"], row.get("mode", 0), row.get("utc_s") or 0,
                        float(row.get("heading") or 0), float(row.get("kmh") or 0),
                        float(row.get("horizontal") or 0), key)))
    events.sort(key=lambda e: e[0])
    if events:
        events.append((events[-1][0] + 1, "E %d" % (events[-1][0] + 1)))
    return "\n".join(e[1] for e in events) + "\n"


@unittest.skipUnless(os.environ.get("MX5DR_YAW_REPLAY") and os.environ.get("MX5DR_YAW_TRIP_DIR"),
                     "private recording smoke: set MX5DR_YAW_REPLAY and MX5DR_YAW_TRIP_DIR")
class RecordingSmoke(unittest.TestCase):
    def test_replayed_recording_yields_valid_yaw_rows(self):
        records = trip_records(os.environ["MX5DR_YAW_TRIP_DIR"])
        result = subprocess.run([os.environ["MX5DR_YAW_REPLAY"]], input=records, capture_output=True,
                                text=True, timeout=600)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()
                if line.startswith('{"kind":"yaw_') or line.startswith('{"kind":"log_digest"')]
        a, report = consume([boot()] + rows)
        self.assertNotIn("malformed_yaw_row", codes(report))
        self.assertNotIn("malformed_digest_yaw", codes(report))
        yaw = report["yaw_data"]
        self.assertGreater(yaw["summary"]["stops"], 0)
        for s in yaw["stops"]:
            self.assertTrue(2000 < s["mean"] < 2100, s)
        print("recording smoke: %s" % json.dumps(yaw["summary"]))


if __name__ == "__main__":
    unittest.main()
