"""Drive data rows in tools/analyze_logs.py
(validation/RELEASE_V100_BETA14_CANDIDATE_2026-10-13.md).

Synthetic rows check validation, the drive_data report (fix1 coverage,
exit_jump with the Naver predicates, yaw_learn at GPS loss, rec_drop totals),
the text section, and that logs without these rows (every earlier build) are
read as before. The product rows themselves are checked against this
analyzer in tests/journal/test_log_profile.py (DriveRows). PC-only; not
vehicle, phone or DHU evidence.
"""
import contextlib
import importlib.util
import io
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("analyze_logs_drive", ROOT / "tools" / "analyze_logs.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
S = 1000000000
BOOT = "dddddddd-1111-2222-3333-444444444444"


def boot():
    return dict(kind="boot", schema=1, pid=1, mono_ns=1, boot_id=BOOT, mode=5, install="ok",
                assist_ready=False, assist_block="synthetic", wire_timestamp_modified=False,
                log_profile="persistent")


def fix1(t_ms, sat=-1, acc=4000, bearing=90000000):
    return dict(kind="fix1", d=[t_ms, 1, 375000000, 1270000000, 11111, bearing, acc, sat, 1000, -12, 50, 400, 10])


def exit_jump(ep, fix_ms, r1=False, r2=False, jump=41.2, along=40.0, send=True):
    return dict(kind="exit_jump", schema=1, mono_ns=fix_ms * 1000000 + 5, ep=ep, fix_ms=fix_ms,
                fix_send_ms=fix_ms + 2 if send else None, mode=1, lost_ms=60000, reps=59, rep_ms=58000, stock=0,
                stock_after=0, sent_choice=3, sent_ms=fix_ms - 996, dt_ms=996, jump_m=jump, along_m=along,
                cross_m=-10.0, sent_brg=90.0, fix_brg=95.0, dbrg=5.0, sent_kmh=36.0, fix_kmh=40.0, sent_acc=25.0,
                fix_acc=3.2, honest=475.97, r1=r1, r1_acc=True, r1_dt=True, r1_dist=jump >= 50, r1_speed=True,
                r2=r2, r2_kmh=148.6)


def learn(ev, at_ms, z=133, age=8000):
    return dict(kind="yaw_learn", schema=1, mono_ns=at_ms * 1000000, at_ms=at_ms, ev=ev, z=z, used=0,
                mean=None if z is None else 67, span_ms=None if z is None else 40138, dc=None if z is None else 1.0,
                dev=None if z is None else 10.0, cover=None if z is None else 1000, fixes=41 if z is not None else 0,
                age_ms=None if z is None else age, n=3)


def drop(**counts):
    row = dict(kind="rec_drop", schema=1, mono_ns=9 * S, fix1=0, exit_jump=0, yaw_learn=0, rec_drop=0, fix1_rate=0,
               exit_rate=0, lost_rate=0, why=dict(journal=0, no_writer=0, lag=0, queue=0))
    row.update(counts)
    return row


def consume(rows):
    a = audit.Auditor()
    for i, row in enumerate(rows):
        a.consume(row, "synthetic:%d" % i)
    return a.report()


def codes(report):
    return [i["code"] for i in report["issues"]]


class DriveRows(unittest.TestCase):
    def test_report(self):
        rows = [boot(), fix1(100000), fix1(101000, sat=7), fix1(104000, acc=-1),
                learn("periodic", 101000), learn("lost", 105000), exit_jump(1, 165000, r1=True, jump=50.3),
                learn("lost", 200000, z=None), exit_jump(2, 210000, send=False),
                drop(fix1=3, rec_drop=1, why=dict(journal=0, no_writer=0, lag=4, queue=0)), drop(fix1=1, exit_rate=2)]
        report = consume(rows)
        self.assertNotIn("malformed_drive_row", codes(report))
        self.assertNotIn("unknown_record_kind", codes(report))
        d = report["drive_data"]
        self.assertEqual(d["summary"], dict(fix1_rows=3, exits=2, r1_exits=1, r2_exits=0, learn_rows=3, lost_rows=2,
                                            lost_with_estimate=1, dropped=dict(fix1=4, exit_jump=0, yaw_learn=0,
                                                                               rec_drop=1, fix1_rate=0, exit_rate=2,
                                                                               lost_rate=0, why_journal=0,
                                                                               why_no_writer=0, why_lag=4,
                                                                               why_queue=0),
                                            counts="lower_bound", lower_bound=d["summary"]["lower_bound"]))
        self.assertEqual(d["fix1_sessions"]["1"], dict(rows=3, first_s=100.0, last_s=104.0, max_gap_s=3.0,
                                                      accuracy_known=2, satellites_known=1, yaw_samples=150))
        first = d["exits"][0]
        self.assertEqual((first["fix_s"], first["dt_ms"], first["dt_send_ms"], first["r1_dt_send"], first["jump_m"],
                          first["r1"], first["r1_parts"], first["reported_acc"]),
                         (165.0, 996, 998, True, 50.3, True, [True, True, True, True], 25.0))
        self.assertEqual((d["exits"][1]["dt_send_ms"], d["exits"][1]["r1_dt_send"]), (None, None))
        lost = [e for e in d["learn"] if e["ev"] == "lost"]
        self.assertEqual((lost[0]["zero"], lost[0]["used"], lost[0]["age_s"]), (2049.33, 2048.0, 8.0))
        self.assertIsNone(lost[1]["zero"])
        text = io.StringIO()
        with contextlib.redirect_stdout(text):
            audit.print_drive_data(d)
        out = text.getvalue()
        self.assertIn("Drive data (record only): 3 fix1 rows, 2 exit_jump (R1 1, R2 0)", out)
        self.assertIn("jump    50.3 m", out)
        self.assertIn("zero 2049.33 (BETA uses 2048.00)", out)
        self.assertIn("reported acc: the accuracy in the last LOCATION sent, authoritative", out)

    def test_malformed_rows(self):
        bad = [dict(fix1(1000), d=[1, 1, 2]), dict(fix1(1000), d=[1000, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
               dict(fix1(1000), d=[1000, 1, 0, 0, 0, 360000000, 0, 0, 0, 0, 0, 0, 0]),
               dict(fix1(1000), d=[1000, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.5]),
               dict(exit_jump(1, 5000), r1="true"), dict(exit_jump(1, 5000), jump_m=-1),
               dict(exit_jump(1, 5000), schema=2), dict(learn("lost", 5000), ev="other"),
               dict(learn("lost", 5000), mean=None), dict(drop(), fix1=-1), dict(drop(), schema=None),
               dict(exit_jump(1, 5000), sent_choice=None), dict(drop(), why=dict(lag=1)),
               dict(exit_jump(1, 5000), fix_send_ms=-1)]
        for row in bad:
            report = consume([boot(), row])
            self.assertIn("malformed_drive_row", codes(report), row)
            self.assertNotIn("drive_data", report, row)

    def test_logs_without_drive_rows(self):
        # No section at all: reports of earlier logs stay byte-identical.
        report = consume([boot(), dict(kind="capture_end", schema=1, mono_ns=5 * S)])
        self.assertNotIn("drive_data", report)
        self.assertNotIn("drive_data", report["lower_bounds"])
        text = io.StringIO()
        with contextlib.redirect_stdout(text):
            audit.print_drive_data(report.get("drive_data"))
        self.assertEqual(text.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
