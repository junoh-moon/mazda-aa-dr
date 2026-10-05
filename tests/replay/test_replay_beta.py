"""BETA replay harness checks on a SYNTHETIC journal (no private trip data).

The fixture is generated here: a known drive (acceleration, straight
cruise, one curve, a 25 s tunnel with a deceleration and GPS mode 0, GPS
return) written as trace.0.jsonl motion_batch rows and collector.0.jsonl
position_poll rows in the vehicle journal format. build/replay_beta then
drives the real Pipeline, BetaController/bridge and adapter with a fake OEM
send. Synthetic receipt-time data only: not vehicle, phone or DHU evidence.
"""
import csv
import json
import math
import os
import subprocess
import tempfile
import unittest

BUILD = os.environ.get("MX5DR_TEST_BUILD", os.path.join(os.path.dirname(__file__), "..", "..", "build"))
TOOL = os.path.join(BUILD, "replay_beta")
BOOT = "00000000-0000-4000-8000-0000000000b7"
YAW_RAD_PER_COUNT = -0.000658615  # research_model_profile()
LAT0, LON0 = 35.0, 135.0
TUNNEL = (200.0, 225.0)
NO_FIX_END = 30.0  # polls before this are NO_FIX (mode 1, utc_s 0, stored fix)
END = 260.0


def speed_kmh(t):
    if t < 5:
        return 0.0
    if t < 15:
        return 4.0 * (t - 5)
    if TUNNEL[0] + 5 <= t < TUNNEL[0] + 15:
        return 40.0 - 1.5 * (t - TUNNEL[0] - 5)  # decelerates 40 -> 25 in the tunnel
    if t >= TUNNEL[0] + 15:
        return 25.0
    return 40.0


def yaw_rate(t):
    # A 90 degree right curve between 100 and 110 s (clockwise is positive).
    return math.pi / 2 / 10 if 100 <= t < 110 else 0.0


def meters_per_degree(lat):
    m = 111132.954 - 559.822 * math.cos(2 * math.radians(lat))
    p = 111412.84 * math.cos(math.radians(lat))
    return m, p


def truth():
    """Integrated truth at 10 ms: list of (t, lat, lon, heading_deg, kmh)."""
    out, north, east, heading, dt = [], 0.0, 0.0, math.radians(90), 0.01
    for i in range(int(END / dt) + 1):
        t = i * dt
        m, p = meters_per_degree(LAT0)
        out.append((t, LAT0 + north / m, LON0 + east / p, math.degrees(heading) % 360, speed_kmh(t)))
        v = speed_kmh(t) / 3.6
        mid = heading + yaw_rate(t) * dt / 2
        north += v * math.cos(mid) * dt
        east += v * math.sin(mid) * dt
        heading += yaw_rate(t) * dt
    return out


def write_fixture(directory):
    states = truth()
    ns = lambda t: int(round(t * 1e9))
    with open(os.path.join(directory, "trace.0.jsonl"), "w") as f:
        f.write(json.dumps({"kind": "boot", "schema": 1, "pid": 1, "mono_ns": ns(0.5), "boot_id": BOOT,
                            "mode": 5, "assist_ready": False}, separators=(",", ":")) + "\n")
        events, seq = [], 0
        for k in range(100, int(END * 10) + 1):  # 10 Hz from 10.0 s
            t = k / 10.0 + 0.003
            if k == 120:
                # One change-only REVERSE (forward) message while the yaw stream
                # runs. A REVERSE queued before the first yaw window would be
                # dropped by the MISSING_SENSOR reset and never latch.
                seq += 1
                events.append([3, seq, ns(t - 0.05), 0, 0, 0, 0, 0, 0, 0])
            kmh = speed_kmh(t)
            seq += 1
            raw = int(round(kmh * 100 + 10000))
            events.append([1, seq, ns(t), 0, raw, raw, raw, raw, 0, 0])
            seq += 1
            # The yaw callback at t closes the mean window that began 100 ms earlier.
            mean = 2047 + yaw_rate(t - 0.05) / YAW_RAD_PER_COUNT
            events.append([2, seq, ns(t + 0.001), 0, int(round(5 * mean)), 0, 0, 0, 5, 0])
        for i in range(0, len(events), 20):
            f.write(json.dumps({"kind": "motion_batch", "schema": 1, "epoch": 77,
                                "producer_time_status": "unknown", "events": events[i:i + 20]},
                               separators=(",", ":")) + "\n")
    with open(os.path.join(directory, "collector.0.jsonl"), "w") as f:
        f.write(json.dumps({"stream": "collector", "kind": "collector_boot", "schema": 1, "boot_id": BOOT},
                           separators=(",", ":")) + "\n")
        frozen = None
        for k in range(2, int(END)):
            t = k + 0.37
            s = states[int(round(t / 0.01))]
            row = {"stream": "collector", "kind": "position_poll", "receipt_ns": ns(t), "mode": 1,
                   "utc_s": 1700000000 + k, "lat": s[1], "lon": s[2], "heading": round(s[3], 1),
                   "kmh": round(s[4]), "request_provenance": False}
            if t < NO_FIX_END:
                # No fix since boot (shadow.5): mode 1, utc_s 0 and a stored
                # stale fix about 300 m away, while the car already moves.
                row.update(utc_s=0, lat=LAT0 + 0.0027, lon=LON0, heading=335, kmh=4)
            elif TUNNEL[0] <= t < TUNNEL[1]:
                row = dict(frozen, receipt_ns=ns(t), mode=0)
            else:
                frozen = row
            f.write(json.dumps(row, separators=(",", ":")) + "\n")


@unittest.skipUnless(os.access(TOOL, os.X_OK), "build/replay_beta not built (make test-replay-beta)")
class ReplayBetaSynthetic(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="mx5dr-replay-beta-")
        cls.trip = os.path.join(cls.tmp.name, "logs")
        os.mkdir(cls.trip)
        write_fixture(cls.trip)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_tool(self, *args, expect=0):
        report = os.path.join(self.tmp.name, "report.json")
        rows = os.path.join(self.tmp.name, "sends.csv")
        p = subprocess.run([TOOL, "--trip", self.trip, "--report", report, "--csv", rows] + list(args),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=300)
        self.assertEqual(p.returncode, expect, p.stderr)
        with open(report) as f:
            data = json.load(f)
        with open(rows) as f:
            sends = list(csv.DictReader(f))
        return data, sends, p.stderr

    def test_pseudo_outage_sweep_passes_check(self):
        r, sends, _ = self.run_tool("--sweep", "20:190:5", "--durations", "10,30,60", "--check")
        self.assertEqual(r["check"], "pass")
        self.assertEqual(r["windows"]["crashed"], 0)
        self.assertGreaterEqual(r["windows"]["engaged"], 40)
        self.assertGreater(r["replaced_with_gps"], 300)
        self.assertGreaterEqual(r["coverage"], 0.95)
        self.assertLess(r["position_error_m"]["all"]["p90"], 10.0)
        # G1: the replaced speed is the wheel speed (scale error only).
        self.assertLess(r["speed_error_vs_wheel_mps"]["max"], 0.6)
        # G2: travel bearing from the anchor course and yaw.
        self.assertLess(r["bearing_error_deg"]["all"]["p90"], 3.0)
        self.assertLessEqual(r["longest_engaged_s"], 60.0)
        for row in sends:
            if row["choice"] == "3":
                self.assertEqual(row["mode"], "0")
                self.assertEqual(row["payload_ok"], "1")
                self.assertTrue(0 < float(row["accuracy_m"]) <= 40.0)
        # The first mode-0 send of every outage passes the original: the
        # adapter revokes the generation at the mode change.
        self.assertEqual(r["first_replacement_after_outage_start_s"]["max"], 1.0)

    def test_real_tunnel_follows_wheel_speed_and_stops_at_gps_return(self):
        r, sends, _ = self.run_tool("--real-only", "--check")
        self.assertEqual(r["check"], "pass")
        # Send-time classes: the current code changes nothing outside LOST
        # (mode 0); NO_FIX (mode 1, utc_s 0) passes the original.
        classes = r["recorded_classes"]
        self.assertEqual(classes["NO_FIX"]["sends"], 28)
        self.assertEqual(classes["NO_FIX"]["speed_only"] + classes["NO_FIX"]["replaced"], 0)
        self.assertEqual(classes["FIX"]["replaced"] + classes["FIX"]["speed_only"], 0)
        self.assertEqual(classes["LOST"]["sends"], 25)
        self.assertEqual([(t["from"], t["to"]) for t in r["beta_transitions"]][:2],
                         [("DISABLED", "ARMED"), ("ARMED", "GPS_LOST")])
        self.assertGreater(r["reverse_latch"]["moving_latched_s"], 200)
        real = r["real_outages"]
        self.assertEqual(real["runs"], 1)
        self.assertGreater(real["replaced"], 5)
        self.assertGreater(real["longest_engaged_s"], 5.0)
        replaced = [row for row in sends if row["choice"] == "3"]
        self.assertTrue(all(TUNNEL[0] <= float(row["t_s"]) < TUNNEL[1] for row in replaced))
        # Speed changes inside the tunnel reach the replaced LOCATION (G1).
        speed = {int(float(row["t_s"])): float(row["speed_mps"]) for row in replaced}
        early = [v for t, v in speed.items() if t <= TUNNEL[0] + 5]
        late = [v for t, v in speed.items() if t >= TUNNEL[0] + 15]
        self.assertTrue(early and late, speed)
        self.assertGreater(min(early) - max(late), 3.0)
        for row in replaced:
            self.assertLess(abs(float(row["speed_mps"]) - float(row["wheel_mps"])), 0.6, row)
        after = [row for row in sends if float(row["t_s"]) >= TUNNEL[1]]
        self.assertTrue(after and all(row["choice"] == "0" for row in after))

    def test_check_fails_on_low_coverage(self):
        # GPS shifted by 3 s (about 30 m at 40 km/h) cannot be covered.
        r, _, err = self.run_tool("--t0", "60,150", "--durations", "20", "--truth-lag-ms", "3000",
                                  "--check", expect=1)
        self.assertEqual(r["check"], "fail")
        self.assertIn("coverage of reported accuracy", err)

    def test_check_detects_corrupted_payload_accuracy_and_mode(self):
        for case, text in (("payload", "LOST send: bytes outside the fields allowed for this class differ"),
                           ("accuracy", "outside (0,40]"),
                           ("mode", "position replaced on a FIX send (original mode 1)")):
            r, _, err = self.run_tool("--t0", "60", "--durations", "10", "--check",
                                      "--self-test-corrupt", case, expect=1)
            self.assertEqual(r["check"], "fail", case)
            self.assertIn(text, err, case)

    def test_without_valid_gps_window_is_skipped(self):
        r, _, _ = self.run_tool("--t0", "1,210", "--durations", "10", "--check")
        self.assertEqual(r["windows"]["skipped_no_gps"], 2)
        self.assertEqual(r["replaced_with_gps"], 0)


if __name__ == "__main__":
    unittest.main()
