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
        self.assertIn("G3 heading carry-over (store, restore, first course): 3 boots", text)
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

    def test_garage_write_overlap_rule(self):
        # Garage carry (validation/G3_GARAGE_EXIT_FIX_2026-10-17.md): writes
        # without fsync tagged "garage":1 run while BETA replaces by design; a
        # GPS loss in their window is expected, a journal lag or lat_ms > 500
        # trips the garage rule; the store rule (fsync writes) is unchanged.
        def write(seq, start, end, lat, sync=0, st="S", mark=False):
            row = dict(kind="g3_store", mono_ns=end + 10, event="written", seq=seq, st=st, hv=0 if st == "M" else 1,
                       h=0 if st == "M" else 100, unc=0 if st == "M" else 40, sync=sync, m_b=1.0, wait_s=0.0,
                       lat_ms=lat, start_ns=start, end_ns=end, ref_s=None, beta=None, garage=1)
            if mark:
                row["mark"] = 1
            return row
        lost = dict(kind="beta_state", mono_ns=10 * S + 1000, domain="beta", assist_ready=False, **{"from": "ARMED"},
                    to="GPS_LOST", reason="gps_lost")
        base_rows = [boot(BOOT_A),
                     dict(kind="g3_store", mono_ns=1, event="missing", reason="missing", g3="UNKNOWN", age_s=None,
                          age_src="none", boot="none", chain="none", seq=0),
                     write(1, 10 * S, 10 * S + 4000000, 4.0), write(2, 30 * S, 30 * S + 3000000, 3.0, st="M", mark=True)]
        _, report = consume(base_rows + [lost])
        o = report["g3"]["write_overlap"]
        self.assertEqual((o["writes"], o["overlaps"], o["default_off_next_build"]), (0, 0, False))
        self.assertEqual((o["garage_writes"], o["garage_lag_overlaps"], o["garage_off_next_build"]), (2, 0, False))
        lag = dict(kind="beta_journal_lag", mono_ns=31 * S, domain="beta", assist_ready=False, event="lagging",
                   lag_ms=1510, unwritten_rows=3, limit_ms=1500, clear_ms=500)
        _, report = consume(base_rows + [lost, lag])
        o = report["g3"]["write_overlap"]
        self.assertEqual((o["garage_lag_overlaps"], o["garage_off_next_build"], o["default_off_next_build"]),
                         (1, True, False))
        _, report = consume(base_rows + [write(3, 50 * S, 51 * S, 700.0)])
        self.assertEqual(report["g3"]["write_overlap"]["garage_lat_over_500"], 1)
        # A garage fsync write keeps the store rule (a GPS loss in its window counts).
        _, report = consume(base_rows + [write(4, 10 * S, 11 * S, 90.0, sync=1), lost])
        o = report["g3"]["write_overlap"]
        self.assertEqual((o["writes"], o["overlaps"], o["default_off_next_build"]), (1, 1, True))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            audit.print_g3_data(report["g3"])
        self.assertIn("G3 garage writes without fsync: 2, lat_ms > 500: 0, journal lag overlaps: 0", out.getvalue())
        self.assertNotIn("malformed_g3_row", codes(report))

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


def location(bearing_e6, speed_e3=280):
    payload = bytearray(range(0x50, 0x80))
    payload[40] = 1
    payload[36:40] = speed_e3.to_bytes(4, "little")
    payload[44:48] = bearing_e6.to_bytes(4, "little", signed=True)
    return bytes(payload)


def send_row(mono, choice, original, outgoing, mode=1, cls=2, reason=None, call=1):
    return dict(kind="send", call=call, generation=5, mono_ns=mono, mode=mode, type=1, length=48, choice=choice,
                reason=(0 if choice else 7) if reason is None else reason, result=0, **{"class": cls},
                original_hex=original.hex(), outgoing_hex=outgoing.hex())


def window(start=2 * S, end=5 * S, reason="trusted", call=7, by="fix"):
    rows = [dict(kind="g3_send", mono_ns=start, event="start", g3="CARRIED", t_s=0.0, h=20, carried=[20, 45],
                 unc=45, tol=100, age_s=600, rev=0, m_b=0.0)]
    if end is not None:
        rows.append(dict(kind="g3_send", mono_ns=end, event="end", reason=reason, why="stock_course", by=by,
                         call=call, t_s=3.0, sent=1, stock=0, h=20, unc=46, gated=4, course=[10, 180], d=10))
    return rows


class G3BearingSend(unittest.TestCase):
    """G3 first-connection bearing send (validation/G3_SEND_2026-10-13.md):
    choice-6 FIX sends change only hasBearing/bearing, inside the window."""

    stock = location(0)
    sent = location(2000000)

    def bearing_codes(self, rows):
        _, report = consume([boot(BOOT_A)] + rows)
        return [c for c in codes(report) if c.startswith("g3_bearing") or c in ("malformed_g3_row",
                "native_mode_mutation", "unknown_send_choice", "beta_nonpass_reason")], report

    def test_valid_window(self):
        found, report = self.bearing_codes(window() + [send_row(3 * S, 6, self.stock, self.sent),
                                                       send_row(6 * S, 0, self.stock, self.stock, call=2)])
        self.assertEqual(found, [])
        send = report["g3"]["send"]
        self.assertEqual((send["windows"], send["end_reasons"], send["overlaid_sends"]), (1, {"trusted": 1}, 1))
        self.assertEqual(send["boots"][0]["start"]["heading_deg"], 2.0)
        self.assertEqual(report["send_choices"]["BETA_BEARING_OVERLAY"], 1)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            audit.print_g3_data(report["g3"])
        self.assertIn("G3 bearing send: 1 windows, ends {'trusted': 1}, overlaid FIX sends 1", out.getvalue())
        self.assertIn("start 2.0 deg +- 4.5 (tol 10.0); end trusted/stock_course at 3.0 s, sent 1, stock 0",
                      out.getvalue())
        # Real-order clock: the end row carries the worker time; the send of
        # the deciding call is stamped by the adapter 1 us or 20 ms later and
        # its row may follow the end row. Exactly one such send is allowed.
        for late in (1000, 20 * 10**6):
            found, _ = self.bearing_codes(window(end=5 * S, call=7) + [send_row(5 * S + late, 6, self.stock, self.sent,
                                                                              call=7)])
            self.assertEqual(found, [], late)
        # A later call after a fix end, or a second post-end send: outside.
        found, _ = self.bearing_codes(window(end=5 * S, call=7) + [send_row(5 * S + 1000, 6, self.stock, self.sent,
                                                                          call=8)])
        self.assertIn("g3_bearing_outside_window", found)
        # A tick end: the call in progress (end call + 1) once, nothing more.
        found, _ = self.bearing_codes(window(end=5 * S, call=7, by="tick") +
                                      [send_row(5 * S + 1000, 6, self.stock, self.sent, call=8)])
        self.assertEqual(found, [])
        found, _ = self.bearing_codes(window(end=5 * S, call=7, by="tick") +
                                      [send_row(5 * S + 1000, 6, self.stock, self.sent, call=8),
                                       send_row(6 * S, 6, self.stock, self.sent, call=9)])
        self.assertIn("g3_bearing_outside_window", found)

    def test_violations(self):
        cases = [
            (window() + [send_row(6 * S, 6, self.stock, self.sent)], "g3_bearing_outside_window"),
            ([send_row(3 * S, 6, self.stock, self.sent)], "g3_bearing_outside_window"),
            (window(end=None) + [send_row(1 * S, 6, self.stock, self.sent)], "g3_bearing_outside_window"),
            (window() + [send_row(3 * S, 6, self.stock, location(2000000, speed_e3=9))], "g3_bearing_payload_mismatch"),
            (window() + [send_row(3 * S, 6, self.stock, bytes(self.sent[:40]) + b"\x00" + bytes(self.sent[41:]))],
             "g3_bearing_payload_mismatch"),
            (window() + [send_row(3 * S, 6, self.stock, location(10000))], "g3_bearing_out_of_range"),
            (window() + [send_row(3 * S, 6, location(5), location(360000000))], "g3_bearing_out_of_range"),
            (window() + [send_row(3 * S, 6, self.stock, self.sent, mode=0, cls=3)], "g3_bearing_wrong_class"),
            (window() + [send_row(3 * S, 6, self.stock, self.sent, cls=1)], "g3_bearing_wrong_class"),
            (window() + [send_row(3 * S, 6, self.stock, self.sent, reason=8)], "beta_nonpass_reason"),
            # A bearing change under any other choice is a native-mode mutation.
            (window() + [send_row(3 * S, 0, self.stock, self.sent)], "native_mode_mutation"),
        ]
        for rows, code in cases:
            found, _ = self.bearing_codes(rows)
            self.assertIn(code, found, (code, rows[-1]))

    def test_rows_and_old_logs(self):
        bad = [dict(kind="g3_send", mono_ns=1, event="exploded"),
               dict(kind="g3_send", mono_ns=1, event="end", reason="tired", why="x", sent=0, stock=0),
               dict(kind="g3_send", mono_ns=1, event="start", g3="CARRIED", h=1.5, carried=[1, 2]),
               dict(kind="g3_send", mono_ns=1, event="none", g3="NOPE", reason="x"),
               dict(kind="g3_send_conflict", mono_ns=1, h=1, unc=1, tol=1, course=[1], d=1, fixes=8)]
        for row in bad:
            found, _ = self.bearing_codes([row])
            self.assertIn("malformed_g3_row", found, row)
        ok = [dict(kind="g3_send", mono_ns=1, event="none", g3="UNKNOWN", reason="missing"),
              dict(kind="g3_send_conflict", mono_ns=1, t_s=4.0, h=900, unc=45, tol=100, course=[0, 360], d=900,
                   fixes=8)]
        found, report = self.bearing_codes(ok)
        self.assertEqual(found, [])
        self.assertEqual(report["g3"]["send"]["boots"][0]["none"], dict(state="UNKNOWN", reason="missing"))
        # Logs without the send (every build before it): no send section.
        _, report = consume([boot(BOOT_A), dict(kind="g3_store", mono_ns=1, event="missing", reason="missing",
                                                g3="UNKNOWN", age_s=None, age_src="none", boot="none",
                                                chain="none", seq=0)])
        self.assertNotIn("send", report["g3"])


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
            self.assertIn("G3 heading carry-over (store, restore, first course): 2 boots", text.getvalue())


if __name__ == "__main__":
    unittest.main()
