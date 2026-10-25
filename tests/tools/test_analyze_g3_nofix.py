"""Garage carry before the first GPS fix: speed plus bearing byte contract."""
import copy
import unittest

import test_analyze_beta as beta
import test_analyze_g3 as g3


def rows():
    result = beta.nofix_drive(wheel_after_send=False)
    start = dict(g3.window(start=2_000_000_001, end=None)[0], garage=1, moved=1, rlost=0)
    send_at = next(i for i, r in enumerate(result) if r.get("choice") == 4)
    result.insert(send_at - 1, start)
    send = next(r for r in result if r.get("choice") == 4)
    send["choice"] = 7
    outgoing = bytearray.fromhex(send["outgoing_hex"])
    outgoing[40] = 1
    outgoing[44:48] = (90000000).to_bytes(4, "little", signed=True)
    send["outgoing_hex"] = outgoing.hex()
    return result


def audit(result):
    _, report = g3.consume(result)
    issues = [i["code"] for i in report["issues"] if i["code"].startswith(("g3_bearing", "beta_overlay"))
              or i["code"] in ("native_mode_mutation", "unknown_send_choice", "beta_nonpass_reason")]
    return issues, report


class GarageNoFixAudit(unittest.TestCase):
    def test_speed_and_bearing_before_any_gps_fix(self):
        found, report = audit(rows())
        self.assertEqual(found, [])
        self.assertEqual(report["send_choices"]["BETA_NOFIX_BEARING"], 1)
        self.assertEqual(report["g3"]["send"]["overlaid_nofix_sends"], 1)
        self.assertEqual(report["g3"]["send"]["overlaid_fix_sends"], 0)

    def test_bearing_alone_at_rest_preserves_absent_speed(self):
        result = rows()
        for row in result:
            if row.get("kind") == "motion_batch":
                for event in row["events"]:
                    event[4:8] = [10000] * 4
            if row.get("choice") == 7:
                for name in ("original_hex", "outgoing_hex"):
                    payload = bytearray.fromhex(row[name])
                    payload[32] = 0
                    payload[36:40] = bytes(4)
                    row[name] = payload.hex()
        self.assertEqual(audit(result)[0], [])

    def test_wrong_class_and_unpermitted_payload_changes(self):
        for change, expected in (("class", "beta_overlay_wrong_class"),
                                 ("utc", "beta_overlay_wrong_class"),
                                 ("coordinate", "beta_overlay_payload_mismatch"),
                                 ("accuracy", "beta_overlay_payload_mismatch"),
                                 ("bearing_absent", "g3_bearing_payload_mismatch"),
                                 ("bearing_zero", "g3_bearing_out_of_range")):
            result = rows()
            send = next(r for r in result if r.get("choice") == 7)
            payload = bytearray.fromhex(send["outgoing_hex"])
            if change == "class":
                send["class"] = 2
            elif change == "utc":
                next(r for r in result if r.get("kind") == "position" and r["call"] == send["call"])["utc_s"] = 123
            elif change == "coordinate":
                payload[8] ^= 1
            elif change == "accuracy":
                payload[20] ^= 1
            elif change == "bearing_absent":
                payload[40] = 0
            else:
                payload[44:48] = bytes(4)
            send["outgoing_hex"] = payload.hex()
            self.assertIn(expected, audit(result)[0], change)

    def test_absent_speed_requires_stationary_wheel_evidence(self):
        result = rows()  # retained wheels show 36 km/h
        send = next(r for r in result if r.get("choice") == 7)
        for name in ("original_hex", "outgoing_hex"):
            payload = bytearray.fromhex(send[name])
            payload[32] = 0
            payload[36:40] = bytes(4)
            send[name] = payload.hex()
        self.assertIn("beta_overlay_speed_mismatch", audit(result)[0])

    def test_requires_garage_window_and_legacy_choice_stays_speed_only(self):
        result = rows()
        next(r for r in result if r.get("kind") == "g3_send").pop("garage")
        self.assertIn("g3_bearing_without_garage", audit(result)[0])
        self.assertIn("g3_bearing_outside_window", audit([r for r in rows() if r.get("kind") != "g3_send"])[0])
        result = rows()
        next(r for r in result if r.get("choice") == 7)["choice"] = 4
        self.assertIn("beta_overlay_payload_mismatch", audit(result)[0])

    def test_old_reports_keep_their_shape(self):
        original = beta.nofix_drive()
        found, report = audit(copy.deepcopy(original))
        self.assertEqual(found, [])
        self.assertNotIn("send", report["g3"])


if __name__ == "__main__":
    unittest.main()
