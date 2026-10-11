"""--reverse-fence runtime of build/replay_beta (validation/REVERSE_GATE_ASSUMED_FORWARD_2026-10-11.md).

The recorded shadow_session/shadow_bus boundary rows are applied in time order
across trace files (trace.0 is read before trace.1): a REVERSE message received
inside an admitted bus interval recorded in the OTHER file is fed, one received
while the fence was closed is excluded. Rule F is off here so only the fence
decides whether BETA can seed. Synthetic data only; not vehicle evidence.
"""
import json
import os
import subprocess
import tempfile
import unittest

import test_replay_beta as base


def row(**fields):
    return json.dumps(fields, separators=(",", ":")) + "\n"


def boundary(kind, t, available, reset=False):
    ns = int(round(t * 1e9))
    return row(kind=kind, mono_ns=ns, domain="model", assist_ready=False, reset=reset,
               input_available=available, raw_since_ns=ns)


@unittest.skipUnless(os.access(base.TOOL, os.X_OK), "build/replay_beta not built (make test-replay-beta)")
class ReverseFence(unittest.TestCase):
    def seed(self, early_bus_available, fence="runtime"):
        with tempfile.TemporaryDirectory(prefix="mx5dr-replay-fence-") as tmp:
            trip = os.path.join(tmp, "logs")
            os.mkdir(trip)
            base.write_fixture(trip)
            with open(os.path.join(trip, "trace.0.jsonl"), "a") as f:
                f.write(boundary("shadow_session", 1.0, True))
                f.write(boundary("shadow_bus", 300.0, False))      # after the recording: must not hide 5 s
            with open(os.path.join(trip, "trace.1.jsonl"), "w") as f:
                f.write(boundary("shadow_bus", 5.0, early_bus_available))
            report = os.path.join(tmp, "r.json")
            p = subprocess.run([base.TOOL, "--trip", trip, "--real-only", "--seed-report", "--assume-forward", "off",
                                "--reverse-fence", fence, "--report", report],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=300)
            self.assertEqual(p.returncode, 0, p.stderr)
            with open(report) as f:
                return json.load(f)["beta_seed"]

    def test_message_inside_an_earlier_file_bus_interval_is_fed(self):
        self.assertIsNotNone(self.seed(True)["first_seed_s"])

    def test_message_while_the_bus_is_closed_is_excluded(self):
        self.assertIsNone(self.seed(False)["first_seed_s"])
        # Feeding as recorded ignores the fence.
        self.assertIsNotNone(self.seed(False, fence="feed")["first_seed_s"])


if __name__ == "__main__":
    unittest.main()
