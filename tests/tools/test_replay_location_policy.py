"""Synthetic counterfactual tests only; no OEM or phone execution."""
import copy
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import replay_location_policy as replay


def payload():
    raw = bytearray(48)
    struct.pack_into("<ii", raw, 8, 351234567, 1291234567)
    raw[32] = raw[40] = 1
    struct.pack_into("<i", raw, 36, 12000)
    struct.pack_into("<i", raw, 44, 45000000)
    raw[33:36] = b"pad"
    raw[41:44] = b"PAD"
    return bytes(raw)


def boot():
    return dict(kind="boot", schema=1, pid=123, mono_ns=0, mode=1,
                install="ok", assist_ready=False, assist_block="unverified",
                wire_timestamp_modified=False)


def health(now=10_000_000_000):
    return dict(kind="health", mono_ns=now, dropped=0, hook_installed=True,
                assist_ready=False, runtime_mode=1, audit_fault=0)


def pair(call=1, mode=0, now=1_000_000_000, raw=None):
    raw = payload() if raw is None else raw
    return [dict(kind="position", call=call, generation=2, mono_ns=now-1,
                 mode=mode, utc_s=0, lat=35.1234567, lon=129.1234567,
                 heading=45, kmh=43.2),
            dict(kind="send", call=call, generation=2, mono_ns=now, mode=mode,
                 type=1, length=48, choice=0, reason=5 if mode == 0 else 7,
                 result=-7, original_hex=raw.hex(), outgoing_hex=raw.hex())]


def encode(rows):
    return ("\n".join(json.dumps(r) for r in rows) + "\n").encode()


class PolicyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def run_rows(self, rows, **kwargs):
        path = self.root / "trace.0.jsonl"
        path.write_bytes(encode(rows))
        return replay.replay([path], **kwargs)

    def test_scrub_bytes_and_drop_unspecified_contract(self):
        raw = payload()
        scrub = replay.project_payload(raw, 0, "SCRUB")
        expected = bytearray(raw)
        expected[32] = expected[40] = 0
        expected[36:40] = expected[44:48] = bytes(4)
        self.assertEqual(scrub, expected)
        self.assertEqual(scrub[:32], raw[:32])
        self.assertEqual(scrub[33:36], b"pad")
        self.assertEqual(scrub[41:44], b"PAD")
        self.assertIsNone(replay.project_payload(raw, 0, "DROP"))
        self.assertEqual(replay.project_payload(raw, 0, "OBSERVE"), raw)

    def test_native_modes_never_changed_or_dropped(self):
        for mode in (1, 2, 3):
            for policy in ("OBSERVE", "SCRUB", "DROP"):
                self.assertEqual(replay.project_payload(payload(), mode, policy), payload())

    def test_bad_pure_policy_arguments(self):
        for raw, mode, policy in ((b"x", 0, "SCRUB"), (payload(), -1, "DROP"),
                                  (payload(), True, "DROP"), (payload(), 0, "ASSIST")):
            with self.assertRaises(ValueError):
                replay.project_payload(raw, mode, policy)

    def test_mode_windows_counts_and_no_phone_claim(self):
        rows = [boot(), *pair(), *pair(2, now=2_000_000_000),
                *pair(3, mode=3, now=3_000_000_000), health()]
        report = self.run_rows(rows)
        self.assertEqual(report["status"], "local_checks_pass")
        self.assertTrue(report["projection_available"])
        self.assertFalse(report["live_activation_allowed"])
        self.assertEqual((report["mode0_samples"], report["mode3_samples"]), (2, 1))
        a, b = report["windows"]
        self.assertEqual(a["observed_span_seconds"], 1)
        self.assertEqual(a["repeated_coordinate_pairs"], 1)
        self.assertEqual(a["zero_wire_timestamp_samples"], 2)
        self.assertEqual(a["counterfactual"]["DROP"]["omitted"], 2)
        self.assertIsNone(a["counterfactual"]["DROP"]["omitted_call_return"])
        self.assertIsNone(a["counterfactual"]["DROP"]["omitted_call_errno"])
        self.assertEqual(b["counterfactual"]["DROP"]["forwarded"], 1)
        self.assertEqual(b["observed_span_seconds"], 0)
        self.assertEqual(report["location_lower_send_results_by_mode"], {"0": {"-7": 2}, "3": {"-7": 1}})
        self.assertEqual(report["phone_fallback"], "not_established")
        self.assertEqual(report["assist_input_qualification"], "not_established")
        text = json.dumps(report)
        self.assertNotIn(payload().hex(), text)
        self.assertNotIn("35.1234567", text)

    def test_noop_scrub_distinguished_from_selected(self):
        raw = replay.project_payload(payload(), 0, "SCRUB")
        report = self.run_rows([boot(), *pair(raw=raw), health()])
        window = report["windows"][0]
        self.assertEqual(window["counterfactual"]["SCRUB"]["changed_payloads"], 0)
        self.assertEqual(window["counterfactual"]["DROP"]["omitted"], 1)

    def test_gap_splits_without_extrapolated_duration(self):
        report = self.run_rows([boot(), *pair(), *pair(2, now=5_000_000_000), health()])
        self.assertEqual(len(report["windows"]), 2)
        self.assertEqual([w["observed_span_seconds"] for w in report["windows"]], [0, 0])

    def test_boots_never_join_and_call_ids_can_restart(self):
        rows = [boot(), *pair(), health(), boot(), *pair(), health()]
        report = self.run_rows(rows)
        self.assertEqual(report["status"], "local_checks_pass")
        self.assertEqual([w["session_index"] for w in report["windows"]], [1, 2])

    def test_health_fault_withholds_entire_projection(self):
        for key, value in (("audit_fault", 1), ("dropped", 1), ("runtime_mode", 2),
                           ("hook_installed", False)):
            rows = [boot(), *pair(), health()]
            rows[-1][key] = value
            with self.subTest(key=key):
                report = self.run_rows(rows)
                self.assertEqual(report["status"], "inconclusive")
                self.assertFalse(report["projection_available"])
                self.assertTrue(all(w["counterfactual"] is None for w in report["windows"]))
                self.assertNotIn("scrub_would_change_samples", report["windows"][0])

    def test_legacy_missing_explicit_health_is_inconclusive(self):
        for key in ("runtime_mode", "audit_fault"):
            rows = [boot(), *pair(), health()]
            del rows[-1][key]
            self.assertEqual(self.run_rows(rows)["status"], "inconclusive")

    def test_missing_boot_health_context_or_tail_blocks(self):
        rows = [boot(), *pair(), health()]
        for subset in (rows[1:], rows[:-1], [rows[0], rows[2], rows[3]],
                       [*rows[:-1], health(now=2)]):
            self.assertFalse(self.run_rows(subset)["projection_available"])

    def test_actual_mutation_is_not_an_observe_counterfactual(self):
        rows = [boot(), *pair(), health()]
        rows[0]["mode"] = 2
        rows[2].update(choice=1, reason=0, outgoing_hex=replay.project_payload(payload(), 0, "SCRUB").hex())
        rows[-1]["runtime_mode"] = 2
        report = self.run_rows(rows)
        self.assertEqual(report["status"], "inconclusive")
        self.assertIn("requires_observe_boot", report["replay_issue_counts"])

    def test_reason_context_unknown_mode_and_flags_block(self):
        for key, value in (("reason", 1), ("reason", 2), ("reason", 3), ("mode", -1)):
            rows = [boot(), *pair(), health()]
            rows[2][key] = value
            self.assertFalse(self.run_rows(rows)["projection_available"])
        raw = bytearray(payload()); raw[32] = 2
        self.assertFalse(self.run_rows([boot(), *pair(raw=raw), health()])["projection_available"])

    def test_unsupported_length_and_duplicate_request_block(self):
        rows = [boot(), *pair(), health()]
        duplicate = copy.deepcopy(rows[2]); duplicate["mono_ns"] += 1
        report = self.run_rows([*rows[:-1], duplicate, rows[-1]])
        self.assertIn("multiple_location_sends_for_request", report["replay_issue_counts"])
        rows[2]["length"] = 47
        self.assertFalse(self.run_rows(rows)["projection_available"])

    def test_time_regression_and_position_after_send_block(self):
        report = self.run_rows([boot(), *pair(now=2_000_000_000), *pair(2), health()])
        self.assertIn("nonmonotonic_send_time", report["replay_issue_counts"])
        rows = [boot(), *pair(), health()]; rows[1]["mono_ns"] = rows[2]["mono_ns"] + 1
        self.assertIn("invalid_position_time_context", self.run_rows(rows)["replay_issue_counts"])

    def test_non_location_separate_histogram_and_window(self):
        other = dict(pair(2, now=2_000_000_000)[1], type=2, length=8, result=1234)
        report = self.run_rows([boot(), *pair(), other, *pair(3, now=3_000_000_000), health()])
        self.assertEqual(report["status"], "local_checks_pass")
        self.assertEqual(report["non_location_sends_observed"], 1)
        self.assertEqual(len(report["windows"]), 2)
        self.assertEqual(report["location_lower_send_results_by_mode"], {"0": {"-7": 2}})

    def test_corrupt_tail_and_violation_keep_parent_exit_semantics(self):
        path = self.root / "trace.0.jsonl"
        path.write_bytes(encode([boot(), *pair(), health()]) + b'{"kind":')
        self.assertEqual(replay.replay([path])["status"], "inconclusive")
        rows = [boot(), *pair(), health()]; rows[2]["outgoing_hex"] = "00" * 48
        self.assertEqual(self.run_rows(rows)["status"], "violation")

    def test_rotation_tar_without_extraction(self):
        rows = [boot(), *pair(), health()]
        path = self.root / "logs.tar"
        with tarfile.open(path, "w") as archive:
            for name, part in (("logs/trace.0.jsonl", rows[2:]), ("logs/trace.1.jsonl", rows[:2])):
                data = encode(part); item = tarfile.TarInfo(name); item.size = len(data)
                archive.addfile(item, io.BytesIO(data))
        report = replay.replay([path])
        self.assertEqual(report["status"], "local_checks_pass")
        self.assertFalse((self.root / "logs").exists())

    def test_budget_limits_block_without_silent_truncation(self):
        for name, limit in (("MAX_WINDOWS", 1), ("MAX_REQUESTS", 1)):
            with patch.object(replay, name, limit):
                report = self.run_rows([boot(), *pair(), *pair(2, mode=3, now=2_000_000_000), health()])
                self.assertEqual(report["status"], "inconclusive")
                self.assertFalse(report["projection_available"])

    def test_bad_gap_and_cli_status(self):
        for gap in (0, -1, float("nan"), float("inf"), 3600001):
            with self.assertRaises(ValueError):
                replay.replay([], gap)
        for rows, expected in (([boot(), *pair(), health()], 0), ([boot(), *pair()], 2)):
            path = self.root / "trace.0.jsonl"; path.write_bytes(encode(rows))
            proc = subprocess.run([sys.executable, str(ROOT / "tools/replay_location_policy.py"), str(path)],
                                  capture_output=True, text=True)
            self.assertEqual(proc.returncode, expected, proc.stderr)
            self.assertFalse(json.loads(proc.stdout)["live_activation_allowed"])


if __name__ == "__main__":
    unittest.main()
