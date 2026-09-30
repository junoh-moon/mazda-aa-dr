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
    def test_bus_lifetimes_and_health(self):
        command = shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(ROOT / 'build/test_journal')))
        p = json.loads(subprocess.check_output(command + ['--emit-requests'], text=True).splitlines()[0])
        connected = dict(result='connected', object=1, lifetime=7)
        trace = dict(p['request'], bus_lifetime=7, issue_connection=connected, reply_connection=connected)
        a = audit.Auditor()
        a.consume(dict(p, request=trace), 'same-bus')
        self.assertFalse(any(i['code'].startswith('bus_') for i in a.issues))
        for snapshot in (dict(connected, object=2, lifetime=8), dict(connected, lifetime=8)):
            a = audit.Auditor()
            a.consume(dict(p, request=dict(trace, reply_connection=snapshot)), 'changed-bus')
            self.assertIn('bus_changed_since_issue', [i['code'] for i in a.issues])
        for result in ('unobserved', 'transition', 'observation_fault', 'disconnected'):
            snapshot = dict(result=result, object=1 if result == 'disconnected' else None, lifetime=None)
            a = audit.Auditor()
            a.consume(dict(p, request=dict(trace, reply_connection=snapshot)), result)
            self.assertIn('bus_observation_unavailable', [i['code'] for i in a.issues])
        for snapshot in (None, {}, dict(connected, lifetime=0), dict(connected, object=True),
                         dict(connected, result='unobserved'), dict(connected, lifetime=2**64)):
            a = audit.Auditor()
            a.consume(dict(p, request=dict(trace, reply_connection=snapshot)), 'malformed')
            self.assertIn('request_record_malformed', [i['code'] for i in a.issues])
        for change in ({'bus_lifetime': None}, {'bus_lifetime': 8}):
            a = audit.Auditor()
            a.consume(dict(p, request=dict(trace, **change)), 'contradictory-issue')
            self.assertIn('request_record_malformed', [i['code'] for i in a.issues])
        old = dict(trace)
        del old['issue_connection'], old['reply_connection']
        a = audit.Auditor()
        a.consume(dict(p, request=old), 'older-schema')
        self.assertFalse(any(i['code'] in ('request_record_malformed', 'bus_changed_since_issue') for i in a.issues))
        observer = dict(prepared=True, contexts=2, capacity=64, faults=0)
        for change in ({'faults': 1}, {'prepared': False}, {'contexts': 65}, {'faults': True}):
            a = audit.Auditor()
            a.consume(dict(kind='health', mono_ns=103, dropped=0, hook_installed=True,
                           assist_ready=False, bus_observer=dict(observer, **change)), 'health')
            self.assertTrue(any(i['code'].startswith('bus_observer') for i in a.issues))

    def test_same_lifetime_callback_history(self):
        command = shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(ROOT / 'build/test_journal')))
        rows = [json.loads(s) for s in subprocess.check_output(command + ['--emit-requests'], text=True).splitlines()]
        send = rows[1]
        for event, state in ((1, -7), (2, 3), (None, None)):
            for revision in (12, 13):
                with self.subTest(event=event, state=state, revision=revision):
                    a = audit.Auditor()
                    a.consume(dict(send, send_session=dict(send['send_session'], event=event,
                                   state=state, revision=revision)), 'contradiction')
                    found = [i for i in a.issues if i['code'] == 'session_state_inconsistent']
                    self.assertEqual(len(found), 1)
                    self.assertEqual(found[0]['severity'], 'violation')
        # A later callback may have any raw state, including the same value.
        for event, state in ((2, -7), (3, -7), (3, 3)):
            a = audit.Auditor()
            a.consume(dict(send, send_session=dict(send['send_session'], event=event,
                           state=state, revision=12+event-2)), 'valid')
            self.assertFalse(any(i['code'].startswith('session_') and
                                 i['code'] != 'session_changed_since_issue' for i in a.issues))
        unknown = dict(send['request']['session_context'], event=None, state=None)
        for event, state in ((None, None), (1, 0)):
            a = audit.Auditor()
            a.consume(dict(send, request=dict(send['request'], session_context=unknown),
                           send_session=dict(send['send_session'], event=event, state=state,
                                             revision=12+(event or 0))), 'first-status')
            self.assertFalse(any(i['code'].startswith('session_') and
                                 i['code'] != 'session_changed_since_issue' for i in a.issues))

    def test_revision_cannot_go_backward_or_hide_completed_callbacks(self):
        command = shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(ROOT / 'build/test_journal')))
        send = json.loads(subprocess.check_output(command + ['--emit-requests'], text=True).splitlines()[1])
        for changes in ({'revision': 11}, {'event': 3}, {'event': 4, 'revision': 13},
                        {'lifetime': 9, 'revision': 11}):
            with self.subTest(changes=changes):
                a = audit.Auditor()
                a.consume(dict(send, send_session=dict(send['send_session'], **changes)), 'contradiction')
                found = [i for i in a.issues if i['code'] == 'session_revision_inconsistent']
                self.assertEqual(len(found), 1)
                self.assertEqual(found[0]['severity'], 'violation')
        unknown = dict(send['request']['session_context'], event=None, state=None)
        a = audit.Auditor()
        a.consume(dict(send, request=dict(send['request'], session_context=unknown),
                       send_session=dict(send['send_session'], event=1)), 'hidden-first-callback')
        self.assertIn('session_revision_inconsistent', [i['code'] for i in a.issues])
        for changes in ({}, {'event': 3, 'revision': 13}, {'revision': 13},
                        {'event': 5, 'revision': 15}):
            a = audit.Auditor()
            a.consume(dict(send, send_session=dict(send['send_session'], **changes)), 'valid')
            self.assertFalse(any(i['severity'] == 'violation' for i in a.issues))
        # Revision-free historical records retain their prior event contract.
        legacy_issue = dict(send['request']['session_context'])
        legacy_send = dict(send['send_session'], event=3)
        del legacy_issue['revision'], legacy_send['revision']
        a = audit.Auditor()
        a.consume(dict(send, request=dict(send['request'], session_context=legacy_issue),
                       send_session=legacy_send), 'legacy')
        self.assertFalse(any(i['code'].startswith('session_') for i in a.issues))
        for missing_issue in (False, True):
            a = audit.Auditor()
            issue = legacy_issue if missing_issue else send['request']['session_context']
            target = send['send_session'] if missing_issue else legacy_send
            a.consume(dict(send, request=dict(send['request'], session_context=issue),
                           send_session=target), 'partial-schema')
            found = [i for i in a.issues if i['code'] == 'session_revision_partial']
            self.assertEqual(len(found), 1)
            self.assertEqual(found[0]['severity'], 'inconclusive')

    def test_route_schema_and_old_records(self):
        command = shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(ROOT / 'build/test_journal')))
        p = json.loads(subprocess.check_output(command + ['--emit-requests'], text=True).splitlines()[0])
        route = {key: dict(value=value, complete=True) for key, value in
                 zip(('destination', 'path', 'interface', 'member'),
                     ('com.jci.lds.data', '/com/jci/lds/data', 'com.jci.lds.data', 'GetPosition'))}
        for value in (route, {k: dict(value=None, complete=False) for k in route}):
            a = audit.Auditor()
            a.consume(dict(p, request=dict(p['request'], route=value)), 'route')
            self.assertNotIn('request_record_malformed', [i['code'] for i in a.issues])
        old = dict(p['request'])
        old.pop('route', None)
        a = audit.Auditor()
        a.consume(dict(p, request=old), 'old')
        self.assertNotIn('request_record_malformed', [i['code'] for i in a.issues])
        for bad in (None, {}, dict(route, member='GetPosition'),
                    dict(route, path=dict(value=None, complete=True)),
                    dict(route, destination=dict(value='x' * 64, complete=True))):
            with self.subTest(route=bad):
                a = audit.Auditor()
                a.consume(dict(p, request=dict(p['request'], route=bad)), 'bad-route')
                self.assertIn('request_record_malformed', [i['code'] for i in a.issues])

    def test_text_fields_match_the_bytewise_encoder(self):
        command = shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(ROOT / 'build/test_journal')))
        p = json.loads(subprocess.check_output(command + ['--emit-requests'], text=True).splitlines()[0])
        for field in ('destination', 'path', 'interface', 'member', 'sender', 'error'):
            for value, complete, valid in (
                    ('\0', True, False), ('a\0b', False, False),
                    ('\u0100', True, False), ('\ud800', False, False),
                    ('\U0001f680', True, False),
                    ('\x01\xff', True, True), ('\xff' * 63, True, True),
                    ('\xff' * 64, False, True), ('\xff' * 64, True, False)):
                with self.subTest(field=field, value=repr(value), complete=complete):
                    trace = dict(p['request'], route=dict(p['request']['route']))
                    target = trace['route'] if field in trace['route'] else trace
                    target[field] = dict(value=value, complete=complete)
                    a = audit.Auditor()
                    a.consume(dict(p, request=trace), 'byte-contract')
                    malformed = 'request_record_malformed' in [i['code'] for i in a.issues]
                    self.assertEqual(malformed, not valid)

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
        self.assertEqual(trace['issue_connection'], dict(result='unobserved', object=None, lifetime=None))
        self.assertEqual(trace['reply_connection'], trace['issue_connection'])
        self.assertEqual(trace['reply_type'], 2)
        self.assertEqual(trace['error']['value'], 'org.freedesktop.DBus.Error.ServiceUnknown')
        self.assertEqual(trace['route'], {k: dict(value=v, complete=True) for k, v in
                         zip(('destination', 'path', 'interface', 'member'),
                             ('com.jci.lds.data', '/com/jci/lds/data', 'com.jci.lds.data', 'GetPosition'))})
        self.assertEqual(trace['session_context'], dict(result='observed', basis='unique_live_context',
                         lifetime=8, event=2, state=-7, revision=12))
        self.assertEqual(s['send_session'], dict(result='observed', basis='send_storage',
                         lifetime=8, event=2, state=-7, revision=12))
        for key in ('bus_lifetime', 'session_lifetime', 'session_state', 'wire_serial'):
            self.assertIsNone(trace[key])
        self.assertEqual(failed['request']['result'], 'observation_capacity')
        self.assertEqual(failed['request']['request_id'], 0)
        self.assertIsNone(failed['request']['error']['value'])
        self.assertTrue(all(v == dict(value=None, complete=False) for v in failed['request']['route'].values()))
        self.assertEqual(failed['request']['session_context']['result'], 'unobserved')
        self.assertIsNone(failed['request']['session_context']['lifetime'])
        self.assertEqual(escaped['request']['sender']['value'], 'quote"\\\n\x01\xff')
        self.assertEqual(longest['request']['issue_connection'], dict(result='connected', object=2**32-1, lifetime=2**64-1))
        self.assertEqual(longest['request']['reply_connection'], longest['request']['issue_connection'])
        self.assertEqual(longest['request']['request_id'], 2**64-1)
        self.assertEqual(longest['request']['session_state'], -2**31)
        self.assertEqual(longest['request']['session_context']['revision'], 2**64-1)
        self.assertFalse(longest['request']['sender']['complete'])
        self.assertEqual(longest['request']['sender']['value'], '\x01'*63)
        self.assertTrue(all(v == dict(value='\x01'*63, complete=False) for v in longest['request']['route'].values()))
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
        b = audit.Auditor()
        b.consume(dict(failed, request=dict(failed['request'], route=trace['route'])), 'stale-route')
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
