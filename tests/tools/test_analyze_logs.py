"""PC-only synthetic log tests; these do not exercise a CMU or prove AA receipt."""
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[2] / "tools" / "analyze_logs.py"
spec = importlib.util.spec_from_file_location("analyze_logs", TOOL)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def records(mode=0, choice=0):
    original = bytes(range(48))
    outgoing = bytearray(original)
    if choice == 1:
        for byte in module.CLEAR_BYTES:
            outgoing[byte] = 0
    return [
        dict(kind="boot", schema=1, pid=123, mono_ns=10, mode=2,
             install="ok", assist_ready=False,
             assist_block="sensor_timing_quality_calibration_unverified", wire_timestamp_modified=False),
        dict(kind="position", call=1, generation=2, mono_ns=20, mode=mode,
             utc_s=100, lat=35.0, lon=129.0, heading=42.0, kmh=20.0),
        dict(kind="send", call=1, generation=2, mono_ns=30, mode=mode,
             type=1, length=48, choice=choice, reason=0, result=0,
             original_hex=original.hex(), outgoing_hex=bytes(outgoing).hex()),
        dict(kind="health", mono_ns=40, dropped=0, hook_installed=True, assist_ready=False),
    ]


def encode(rows):
    return ("\n".join(json.dumps(row) for row in rows) + "\n").encode()


def collector(kind, pid=123, observed=20,
              boot_id="12345678-1234-1234-1234-123456789abc"):
    # Production stop/poll envelopes have PID and receipt time, but no boot ID.
    row = dict(stream="collector", collector_pid=pid, observed_at_mono_ns=observed,
               producer_mono_ns=None, producer_time_status="unknown", kind=kind)
    if kind == "collector_boot":
        row.update(schema=1, sample_ms=1000, session_seconds=28800, boot_id=boot_id)
    elif kind == "collector_stop":
        row.update(samples=0, reason="stop_marker")
    elif kind == "position_poll_error":
        row.update(reason="bus_unavailable")
    return row


class AnalyzeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()

    def tearDown(self):
        self.tmp.cleanup()

    def audit(self, rows=None, raw=None):
        path = self.root / "trace.0.jsonl"
        path.write_bytes(raw if raw is not None else encode(rows if rows is not None else records()))
        return module.analyze([path])

    def codes(self, report):
        return {issue["code"] for issue in report["issues"]}

    def audit_collector(self, current, older=()):
        (self.root / "trace.0.jsonl").write_bytes(encode(records()))
        (self.root / "collector.1.jsonl").write_bytes(encode(older))
        (self.root / "collector.0.jsonl").write_bytes(encode(current))
        return module.analyze([self.root])

    def test_primary_pipeline_reset_is_a_named_inconclusive_reason(self):
        reset = dict(kind='shadow_pipeline_reset', mono_ns=35, domain='model',
                     assist_ready=False, reason='LATE', operation='raw', input_ns=20,
                     receive_seq=45, sensor=2, call=0, resets=1)
        rows = records()
        rows.insert(-1, reset)
        report = self.audit(rows)
        self.assertEqual(report['status'], 'inconclusive')
        self.assertIn('shadow_pipeline_reset', self.codes(report))
        self.assertNotIn('unknown_record_kind', self.codes(report))
        reset['receive_seq'] = -1
        self.assertIn('partial_record', self.codes(self.audit(rows)))

    def test_malformed_optional_drain_counter_is_inconclusive(self):
        row = dict(kind='shadow', mono_ns=35, domain='model', assist_ready=False,
                   model_valid=False, state=0, result='E_NO_SEED', pipeline='WAITING',
                   uncertainties=0, events=0, intervals=0, resets=0, rejected=0,
                   drain_calls_total='3', frontier_ns=0, lat=None, lon=None,
                   heading_rad=None, speed_mps=None, error_model_m=None,
                   stopped=False, yaw_zero=0.0, calibration_version=0,
                   wheel_scale=1.0, wheel_scale_version=0,
                   preview_encoded=False, location_preview_hex='')
        rows = records()
        rows.insert(-1, row)
        self.assertIn('partial_record', self.codes(self.audit(rows)))

    def test_original_pass_scope_is_local(self):
        report = self.audit()
        self.assertEqual(report["status"], "local_checks_pass")
        self.assertEqual(report["phone_acceptance"], "not_established")

    def test_storage_stops_are_read_from_directory_and_export_archive(self):
        for stream in ('trace', 'collector'):
            marker = dict(kind='storage_stop', stream=stream,
                          boot_id='12345678-1234-1234-1234-123456789abc',
                          pid=123, mono_ns=50, reason='low_space', available_bytes=7340032,
                          reserve_bytes=8388608, margin_bytes=65536, syscall_errno=0)
            trace = self.root / 'trace.0.jsonl'
            trace.write_bytes(encode(records()))
            stop = self.root / (stream + '.storage.json')
            stop.write_bytes(encode([marker]))
            archive = self.root / (stream + '.tar')
            with tarfile.open(archive, 'w') as tar:
                tar.add(trace, arcname='logs/trace.0.jsonl')
                tar.add(stop, arcname='logs/' + stop.name)
            for path in (self.root, archive):
                report = module.analyze([path])
                self.assertEqual(report['status'], 'inconclusive', report)
                self.assertIn('storage_stopped', self.codes(report))
                self.assertEqual(report['storage_stops'], [marker])
                self.assertNotIn('unexpected_collector_record', self.codes(report))
            stop.unlink()

    def test_storage_stop_cannot_claim_negative_or_boolean_available_space(self):
        for available in (-1, True):
            row = dict(kind='storage_stop', stream='trace', boot_id='unknown', pid=123,
                       mono_ns=50, reason='low_space', available_bytes=available,
                       reserve_bytes=8388608, margin_bytes=65536, syscall_errno=0)
            report = self.audit(records() + [row])
            self.assertIn('partial_record', self.codes(report))
        self.assertEqual(report["dr_accuracy"], "not_established")
        self.assertEqual(report["send_choices"], {"ORIGINAL": 1})

    def test_scrub_exact_bytes(self):
        self.assertEqual(self.audit(records(choice=1))["status"], "local_checks_pass")
        rows = records(choice=1)
        outgoing = bytearray.fromhex(rows[2]["outgoing_hex"])
        outgoing[33] = 0  # Padding is not permitted to change.
        rows[2]["outgoing_hex"] = outgoing.hex()
        self.assertIn("scrub_payload_mismatch", self.codes(self.audit(rows)))

    def test_scrub_must_clear_every_target(self):
        rows = records(choice=1)
        outgoing = bytearray.fromhex(rows[2]["outgoing_hex"])
        outgoing[44] = 44
        rows[2]["outgoing_hex"] = outgoing.hex()
        self.assertEqual(self.audit(rows)["status"], "violation")

    def test_native_modes_unchanged(self):
        for mode in (1, 2, 3):
            with self.subTest(mode=mode):
                self.assertEqual(self.audit(records(mode))["status"], "local_checks_pass")
                report = self.audit(records(mode, choice=1))
                self.assertEqual(report["status"], "violation")
                self.assertIn("native_mode_mutation", self.codes(report))

    def test_original_change_is_violation(self):
        rows = records()
        rows[2]["outgoing_hex"] = "00" * 48
        self.assertIn("original_payload_changed", self.codes(self.audit(rows)))

    def test_dr_unexpected_even_when_bytes_identical(self):
        report = self.audit(records(choice=2))
        self.assertEqual(report["status"], "violation")
        self.assertIn("dr_replacement_impossible", self.codes(report))

    def test_malformed_truncated_and_duplicate_json(self):
        for tail in (b'{"kind":', b'{"kind":"health","kind":"send"}\n'):
            with self.subTest(tail=tail):
                report = self.audit(raw=encode(records()) + tail)
                self.assertEqual(report["status"], "inconclusive")
                self.assertIn("malformed_json", self.codes(report))
        report = self.audit(raw=encode(records()).rstrip(b"\n"))
        self.assertIn("partial_final_line", self.codes(report))

    def test_invalid_hex_is_inconclusive(self):
        for value in ("00" * 47, "gg" * 48, "", "00 " * 32):
            with self.subTest(value=value):
                rows = records()
                rows[2]["original_hex"] = value
                report = self.audit(rows)
                self.assertEqual(report["status"], "inconclusive")
                self.assertIn("invalid_payload_hex", self.codes(report))

    def test_incomplete_evidence_not_pass(self):
        for rows, expected in ((records()[1:], "missing_boot"),
                               (records()[:-1], "missing_health"),
                               ([records()[0], *records()[2:]], "missing_position_context")):
            with self.subTest(expected=expected):
                report = self.audit(rows)
                self.assertEqual(report["status"], "inconclusive")
                self.assertIn(expected, self.codes(report))
        rows = records()
        rows[-1]["mono_ns"] = 25
        self.assertIn("uncovered_trace_tail", self.codes(self.audit(rows)))

    def test_drops_and_install_failure(self):
        rows = records()
        rows[-1]["dropped"] = 2
        report = self.audit(rows)
        self.assertEqual(report["status"], "inconclusive")
        self.assertEqual(report["drop_health"]["max_dropped"], 2)
        rows = records()
        rows[0]["install"] = "original_bytes_mismatch"
        self.assertIn("install_not_ok", self.codes(self.audit(rows)))

    def test_counts_do_not_infer_poll_provenance(self):
        rows = records()
        rows.extend([
            dict(kind="owner_poll", receipt_ns=50, owner=":1.12", pid=456,
                 comm="jciNativeNavi", request_provenance=False),
            dict(kind="receiver_poll", receipt_ns=50, receiver=1),
            dict(kind="position_poll", receipt_ns=50, mode=2, utc_s=100,
                 lat=None, lon=None, heading=None, kmh=None, request_provenance=False),
        ])
        rows[2]["result"] = -1
        report = self.audit(rows)
        self.assertEqual(report["input_modes"], {"0": 1})
        self.assertEqual(report["position_poll_modes"], {"2": 1})
        self.assertEqual(report["owner_pid_comm"][0]["comm"], "jciNativeNavi")
        self.assertEqual(report["receiver_counts"], {"1": 1})
        self.assertEqual(report["lower_send_results"], {"-1": 1})
        self.assertEqual(report["phone_acceptance"], "not_established")

    def test_directory_rotation_read_oldest_first(self):
        rows = records()
        (self.root / "trace.1.jsonl").write_bytes(encode(rows[:2]))
        (self.root / "trace.0.jsonl").write_bytes(encode(rows[2:]))
        report = module.analyze([self.root])
        self.assertEqual(report["status"], "local_checks_pass")

    def make_tar(self, members):
        path = self.root / "logs.tar"
        with tarfile.open(path, "w") as archive:
            for name, payload, kind in members:
                member = tarfile.TarInfo(name)
                if kind == "symlink":
                    member.type = tarfile.SYMTYPE
                    member.linkname = "../../outside"
                    archive.addfile(member)
                else:
                    member.size = len(payload)
                    archive.addfile(member, io.BytesIO(payload))
        return path

    def test_tar_rotation_without_extraction(self):
        rows = records()
        path = self.make_tar([("logs/trace.0.jsonl", encode(rows[2:]), "file"),
                              ("logs/trace.1.jsonl", encode(rows[:2]), "file"),
                              ("mx5dr.conf", b"mode=SCRUB\n", "file")])
        report = module.analyze([path])
        self.assertEqual(report["status"], "local_checks_pass")
        self.assertFalse((self.root / "logs").exists())
        self.assertEqual(report["ignored_archive_members"], ["mx5dr.conf"])

    def test_tar_rejects_traversal_links_and_duplicate_members(self):
        for name, kind in (("../../escape.jsonl", "file"),
                           ("/absolute.jsonl", "file"),
                           ("logs/link.jsonl", "symlink")):
            with self.subTest(name=name):
                path = self.make_tar([(name, encode(records()), kind),
                                      ("logs/trace.0.jsonl", encode(records()), "file")])
                report = module.analyze([path])
                self.assertEqual(report["status"], "inconclusive")
                self.assertIn("unsafe_archive_member", self.codes(report))
        path = self.make_tar([("logs/a.jsonl", encode(records()), "file")] * 2)
        self.assertIn("duplicate_archive_member", self.codes(module.analyze([path])))

    def test_partial_record_and_line_limit(self):
        rows = records()
        del rows[2]["result"]
        self.assertIn("partial_record", self.codes(self.audit(rows)))
        self.assertIn("line_limit", self.codes(self.audit(raw=b" " * (module.MAX_LINE_BYTES + 1))))

    def test_scrub_configuration_and_nonfinite_json(self):
        rows = records(choice=1)
        rows[0]["mode"] = 1
        self.assertIn("scrub_without_scrub_config", self.codes(self.audit(rows)))
        report = self.audit(raw=encode(records()) + b'{"kind":"poll","value":1e999}\n')
        self.assertEqual(report["status"], "inconclusive")
        self.assertIn("malformed_json", self.codes(report))

    def test_runtime_health_fault_and_poll_qualification(self):
        rows = records()
        rows[-1].update(runtime_mode=1, audit_fault=1)
        report = self.audit(rows)
        self.assertEqual(report["status"], "inconclusive")
        self.assertIn("audit_fault", self.codes(report))
        self.assertEqual(report["actual_runtime_modes"], {"1": 1})
        self.assertEqual(report["drop_health"]["audit_fault_counts"], {"1": 1})
        base = dict(kind="poll", seq=0, begin_ns=50, end_ns=60,
                    speed_raw="1", yaw_raw="1", gear_raw="D",
                    freshness="unproven_poll", quality="unknown", assist_ready=False)
        for key, value in (("assist_ready", True), ("freshness", "fresh"), ("quality", "verified")):
            with self.subTest(key=key):
                poll = dict(base)
                poll[key] = value
                report = self.audit(records() + [poll])
                self.assertEqual(report["status"], "violation")
                self.assertIn("unexpected_poll_qualification", self.codes(report))

    def test_cli_json_and_exit_codes(self):
        for rows, expected in ((records(), 0), (records(choice=2), 1), (records()[:-1], 2)):
            path = self.root / "cli.jsonl"
            path.write_bytes(encode(rows))
            proc = subprocess.run([sys.executable, str(TOOL), "--json", str(path)],
                                  capture_output=True, text=True)
            self.assertEqual(proc.returncode, expected, proc.stderr)
            self.assertEqual(json.loads(proc.stdout)["report_schema"], 1)

    def test_collector_boot_identity_is_separate_from_request_provenance(self):
        kernel_id = "12345678-1234-1234-1234-123456789abc"
        other_id = "87654321-1234-1234-1234-123456789abc"
        for collector_id in (kernel_id, other_id):
            with self.subTest(collector_id=collector_id):
                rows = records()
                rows[0]["boot_id"] = kernel_id
                envelope = dict(stream="collector", collector_pid=123, observed_at_mono_ns=20,
                                producer_mono_ns=None, producer_time_status="unknown")
                rows.extend([
                    dict(envelope, kind="collector_boot", schema=1, sample_ms=1000,
                         session_seconds=28800, boot_id=collector_id),
                    dict(envelope, kind="collector_stop", samples=0, reason="stop_marker"),
                ])
                report = self.audit(rows)
                self.assertEqual(report["status"], "local_checks_pass")
                expected = [kernel_id] if collector_id == kernel_id else []
                self.assertEqual(report["stream_correlation"]["shared_kernel_boot_ids"], expected)
                self.assertEqual(report["collector"]["request_provenance"], "not_established")

    def test_collector_cannot_claim_hook_or_producer_time(self):
        row = dict(stream="collector", collector_pid=123, observed_at_mono_ns=20,
                   producer_mono_ns=17, producer_time_status="verified", kind="send")
        report = self.audit(records() + [row])
        self.assertEqual(report["status"], "violation")
        self.assertIn("unexpected_collector_record", self.codes(report))
        self.assertIn("unexpected_poll_qualification", self.codes(report))


    def test_collector_rotated_old_stop_cannot_close_new_session(self):
        for old_pid in (111, 222):
            with self.subTest(pid_reused=old_pid == 222):
                report = self.audit_collector(
                    [collector("collector_boot", pid=222, observed=30)],
                    older=[collector("collector_stop", pid=old_pid, observed=20)])
                self.assertEqual(report["status"], "inconclusive")
                self.assertIn("collector_missing_boot", self.codes(report))
                self.assertIn("collector_open_session", self.codes(report))

    def test_collector_boot_and_stop_across_rotation_are_one_session(self):
        report = self.audit_collector(
            [collector("position_poll_error", observed=30),
             collector("collector_stop", observed=40)],
            older=[collector("collector_boot", observed=20)])
        self.assertEqual(report["status"], "local_checks_pass")

    def test_collector_normal_sessions_allow_pid_reuse_and_new_boot_clock(self):
        other_boot = "87654321-1234-1234-1234-123456789abc"
        report = self.audit_collector([
            collector("collector_boot", observed=20),
            collector("collector_stop", observed=30),
            collector("collector_boot", observed=40),
            collector("collector_stop", observed=50),
            collector("collector_boot", observed=10, boot_id=other_boot),
            collector("collector_stop", observed=15),
        ])
        self.assertEqual(report["status"], "local_checks_pass")
        self.assertEqual(len(report["collector"]["boots"]), 3)
        self.assertEqual(len(report["collector"]["stops"]), 3)

    def test_collector_duplicate_old_stop_cannot_close_reused_pid_new_boot(self):
        report = self.audit_collector(
            [collector("collector_boot", observed=10,
                       boot_id="87654321-1234-1234-1234-123456789abc")],
            older=[collector("collector_boot", observed=20),
                   collector("collector_stop", observed=30),
                   collector("collector_stop", observed=30)])
        self.assertEqual(report["status"], "inconclusive")
        self.assertIn("collector_record_after_stop", self.codes(report))
        self.assertIn("collector_open_session", self.codes(report))

    def test_collector_stop_requires_the_current_pid(self):
        report = self.audit_collector([
            collector("collector_boot", pid=123, observed=20),
            collector("collector_stop", pid=456, observed=30),
        ])
        self.assertEqual(report["status"], "inconclusive")
        self.assertIn("collector_session_mismatch", self.codes(report))
        self.assertIn("collector_open_session", self.codes(report))

    def test_collector_orphan_rows_are_not_healed_by_later_complete_session(self):
        report = self.audit_collector(
            [collector("collector_boot", pid=222, observed=20),
             collector("collector_stop", pid=222, observed=30)],
            older=[collector("position_poll_error", pid=111, observed=10)])
        self.assertEqual(report["status"], "inconclusive")
        self.assertIn("collector_missing_boot", self.codes(report))

    def test_collector_regressed_or_malformed_stop_cannot_close_session(self):
        for change in ({"observed_at_mono_ns": 19}, {"observed_at_mono_ns": -1},
                       {"samples": "missing"}):
            with self.subTest(change=change):
                stop = collector("collector_stop", observed=30)
                stop.update(change)
                report = self.audit_collector([collector("collector_boot", observed=20), stop])
                self.assertEqual(report["status"], "inconclusive")
                self.assertIn("collector_open_session", self.codes(report))

    def test_collector_records_after_stop_are_incomplete(self):
        report = self.audit_collector([
            collector("collector_boot", observed=20),
            collector("collector_stop", observed=30),
            collector("position_poll_error", observed=40),
        ])
        self.assertEqual(report["status"], "inconclusive")
        self.assertIn("collector_record_after_stop", self.codes(report))

    def test_collector_open_session_has_no_terminal_evidence(self):
        report = self.audit_collector([collector("collector_boot")])
        self.assertEqual(report["status"], "inconclusive")
        self.assertIn("collector_open_session", self.codes(report))

    def test_collector_unknown_boot_identity_is_inconclusive(self):
        report = self.audit_collector([
            collector("collector_boot", boot_id="unknown"),
            collector("collector_stop", observed=30),
        ])
        self.assertEqual(report["status"], "inconclusive")


if __name__ == "__main__":
    unittest.main()
