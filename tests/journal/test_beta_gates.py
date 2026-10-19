"""BETA worker/controller gates checked with build/log_rate --probe (SYNTHETIC).

build/log_rate (tests/runtime/log_rate.cpp) runs the product adapter,
Pipeline and BetaController in simulated time. Each probe authors one
stimulus at the worker/controller boundary and checks one safety gate that the
adapter's own per-call gates or leases would otherwise hide
(validation/V1_TEST_HARDENING_2026-10-19.md):

  generation   BetaController::publish generation gate (position_generation_):
               a GPS return and a new loss that the worker has not popped
               before its tick must not let a candidate reach the next LOST
               send, although that send passes the adapter's class and
               generation checks.
  silence      all motion stops while ENGAGED: withdrawn as sensor_silence
               within 350 ms (300 ms receipt silence plus one 50 ms turn; the
               pipeline's MISSING_SENSOR path fires on the same turn).
  silence_arg  the tick contract alone: last_motion_ns 299 ms old keeps the
               episode, 301 ms old withdraws it as sensor_silence.
  wheel_gap    wheels stop (yaw continues) while SPEED_ENGAGED: the overlay
               ends as speed_unavailable within the 500 ms BETA lease plus
               one turn (Pipeline::speed_publication lease).

Synthetic data: not vehicle, phone or DHU evidence.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOL = Path(os.environ.get('MX5DR_LOG_RATE', ROOT / 'build/log_rate'))


def probe(name, seconds):
    with tempfile.TemporaryDirectory() as directory:
        result = subprocess.run([str(TOOL), '--profile', 'full', '--seconds', str(seconds),
                                 '--event-every', '120', '--outage', '25', '--out', directory,
                                 '--probe', name],
                                capture_output=True, text=True, timeout=120)
    rows = [json.loads(line) for line in result.stderr.splitlines() if line.startswith('{"probe"')]
    return result.returncode, rows


@unittest.skipUnless(TOOL.exists(), 'build/log_rate not built')
class BetaGateProbes(unittest.TestCase):
    def check(self, name, seconds, note, max_delay_ms=None):
        code, rows = probe(name, seconds)
        self.assertEqual(len(rows), 1, rows)
        row = rows[0]
        self.assertEqual(code, 0, row)
        self.assertTrue(row['ok'], row)
        self.assertEqual(row['note'], note)
        self.assertGreater(row['at_ms'], 0)
        if max_delay_ms is not None:
            self.assertLessEqual(row['delay_ms'], max_delay_ms)
        return row

    def test_generation_gate(self):
        self.check('generation', 230, 'stock')

    def test_sensor_silence(self):
        self.check('silence', 230, 'sensor_silence', 350)

    def test_sensor_silence_contract(self):
        self.check('silence_arg', 230, 'sensor_silence', 50)

    def test_speed_lease(self):
        self.check('wheel_gap', 30, 'speed_unavailable', 600)


if __name__ == '__main__':
    unittest.main()
