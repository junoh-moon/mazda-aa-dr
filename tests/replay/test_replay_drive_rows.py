"""Record-only drive data rows in the BETA replay harness
(validation/RELEASE_V100_BETA14_CANDIDATE_2026-10-13.md).

1. The product output is the same with and without --record-out: report,
   CSV, BETA journal, stdout and stderr of build/replay_beta are
   byte-identical on the synthetic tunnel drive of test_replay_beta.
2. The rows have plausible values on that drive: fix1 at 1 Hz while moving
   with the fixture's position, speed, bearing and the straight-road yaw
   zero; one exit_jump at the tunnel exit; yaw_learn at the episode start
   with the fixture's zero (2048) and every 60 s.
Synthetic receipt-time data only: not vehicle, phone or DHU evidence.
"""
import json
import os
import subprocess
import tempfile
import unittest

import test_replay_beta as base


def run(trip, out, record, *extra):
    args = [base.TOOL, "--trip", trip, "--real-only", "--report", out + ".json", "--csv", out + ".csv",
            "--journal", out + ".jsonl"] + list(extra)
    if record:
        args += ["--record-out", out + ".rec.jsonl"]
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600)


def read(path):
    with open(path, "rb") as f:
        return f.read()


def rows(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


@unittest.skipUnless(os.access(base.TOOL, os.X_OK), "build/replay_beta not built (make test-replay-beta)")
class DriveRowsReplay(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.trip = os.path.join(cls.tmp.name, "trip")
        os.mkdir(cls.trip)
        base.write_fixture(cls.trip)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def outputs(self, out, result):
        return (result.stdout, result.stderr) + tuple(read(out + ext) for ext in (".json", ".csv", ".jsonl"))

    def test_product_outputs_identical_with_and_without_rows(self):
        for extra in ((), ("--acc-cap", "off")):
            off = os.path.join(self.tmp.name, "off")
            on = os.path.join(self.tmp.name, "on")
            a, b = run(self.trip, off, False, *extra), run(self.trip, on, True, *extra)
            self.assertEqual(a.returncode, 0, a.stderr)
            self.assertEqual(b.returncode, 0, b.stderr)
            self.assertEqual(self.outputs(off, a), self.outputs(on, b), extra)
            self.assertTrue(rows(on + ".rec.jsonl"))

    def test_rows_on_the_synthetic_tunnel(self):
        out = os.path.join(self.tmp.name, "rec")
        result = run(self.trip, out, True)
        self.assertEqual(result.returncode, 0, result.stderr)
        rec = rows(out + ".rec.jsonl")
        kinds = {}
        for r in rec:
            kinds.setdefault(r["kind"], []).append(r)
        self.assertEqual(set(kinds), {"fix1", "exit_jump", "yaw_learn"})
        # fix1: every new stock fix while moving (utc > 0 from 30 s, the
        # tunnel 200-225 s has mode 0), one per second.
        fix1 = kinds["fix1"]
        t = [r["d"][0] / 1000.0 for r in fix1]
        self.assertTrue(all(30 <= x < base.END for x in t))
        self.assertFalse([x for x in t if base.TUNNEL[0] <= x < base.TUNNEL[1]])
        self.assertEqual(len(fix1), int(base.END) - 30 - int(base.TUNNEL[1] - base.TUNNEL[0]))
        states = base.truth()
        for r in fix1:
            tm, mode, lat, lon, mm_s, brg, acc, sat, window, ysum, yn, wkmh, wn = r["d"]
            s = states[int(round(tm / 1000.0 / 0.01))]
            self.assertEqual((mode, acc, sat), (1, 5000, -1))
            self.assertLess(abs(lat / 1e7 - s[1]) * 111000, 0.5)
            self.assertLess(abs(lon / 1e7 - s[2]) * 91000, 0.5)
            self.assertLess(abs(brg / 1e6 - round(s[3], 1)), 1e-6)
            self.assertEqual(mm_s, round(round(s[4]) / 3.6 * 1000))
            if tm > 31000 and not base.TUNNEL[1] <= tm / 1000.0 < base.TUNNEL[1] + 1:
                # one second of yaw/wheel sums (the first fix after the tunnel covers 26 s)
                self.assertTrue(950 <= window <= 1050 and 45 <= yn <= 55 and 9 <= wn <= 11, r)
                self.assertLess(abs(wkmh / 10.0 - s[4]), 3.0, r)
            if 120000 < tm < 200000:   # straight after the curve: the fixture's zero is 2048
                self.assertEqual(ysum, 0, r)
        # exit_jump: the first fix after the tunnel, the last replaced send 1 s before.
        (e,) = kinds["exit_jump"]
        self.assertEqual((e["ep"], e["mode"], e["dt_ms"]), (1, 1, 1000))
        self.assertAlmostEqual(e["fix_ms"] / 1000.0, base.TUNNEL[1] + 0.37, places=2)
        self.assertGreaterEqual(e["reps"], 20)
        self.assertLess(e["jump_m"], 30.0)
        self.assertLess(abs(e["dbrg"]), 5.0)
        self.assertEqual((e["sent_acc"], e["fix_acc"], e["sent_choice"], e["stock_after"]), (25.0, 5.0, 3, 0))
        self.assertGreater(e["honest"], 25.0)
        self.assertFalse(e["r1"] or e["r2"])
        self.assertTrue(e["r1_acc"] and e["r1_dt"] and not e["r1_dist"])
        # yaw_learn: 60 s cadence while the GPS is valid, and the episode start.
        learn = kinds["yaw_learn"]
        lost = [r for r in learn if r["ev"] == "lost"]
        self.assertEqual(len(lost), 1)
        self.assertAlmostEqual(lost[0]["at_ms"] / 1000.0, base.TUNNEL[0] + 0.37, places=2)
        self.assertEqual((lost[0]["z"], lost[0]["used"], lost[0]["dc"]), (0, 0, 0.0))
        self.assertTrue(lost[0]["span_ms"] >= 40000 and lost[0]["cover"] >= 900 and lost[0]["age_ms"] <= 1000)
        periodic = [r["at_ms"] for r in learn if r["ev"] == "periodic"]
        self.assertGreaterEqual(len(periodic), 3)
        self.assertTrue(all(b - a >= 60000 for a, b in zip(periodic, periodic[1:])))


if __name__ == "__main__":
    unittest.main()
