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
import zlib

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


def carried_record(heading_cdeg, rt):
    """A parked record of an earlier boot at the fixture's start (the
    g3_heading.h text format, CRC32 IEEE = zlib.crc32)."""
    body = ("mx5g3 1 3 aaaaaaaa-1111-2222-3333-444444444444 S 3 %d 450 %d %d 50 0 %d 1000 none" %
            (heading_cdeg, round(base.LAT0 * 1e7), round(base.LON0 * 1e7), rt))
    return body + " %08x\n" % zlib.crc32(body.encode())


def parked_start(t):
    """Parked until 20 s (the motion stream starts at 10 s with zero wheels),
    then away east to 40 km/h; the first fix (utc > 0) comes at 30 s."""
    if t < 20:
        return 0.0
    return min(40.0, 4.0 * (t - 20))


@unittest.skipUnless(os.path.exists(base.TOOL), "build/replay_beta missing")
class G3BearingSendReplay(unittest.TestCase):
    """--g3-send on (validation/G3_SEND_2026-10-13.md) on the synthetic drive
    with a restored CARRIED record: only the bearing of FIX sends inside the
    window changes; everything else equals --g3-send off."""

    def run_pair(self, heading_cdeg):
        out_rows = {}
        with tempfile.TemporaryDirectory() as trip, tempfile.TemporaryDirectory() as out:
            base.write_fixture(trip, speed=parked_start)
            rt = 1760000000
            for mode in ("off", "on"):
                with tempfile.TemporaryDirectory() as store:
                    with open(os.path.join(store, "g3-heading.v1"), "w") as f:
                        f.write(carried_record(heading_cdeg, rt - 600))
                    prefix = os.path.join(out, mode)
                    result = subprocess.run(
                        [base.TOOL, "--trip", trip, "--report", prefix + ".json", "--csv", prefix + ".csv",
                         "--journal", prefix + ".jsonl", "--g3", "on", "--g3-store", store, "--g3-rt", str(rt),
                         "--g3-out", prefix + ".g3.jsonl", "--g3-send", mode, "--g3-send-out", prefix + ".sends.jsonl",
                         "--real-only", "--check"],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=600)
                    self.assertEqual(result.returncode, 0, result.stderr)
                out_rows[mode] = dict(journal=read(prefix + ".jsonl"), g3=g3_rows(prefix + ".g3.jsonl"),
                                      sends=g3_rows(prefix + ".sends.jsonl"), csv=read(prefix + ".csv"))
        return out_rows

    def test_on_differs_only_in_bearing(self):
        runs = self.run_pair(9000)
        on, off = runs["on"], runs["off"]
        self.assertEqual(on["journal"], off["journal"])                      # BETA rows unchanged
        self.assertEqual([r for r in on["g3"] if not r["kind"].startswith("g3_send")], off["g3"])
        self.assertFalse([r for r in off["g3"] if r["kind"].startswith("g3_send")])
        self.assertEqual(len(on["sends"]), len(off["sends"]))
        overlaid = 0
        for a, b in zip(on["sends"], off["sends"]):
            self.assertEqual(a["orig"], b["orig"])
            if a["choice"] == 6:
                overlaid += 1
                self.assertEqual((a["sent"][0], a["sent"][1]), (1, 90000000))
                self.assertEqual(a["mode"], 1)
                self.assertGreater(a["utc"], 0)
            else:
                self.assertEqual(a["sent"], b["sent"])
            self.assertEqual(b["choice"] if b["choice"] != 6 else None, b["choice"])
        g3send = [r for r in on["g3"] if r["kind"] == "g3_send"]
        events = [r["event"] for r in g3send]
        self.assertEqual(events[0], "start")
        end = [r for r in g3send if r["event"] == "end"]
        self.assertEqual(len(end), 1)
        self.assertEqual(end[0]["reason"], "trusted")
        self.assertGreaterEqual(overlaid, 1)
        # Never after the window: no overlaid send later than the end row.
        self.assertTrue(all(a["t_ns"] <= end[0]["mono_ns"] for a in on["sends"] if a["choice"] == 6))

    def test_wrong_carried_heading_conflict_or_rejected(self):
        runs = self.run_pair(27000)        # carried 270 deg, the car drives east (90)
        g3send = [r for r in runs["on"]["g3"] if r["kind"] in ("g3_send", "g3_send_conflict")]
        end = [r for r in g3send if r.get("event") == "end"]
        self.assertEqual(len(end), 1)
        self.assertIn(end[0]["reason"], ("conflict", "rejected"))


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
