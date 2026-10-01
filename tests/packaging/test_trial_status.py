"""Read-only parked check with synthetic bounded journals; no firmware needed."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

PACK = Path(__file__).resolve().parents[2] / 'packaging'
COLLECTOR = Path(os.environ.get('MX5DR_TEST_BUILD', PACK.parent / 'build')) / 'test_collector'
BOOT = '12345678-1234-1234-1234-123456789abc'
OLD = '87654321-1234-1234-1234-123456789abc'


class TrialStatusTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / '.mx5dr-fixture').touch()
        self.base = self.root / 'data_persist/mx5-aa-dr'
        self.logs = self.base / 'logs'
        self.logs.mkdir(parents=True)
        (self.base / 'guard').mkdir()
        (self.base / 'guard/last-boot').write_text(BOOT + '\n')
        bootfile = self.root / 'proc/sys/kernel/random/boot_id'
        bootfile.parent.mkdir(parents=True)
        bootfile.write_text(BOOT + '\n')
        (self.root / 'proc/uptime').write_text('100.00 1.00\n')
        self.trace = [dict(kind='boot', boot_id=BOOT, mono_ns=1000000000, mode=4),
                      dict(kind='shadow_boot', active=True, capture_active=True),
                      dict(kind='health', mono_ns=99000000000, hook_installed=True,
                           audit_fault=0, dropped=0, capture_active=True, computation_active=True),
                      dict(kind='position', mono_ns=99000000000, mode=1),
                      dict(kind='shadow_calibration', mono_ns=99000000000,
                           gps_anchor_gate='WAITING'),
                      dict(kind='shadow', mono_ns=99000000000, domain='model',
                           assist_ready=False, model_valid=False, events=4, intervals=0,
                           result='E_NO_SEED', pipeline='WAITING'),
                      dict(kind='motion_batch', schema=1, epoch=1, events=[
                          [sensor, sensor, 99000000000, 90000, 0, 0, 0, 0, 1, 0]
                          for sensor in (1, 2, 3)])]
        self.collector = [dict(kind='collector_boot', boot_id=BOOT, schema=1),
                          dict(kind='poll', end_ns=99000000000, seq=0)]

    def write(self, name, rows):
        if name.startswith('collector.'):
            # Exact envelope order emitted by collector.cpp Journal::line.
            rows = [dict(stream='collector', collector_pid=123, observed_at_mono_ns=99000000000,
                         producer_mono_ns=None, producer_time_status='unknown', **row)
                    for row in rows]
        (self.logs / name).write_text(''.join(json.dumps(row, separators=(',', ':')) + '\n'
                                               for row in rows))

    def run_status(self, trace=None, collector=None):
        self.write('trace.0.jsonl', self.trace if trace is None else trace)
        self.write('collector.0.jsonl', self.collector if collector is None else collector)
        before = {str(p.relative_to(self.root)): p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        result = subprocess.run(['sh', str(PACK / 'trial_status.sh')], capture_output=True, text=True,
                                env=dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root)))
        after = {str(p.relative_to(self.root)): p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        self.assertEqual(before, after)
        return result

    def test_current_collection_read_only(self):
        r = self.run_status()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('does not approve driving or ASSIST', r.stdout)
        self.assertIn('reverse_received_recently=observed receipt_only_not_direction_quality', r.stdout)

    def test_capture_without_computation_preserves_collection_result(self):
        self.trace[1]['active'] = False
        self.trace[2]['computation_active'] = False
        rows = [row for row in self.trace if row['kind'] != 'shadow']
        r = self.run_status(trace=rows)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('capture_active=observed', r.stdout)
        self.assertIn('computation_active=unavailable', r.stdout)
        self.assertIn('model_diagnostic_recent=unavailable', r.stdout)

    def test_model_queue_count_does_not_claim_input_processing(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row['events'] = 0
        r = self.run_status()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('model_diagnostic_recent=observed events_queued_total=0', r.stdout)
        self.assertNotIn('inputs_processed', r.stdout)

    def test_parked_wait_for_anchor_is_explicit_without_requiring_a_solution(self):
        r = self.run_status()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('model_diagnostic_recent=observed events_queued_total=4', r.stdout)
        self.assertIn('model_solution=not_observed', r.stdout)
        self.assertIn('pipeline=WAITING', r.stdout)
        self.assertIn('gps_anchor_gate=WAITING', r.stdout)
        self.assertIn('Capture startup evidence only', r.stdout)

    def test_latest_solution_state_replaces_a_previous_valid_result(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        good = dict(row, mono_ns=98000000000, model_valid=True, result='OK', pipeline='OK')
        self.trace.insert(self.trace.index(row), good)
        r = self.run_status()
        self.assertIn('model_solution=not_observed', r.stdout)
        row.update(model_valid=True, result='OK', pipeline='OK')
        self.assertIn('model_solution=observed', self.run_status().stdout)

    def test_capture_terminal_records_revoke_recent_health_and_model_snapshot(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row.update(model_valid=True, result='OK', pipeline='OK')
        for kind in ('capture_end', 'capture_incomplete'):
            r = self.run_status(trace=self.trace + [dict(kind=kind, mono_ns=99000000000,
                                                        boot_id=BOOT)])
            self.assertNotEqual(r.returncode, 0, r.stdout)
            self.assertIn('capture_active=unavailable', r.stdout)
            self.assertIn('model_diagnostic_recent=unavailable', r.stdout)
            self.assertIn('model_solution=not_observed', r.stdout)

    def test_model_boundaries_revoke_prior_solution_without_losing_raw_capture(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row.update(model_valid=True, result='OK', pipeline='OK')
        for boundary in (dict(kind='shadow_session', mono_ns=99000000000, reset=True),
                         dict(kind='shadow_bus', mono_ns=99000000000, reset=True),
                         dict(kind='shadow_input_reset', mono_ns=99000000000,
                              reason='stale'),
                         dict(kind='shadow_disabled', reason='audit_fault')):
            result = self.run_status(trace=self.trace + [boundary])
            self.assertEqual(result.returncode, 0, boundary)
            self.assertIn('capture_active=observed', result.stdout)
            self.assertIn('model_diagnostic_recent=unavailable', result.stdout)
            self.assertIn('model_solution=not_observed', result.stdout)
            self.assertIn('gps_anchor_gate=none_observed', result.stdout)

    def test_primary_pipeline_reset_retracts_old_solution_and_shows_reason(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row.update(model_valid=True, result='OK', pipeline='OK')
        reset = dict(kind='shadow_pipeline_reset', mono_ns=99000000000,
                     domain='model', assist_ready=False, reason='BAD_INPUT',
                     operation='raw', input_ns=99000000000,
                     receive_seq=45, sensor=1, call=0, resets=1)
        result = self.run_status(trace=self.trace + [reset])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('model_solution=not_observed', result.stdout)
        self.assertIn('last_pipeline_reset_this_boot=BAD_INPUT operation=raw receive_seq=45', result.stdout)

    def test_drain_counter_is_a_separate_calculation_attempt_not_a_solution(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row['drain_calls_total'] = 3
        row['events'] = 0
        result = self.run_status()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('calculation_attempt_recent=observed drain_calls_total=3 intervals_total=0', result.stdout)
        self.assertIn('model_solution=not_observed', result.stdout)

    def test_missing_stale_future_or_malformed_model_diagnostic_is_not_observed(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        for change in ({'mono_ns': 1000000000}, {'mono_ns': 101000000000},
                       {'events': -1}, {'events': '4'}, {'domain': 'qualified'}):
            rows = [dict(item, **change) if item is row else item for item in self.trace]
            result = self.run_status(trace=rows)
            self.assertEqual(result.returncode, 0, change)
            self.assertIn('model_diagnostic_recent=unavailable', result.stdout)
            self.assertNotIn('events_queued_total=', result.stdout)
        result = self.run_status(trace=[item for item in self.trace if item is not row])
        self.assertEqual(result.returncode, 0)
        self.assertIn('model_diagnostic_recent=unavailable', result.stdout)

    def test_position_and_rejection_reasons_are_visible(self):
        rows = [row for row in self.trace if row['kind'] != 'position']
        rows.append(dict(kind='shadow_position_rejected', mono_ns=99000000000,
                         reason='session_unavailable'))
        rows.append(dict(kind='shadow_input_reset', mono_ns=99000000000,
                         reason='stale'))
        rows.append(dict(kind='shadow_motion_excluded', mono_ns=99000000000,
                         reason='receipt_before_session'))
        r = self.run_status(trace=rows)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('oem_position_recent=unavailable', r.stdout)
        self.assertIn('last_position_rejection_30s=session_unavailable', r.stdout)
        self.assertIn('last_motion_reset_30s=stale', r.stdout)
        self.assertIn('last_model_exclusion_30s=receipt_before_session', r.stdout)

    def test_rejected_raw_checked_now_does_not_refresh_old_receipt(self):
        self.trace[-1]['events'] = self.trace[-1]['events'][:2]
        self.trace.append(dict(kind='motion_rejected', authenticated_decoded=True,
                               sensor=3, checked_ns=99000000000, received_ns=1000000000,
                               reason='stale'))
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('reverse_received_recently=unavailable receipt_only_not_direction_quality', r.stdout)
        self.assertIn('reverse_rejected_checked_recently=observed worker_check_only', r.stdout)
        self.assertIn('rejected_raw_seen_this_boot=true', r.stdout)

    def test_recent_rejected_raw_is_diagnostic_only(self):
        self.trace[-1]['events'] = self.trace[-1]['events'][:2]
        self.trace.append(dict(kind='motion_rejected', authenticated_decoded=True,
                               sensor=3, checked_ns=99000000000, received_ns=99000000000,
                               reason='sequence_discontinuity'))
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('reverse_received_recently=unavailable receipt_only_not_direction_quality', r.stdout)
        self.assertIn('reverse_rejected_checked_recently=observed worker_check_only', r.stdout)

    def test_future_at_worker_check_never_becomes_capture_success_later(self):
        self.trace[-1]['events'] = self.trace[-1]['events'][:2]
        self.trace.append(dict(kind='motion_rejected', authenticated_decoded=True,
                               sensor=3, checked_ns=99000000000, received_ns=99500000000,
                               reason='future'))
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('reverse_received_recently=unavailable', r.stdout)
        self.assertIn('reverse_rejected_checked_recently=observed', r.stdout)

    def test_only_rejected_sensors_do_not_pass_collection_gate(self):
        self.trace.pop()
        for sensor in (1, 2, 3):
            self.trace.append(dict(kind='motion_rejected', authenticated_decoded=True,
                                   sensor=sensor, checked_ns=99000000000,
                                   received_ns=99000000000, reason='sequence_discontinuity'))
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('wheels_rejected_checked_recently=observed', r.stdout)
        self.assertIn('yaw_rejected_checked_recently=observed', r.stdout)
        self.assertIn('reverse_rejected_checked_recently=observed', r.stdout)

    def test_untimed_rejection_is_visible_without_recent_receipt_claim(self):
        self.trace[-1]['events'] = self.trace[-1]['events'][:2]
        self.trace.append(dict(kind='motion_rejected', authenticated_decoded=True,
                               sensor=3, checked_ns=0, received_ns=99000000000,
                               reason='clock_unavailable'))
        self.trace.append(dict(kind='shadow_input_reset', mono_ns=0,
                               reason='clock_unavailable'))
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('reverse_received_recently=unavailable', r.stdout)
        self.assertIn('reverse_rejected_checked_recently=unavailable', r.stdout)
        self.assertIn('untimed_rejected_raw_seen=true', r.stdout)
        self.assertIn('untimed_motion_reset_seen=true', r.stdout)

    def test_silent_writer_stop_does_not_reuse_five_second_old_health(self):
        self.trace[2]['mono_ns'] = 94000000000
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row.update(model_valid=True, result='OK', pipeline='OK')
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('health_recent=unavailable window=5s', r.stdout)
        self.assertIn('model_solution=not_observed', r.stdout)

    def test_silent_collector_stop_does_not_reuse_eight_second_old_poll(self):
        self.collector[-1]['end_ns'] = 91000000000
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('collector_poll_recent=unavailable window=8s', r.stdout)

    def test_old_model_snapshot_is_not_presented_as_current_solution(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row.update(mono_ns=97000000000, model_valid=True, result='OK', pipeline='OK')
        r = self.run_status()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('model_diagnostic_recent=unavailable', r.stdout)
        self.assertIn('model_solution=not_observed', r.stdout)

    def test_old_boot_and_missing_boot_do_not_use_fresh_looking_rows(self):
        self.trace[0]['boot_id'] = OLD
        self.assertNotEqual(self.run_status().returncode, 0)
        self.assertNotEqual(self.run_status(trace=self.trace[1:]).returncode, 0)

    def test_expired_or_future_health_and_sensors(self):
        for ns in (1000000000, 101000000000):
            self.trace[2]['mono_ns'] = ns
            self.assertNotEqual(self.run_status().returncode, 0)
        self.trace[2]['mono_ns'] = 99000000000
        self.trace[-1]['events'][2][2] = 1000000000
        self.assertNotEqual(self.run_status().returncode, 0)

    def test_uptime_centisecond_quantization_has_bounded_allowance(self):
        # Real CLOCK_MONOTONIC records may be just under 10 ms ahead of the
        # truncated /proc/uptime text, but a later future record must fail.
        for ns, expected in ((100009999999, 0), (100010000000, 0),
                             (100010000001, 1), (100020000000, 1)):
            self.collector[-1]['end_ns'] = ns
            r = self.run_status()
            self.assertEqual(r.returncode, expected, r.stdout + r.stderr)
        self.collector[-1]['end_ns'] = 99000000000
        self.trace[2]['mono_ns'] = 100010000001
        self.assertNotEqual(self.run_status().returncode, 0)
        self.trace[2]['mono_ns'] = 69999999999
        self.assertNotEqual(self.run_status().returncode, 0)  # age limit unchanged

    def test_rejected_capture_hook_and_audit(self):
        for field, value in (('capture_active', False), ('hook_installed', False),
                             ('audit_fault', 1), ('dropped', 1)):
            original = self.trace[2][field]
            self.trace[2][field] = value
            self.assertNotEqual(self.run_status().returncode, 0)
            self.trace[2][field] = original

    def test_collector_stopped_stale_and_old_boot(self):
        self.assertNotEqual(self.run_status(collector=self.collector + [dict(kind='collector_stop', reason='signal')]).returncode, 0)
        self.collector[-1]['end_ns'] = 1000000000
        self.assertNotEqual(self.run_status().returncode, 0)
        self.collector[-1]['end_ns'] = 99000000000
        self.collector[0]['boot_id'] = OLD
        self.assertNotEqual(self.run_status().returncode, 0)

    def test_rotation_continuity_and_new_boot_reset(self):
        self.write('trace.1.jsonl', self.trace[:2])
        self.assertEqual(self.run_status(trace=self.trace[2:]).returncode, 0)
        self.assertNotEqual(self.run_status(trace=[dict(kind='boot', boot_id=OLD, mono_ns=1, mode=4)] + self.trace[2:]).returncode, 0)

    def test_oneboot_marker_and_rearm_never_count_as_active_capture(self):
        (self.base / 'guard/last-boot').write_text(OLD + '\n')
        self.assertNotEqual(self.run_status().returncode, 0)
        (self.base / 'guard/last-boot').write_text(BOOT + '\n')
        (self.base / 'guard/arm').write_text('pending')
        self.assertNotEqual(self.run_status().returncode, 0)

    @unittest.skipUnless(COLLECTOR.exists(), 'Host collector build unavailable')
    def test_real_collector_journal_envelope(self):
        actual_boot = Path('/proc/sys/kernel/random/boot_id').read_text()
        (self.root / 'proc/sys/kernel/random/boot_id').write_text(actual_boot)
        (self.base / 'guard/last-boot').write_text(actual_boot)
        (self.base / 'mx5dr.conf').write_text('mode=SHADOW\nsample_ms=500\n')
        proc = subprocess.Popen([str(COLLECTOR), '--root', str(self.base),
                                 '--bus-address', 'unix:path=' + str(self.root / 'absent'),
                                 '--smdb', '/nonexistent-mx5dr-smdb'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        captured = []
        try:
            deadline = time.monotonic() + 3
            logfile = self.logs / 'collector.0.jsonl'
            while time.monotonic() < deadline:
                if logfile.exists() and '"kind":"poll"' in logfile.read_text():
                    break
                self.assertIsNone(proc.poll())
                time.sleep(.01)
            self.assertTrue(logfile.exists())
            for line in logfile.read_text().splitlines():
                captured.append(line)
                if '"kind":"poll"' in line:
                    break
            self.assertTrue(any('"kind":"poll"' in line for line in captured))
        finally:
            (self.logs / 'collector.stop').mkdir(exist_ok=True)
            try:
                proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.communicate()
                self.fail('Host fixture collector failed cooperative shutdown')
        # Replay the real emitted envelope as a fixed snapshot. Otherwise a
        # second live poll can race the fixture's earlier /proc/uptime value.
        logfile.write_text('\n'.join(captured) + '\n')
        now = time.monotonic_ns() - 100000000
        self.trace[0]['boot_id'] = actual_boot.strip()
        self.trace[0]['mono_ns'] = now - 1000000000
        for row in self.trace[2:]:
            if 'mono_ns' in row:
                row['mono_ns'] = now
        for row in self.trace[-1]['events']:
            row[2] = now
        self.write('trace.0.jsonl', self.trace)
        (self.root / 'proc/uptime').write_text(Path('/proc/uptime').read_text())
        r = subprocess.run(['sh', str(PACK / 'trial_status.sh')], capture_output=True, text=True,
                           env=dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root)))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('collector_poll_recent=observed', r.stdout)

    def test_actual_collector_envelope_required(self):
        self.write('trace.0.jsonl', self.trace)
        # Plain kind-first records are not produced by the collector journal.
        (self.logs / 'collector.0.jsonl').write_text(''.join(
            json.dumps(row, separators=(',', ':')) + '\n' for row in self.collector))
        r = subprocess.run(['sh', str(PACK / 'trial_status.sh')], capture_output=True, text=True,
                           env=dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root)))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('collector_poll_recent=unavailable', r.stdout)

    def test_symlink_log_rejected(self):
        target = self.root / 'outside'
        target.write_text('do not interpret\n')
        (self.logs / 'trace.2.jsonl').symlink_to(target)
        self.assertNotEqual(self.run_status().returncode, 0)

    def test_symlink_parent_rejected(self):
        target = self.root / 'outside'
        self.logs.rename(target)
        self.logs.symlink_to(target, target_is_directory=True)
        self.assertNotEqual(self.run_status().returncode, 0)


if __name__ == '__main__':
    unittest.main()
