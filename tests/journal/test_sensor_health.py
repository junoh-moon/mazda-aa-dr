"""Receipt-only sensor diagnostics, independent of physical cadence assumptions."""
import contextlib
import gc
import io
import json
import tracemalloc
import unittest
from unittest.mock import patch

from test_motion_logs import audit, boot, codes, consume, event, packed


def motion(sensor, seq, received_ns, epoch=42):
    row = event(0)
    row.update(sensor=sensor, receive_seq=seq, received_ns=received_ns, epoch=epoch)
    return row


class SensorHealth(unittest.TestCase):
    def test_interleaved_sensor_intervals_and_diagnostic_threshold(self):
        rows = [motion(1, 1, 1), motion(2, 2, 100000001),
                motion(3, 3, 150000001), motion(1, 4, 250000001),
                motion(2, 5, 300000001), motion(1, 6, 550000001)]
        a = consume(rows)
        sensors = a.report()['motion']['sensor_health']
        wheel = sensors['1']
        self.assertEqual(wheel['receipt_intervals_ns'],
                         dict(count=2, min=250000000, max=300000000, mean=275000000))
        self.assertEqual(wheel['gaps_over_250ms'], 1)
        self.assertEqual(wheel['coverage_ns'], 550000000)
        self.assertEqual(wheel['first_received_ns'], 1)
        self.assertEqual(wheel['last_received_ns'], 550000001)
        self.assertEqual(sensors['2']['receipt_intervals_ns']['max'], 200000000)
        self.assertEqual(sensors['3']['receipt_intervals_ns']['count'], 0)
        self.assertNotIn('motion_sequence_gap', codes(a))
        compact = consume([packed(rows)]).report()['motion']['sensor_health']
        self.assertEqual(sensors, compact)

    def test_empty_and_single_record_availability(self):
        empty = consume([]).report()['motion']['sensor_health']
        self.assertEqual(set(empty), {'1', '2', '3'})
        for stats in empty.values():
            self.assertEqual(stats['availability'], 'absent')
            self.assertEqual(stats['samples'], 0)
            self.assertEqual(stats['coverage_ns'], 0)
            self.assertIsNone(stats['first_received_ns'])
            self.assertEqual(stats['receipt_intervals_ns'], dict(count=0, min=None, max=None, mean=None))
        one = consume([motion(1, 1, 7)]).report()['motion']['sensor_health']
        self.assertEqual(one['1']['availability'], 'observed')
        self.assertEqual(one['1']['session_epoch_segments'], 1)
        self.assertEqual(one['1']['first_received_ns'], one['1']['last_received_ns'])
        self.assertEqual(one['2']['availability'], 'absent')
        self.assertEqual(one['3']['availability'], 'absent')

    def test_epoch_and_session_boundaries_reset_every_sensor_cursor(self):
        rows = [motion(1, 1, 100), motion(2, 2, 110), motion(3, 3, 120),
                motion(1, 1, 1, 43), motion(2, 2, 2, 43), motion(3, 3, 3, 43),
                motion(1, 4, 11, 43), boot(), motion(2, 1, 1, 43),
                motion(1, 2, 2, 43), motion(3, 3, 3, 43), motion(1, 4, 12, 43)]
        a = consume(rows)
        self.assertEqual(codes(a), ['motion_source_restart'])
        sensors = a.report()['motion']['sensor_health']
        for stats in sensors.values():
            self.assertEqual(stats['session_epoch_segments'], 3)
            self.assertEqual(stats['regressed_receipts'], 0)
        self.assertEqual(sensors['1']['receipt_intervals_ns'], dict(count=2, min=10, max=10, mean=10))
        self.assertEqual(sensors['1']['coverage_ns'], 20)
        self.assertEqual(sensors['2']['receipt_intervals_ns']['count'], 0)
        self.assertEqual(sensors['3']['receipt_intervals_ns']['count'], 0)

    def test_duplicate_and_regression_never_rewind_or_double_count(self):
        a = consume([motion(1, i + 1, ns) for i, ns in enumerate((100, 100, 80, 90, 110))])
        stats = a.report()['motion']['sensor_health']['1']
        self.assertEqual(stats['samples'], 5)
        self.assertEqual(stats['duplicate_receipts'], 1)
        self.assertEqual(stats['regressed_receipts'], 2)
        self.assertEqual(stats['coverage_ns'], 10)
        self.assertEqual(stats['receipt_intervals_ns'], dict(count=1, min=10, max=10, mean=10))
        self.assertEqual(codes(a).count('motion_clock_regressed'), 2)

    def test_uint64_timestamps_are_subtracted_before_float_conversion(self):
        maximum = 2**64 - 1
        stats = consume([motion(1, maximum - 2, maximum - 2),
                         motion(1, maximum - 1, maximum - 1),
                         motion(1, maximum, maximum)]).report()['motion']['sensor_health']['1']
        self.assertEqual(stats['last_received_ns'], maximum)
        self.assertEqual(stats['coverage_ns'], 2)
        self.assertEqual(stats['receipt_intervals_ns'], dict(count=2, min=1, max=1, mean=1))
        json.dumps(stats, allow_nan=False)

    def test_source_clock_values_do_not_change_receipt_summary(self):
        rows = [motion(1, 1, 1), motion(1, 2, 300000001)]
        expected = consume(rows).report()['motion']
        rows[0]['source_mono_ms'] = -2**63
        rows[1]['source_mono_ms'] = 2**63 - 1
        actual = consume(rows).report()['motion']
        self.assertEqual(expected, actual)
        self.assertEqual(actual['producer_time'], 'unknown')
        self.assertNotIn('latency', actual)
        self.assertNotIn('assist_ready', actual)

    def test_streaming_sensor_storage_is_bounded(self):
        a = consume([])
        tracemalloc.start()
        try:
            for i in range(1000):
                a.consume(event(i), 'stream')
            gc.collect()
            before = tracemalloc.get_traced_memory()[0]
            for i in range(1000, 30000):
                a.consume(event(i), 'stream')
            gc.collect()
            after = tracemalloc.get_traced_memory()[0]
            self.assertLess(after - before, 128 * 1024)
        finally:
            tracemalloc.stop()
        self.assertEqual(len(a.motion_sensor_health), 3)
        self.assertEqual(len(a._motion_sensor_last), 3)
        self.assertEqual(sum(s['samples'] for s in a.motion_sensor_health.values()), 30000)
        self.assertEqual(codes(a), [])

    def test_missing_sensors_and_large_gaps_do_not_change_exit_status(self):
        a = consume([motion(1, 1, 1), motion(1, 2, 900000001)])
        self.assertEqual(codes(a), [])
        # Isolate the receipt diagnostics from the pre-existing LOCATION audit.
        a.checked = 1
        a.consume(dict(kind='health', mono_ns=900000001, dropped=0,
                       hook_installed=True, assist_ready=False), 'health')
        report = a.report()
        self.assertEqual(report['status'], 'local_checks_pass')
        output = io.StringIO()
        with patch.object(audit, 'analyze', return_value=report), contextlib.redirect_stdout(output):
            self.assertEqual(audit.main(['unused']), 0)
        text = output.getvalue()
        self.assertIn('wheel receipt observations: 2 samples (observed); max gap 900.000 ms; gaps >250 ms: 1', text)
        self.assertIn('yaw receipt observations: 0 samples (absent)', text)
        self.assertIn('reverse receipt observations: 0 samples (absent)', text)
        self.assertIn('producer cadence, latency and ASSIST readiness remain unverified', text)


if __name__ == '__main__':
    unittest.main()
