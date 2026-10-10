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


def truth(speed=speed_kmh, end=END):
    """Integrated truth at 10 ms: list of (t, lat, lon, heading_deg, kmh)."""
    out, north, east, heading, dt = [], 0.0, 0.0, math.radians(90), 0.01
    for i in range(int(end / dt) + 1):
        t = i * dt
        m, p = meters_per_degree(LAT0)
        out.append((t, LAT0 + north / m, LON0 + east / p, math.degrees(heading) % 360, speed(t)))
        v = speed(t) / 3.6
        mid = heading + yaw_rate(t) * dt / 2
        north += v * math.cos(mid) * dt
        east += v * math.sin(mid) * dt
        heading += yaw_rate(t) * dt
    return out


def write_fixture(directory, tunnel=True, speed=speed_kmh, window=TUNNEL, end=END):
    states = truth(speed, end)
    ns = lambda t: int(round(t * 1e9))
    with open(os.path.join(directory, "trace.0.jsonl"), "w") as f:
        f.write(json.dumps({"kind": "boot", "schema": 1, "pid": 1, "mono_ns": ns(0.5), "boot_id": BOOT,
                            "mode": 5, "assist_ready": False}, separators=(",", ":")) + "\n")
        events, seq = [], 0
        for k in range(100, int(end * 10) + 1):  # 10 Hz from 10.0 s
            t = k / 10.0 + 0.003
            if k in (120, 121):
                # Change-only REVERSE messages while the yaw stream runs: reverse
                # then forward, so the producer has been seen leaving reverse
                # (BETA_DECISIONS 3.4; a single forward message no longer seeds
                # BETA). A REVERSE queued before the first yaw window would be
                # dropped by the MISSING_SENSOR reset and never latch.
                seq += 1
                events.append([3, seq, ns(t - 0.05), 0, 0, 0, 0, 0, 0, 1 if k == 120 else 0])
            kmh = speed(t)
            seq += 1
            raw = int(round(kmh * 100 + 10000))
            events.append([1, seq, ns(t), 0, raw, raw, raw, raw, 0, 0])
            seq += 1
            # The yaw callback at t closes the mean window that began 100 ms earlier.
            # Straight is the fixed BETA yaw zero 2048 (BETA_DECISIONS 3.5).
            mean = 2048 + yaw_rate(t - 0.05) / YAW_RAD_PER_COUNT
            events.append([2, seq, ns(t + 0.001), 0, int(round(5 * mean)), 0, 0, 0, 5, 0])
        for i in range(0, len(events), 20):
            f.write(json.dumps({"kind": "motion_batch", "schema": 1, "epoch": 77,
                                "producer_time_status": "unknown", "events": events[i:i + 20]},
                               separators=(",", ":")) + "\n")
    with open(os.path.join(directory, "collector.0.jsonl"), "w") as f:
        f.write(json.dumps({"stream": "collector", "kind": "collector_boot", "schema": 1, "boot_id": BOOT},
                           separators=(",", ":")) + "\n")
        frozen = None
        for k in range(2, int(end)):
            t = k + 0.37
            s = states[int(round(t / 0.01))]
            row = {"stream": "collector", "kind": "position_poll", "receipt_ns": ns(t), "mode": 1,
                   "utc_s": 1700000000 + k, "lat": s[1], "lon": s[2], "heading": round(s[3], 1),
                   "kmh": round(s[4]), "request_provenance": False}
            if t < NO_FIX_END:
                # No fix since boot (shadow.5): mode 1, utc_s 0 and a stored
                # stale fix about 300 m away, while the car already moves.
                row.update(utc_s=0, lat=LAT0 + 0.0027, lon=LON0, heading=335, kmh=4)
            elif tunnel and window[0] <= t < window[1]:
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
        # Tunnel mode (v1.0.0-beta.6): the 60 s limit no longer ends an outage;
        # a pseudo outage (at most 60 s) can run into the recorded tunnel, so the
        # longest engaged window is bounded by their union, not by 60 s.
        self.assertGreater(r["longest_engaged_s"], 60.0)
        self.assertLessEqual(r["longest_engaged_s"], 60.0 + (TUNNEL[1] - TUNNEL[0]))
        for row in sends:
            if int(row["window"]) >= 0:
                self.assertGreaterEqual(float(row["entry_kmh"]), 0)
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
        # Send-time classes (BETA_DECISIONS 1-2): NO_FIX (mode 1, utc_s 0)
        # gets only the wheel speed once wheel data exists (from 10 s); the
        # stored position is never replaced; FIX is untouched.
        classes = r["recorded_classes"]
        self.assertEqual(classes["NO_FIX"]["sends"], 28)
        self.assertEqual(classes["NO_FIX"]["replaced"], 0)
        self.assertGreaterEqual(classes["NO_FIX"]["speed_only"], 15)
        self.assertEqual(classes["FIX"]["replaced"] + classes["FIX"]["speed_only"], 0)
        self.assertEqual(classes["LOST"]["sends"], 25)
        transitions = [(t["from"], t["to"]) for t in r["beta_transitions"]]
        self.assertEqual(transitions[:3], [("DISABLED", "ARMED"), ("ARMED", "NO_FIX"),
                                           ("NO_FIX", "SPEED_ENGAGED")])
        self.assertIn(("ARMED", "GPS_LOST"), transitions)
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
        # Tunnel mode (v1.0.0-beta.6): no budget withdrawal; the replacement
        # runs until the GPS return and a return jump is measured and reported.
        self.assertNotIn(("ENGAGED", "WITHDRAWN"), transitions)
        self.assertIn(("ENGAGED", "ARMED"), transitions)
        self.assertIsNotNone(real["return_jump_m"])

    def test_no_replacement_reports_no_return_jump(self):
        # NO_FIX overlays then FIX, no mode 0: a speed-only send before the
        # first fix carries no position, so there is no return jump (it was
        # once reported as n=1 with about 1.2e7 m from unset coordinates).
        trip = os.path.join(self.tmp.name, "no_tunnel")
        os.mkdir(trip)
        write_fixture(trip, tunnel=False)
        report = os.path.join(self.tmp.name, "no_tunnel.json")
        p = subprocess.run([TOOL, "--trip", trip, "--real-only", "--report", report, "--check"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=300)
        self.assertEqual(p.returncode, 0, p.stderr)
        with open(report) as f:
            r = json.load(f)
        self.assertGreater(r["recorded_classes"]["NO_FIX"]["speed_only"], 0)
        self.assertEqual(r["real_outages"]["replaced"], 0)
        self.assertIsNone(r["real_outages"]["return_jump_m"])

    def test_check_fails_on_low_coverage(self):
        # GPS shifted by 3 s (about 30 m at 40 km/h) cannot be covered.
        r, _, err = self.run_tool("--t0", "60,150", "--durations", "20", "--truth-lag-ms", "3000",
                                  "--check", expect=1)
        self.assertEqual(r["check"], "fail")
        self.assertIn("coverage of reported accuracy", err)

    def test_check_detects_corrupted_payload_accuracy_and_mode(self):
        # Since the NO_FIX speed overlay the first changed sends are NO_FIX
        # ones, so a corrupted byte 0 is first reported there.
        for case, text in (("payload", "position replaced on a NO_FIX send (original mode 1)"),
                           ("accuracy", "outside (0,40]"),
                           # a relabelled speed overlay is a FIX send with changed bytes
                           ("mode", "FIX send: bytes outside the fields allowed for this class differ")):
            r, _, err = self.run_tool("--t0", "60", "--durations", "10", "--check",
                                      "--self-test-corrupt", case, expect=1)
            self.assertEqual(r["check"], "fail", case)
            self.assertIn(text, err, case)

    def test_without_valid_gps_window_is_skipped(self):
        r, _, _ = self.run_tool("--t0", "1,210", "--durations", "10", "--check")
        self.assertEqual(r["windows"]["skipped_no_gps"], 2)
        self.assertEqual(r["replaced_with_gps"], 0)

    def test_engagement_policy_comparison_and_heading_journal(self):
        for policy in ("continuous", "legacy", "latest-course"):
            journal = os.path.join(self.tmp.name, "anchors.jsonl")
            r, _, _ = self.run_tool("--engagement", policy, "--real-only", "--journal", journal, "--check")
            self.assertEqual(r["config"]["engagement"], policy)
            with open(journal) as f:
                anchors = [json.loads(line) for line in f if '"kind":"beta_anchor"' in line]
            accepted = [a for a in anchors if a["gate"] == "ACCEPTED"]
            self.assertTrue(accepted)
            # Compact "h":[source code, heading 0.1 deg, uncertainty 0.1 deg(,
            # GPS weight %)] keeps every decision within the persistent log
            # rate budget. Codes: 1 seed, 2 blend, 3 yaw, 4 reverse, 5 resync,
            # 9 legacy (navigation::beta_heading_source_code).
            self.assertTrue(all("domain" not in a for a in anchors))
            self.assertTrue(all(0 <= a["h"][1] < 3600 and a["h"][2] >= 0 for a in accepted))
            self.assertTrue(all(len(a["h"]) == (4 if a["h"][0] == 2 else 3) for a in accepted))
            codes = {a["h"][0] for a in accepted}
            if policy == "legacy":
                self.assertEqual(codes, {9})
            else:
                self.assertTrue(codes <= {1, 2, 3, 4, 5}, codes)
                self.assertIn(1, codes)


# Stopped accuracy (validation/STOPPED_ACCURACY_2026-10-10.md): a 70 s tunnel
# with a 15 s standstill once the honest budget is above 40 m (and below 150 m).
STOP_TUNNEL = (200.0, 270.0)
STOP = (232.0, 247.0)
STOP_END = 300.0


def stop_speed_kmh(t):
    if STOP[0] <= t < STOP[1]:
        return 0.0
    if STOP_TUNNEL[0] <= t < STOP_TUNNEL[1]:
        return 40.0
    return speed_kmh(t) if t < TUNNEL[0] else 40.0


@unittest.skipUnless(os.access(TOOL, os.X_OK), "build/replay_beta not built (make test-replay-beta)")
class ReplayBetaStoppedAccuracy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="mx5dr-replay-stop-")
        cls.trip = os.path.join(cls.tmp.name, "logs")
        os.mkdir(cls.trip)
        write_fixture(cls.trip, speed=stop_speed_kmh, window=STOP_TUNNEL, end=STOP_END)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_tool(self, *args):
        report = os.path.join(self.tmp.name, "report.json")
        rows = os.path.join(self.tmp.name, "sends.csv")
        journal = os.path.join(self.tmp.name, "beta.jsonl")
        p = subprocess.run([TOOL, "--trip", self.trip, "--real-only", "--check", "--report", report, "--csv", rows,
                            "--journal", journal] + list(args),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=300)
        self.assertEqual(p.returncode, 0, p.stderr)
        with open(report) as f:
            data = json.load(f)
        with open(rows) as f:
            replaced = [r for r in csv.DictReader(f) if r["choice"] == "3"]
        with open(journal) as f:
            beta = [b for b in map(json.loads, f) if b.get("kind") in ("beta_state", "beta_summary")]
        return data, replaced, beta

    def phases(self, replaced):
        still = [r for r in replaced if float(r["speed_mps"]) == 0.0]
        before = [r for r in replaced if STOP[0] - 8 <= float(r["t_s"]) < STOP[0]]
        after = [r for r in replaced if STOP[1] + 2 <= float(r["t_s"]) < STOP_TUNNEL[1]]
        self.assertTrue(len(still) >= 12 and before and after, (len(still), len(before), len(after)))
        self.assertTrue(all(STOP[0] <= float(r["t_s"]) < STOP[1] + 1.5 for r in still))
        return still, before, after

    def test_production_profile_reports_25_m_while_stopped_and_40_m_after_the_restart(self):
        # beta_profile_tunnel(): A* 25 m up to an honest budget of 150 m, crawl off.
        r, replaced, beta = self.run_tool()
        self.assertEqual(r["check"], "pass")
        self.assertNotIn("stopped_accuracy", r["config"])
        still, before, after = self.phases(replaced)
        self.assertTrue(all(float(row["accuracy_m"]) == 25.0 for row in still), still)
        self.assertTrue(all(float(row["accuracy_m"]) == 40.0 for row in before + after))
        # acc_rule appears only while the stopped rule held (rule 1 rows are
        # under-reports; the honest budget stays in accuracy_honest_m).
        self.assertTrue(all(b.get("acc_rule", 0) in (0, 1) for b in beta))
        self.assertTrue(all("acc_rule" not in b or b["acc_rule"] != 0 or "acc_h0" in b for b in beta))
        stopped = [b for b in beta if b["kind"] == "beta_summary" and b.get("acc_rule") == 1]
        self.assertGreaterEqual(len(stopped), 10)
        self.assertTrue(all(b["accuracy_m"] == 25 and 40 < b["accuracy_honest_m"] <= 150 for b in stopped))
        self.assertTrue(any(b["kind"] == "beta_summary" and "acc_rule" not in b and b["state"] == "ENGAGED"
                            and b["accuracy_m"] == 40 for b in beta))

    def stop_rows(self, beta):
        return [b for b in beta if b["kind"] == "beta_summary" and "acc_h0" in b]

    def test_stop_decision_is_latched_for_the_whole_stop(self):
        # Review H1: the honest budget grows during the stop; the H_max
        # decision taken at the stop start (journaled as acc_h0) holds.
        _, _, beta = self.run_tool()
        rows = self.stop_rows(beta)
        self.assertGreaterEqual(len(rows), 10)
        h0 = rows[0]["acc_h0"]
        self.assertTrue(all(b["acc_h0"] == h0 and b["acc_rule"] == 1 for b in rows))
        h_end = max(b["accuracy_honest_m"] for b in rows)
        self.assertGreater(h_end, h0 + 2)
        # H_max between the start and the end budget: still 25 m for the whole stop.
        mid = "%.2f" % ((h0 + h_end) / 2)
        _, replaced, beta = self.run_tool("--stopped-honest-max-m", mid)
        still, before, after = self.phases(replaced)
        self.assertTrue(all(float(row["accuracy_m"]) == 25.0 for row in still), still)
        self.assertTrue(any(b["accuracy_honest_m"] > float(mid) and b["accuracy_m"] == 25 for b in self.stop_rows(beta)))
        # H_max just below the start budget: 40 m for the whole stop, acc_rule 0 with acc_h0.
        _, replaced, beta = self.run_tool("--stopped-honest-max-m", "%.2f" % (h0 - 0.05))
        still, before, after = self.phases(replaced)
        self.assertTrue(all(float(row["accuracy_m"]) == 40.0 for row in still))
        rows = self.stop_rows(beta)
        self.assertTrue(rows and all(b["acc_rule"] == 0 and b["accuracy_m"] == 40 for b in rows))

    def test_neutral_profile_keeps_40_m_and_journal_rows(self):
        r, replaced, beta = self.run_tool("--stopped-accuracy-m", "40", "--stopped-honest-max-m", "inf")
        self.assertEqual(r["check"], "pass")
        self.assertEqual(r["config"]["stopped_accuracy"],
                         dict(stopped_accuracy_m=40, stopped_honest_max_m=None, crawl_speed_mps=0))
        still, before, after = self.phases(replaced)
        for row in still + before + after:
            self.assertEqual(float(row["accuracy_m"]), 40.0, row)
        self.assertTrue(beta and all("acc_rule" not in b for b in beta))

    def test_other_stopped_accuracy_is_a_profile_value(self):
        r, replaced, beta = self.run_tool("--stopped-accuracy-m", "20", "--stopped-honest-max-m", "inf")
        still, before, after = self.phases(replaced)
        self.assertTrue(all(float(row["accuracy_m"]) == 20.0 for row in still), still)
        self.assertTrue(all(float(row["accuracy_m"]) == 40.0 for row in before + after))

    def test_honest_max_keeps_40_m_on_a_drifted_stop(self):
        r, replaced, beta = self.run_tool("--stopped-honest-max-m", "35")
        self.assertEqual(r["config"]["stopped_accuracy"]["stopped_honest_max_m"], 35)
        still, before, after = self.phases(replaced)
        self.assertTrue(all(float(row["accuracy_m"]) == 40.0 for row in still + before + after))
        self.assertTrue(all(b.get("acc_rule", 0) == 0 for b in beta))

if __name__ == "__main__":
    unittest.main()
