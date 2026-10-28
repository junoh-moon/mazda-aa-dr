"""Offline diagnostic must preserve missing evidence and record provenance."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "g3_replay_diagnose.py"


class G3ReplayDiagnosis(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "on").mkdir()
        self.runs = []
        self.extra = []

    def boot(self, index, g3, beta=(), rc=0, sends=()):
        self.runs.append(dict(index=index, mode="on", rc=rc,
                              boot=f"{index:08d}-PRIVATE-BOOT-ID"))
        if rc:
            return
        self.rows(f"on/{index:02d}.g3.jsonl", g3)
        self.rows(f"on/{index:02d}.jsonl", beta)
        self.rows(f"on/{index:02d}.sends.jsonl", sends)

    def rows(self, name, rows):
        (self.root / name).write_text("".join(json.dumps(r) + "\n" for r in rows))

    def diagnose(self):
        (self.root / "real-replay-summary.json").write_text(json.dumps(dict(runs=self.runs)))
        result = subprocess.run([sys.executable, str(TOOL), "--archive", str(self.root)] + self.extra,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("PRIVATE", result.stdout)
        self.assertNotIn(self.temp.name, result.stdout)
        return json.loads(result.stdout)

    def test_links_rejected_restore_to_successfully_written_headingless_record(self):
        self.boot(1, [dict(kind="g3_store", event="missing", reason="missing", mono_ns=1),
                      dict(kind="g3_store", event="written", seq=7, st="S", hv=0,
                           beta=None, ref_s=None, mono_ns=40, start_ns=39, lat=37.123456)])
        self.boot(2, [], rc=2)
        self.boot(3, [dict(kind="g3_store", event="rejected", reason="no_heading",
                           seq=7, hv=0, st="S", mono_ns=1, boot="00000001"),
                      dict(kind="g3_send", event="none", reason="no_heading", mono_ns=2)])
        report = self.diagnose()
        self.assertEqual(report["summary"]["not_replayable"], 1)
        blocked = report["boots"][2]
        self.assertEqual(blocked["loaded_from"]["boot"], "B01")
        self.assertEqual(blocked["loaded_from"]["seq"], 7)
        self.assertEqual(blocked["first_send_block"]["classification"], "observed_rejection")
        self.assertEqual(blocked["loaded_from"]["heading"], "absent")
        self.assertTrue(blocked["unreplayed_boot_since_write"])

    def test_sequence_collision_with_different_stored_boot_is_not_a_provenance_link(self):
        self.boot(1, [dict(kind="g3_store", event="written", seq=1, st="S", hv=0, mono_ns=2)])
        self.boot(2, [dict(kind="g3_store", event="rejected", reason="no_heading",
                           seq=1, hv=0, st="S", mono_ns=1, boot="99999999")])
        self.assertNotIn("loaded_from", self.diagnose()["boots"][1])

    def test_future_anchor_is_not_used_to_explain_an_earlier_write(self):
        self.boot(1, [dict(kind="g3_store", event="written", seq=1, st="S", hv=0,
                           beta=None, ref_s=None, mono_ns=20, start_ns=9_000_000_000)],
                  [dict(kind="beta_anchor", gate="REVERSE", mono_ns=10),
                   dict(kind="beta_anchor", gate="ACCEPTED", h=[1, 900, 30], mono_ns=30)])
        write = self.diagnose()["boots"][0]["writes"][0]
        self.assertEqual(write["origin"], "no_accepted_heading_observed")
        self.assertEqual(write["prior_accepted_headings"], 0)
        self.assertEqual(write["anchor_gates_since_last_heading"], {"REVERSE": 1})

    def test_prior_heading_and_subsequent_cadence_gap_remain_evidence_not_a_sensor_claim(self):
        self.boot(1, [dict(kind="g3_store", event="written", seq=1, st="S", hv=0,
                           beta=None, ref_s=None, mono_ns=40, start_ns=39)],
                  [dict(kind="beta_anchor", gate="ACCEPTED", h=[1, 900, 30], mono_ns=10),
                   dict(kind="beta_cadence_fence", stream="send", mono_ns=20)])
        write = self.diagnose()["boots"][0]["writes"][0]
        self.assertEqual(write["origin"], "accepted_then_cadence_fence")
        self.assertEqual(write["classification"], "input_gap_evidence")
        self.assertEqual(write["fence_streams"], {"send": 1})
        self.assertEqual(write["trace_history_at_completion"], "unobserved")

    def test_moving_departure_mark_is_not_a_failed_heading_store(self):
        self.boot(1, [dict(kind="g3_store", event="written", seq=1, st="S", hv=1,
                           beta=[900, 30], ref_s=0, mono_ns=10),
                      dict(kind="g3_store", event="written", seq=2, st="M", hv=0,
                           mark=1, mono_ns=20)])
        report = self.diagnose()
        self.assertEqual(report["boots"][0]["writes"][1]["origin"], "departure_mark")
        self.assertEqual(report["summary"]["parked_heading_absent_writes"], 0)

    def test_missing_journal_does_not_mean_no_heading_or_no_window(self):
        self.boot(1, [])
        (self.root / "on/01.g3.jsonl").unlink()
        boot = self.diagnose()["boots"][0]
        self.assertEqual(boot["classification"], "insufficient_evidence")
        self.assertEqual(boot["first_send_block"]["classification"], "insufficient_evidence")

    def test_wait_is_distinct_from_rejection_and_unknown_text_is_not_published(self):
        self.boot(1, [dict(kind="g3_store", event="loaded", reason="chain_depart", mono_ns=1),
                      dict(kind="g3_send", event="wait", reason="latch_unknown", mono_ns=2),
                      dict(kind="g3_store", event="checked", reason="PRIVATE-REASON", mono_ns=3)])
        block = self.diagnose()["boots"][0]["first_send_block"]
        self.assertEqual(block["classification"], "observed_wait")
        self.assertEqual(block["reason"], "latch_unknown")

    def test_trace_finds_reference_break_after_last_valid_seed(self):
        self.boot(1, [dict(kind="g3_store", event="written", seq=1, st="S", hv=0,
                           beta=None, ref_s=None, mono_ns=4_000_000_000)],
                  [dict(kind="beta_anchor", gate="ACCEPTED", h=[1, 900, 30], mono_ns=1_000_000_000)])
        (self.root / "01.heading.txt").write_text(
            "1.00 seeded=1 heading=PRIVATE hb=3 g3_breaks=0 resets=0\n"
            "2.00 seeded=0 heading=nan hb=nan g3_breaks=0 resets=0\n"
            "3.00 seeded=0 heading=nan hb=nan g3_breaks=1 resets=1\n"
            "4.00 seeded=0 heading=nan hb=nan g3_breaks=1 resets=1\n")
        self.extra = ["--traces", str(self.root)]
        write = self.diagnose()["boots"][0]["writes"][0]
        self.assertEqual(write["trace_history_at_completion"], "break_after_last_seed")
        self.assertEqual(write["request_reference_loss"], "unobserved_request_time_not_recorded")
        self.assertEqual(write["trace"]["breaks_after_seed"], 1)
        self.assertEqual(write["trace"]["unseeded_before_write_s"], 2.0)

    def test_trace_before_new_seed_is_not_a_later_reference_break(self):
        self.boot(1, [dict(kind="g3_store", event="written", seq=1, st="S", hv=0,
                           beta=None, ref_s=None, mono_ns=4_000_000_000)])
        (self.root / "01.heading.txt").write_text(
            "1.00 seeded=0 g3_breaks=1 resets=1\n"
            "2.00 seeded=1 g3_breaks=1 resets=1\n"
            "3.00 seeded=0 g3_breaks=1 resets=1\n"
            "4.00 seeded=0 g3_breaks=1 resets=1\n")
        self.extra = ["--traces", str(self.root)]
        write = self.diagnose()["boots"][0]["writes"][0]
        self.assertEqual(write["trace_history_at_completion"], "unobserved")
        self.assertEqual(write["trace"]["breaks_after_seed"], 0)

    def test_window_end_is_not_mislabeled_as_the_first_opening_block(self):
        self.boot(1, [dict(kind="g3_send", event="start", mono_ns=1),
                      dict(kind="g3_send", event="end", reason="fault", why="wheel_stale", mono_ns=2)])
        boot = self.diagnose()["boots"][0]
        self.assertEqual(boot["first_send_block"]["classification"], "opened_without_prior_block")
        self.assertEqual(boot["window_end"]["reason"], "fault")
        self.assertEqual(boot["window_end"]["why"], "wheel_stale")

    def test_anchor_history_uses_timestamps_rather_than_delayed_file_order(self):
        self.boot(1, [dict(kind="g3_store", event="written", seq=1, st="S", hv=0,
                           beta=None, ref_s=None, mono_ns=40)],
                  [dict(kind="beta_anchor", gate="ACCEPTED", h=[1, 900, 30], mono_ns=30),
                   dict(kind="beta_cadence_fence", stream="send", mono_ns=20),
                   dict(kind="beta_anchor", gate="ACCEPTED", h=[1, 900, 30], mono_ns=10)])
        write = self.diagnose()["boots"][0]["writes"][0]
        self.assertEqual(write["fence_streams"], {})

    def test_raw_receipt_gap_near_break_is_labeled_without_claiming_a_physical_fault(self):
        self.test_trace_finds_reference_break_after_last_valid_seed()
        trip = self.root / "trip"
        trip.mkdir()
        self.rows("trip/trace.0.jsonl", [
            dict(kind="boot", boot_id="00000001-PRIVATE-BOOT-ID"),
            dict(kind="motion_batch", events=[[2, 1, 500_000_000], [2, 2, 3_000_000_000]]),
            dict(kind="shadow_input_reset", mono_ns=3_000_000_000),
            dict(kind="boot", boot_id="OTHER-PRIVATE-BOOT-ID"),
            dict(kind="motion_batch", events=[[2, 3, 3_010_000_000]])])
        (self.root / "commands.json").write_text(json.dumps([
            ["binary-not-executed", "--trip", str(trip), "--boot-id", "00000001-PRIVATE-BOOT-ID"]]))
        self.extra += ["--commands", str(self.root / "commands.json")]
        evidence = self.diagnose()["boots"][0]["writes"][0]["raw_near_first_break"]
        self.assertEqual(evidence["yaw_receipt_gap_s"], 2.5)
        self.assertEqual(evidence["recorded_input_resets"], 1)
        self.assertEqual(evidence["classification"], "recorded_input_gap_not_physical_fault")


if __name__ == "__main__":
    unittest.main()
