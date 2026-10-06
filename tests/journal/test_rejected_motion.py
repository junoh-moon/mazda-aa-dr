"""Rejected callback evidence stays diagnostic-only, never accepted motion."""
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('rejected_audit', ROOT / 'tools/analyze_logs.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def rejected(**changes):
    row = dict(kind='motion_rejected', schema=1, domain='model', assist_ready=False,
               authenticated_decoded=True, producer_time_status='unknown',
               reason='stale', checked_ns=2000000000, sender_pid=123, sender_uid=501,
               sensor=3, epoch=42, receive_seq=2, received_ns=1000000000,
               source_mono_ms=0, raw=[0, 0, 0, 0], count=0, reverse=1)
    row.update(changes)
    return row


def read(*rows):
    a = audit.Auditor()
    for row in rows:
        a.consume(row, 'synthetic')
    return a


class RejectedMotion(unittest.TestCase):
    def test_production_cpp_rejection_formatter_contract(self):
        command = shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(ROOT / 'build/test_journal')))
        row = json.loads(subprocess.check_output(command + ['--emit-rejected'], text=True))
        a = read(row)
        self.assertEqual(a.motion_rejected_reasons, {'stale': 1})
        self.assertEqual(a.motion_samples, 0)
        self.assertEqual([x['code'] for x in a.issues], ['motion_channel_rejected'])

    def test_diagnostics_never_increment_accepted_samples(self):
        a = read(rejected())
        self.assertEqual(a.motion_samples, 0)
        self.assertEqual(a.motion_rejected_reasons, {'stale': 1})
        self.assertEqual(a.motion_rejected_sensors, {'3': 1})
        self.assertIn('motion_channel_rejected', [x['code'] for x in a.issues])
        self.assertIsNone(a.session['motion_epoch'])
        self.assertEqual(a.session['last_diagnostic_ns'], 2000000000)

    def test_preserved_rejection_does_not_heal_accepted_sequence_gap(self):
        first = dict(rejected(), kind='motion', receive_seq=1)
        last = dict(first, receive_seq=3, received_ns=2100000000)
        a = read(first, rejected(), last)
        self.assertEqual(a.motion_samples, 2)
        self.assertIn('motion_sequence_gap', [x['code'] for x in a.issues])

    def test_future_payload_not_used_as_health_frontier(self):
        a = read(rejected(reason='future', received_ns=2**64-1, checked_ns=101))
        self.assertEqual(a.session['last_diagnostic_ns'], 101)
        self.assertEqual(a.motion_samples, 0)

    def test_only_authenticated_decoded_payload_and_model_qualification(self):
        for field, value in (('authenticated_decoded', False), ('assist_ready', True),
                             ('domain', 'qualified'), ('producer_time_status', 'valid')):
            a = read(rejected(**{field: value}))
            self.assertEqual(a.issue_counts['violation'], 1)
            self.assertFalse(a.motion_rejected_reasons)

    def test_malformed_metadata_and_raw_are_not_counted(self):
        for changes in ({'schema': True}, {'reason': 'credentials_mismatch'},
                        {'checked_ns': -1}, {'sender_pid': 0}, {'sender_uid': 2**32},
                        {'raw': [1, 2, 3]}, {'reverse': -1}, {'receive_seq': 1.0}):
            a = read(rejected(**changes))
            self.assertEqual(a.motion_samples, 0)
            self.assertFalse(a.motion_rejected_reasons)
            self.assertIn('malformed_rejected_motion', [x['code'] for x in a.issues])

    def test_all_supported_reasons_and_large_integer_exactness(self):
        for reason in ('clock_unavailable', 'future', 'stale', 'source_changed',
                       'sequence_discontinuity'):
            a = read(rejected(reason=reason, checked_ns=0 if reason == 'clock_unavailable' else 2**63+3,
                              epoch=2**64-1, receive_seq=2**64-1, source_mono_ms=-2**63))
            self.assertEqual(a.motion_rejected_reasons, {reason: 1})
            self.assertEqual(a.session['last_diagnostic_ns'], 0 if reason == 'clock_unavailable' else 2**63+3)

    def test_late_arrival_diagnostic_is_bounded_and_not_a_sample(self):
        late = dict(kind='motion_late_accepted', schema=1, mono_ns=3000000000, domain='model',
                    assist_ready=False, epoch=42, first_seq=2, last_seq=6, events=5,
                    max_late_ms=1500, fresh_limit_ms=250, late_limit_ms=2000, late_accepted_total=5)
        a = read(late)
        self.assertEqual(a.motion_late, dict(bursts=1, events=5, max_late_ms=1500))
        self.assertEqual(a.motion_samples, 0)
        self.assertEqual(a.issue_counts['violation'], 0)
        self.assertNotIn('unknown_record_kind', [x['code'] for x in a.issues])
        for changes in ({'max_late_ms': 2001}, {'max_late_ms': 249}):
            a = read(dict(late, **changes))
            self.assertIn('late_motion_out_of_bounds', [x['code'] for x in a.issues])
        # Exact ns (F3-E): 250 ms + 1 ns is late, recorded as 250 ms; 250 ms
        # itself was fresh and cannot be a late arrival; 2 s is inclusive.
        for ns, ok in ((250000001, True), (250000000, False), (2000000000, True), (2000000001, False)):
            a = read(dict(late, max_late_ms=ns // 1000000, max_late_ns=ns))
            self.assertEqual('late_motion_out_of_bounds' in [x['code'] for x in a.issues], not ok, ns)
        # Legacy rows without ns: 250 (a truncated 250.x ms) is accepted.
        self.assertNotIn('late_motion_out_of_bounds',
                         [x['code'] for x in read(dict(late, max_late_ms=250)).issues])
        for changes in ({'assist_ready': True}, {'domain': 'beta'}, {'events': 0},
                        {'last_seq': 1}, {'schema': 2}):
            a = read(dict(late, **changes))
            self.assertIn('malformed_late_motion', [x['code'] for x in a.issues])
            self.assertEqual(a.motion_late['bursts'], 0)

    def test_profile_suppressed_rejections_are_counted_in_the_totals(self):
        # F3-E: the persistent profile writes 5 rejections per kind per 10 s
        # and counts the rest in the digest; the report adds them back.
        digest = dict(kind='log_digest', schema=1, digest='periodic', profile='persistent',
                      mono_ns=3000000000, suppressed={'motion_rejected': 7, 'shadow_input_reset': 7})
        a = read(rejected(), digest)
        report = a.report()
        self.assertEqual(report['motion_rejected']['reasons'], {'stale': 1})
        self.assertEqual(report['motion_rejected']['suppressed_by_profile'], 7)
        self.assertEqual(report['motion_rejected']['total_including_suppressed'], 8)
        self.assertEqual(report['persistent_profile']['suppressed'],
                         {'motion_rejected': 7, 'shadow_input_reset': 7})

    def test_capture_inactive_not_confused_with_active_model(self):
        base = dict(kind='shadow_boot', domain='model', assist_ready=False,
                    source='existing_vbs_vim_callback', active=False, capture_active=True)
        a = read(base)
        self.assertIn('shadow_inactive', [x['code'] for x in a.issues])
        self.assertNotIn('motion_capture_inactive', [x['code'] for x in a.issues])
        a = read(dict(base, capture_active=False))
        self.assertIn('motion_capture_inactive', [x['code'] for x in a.issues])

    def test_capture_completion_is_current_boot_and_cannot_hide_new_records(self):
        boot_id = '01234567-89ab-cdef-0123-456789abcdef'
        boot = dict(kind='boot', schema=1, pid=123, mono_ns=1, mode=4,
                    install='ok', assist_ready=False, assist_block='unverified',
                    wire_timestamp_modified=False, boot_id=boot_id)
        end = dict(kind='capture_end', schema=1, mono_ns=3, boot_id=boot_id,
                   domain='model', assist_ready=False, reason='requested',
                   cutoff_ns=2, bounded_final_drain=True)
        a = read(boot, end)
        self.assertEqual(a.capture_ends, 1)
        self.assertEqual(a.session['last_diagnostic_ns'], 3)
        a.consume(dict(rejected(), kind='motion'), 'late')
        self.assertIn('record_after_capture_end', [x['code'] for x in a.issues])
        a = read(boot, end, end)
        self.assertEqual(a.capture_ends, 1)
        self.assertIn('invalid_capture_end_order', [x['code'] for x in a.issues])
        a = read(boot, dict(end, boot_id='unknown'))
        self.assertEqual(a.capture_ends, 0)
        self.assertIn('malformed_capture_end', [x['code'] for x in a.issues])
        for changes in ({'cutoff_ns': 4}, {'cutoff_ns': 0}, {'bounded_final_drain': 1}):
            a = read(boot, dict(end, **changes))
            self.assertEqual(a.capture_ends, 0)
            self.assertIn('malformed_capture_end', [x['code'] for x in a.issues])


if __name__ == '__main__':
    unittest.main()
