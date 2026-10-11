"""G3 record-only tracker in the BETA replay harness (validation/G3_RECORD_ONLY_2026-10-11.md).

1. The published LOCATION sequence is byte-identical with the tracker off, on
   and with a slow (3 s) store: build/replay_beta runs the same synthetic
   drive (with a 30 s park, so a parked record is written) three times and its
   report, CSV and BETA journal must not differ.
2. Optional private replay: MX5DR_G3_TRIPS="dir:dir:dir" (chronological trace
   directories, e.g. trip2:trip3:trip4) are replayed in order with one store
   directory, so each boot restores the previous boot's parked record; the
   carried and first-course errors are printed. Skipped cleanly when unset.
Synthetic or private receipt-time data only: not vehicle, phone or DHU evidence.
"""
import json
import os
import subprocess
import tempfile
import unittest

import test_replay_beta as base

PARK = (120.0, 150.0)


def speed_with_park(t):
    if PARK[0] <= t < PARK[1]:
        return 0.0
    return base.speed_kmh(t)


def replay(trip, out, g3, store=None):
    args = [base.TOOL, "--trip", trip, "--report", out + ".json", "--csv", out + ".csv",
            "--journal", out + ".jsonl", "--g3", g3, "--g3-out", out + ".g3.jsonl"]
    if store:
        args += ["--g3-store", store]
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True,
                            timeout=600)
    return result


def read(path):
    with open(path, "rb") as f:
        return f.read()


def g3_rows(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


@unittest.skipUnless(os.path.exists(base.TOOL), "build/replay_beta missing")
class G3ReplayIdentical(unittest.TestCase):
    def test_sends_identical_with_tracker_off_on_and_slow_store(self):
        with tempfile.TemporaryDirectory() as trip, tempfile.TemporaryDirectory() as out, \
                tempfile.TemporaryDirectory() as store:
            base.write_fixture(trip, speed=speed_with_park)
            runs = {}
            for mode in ("off", "on", "slow"):
                result = replay(trip, os.path.join(out, mode), mode, store if mode == "on" else None)
                self.assertEqual(result.returncode, 0, result.stderr)
                runs[mode] = tuple(read(os.path.join(out, mode + ext)) for ext in (".json", ".csv", ".jsonl"))
            self.assertEqual(runs["off"], runs["on"])
            self.assertEqual(runs["off"], runs["slow"])
            self.assertEqual(read(os.path.join(out, "off.g3.jsonl")), b"")
            for mode in ("on", "slow"):
                rows = g3_rows(os.path.join(out, mode + ".g3.jsonl"))
                kinds = [(r["kind"], r["event"]) for r in rows]
                self.assertIn(("g3_store", "missing"), kinds)
                self.assertIn(("g3_first", "start"), kinds)
                self.assertIn(("g3_first", "course"), kinds)
                written = [r for r in rows if r["kind"] == "g3_store" and r["event"] == "written"]
                self.assertEqual(len(written), 1, rows)
                self.assertEqual((written[0]["st"], written[0]["hv"]), ("S", 1))
                self.assertGreaterEqual(written[0]["mono_ns"], int(PARK[0] * 1e9) + 15 * 10**9)
                if mode == "slow":
                    self.assertGreaterEqual(written[0]["lat_ms"], 3000.0)
            # The parked record exists and decodes as the next boot would see it.
            with open(os.path.join(store, "g3-heading.v1")) as f:
                text = f.read()
            self.assertTrue(text.startswith("mx5g3 1 1 " + base.BOOT + " S 3 "), text)


TRIPS = [d for d in os.environ.get("MX5DR_G3_TRIPS", "").split(":") if d]


@unittest.skipUnless(TRIPS and os.path.exists(base.TOOL), "MX5DR_G3_TRIPS unset: private G3 replay skipped")
class G3PrivateChain(unittest.TestCase):
    def test_chained_private_boots(self):
        with tempfile.TemporaryDirectory() as out, tempfile.TemporaryDirectory() as store:
            for index, trip in enumerate(TRIPS):
                result = replay(trip, os.path.join(out, str(index)), "on", store)
                self.assertEqual(result.returncode, 0, result.stderr)
                rows = g3_rows(os.path.join(out, "%d.g3.jsonl" % index))
                restore = next(r for r in rows if r["kind"] == "g3_store")
                checks = [r for r in rows if r["kind"] == "g3_store" and r["event"] == "checked"]
                writes = [r for r in rows if r["kind"] == "g3_store" and r["event"] == "written"]
                course = next((r for r in rows if r["kind"] == "g3_first" and r["event"] == "course"), None)
                print("G3 private boot %d: restore %s/%s carried %s; checks %s; course %s err_c %s err_p %s err_s %s "
                      "-> %s; parked writes %d" %
                      (index + 1, restore["event"], restore["reason"],
                       (restore.get("h"), restore.get("unc")),
                       [(c["reason"], c.get("d_m")) for c in checks],
                       None if course is None else course["course"],
                       None if course is None else course["err_c"],
                       None if course is None else course["err_p"],
                       None if course is None else course["err_s"],
                       rows[-1].get("g3"), len(writes)))


if __name__ == "__main__":
    unittest.main()
