"""Production journal/Python contract; MODEL differences are not vehicle accuracy."""
import copy
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('analyze_logs', ROOT / 'tools/analyze_logs.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
FIXTURE = shlex.split(os.environ.get('MX5DR_SHADOW_FIXTURE', str(ROOT / 'build/test_shadow_log')))


def boot():
    return dict(kind='boot', schema=1, pid=123, mono_ns=1, mode=4,
                install='ok', assist_ready=False, assist_block='unverified',
                wire_timestamp_modified=False)


def calibration(**changes):
    row = dict(kind='shadow_calibration', mono_ns=100, domain='model', assist_ready=False,
               enabled=True, state='READY', candidate_ready=True, active_zero=2047,
               candidate_zero=2048.5, variance_counts2=0.25, samples=25,
               evidence_start_ns=10, evidence_end_ns=90, calibration_version=0)
    row.update(changes)
    return row


def holdout(event_name='BEGIN', **changes):
    row = dict(kind='shadow_holdout', mono_ns=100, domain='model', assist_ready=False,
               time_basis='receipt_model', event=event_name, reason='none', window_id=1,
               anchor_ns=100, reference_ns=0, frontier_ns=0, model_valid=False,
               lat=None, lon=None, ref_lat=None, ref_lon=None, position_error_m=None,
               heading_error_rad=None, calibration_version=1, yaw_zero=2048.5)
    if event_name == 'BEGIN':
        row.update(reference_ns=100, frontier_ns=100)
    elif event_name == 'COMPARED':
        row.update(mono_ns=200, reference_ns=200, frontier_ns=200, model_valid=True,
                   lat=35.123456789, lon=129.987654321, ref_lat=35.123456790,
                   ref_lon=129.987654322, position_error_m=3.0, heading_error_rad=-0.1)
    elif event_name in ('END', 'ABORT'):
        row.update(mono_ns=300, reason='complete' if event_name == 'END' else 'source_fault')
        if event_name == 'END':
            row['frontier_ns'] = 300
    row.update(changes)
    return row


def skipped_reference(**changes):
    row = holdout('SKIPPED', mono_ns=351, reason='stale_reference', window_id=0,
                  anchor_ns=0, reference_ns=100, frontier_ns=0)
    row.update(changes)
    return row


def wheel_calibration(**changes):
    row = calibration(wheel_enabled=True, wheel_candidate_ready=True, wheel_scale=1,
                      wheel_candidate_scale=1.02, wheel_scale_version=0, wheel_segments=3,
                      wheel_gps_distance_m=100, wheel_distance_m=98, wheel_evidence_end_ns=90,
                      gps_anchor_gate='ACCEPTED')
    row.update(changes)
    return row


def shadow(**changes):
    row = dict(kind='shadow', mono_ns=100, domain='model', model_valid=False,
               assist_ready=False, state=0, result='E_NO_SEED', pipeline='WAITING',
               uncertainties=127, events=0, intervals=0, resets=0, rejected=0,
               frontier_ns=0, lat=None, lon=None, heading_rad=None, speed_mps=None,
               error_model_m=None, stopped=False, preview_encoded=False, location_preview_hex='')
    row.update(changes)
    return row


def consume(rows, health_ns=1000):
    a = audit.Auditor()
    a.consume(boot(), 'boot')
    # Complete unchanged LOCAL payload pair permits meaningful pass/inconclusive status.
    a.consume(dict(kind='position', call=1, generation=1, mono_ns=2, mode=1,
                   utc_s=1, lat=0, lon=0, heading=0, kmh=0), 'position')
    a.consume(dict(kind='send', call=1, generation=1, mono_ns=3, mode=1,
                   type=1, length=48, choice=0, reason=0, result=0,
                   original_hex='00'*48, outgoing_hex='00'*48), 'send')
    for i, row in enumerate(rows):
        a.consume(row, str(i))
    if health_ns is not None:
        a.consume(dict(kind='health', mono_ns=health_ns, dropped=0,
                       hook_installed=True, assist_ready=False), 'health')
    return a


def codes(a):
    return [issue['code'] for issue in a.issues]


def reference_position(call=101, generation=7, mono_ns=100, **changes):
    row = dict(kind='position', call=call, generation=generation, mono_ns=mono_ns,
               mode=1, utc_s=1, lat=35.123456790, lon=129.987654322, heading=0, kmh=0)
    row.update(changes)
    return row


def identified_holdout(event_name='BEGIN', **changes):
    pair = ((101, 7) if event_name == 'BEGIN' else
            (202, 8) if event_name == 'COMPARED' else (None, None))
    row = holdout(event_name, reference_call=pair[0], reference_generation=pair[1])
    row.update(changes)
    return row


class HoldoutReferences(unittest.TestCase):
    def links(self, auditor):
        return auditor.report()['shadow_holdout'].get('reference_links', {})

    def test_exact_identity_is_separate_from_model_difference_statistics(self):
        rows = [reference_position(), identified_holdout(),
                reference_position(202, 8, 200), identified_holdout('COMPARED'),
                identified_holdout('END')]
        report = consume(rows).report()
        summary = report['shadow_holdout']
        self.assertEqual(summary.get('reference_links'), {'matched': 2, 'no_reference': 1})
        self.assertEqual(summary.get('reference_link_scope'),
                         'same_trace_group_and_recorded_session')
        self.assertEqual(report['status'], 'local_checks_pass')
        self.assertEqual(summary['position_difference_m']['count'], 1)
        self.assertEqual(summary['completed_windows'], 1)
        self.assertFalse(summary['gps_is_ground_truth'])
        self.assertEqual(summary['reference_exclusion'], 'not_provable_from_journal')
        self.assertEqual(summary['time_basis'], 'receipt_model')

    def test_time_and_coordinates_cannot_replace_call_and_generation(self):
        for call, generation in ((303, 8), (202, 9)):
            with self.subTest(call=call, generation=generation):
                a = consume([holdout(), reference_position(call, generation, 200),
                             identified_holdout('COMPARED'), identified_holdout('END')])
                self.assertEqual(self.links(a), {'legacy_without_identity': 1,
                                                'raw_missing': 1, 'no_reference': 1})
                self.assertIn('holdout_reference_missing', codes(a))
                self.assertEqual(a.holdout_position['count'], 1)
                self.assertEqual(a.holdout_completed, 1)

    def test_same_call_other_generation_cannot_displace_exact_reference(self):
        a = consume([holdout(), reference_position(202, 8, 200),
                     reference_position(202, 9, 200, lat=0),
                     identified_holdout('COMPARED'), identified_holdout('END')])
        self.assertEqual(self.links(a).get('matched'), 1)
        self.assertNotIn('holdout_reference_mismatch', codes(a))

    def test_uint32_zero_and_max_are_real_reference_values(self):
        for call, generation in ((0, 2**32-1), (2**32-1, 0)):
            with self.subTest(call=call, generation=generation):
                a = consume([reference_position(call, generation),
                             identified_holdout(reference_call=call, reference_generation=generation)])
                self.assertEqual(self.links(a).get('matched'), 1)
                self.assertNotIn('partial_record', codes(a))

    def test_legacy_and_null_reference_are_distinct(self):
        a = consume([holdout(), holdout('COMPARED'), holdout('END'),
                     identified_holdout('ABORT', window_id=0, anchor_ns=0)])
        self.assertEqual(self.links(a), {'legacy_without_identity': 3, 'no_reference': 1})
        self.assertEqual(a.holdout_position['count'], 1)

    def test_invalid_identity_is_diagnostic_without_dropping_comparison(self):
        invalid = [dict(reference_call=202), dict(reference_generation=8),
                   dict(reference_call=None, reference_generation=8),
                   dict(reference_call=202, reference_generation=None),
                   dict(reference_call=None, reference_generation=None)]
        for value in (True, -1, 2**32, 1.0, '202'):
            invalid += [dict(reference_call=value, reference_generation=8),
                        dict(reference_call=202, reference_generation=value)]
        for fields in invalid:
            with self.subTest(fields=fields):
                a = consume([holdout(), holdout('COMPARED', **fields), holdout('END')])
                self.assertEqual(self.links(a), {'legacy_without_identity': 2, 'malformed': 1})
                self.assertIn('partial_record', codes(a))
                self.assertEqual(a.holdout_position['count'], 1)
                self.assertEqual(a.holdout_completed, 1)

    def test_reference_events_require_identity_when_new_fields_are_present(self):
        for row in (identified_holdout(reference_call=None, reference_generation=None),
                    skipped_reference(reference_call=None, reference_generation=None)):
            with self.subTest(event=row['event']):
                a = consume([row])
                self.assertEqual(self.links(a).get('malformed'), 1)
                self.assertIn('partial_record', codes(a))

    def test_terminal_events_cannot_invent_reference_except_output_overflow(self):
        for event in ('END', 'ABORT'):
            with self.subTest(event=event):
                a = consume([holdout(), reference_position(202, 8, 200),
                             identified_holdout('COMPARED'),
                             identified_holdout(event, reference_call=202, reference_generation=8)])
                self.assertEqual(self.links(a), {'legacy_without_identity': 1, 'matched': 1, 'malformed': 1})
                self.assertIn('partial_record', codes(a))
                self.assertEqual(a.holdout_position['count'], 1)

    def test_overflow_abort_can_retain_real_reference(self):
        a = consume([holdout(), reference_position(202, 8, 200),
                     identified_holdout('ABORT', reason='output_overflow', reference_ns=200,
                                        reference_call=202, reference_generation=8)])
        self.assertEqual(self.links(a), {'legacy_without_identity': 1, 'matched': 1})
        self.assertEqual(a.holdout_aborted, 1)
        self.assertNotIn('partial_record', codes(a))

    def test_same_identity_in_next_boot_cannot_reuse_old_raw_row(self):
        a = consume([reference_position(), identified_holdout(), identified_holdout('END'),
                     boot(), identified_holdout(), identified_holdout('END')])
        self.assertEqual(self.links(a), {'matched': 1, 'no_reference': 2, 'raw_missing': 1})
        self.assertIn('holdout_reference_missing', codes(a))

    def test_malformed_boot_still_breaks_reference_identity(self):
        malformed = boot(); del malformed['pid']
        a = consume([reference_position(), malformed, identified_holdout()])
        self.assertEqual(self.links(a), {'raw_missing': 1})
        self.assertIn('partial_record', codes(a))

    def test_duplicate_before_or_after_reference_is_ambiguous(self):
        for duplicate_at in ('before', 'after'):
            with self.subTest(duplicate_at=duplicate_at):
                raw = reference_position(202, 8, 200)
                rows = [holdout(), raw, identified_holdout('COMPARED'), identified_holdout('END')]
                rows.insert(2 if duplicate_at == 'before' else len(rows), dict(raw))
                a = consume(rows)
                self.assertEqual(self.links(a), {'legacy_without_identity': 1,
                                                'ambiguous': 1, 'no_reference': 1})
                self.assertIn('holdout_reference_ambiguous', codes(a))
                self.assertEqual(a.holdout_position['count'], 1)

    def test_failed_adapter_position_cannot_be_holdout_reference(self):
        raw = reference_position(202, 8, 200)
        raw['reason'] = 13
        a = consume([holdout(), raw, identified_holdout('COMPARED'),
                     identified_holdout('END')])
        self.assertEqual(self.links(a), {'legacy_without_identity': 1,
                                         'raw_unavailable': 1, 'no_reference': 1})
        self.assertIn('holdout_reference_unavailable', codes(a))
        self.assertEqual(a.report()['status'], 'inconclusive')

    def test_late_duplicates_reclassify_all_matches_once(self):
        raw = reference_position()
        a = consume([raw, identified_holdout(), identified_holdout('END'),
                     identified_holdout(window_id=2), identified_holdout('END', window_id=2),
                     dict(raw), dict(raw)])
        self.assertEqual(self.links(a), {'ambiguous': 2, 'no_reference': 2})

    def test_reference_consistency_is_checked_only_after_identity_match(self):
        for changes in (dict(mono_ns=199), dict(lat=0), dict(lon=0), dict(lat=None)):
            with self.subTest(changes=changes):
                raw = reference_position(202, 8, 200)
                raw.update(changes)
                a = consume([holdout(), raw, identified_holdout('COMPARED'), identified_holdout('END')])
                report = a.report()
                self.assertEqual(report['shadow_holdout'].get('reference_links'),
                                 {'legacy_without_identity': 1, 'mismatch': 1, 'no_reference': 1})
                self.assertIn('holdout_reference_mismatch', codes(a))
                self.assertEqual(report['status'], 'violation')
                self.assertEqual(a.holdout_position['count'], 1)

    def encode(self, rows):
        return ''.join(json.dumps(row) + '\n' for row in rows).encode()

    def test_trace_rotations_share_reference_group_for_directory_and_file_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            older, newer = root / 'trace.2.jsonl', root / 'trace.0.jsonl'
            older.write_bytes(self.encode([boot(), reference_position()]))
            newer.write_bytes(self.encode([identified_holdout(), identified_holdout('END')]))
            for inputs in ([root], [older, newer]):
                with self.subTest(inputs=inputs):
                    report = audit.analyze(inputs)
                    self.assertEqual(report['shadow_holdout'].get('reference_links'),
                                     {'matched': 1, 'no_reference': 1})
                    self.assertNotIn('missing_boot', [i['code'] for i in report['issues']])

    def test_collector_and_storage_files_do_not_split_aa_rotations(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            older, newer = root / 'trace.1.jsonl', root / 'trace.0.jsonl'
            collector = root / 'collector.0.jsonl'
            storage = root / 'trace.storage.json'
            older.write_bytes(self.encode([boot(), reference_position()]))
            newer.write_bytes(self.encode([identified_holdout(), identified_holdout('END')]))
            collector.write_bytes(self.encode([dict(kind='collector_boot', stream='collector',
                collector_pid=456, observed_at_mono_ns=50, producer_mono_ns=None,
                producer_time_status='unknown', schema=1, sample_ms=1000, session_seconds=10,
                boot_id='12345678-1234-1234-1234-123456789abc')]))
            storage.write_bytes(self.encode([dict(kind='storage_stop', stream='trace', pid=123,
                mono_ns=150, reserve_bytes=0, margin_bytes=0, syscall_errno=5,
                boot_id='12345678-1234-1234-1234-123456789abc', reason='space_query_failed',
                available_bytes=None)]))
            report = audit.analyze([older, collector, storage, newer])
            self.assertEqual(report['shadow_holdout'].get('reference_links'),
                             {'matched': 1, 'no_reference': 1})
            self.assertIn('storage_stopped', [i['code'] for i in report['issues']])
            self.assertNotIn('missing_boot', [i['code'] for i in report['issues']])

    def test_independent_directories_and_plain_files_cannot_share_missing_boot_identity(self):
        for plain_files in (False, True):
            with self.subTest(plain_files=plain_files), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                if plain_files:
                    first, second = root / 'first.jsonl', root / 'second.jsonl'
                else:
                    (root / 'first').mkdir(); (root / 'second').mkdir()
                    first = root / 'first' / 'trace.0.jsonl'
                    second = root / 'second' / 'trace.0.jsonl'
                first.write_bytes(self.encode([boot(), reference_position()]))
                second.write_bytes(self.encode([identified_holdout(), identified_holdout('END')]))
                report = audit.analyze([first, second])
                self.assertEqual(report['shadow_holdout'].get('reference_links'),
                                 {'raw_missing': 1, 'no_reference': 1})
                self.assertIn('missing_boot', [i['code'] for i in report['issues']])

    def write_tar(self, path, entries):
        with tarfile.open(path, 'w') as archive:
            for name, rows in entries:
                raw = self.encode(rows)
                member = tarfile.TarInfo(name); member.size = len(raw)
                archive.addfile(member, io.BytesIO(raw))

    def test_archive_rotations_share_group_but_separate_exports_do_not(self):
        for separate_export in (False, True):
            with self.subTest(separate_export=separate_export), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                first, second = root / 'first.tar', root / 'second.tar'
                old = ('logs/trace.2.jsonl', [boot(), reference_position()])
                new = ('logs/trace.0.jsonl', [identified_holdout(), identified_holdout('END')])
                self.write_tar(first, [old] if separate_export else [new, old])
                inputs = [first]
                if separate_export:
                    self.write_tar(second, [new]); inputs.append(second)
                report = audit.analyze(inputs)
                self.assertEqual(report['shadow_holdout'].get('reference_links'),
                                 {'raw_missing' if separate_export else 'matched': 1, 'no_reference': 1})

    def test_separate_member_directories_cannot_share_reference_group(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'sessions.tar'
            self.write_tar(path, [('first/trace.0.jsonl', [boot(), reference_position()]),
                                  ('second/trace.0.jsonl', [identified_holdout(), identified_holdout('END')])])
            report = audit.analyze([path])
            self.assertEqual(report['shadow_holdout'].get('reference_links'),
                             {'raw_missing': 1, 'no_reference': 1})

    def test_bootless_same_group_can_pair_without_claiming_complete_session(self):
        raw = self.encode([reference_position(), identified_holdout(), identified_holdout('END')])
        a = audit.Auditor(); a.read_stream(io.BytesIO(raw), 'partial trace', len(raw))
        report = a.report()
        self.assertEqual(report['shadow_holdout'].get('reference_links'), {'matched': 1, 'no_reference': 1})
        self.assertIn('missing_boot', [i['code'] for i in report['issues']])

    def test_truncated_raw_is_missing_without_discarding_recorded_comparison(self):
        raw = (self.encode([boot(), holdout()]) + b'{"kind":"position","call":202\n' +
               self.encode([identified_holdout('COMPARED'), identified_holdout('END')]))
        a = audit.Auditor(); a.read_stream(io.BytesIO(raw), 'partial trace', len(raw))
        self.assertEqual(self.links(a), {'legacy_without_identity': 1, 'raw_missing': 1, 'no_reference': 1})
        self.assertIn('malformed_json', codes(a))
        self.assertEqual(a.holdout_position['count'], 1)


class CalibrationLogs(unittest.TestCase):
    def test_cpp_overflow_reference_legacy_remains_an_inconclusive_abort(self):
        row = json.loads(subprocess.check_output(FIXTURE + ['--emit-overflow-reference'], text=True))
        # Before nullable identity was added, this exact formatter event already
        # existed. Remove only the two added keys to preserve the legacy shape.
        del row['reference_call']; del row['reference_generation']
        a = consume([row], health_ns=2**64-1)
        self.assertEqual(codes(a), ['holdout_aborted'])
        report = a.report()
        self.assertEqual(report['status'], 'inconclusive')
        self.assertEqual(report['shadow_holdout']['reference_links'], {'legacy_without_identity': 1})
        self.assertEqual(report['shadow_holdout']['position_difference_m']['count'], 0)
        for fields in (dict(reference_call=0), dict(reference_generation=2**32-1),
                       dict(reference_call=None, reference_generation=None),
                       dict(reference_call=True, reference_generation=2**32-1)):
            with self.subTest(fields=fields):
                self.assertIn('invalid_holdout_time', codes(consume([dict(row, **fields)])))

    def test_cpp_overflow_reference_is_an_abort_with_real_identity(self):
        row = json.loads(subprocess.check_output(FIXTURE + ['--emit-overflow-reference'], text=True))
        self.assertEqual((row['event'], row['reason']), ('ABORT', 'output_overflow'))
        self.assertEqual((row['window_id'], row['anchor_ns'], row['frontier_ns']), (0, 0, 0))
        self.assertEqual((row['reference_call'], row['reference_generation']), (0, 2**32-1))
        self.assertEqual((row['reference_ns'], row['mono_ns']), (1000000000, 1100000000))
        a = consume([reference_position(0, 2**32-1, 1000000000), row], health_ns=2**64-1)
        self.assertEqual(codes(a), ['holdout_aborted'])
        report = a.report()
        self.assertEqual(report['status'], 'inconclusive')
        self.assertEqual(report['shadow_holdout']['reference_links'], {'matched': 1})
        self.assertEqual(report['shadow_holdout']['aborted_windows'], 0)
        self.assertEqual(report['shadow_holdout']['position_difference_m']['count'], 0)
        for fields in (dict(reason='source_fault'), dict(reference_call=None, reference_generation=None),
                       dict(frontier_ns=1), dict(anchor_ns=1), dict(model_valid=True), dict(lat=1),
                       dict(reference_ns=1100000001)):
            with self.subTest(fields=fields):
                self.assertIn('invalid_holdout_time', codes(consume([dict(row, **fields)])))

    def test_cpp_production_formatter_roundtrip(self):
        emitted = subprocess.check_output(FIXTURE + ['--emit'], text=True)
        rows = [json.loads(line) for line in emitted.splitlines()]
        self.assertTrue(any(row['kind'] == 'shadow_calibration' for row in rows))
        self.assertEqual([r['event'] for r in rows if r['kind'] == 'shadow_holdout'],
                         ['BEGIN', 'COMPARED', 'END'])
        self.assertEqual([(row.get('reference_call'), row.get('reference_generation'))
                          for row in rows if row['kind'] == 'shadow_holdout'],
                         [(101, 7), (202, 8), (None, None)])
        self.assertTrue(all('reference_call' in row and 'reference_generation' in row
                            for row in rows if row['kind'] == 'shadow_holdout'))
        # Known authored fixture observations, independent of emitted IDs/time.
        observed = [rows[0], reference_position(101, 7, 6000000000), rows[1],
                    reference_position(202, 8, 7000000000, lat=35, lon=135), *rows[2:]]
        a = consume(observed, health_ns=2**64-1)
        self.assertEqual(codes(a), [])
        report = a.report()
        self.assertEqual(report['status'], 'local_checks_pass')
        self.assertEqual(report['shadow_holdout']['position_difference_m'],
                         dict(count=1, min=3.0, max=3.0, mean=3.0))
        self.assertEqual(report['shadow_holdout']['reference_links'], {'matched': 2, 'no_reference': 1})
        self.assertEqual(report['shadow_calibration']['wheel_scale'],
                         dict(count=1, min=1.02, max=1.02, mean=1.02))
        self.assertEqual(report['shadow_calibration']['wheel_versions'], {'1': 1})
        self.assertEqual(report['shadow_calibration']['gps_anchor_gates'], {'ACCEPTED': 1})
        self.assertTrue(all(row['wheel_scale'] == 1.02 and row['wheel_scale_version'] == 1
                            for row in rows if row['kind'] == 'shadow_holdout'))
        self.assertLess(max(map(len, emitted.splitlines())), audit.MAX_LINE_BYTES)

    def test_optional_wheel_pair_legacy_bounds_and_invalid_fields(self):
        for factory in (shadow, holdout):
            for fields in ({}, dict(wheel_scale=0.95, wheel_scale_version=0),
                           dict(wheel_scale=1.05, wheel_scale_version=2**64-1)):
                with self.subTest(factory=factory.__name__, fields=fields):
                    self.assertEqual(codes(consume([factory(**fields)])), [])
            invalid = [dict(wheel_scale=1), dict(wheel_scale_version=0)]
            invalid += [dict(wheel_scale=value, wheel_scale_version=0)
                        for value in (True, None, '1', 0.949, 1.051, float('inf'), float('nan'))]
            invalid += [dict(wheel_scale=1, wheel_scale_version=value)
                        for value in (True, -1, 2**64, 1.0)]
            for fields in invalid:
                with self.subTest(factory=factory.__name__, fields=fields):
                    self.assertIn('partial_record', codes(consume([factory(**fields)])))

    def test_wheel_calibration_group_is_optional_but_complete(self):
        self.assertEqual(codes(consume([calibration()])), [])
        row = wheel_calibration()
        for key in set(row) - set(calibration()):
            bad = dict(row); del bad[key]
            with self.subTest(missing=key):
                a = consume([bad])
                self.assertIn('partial_record', codes(a))
                self.assertEqual(a.calibration_states, {})
                self.assertEqual(a.wheel_scales['count'], 0)
        summary = consume([row, wheel_calibration(wheel_scale=1.02, wheel_scale_version=1,
            wheel_candidate_ready=False, wheel_segments=0, wheel_gps_distance_m=0,
            wheel_distance_m=0, wheel_evidence_end_ns=0, gps_anchor_gate='JUMP')]).report()['shadow_calibration']
        self.assertEqual(summary['wheel_scale'], dict(count=2, min=1, max=1.02, mean=1.01))
        self.assertEqual(summary['wheel_versions'], {'0': 1, '1': 1})
        self.assertEqual(summary['gps_anchor_gates'], {'ACCEPTED': 1, 'JUMP': 1})

    def test_wheel_calibration_invalid_evidence(self):
        malformed = [('wheel_enabled', 1), ('wheel_candidate_ready', 'true'),
                     ('wheel_candidate_scale', 0.949), ('wheel_candidate_scale', 1.051),
                     ('wheel_scale', float('nan')), ('wheel_segments', True),
                     ('wheel_segments', -1), ('wheel_segments', 2**64),
                     ('wheel_scale_version', 2**64), ('wheel_evidence_end_ns', -1),
                     ('wheel_evidence_end_ns', 2**64), ('wheel_gps_distance_m', -1),
                     ('wheel_distance_m', float('inf')), ('wheel_distance_m', True),
                     ('gps_anchor_gate', ''), ('gps_anchor_gate', None)]
        inconsistent = [('wheel_enabled', False), ('wheel_segments', 2),
                        ('wheel_gps_distance_m', 99), ('wheel_distance_m', 0),
                        ('wheel_evidence_end_ns', 0), ('wheel_evidence_end_ns', 101)]
        for changes, expected in ((malformed, 'partial_record'),
                                  (inconsistent, 'invalid_calibration_evidence')):
            for key, value in changes:
                with self.subTest(key=key, value=value):
                    a = consume([wheel_calibration(**{key: value})])
                    self.assertIn(expected, codes(a))
                    self.assertEqual(a.calibration_states, {})
        # A consumed candidate may retain evidence or reset it on apply.
        self.assertEqual(codes(consume([wheel_calibration(wheel_candidate_ready=False)])), [])
        self.assertIn('invalid_calibration_evidence', codes(consume([
            wheel_calibration(wheel_candidate_ready=False, wheel_evidence_end_ns=101)])))

    def test_wheel_pair_cannot_change_appear_or_disappear_within_window(self):
        pair = dict(wheel_scale=1.02, wheel_scale_version=1)
        for event in ('COMPARED', 'END'):
            for fields in ({}, dict(wheel_scale=1, wheel_scale_version=1),
                           dict(wheel_scale=1.02, wheel_scale_version=2)):
                with self.subTest(event=event, fields=fields):
                    a = consume([holdout(**pair), holdout(event, **fields)])
                    self.assertIn('holdout_window_changed', codes(a))
                    self.assertEqual(a.holdout_position['count'], 0)
                    self.assertEqual(a.holdout_completed, 0)
            a = consume([holdout(), holdout(event, **pair)])
            self.assertIn('holdout_window_changed', codes(a))
        a = consume([holdout(**pair), holdout('COMPARED', **pair), holdout('END', **pair)])
        self.assertEqual(codes(a), [])
        self.assertEqual(a.holdout_position['count'], 1)
        for fields in ({}, dict(wheel_scale=1, wheel_scale_version=0)):
            a = consume([holdout(**pair), holdout('ABORT', **fields)])
            self.assertEqual(codes(a), ['holdout_aborted'])
            self.assertIsNone(a.session['holdout_window'])

    def test_calibration_counts_do_not_qualify_model(self):
        rows = [calibration(), calibration(state='APPLIED', candidate_ready=False,
                                          active_zero=2048.5, calibration_version=1),
                calibration(enabled=False, state='DISABLED', candidate_ready=False,
                            samples=0, evidence_start_ns=0, evidence_end_ns=0)]
        report = consume(rows).report()
        self.assertEqual(report['status'], 'local_checks_pass')
        summary = report['shadow_calibration']
        self.assertEqual(summary['states'], {'READY': 1, 'APPLIED': 1, 'DISABLED': 1})
        self.assertEqual(summary['versions'], {'0': 2, '1': 1})
        self.assertEqual(summary['enabled'], {'true': 2, 'false': 1})
        self.assertEqual(summary['candidate_ready'], {'true': 1, 'false': 2})
        self.assertEqual(summary['samples_max'], 25)
        self.assertIn('not_verified_calibration', summary['scope'])
        self.assertEqual(report['dr_accuracy'], 'not_established')

    def test_calibration_invalid_types_and_ranges(self):
        changes = [('mono_ns', 0), ('mono_ns', 2**64), ('samples', True),
                   ('samples', -1), ('samples', 2**64), ('calibration_version', 1.0),
                   ('calibration_version', 2**64), ('evidence_start_ns', -1),
                   ('evidence_end_ns', 2**64), ('state', ''), ('enabled', 1),
                   ('candidate_ready', 'true'), ('active_zero', False),
                   ('active_zero', -1), ('candidate_zero', 4094),
                   ('candidate_zero', float('nan')), ('active_zero', float('inf')),
                   ('variance_counts2', -0.1), ('variance_counts2', None)]
        for key, value in changes:
            with self.subTest(key=key, value=value):
                a = consume([calibration(**{key: value})])
                self.assertIn('partial_record', codes(a))
                self.assertEqual(a.calibration_states, {})

    def test_calibration_evidence_order_and_candidate_consistency(self):
        for changes in (dict(evidence_start_ns=0), dict(evidence_end_ns=0),
                        dict(evidence_start_ns=95), dict(evidence_end_ns=101),
                        dict(evidence_start_ns=90), dict(samples=0), dict(enabled=False)):
            with self.subTest(changes=changes):
                a = consume([calibration(**changes)])
                self.assertIn('invalid_calibration_evidence', codes(a))
        a = consume([calibration(candidate_ready=False, samples=0, candidate_zero=0,
                                 evidence_start_ns=0, evidence_end_ns=0)])
        self.assertEqual(codes(a), [])

    def test_envelope_violations_cannot_be_hidden_by_bad_payload(self):
        for factory in (calibration, holdout):
            for changes, code in ((dict(domain='qualified'), 'unexpected_shadow_domain'),
                                  (dict(assist_ready=True), 'impossible_live_capability')):
                with self.subTest(factory=factory.__name__, changes=changes):
                    bad = factory(**changes)
                    bad['mono_ns'] = -1
                    a = consume([bad])
                    self.assertIn(code, codes(a))
                    self.assertEqual(a.report()['status'], 'violation')
        a = consume([holdout(time_basis='producer_verified')])
        self.assertIn('unexpected_holdout_time_basis', codes(a))
        self.assertEqual(a.report()['status'], 'violation')

    def test_holdout_comparison_statistics_without_coordinates(self):
        rows = [holdout(), holdout('COMPARED'),
                holdout('COMPARED', mono_ns=250, reference_ns=250, frontier_ns=250,
                        position_error_m=7, heading_error_rad=0.3), holdout('END')]
        report = consume(rows).report()
        self.assertEqual(report['status'], 'local_checks_pass')
        summary = report['shadow_holdout']
        self.assertEqual(summary['events'], {'BEGIN': 1, 'COMPARED': 2, 'END': 1})
        self.assertEqual(summary['reasons'], {'none': 3, 'complete': 1})
        self.assertEqual(summary['position_difference_m'], dict(count=2, min=3, max=7, mean=5))
        self.assertAlmostEqual(summary['heading_difference_rad']['mean'], 0.1)
        self.assertEqual(summary['completed_windows'], 1)
        self.assertFalse(summary['gps_is_ground_truth'])
        self.assertEqual(summary['reference_exclusion'], 'not_provable_from_journal')
        encoded = json.dumps(report)
        for coordinate in ('35.123456789', '129.987654321', '35.12345679', '129.987654322'):
            self.assertNotIn(coordinate, encoded)
        for key in ('lat', 'lon', 'ref_lat', 'ref_lon'):
            self.assertNotIn('"' + key + '"', encoded)

    def test_nullable_heading_does_not_invent_heading_comparison(self):
        a = consume([holdout(), holdout('COMPARED', heading_error_rad=None), holdout('END')])
        self.assertEqual(codes(a), [])
        self.assertEqual(a.holdout_position['count'], 1)
        self.assertEqual(a.holdout_heading, dict(count=0, min=None, max=None, mean=None))

    def test_holdout_malformed_numbers_and_event(self):
        changes = [('window_id', 0), ('window_id', 2**64), ('anchor_ns', True),
                   ('reference_ns', -1), ('frontier_ns', 2**64), ('calibration_version', -1),
                   ('event', 'ACCURATE'), ('model_valid', 1), ('yaw_zero', -1),
                   ('yaw_zero', float('nan')), ('lat', 91), ('lon', -181),
                   ('ref_lat', True), ('ref_lon', 181), ('position_error_m', -1),
                   ('position_error_m', float('inf')), ('heading_error_rad', math.pi+0.01)]
        for key, value in changes:
            with self.subTest(key=key, value=value):
                a = consume([holdout(), holdout('COMPARED', **{key: value})])
                self.assertIn('partial_record', codes(a))
                self.assertEqual(a.holdout_position['count'], 0)
        for key in ('lat', 'position_error_m'):
            bad = holdout('COMPARED'); del bad[key]
            self.assertIn('partial_record', codes(consume([holdout(), bad])))

    def test_holdout_timestamp_alignment_and_required_comparison_values(self):
        for changes, expected in (
                (dict(anchor_ns=0), 'invalid_holdout_time'),
                (dict(mono_ns=199), 'invalid_holdout_time'),
                (dict(model_valid=False), 'invalid_holdout_comparison'),
                (dict(anchor_ns=200), 'invalid_holdout_comparison'),
                (dict(reference_ns=199), 'invalid_holdout_comparison'),
                (dict(frontier_ns=199), 'invalid_holdout_comparison'),
                (dict(lat=None), 'invalid_holdout_comparison'),
                (dict(position_error_m=None), 'invalid_holdout_comparison')):
            with self.subTest(changes=changes):
                a = consume([holdout(), holdout('COMPARED', **changes)])
                self.assertIn(expected, codes(a))
                self.assertEqual(a.holdout_position['count'], 0)
        for event in ('BEGIN', 'END', 'ABORT'):
            for field in ('position_error_m', 'heading_error_rad'):
                a = consume([holdout(event, **{field: 0})])
                self.assertIn('invalid_holdout_comparison', codes(a))

    def test_missing_begin_wrong_window_and_open_tail_are_inconclusive(self):
        for rows in ([holdout('COMPARED')], [holdout('END')],
                     [holdout(), holdout('COMPARED', window_id=2)],
                     [holdout('BEGIN')]):
            with self.subTest(rows=rows):
                a = consume(rows)
                report = a.report()
                self.assertEqual(report['status'], 'inconclusive')
                self.assertEqual(a.holdout_position['count'], 0)
                self.assertTrue({'holdout_missing_begin', 'holdout_unfinished_window'} & set(codes(a)))

    def test_repeated_begin_does_not_hide_unfinished_window(self):
        a = consume([holdout(), holdout(window_id=2), holdout('END', window_id=2)])
        self.assertIn('holdout_unfinished_window', codes(a))
        self.assertEqual(a.report()['status'], 'inconclusive')

    def test_begin_requires_reference_and_frontier_at_anchor(self):
        for changes in (dict(reference_ns=0), dict(frontier_ns=0),
                        dict(reference_ns=99), dict(frontier_ns=99),
                        dict(reference_ns=101), dict(frontier_ns=101)):
            with self.subTest(changes=changes):
                a = consume([holdout(mono_ns=200, **changes)])
                self.assertIn('invalid_holdout_time', codes(a))
                self.assertIsNone(a.session['holdout_window'])
                self.assertEqual(a.report()['status'], 'inconclusive')
        a = consume([holdout(), holdout(window_id=2, reference_ns=0)])
        self.assertIn('invalid_holdout_time', codes(a))
        self.assertEqual(a.session['holdout_window']['window_id'], 1)

    def test_end_frontier_must_advance_anchor_and_cover_last_comparison(self):
        for compared, frontier in ((False, 0), (False, 99), (False, 100),
                                   (True, 0), (True, 100), (True, 199)):
            with self.subTest(compared=compared, frontier=frontier):
                rows = [holdout()] + ([holdout('COMPARED')] if compared else [])
                a = consume(rows + [holdout('END', frontier_ns=frontier)])
                self.assertIn('invalid_holdout_time', codes(a))
                self.assertEqual(a.holdout_completed, 0)
                self.assertIsNotNone(a.session['holdout_window'])
                report = a.report()
                self.assertEqual(report['status'], 'inconclusive')
                self.assertIn('holdout_unfinished_window', codes(a))
        for frontier in (200, 300):
            a = consume([holdout(), holdout('COMPARED'),
                         holdout('END', reference_ns=0, frontier_ns=frontier)])
            self.assertEqual(codes(a), [])
            self.assertEqual(a.holdout_completed, 1)
            self.assertIsNone(a.session['holdout_window'])

    def test_comparison_reference_high_water_never_rewinds(self):
        rows = [holdout(), holdout('COMPARED', mono_ns=300, reference_ns=250, frontier_ns=250),
                holdout('COMPARED', mono_ns=310), holdout('COMPARED', mono_ns=320),
                holdout('COMPARED', mono_ns=330, reference_ns=275, frontier_ns=275),
                holdout('END', mono_ns=340)]
        a = consume(rows)
        self.assertEqual(codes(a).count('holdout_reference_replayed'), 2)
        self.assertEqual(a.holdout_position['count'], 2)
        self.assertEqual(a.report()['status'], 'inconclusive')

    def test_window_anchor_calibration_and_clock_cannot_change(self):
        for changes in (dict(anchor_ns=99), dict(calibration_version=2), dict(yaw_zero=2047)):
            a = consume([holdout(), holdout('COMPARED', **changes)])
            self.assertIn('holdout_window_changed', codes(a))
            self.assertEqual(a.holdout_position['count'], 0)
        a = consume([holdout(mono_ns=250), holdout('COMPARED')])
        self.assertIn('invalid_holdout_time', codes(a))
        self.assertEqual(a.holdout_position['count'], 0)

    def test_abort_and_later_recovery_keep_fault_visible(self):
        a = consume([holdout(), holdout('ABORT'), holdout(window_id=2, mono_ns=400),
                     holdout('COMPARED', window_id=2, mono_ns=500, reference_ns=500, frontier_ns=500),
                     holdout('END', window_id=2, mono_ns=600, frontier_ns=600)])
        self.assertIn('holdout_aborted', codes(a))
        report = a.report()
        self.assertEqual(report['status'], 'inconclusive')
        self.assertEqual(report['shadow_holdout']['aborted_windows'], 1)
        self.assertEqual(report['shadow_holdout']['completed_windows'], 1)
        self.assertEqual(a.holdout_position['count'], 1)

    def test_warmup_abort_is_visible_without_inventing_window(self):
        warmup = holdout('ABORT', window_id=0, anchor_ns=0, reference_ns=0, frontier_ns=0)
        a = consume([warmup])
        self.assertEqual(codes(a), ['holdout_aborted'])
        report = a.report()
        self.assertEqual(report['status'], 'inconclusive')
        self.assertEqual(report['shadow_holdout']['events'], {'ABORT': 1})
        self.assertEqual(report['shadow_holdout']['aborted_windows'], 0)
        a = consume([holdout(), warmup])
        self.assertIn('holdout_missing_begin', codes(a))
        self.assertIsNotNone(a.session['holdout_window'])
        self.assertIn('invalid_holdout_time', codes(consume([
            holdout('ABORT', window_id=0, anchor_ns=0, frontier_ns=1)])))

    def test_stale_reference_skip_is_visible_without_ending_a_window(self):
        emitted = json.loads(subprocess.check_output(FIXTURE + ['--emit-skipped'], text=True))
        self.assertEqual(emitted['event'], 'SKIPPED')
        self.assertEqual(emitted['reason'], 'stale_reference')
        self.assertEqual((emitted.get('reference_call'), emitted.get('reference_generation')), (0, 2**32-1))
        linked = consume([reference_position(0, 2**32-1, 1000000000), emitted], health_ns=2**64-1)
        self.assertEqual(codes(linked), ['holdout_reference_stale'])
        self.assertEqual(linked.report()['shadow_holdout']['reference_links'], {'matched': 1})
        a = consume([skipped_reference(), skipped_reference(mono_ns=500, reference_ns=200)])
        self.assertEqual(codes(a), ['holdout_reference_stale'] * 2)
        self.assertEqual(a.report()['status'], 'inconclusive')
        self.assertEqual(a.report()['shadow_holdout']['events'], {'SKIPPED': 2})
        self.assertEqual(a.report()['shadow_holdout']['aborted_windows'], 0)
        for changes in (dict(window_id=1), dict(anchor_ns=100), dict(reference_ns=0),
                        dict(reference_ns=351), dict(frontier_ns=100), dict(reason='none'),
                        dict(model_valid=True), dict(ref_lat=35)):
            with self.subTest(changes=changes):
                self.assertIn('invalid_holdout_time', codes(consume([skipped_reference(**changes)])))
        self.assertIn('invalid_holdout_boundary', codes(consume([holdout(), skipped_reference()])))
        stale_abort = holdout('ABORT', reason='stale_reference', window_id=0,
                              anchor_ns=0, reference_ns=0, frontier_ns=0)
        self.assertEqual(codes(consume([stale_abort])), ['holdout_aborted'])

    def test_abort_after_pipeline_reset_may_report_reset_calibration(self):
        a = consume([holdout(), holdout('ABORT', calibration_version=0, yaw_zero=2047)])
        self.assertEqual(codes(a), ['holdout_aborted'])
        self.assertIsNone(a.session['holdout_window'])

    def test_uint64_timestamps_remain_exact_and_difference_mean_stays_finite(self):
        offset = 2**63
        rows = [calibration(), holdout(), holdout('COMPARED', position_error_m=1e308),
                holdout('COMPARED', mono_ns=250, reference_ns=250, frontier_ns=250,
                        position_error_m=1.5e308), holdout('END')]
        for row in rows:
            for key in ('mono_ns', 'anchor_ns', 'reference_ns', 'frontier_ns',
                        'evidence_start_ns', 'evidence_end_ns'):
                if row.get(key):
                    row[key] += offset
        a = consume(rows, health_ns=2**64-1)
        self.assertEqual(codes(a), [])
        self.assertEqual(a.session['last_diagnostic_ns'], offset + 300)
        self.assertEqual(a.holdout_position['mean'], 1.25e308)
        json.dumps(a.report(), allow_nan=False)

    def test_boot_resets_window_correlation_and_preserves_prior_open_tail(self):
        a = consume([holdout(), boot(), holdout('COMPARED')])
        self.assertIn('holdout_missing_begin', codes(a))
        self.assertEqual(a.holdout_position['count'], 0)
        self.assertIn('holdout_unfinished_window', {i['code'] for i in a.report()['issues']})
        a = consume([holdout(), holdout('END'), boot(), holdout(), holdout('COMPARED'), holdout('END')])
        self.assertEqual(codes(a), [])
        self.assertEqual(a.holdout_position['count'], 1)

    def test_both_diagnostic_kinds_extend_required_health_coverage(self):
        for row in (calibration(), holdout('ABORT')):
            with self.subTest(kind=row['kind']):
                a = consume([row], health_ns=99)
                self.assertIn('uncovered_trace_tail', {i['code'] for i in a.report()['issues']})
                # Even malformed diagnostics with usable timestamps leave coverage incomplete.
                bad = copy.deepcopy(row); bad['domain'] = 'qualified'
                a = consume([bad], health_ns=99)
                self.assertIn('uncovered_trace_tail', {i['code'] for i in a.report()['issues']})

    def test_nonfinite_json_rejected_before_summary(self):
        row = holdout('COMPARED', position_error_m=float('nan'))
        raw = (json.dumps(row) + '\n').encode()
        a = audit.Auditor(); a.read_stream(io.BytesIO(raw), 'synthetic', len(raw))
        self.assertIn('malformed_json', codes(a))
        self.assertEqual(a.holdout_position['count'], 0)

    def test_empty_and_legacy_logs_do_not_require_new_records(self):
        report = consume([]).report()
        self.assertEqual(report['status'], 'local_checks_pass')
        self.assertEqual(report['shadow_holdout']['events'], {})
        self.assertEqual(report['shadow_calibration']['states'], {})


if __name__ == '__main__':
    unittest.main()
