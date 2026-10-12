"""Synthetic diagnostic journals only; no OEM, vehicle or phone execution."""
import contextlib
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


audit = load("analyze_diag", ROOT / "tools/analyze_logs.py")
beta = load("diag_beta_fixture", ROOT / "tests/tools/test_analyze_beta.py")
legacy = load("diag_legacy_fixture", ROOT / "tests/tools/test_analyze_logs.py")
S = 1_000_000_000
BOOT = "aaaaaaaa-1111-2222-3333-444444444444"


def diag_boot(boot_id=BOOT, active=True):
    return dict(kind="diag_boot", mono_ns=11, domain="beta", assist_ready=False,
                active=active, reason="marker" if active else "beta_core_unavailable",
                boot_id=boot_id, arms=["CONTROL", "A20", "S5"], block=3,
                rejoin_ms=10000, cap_m=20, sat_type=21, sat_length=864, sat_in_use=5)


def episode(event, ns, arm="S5", index=0, boot_id=BOOT, **extra):
    row = dict(kind="diag_episode", mono_ns=ns, domain="beta", assist_ready=False,
               event=event, boot_id=boot_id, episode=index, arm=arm, reason="gps_lost",
               state="GPS_LOST", session_epoch=1, generation=1)
    if event == "assigned":
        row.update(block=0, perm=0, cap_m=20, sat_in_use=5)
    elif event == "continued":
        row.update(gap_ms=1000)
    else:
        row.update(reason="gps_returned", state="ARMED", open_ms=3000, pubs=4,
                   capped=4 if arm == "A20" else 0, honest_max_m=120,
                   reported_min_m=20 if arm == "A20" else 40,
                   reported_max_m=20 if arm == "A20" else 40, loc_replaced=2,
                   loc_nonzero=0, loc_original_mode0=1, sat=3,
                   sat_applied=2 if arm == "S5" else 0, sat_nonzero=0,
                   sat_outcomes={"no_replacement": 1,
                                 {"S5": "applied", "A20": "arm_a20", "CONTROL": "arm_control", "CAP": "arm_cap"}[arm]: 2},
                   first_sat=dict(before=0, presence=0, outcome="no_replacement", after_replacement=False))
    row.update(extra)
    return row


def satellite(call, ns, arm="NONE", outcome="no_replacement", result=0):
    applied = outcome == "applied"
    return dict(kind="send", call=call, generation=1, mono_ns=ns, mode=0, type=21,
                length=864, choice=5 if applied else 0, reason=0, result=result,
                original_hex="", outgoing_hex="", **{"class": 3},
                diag=dict(arm=arm, sat=outcome, in_use=[0, 5 if applied else 0], presence=0))


def diagnostic_drive(arm="S5", boot_id=BOOT):
    """Real row names/fields from the diagnostic working-tree diff.

    The first original send predates the worker's assignment timestamp,
    and the final zero-speed sample makes GPS-return alignment unambiguous.
    """
    rows = beta.drive(gps_offset_m=50, accuracy_e3=20000 if arm == "A20" else 40000, honest_m=120)
    rows[0]["boot_id"] = boot_id
    rows.insert(1, diag_boot(boot_id))
    out = []
    for row in rows:
        if row["kind"] == "send" and row["choice"] == 3:
            row["diag"] = dict(arm=arm)
            if row["call"] == 4:
                data = bytearray.fromhex(row["outgoing_hex"])
                beta.put32(data, 36, 0)
                row["outgoing_hex"] = data.hex()
        out.append(row)
        if row["kind"] == "beta_state" and row["to"] == "GPS_LOST":
            out.append(episode("assigned", row["mono_ns"], arm, boot_id=boot_id))
        if row["kind"] == "send" and row["mode"] == 0:
            outcome = "no_replacement" if row["choice"] == 0 else {
                "S5": "applied", "A20": "arm_a20", "CONTROL": "arm_control"}[arm]
            out.append(satellite(row["call"], row["mono_ns"] + 1,
                                 arm if row["choice"] else "NONE", outcome))
        if row["kind"] == "beta_state" and row["to"] == "ARMED" and row["reason"] == "gps_returned":
            out.append(episode("ended", row["mono_ns"], arm, boot_id=boot_id))
    return out


def diag_boot_v2(boot_id=BOOT, active=True, reason=None):
    """The version 2 diag_boot row (validation/TUNNEL_REPORT_CAP_2026-10-12.md)."""
    return dict(kind="diag_boot", mono_ns=11, domain="beta", assist_ready=False,
                active=active, reason=reason or ("marker" if active else "acc_cap_off"),
                boot_id=boot_id, version=2, arms=["CONTROL", "CAP"], block=3,
                rejoin_ms=10000, cap_m=25, control_ceiling_m=40, sat_type=21, sat_length=864)


def shadow_boot(acc_cap_m=25):
    return dict(kind="shadow_boot", active=True, capture_active=True, domain="model",
                source="existing_vbs_vim_callback", assist_ready=False, motion_sampling=False,
                acc_cap_m=acc_cap_m)


def diagnostic_drive_v2(arm="CAP", boot_id=BOOT):
    """Version 2: CAP episodes send 25 m, CONTROL 40 m; type 21 observed only."""
    rows = beta.drive(gps_offset_m=50, accuracy_e3=25000 if arm == "CAP" else 40000, honest_m=120)
    rows[0]["boot_id"] = boot_id
    rows.insert(1, shadow_boot())
    rows.insert(2, diag_boot_v2(boot_id))
    out = []
    for row in rows:
        if row["kind"] == "send" and row["choice"] == 3:
            row["diag"] = dict(arm=arm)
        out.append(row)
        if row["kind"] == "beta_state" and row["to"] == "GPS_LOST":
            assigned = episode("assigned", row["mono_ns"], arm, boot_id=boot_id)
            for key in ("cap_m", "sat_in_use"):
                del assigned[key]
            assigned.update(perm=1, ceiling_m=25 if arm == "CAP" else 40)
            out.append(assigned)
        if row["kind"] == "send" and row["mode"] == 0:
            outcome = "no_replacement" if row["choice"] == 0 else {"CAP": "arm_cap", "CONTROL": "arm_control"}[arm]
            out.append(satellite(row["call"], row["mono_ns"] + 1, arm if row["choice"] else "NONE", outcome))
        if row["kind"] == "beta_state" and row["to"] == "ARMED" and row["reason"] == "gps_returned":
            out.append(episode("ended", row["mono_ns"], arm, boot_id=boot_id, capped=4 if arm == "CAP" else 0,
                               reported_min_m=25 if arm == "CAP" else 40, reported_max_m=25 if arm == "CAP" else 40,
                               sat_applied=0,
                               sat_outcomes={"no_replacement": 1,
                                             {"CAP": "arm_cap", "CONTROL": "arm_control"}[arm]: 2}))
    return out


def consume(rows, module=audit):
    a = module.Auditor()
    for index, row in enumerate(rows):
        a.consume(row, "synthetic:%d" % index)
    return a.report()


class ControllerRowsAcceptedTests(unittest.TestCase):
    """The rows the C++ controller really writes pass the analyzer's schema check."""

    def test_cpp_episode_rows_are_valid_analyzer_rows(self):
        fixture = os.environ.get("MX5DR_DIAG_EPISODE_FIXTURE")
        if not fixture:
            self.skipTest("MX5DR_DIAG_EPISODE_FIXTURE is not set (make test-tools sets it)")
        out = subprocess.run([fixture, "rows"], check=True, capture_output=True, text=True).stdout
        rows = [json.loads(line) for line in out.splitlines() if line.strip()]
        kinds = {(r["kind"], r.get("event")) for r in rows}
        self.assertIn(("diag_episode", "assigned"), kinds)
        self.assertIn(("diag_episode", "continued"), kinds)
        self.assertIn(("diag_episode", "ended"), kinds)
        for row in rows:
            if row["kind"] == "diag_episode":
                self.assertTrue(audit.diag_row_valid(row), row)


class DiagnosticAnalyzeTests(unittest.TestCase):
    def test_s5_episode_and_text(self):
        report = consume(diagnostic_drive())
        self.assertIn("diagnostic", report)
        d = report["diagnostic"]
        self.assertEqual(len(d["episodes"]), 1)
        e = d["episodes"][0]
        self.assertEqual((e["boot_id"], e["episode"], e["arm"], e["assigned"]), (BOOT, 0, "S5", True))
        self.assertEqual(e["duration_s"], 3)
        self.assertEqual((e["loc_replaced"], e["sat_applied"], e["sat"]), (2, 2, 3))
        self.assertEqual((e["honest_max_m"], e["reported_min_m"], e["reported_max_m"]), (120, 40, 40))
        self.assertEqual(e["sat_transitions"], {"0->0/no_replacement": 1, "0->5/applied": 2})
        self.assertEqual(e["first_original"]["mono_ns"], 2 * S + 100)
        self.assertIsNone(e["first_original"]["reported_accuracy_m"])
        self.assertFalse(e["first_sat"]["after_replacement"])
        self.assertEqual((e["speed_min_mps"], e["speed_max_mps"], e["speed_change_mps"], e["sampled_stops"]),
                         (0, 10, 10, 1))
        self.assertEqual(e["gps_returns"], report["beta"]["gps_return_checks"])
        self.assertEqual(d["per_arm"]["S5"]["assigned"], 1)
        self.assertEqual(d["per_arm"]["S5"]["sat_applied_episodes"], 1)
        self.assertNotIn("non_location_mutation", {i["code"] for i in report["issues"]})
        self.assertNotIn("unknown_record_kind", {i["code"] for i in report["issues"]})
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            audit.print_diag_data(d)
        self.assertIn("Tunnel diagnostic", out.getvalue())
        self.assertIn("Small sample", out.getvalue())
        self.assertIn("honest", out.getvalue())
        self.assertIn("0->5/applied", out.getvalue())

    def test_a20_uses_honest_budget_for_return(self):
        report = consume(diagnostic_drive("A20"))
        check, = report["beta"]["gps_return_checks"]
        self.assertEqual(check["honest_accuracy_m"], 120)
        self.assertEqual(check["reported_accuracy_m"], 20)
        self.assertTrue(check["reported_accuracy_clamped"])
        self.assertTrue(check["within_honest_budget"])
        self.assertFalse(check["within_reported_accuracy"])
        e, = report["diagnostic"]["episodes"]
        self.assertEqual((e["capped"], e["pubs"]), (4, 4))

    def test_missing_honest_budget_stays_unknown(self):
        rows = diagnostic_drive("A20")
        for row in rows:
            row.pop("accuracy_honest_m", None)
            if row.get("event") == "ended":
                row["honest_max_m"] = None
        report = consume(rows)
        self.assertIsNone(report["beta"]["gps_return_checks"][0]["within_honest_budget"])
        self.assertIsNone(report["diagnostic"]["episodes"][0]["honest_max_m"])

    def test_continued_episode_uses_cumulative_end_once(self):
        rows = diagnostic_drive()
        rows += [episode("continued", 6 * S),
                 episode("ended", 8 * S, open_ms=5000, pubs=8, loc_replaced=4, sat=5, sat_applied=4)]
        d = consume(rows)["diagnostic"]
        e, = d["episodes"]
        self.assertEqual((e["duration_s"], e["pubs"], e["sat_applied"]), (5, 8, 4))
        self.assertEqual(d["per_arm"]["S5"]["assigned"], 1)

    def test_boot_and_trace_boundaries_do_not_merge_episode_zero(self):
        rows = diagnostic_drive() + diagnostic_drive("A20", "bbbbbbbb-1111-2222-3333-444444444444")
        episodes = consume(rows)["diagnostic"]["episodes"]
        self.assertEqual(len(episodes), 2)
        self.assertNotEqual(episodes[0]["boot_id"], episodes[1]["boot_id"])
        # Even the same kernel boot can contain separate process journals.
        self.assertEqual(len(consume(diagnostic_drive() * 2)["diagnostic"]["episodes"]), 2)

    def test_open_or_truncated_episode_is_not_a_complete_assignment(self):
        rows = [r for r in diagnostic_drive() if r.get("event") != "ended"]
        e, = consume(rows)["diagnostic"]["episodes"]
        self.assertIsNone(e["duration_s"])
        self.assertFalse(e["complete"])
        self.assertEqual(e["observed_sat_applied"], 2)
        rows = [r for r in diagnostic_drive() if r.get("event") != "assigned"]
        e, = consume(rows)["diagnostic"]["episodes"]
        self.assertFalse(e["assigned"])
        self.assertFalse(e["complete"])

    def test_send_only_does_not_invent_episode_or_assignment(self):
        rows = [r for r in diagnostic_drive() if not r["kind"].startswith("diag_")]
        d = consume(rows)["diagnostic"]
        self.assertEqual(d["episodes"], [])
        self.assertGreater(d["unassigned_sends"], 0)

    def test_inactive_boot_still_explains_missing_episodes(self):
        d = consume([dict(beta.boot(), boot_id=BOOT), diag_boot(active=False)])["diagnostic"]
        self.assertEqual(d["episodes"], [])
        self.assertEqual(d["boots"][0]["reason"], "beta_core_unavailable")

    def test_delayed_raw_window_first_original_is_attached_by_time(self):
        rows = diagnostic_drive()
        original = next(r for r in rows if r["kind"] == "send" and r["call"] == 2 and r["type"] == 1)
        rows.remove(original)
        rows.append(dict(original, raw_window=True))
        e, = consume(rows)["diagnostic"]["episodes"]
        self.assertEqual(e["first_original"]["mono_ns"], original["mono_ns"])
        self.assertEqual(e["observed_loc_replaced"], 2)

    def test_delayed_raw_window_position_also_bounds_the_first_send(self):
        rows = diagnostic_drive()
        buffered = [r for r in rows if r["kind"] in ("position", "send") and r.get("call") == 2 and
                    r.get("type", 1) == 1]
        rows = [r for r in rows if r not in buffered]
        rows += [dict(r, raw_window=True) for r in buffered]
        e, = consume(rows)["diagnostic"]["episodes"]
        self.assertIsNotNone(e["first_original"])
        self.assertEqual(e["first_original"]["mono_ns"], 2 * S + 100)

    def test_malformed_non_diag_send_inside_episode_does_not_crash(self):
        rows = diagnostic_drive()
        rows.insert(10, dict(beta.send(2, 2 * S + 1000, 0, beta.original_payload()), call=[]))
        report = consume(rows)
        self.assertIn("partial_record", {i["code"] for i in report["issues"]})

    def test_satellite_refusal_and_nonzero_are_not_success(self):
        rows = diagnostic_drive()
        sat = next(r for r in rows if r["kind"] == "send" and r["choice"] == 5)
        sat["result"] = 7
        end = next(r for r in rows if r.get("event") == "ended")
        end.update(sat_nonzero=1)
        d = consume(rows)["diagnostic"]
        self.assertEqual(d["episodes"][0]["sat_nonzero"], 1)
        self.assertEqual(d["per_arm"]["S5"]["nonzero_episodes"], 1)

    def test_malformed_diag_cannot_authorize_type21_rewrite(self):
        for change in (dict(length=8), dict(mode=1), dict(diag=dict(arm="CONTROL", sat="applied", in_use=[0, 5], presence=0)),
                       dict(diag=dict(arm="S5", sat="applied", in_use=[0, 5], presence=1))):
            rows = diagnostic_drive()
            sat = next(r for r in rows if r["kind"] == "send" and r["choice"] == 5)
            sat.update(change)
            report = consume(rows)
            self.assertEqual(report["status"], "violation", change)

    def test_s5_rewrite_requires_matching_successful_location(self):
        for result in (3, None):
            rows = diagnostic_drive()
            loc = next(r for r in rows if r["kind"] == "send" and r["choice"] == 3)
            if result is None:
                rows.remove(loc)
            else:
                loc["result"] = result
            report = consume(rows)
            self.assertIn("diag_sat_location_unconfirmed", {i["code"] for i in report["issues"]})

    def test_false_applied_outcome_is_not_counted(self):
        rows = diagnostic_drive()
        sat = next(r for r in rows if r["kind"] == "send" and r["choice"] == 5)
        sat["choice"] = 0
        report = consume(rows)
        self.assertIn("malformed_diag_row", {i["code"] for i in report["issues"]})
        self.assertEqual(report["diagnostic"]["episodes"][0]["observed_sat_applied"], 1)

    def test_control_and_short_satellite_keep_nulls_and_refusal(self):
        rows = diagnostic_drive("CONTROL")
        sat = next(r for r in rows if r["kind"] == "send" and r["type"] == 21)
        sat.update(length=4, diag=dict(arm="NONE", sat="no_replacement", in_use=[None, None], presence=None))
        e, = consume(rows)["diagnostic"]["episodes"]
        self.assertEqual(e["sat_applied"], 0)
        self.assertEqual(e["capped"], 0)
        self.assertIn("None->None/no_replacement", e["sat_transitions"])

    def test_diagnostic_presence_does_not_hide_unrelated_contract_violations(self):
        rows = diagnostic_drive()
        loc = next(r for r in rows if r["kind"] == "send" and r["choice"] == 3)
        loc["mode"] = 1
        report = consume(rows)
        self.assertIn("native_mode_mutation", {i["code"] for i in report["issues"]})

    def test_bad_diagnostic_rows_are_inconclusive_not_crashes(self):
        for row in (dict(diag_boot(), active="yes"), episode("invented", S),
                    episode("ended", S, open_ms="three"),
                    episode("ended", S, honest_max_m=float("inf")),
                    dict(satellite(1, S), diag=["S5"])):
            report = consume([dict(beta.boot(), boot_id=BOOT), row])
            self.assertIn("malformed_diag_row", {i["code"] for i in report["issues"]}, row)

    def test_version2_cap_episode_uses_honest_budget_and_new_arms(self):
        report = consume(diagnostic_drive_v2("CAP"))
        d = report["diagnostic"]
        self.assertEqual(list(d["per_arm"]), ["CONTROL", "CAP"])
        e, = d["episodes"]
        self.assertEqual((e["arm"], e["assigned"], e["complete"]), ("CAP", True, True))
        self.assertEqual((e["capped"], e["pubs"], e["sat_applied"]), (4, 4, 0))
        self.assertEqual(e["sat_transitions"], {"0->0/no_replacement": 1, "0->0/arm_cap": 2})
        self.assertEqual((e["sent_reported_min_m"], e["sent_reported_max_m"]), (25, 25))
        check, = report["beta"]["gps_return_checks"]
        self.assertEqual((check["honest_accuracy_m"], check["reported_accuracy_m"]), (120, 25))
        self.assertTrue(check["reported_accuracy_clamped"] and check["within_honest_budget"])
        codes = {i["code"] for i in report["issues"]}
        self.assertNotIn("malformed_diag_row", codes)
        self.assertNotIn("beta_return_exceeds_accuracy", codes)
        self.assertNotIn("non_location_mutation", codes)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            audit.print_diag_data(d)
        self.assertIn("Version 2: CAP = 25 m", out.getvalue())
        self.assertIn("per-arm CAP", out.getvalue())

    def test_version2_control_episode_keeps_the_40_m_rule(self):
        report = consume(diagnostic_drive_v2("CONTROL"))
        e, = report["diagnostic"]["episodes"]
        self.assertEqual((e["arm"], e["capped"]), ("CONTROL", 0))
        check, = report["beta"]["gps_return_checks"]
        self.assertEqual(check["reported_accuracy_m"], 40)
        self.assertTrue(check["reported_accuracy_clamped"] and check["within_honest_budget"])
        self.assertEqual(report["diagnostic"]["per_arm"]["CONTROL"]["assigned"], 1)

    def test_version1_logs_keep_their_three_arms(self):
        self.assertEqual(list(consume(diagnostic_drive("A20"))["diagnostic"]["per_arm"]), ["CONTROL", "A20", "S5"])

    def text(self, d):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            audit.print_diag_data(d)
        return out.getvalue()

    def test_version2_control_only_log_is_version2(self):
        # Review finding (beta.13): about one third of version 2 episodes are
        # CONTROL, so a short drive can hold only CONTROL episodes. The version
        # comes from diag_boot.version, not from the arms that occurred.
        d = consume(diagnostic_drive_v2("CONTROL"))["diagnostic"]
        self.assertEqual(d["versions"], [2])
        self.assertEqual(list(d["per_arm"]), ["CONTROL", "CAP"])
        self.assertEqual((d["per_arm"]["CONTROL"]["assigned"], d["per_arm"]["CAP"]["assigned"]), (1, 0))
        self.assertIn("Version 2: CAP = 25 m", self.text(d))
        self.assertNotIn("per-arm A20", self.text(d))

    def test_version2_without_diag_boot_row_uses_the_assigned_row(self):
        # A lost diag_boot row: the version 2 assigned row (ceiling_m) decides.
        rows = [r for r in diagnostic_drive_v2("CONTROL") if r["kind"] != "diag_boot"]
        d = consume(rows)["diagnostic"]
        self.assertEqual(d["versions"], [2])
        self.assertEqual(list(d["per_arm"]), ["CONTROL", "CAP"])

    def test_version2_inactive_boot_without_episodes_is_version2(self):
        # acc-cap-off: diag_boot active=false reason acc_cap_off, no episode.
        rows = [r for r in diagnostic_drive_v2("CAP") if r["kind"] not in ("diag_episode", "diag_boot")]
        rows.insert(2, diag_boot_v2(active=False))
        d = consume(rows)["diagnostic"]
        self.assertEqual(d["versions"], [2])
        self.assertEqual(list(d["per_arm"]), ["CONTROL", "CAP"])
        self.assertIn("reason=acc_cap_off", self.text(d))
        self.assertIn("Version 2: CAP = 25 m", self.text(d))

    def test_version1_control_only_log_stays_version1(self):
        for arm in ("CONTROL", "A20", "S5"):
            d = consume(diagnostic_drive(arm))["diagnostic"]
            self.assertEqual(d["versions"], [1])
            self.assertEqual(list(d["per_arm"]), ["CONTROL", "A20", "S5"])
            self.assertNotIn("Version 2:", self.text(d))

    def test_product_ceiling_without_diagnostic_uses_honest_budget(self):
        # Product default (no diag marker): shadow_boot acc_cap_m 25, a 25 m
        # LOCATION with an honest 120 m budget; the GPS return 50 m off is
        # judged against the honest budget, as for the 40 m clamp.
        rows = beta.drive(gps_offset_m=50, accuracy_e3=25000, honest_m=120)
        rows.insert(1, shadow_boot(25))
        report = consume(rows)
        check, = report["beta"]["gps_return_checks"]
        self.assertTrue(check["reported_accuracy_clamped"] and check["within_honest_budget"])
        self.assertEqual(check["honest_accuracy_m"], 120)
        self.assertNotIn("beta_return_exceeds_accuracy", {i["code"] for i in report["issues"]})
        self.assertNotIn("diagnostic", report)
        # acc-cap-off (acc_cap_m 0) or an older boot without the field: 25 m is
        # an honest report again and the return exceeds it.
        for extra in ([shadow_boot(0)], []):
            rows = beta.drive(gps_offset_m=50, accuracy_e3=25000, honest_m=120)
            rows[1:1] = extra
            report = consume(rows)
            self.assertFalse(report["beta"]["gps_return_checks"][0]["reported_accuracy_clamped"])
            self.assertIn("beta_return_exceeds_accuracy", {i["code"] for i in report["issues"]})

    def test_version2_rows_are_strict(self):
        assigned = episode("assigned", S, "CAP")
        for key in ("cap_m", "sat_in_use"):
            del assigned[key]
        assigned.update(perm=0, ceiling_m=25)
        self.assertTrue(audit.diag_row_valid(assigned))
        self.assertTrue(audit.diag_row_valid(diag_boot_v2()))
        for row in (dict(assigned, perm=3), dict(assigned, ceiling_m=41), dict(assigned, arm="S5"),
                    dict(assigned, cap_m=20), dict(diag_boot_v2(), arms=["CONTROL", "A20", "S5"]),
                    dict(diag_boot_v2(), version=3), dict(diag_boot_v2(), sat_in_use=5),
                    dict(diag_boot_v2(), control_ceiling_m=25), dict(diag_boot(), arms=["CONTROL", "CAP"]),
                    dict(episode("assigned", S, "CAP"))):
            self.assertFalse(audit.diag_row_valid(row), row)

    def test_cli_json_and_text_with_synthetic_journal(self):
        with tempfile.TemporaryDirectory(dir=ROOT) as tmp:
            path = Path(tmp) / "trace.0.jsonl"
            path.write_text("".join(json.dumps(r) + "\n" for r in diagnostic_drive()))
            for args in ([str(path)], ["--json", str(path)]):
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    audit.main(args)
                if "--json" in args:
                    self.assertIn("diagnostic", json.loads(out.getvalue()))
                else:
                    self.assertIn("Tunnel diagnostic", out.getvalue())

    def test_legacy_reports_byte_identical_to_5763025(self):
        # Content-addressed baseline also works in a history-free scratch copy.
        blob = subprocess.run(["git", "cat-file", "blob", "93ab19de0fb3d330c720aba74b219ac569f41452"], cwd=ROOT,
                              capture_output=True, text=True)
        if blob.returncode:
            self.skipTest("baseline analyzer blob unavailable (history-free checkout)")
        source = blob.stdout
        baseline = types.ModuleType("baseline_analyzer")
        exec(compile(source, "baseline_analyzer", "exec"), baseline.__dict__)
        fixtures = [legacy.records(), legacy.records(choice=1), beta.drive(), beta.nofix_drive(),
                    beta.drive(accuracy_e3=40000, honest_m=120),
                    [dict(r, choice=5) if r["kind"] == "send" else r for r in beta.drive()]]
        with tempfile.TemporaryDirectory(dir=ROOT) as tmp:
            path = Path(tmp) / "trace.0.jsonl"
            for rows in fixtures:
                self.assertEqual(json.dumps(consume(copy.deepcopy(rows))),
                                 json.dumps(consume(copy.deepcopy(rows), baseline)))
                path.write_text("".join(json.dumps(r) + "\n" for r in rows))
                for args in ([str(path)], ["--json", str(path)]):
                    outputs = []
                    for module in (baseline, audit):
                        out = io.StringIO()
                        with contextlib.redirect_stdout(out):
                            code = module.main(args)
                        outputs.append((code, out.getvalue().encode()))
                    self.assertEqual(outputs[0], outputs[1])


if __name__ == "__main__":
    unittest.main()
