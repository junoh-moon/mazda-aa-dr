"""Synthetic raw-journal -> production navigation tests, never vehicle validation."""
import copy
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "replay_navigation.py"
spec = importlib.util.spec_from_file_location("replay_navigation", TOOL)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def encode(rows):
    return ("\n".join(json.dumps(r, allow_nan=False) for r in rows) + "\n").encode()


def fixture(seconds=55, scale=1.02, offset=0):
    rows = [dict(kind="boot", schema=1, pid=123, mono_ns=offset+10, mode=4,
                 install="ok", assist_ready=False, assist_block="unverified", wire_timestamp_modified=False),
            dict(kind="shadow_boot", domain="model", source="vbs_callback_tap", active=True,
                 motion_sampling=False, assist_ready=False)]
    seq = 0
    for tick in range(seconds*10+1):
        ns = offset + 1_000_000_000 + tick*100_000_000
        rows.append(dict(kind="position", call=tick+1, generation=1, mono_ns=ns, mode=1,
                         utc_s=1700000000+tick//10, lat=35+tick*scale/111320,
                         lon=135, heading=0, kmh=36*scale))
        events = []
        for sensor in (1, 3, 2):
            seq += 1
            raw = [2047, 0, 0, 0] if sensor == 2 else [13600]*4
            events.append([sensor, seq, ns, 0, *raw, 1, 0])
        rows.append(dict(kind="motion_batch", schema=1, epoch=1,
                         producer_time_status="unknown", events=events))
    last = rows[-2]
    payload = bytes(range(48)).hex()
    rows.extend([dict(kind="send", call=last["call"], generation=1, mono_ns=last["mono_ns"]+1,
                      mode=1, type=1, length=48, choice=0, reason=0, result=0,
                      original_hex=payload, outgoing_hex=payload),
                 dict(kind="health", mono_ns=last["mono_ns"]+100_000_000, dropped=0,
                      hook_installed=True, assist_ready=False, audit_fault=0, runtime_mode=4)])
    return rows


class ReplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "build/replay_navigation"], cwd=ROOT, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def replay(self, rows=None, raw=None):
        path = self.root / "trace.0.jsonl"
        path.write_bytes(raw if raw is not None else encode(rows if rows is not None else fixture()))
        return module.replay([path])

    def test_actual_navigation_learns_and_compares_same_references(self):
        report = self.replay()
        self.assertEqual(report["status"], "comparison_produced", report)
        result = report["comparison"]
        self.assertGreaterEqual(result["paired_windows"], 2)
        self.assertGreater(result["paired_samples"], 100)
        self.assertEqual(result["variants"]["fixed"]["wheel_scale"], 1)
        self.assertEqual(result["variants"]["fixed"]["wheel_scale_version"], 0)
        self.assertGreater(result["variants"]["adaptive"]["wheel_scale_version"], 0)
        self.assertAlmostEqual(result["variants"]["adaptive"]["wheel_scale"], 1.02, places=5)
        metric = result["paired_metrics"]["position_difference_m"]
        self.assertEqual(metric["fixed"]["count"], result["paired_samples"])
        self.assertAlmostEqual(metric["adaptive_minus_fixed"]["mean"],
                               metric["adaptive"]["mean"] - metric["fixed"]["mean"], places=8)
        self.assertEqual(report["time_basis"], "offline_model_receipt_order")
        self.assertFalse(report["assist_ready"])
        self.assertNotIn('"lat"', json.dumps(report))
        self.assertNotIn('"lon"', json.dumps(report))

    def test_fixed_dataset_and_incomplete_window_coverage(self):
        report = self.replay(fixture(seconds=47, scale=1.0))
        self.assertEqual(report["status"], "comparison_produced", report)
        result = report["comparison"]
        self.assertEqual(result["paired_windows"], 1)
        self.assertGreater(result["coverage"]["fixed"]["incomplete_compared_events"], 0)
        self.assertGreater(result["coverage"]["adaptive"]["incomplete_windows"], 0)
        metric = result["paired_metrics"]["position_difference_m"]
        self.assertAlmostEqual(metric["adaptive_minus_fixed"]["mean"], 0, places=8)

    def test_tar_reuses_audit_safety_and_rotation(self):
        rows = fixture(seconds=15)
        path = self.root / "logs.tar.gz"
        with tarfile.open(path, "w:gz") as archive:
            for name, content in (("export/trace.0.jsonl", encode(rows[100:])),
                                  ("export/trace.1.jsonl", encode(rows[:100]))):
                member = tarfile.TarInfo(name)
                member.size = len(content)
                archive.addfile(member, io.BytesIO(content))
        self.assertEqual(module.replay([path])["status"], "comparison_produced")
        with tarfile.open(path, "w:gz") as archive:
            member = tarfile.TarInfo("../trace.0.jsonl")
            content = encode(rows)
            member.size = len(content)
            archive.addfile(member, io.BytesIO(content))
        with mock.patch.object(module, "run_engine") as engine:
            report = module.replay([path])
            engine.assert_not_called()
        self.assertIn("unsafe_archive_member", {r["code"] for r in report["issues"]})

    def test_corruption_partial_multiple_sessions_and_missing_sensor_refused(self):
        base = fixture(seconds=1)
        cases = [encode(base)[:-1], encode(base)+b'{"kind":\n', encode(base+base),
                 encode(base[1:]), encode([r for r in base if r["kind"] != "shadow_boot"])]
        for change in ("epoch", "gap", "audit", "partial", "sensor", "violation"):
            rows = copy.deepcopy(base)
            if change == "epoch":
                rows[5]["epoch"] = 2
            elif change == "gap":
                rows[5]["events"][0][1] += 1
            elif change == "audit":
                rows[-1]["audit_fault"] = 1
            elif change == "partial":
                del rows[2]["lat"]
            elif change == "sensor":
                for row in rows:
                    for event in row.get("events", []):
                        if event[0] == 3:
                            event[0] = 1
            else:
                rows[-2]["outgoing_hex"] = "00"*48
            cases.append(encode(rows))
        for raw in cases:
            with self.subTest(raw=raw[-80:]), mock.patch.object(module, "run_engine") as engine:
                report = self.replay(raw=raw)
                engine.assert_not_called()
                self.assertIn(report["status"], ("inconclusive", "violation"))

    def test_uint64_and_null_protocol_stays_exact_and_ties_stable(self):
        base = 2**63 + 123
        rows = fixture(seconds=1, offset=base)
        rows[2]["lat"] = None
        auditor = module.ReplayAuditor()
        for row in rows:
            auditor.consume(row, "fixture")
        self.assertEqual(auditor.replay_report()["status"], "local_checks_pass")
        output = io.BytesIO()
        module.write_protocol(list(reversed(auditor.replay_events)), output)
        lines = output.getvalue().decode().splitlines()
        self.assertEqual(lines[0].split()[1], str(base+1_000_000_000))
        self.assertEqual(lines[0].split()[4], "null")
        self.assertEqual([line.split()[0] for line in lines[:4]], ["P", "M", "M", "M"])
        self.assertEqual([line.split()[3] for line in lines[1:4]], ["1", "2", "3"])
        report = self.replay(fixture(seconds=15, offset=base))
        self.assertEqual(report["status"], "comparison_produced", report)
        self.assertGreater(report["comparison"]["engine"]["last_received_ns"], 2**63)

    def test_real_gap_and_old_derived_abort_are_not_input_corruption(self):
        rows = fixture(seconds=15)
        rows[-4]["mode"] = 0
        rows[-2]["mode"] = 0
        marker = dict(kind="shadow_holdout", mono_ns=rows[-1]["mono_ns"], domain="model",
                      assist_ready=False, time_basis="receipt_model", event="ABORT", reason="REAL_GAP",
                      window_id=0, anchor_ns=0, reference_ns=0, frontier_ns=0, calibration_version=0,
                      model_valid=False, yaw_zero=2047, lat=None, lon=None, ref_lat=None, ref_lon=None,
                      position_error_m=None, heading_error_rad=None)
        rows.insert(-1, marker)
        report = self.replay(rows)
        self.assertEqual(report["audit"]["status"], "inconclusive")
        self.assertEqual(report["status"], "comparison_produced", report)
        self.assertEqual(report["nonblocking_audit_issues"], 1)

    def test_limits_stop_buffering_and_never_execute(self):
        with mock.patch.object(module, "MAX_EVENTS", 5), mock.patch.object(module, "run_engine") as engine:
            report = self.replay(fixture(seconds=1))
            engine.assert_not_called()
            self.assertEqual(report["input_events"], 0)
            self.assertIn("replay_input_limit", {r["code"] for r in report["issues"]})
        with mock.patch.object(module, "MAX_INPUT_ROWS", 3), mock.patch.object(module, "run_engine") as engine:
            self.assertEqual(self.replay(fixture(seconds=1))["status"], "inconclusive")
            engine.assert_not_called()

    def test_collector_completeness_only_is_nonblocking(self):
        for missing_boot in (False, True):
            rows = fixture(seconds=15)
            common = dict(stream="collector", collector_pid=10, observed_at_mono_ns=10,
                          producer_mono_ns=None, producer_time_status="unknown")
            if missing_boot:
                rows.append(dict(common, kind="position_poll", receipt_ns=10, mode=1, utc_s=1,
                                 lat=None, lon=None, heading=None, kmh=None, request_provenance=False))
            else:
                rows.append(dict(common, kind="collector_boot", schema=1, sample_ms=100,
                                 session_seconds=0, boot_id="fixture"))
            report = self.replay(rows)
            self.assertEqual(report["status"], "comparison_produced", report)
            self.assertEqual(report["audit"]["status"], "inconclusive")
            self.assertEqual(report["nonblocking_audit_issues"], 1)

    def test_last_position_health_and_boot_temporal_coverage(self):
        for change in ("tail", "boot"):
            rows = fixture(seconds=15)
            if change == "tail":
                rows.insert(-1, dict(rows[-4], call=99999, mono_ns=rows[-1]["mono_ns"]+1))
            else:
                rows[0]["mono_ns"] = rows[2]["mono_ns"]
            with mock.patch.object(module, "run_engine") as engine:
                report = self.replay(rows)
                self.assertEqual(report["status"], "inconclusive")
                engine.assert_not_called()

    def test_pairing_unmatched_aborted_and_invalid_engine_output(self):
        auditor = module.ReplayAuditor()
        for row in fixture(seconds=15):
            auditor.consume(row, "fixture")
        protocol = io.BytesIO()
        module.write_protocol(auditor.replay_events, protocol)
        result = subprocess.run([str(ROOT / "build" / "replay_navigation")], input=protocol.getvalue(),
                                capture_output=True, check=True)
        rows = [json.loads(raw) for raw in result.stdout.splitlines()]
        baseline = module.parse_engine(encode(rows))
        warmup = dict(next(r for r in rows if r.get("variant") == "adaptive" and r.get("event") == "BEGIN"),
                      event="ABORT", reason="bad_gps", window_id=0, anchor_ns=0, reference_ns=0, frontier_ns=0)
        repeated_aborts = module.parse_engine(encode([warmup, warmup, *rows]))
        coverage = repeated_aborts["coverage"]["adaptive"]
        self.assertEqual(coverage["warmup_aborts"], 2)
        self.assertEqual(coverage["windows"], baseline["coverage"]["adaptive"]["windows"])
        self.assertEqual(coverage["incomplete_windows"], 0)
        self.assertEqual(coverage["aborted_windows"], 0)
        compared = next(i for i, r in enumerate(rows) if r.get("variant") == "adaptive" and r.get("event") == "COMPARED")
        fewer = module.parse_engine(encode(rows[:compared]+rows[compared+1:]))
        self.assertEqual(fewer["paired_samples"], baseline["paired_samples"]-1)
        self.assertEqual(fewer["coverage"]["fixed"]["unmatched_completed_compared_events"], 1)
        self.assertEqual(fewer["coverage"]["adaptive"]["unmatched_completed_compared_events"], 0)
        aborted = copy.deepcopy(rows)
        end = next(r for r in aborted if r.get("variant") == "adaptive" and r.get("event") == "END")
        end["event"] = "ABORT"
        report = module.parse_engine(encode(aborted))
        self.assertEqual(report["paired_samples"], 0)
        self.assertGreater(report["coverage"]["adaptive"]["incomplete_compared_events"], 0)
        self.assertEqual(report["coverage"]["adaptive"]["aborted_windows"], 1)
        for field, value in (("frontier_ns", 0), ("wheel_scale", 1.01), ("domain", "qualified"),
                             ("assist_ready", True), ("window_id", 999), ("position_error_m", -1)):
            bad = copy.deepcopy(rows)
            bad[compared][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                module.parse_engine(encode(bad))
        for field, value in (("domain", "qualified"), ("assist_ready", True), ("state", True)):
            bad = copy.deepcopy(rows)
            next(r for r in bad if r["kind"] == "summary")[field] = value
            with self.assertRaises(ValueError):
                module.parse_engine(encode(bad))
        bad = copy.deepcopy(rows)
        bad[-1]["records"] = 1.5
        with self.assertRaises(ValueError):
            module.parse_engine(encode(bad))

    def test_engine_failure_timeout_output_budget_and_absolute_path(self):
        events = [(1, 0, 0, "P", 1, 1, 1700000000, 35, 135, 0, 36)]
        with self.assertRaises(ValueError):
            module.run_engine(events, "relative-engine")
        for body, timeout in (("import time; time.sleep(10)", 0.05),
                              ("print('x'*10000)", 2), ("raise SystemExit(4)", 2)):
            engine = self.root / "engine;literal-name"
            engine.write_text("#!" + sys.executable + "\n" + body + "\n")
            engine.chmod(0o700)
            with mock.patch.object(module, "MAX_OUTPUT_BYTES", 100), self.assertRaises(ValueError):
                module.run_engine(events, engine, timeout=timeout)

    def test_cli_json_and_exit_statuses(self):
        path = self.root / "trace.0.jsonl"
        for rows, expected in ((fixture(seconds=15), 0), (fixture(seconds=1), 2)):
            path.write_bytes(encode(rows))
            result = subprocess.run([sys.executable, str(TOOL), str(path), "--json"],
                                    capture_output=True, check=False)
            self.assertEqual(result.returncode, expected, result.stderr)
            json.loads(result.stdout)
        rows[-2]["outgoing_hex"] = "00"*48
        path.write_bytes(encode(rows))
        result = subprocess.run([sys.executable, str(TOOL), str(path), "--json"], capture_output=True)
        self.assertEqual(result.returncode, 1)


if __name__ == "__main__":
    unittest.main()
