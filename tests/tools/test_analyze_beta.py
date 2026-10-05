"""PC-only synthetic BETA journal tests; they do not exercise a CMU, a phone or AA receipt."""
import contextlib
import importlib.util
import io
import json
import math
from pathlib import Path
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[2] / "tools" / "analyze_logs.py"
spec = importlib.util.spec_from_file_location("analyze_logs_beta", TOOL)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

LAT, LON = 37.5, 127.0
# One metre of latitude in degrees on the analyzer's sphere.
M_LAT = math.degrees(1.0 / module.EARTH_RADIUS_M)


def put32(buf, offset, value, signed=False):
    buf[offset:offset + 4] = int(value).to_bytes(4, "little", signed=signed)


def original_payload(lat=LAT, lon=LON):
    # Stale mode-0 original: last coordinates, no accuracy/speed/bearing.
    buf = bytearray(48)
    buf[0:8] = (1700000000123456789).to_bytes(8, "little")
    put32(buf, 8, round(lat * 1e7), True)
    put32(buf, 12, round(lon * 1e7), True)
    buf[24:32] = bytes(range(24, 32))  # altitude block kept verbatim
    return bytes(buf)


def beta_payload(original, lat, lon, accuracy_e3=12000, speed_mps=10.0, bearing_deg=0.0):
    buf = bytearray(original)
    put32(buf, 8, round(lat * 1e7), True)
    put32(buf, 12, round(lon * 1e7), True)
    buf[16] = 1
    put32(buf, 20, accuracy_e3)
    buf[32] = 1
    put32(buf, 36, round(speed_mps * 1000))
    buf[40] = 1
    put32(buf, 44, round(bearing_deg * 1e6))
    return bytes(buf)


def boot(mode=5, enabled=True, install="ok"):
    row = dict(kind="boot", schema=1, pid=123, mono_ns=10, mode=mode, install=install,
               assist_ready=False, assist_block="sensor_timing_quality_calibration_unverified",
               wire_timestamp_modified=False, session_hooks="declined_third_party_interposer")
    if mode == 5:
        row["beta"] = dict(mode="BETA", enabled=enabled,
                           reason="adapter_opt_in" if enabled else "hook_not_installed",
                           session_fence="declined_send_storage_counter")
    else:
        row["beta"] = dict(mode="off", enabled=False, reason="not_requested", session_fence="none")
    return row


def state(mono_ns, old, new, reason, accuracy=None):
    return dict(kind="beta_state", mono_ns=mono_ns, domain="beta", assist_ready=False,
                **{"from": old}, to=new, reason=reason, adapter_mode=4, generation=1,
                source_epoch=1, session_epoch=1, held=False, bridge="OK",
                accuracy_m=accuracy, valid_until_ns=0)


def position(call, mono_ns, mode, lat=LAT, lon=LON):
    return dict(kind="position", call=call, generation=1, mono_ns=mono_ns, mode=mode,
                utc_s=100, lat=lat, lon=lon, heading=0.0, kmh=36.0, horizontal=5.0)


def send(call, mono_ns, mode, original, outgoing=None, choice=0, result=0):
    outgoing = original if outgoing is None else outgoing
    return dict(kind="send", call=call, generation=1, mono_ns=mono_ns, mode=mode, type=1,
                length=48, choice=choice, reason=0, result=result,
                original_hex=original.hex(), outgoing_hex=outgoing.hex())


def health(mono_ns):
    return dict(kind="health", mono_ns=mono_ns, dropped=0, hook_installed=True, assist_ready=False)


def drive(gps_offset_m=5.0, accuracy_e3=12000):
    """GPS fix -> loss -> ENGAGED with two replaced sends -> GPS return."""
    stale = original_payload()
    gps = original_payload()
    dr1 = beta_payload(stale, LAT + 10 * M_LAT, LON, accuracy_e3)
    dr2 = beta_payload(stale, LAT + 20 * M_LAT, LON, accuracy_e3)
    # The car keeps going north at 10 m/s; the first fix is 1 s after the last
    # replaced send, offset east by gps_offset_m.
    east = gps_offset_m / (module.EARTH_RADIUS_M * math.cos(math.radians(LAT))) * 180 / math.pi
    fix_lat, fix_lon = LAT + 30 * M_LAT, LON + east
    return [
        boot(),
        state(20, "DISABLED", "ARMED", "enabled"),
        position(1, 1_000_000_000, 1), send(1, 1_000_000_100, 1, gps),
        position(2, 2_000_000_000, 0), state(2_000_000_200, "ARMED", "GPS_LOST", "gps_lost"),
        send(2, 2_000_000_100, 0, stale),
        state(2_500_000_000, "GPS_LOST", "ENGAGED", "published", 11.5),
        position(3, 3_000_000_000, 0), send(3, 3_000_000_100, 0, stale, dr1, choice=3),
        position(4, 4_000_000_000, 0), send(4, 4_000_000_100, 0, stale, dr2, choice=3),
        dict(kind="beta_summary", mono_ns=4_100_000_000, domain="beta", assist_ready=False,
             state="ENGAGED", reason="published", adapter_mode=4, held=False, publications=3,
             publish_skipped=0, last_skip="none", withdrawals=0, replaced_sends=2,
             replaced_nonzero=0, original_mode0_sends=1, transitions=3, bridge="OK",
             accuracy_m=11.5, frontier_ns=4_000_000_000, valid_until_ns=4_500_000_000,
             source_epoch=1, session_epoch=1, generation=1),
        position(5, 5_000_000_100, 1, fix_lat, fix_lon),
        state(5_000_000_300, "ENGAGED", "ARMED", "gps_returned"),
        send(5, 5_000_000_200, 1, original_payload(fix_lat, fix_lon)),
        health(6_000_000_000),
    ]


class BetaAnalyzeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "trace.0.jsonl"

    def audit(self, rows):
        self.path.write_text("".join(json.dumps(row) + "\n" for row in rows))
        return module.analyze([self.path])

    def codes(self, report, severity=None):
        return {i["code"] for i in report["issues"] if severity in (None, i["severity"])}

    def test_clean_beta_drive_reports_engagement_and_gps_return_distance(self):
        report = self.audit(drive())
        self.assertEqual(report["status"], "local_checks_pass", report["issues"])
        beta = report["beta"]
        self.assertEqual(beta["replaced_sends"], 2)
        self.assertEqual(beta["engaged_periods"], 1)
        self.assertEqual(beta["engaged_exit_reasons"], {"gps_returned": 1})
        self.assertAlmostEqual(beta["engaged_seconds"]["max"], 2.5000003, places=5)
        self.assertEqual(beta["boots"][0]["session_fence"], "declined_send_storage_counter")
        self.assertEqual(report["send_choices"]["BETA_REPLACEMENT"], 2)
        check, = beta["gps_return_checks"]
        self.assertEqual(check["reported_accuracy_m"], 12.0)
        self.assertAlmostEqual(check["gap_s"], 1.0, places=2)
        # Raw distance includes 1 s of travel; time-aligned leaves the offset.
        self.assertAlmostEqual(check["distance_m"], math.hypot(10, 5), delta=0.3)
        self.assertAlmostEqual(check["time_aligned_distance_m"], 5.0, delta=0.3)
        self.assertTrue(check["within_reported_accuracy"])
        self.assertFalse(beta["gps_is_ground_truth"])

    def test_text_report_prints_the_owner_line_and_return_measurement(self):
        self.path.write_text("".join(json.dumps(row) + "\n" for row in drive()))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(module.main([str(self.path)]), 0)
        text = out.getvalue()
        self.assertIn("BETA: engaged 1 times", text)
        self.assertIn("replaced 2 sends", text)
        self.assertIn("last state ARMED (gps_returned)", text)
        self.assertIn("BETA GPS return: last DR vs first GPS fix", text)
        self.assertIn("reported accuracy 12.0 m -> within", text)

    def test_return_beyond_reported_accuracy_is_flagged_not_hidden(self):
        report = self.audit(drive(gps_offset_m=30.0))
        self.assertIn("beta_return_exceeds_accuracy", self.codes(report, "inconclusive"))
        self.assertFalse(report["beta"]["gps_return_checks"][0]["within_reported_accuracy"])

    def replace_send(self, rows, call, **changes):
        for row in rows:
            if row["kind"] == "send" and row["call"] == call:
                row.update(changes)
        return rows

    def test_replacement_must_keep_original_mode0_and_header_fields(self):
        rows = drive()
        stale = original_payload()
        bad = bytearray(beta_payload(stale, LAT, LON))
        bad[0] ^= 1          # time block
        bad[27] ^= 1         # altitude block
        bad[16] = 0          # hasAccuracy
        self.replace_send(rows, 3, outgoing_hex=bytes(bad).hex())
        report = self.audit(rows)
        self.assertEqual(report["status"], "violation")
        details = [i["detail"] for i in report["issues"] if i["code"] == "beta_payload_mismatch"]
        self.assertTrue(any("0..7 and 24..31" in d for d in details), details)
        self.assertTrue(any("hasAccuracy" in d for d in details), details)

    def test_replacement_of_a_gps_fix_is_a_violation(self):
        rows = drive()
        for row in rows:
            if row.get("call") == 3:
                row["mode"] = 1
        self.assertIn("beta_wrong_mode", self.codes(self.audit(rows), "violation"))

    def test_accuracy_bounds(self):
        for accuracy in (0, 40001):
            rows = drive()
            stale = original_payload()
            self.replace_send(rows, 3, outgoing_hex=beta_payload(stale, LAT, LON, accuracy).hex())
            self.assertIn("beta_accuracy_out_of_range", self.codes(self.audit(rows), "violation"))
        rows = drive(accuracy_e3=40000)
        self.assertNotIn("beta_accuracy_out_of_range", self.codes(self.audit(rows)))

    def test_replacement_without_engaged_or_gps_lost_state_is_an_error(self):
        rows = [r for r in drive() if r["kind"] != "beta_state" or r["to"] == "ARMED"]
        self.assertIn("beta_replacement_without_engagement", self.codes(self.audit(rows), "violation"))

    def test_replacement_after_gps_return_is_an_error(self):
        rows = drive()
        stale = original_payload()
        late = [position(6, 5_000_000_150, 0),
                send(6, 5_000_000_160, 0, stale, beta_payload(stale, LAT, LON), choice=3)]
        index = next(i for i, r in enumerate(rows) if r["kind"] == "position" and r["call"] == 5)
        rows[index + 1:index + 1] = late  # before the worker's ARMED row
        self.assertIn("beta_replacement_after_gps_return", self.codes(self.audit(rows), "violation"))

    def test_send_decided_before_a_journaled_withdrawal_is_tolerated(self):
        stale = original_payload()
        base = [boot(), state(20, "DISABLED", "ARMED", "enabled"),
                position(1, 100, 0), state(110, "ARMED", "GPS_LOST", "gps_lost"),
                state(120, "GPS_LOST", "ENGAGED", "published", 10.0),
                state(500, "ENGAGED", "WITHDRAWN", "sensor_silence")]
        for mono_ns, expected in ((490, False), (510, True)):
            rows = base + [position(2, mono_ns - 5, 0),
                           send(2, mono_ns, 0, stale, beta_payload(stale, LAT, LON), choice=3),
                           health(1000)]
            codes = self.codes(self.audit(rows), "violation")
            self.assertEqual("beta_replacement_without_engagement" in codes, expected, mono_ns)

    def test_withdrawal_reasons_and_hold_counts_are_summarized(self):
        rows = drive()
        rows[-1:-1] = [state(5_100_000_000, "ARMED", "GPS_LOST", "gps_lost"),
                       state(5_200_000_000, "GPS_LOST", "ENGAGED", "published", 9.0),
                       dict(kind="beta_hold", mono_ns=5_300_000_000, domain="beta", event="hold_set",
                            count=2, held=True, state="ENGAGED"),
                       state(5_300_000_001, "ENGAGED", "WITHDRAWN", "send_result_hold"),
                       dict(kind="beta_session_storage", mono_ns=5_400_000_000, domain="beta",
                            session_epoch=2, previous=1, state="WITHDRAWN", generation=3)]
        report = self.audit(rows)
        beta = report["beta"]
        self.assertEqual(beta["engaged_periods"], 2)
        self.assertEqual(beta["withdraw_reasons"], {"send_result_hold": 1})
        self.assertEqual(beta["engaged_exit_reasons"], {"gps_returned": 1, "send_result_hold": 1})
        self.assertEqual(beta["hold_events"]["hold_set"], 2)
        self.assertEqual(beta["session_storage_changes"], 1)
        self.assertEqual(beta["last_state"]["state"], "WITHDRAWN")

    def test_unfinished_engaged_period_is_inconclusive(self):
        rows = [r for r in drive() if not (r["kind"] == "beta_state" and r["to"] == "ARMED"
                                           and r["reason"] == "gps_returned")]
        rows = [r for r in rows if not (r["kind"] == "position" and r["call"] == 5) and
                not (r["kind"] == "send" and r["call"] == 5)]
        report = self.audit(rows)
        self.assertIn("beta_engaged_unfinished", self.codes(report, "inconclusive"))
        self.assertEqual(report["beta"]["open_engaged_periods"], 1)

    def test_beta_needs_beta_configuration(self):
        rows = drive()
        rows[0] = boot(mode=4)
        codes = self.codes(self.audit(rows), "violation")
        self.assertIn("beta_without_beta_config", codes)
        self.assertIn("beta_row_without_beta_config", codes)

    def test_boot_object_is_checked_and_assist_stays_unready(self):
        rows = drive()
        rows[0]["beta"]["mode"] = "off"
        self.assertIn("beta_boot_mode_mismatch", self.codes(self.audit(rows), "violation"))
        rows = drive()
        rows[0] = boot(enabled=True, install="symbol_missing")
        self.assertIn("beta_enabled_without_hook", self.codes(self.audit(rows), "violation"))
        rows = drive()
        rows[0] = boot(enabled=False)
        self.assertIn("beta_not_enabled", self.codes(self.audit(rows), "inconclusive"))
        rows = drive()
        rows[1]["assist_ready"] = True
        self.assertIn("impossible_live_capability", self.codes(self.audit(rows), "violation"))
        rows = drive()
        rows[0]["assist_ready"] = True
        self.assertIn("impossible_live_capability", self.codes(self.audit(rows), "violation"))

    def test_assist_mode_and_unknown_choice_stay_rejected(self):
        rows = drive()
        rows[0]["mode"] = 3
        self.assertIn("unexpected_boot_mode", self.codes(self.audit(rows), "violation"))
        rows = drive()
        self.replace_send(rows, 3, choice=4)
        self.assertIn("unknown_send_choice", self.codes(self.audit(rows), "violation"))
        rows = drive()
        self.replace_send(rows, 3, choice=2)
        self.assertIn("dr_replacement_impossible", self.codes(self.audit(rows), "violation"))

    def test_declined_session_fence_admits_unobserved_model_input_only_for_beta(self):
        # Row shape copied from the runtime BETA worker test journal.
        session = dict(kind="shadow_session", mono_ns=15, domain="model", assist_ready=False, reset=False,
                       input_available=True, model_session_epoch=1, raw_since_ns=15,
                       session=dict(result="unobserved", basis="unique_live_context", lifetime=None,
                                    revision=None, event=None, state=None))
        rows = drive()
        rows.insert(1, session)
        self.assertNotIn("model_session_malformed", self.codes(self.audit(rows)))
        for first in (boot(mode=4), dict(boot(), beta=dict(boot()["beta"], session_fence="observed_session_and_send_storage_counter"))):
            rows = [first, dict(session)] + [r for r in drive()[1:] if not r["kind"].startswith("beta")
                                             and r.get("choice", 0) != 3]
            self.assertIn("model_session_malformed", self.codes(self.audit(rows)))

    def test_fault_is_sticky(self):
        rows = drive()
        rows[-1:-1] = [state(5_100_000_000, "ARMED", "FAULT", "audit_fault"),
                       state(5_200_000_000, "FAULT", "ARMED", "enabled")]
        self.assertIn("beta_fault_not_sticky", self.codes(self.audit(rows), "violation"))


if __name__ == "__main__":
    unittest.main()
