"""Private replay of the 2026-10-10 beta.7 restart from standstill (E_FRAME).

validation/FRAME_REJECT_RESTART_2026-10-10.md. The tunnel-mode episode of the
real drive ended at mono 3358.97 s with core_rejected E_FRAME when the car
started to move (reverse, steering) after a 17 s standstill: the yaw mean
window [3358.861, 3358.963] s (about 0.026 rad/s) was integrated with the last
all-zero wheel sample (3358.859 s) before the first moving wheel sample
(3358.960 s) was drained.

The recorded motion (raw window rows, about 3299..3389 s) is replayed through
the real Pipeline/BetaController with build/replay_beta. The vehicle had no
GPS there (the episode began at 3253 s, outside the raw window), so this test
SYNTHESIZES GPS fixes from the recorded wheel/yaw values at a fictitious
origin to seed BETA, and one forward REVERSE message at the start (the
journal digest shows reverse 0 since 843 s). Everything after the synthetic
GPS loss at 3315.37 s is the recorded motion only.

Private data: set MX5DR_TRIP6_TRACE to the trip's trace.0.jsonl; without it
the test is skipped. No coordinates of the private log are read or written.
Offline MODEL replay, not vehicle evidence.
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
TRACE = os.environ.get("MX5DR_TRIP6_TRACE", "")
YAW_RAD_PER_COUNT = -0.000658615
BETA_ZERO = 2048.0
LAT0, LON0 = 35.0, 135.0          # fictitious origin
START, END = 3299.0, 3372.0       # recorded motion used (s)
FIX_FROM, LOST_AT = 3300.37, 3315.37
# Candidate 1 (recorded ENGAGED->WITHDRAWN core_rejected E_FRAME at 3358.97):
# wheels zero 3341.0-3358.859, yaw window [3358.861, 3358.963] 0.026 rad/s,
# first wheel pulse 3358.960 (held by hold_stopped_yaw). Candidate 2 (never
# reached on the car, the
# episode had ended; the 1.5 s stop is not confirmed in this replay, short by
# about 74 ms): wheels zero 3365.469-3366.993, yaw window
# [3366.996, 3367.100] 0.040 rad/s, first wheel pulse 3367.096.
STOPS = ((3343.0, 3358.96), (3367.0, 3367.09))   # confirmed stop .. first pulse
MOVING = ((3359.3, 3364.2), (3367.4, 3371.5))    # wheels moving
DENSE = ((3357.0, 3360.0), (3365.0, 3368.5))     # 10 Hz mode-0 calls here


def load_events(path):
    boot, epoch, events = None, None, []
    with open(path) as f:
        for line in f:
            if '"kind":"boot"' in line and boot is None:
                boot = json.loads(line).get("boot_id")
            if '"kind":"motion_batch"' not in line:
                continue
            row = json.loads(line)
            for e in row["events"]:
                if START * 1e9 <= e[2] <= END * 1e9:
                    epoch = row["epoch"]
                    events.append(e)
    events.sort(key=lambda e: e[2])
    return boot, epoch, events


def synthetic_fixes(events):
    """1 Hz fixes integrated from the recorded wheel/yaw (BETA zero)."""
    m = 111132.954 - 559.822 * math.cos(2 * math.radians(LAT0))
    p = 111412.84 * math.cos(math.radians(LAT0))
    north = east = 0.0
    heading = math.radians(90.0)
    v = rate = 0.0
    t_prev = START
    fixes, k = [], 0
    targets = [FIX_FROM + i for i in range(int(END - FIX_FROM) + 1)]
    for e in events + [[0, 0, int(END * 1e9) + 1, 0, 0, 0, 0, 0, 0, 0]]:
        t = e[2] / 1e9
        while k < len(targets) and targets[k] <= t:
            dt = targets[k] - t_prev
            mid = heading + rate * dt / 2
            north += v * math.cos(mid) * dt
            east += v * math.sin(mid) * dt
            heading += rate * dt
            t_prev = targets[k]
            fixes.append((targets[k], LAT0 + north / m, LON0 + east / p,
                          math.degrees(heading) % 360, v * 3.6))
            k += 1
        dt = t - t_prev
        if dt > 0:
            mid = heading + rate * dt / 2
            north += v * math.cos(mid) * dt
            east += v * math.sin(mid) * dt
            heading += rate * dt
            t_prev = t
        if e[0] == 1:
            v = sum(e[4:8]) * 0.01 / 4 - 100.0
            v /= 3.6
        elif e[0] == 2 and e[8]:
            rate = (e[4] / e[8] - BETA_ZERO) * YAW_RAD_PER_COUNT
    return fixes


def write_trip(directory, boot, epoch, events):
    ns = lambda t: int(round(t * 1e9))
    first_yaw = next(e for e in events if e[0] == 2)
    # One forward REVERSE message after the first yaw callback (the latched
    # state of the recording); its sequence precedes the recorded one.
    rev_seq = min(e[1] for e in events) - 1
    rows = list(events) + [[3, rev_seq, first_yaw[2] + 50000000, 0, 0, 0, 0, 0, 0, 0]]
    rows.sort(key=lambda e: e[2])
    with open(os.path.join(directory, "trace.0.jsonl"), "w") as f:
        f.write(json.dumps({"kind": "boot", "schema": 1, "pid": 1, "mono_ns": ns(START - 1),
                            "boot_id": boot, "mode": 5, "assist_ready": False},
                           separators=(",", ":")) + "\n")
        for i in range(0, len(rows), 20):
            f.write(json.dumps({"kind": "motion_batch", "schema": 1, "epoch": epoch,
                                "producer_time_status": "unknown", "events": rows[i:i + 20]},
                               separators=(",", ":")) + "\n")
    with open(os.path.join(directory, "collector.0.jsonl"), "w") as f:
        f.write(json.dumps({"stream": "collector", "kind": "collector_boot", "schema": 1,
                            "boot_id": boot}, separators=(",", ":")) + "\n")
        frozen = None
        fixes = synthetic_fixes(events)
        # Extra mode-0 calls at 10 Hz around the restarts sample the
        # interval between the yaw rise and the first wheel pulse.
        dense = [round(a + k * 0.1, 3) for a, b in DENSE for k in range(int(round((b - a) * 10)))]
        rows = sorted([(f[0], 1) + tuple(f[1:]) for f in fixes] +
                      [(t, 0, 0.0, 0.0, 0.0, 0.0) for t in dense if all(abs(t - f[0]) > 0.02 for f in fixes)])
        utc = 0
        for t, real, lat, lon, hdg, kmh in rows:
            if not real and t < LOST_AT:
                continue
            if real:
                utc += 1
            if t >= LOST_AT:
                row = dict(frozen, receipt_ns=ns(t), mode=0)
            else:
                row = {"stream": "collector", "kind": "position_poll", "receipt_ns": ns(t), "mode": 1,
                       "utc_s": 1791638000 + utc, "lat": lat, "lon": lon, "heading": round(hdg, 1),
                       "kmh": round(kmh), "request_provenance": False}
                frozen = row
            f.write(json.dumps(row, separators=(",", ":")) + "\n")


@unittest.skipUnless(os.access(TOOL, os.X_OK), "build/replay_beta not built (make test-replay-beta)")
@unittest.skipUnless(TRACE and os.path.isfile(TRACE), "MX5DR_TRIP6_TRACE unset: private trip replay skipped")
class ReplayRestartFromStandstill(unittest.TestCase):
    def test_restart_after_a_stop_keeps_the_tunnel_episode(self):
        boot, epoch, events = load_events(TRACE)
        self.assertTrue(boot and epoch and len(events) > 1000)
        with tempfile.TemporaryDirectory() as d:
            write_trip(d, boot, epoch, events)
            report = os.path.join(d, "r.json")
            table = os.path.join(d, "r.csv")
            subprocess.run([TOOL, "--trip", d, "--real-only", "--report", report, "--csv", table,
                            "--journal", os.path.join(d, "j.jsonl")],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            with open(report) as f:
                r = json.load(f)
            with open(table) as f:
                calls = [row for row in csv.DictReader(f) if row["window"] == "-1"]
        tr = r["beta_transitions"]
        engaged = [x for x in tr if x["to"] == "ENGAGED"]
        # The episode engages after the synthetic loss, before the stop.
        self.assertTrue(engaged and engaged[0]["t_s"] < 3341.0, tr)
        # No withdrawal at the restart (or anywhere before the end).
        self.assertFalse([x for x in tr if x["to"] == "WITHDRAWN"], tr)
        self.assertGreater(r["real_outages"]["longest_engaged_s"], END - LOST_AT - 5.0, tr)
        # Reported accuracy of every replaced mode-0 call: 25 m during both
        # confirmed stops, including the interval between the yaw rise and
        # the first wheel pulse, 40 m once the wheels move.
        acc = [(float(c["t_s"]), c["accuracy_m"]) for c in calls if int(c["mode"]) == 0]
        for lo, hi in STOPS:
            inside = [a for t, a in acc if lo <= t <= hi]
            self.assertTrue(inside and all(a == "25.000" for a in inside), (lo, hi, inside))
        for lo, hi in MOVING:
            inside = [a for t, a in acc if lo <= t <= hi]
            self.assertTrue(inside and all(a == "40.000" for a in inside), (lo, hi, inside))
        # The yaw-lead intervals were sampled by the dense calls.
        self.assertTrue([t for t, a in acc if 3358.87 <= t <= 3358.95])


if __name__ == "__main__":
    unittest.main()
