"""The offline report must not turn API submission into a connected phone."""
import importlib.util
from pathlib import Path
import json
import subprocess
import struct
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('oem_runner', HERE / 'oem_system_emulation.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class SessionProbeReport(unittest.TestCase):
    def completed_records(self):
        records = [{'kind': 'scope', 'physical_phone': False, 'oem_queue_started': False,
                    'start_input': 'synthetic_zero_304_bytes'}]
        for cycle in (1, 2):
            records += [
                {'kind': 'create_begin', 'cycle': cycle, 'userdata_null': True},
                {'kind': 'create_end', 'cycle': cycle, 'result': 0, 'handle_nonnull': True},
                {'kind': 'identity', 'cycle': cycle,
                 'same_handle_address_as_previous': False,
                 'same_storage_as_previous': cycle == 2},
                {'kind': 'send', 'cycle': cycle, 'result': 0},
                {'kind': 'start', 'cycle': cycle, 'result': 0},
                {'kind': 'stop', 'cycle': cycle, 'result': 264},
                {'kind': 'destroy_end', 'cycle': cycle, 'result': 0, 'handle_null': True},
            ]
        return records + [{'kind': 'complete', 'cycles': 2, 'status_callbacks': 0}]

    def analyze(self, records):
        self.assertTrue(callable(getattr(runner, 'session_probe_report', None)),
                        'Missing explicit session-probe result validation')
        return runner.session_probe_report(records)

    def test_submission_success_cannot_prove_phone_acceptance(self):
        report = self.analyze([
            {'kind': 'scope', 'physical_phone': False},
            {'kind': 'send', 'cycle': 1, 'result': 0},
        ])
        self.assertFalse(report['complete'])
        self.assertFalse(report['phone_acceptance_verified'])
        self.assertIn('missing_completion', report['failures'])

    def test_a_completion_marker_alone_is_not_sufficient(self):
        report = self.analyze([{'kind': 'complete', 'cycles': 2}])
        self.assertFalse(report['complete'])
        self.assertIn('missing_scope', report['failures'])
        self.assertIn('incomplete_cycles', report['failures'])

    def test_missing_callback_is_unknown_even_after_api_success(self):
        report = self.analyze(self.completed_records())
        self.assertTrue(report['complete'], report)
        self.assertEqual(report['status_callbacks'], 0)
        self.assertFalse(report['session_state_observed'])
        self.assertFalse(report['phone_acceptance_verified'])

    def test_failed_create_or_incomplete_destroy_is_not_success(self):
        for result, handle_null in ((260, True), (0, False)):
            with self.subTest(result=result, handle_null=handle_null):
                records = self.completed_records()
                for record in records:
                    if record['kind'] == 'create_end':
                        record.update(result=result, handle_nonnull=result == 0)
                    elif record['kind'] == 'destroy_end':
                        record['handle_null'] = handle_null
                self.assertFalse(self.analyze(records)['complete'])

    def test_callback_must_return_once_with_unchanged_context(self):
        records = [
            {'kind': 'scope', 'physical_phone': False},
            {'kind': 'status', 'cycle': 1, 'event': 1, 'state': 99,
             'userdata_unchanged': False},
        ]
        report = self.analyze(records)
        self.assertIn('callback_forwarding', report['failures'])
        self.assertFalse(report['complete'])

    def test_completion_before_cycles_and_mixed_runs_are_rejected(self):
        records = self.completed_records()
        for corrupted in ([records[-1]] + records[:-1], records + records):
            self.assertFalse(self.analyze(corrupted)['complete'])

    def test_bad_callback_identity_does_not_crash_the_report(self):
        records = self.completed_records()
        records[1:1] = [
            {'kind': 'status', 'cycle': 1, 'event': 1, 'userdata_unchanged': True},
            {'kind': 'status', 'cycle': 1, 'event': None, 'userdata_unchanged': True},
        ]
        self.assertFalse(self.analyze(records)['complete'])

    def test_callback_return_before_entry_and_wrong_count_are_rejected(self):
        records = self.completed_records()
        callback = {'kind': 'status', 'cycle': 1, 'event': 1, 'state': 0, 'detail': -1,
                    'data_nonnull': True, 'userdata_unchanged': True}
        returned = {'kind': 'status_return', 'cycle': 1, 'event': 1}
        valid = records[:3] + [callback, returned] + records[3:]
        valid[-1] = dict(valid[-1], status_callbacks=1)
        self.assertTrue(self.analyze(valid)['complete'])
        invalid = records[:3] + [returned, callback] + records[3:]
        invalid[-1] = dict(invalid[-1], status_callbacks=1)
        report = self.analyze(invalid)
        self.assertFalse(report['complete'])
        self.assertEqual(report['failures'], ['callback_order'])
        valid[-1] = dict(valid[-1], status_callbacks=2)
        self.assertFalse(self.analyze(valid)['complete'])

    def test_console_requires_successful_probe_exit_and_complete_records(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'console.log'
            body = ''.join('MX5_SESSION ' + json.dumps(r) + '\n'
                           for r in self.completed_records())
            for suffix, expected in (('VM_SESSION_PROBE_RC=0\n', 0), ('', 2),
                                     ('VM_SESSION_PROBE_RC=139\n', 2),
                                     ('MX5_SESSION {broken\nVM_SESSION_PROBE_RC=0\n', 2)):
                path.write_text(body + suffix)
                result = subprocess.run([sys.executable, str(HERE / 'oem_system_emulation.py'),
                                         'check-session', '--console', str(path)],
                                        text=True, capture_output=True)
                self.assertEqual(result.returncode, expected, result.stderr)
                self.assertFalse(json.loads(result.stdout)['phone_acceptance_verified'])

    def test_oem_prefix_on_same_line_does_not_hide_a_complete_callback(self):
        records = self.completed_records()
        records[3:3] = [
            {'kind': 'status', 'cycle': 1, 'event': 1, 'state': 0, 'detail': -1,
             'data_nonnull': True, 'userdata_unchanged': True},
            {'kind': 'status_return', 'cycle': 1, 'event': 1},
        ]
        records[-1]['status_callbacks'] = 1
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'console.log'
            path.write_text(''.join(
                ('ERR::AAP_SDK_IFACE::3219::' if r['kind'] == 'status' else '')
                + 'MX5_SESSION ' + json.dumps(r) + '\n' for r in records)
                + 'VM_SESSION_PROBE_RC=0\n')
            result = subprocess.run([sys.executable, str(HERE / 'oem_system_emulation.py'),
                                     'check-session', '--console', str(path)],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(json.loads(result.stdout)['status_callbacks'], 1)

    def test_probe_exit_must_follow_completion_of_the_same_record_stream(self):
        records = self.completed_records()
        lines = ['MX5_SESSION ' + json.dumps(r) + '\n' for r in records]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'console.log'
            for index in (0, 8, len(lines) - 1, len(lines)):
                with self.subTest(exit_after_records=index):
                    path.write_text(''.join(lines[:index]) + 'VM_SESSION_PROBE_RC=0\n'
                                    + ''.join(lines[index:]))
                    result = subprocess.run(
                        [sys.executable, str(HERE / 'oem_system_emulation.py'),
                         'check-session', '--console', str(path)], text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0 if index == len(lines) else 2,
                                     result.stdout + result.stderr)

    def test_oem_prefix_does_not_hide_the_actual_exit_status(self):
        body = ''.join('MX5_SESSION ' + json.dumps(r) + '\n' for r in self.completed_records())
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'console.log'
            for value, expected in (('0', 0), ('139', 2), ('0 trailing', 2)):
                with self.subTest(value=value):
                    path.write_text(body + 'ERR::OEM_PREFIX::VM_SESSION_PROBE_RC=' + value + '\n')
                    result = subprocess.run(
                        [sys.executable, str(HERE / 'oem_system_emulation.py'),
                         'check-session', '--console', str(path)], text=True, capture_output=True)
                    self.assertEqual(result.returncode, expected, result.stdout + result.stderr)

    def test_null_payload_is_unknown_and_does_not_become_invalid_state(self):
        for state, detail in ((None, None), (0, 0)):
            with self.subTest(legacy_zero_fields=state == 0):
                records = self.completed_records()
                records[3:3] = [
                    {'kind': 'status', 'cycle': 1, 'event': 1, 'state': state,
                     'detail': detail, 'data_nonnull': False, 'userdata_unchanged': True},
                    {'kind': 'status_return', 'cycle': 1, 'event': 1},
                ]
                records[-1]['status_callbacks'] = 1
                report = self.analyze(records)
                self.assertTrue(report['complete'], report)
                self.assertEqual(report['status_callbacks'], 1)
                self.assertFalse(report['session_state_observed'])
                self.assertEqual(report['states'], [None])

    def test_every_operation_and_terminal_callback_count_are_required(self):
        for kind in ('identity', 'start', 'stop'):
            with self.subTest(missing=kind):
                records = [r for r in self.completed_records() if r['kind'] != kind]
                self.assertFalse(self.analyze(records)['complete'])
        records = self.completed_records()
        del records[-1]['status_callbacks']
        self.assertFalse(self.analyze(records)['complete'])

    def test_scope_and_lifecycle_types_are_not_silently_coerced(self):
        changes = [('scope', 'oem_queue_started', True),
                   ('scope', 'start_input', 'real_device'),
                   ('create_begin', 'userdata_null', False),
                   ('create_end', 'result', False), ('send', 'result', '0'),
                   ('start', 'result', None), ('stop', 'result', True),
                   ('identity', 'same_storage_as_previous', 1),
                   ('destroy_end', 'cycle', True), ('complete', 'status_callbacks', False)]
        for kind, field, value in changes:
            with self.subTest(kind=kind, field=field):
                records = self.completed_records()
                next(r for r in records if r['kind'] == kind)[field] = value
                self.assertFalse(self.analyze(records)['complete'])

    def test_global_cycle_order_and_unrecognized_records_are_rejected(self):
        records = self.completed_records()
        reordered = records[:1] + records[8:15] + records[1:8] + records[-1:]
        self.assertFalse(self.analyze(reordered)['complete'])
        for extra in ({'kind': 'start', 'cycle': 3, 'result': 0},
                      {'kind': 'unexpected'}, {'kind': 'status_retun', 'cycle': 1}):
            self.assertFalse(self.analyze(records[:-1] + [extra] + records[-1:])['complete'])

    def test_callback_payload_and_generation_are_checked_without_assuming_state(self):
        callback = {'kind': 'status', 'cycle': 1, 'event': 1, 'state': 99, 'detail': -1,
                    'data_nonnull': True, 'userdata_unchanged': True}
        returned = {'kind': 'status_return', 'cycle': 1, 'event': 1}
        for field, value in (('state', None), ('state', True), ('detail', 'error'),
                             ('data_nonnull', 1), ('event', 3)):
            with self.subTest(field=field):
                records = self.completed_records()
                records[3:3] = [dict(callback, **{field: value}), returned]
                records[-1]['status_callbacks'] = 1
                self.assertFalse(self.analyze(records)['complete'])
        records = self.completed_records()
        records[1:1] = [callback, returned]
        records[-1]['status_callbacks'] = 1
        self.assertFalse(self.analyze(records)['complete'])
        # A late old-generation callback is retained under its original cycle.
        records = self.completed_records()
        records[-1:-1] = [callback, returned]
        records[-1]['status_callbacks'] = 1
        report = self.analyze(records)
        self.assertTrue(report['complete'], report)
        self.assertEqual(report['states'], [99])

    def test_start_and_stop_results_are_reported_without_becoming_phone_success(self):
        records = self.completed_records()
        next(r for r in records if r['kind'] == 'start')['result'] = 265
        report = self.analyze(records)
        self.assertTrue(report['complete'], report)
        self.assertEqual(report['start_results'], [265, 0])
        self.assertEqual(report['stop_results'], [264, 264])
        self.assertFalse(report['phone_acceptance_verified'])

    def test_probe_input_rejects_host_or_truncated_elf(self):
        self.assertTrue(callable(getattr(runner, 'arm_probe_input', None)),
                        'Missing pinned ARM diagnostic input guard')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'probe'
            header = bytearray(52)
            header[:7] = b'\x7fELF\x01\x01\x01'
            struct.pack_into('<HHI', header, 16, 2, 40, 1)
            struct.pack_into('<I', header, 36, 0x05000002)
            struct.pack_into('<H', header, 40, 52)
            path.write_bytes(header)
            self.assertEqual(runner.arm_probe_input(path), path.resolve())
            for source in (bytes(header[:40]), b'#!/bin/sh\nexit 0\n', Path(sys.executable).read_bytes(),
                           bytes(header[:36]) + struct.pack('<I', 0x05000400) + header[40:]):
                path.write_bytes(source)
                with self.assertRaises(RuntimeError):
                    runner.arm_probe_input(path)


if __name__ == '__main__':
    unittest.main()
