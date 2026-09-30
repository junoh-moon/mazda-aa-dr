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
        records = [{'kind': 'scope', 'physical_phone': False}]
        for cycle in (1, 2):
            records += [
                {'kind': 'create_begin', 'cycle': cycle},
                {'kind': 'create_end', 'cycle': cycle, 'result': 0, 'handle_nonnull': True},
                {'kind': 'send', 'cycle': cycle, 'result': 0},
                {'kind': 'destroy_end', 'cycle': cycle, 'result': 0, 'handle_null': True},
            ]
        return records + [{'kind': 'complete', 'cycles': 2}]

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
        records = [{'kind': 'scope', 'physical_phone': False}]
        for cycle in (1, 2):
            records += [
                {'kind': 'create_begin', 'cycle': cycle},
                {'kind': 'create_end', 'cycle': cycle, 'result': 0, 'handle_nonnull': True},
                {'kind': 'send', 'cycle': cycle, 'result': 0},
                {'kind': 'destroy_end', 'cycle': cycle, 'result': 0, 'handle_null': True},
            ]
        records.append({'kind': 'complete', 'cycles': 2})
        report = self.analyze(records)
        self.assertTrue(report['complete'], report)
        self.assertEqual(report['status_callbacks'], 0)
        self.assertFalse(report['session_state_observed'])
        self.assertFalse(report['phone_acceptance_verified'])

    def test_failed_create_or_incomplete_destroy_is_not_success(self):
        for result, handle_null in ((260, True), (0, False)):
            with self.subTest(result=result, handle_null=handle_null):
                records = [{'kind': 'scope', 'physical_phone': False}]
                for cycle in (1, 2):
                    records += [
                        {'kind': 'create_begin', 'cycle': cycle},
                        {'kind': 'create_end', 'cycle': cycle, 'result': result,
                         'handle_nonnull': result == 0},
                        {'kind': 'send', 'cycle': cycle, 'result': 0},
                        {'kind': 'destroy_end', 'cycle': cycle, 'result': 0,
                         'handle_null': handle_null},
                    ]
                records.append({'kind': 'complete', 'cycles': 2})
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
        callback = {'kind': 'status', 'cycle': 1, 'event': 1, 'state': 0,
                    'data_nonnull': True, 'userdata_unchanged': True}
        returned = {'kind': 'status_return', 'cycle': 1, 'event': 1}
        valid = records[:3] + [callback, returned] + records[3:]
        self.assertTrue(self.analyze(valid)['complete'])
        invalid = records[:3] + [returned, callback] + records[3:]
        self.assertFalse(self.analyze(invalid)['complete'])
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
            {'kind': 'status', 'cycle': 1, 'event': 1, 'state': 0,
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
            for source in (bytes(header[:40]), b'#!/bin/sh\nexit 0\n', Path('/bin/true').read_bytes(),
                           bytes(header[:36]) + struct.pack('<I', 0x05000400) + header[40:]):
                path.write_bytes(source)
                with self.assertRaises(RuntimeError):
                    runner.arm_probe_input(path)


if __name__ == '__main__':
    unittest.main()
