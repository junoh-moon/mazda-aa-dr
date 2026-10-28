"""G3 store requests observed at request time in the replay harness.

validation/G3_REPLAY_DIAGNOSIS_2026-10-28.md could not name why parked S
records lacked a heading: the G3 journal shows the request snapshot only at
the completion tick. --g3-request-out writes one replay-only line per taken
request with the BETA seed state, the yaw-chain break causes and the BETA
anchor gate counts at that moment. Synthetic receipt-time data only: not
vehicle, phone or DHU evidence; the product journal is unchanged.
"""
import json
import os
import subprocess
import tempfile
import unittest

import test_replay_beta as base
import test_replay_g3 as g3
import test_replay_g3_nofix as nofix


def run(trip, out, store, extra=()):
    args = [base.TOOL, "--trip", trip, "--real-only", "--g3", "on", "--g3-garage", "on",
            "--g3-send", "off", "--g3-store", store, "--report", out + ".json",
            "--csv", out + ".csv", "--journal", out + ".jsonl", "--g3-out", out + ".g3.jsonl",
            "--check"] + list(extra)
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            universal_newlines=True, timeout=120)
    return result


@unittest.skipUnless(os.access(base.TOOL, os.X_OK), "build/replay_beta missing")
class G3RequestTrace(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def replay(self, trip, name, requests=True):
        store = os.path.join(self.root, name + "-store")
        os.mkdir(store)
        out = os.path.join(self.root, name)
        extra = ["--g3-request-out", out + ".req.jsonl"] if requests else []
        result = run(trip, out, store, extra)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = g3.g3_rows(out + ".req.jsonl") if requests else None
        return dict(rows=rows, journal=g3.read(out + ".jsonl"), g3=g3.read(out + ".g3.jsonl"),
                    csv=g3.read(out + ".csv"))

    def seeded_trip(self):
        trip = os.path.join(self.root, "seeded")
        os.mkdir(trip)
        base.write_fixture(trip, tunnel=True, speed=nofix.first_speed, window=(99, 161), end=160)
        return trip

    def unseeded_trip(self):
        trip = os.path.join(self.root, "unseeded")
        os.mkdir(trip)
        nofix.nofix_fixture(trip)
        return trip

    def test_seeded_park_reports_the_seed_at_request_time(self):
        rows = self.replay(self.seeded_trip(), "seeded")["rows"]
        parked = [r for r in rows if r["target"] == "store" and r["st"] == "S"]
        self.assertTrue(parked, rows)
        last = parked[-1]
        self.assertEqual(last["hv"], 1)
        self.assertTrue(last["beta_ok"])
        self.assertIsNotNone(last["seed_first_s"])
        self.assertLessEqual(last["seed_first_s"], last["t_s"])
        self.assertGreater(last["gates"].get("ACCEPTED", 0), 0)
        for key in ("breaks", "causes", "lost", "seq", "sync"):
            self.assertIn(key, last)

    def test_unseeded_park_says_no_seed_and_no_accepted_anchor(self):
        # Every position of this boot has utc 0: BETA never seeds. Its park
        # is short, so the lazy token is the only request (the S follows it).
        rows = self.replay(self.unseeded_trip(), "unseeded")["rows"]
        self.assertTrue(rows)
        for r in rows:
            self.assertIn(r["hv"], (0, None))
            self.assertFalse(r["beta_ok"])
            self.assertIsNone(r["seed_first_s"])
            self.assertEqual(r["gates"].get("ACCEPTED", 0), 0)
            self.assertGreater(r["gates"].get("BAD_FIX", 0), 0)

    def test_request_trace_does_not_change_replay_outputs(self):
        trip = self.seeded_trip()
        on = self.replay(trip, "with", requests=True)
        off = self.replay(trip, "without", requests=False)
        self.assertEqual(on["journal"], off["journal"])
        self.assertEqual(on["csv"], off["csv"])
        strip = lambda b: [json.loads(x) for x in b.decode().splitlines()]
        drop = lambda rows: [{k: v for k, v in r.items() if k not in ("start_ns", "end_ns", "lat_ms")}
                             for r in rows]
        self.assertEqual(drop(strip(on["g3"])), drop(strip(off["g3"])))


if __name__ == "__main__":
    unittest.main()
