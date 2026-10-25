"""Garage carry through the real replay pipeline before this boot has GPS.

Two synthetic boots and a real G3 store; no vehicle/phone effect is claimed.
"""
import csv
import json
import os
import shutil
import subprocess
import tempfile
import unittest

import test_replay_beta as base
import test_replay_g3 as g3


NEXT_BOOT = "00000000-0000-4000-8000-0000000000b8"


def first_speed(t):
    return 0.0 if t >= 120 else g3.parked_start(t)


def nofix_fixture(path, reverse=False, gap=False, identical_heading=None):
    base.write_fixture(path, tunnel=False, speed=g3.parked_start, end=130)
    trace = os.path.join(path, "trace.0.jsonl")
    with open(trace) as f:
        rows = [json.loads(line) for line in f]
    for row in rows:
        if row.get("kind") == "boot":
            row["boot_id"] = NEXT_BOOT
        if "events" in row and gap:
            row["events"] = [e for e in row["events"] if not 115 <= e[2] / 1e9 < 117]
    if reverse:
        events = [e for row in rows for e in row.get("events", [])]
        events.append([3, 0, 115_303_000_000, 0, 0, 0, 0, 0, 0, 1])
        events.sort(key=lambda e: e[2])
        for seq, event in enumerate(events, 1):
            event[1] = seq
        rows = [rows[0]] + [dict(kind="motion_batch", schema=1, epoch=77,
                                producer_time_status="unknown", events=events[i:i+20])
                            for i in range(0, len(events), 20)]
    with open(trace, "w") as f:
        for row in rows:
            if row.get("kind") != "motion_batch" or row["events"]:
                f.write(json.dumps(row, separators=(",", ":")) + "\n")
    positions = os.path.join(path, "collector.0.jsonl")
    with open(positions) as f:
        rows = [json.loads(line) for line in f]
    for row in rows:
        if row.get("kind") == "collector_boot":
            row["boot_id"] = NEXT_BOOT
        elif row.get("kind") == "position_poll":
            row.update(utc_s=0, lat=base.LAT0 + .0027, lon=base.LON0,
                       heading=335, kmh=4)
            if identical_heading is not None:
                row.update(heading=identical_heading,
                           kmh=g3.parked_start(row["receipt_ns"] / 1e9))
    with open(positions, "w") as f:
        for row in rows:
            f.write(json.dumps(row, separators=(",", ":")) + "\n")


@unittest.skipUnless(os.access(base.TOOL, os.X_OK), "build/replay_beta missing")
class G3NoFixReplay(unittest.TestCase):
    def run_replay(self, trip, out, store, garage="on", send="on", extra=()):
        result = subprocess.run(
            [base.TOOL, "--trip", trip, "--real-only", "--g3", "on",
             "--g3-garage", garage, "--g3-send", send, "--g3-store", store,
             "--report", out + ".json", "--csv", out + ".csv",
             "--journal", out + ".jsonl", "--g3-out", out + ".g3.jsonl",
             "--g3-send-out", out + ".sends.jsonl", "--check"] + list(extra),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        with open(out + ".json") as f:
            report = json.load(f)
        with open(out + ".csv") as f:
            rows = list(csv.DictReader(f))
        return dict(report=report, rows=rows, sends=g3.g3_rows(out + ".sends.jsonl"),
                    g3=g3.g3_rows(out + ".g3.jsonl"), journal=g3.read(out + ".jsonl"))

    def run_pair(self, reverse=False, gap=False, garage="on", identity=False):
        with tempfile.TemporaryDirectory() as root:
            first, second, store = [os.path.join(root, name) for name in ("first", "second", "store")]
            for path in (first, second, store):
                os.mkdir(path)
            # The last GPS course is the road's 90 degrees. GPS disappears
            # just before the 100..110 s right turn; no new GPS course can
            # supply the parked direction after that turn.
            base.write_fixture(first, tunnel=True, speed=first_speed, window=(99, 161), end=160)
            previous = self.run_replay(first, os.path.join(root, "previous"), store)
            parked = [r for r in previous["g3"] if r["kind"] == "g3_store" and
                      r.get("event") == "written" and r.get("st") == "S" and r.get("hv") == 1]
            self.assertTrue(parked, previous["g3"])
            self.assertAlmostEqual(parked[-1]["h"] / 10, 180, delta=1)
            with open(os.path.join(store, "g3-heading.v1")) as f:
                stored_heading = int(f.read().split()[6]) / 100
            nofix_fixture(second, reverse=reverse, gap=gap,
                          identical_heading=stored_heading if identity else None)
            results = {}
            for send in ("off", "on"):
                copied = os.path.join(root, "store-" + send)
                shutil.copytree(store, copied)
                results[send] = self.run_replay(second, os.path.join(root, send), copied,
                                                garage=garage, send=send)
            results["parked_heading"] = parked[-1]["h"] / 10
            return results

    def test_previous_boot_heading_turns_without_any_current_boot_gps(self):
        runs = self.run_pair()
        on, off = runs["on"], runs["off"]
        self.assertEqual(on["report"]["input"]["valid_gps_fixes"], 0)
        self.assertTrue(all(r["utc"] == 0 for r in on["sends"]))
        self.assertTrue(any(r.get("reason") == "chain_depart" for r in on["g3"]))
        self.assertEqual(on["journal"], off["journal"])
        held = [r for r in on["sends"] if r["choice"] == 7]
        self.assertGreater(len(held), 20)
        before = next(r for r in held if 98 < r["t_ns"] / 1e9 < 99)
        after = next(r for r in held if 112 < r["t_ns"] / 1e9 < 113)
        self.assertAlmostEqual(before["sent"][1] / 1e6, runs["parked_heading"], delta=.2)
        turn = (after["sent"][1] - before["sent"][1]) / 1e6
        self.assertAlmostEqual(turn, 90, delta=1)
        for a, b in zip(on["rows"], off["rows"]):
            self.assertEqual(a["payload_ok"], "1")
            self.assertEqual(a["speed_mps"], b["speed_mps"])
        self.assertEqual(on["report"]["recorded_classes"]["NO_FIX"]["replaced"], 0)
        self.assertEqual(on["report"]["recorded_classes"]["NO_FIX"]["speed_bearing"], len(held))

    def test_identical_speed_and_bearing_still_count_as_valid_overlay(self):
        runs = self.run_pair(identity=True)
        on = runs["on"]
        identical = [r for r in on["sends"] if r["choice"] == 7 and 35 < r["t_ns"] / 1e9 < 95]
        self.assertGreater(len(identical), 20)
        self.assertTrue(all(r["orig"] == r["sent"] for r in identical))
        held = sum(r["choice"] == 7 for r in on["sends"])
        self.assertEqual(on["report"]["recorded_classes"]["NO_FIX"]["speed_bearing"], held)
        for a, b, original in zip(on["rows"], runs["off"]["rows"], runs["off"]["sends"]):
            if b["choice"] == "0" and a["choice"] == "7":
                # A current zero-wheel sample needs no speed rewrite when
                # the original already says zero; only bearing is added.
                self.assertEqual(original["kmh"], 0)
                self.assertEqual(a["speed_mps"], "0.000")
            else:
                self.assertEqual(a["speed_mps"], b["speed_mps"])

    def test_garage_off_preserves_the_speed_only_sequence(self):
        runs = self.run_pair(garage="off")
        self.assertEqual(runs["on"]["rows"], runs["off"]["rows"])
        self.assertFalse(any(r["choice"] == 7 for r in runs["on"]["sends"]))

    def test_reverse_ends_bearing_and_keeps_wheel_speed(self):
        runs = self.run_pair(reverse=True)
        self.assertTrue(any(r["choice"] == 7 for r in runs["on"]["sends"]))
        after = [r for r in runs["on"]["sends"] if r["t_ns"] / 1e9 >= 116]
        self.assertTrue(after)
        self.assertTrue(all(r["choice"] == 4 for r in after))
        self.assertTrue(any(r.get("event") == "end" and r.get("reason") == "latch"
                            for r in runs["on"]["g3"]))

    def test_sensor_gap_ends_bearing_without_reopening(self):
        runs = self.run_pair(gap=True)
        self.assertTrue(any(r["choice"] == 7 for r in runs["on"]["sends"]))
        after = [r for r in runs["on"]["sends"] if r["t_ns"] / 1e9 >= 118]
        self.assertTrue(after)
        self.assertTrue(all(r["choice"] == 4 for r in after))


if __name__ == "__main__":
    unittest.main()
