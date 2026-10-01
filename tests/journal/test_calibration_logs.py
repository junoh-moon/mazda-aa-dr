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


class CalibrationLogs(unittest.TestCase):
    def test_cpp_production_formatter_roundtrip(self):
        emitted = subprocess.check_output(FIXTURE + ['--emit'], text=True)
        rows = [json.loads(line) for line in emitted.splitlines()]
        self.assertTrue(any(row['kind'] == 'shadow_calibration' for row in rows))
        self.assertEqual([r['event'] for r in rows if r['kind'] == 'shadow_holdout'],
                         ['BEGIN', 'COMPARED', 'END'])
        a = consume(rows, health_ns=2**64-1)
        self.assertEqual(codes(a), [])
        report = a.report()
        self.assertEqual(report['status'], 'local_checks_pass')
        self.assertEqual(report['shadow_holdout']['position_difference_m'],
                         dict(count=1, min=3.0, max=3.0, mean=3.0))
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
        self.assertEqual(codes(consume([emitted], health_ns=2**64-1)), ['holdout_reference_stale'])
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
