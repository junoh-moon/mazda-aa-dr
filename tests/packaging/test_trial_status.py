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

    def test_capture_without_computation_is_reported_as_incomplete(self):
        self.trace[1]['active'] = False
        self.trace[2]['computation_active'] = False
        rows = [row for row in self.trace if row['kind'] != 'shadow']
        r = self.run_status(trace=rows)
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('capture_active=observed', r.stdout)
        self.assertIn('computation_active=unavailable', r.stdout)
        self.assertIn('shadow_inputs_processed=unavailable', r.stdout)

    def test_live_worker_does_not_prove_it_processed_inputs(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row['events'] = 0
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('shadow_inputs_processed=unavailable', r.stdout)

    def test_parked_wait_for_anchor_is_explicit_without_requiring_a_solution(self):
        r = self.run_status()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('shadow_inputs_processed=observed', r.stdout)
        self.assertIn('model_solution=not_observed', r.stdout)
        self.assertIn('pipeline=WAITING', r.stdout)
        self.assertIn('gps_anchor_gate=WAITING', r.stdout)
        self.assertIn('Startup evidence only', r.stdout)

    def test_latest_solution_state_replaces_a_previous_valid_result(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        good = dict(row, mono_ns=98000000000, model_valid=True, result='OK', pipeline='OK')
        self.trace.insert(self.trace.index(row), good)
        r = self.run_status()
        self.assertIn('model_solution=not_observed', r.stdout)
        row.update(model_valid=True, result='OK', pipeline='OK')
        self.assertIn('model_solution=observed', self.run_status().stdout)

    def test_missing_stale_future_or_malformed_model_diagnostic_is_not_progress(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        for change in ({'mono_ns': 1000000000}, {'mono_ns': 101000000000},
                       {'events': -1}, {'events': '4'}, {'domain': 'qualified'}):
            rows = [dict(item, **change) if item is row else item for item in self.trace]
            self.assertNotEqual(self.run_status(trace=rows).returncode, 0, change)
        self.assertNotEqual(self.run_status(trace=[item for item in self.trace if item is not row]).returncode, 0)

    def test_position_and_rejection_reasons_are_visible(self):
        rows = [row for row in self.trace if row['kind'] != 'position']
        rows.append(dict(kind='shadow_position_rejected', mono_ns=99000000000,
                         reason='request_session_unavailable'))
        rows.append(dict(kind='shadow_input_reset', mono_ns=99000000000,
                         reason='stale_receipt'))
        r = self.run_status(trace=rows)
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('oem_position_recent=unavailable', r.stdout)
        self.assertIn('position_rejection=request_session_unavailable', r.stdout)
        self.assertIn('motion_rejection=stale_receipt', r.stdout)

    def test_pipeline_reset_reason_survives_a_later_successful_input(self):
        self.trace.insert(3, dict(kind='shadow_pipeline_reset', mono_ns=98000000000,
                                 reason='LATE', operation='raw', receive_seq=45))
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row['pipeline'] = 'OK'
        r = self.run_status()
        self.assertIn('last_pipeline_reset=LATE operation=raw receive_seq=45', r.stdout)
        self.assertIn('capture_active=observed', r.stdout)

    def test_rejected_raw_is_still_capture_evidence(self):
        self.trace[-1]['events'] = self.trace[-1]['events'][:2]
        self.trace.append(dict(kind='motion_rejected', authenticated_decoded=True,
                               sensor=3, checked_ns=99000000000, received_ns=1000000000,
                               reason='stale_receipt'))
        r = self.run_status()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('reverse_received_recently=observed receipt_only_not_direction_quality', r.stdout)
        self.assertIn('rejected_raw_seen=true', r.stdout)

    def test_storage_stop_is_visible_even_while_last_health_is_recent(self):
        self.write('trace.storage.json', [dict(kind='storage_stop', stream='trace',
                   boot_id=BOOT, mono_ns=99500000000, reason='low_space',
                   available_bytes=7 * 1024 * 1024, reserve_bytes=8 * 1024 * 1024)])
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('storage_stop=trace reason=low_space', r.stdout)
        self.assertIn('capture_active=unavailable', r.stdout)
        self.assertIn('computation_active=unavailable', r.stdout)

    def test_old_boot_storage_stop_does_not_disable_current_capture(self):
        self.write('trace.storage.json', [dict(kind='storage_stop', stream='trace',
                   boot_id=OLD, mono_ns=99000000000, reason='low_space')])
        self.assertEqual(self.run_status().returncode, 0)

    def test_restarted_worker_does_not_hide_prior_storage_failure(self):
        self.trace[0]['mono_ns'] = 90000000000
        self.write('trace.storage.json', [dict(kind='storage_stop', stream='trace',
                   boot_id=BOOT, mono_ns=80000000000, reason='low_space')])
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('storage_stop=trace reason=low_space', r.stdout)
        self.assertIn('capture_active=observed', r.stdout)

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
        try:
            deadline = time.monotonic() + 3
            logfile = self.logs / 'collector.0.jsonl'
            while time.monotonic() < deadline:
                if logfile.exists() and '"kind":"poll"' in logfile.read_text():
                    break
                self.assertIsNone(proc.poll())
                time.sleep(.01)
            self.assertTrue(logfile.exists())
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
        finally:
            (self.logs / 'collector.stop').mkdir(exist_ok=True)
            try:
                proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.communicate()
                self.fail('Host fixture collector failed cooperative shutdown')

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
