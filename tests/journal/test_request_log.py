"""Production JSON formatter: association is diagnostic, never qualification."""
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('request_audit', ROOT / 'tools/analyze_logs.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class RequestJournal(unittest.TestCase):
    def test_production_records_and_bounds(self):
        command = shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(ROOT / 'build/test_journal')))
        rows = [json.loads(s) for s in subprocess.check_output(command + ['--emit-requests'], text=True).splitlines()]
        self.assertEqual(len(rows), 5)
        p, s, failed, escaped, longest = rows
        self.assertEqual(p['request'], s['request'])
        trace = p['request']
        self.assertEqual(trace['result'], 'observed')
        self.assertTrue(trace['association_only'])
        self.assertEqual((trace['request_id'], trace['worker_id']), (1, 2))
        self.assertEqual((trace['issue_observed_ns'], trace['reply_observed_ns']), (101, 102))
        self.assertEqual(trace['reply_type'], 2)
        self.assertEqual(trace['error']['value'], 'org.freedesktop.DBus.Error.ServiceUnknown')
        self.assertEqual(trace['session_context'], dict(result='observed', basis='unique_live_context',
                         lifetime=8, event=2, state=-7))
        self.assertEqual(s['send_session'], dict(result='observed', basis='send_storage',
                         lifetime=8, event=2, state=-7))
        for key in ('bus_lifetime', 'session_lifetime', 'session_state', 'wire_serial'):
            self.assertIsNone(trace[key])
        self.assertEqual(failed['request']['result'], 'observation_capacity')
        self.assertEqual(failed['request']['request_id'], 0)
        self.assertIsNone(failed['request']['error']['value'])
        self.assertEqual(failed['request']['session_context']['result'], 'unobserved')
        self.assertIsNone(failed['request']['session_context']['lifetime'])
        self.assertEqual(escaped['request']['sender']['value'], 'quote"\\\n\x01\xff')
        self.assertEqual(longest['request']['request_id'], 2**64-1)
        self.assertEqual(longest['request']['session_state'], -2**31)
        self.assertFalse(longest['request']['sender']['complete'])
        self.assertEqual(longest['request']['sender']['value'], '\x01'*63)
        a = audit.Auditor()
        a.consume(p, 'position')
        a.consume(s, 'send')
        self.assertEqual(a.request_results, {'observed': 1})
        self.assertEqual(a.request_errors, {'org.freedesktop.DBus.Error.ServiceUnknown': 1})
        self.assertFalse(any(i['code'].startswith('request_') for i in a.issues))
        a.consume(failed, 'failed')
        self.assertIn('request_observation_failed', [i['code'] for i in a.issues])
        for invalid in (None, {}, dict(trace, association_only=False),
                        dict(trace, request_id=True), dict(trace, wire_serial=0),
                        dict(trace, sender=dict(value=None, complete=True)),
                        dict(trace, sender=dict(complete=False))):
            b = audit.Auditor()
            b.consume(dict(p, request=invalid), 'malformed')
            self.assertIn('request_record_malformed', [i['code'] for i in b.issues])
        for invalid in (None, {}, dict(trace['session_context'], basis='qualified'),
                        dict(trace['session_context'], lifetime=True),
                        dict(trace['session_context'], lifetime=0),
                        dict(trace['session_context'], event=None),
                        dict(trace['session_context'], result='no_live_session')):
            b = audit.Auditor()
            b.consume(dict(p, request=dict(trace, session_context=invalid)), 'malformed-session')
            self.assertIn('request_record_malformed', [i['code'] for i in b.issues])
        b = audit.Auditor()
        b.consume(p, 'position')
        b.consume(dict(s, send_session=dict(s['send_session'], lifetime=9)), 'changed-session')
        self.assertIn('session_changed_since_issue', [i['code'] for i in b.issues])
        b = audit.Auditor()
        b.consume(dict(s, send_session=dict(s['send_session'], basis='unique_live_context')), 'bad-send')
        self.assertIn('send_session_malformed', [i['code'] for i in b.issues])

    def test_session_faults_and_ambiguous_observation(self):
        observer = dict(prepared=True, contexts=2, capacity=64, faults=0)
        for change in ({'faults': 1}, {'prepared': False}, {'contexts': 65}, {'faults': True}):
            with self.subTest(change=change):
                a = audit.Auditor()
                a.consume(dict(kind='health', mono_ns=103, dropped=0, hook_installed=True,
                               assist_ready=False, session_observer=dict(observer, **change)), 'test')
                self.assertTrue(any(i['code'].startswith('session_observer') for i in a.issues))

    def test_observer_health_loss_is_not_clean_evidence(self):
        observer = dict(prepared=True, abi_fault=False, result='observed', loss_epoch=1,
                        requests=0, workers=0, loss_reasons=0, exhausted=False)
        for change in ({'loss_reasons': 1}, {'abi_fault': True}, {'exhausted': True},
                       {'prepared': False}, {'result': 'observation_busy'}, {'loss_reasons': True}):
            with self.subTest(change=change):
                a = audit.Auditor()
                a.consume(dict(kind='health', mono_ns=103, dropped=0, hook_installed=True,
                               assist_ready=False, request_observer=dict(observer, **change)), 'test')
                self.assertTrue(any(i['code'].startswith('request_observer') for i in a.issues))

    def test_old_log_compatibility_and_clean_observer(self):
        for observer in (None, dict(prepared=True, abi_fault=False, result='observed', loss_epoch=1,
                                   requests=4, workers=0, loss_reasons=0, exhausted=False)):
            a = audit.Auditor()
            row = dict(kind='health', mono_ns=103, dropped=0, hook_installed=True, assist_ready=False)
            if observer is not None:
                row['request_observer'] = observer
            a.consume(row, 'test')
            self.assertFalse(any(i['code'].startswith('request_observer') for i in a.issues))


if __name__ == '__main__':
    unittest.main()
