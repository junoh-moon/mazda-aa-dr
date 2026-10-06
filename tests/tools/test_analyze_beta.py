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


def nofix_original(lat=LAT, lon=LON):
    # The stored no-fix LOCATION as the stock sends it (shape of the
    # 2026-10-05 rows, synthetic coordinates): utc 0, hasAccuracy 8.8 m,
    # hasSpeed 1.111 m/s, hasBearing 335 deg.
    buf = bytearray(48)
    put32(buf, 8, round(lat * 1e7), True)
    put32(buf, 12, round(lon * 1e7), True)
    buf[16] = 1
    put32(buf, 20, 8800)
    buf[24] = 1
    put32(buf, 28, 47)
    buf[32] = 1
    put32(buf, 36, 1111)
    buf[40] = 1
    put32(buf, 44, 335000000)
    return bytes(buf)


def overlay_payload(original, speed_e3):
    buf = bytearray(original)
    buf[32] = 1
    put32(buf, 36, speed_e3)
    return bytes(buf)


def nofix_position(call, mono_ns, utc_s=0):
    return dict(kind="position", call=call, generation=5, mono_ns=mono_ns, mode=1, utc_s=utc_s,
                lat=LAT, lon=LON, heading=335.0, kmh=4.0, altitude_m=47, horizontal=4.4,
                vertical=9.7, reason=0, **{"class": 1 if utc_s == 0 else 2})


def nofix_send(call, mono_ns, outgoing=None, choice=0, reason=0, result=0, original=None, cls=1):
    original = nofix_original() if original is None else original
    outgoing = original if outgoing is None else outgoing
    return dict(kind="send", call=call, generation=5, mono_ns=mono_ns, mode=1, type=1, length=48,
                choice=choice, reason=reason, result=result, original_hex=original.hex(),
                outgoing_hex=outgoing.hex(), **{"class": cls})


def nofix_state(mono_ns, old, new, reason, speed=None, payload="none"):
    # Field set of the runtime beta_state row (src/runtime/beta_controller.h).
    return dict(kind="beta_state", mono_ns=mono_ns, domain="beta", assist_ready=False,
                **{"from": old}, to=new, reason=reason, adapter_mode=4, generation=5,
                source_epoch=1, session_epoch=1, held=False, bridge="NO_OUTPUT", accuracy_m=0,
                valid_until_ns=0, position_class="NO_FIX", payload=payload, original_utc_s=0,
                original_accuracy_m=8.8, speed_mps=speed, core_result="OK")


def wheel_batch(first_seq, times_ns, raw=13600):
    # 13600 counts = 36 km/h per wheel = 10.000 m/s (research_model_profile).
    events = [[1, first_seq + i, t, 0, raw, raw, raw, raw, 0, 0] for i, t in enumerate(times_ns)]
    return dict(kind="motion_batch", schema=1, epoch=7, producer_time_status="unknown", events=events)


def nofix_drive(overlay_speed_e3=10000, wheel_after_send=True):
    """Boot in NO_FIX (stored position, utc 0), wheel speed 10 m/s, one
    overlaid send, then the first real fix returns BETA to ARMED."""
    original = nofix_original()
    wheels = wheel_batch(1, [1_900_000_000, 2_000_000_000, 2_100_000_000])
    rows = [
        boot(),
        state(20, "DISABLED", "ARMED", "enabled"),
        nofix_position(1, 1_000_000_000), nofix_send(1, 1_000_000_100, reason=8),
        nofix_state(1_050_000_000, "ARMED", "NO_FIX", "no_fix"),
        nofix_state(2_150_000_000, "NO_FIX", "SPEED_ENGAGED", "speed_published", 10.0, "speed_only"),
        nofix_position(2, 2_200_000_000),
        nofix_send(2, 2_200_000_100, overlay_payload(original, overlay_speed_e3), choice=4),
        dict(kind="beta_anchor", mono_ns=2_200_000_000, domain="beta", seq=2, mode=1, utc_s=0,
             gate="BAD_FIX", hdop=4.4, kmh=4, displacement_ratio=None, streak_s=None,
             reverse_exit_seen=False, dropped=0),
    ]
    if wheel_after_send:
        rows.append(wheels)  # batches are journaled after the send they fed
    else:
        rows.insert(4, wheels)
    rows += [
        nofix_position(3, 3_000_000_000, utc_s=1_790_000_000),
        nofix_state(3_050_000_000, "SPEED_ENGAGED", "ARMED", "gps_returned", 10.0, "speed_only"),
        nofix_send(3, 3_000_000_100, original=original_payload(), cls=2),
        health(4_000_000_000),
    ]
    return rows


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
        # Choice 4 is the known NO_FIX speed overlay (adapter.h Choice); on a
        # mode-0 (LOST) send it is a class violation, not an unknown choice.
        rows = drive()
        self.replace_send(rows, 3, choice=4)
        codes = self.codes(self.audit(rows), "violation")
        self.assertIn("beta_overlay_wrong_class", codes)
        self.assertNotIn("unknown_send_choice", codes)
        rows = drive()
        self.replace_send(rows, 3, choice=5)
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

    def test_optional_position_class_and_payload_fields_are_tolerated(self):
        rows = drive()
        for row in rows:
            if row["kind"] == "beta_state":
                row["position_class"] = "NO_FIX" if row["to"] == "GPS_LOST" else "FIX"
                row["payload"] = "future_field"
        report = self.audit(rows)
        self.assertEqual(report["status"], "local_checks_pass", report["issues"])
        self.assertEqual(report["beta"]["position_classes"], {"FIX": 3, "NO_FIX": 1})
        self.path.write_text("".join(json.dumps(row) + "\n" for row in rows))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            module.main([str(self.path)])
        self.assertIn("BETA NO_FIX: 1 state rows", out.getvalue())
        self.assertEqual(self.audit(drive())["beta"]["position_classes"], {})

    def test_fault_is_sticky(self):
        rows = drive()
        rows[-1:-1] = [state(5_100_000_000, "ARMED", "FAULT", "audit_fault"),
                       state(5_200_000_000, "FAULT", "ARMED", "enabled")]
        self.assertIn("beta_fault_not_sticky", self.codes(self.audit(rows), "violation"))

    def test_normal_no_fix_speed_overlay_is_not_a_violation(self):
        for after in (True, False):
            report = self.audit(nofix_drive(wheel_after_send=after))
            self.assertEqual(report["status"], "local_checks_pass", report["issues"])
            beta = report["beta"]
            self.assertEqual(beta["speed_overlay_sends"], 1)
            self.assertEqual(beta["speed_overlay_nonzero_results"], 0)
            self.assertEqual(beta["replaced_sends"], 0)
            self.assertEqual(beta["speed_overlay_wheel_checks"], {"checked": 1})
            self.assertEqual(beta["speed_engaged_periods"], 1)
            self.assertAlmostEqual(beta["state_seconds"]["SPEED_ENGAGED"], 0.9, places=6)
            self.assertAlmostEqual(beta["state_seconds"]["NO_FIX"], 1.1, places=6)
            self.assertAlmostEqual(beta["no_fix_seconds"]["max"], 2.0, places=6)
            self.assertEqual(report["send_choices"]["BETA_SPEED_OVERLAY"], 1)
            self.assertEqual(beta["anchor_gates"], {"BAD_FIX": 1})
            # The stored NO_FIX position is not a GPS-return reference.
            self.assertEqual(beta["gps_return_checks_total"], 0)

    def test_overlay_text_report(self):
        self.path.write_text("".join(json.dumps(row) + "\n" for row in nofix_drive()))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(module.main([str(self.path)]), 0)
        self.assertIn("BETA speed overlay: 1 sends (non-zero results 0), SPEED_ENGAGED 1 times", out.getvalue())

    def test_overlay_reasons_are_known(self):
        self.assertEqual(module.REASONS[15], "OVERLAY_MISMATCH")
        self.assertEqual(module.REASONS[16], "OVERLAY_NOT_NEEDED")
        rows = nofix_drive()
        for row in rows:
            if row["kind"] == "send" and row["call"] == 1:
                row["reason"] = 16
        report = self.audit(rows)
        self.assertEqual(report["status"], "local_checks_pass", report["issues"])
        self.assertEqual(report["send_reasons"].get("OVERLAY_NOT_NEEDED"), 1)

    def overlay_rows(self, **changes):
        rows = nofix_drive()
        for row in rows:
            if row["kind"] == "send" and row.get("choice") == 4:
                row.update(changes)
        return rows

    def test_overlay_on_a_fix_or_lost_send_is_a_violation(self):
        rows = nofix_drive()
        for row in rows:
            if row["kind"] == "position" and row["call"] == 2:
                row["utc_s"] = 1_790_000_000
        self.assertIn("beta_overlay_wrong_class", self.codes(self.audit(rows), "violation"))
        self.assertIn("beta_overlay_wrong_class", self.codes(self.audit(self.overlay_rows(mode=0)), "violation"))
        self.assertIn("beta_overlay_wrong_class",
                      self.codes(self.audit(self.overlay_rows(**{"class": 3})), "violation"))

    def test_overlay_may_change_only_the_speed_bytes(self):
        original = nofix_original()
        bad = bytearray(overlay_payload(original, 10000))
        bad[40] = 0  # hasBearing
        rows = self.overlay_rows(outgoing_hex=bytes(bad).hex())
        self.assertIn("beta_overlay_payload_mismatch", self.codes(self.audit(rows), "violation"))
        bad = bytearray(overlay_payload(original, 10000))
        bad[32] = 0
        rows = self.overlay_rows(outgoing_hex=bytes(bad).hex())
        self.assertIn("beta_overlay_payload_mismatch", self.codes(self.audit(rows), "violation"))

    def test_overlay_speed_must_follow_the_wheels(self):
        report = self.audit(nofix_drive(overlay_speed_e3=12000))
        self.assertIn("beta_overlay_speed_mismatch", self.codes(report, "violation"))
        # One unit of rounding is tolerated.
        report = self.audit(nofix_drive(overlay_speed_e3=10001))
        self.assertEqual(report["status"], "local_checks_pass", report["issues"])

    def test_overlay_without_wheel_rows_checks_plausibility_only(self):
        rows = [r for r in nofix_drive() if r["kind"] != "motion_batch"]
        report = self.audit(rows)
        self.assertEqual(report["status"], "local_checks_pass", report["issues"])
        self.assertEqual(report["beta"]["speed_overlay_wheel_checks"], {"unverified_no_wheel_row": 1})
        rows = [r for r in nofix_drive(overlay_speed_e3=100001) if r["kind"] != "motion_batch"]
        self.assertIn("beta_overlay_speed_out_of_range", self.codes(self.audit(rows), "violation"))

    def test_overlay_needs_a_no_fix_state_and_beta_config(self):
        rows = [r for r in nofix_drive() if r["kind"] != "beta_state" or r["to"] == "ARMED"]
        self.assertIn("beta_overlay_without_no_fix_state", self.codes(self.audit(rows), "violation"))
        rows = nofix_drive()
        rows[0] = boot(mode=4)
        self.assertIn("beta_without_beta_config", self.codes(self.audit(rows), "violation"))

    def test_dr_replacement_of_a_no_fix_send_is_a_violation(self):
        original = nofix_original()
        rows = self.overlay_rows(choice=3, outgoing_hex=beta_payload(original, LAT, LON).hex())
        self.assertIn("beta_wrong_mode", self.codes(self.audit(rows), "violation"))
        rows = drive()
        self.replace_send(rows, 3, **{"class": 0})
        self.assertIn("beta_wrong_mode", self.codes(self.audit(rows), "violation"))

    def test_anchor_and_reverse_latch_rows_are_known(self):
        rows = nofix_drive()
        latch = dict(kind="beta_reverse_latch", mono_ns=2_300_000_000, domain="model", event="kept_across_gap",
                     reason="sequence_discontinuity", missing_events=3, gap_ms=400, latched=True, value=1,
                     keep_limit_events=16, keep_limit_ms=2000)
        rows.insert(-1, latch)
        report = self.audit(rows)
        self.assertEqual(report["status"], "local_checks_pass", report["issues"])
        self.assertEqual(report["beta"]["reverse_latch_events"], {"kept_across_gap:sequence_discontinuity": 1})
        rows = nofix_drive()
        rows.insert(-1, dict(latch, domain="beta"))
        self.assertIn("unexpected_reverse_latch_domain", self.codes(self.audit(rows), "violation"))


    def lag(self, mono_ns, event):
        return dict(kind="beta_journal_lag", mono_ns=mono_ns, domain="beta", assist_ready=False,
                    event=event, lag_ms=1500, unwritten_rows=40, limit_ms=1500, clear_ms=500)

    def test_replacement_while_the_journal_lags_is_a_violation(self):
        # F3-A: the worker withholds BETA provenance once the writer is 1.5 s
        # behind; a replacement decided after that (one call of grace) is wrong.
        rows = drive()
        index = next(i for i, r in enumerate(rows) if r.get("call") == 3 and r["kind"] == "position")
        rows.insert(index, self.lag(2_600_000_000, "lagging"))
        report = self.audit(rows)
        self.assertIn("beta_change_during_journal_lag", self.codes(report, "violation"))
        self.assertEqual(report["journal_lag"], {"lagging": 1})
        # Restored before the replacements: no violation, only the lag notice.
        rows.insert(index + 1, self.lag(2_700_000_000, "current"))
        report = self.audit(rows)
        self.assertNotIn("beta_change_during_journal_lag", self.codes(report))
        self.assertIn("beta_journal_lag", self.codes(report, "inconclusive"))
        # A send already inside its POSITION call when the flag fell is tolerated.
        rows = drive()
        index = next(i for i, r in enumerate(rows) if r.get("call") == 4 and r["kind"] == "position")
        rows.insert(index, self.lag(3_050_000_000, "lagging"))
        rows.insert(index + 3, self.lag(4_050_000_000, "current"))
        report = self.audit(rows)
        details = [i for i in report["issues"] if i["code"] == "beta_change_during_journal_lag"]
        self.assertEqual(len(details), 1)   # the call-4 send at 4.0 s, not the call-3 one
        for event in ("late", None):
            rows = drive() + [dict(self.lag(7_000_000_000, "lagging"), event=event)]
            self.assertIn("partial_record", self.codes(self.audit(rows)))

    def test_boot_flush_timeout_is_reported_not_a_violation(self):
        rows = drive()
        rows.insert(1, dict(kind="journal_not_durable", stage="boot", assist_ready=False))
        report = self.audit(rows)
        self.assertIn("journal_not_durable", self.codes(report, "inconclusive"))
        self.assertEqual(report["issue_counts"].get("violation", 0), 0)


if __name__ == "__main__":
    unittest.main()
