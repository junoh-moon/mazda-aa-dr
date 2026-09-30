"""MODEL result contradictions must not become passing numerical evidence."""
import json
import math
import os
from pathlib import Path
import shlex
import subprocess
import unittest

from test_calibration_logs import consume
from test_motion_logs import shadow, valid_shadow


def report(row):
    return consume([row], health_ns=2**64-1).report()


class ShadowResults(unittest.TestCase):
    def producer_cases(self):
        root = Path(__file__).resolve().parents[2]
        command = shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(root / 'build/test_journal')))
        rows = [json.loads(line) for line in subprocess.check_output(
            command + ['--emit-model-results'], text=True).splitlines()]
        cases = {row['case']: row for row in rows}
        self.assertEqual(len(rows), 18)
        self.assertEqual(len(cases), len(rows))
        return cases

    def test_actual_pipeline_snapshots_pass_in_complete_legacy_envelope(self):
        cases = self.producer_cases()
        expected = {'unseeded', 'active_valid', 'active_stale', 'active_time_error',
                    'active_valid_waiting', 'active_queued_gps', 'active_queued_native',
                    'native', 'active_near_zero_not_stopped', 'active_stopped'}
        rows = {name: row for name, row in cases.items() if row['kind'] == 'shadow'}
        self.assertEqual(set(rows), expected)
        for name, row in rows.items():
            with self.subTest(case=name):
                self.assertEqual(report(row)['status'], 'local_checks_pass')
        self.assertTrue(rows['active_valid']['model_valid'])
        self.assertEqual(rows['active_valid_waiting']['pipeline'], 'WAITING')
        self.assertTrue(rows['active_valid_waiting']['model_valid'])
        for name, state in (('unseeded', 0), ('native', 4)):
            self.assertFalse(rows[name]['model_valid'])
            self.assertEqual(rows[name]['state'], state)
            self.assertEqual(rows[name]['result'], 'E_NO_SEED')
        for name, result in (('active_stale', 'E_STALE'), ('active_time_error', 'E_TIME'),
                             ('active_queued_gps', 'E_NO_SEED'), ('active_queued_native', 'E_NO_SEED')):
            self.assertEqual(rows[name]['state'], 2)
            self.assertEqual(rows[name]['result'], result)
            self.assertFalse(rows[name]['model_valid'])
        # GNU may produce exact zero, while Clang can retain a tiny residual.
        near_zero = rows['active_near_zero_not_stopped']
        self.assertTrue(near_zero['model_valid'])
        self.assertFalse(near_zero['stopped'])
        self.assertLess(abs(near_zero['speed_mps']), 1e-12)
        self.assertTrue(rows['active_stopped']['model_valid'])
        self.assertTrue(rows['active_stopped']['stopped'])
        self.assertEqual(rows['active_stopped']['speed_mps'], 0)

    def test_actual_core_boundaries_with_explicit_authored_log_metadata(self):
        cases = self.producer_cases()
        rows = {name: row for name, row in cases.items() if row['kind'] == 'test_core_snapshot'}
        self.assertEqual(set(rows), {'zero_not_stopped', 'heading_2pi', 'longitude_180',
                                    'error_limit', 'duration_limit', 'exact_age_limit',
                                    'over_age_limit', 'custom_speed'})
        # These are actual core queries, not Pipeline/worker runs. Only their
        # query/snapshot/preview fields enter an explicitly authored envelope;
        # events and intervals below do not claim observed worker activity.
        fields = ('mono_ns', 'model_valid', 'state', 'result', 'frontier_ns', 'lat', 'lon',
                  'heading_rad', 'speed_mps', 'error_model_m', 'stopped',
                  'preview_encoded', 'location_preview_hex')
        for name, row in rows.items():
            with self.subTest(case=name):
                envelope = valid_shadow(**{key: row[key] for key in fields})
                self.assertEqual(report(envelope)['status'], 'local_checks_pass')
                self.assertEqual(row['model_valid'], name not in (
                    'error_limit', 'duration_limit', 'over_age_limit'))
        self.assertEqual(rows['heading_2pi']['heading_rad'], 2*math.pi)
        self.assertEqual(rows['longitude_180']['lon'], 180)
        self.assertTrue(rows['zero_not_stopped']['model_valid'])
        self.assertFalse(rows['zero_not_stopped']['stopped'])
        self.assertFalse(rows['zero_not_stopped']['preview_encoded'])
        self.assertEqual(rows['error_limit']['result'], 'E_LIMIT')
        self.assertGreater(rows['error_limit']['error_model_m'], 100)
        self.assertEqual(rows['duration_limit']['result'], 'E_LIMIT')
        self.assertLess(rows['duration_limit']['error_model_m'], 100)
        self.assertTrue(rows['exact_age_limit']['model_valid'])
        self.assertEqual(rows['over_age_limit']['result'], 'E_STALE')
        self.assertEqual(rows['exact_age_limit']['mono_ns'] - rows['exact_age_limit']['frontier_ns'], 150000000)
        self.assertEqual(rows['over_age_limit']['mono_ns'] - rows['over_age_limit']['frontier_ns'], 150000001)
        self.assertTrue(rows['custom_speed']['model_valid'])
        self.assertGreater(rows['custom_speed']['speed_mps'], 100)

    def assert_contradiction(self, row):
        result = report(row)
        found = [i for i in result['issues'] if i['code'] == 'shadow_result_inconsistent']
        self.assertTrue(found, result)
        self.assertTrue(all(i['severity'] == 'violation' for i in found))
        self.assertNotEqual(result['status'], 'local_checks_pass')

    def test_original_false_positive_is_rejected(self):
        row = shadow()
        row.update(model_valid=True, mono_ns=200, frontier_ns=150)
        self.assert_contradiction(row)

    def test_active_state_and_ok_result_are_necessary_for_valid_model(self):
        self.assertEqual(report(valid_shadow())['status'], 'local_checks_pass')
        for state in (0, 1, 3, 4, 5, 6):
            with self.subTest(state=state):
                self.assert_contradiction(valid_shadow(state=state))
        for result in ('E_CONFIG', 'E_NO_SEED', 'E_CONTEXT', 'E_QUALITY',
                       'E_TIME', 'E_LIMIT', 'E_STALE'):
            with self.subTest(result=result):
                self.assert_contradiction(valid_shadow(result=result))
        self.assert_contradiction(valid_shadow(model_valid=False))

    def test_invalid_active_preserves_its_values(self):
        for result in ('E_STALE', 'E_TIME', 'E_LIMIT', 'E_NO_SEED'):
            with self.subTest(result=result):
                row = valid_shadow(model_valid=False, result=result)
                if result == 'E_TIME': row['frontier_ns'] = row['mono_ns'] + 1
                if result == 'E_LIMIT': row['error_model_m'] = 101
                self.assertEqual(report(row)['status'], 'local_checks_pass')
        for state in range(7):
            row = shadow(); row.update(state=state)
            self.assertEqual(report(row)['status'], 'local_checks_pass')

    def test_query_error_requires_the_producer_state(self):
        for result in ('E_TIME', 'E_STALE', 'E_LIMIT'):
            for state in (0, 1, 3, 4, 5, 6):
                with self.subTest(result=result, state=state):
                    self.assert_contradiction(valid_shadow(model_valid=False, result=result, state=state))
            self.assert_contradiction(valid_shadow(model_valid=False, result=result, frontier_ns=0))
        self.assert_contradiction(valid_shadow(model_valid=False, result='E_CONFIG'))
        self.assert_contradiction(valid_shadow(model_valid=False, result='E_STALE', frontier_ns=201))
        row = shadow(); row.update(result='E_CONFIG')
        self.assertEqual(report(row)['status'], 'local_checks_pass')

    def test_valid_requires_real_bounded_numerical_output(self):
        for field in ('lat', 'lon', 'heading_rad', 'speed_mps', 'error_model_m'):
            with self.subTest(null_field=field):
                self.assert_contradiction(valid_shadow(**{field: None}))
        for field, value in (('lat', 85), ('lat', -85), ('lon', 181), ('lon', -181),
                             ('heading_rad', -0.01), ('heading_rad', math.nextafter(2*math.pi, math.inf)),
                             ('speed_mps', -0.01), ('error_model_m', -0.01), ('error_model_m', 101)):
            with self.subTest(field=field, value=value):
                self.assert_contradiction(valid_shadow(**{field: value}))

    def test_numeric_limits_preserve_actual_rounding_and_stop_contract(self):
        # wrap(-tiny) may round to precisely 2*pi after adding a full turn.
        for fields in (dict(heading_rad=2*math.pi), dict(lon=180), dict(lon=-180),
                       dict(lat=math.nextafter(85, 0)), dict(lat=math.nextafter(-85, 0)),
                       dict(error_model_m=100), dict(error_model_m=0),
                       dict(speed_mps=1000), dict(speed_mps=0, stopped=False),
                       dict(speed_mps=0, stopped=True), dict(speed_mps=-0.0, stopped=True)):
            with self.subTest(fields=fields):
                self.assertEqual(report(valid_shadow(**fields))['status'], 'local_checks_pass')
        self.assert_contradiction(valid_shadow(stopped=True, speed_mps=1))

    def test_valid_frontier_must_exist_and_not_follow_query(self):
        for frontier in (0, 201):
            self.assert_contradiction(valid_shadow(frontier_ns=frontier))
        self.assertEqual(report(valid_shadow(frontier_ns=200))['status'], 'local_checks_pass')
        for field in ('events', 'intervals'):
            self.assert_contradiction(valid_shadow(**{field: 0}))
        self.assertEqual(report(valid_shadow(mono_ns=150000150))['status'], 'local_checks_pass')
        self.assert_contradiction(valid_shadow(mono_ns=150000151))
        self.assert_contradiction(valid_shadow(model_valid=False, result='E_LIMIT', mono_ns=150000151))

    def test_pipeline_status_and_optional_preview_are_separate_from_validity(self):
        for pipeline in ('OK', 'WAITING', 'MISSING_SENSOR', 'NO_ANCHOR'):
            row = valid_shadow(pipeline=pipeline, preview_encoded=False)
            self.assertEqual(report(row)['status'], 'local_checks_pass')
        row = valid_shadow(preview_encoded=True, location_preview_hex='00'*48)
        self.assertEqual(report(row)['status'], 'local_checks_pass')
        row.update(model_valid=False, result='E_STALE')
        self.assertIn('invalid_shadow_preview', [i['code'] for i in report(row)['issues']])

    def test_unknown_status_is_not_normal_recovery(self):
        for field, values in (('result', ('', 'OKAY', 'E_UNKNOWN', 'DUPLICATE')),
                              ('pipeline', ('', 'SUCCESS', 'UNKNOWN'))):
            for value in values:
                with self.subTest(field=field, value=value):
                    row = shadow(); row[field] = value
                    self.assertIn('partial_record', [i['code'] for i in report(row)['issues']])

    def test_malformed_numeric_shape_is_not_silently_counted(self):
        for field in ('lat', 'lon', 'heading_rad', 'speed_mps', 'error_model_m'):
            for value in (True, [], {}, '0', float('nan'), float('inf'), -float('inf'), 10**400):
                with self.subTest(field=field, value=repr(value)):
                    row = shadow(); row[field] = value
                    self.assertIn('partial_record', [i['code'] for i in report(row)['issues']])
            row = shadow(); del row[field]
            self.assertIn('partial_record', [i['code'] for i in report(row)['issues']])


if __name__ == '__main__':
    unittest.main()
