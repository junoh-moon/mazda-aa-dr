"""G3 record-only rows in tools/analyze_logs.py (validation/G3_RECORD_ONLY_2026-10-11.md).

Synthetic rows check validation and the per-boot report (restore decision,
continuity checks, store writes, first-session rows, first-course error
distribution, reverse starts). The replay check runs the real tracker and
store file through two synthetic boots (build/test_g3 --two-boot, set by
MX5DR_G3_FIXTURE) and analyzes its JSONL. PC-only; not vehicle, phone or DHU
evidence; a GPS course is a reference, not ground truth.
"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("analyze_logs_g3", ROOT / "tools" / "analyze_logs.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
S = 1000000000
BOOT_A = "aaaaaaaa-1111-2222-3333-444444444444"
BOOT_B = "bbbbbbbb-1111-2222-3333-444444444444"


def boot(boot_id):
    return dict(kind="boot", schema=1, pid=1, mono_ns=1, boot_id=boot_id, mode=5, install="ok",
                assist_ready=False, assist_block="synthetic", wire_timestamp_modified=False,
                log_profile="persistent")


def first(event, state, mono, **extra):
    row = dict(kind="g3_first", mono_ns=mono, event=event, g3=state, reason="x", t_s=1.0, m_s=0.0, m_b=0.0,
               v=0.0, rev=[0, 0], carried=[1970, 45], body=[1970, 45], travel=1970, stock=[1, 0, 0],
               stock_n=1, stock_zero=1, stock_none=0)
    row.update(extra)
    return row


def consume(rows):
    a = audit.Auditor()
    for i, row in enumerate(rows):
        a.consume(row, "synthetic:%d" % i)
    return a, a.report()


def codes(report):
    return [i["code"] for i in report["issues"]]


class G3Rows(unittest.TestCase):
    def test_report_per_boot_and_errors(self):
        rows = [boot(BOOT_A),
                dict(kind="g3_store", mono_ns=1, event="missing", reason="missing", g3="UNKNOWN", age_s=None,
                     age_src="none", boot="none", chain="none", seq=0),
                dict(kind="g3_store", mono_ns=2 * S, event="written", seq=1, st="S", hv=1, h=1973, unc=44,
                     m_b=820.0, lat_ms=3.2),
                dict(kind="g3_store", mono_ns=3 * S, event="failed", seq=2, st="S", hv=0, h=0, unc=0, m_b=830.0,
                     lat_ms=1.0, stage="open", errno=13),
                dict(kind="g3_since", mono_ns=4 * S, event="unstored", reason="beta_replacing", g3="UNKNOWN",
                     m=80.0, rot=-955, arot=4870, age_s=12.5, m_b=900.0),
                dict(kind="g3_store", mono_ns=5 * S, event="cancelled", seq=3, st="S", hv=1, h=10, unc=5,
                     m_b=900.0, lat_ms=2.0, start_ns=1, end_ns=2, reason="cancelled", at="rename"),
                boot(BOOT_B),
                dict(kind="g3_store", mono_ns=1, event="loaded", reason="chain_ok", g3="CARRIED", st="S", hv=1,
                     h=1973, unc=44, age_s=10800, age_src="rt", boot="aaaaaaaa", chain="ok", seq=1),
                dict(kind="g3_store", mono_ns=2, event="checked", reason="chain_ok", g3="CARRIED", pos="ok",
                     d_m=3.3, limit_m=62.0, age_s=10803, age_src="gps", m_b=0.0),
                first("start", "CARRIED", 3 * S, d_sp=-1023, stock=[1, 950, 40]),
                first("course", "CONFIRMED", 50 * S, rev=[1, 0], body=[2343, 98], travel=2343,
                      course=[2340, 140, 1], err_c=-367, err_p=3, err_s=1260, t_s=46.0, m_s=40.0),
                boot("cccccccc-1111-2222-3333-444444444444"),
                dict(kind="g3_store", mono_ns=1, event="rejected", reason="chain_broken", g3="UNKNOWN",
                     st="S", hv=1, h=100, unc=40, age_s=None, age_src="none", boot="bbbbbbbb", chain="broken",
                     seq=1),
                first("course", "UNKNOWN", 20 * S, carried=None, body=None, travel=None, course=[900, 200, 1],
                      err_c=None, err_p=None, err_s=-900)]
        _, report = consume(rows)
        self.assertNotIn("unknown_record_kind", codes(report))
        self.assertNotIn("malformed_g3_row", codes(report))
        g3 = report["g3"]
        self.assertEqual(len(g3["boots"]), 3)
        a, b, c = g3["boots"]
        self.assertEqual((a["boot_id"], a["restore"]["event"], a["writes"], a["failures"]), ("aaaaaaaa", "missing", 1, 1))
        self.assertEqual(a["last_write"]["heading_deg"], 197.3)
        self.assertEqual((b["restore"]["state"], b["restore"]["chain"], b["restore"]["heading_deg"]), ("CARRIED", "ok", 197.3))
        self.assertEqual(b["checks"][0]["pos"], "ok")
        self.assertEqual(b["first"]["start"]["stock_minus_travel_deg"], -102.3)
        self.assertEqual(b["first"]["course"]["err_prop_deg"], 0.3)
        self.assertTrue(b["reverse_start"])
        self.assertEqual(b["final_state"], "CONFIRMED")
        self.assertEqual((c["restore"]["reason"], c["final_state"]), ("chain_broken", "UNKNOWN"))
        self.assertEqual(g3["first_course_error_deg"]["count"], 1)
        self.assertEqual(g3["first_course_error_deg"]["median_abs"], 0.3)
        self.assertEqual(g3["carried_unpropagated_error_deg"]["max_abs"], 36.7)
        self.assertEqual(g3["stock_bearing_error_deg"]["count"], 2)
        self.assertEqual(g3["stock_bearing_error_deg"]["median_abs"], 108.0)
        self.assertEqual(g3["reverse_starts"], 1)
        self.assertEqual(g3["restore_events"], {"missing": 1, "loaded": 1, "rejected": 1})
        self.assertEqual(g3["final_states"], {"UNKNOWN": 2, "CONFIRMED": 1})
        self.assertEqual((g3["writes"], g3["write_failures"], g3["parked_stops_unstored"],
                          g3["writes_cancelled"]), (1, 1, 1, 1))
        self.assertEqual(a["unstored"], {"beta_replacing": 1})
        self.assertEqual(a["cancelled"], {"cancelled@rename": 1})
        self.assertEqual(a["since"][0]["rot_deg"], -95.5)
        json.dumps(report, allow_nan=False)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            audit.print_g3_data(g3)
        text = out.getvalue()
        self.assertIn("G3 heading carry-over (record-only; nothing sent): 3 boots", text)
        self.assertIn("boot bbbbbbbb: store loaded/chain_ok carried 197.3+-4.4", text)
        self.assertIn("err carried -36.7 prop 0.3 stock 126.0", text)

    def test_write_overlap_check(self):
        def write(seq, start, end, lat, event="written"):
            row = dict(kind="g3_store", mono_ns=end + 10, event=event, seq=seq, st="S", hv=1, h=100, unc=40,
                       sync=0, m_b=1.0, wait_s=0.0, lat_ms=lat, start_ns=start, end_ns=end)
            if event == "cancelled":
                row.update(reason="cancelled", at="rename", why="beta_replacing")
            return row
        base_rows = [boot(BOOT_A),
                     dict(kind="g3_store", mono_ns=1, event="missing", reason="missing", g3="UNKNOWN", age_s=None,
                          age_src="none", boot="none", chain="none", seq=0),
                     write(1, 10 * S, 10 * S + 4000000, 4.0)]
        _, report = consume(base_rows)
        o = report["g3"]["write_overlap"]
        self.assertEqual((o["writes"], o["max_lat_ms"], o["overlaps"], o["default_off_next_build"]), (1, 4.0, 0, False))
        lag = dict(kind="beta_journal_lag", mono_ns=11 * S, domain="beta", assist_ready=False, event="lagging",
                   lag_ms=1510, unwritten_rows=3, limit_ms=1500, clear_ms=500)
        _, report = consume(base_rows + [lag])
        o = report["g3"]["write_overlap"]
        self.assertEqual((o["overlaps"], o["default_off_next_build"]), (1, True))
        self.assertEqual(o["overlap_events"][0]["event"], "beta_journal_lag")
        late = dict(lag, mono_ns=13 * S)     # outside end + 2 s
        _, report = consume(base_rows + [late])
        self.assertEqual(report["g3"]["write_overlap"]["overlaps"], 0)
        _, report = consume(base_rows + [write(2, 20 * S, 21 * S, 600.0, "cancelled")])
        o = report["g3"]["write_overlap"]
        self.assertEqual((o["writes"], o["max_lat_ms"], o["lat_over_500"], o["default_off_next_build"]),
                         (2, 600.0, 1, True))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            audit.print_g3_data(report["g3"])
        self.assertIn("G3 write overlap check: 2 writes, max lat_ms 600.0, overlaps with beta_journal_lag/GPS_LOST: 0 "
                      "-> pre-registered rule: next build defaults the store OFF", out.getvalue())

    def test_malformed_rows(self):
        bad = [dict(kind="g3_store", mono_ns=1, event="exploded", reason="x"),
               dict(kind="g3_store", mono_ns=1, event="written", seq=1, st="X", hv=1, h=1, unc=1),
               dict(kind="g3_first", mono_ns=1, event="course", g3="CARRIED", rev=[0, 0], course=[1, 2]),
               first("start", "SOMETHING", 1),
               first("start", "CARRIED", 1, rev=[2, 0]),
               first("start", "CARRIED", 1, carried=[1, 2, 3]),
               first("start", "CARRIED", 1, err_p=1.5),
               dict(kind="g3_store", event="missing", reason="missing")]
        for row in bad:
            _, report = consume([boot(BOOT_A), row])
            self.assertIn("malformed_g3_row", codes(report), row)
            self.assertEqual(report["g3"]["boots"], [])

    def test_no_rows_no_section(self):
        _, report = consume([boot(BOOT_A)])
        self.assertEqual(report["g3"]["boots"], [])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            audit.print_g3_data(report["g3"])
        self.assertEqual(out.getvalue(), "")


class G3TwoBootReplay(unittest.TestCase):
    """The real tracker, store codec and atomic file writer across two boots."""

    def test_two_boot_file(self):
        fixture = os.environ.get("MX5DR_G3_FIXTURE")
        if not fixture:
            self.skipTest("MX5DR_G3_FIXTURE unset (make test-tools sets it to build/test_g3)")
        with tempfile.TemporaryDirectory() as store, tempfile.TemporaryDirectory() as out:
            result = subprocess.run([fixture, "--two-boot", store], stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, universal_newlines=True, check=True, timeout=60)
            trace = Path(out) / "trace.0.jsonl"
            trace.write_text(result.stdout)
            rows = [json.loads(line) for line in result.stdout.splitlines()]
            self.assertTrue(all(isinstance(r, dict) and r.get("kind") for r in rows))
            stored = (Path(store) / "g3-heading.v1").read_text()
            self.assertTrue(stored.startswith("mx5g3 1 2 " + BOOT_A + " S 3 19730 440 "))
            report = audit.analyze([str(trace)])
            self.assertNotIn("malformed_g3_row", codes(report))
            self.assertNotIn("unknown_record_kind", codes(report))
            g3 = report["g3"]
            self.assertEqual(len(g3["boots"]), 2)
            a, b = g3["boots"]
            self.assertEqual((a["boot_id"], a["restore"]["event"], a["writes"], a["failures"]),
                             ("aaaaaaaa", "missing", 2, 0))
            self.assertEqual((a["last_write"]["st"], a["last_write"]["heading_deg"]), ("S", 197.3))
            self.assertEqual(a["final_state"], "UNKNOWN")
            self.assertIsNone(a["first"]["course"]["err_prop_deg"])
            self.assertEqual(a["first"]["course"]["err_stock_deg"], -120.0)
            self.assertEqual((b["boot_id"], b["restore"]["event"], b["restore"]["state"], b["restore"]["chain"]),
                             ("bbbbbbbb", "loaded", "CARRIED", "ok"))
            self.assertEqual((b["restore"]["heading_deg"], b["restore"]["unc_deg"], b["restore"]["seq"]),
                             (197.3, 4.4, 2))
            self.assertEqual(b["checks"][0]["pos"], "ok")
            self.assertEqual(b["first"]["t30"]["stock_zero"], 10)
            self.assertEqual(b["first"]["course"]["err_prop_deg"], 0.3)
            self.assertEqual(b["first"]["course"]["err_carried_deg"], -36.7)
            self.assertTrue(b["reverse_start"])
            self.assertEqual(b["final_state"], "CONFIRMED")
            self.assertEqual(g3["reverse_starts"], 1)
            self.assertEqual(g3["first_course_error_deg"]["count"], 1)
            text = io.StringIO()
            with contextlib.redirect_stdout(text):
                audit.main([str(trace)])
            self.assertIn("G3 heading carry-over (record-only; nothing sent): 2 boots", text.getvalue())


if __name__ == "__main__":
    unittest.main()
