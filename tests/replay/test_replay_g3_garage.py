"""G3 garage carry in the BETA replay harness (validation/G3_GARAGE_EXIT_FIX_2026-10-17.md).

A SYNTHETIC two-boot garage chain in the vehicle journal format:
  boot A drives outdoors with GPS, loses GPS at a garage entry, spirals down
  (about 516 deg of rotation), parks for 18 s and the recording ends (sensors
  stop); boot B starts parked without any wall clock (replay has none), spirals
  up for 30 s (about -600 deg) without GPS (NO_FIX, the stale stored fix) and
  reaches its first GPS fix at the exit.
build/replay_beta runs the real Pipeline, BetaController, G3 tracker, G3 send
window and adapter, with one store directory across both boots.
  --g3-garage on : boot A stores its final garage park (BETA replacing, no
                   fsync), boot B restores it as CARRIED/chain_depart and the
                   first FIX LOCATIONs at the exit carry the integrated heading.
  --g3-garage off: beta.16 rules (no store while BETA replaces; nothing carried,
                   no overlay). The --g3-send-out record holds only the send's
                   bytes 40 and 44..47 (stock and sent) and the decoded fields,
                   so this test shows that the off run sends the stock bearing
                   and that the on run changes the bearing only in overlaid
                   sends; that the OTHER payload bytes are unchanged is shown by
                   the adapter test (test_adapter beta_bearing) and the worker
                   payload comparison (tests/runtime/g3_send_identity.py).
Synthetic receipt-time data only: not vehicle, phone or DHU evidence.
"""
import json
import math
import os
import subprocess
import tempfile
import unittest

import test_replay_beta as base

BOOT_A = "00000000-0000-4000-8000-00000000a001"
BOOT_B = "00000000-0000-4000-8000-00000000b002"
BOOT_X = "00000000-0000-4000-8000-00000000c003"   # a product boot between A and B ("gap" run)
A_LOST = 104.0          # garage entry (GPS mode 0 from here)
A_END = 150.0
B_EXIT = 130.0          # first GPS fix of boot B
B_END = 230.0


def speed_a(t):
    if t < 15:
        return 0.0
    if t < 25:
        return 4.0 * (t - 15)
    if t < 100:
        return 40.0
    if t < 106:
        return 40.0 - 5.0 * (t - 100)
    if t < 130:
        return 10.0
    if t < 132:
        return 10.0 - 5.0 * (t - 130)
    return 0.0


def yaw_a(t):
    if 60 <= t < 70:
        return math.pi / 2 / 10          # 90 deg right curve outdoors
    if 108 <= t < 128:
        return 0.45                      # garage spiral down, clockwise
    return 0.0


def speed_b(t):
    if t < 40:
        return 0.0
    if t < 41:
        return 10.0 * (t - 40)
    if t < 120:
        return 10.0
    if t < 125:
        return 10.0 + 2.0 * (t - 120)
    return 20.0


def yaw_b(t):
    if 50 <= t < 80:
        return -0.35                     # garage spiral up, counter-clockwise
    if 90 <= t < 96:
        return math.pi / 4 / 6           # a 45 deg right turn before the exit
    return 0.0


def integrate(speed, yaw, heading0, end, lat0=base.LAT0, lon0=base.LON0):
    """Truth at 10 ms: list of (t, lat, lon, heading_deg, kmh)."""
    out, north, east, heading, dt = [], 0.0, 0.0, math.radians(heading0), 0.01
    for i in range(int(end / dt) + 1):
        t = i * dt
        m, p = base.meters_per_degree(base.LAT0)
        out.append((t, lat0 + north / m, lon0 + east / p, math.degrees(heading) % 360, speed(t)))
        v = speed(t) / 3.6
        mid = heading + yaw(t) * dt / 2
        north += v * math.cos(mid) * dt
        east += v * math.sin(mid) * dt
        heading += yaw(t) * dt
    return out


def write_trip(directory, boot, speed, yaw, heading0, end, fix_from, lost_from, reverse_at=(),
               lat0=base.LAT0, lon0=base.LON0, utc0=1700000000):
    states = integrate(speed, yaw, heading0, end, lat0, lon0)
    ns = lambda t: int(round(t * 1e9))
    with open(os.path.join(directory, "trace.0.jsonl"), "w") as f:
        f.write(json.dumps({"kind": "boot", "schema": 1, "pid": 1, "mono_ns": ns(0.5), "boot_id": boot,
                            "mode": 5, "assist_ready": False}, separators=(",", ":")) + "\n")
        events, seq = [], 0
        for k in range(100, int(end * 10) + 1):
            t = k / 10.0 + 0.003
            for at, value in reverse_at:
                if k == int(round(at * 10)):
                    seq += 1
                    events.append([3, seq, ns(t - 0.05), 0, 0, 0, 0, 0, 0, value])
            raw = int(round(speed(t) * 100 + 10000))
            seq += 1
            events.append([1, seq, ns(t), 0, raw, raw, raw, raw, 0, 0])
            seq += 1
            mean = 2048 + yaw(t - 0.05) / base.YAW_RAD_PER_COUNT
            events.append([2, seq, ns(t + 0.001), 0, int(round(5 * mean)), 0, 0, 0, 5, 0])
        for i in range(0, len(events), 20):
            f.write(json.dumps({"kind": "motion_batch", "schema": 1, "epoch": 77,
                                "producer_time_status": "unknown", "events": events[i:i + 20]},
                               separators=(",", ":")) + "\n")
    with open(os.path.join(directory, "collector.0.jsonl"), "w") as f:
        f.write(json.dumps({"stream": "collector", "kind": "collector_boot", "schema": 1, "boot_id": boot},
                           separators=(",", ":")) + "\n")
        frozen = None
        for k in range(2, int(end)):
            t = k + 0.37
            s = states[int(round(t / 0.01))]
            row = {"stream": "collector", "kind": "position_poll", "receipt_ns": ns(t), "mode": 1,
                   "utc_s": utc0 + k, "lat": s[1], "lon": s[2], "heading": round(s[3], 1),
                   "kmh": round(s[4]), "request_provenance": False}
            if t < fix_from:
                row.update(utc_s=0, lat=lat0 + 0.0027, lon=lon0, heading=335, kmh=4)
            elif lost_from is not None and t >= lost_from:
                row = dict(frozen, receipt_ns=ns(t), mode=0)
            else:
                frozen = row
            f.write(json.dumps(row, separators=(",", ":")) + "\n")
    return states


def rows(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def wrap180(d):
    return (d + 180.0) % 360.0 - 180.0


@unittest.skipUnless(os.access(base.TOOL, os.X_OK), "build/replay_beta not built")
class G3GarageChainReplay(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="mx5dr-g3-garage-")
        root = cls.tmp.name
        cls.trip_a = os.path.join(root, "a")
        cls.trip_b = os.path.join(root, "b")
        os.mkdir(cls.trip_a)
        os.mkdir(cls.trip_b)
        states_a = write_trip(cls.trip_a, BOOT_A, speed_a, yaw_a, 90.0, A_END, 0.0, A_LOST,
                              reverse_at=((12.0, 1), (12.1, 0)))
        cls.park_heading = states_a[-1][3]
        # Boot B starts where boot A parked, one hour later (GPS time).
        cls.states_b = write_trip(cls.trip_b, BOOT_B, speed_b, yaw_b, cls.park_heading, B_END, B_EXIT, None,
                                  lat0=states_a[-1][1], lon0=states_a[-1][2], utc0=1700003600)
        cls.runs = {}
        # gap: garage on, but another product boot (BOOT_X) started between A
        # and B and wrote no record (review P1: its token replaces A's).
        # lag: garage on with a journal writer lag of 150 ms at every turn
        # (--writer-lag-ms; third review C3): the departure mark while BETA
        # replaces waits (> 100 ms), the parked S is not held (idle rule 300 ms).
        for mode in ("on", "off", "gap", "lag"):
            store = os.path.join(root, "store-" + mode)
            os.mkdir(store)
            out = {}
            for name, trip in (("a", cls.trip_a), ("b", cls.trip_b)):
                if mode == "gap" and name == "b":
                    with open(os.path.join(store, "g3-last-start"), "w") as f:
                        f.write("mx5g3start 1 %s\n" % BOOT_X)
                prefix = os.path.join(root, mode + "-" + name)
                p = subprocess.run(
                    [base.TOOL, "--trip", trip, "--real-only", "--report", prefix + ".json", "--csv", prefix + ".csv",
                     "--journal", prefix + ".jsonl", "--g3", "on", "--g3-store", store,
                     "--g3-garage", "off" if mode == "off" else "on",
                     "--g3-send", "on", "--g3-out", prefix + ".g3.jsonl", "--g3-send-out", prefix + ".sends.jsonl"] +
                    (["--writer-lag-ms", "150"] if mode == "lag" else []),
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=600)
                assert p.returncode == 0, p.stderr
                with open(prefix + ".jsonl", "rb") as f:
                    journal = f.read()
                out[name] = dict(g3=rows(prefix + ".g3.jsonl"), sends=rows(prefix + ".sends.jsonl"), journal=journal)
                with open(os.path.join(store, "g3-heading.v1")) if os.path.exists(
                        os.path.join(store, "g3-heading.v1")) else open(os.devnull) as f:
                    out[name]["store"] = f.read()
                with open(os.path.join(store, "g3-last-start")) as f:
                    out[name]["token"] = f.read()
            cls.runs[mode] = out

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def truth_b(self, t_ns):
        return self.states_b[int(round(t_ns / 1e9 / 0.01))][3]

    def test_on_stores_the_final_garage_park_without_fsync(self):
        a = self.runs["on"]["a"]
        written = [r for r in a["g3"] if r["kind"] == "g3_store" and r["event"] == "written"]
        self.assertTrue(written, a["g3"])
        last = written[-1]
        self.assertEqual((last["st"], last["hv"], last["sync"]), ("S", 1, 0))
        self.assertGreaterEqual(last["mono_ns"], int(137e9))          # 5 s after the stop at 132 s
        fields = a["store"].split()
        self.assertEqual((fields[4], fields[5]), ("S", "7"))          # heading | position | departure
        stored = int(fields[6]) / 100.0
        self.assertLess(abs(wrap180(stored - self.park_heading)), 3.0, (stored, self.park_heading))
        self.assertLessEqual(int(fields[7]), 6000)

    def test_on_restores_chain_depart_and_overlays_the_exit_fixes(self):
        b = self.runs["on"]["b"]
        loaded = [r for r in b["g3"] if r["kind"] == "g3_store" and r["event"] == "loaded"]
        self.assertEqual([(r["g3"], r["reason"], r["prev"]) for r in loaded], [("CARRIED", "chain_depart", BOOT_A[:8])])
        self.assertEqual((self.runs["on"]["a"]["token"], b["token"]),
                         ("mx5g3start 1 %s\n" % BOOT_A, "mx5g3start 1 %s\n" % BOOT_B))
        start = [r for r in b["g3"] if r["kind"] == "g3_send" and r["event"] == "start"]
        self.assertEqual(len(start), 1)
        self.assertEqual((start[0]["garage"], start[0]["rev"]), (1, -1))
        overlaid = [s for s in b["sends"] if s["choice"] == 6]
        self.assertGreaterEqual(len(overlaid), 1, b["g3"])
        for s in overlaid:
            self.assertGreaterEqual(s["t_ns"], int(B_EXIT * 1e9))
            self.assertEqual((s["mode"], s["sent"][0]), (1, 1))
            err = wrap180(s["sent"][1] / 1e6 - self.truth_b(s["t_ns"]))
            self.assertLess(abs(err), 5.0, (s, self.truth_b(s["t_ns"])))
        # The first two FIX sends after the exit pass the original (BETA leaves
        # SPEED_ENGAGED at the first FIX and the window needs a FIX POSITION of
        # the current generation, as in beta.15); the third is overlaid.
        fixes = [s for s in b["sends"] if s["mode"] == 1 and s["utc"] > 0]
        self.assertEqual([s["choice"] for s in fixes[:3]], [0, 0, 6])
        end = [r for r in b["g3"] if r["kind"] == "g3_send" and r["event"] == "end"]
        self.assertEqual(len(end), 1)
        self.assertIn(end[0]["reason"], ("trusted",))
        # Departure mark: the restored S record was replaced by M when boot B left.
        marks = [r for r in b["g3"] if r["kind"] == "g3_store" and r["event"] == "written" and r["st"] == "M"]
        self.assertEqual(len(marks), 1)
        self.assertGreaterEqual(marks[0]["mono_ns"], int(40e9))
        self.assertEqual(b["store"].split()[4], "M")

    def test_off_is_beta16(self):
        a, b = self.runs["off"]["a"], self.runs["off"]["b"]
        self.assertFalse([r for r in a["g3"] if r["kind"] == "g3_store" and r["event"] == "written"])
        self.assertEqual([(r["event"], r["reason"]) for r in b["g3"] if r["kind"] == "g3_store"][:1],
                         [("missing", "missing")])
        self.assertFalse([s for s in b["sends"] if s["choice"] == 6])
        self.assertFalse([r for r in b["g3"] if r["kind"] == "g3_send" and r["event"] == "start"])

    def test_gap_boot_refuses_the_carry(self):
        # Review P1: the record of boot A is not carried when another product
        # boot started in between (it might have moved the car unmarked).
        b = self.runs["gap"]["b"]
        loaded = [r for r in b["g3"] if r["kind"] == "g3_store" and r["event"] == "loaded"]
        self.assertEqual([(r["g3"], r["reason"], r["prev"]) for r in loaded],
                         [("CARRIED_UNVERIFIED", "not_prev_boot", BOOT_X[:8])])
        self.assertFalse([s for s in b["sends"] if s["choice"] == 6])
        self.assertFalse([r for r in b["g3"] if r["kind"] == "g3_send" and r["event"] == "start"])
        self.assertEqual(self.runs["gap"]["a"]["store"], self.runs["on"]["a"]["store"])

    def test_writer_lag_holds_the_mark_while_replacing(self):
        # --writer-lag-ms 150: boot A still stores its final garage park (the
        # idle rule allows up to 300 ms); boot B's departure mark waits while
        # BETA replaces (NO_FIX/SPEED_ENGAGED in the garage) and lands only
        # once BETA stops replacing at the exit; the sends are those of "on".
        on, lag = self.runs["on"], self.runs["lag"]
        self.assertEqual(lag["a"]["store"], on["a"]["store"])
        marks = [r for r in lag["b"]["g3"] if r["kind"] == "g3_store" and r["event"] == "written" and r["st"] == "M"]
        on_marks = [r for r in on["b"]["g3"] if r["kind"] == "g3_store" and r["event"] == "written" and r["st"] == "M"]
        self.assertEqual((len(marks), len(on_marks)), (1, 1))
        self.assertLess(on_marks[0]["mono_ns"], int(45e9))
        self.assertGreaterEqual(marks[0]["mono_ns"], int(B_EXIT * 1e9))
        self.assertEqual(lag["b"]["store"].split()[4], "M")
        self.assertEqual(lag["b"]["sends"], on["b"]["sends"])

    def test_sends_differ_only_in_the_bearing_of_overlaid_fixes(self):
        # Bytes 40 and 44..47 only (the record holds no other payload byte).
        for name in ("a", "b"):
            on, off = self.runs["on"][name], self.runs["off"][name]
            self.assertEqual(on["journal"], off["journal"])           # BETA rows unchanged
            self.assertEqual(len(on["sends"]), len(off["sends"]))
            for x, y in zip(on["sends"], off["sends"]):
                self.assertEqual(x["orig"], y["orig"])
                if x["choice"] == 6:
                    self.assertEqual(y["sent"], y["orig"])
                else:
                    self.assertEqual(x["sent"], y["sent"])


if __name__ == "__main__":
    unittest.main()
