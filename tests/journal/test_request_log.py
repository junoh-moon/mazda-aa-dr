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
    @staticmethod
    def position_rows():
        command = shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(ROOT / 'build/test_journal')))
        return [json.loads(s) for s in subprocess.check_output(command + ['--emit-positions'], text=True).splitlines()]

    def test_position_formatter_preserves_partial_cache_updates(self):
        rows = self.position_rows()
        self.assertEqual(len(rows), 8)
        expected = dict(mode=1, utc_s=1790856000, lat=35, lon=135, altitude_m=0,
                        heading=20, kmh=18, horizontal=0, vertical=0)
        updates = ({}, dict(horizontal=1, vertical=1.5), dict(altitude_m=12),
                   dict(lat=36, lon=136, altitude_m=24),
                   dict(utc_s=1790856001, heading=40, kmh=37))
        for stage, update in enumerate(updates):
            with self.subTest(stage=stage):
                expected.update(update)
                row = rows[stage]
                self.assertEqual({key: row.get(key) for key in expected}, expected)
                self.assertEqual((row['call'], row['generation'], row['mono_ns']), (17+stage, 4, 103+stage))
                # Preserve the unqualified request state; values do not prove
                # simultaneous production, freshness, or a verified source.
                self.assertEqual(row['request'], rows[0]['request'])
                auditor = audit.Auditor()
                auditor.consume(row, 'full-position')
                self.assertNotIn('partial_record', [i['code'] for i in auditor.issues])
                legacy = {key: value for key, value in row.items()
                          if key not in ('altitude_m', 'horizontal', 'vertical')}
                older = audit.Auditor()
                older.consume(legacy, 'legacy-position')
                self.assertNotIn('partial_record', [i['code'] for i in older.issues])

    def test_position_formatter_keeps_quality_zero_and_nonfinite_distinct(self):
        rows = self.position_rows()
        for index, expected in ((0, (0, 0)), (5, (None, None)), (6, (None, 0)), (7, (-0.25, 0.5))):
            with self.subTest(index=index):
                self.assertIn('horizontal', rows[index])
                self.assertIn('vertical', rows[index])
                self.assertEqual((rows[index]['horizontal'], rows[index]['vertical']), expected)

    def test_position_formatter_preserves_signed_altitude(self):
        rows = self.position_rows()
        for index, altitude in ((0, 0), (2, 12), (3, 24), (5, -(2**31)), (6, 2**31-1), (7, -12)):
            with self.subTest(index=index):
                self.assertEqual(rows[index].get('altitude_m'), altitude)
                self.assertIs(type(rows[index]['altitude_m']), int)

    def test_context_pool_loss_keeps_raw_position_inconclusive(self):
        position, _ = self.bus_rows()
        lost = dict(position, reason=13)
        auditor = audit.Auditor()
        auditor.consume(lost, 'pool-loss')
        self.assertEqual(auditor.report()['status'], 'inconclusive')
        self.assertIn('adapter_context_unavailable',
                      {issue['code'] for issue in auditor.issues})
        # Even an otherwise complete wire identity must not admit a lost
        # POSITION as a successful source association.
        self.assertFalse(auditor.lds.entries)
        self.assertEqual(self.position_rows()[0]['reason'], 0)

    def test_context_pool_failure_marker_and_send_are_inconclusive(self):
        rows = [json.loads(s) for s in subprocess.check_output(
            shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(ROOT / 'build/test_journal')))
            + ['--emit-requests'], text=True).splitlines()]
        auditor = audit.Auditor()
        auditor.consume(dict(rows[1], reason=13), 'pool-send')
        auditor.consume(dict(kind='capture_incomplete', reason='adapter_context_unavailable',
                             assist_ready=False), 'pool-marker')
        self.assertEqual(auditor.reasons['CONTEXT_UNAVAILABLE'], 1)
        self.assertEqual(auditor.report()['status'], 'inconclusive')
        self.assertTrue({'adapter_context_unavailable', 'capture_incomplete'}.issubset(
            {issue['code'] for issue in auditor.issues}))

    def test_malformed_send_after_context_loss_is_not_a_copy_violation(self):
        rows = [json.loads(s) for s in subprocess.check_output(
            shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(ROOT / 'build/test_journal')))
            + ['--emit-requests'], text=True).splitlines()]
        for reason in (4, 13):  # The older bad-length row and the corrected row.
            with self.subTest(reason=reason):
                auditor = audit.Auditor()
                auditor.consume(dict(rows[0], reason=13), 'lost-position')
                auditor.consume(dict(rows[2], reason=reason, length=47), 'short-send')
                self.assertNotIn('request_copy_mismatch',
                                 {issue['code'] for issue in auditor.issues})
                self.assertEqual(auditor.report()['status'], 'inconclusive')

    def test_failed_position_cannot_authorize_pass_or_mutation(self):
        rows = [json.loads(s) for s in subprocess.check_output(
            shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(ROOT / 'build/test_journal')))
            + ['--emit-requests'], text=True).splitlines()]
        for choice, reason in ((0, 0), (1, 13)):
            with self.subTest(choice=choice, reason=reason):
                auditor = audit.Auditor()
                auditor.consume(dict(rows[0], reason=13), 'lost-position')
                auditor.consume(dict(rows[1], choice=choice, reason=reason), 'impossible-send')
                self.assertIn('context_unavailable_send_inconsistent',
                              {issue['code'] for issue in auditor.issues})
                self.assertEqual(auditor.report()['status'], 'violation')

    def test_extra_location_fault_is_inconclusive(self):
        rows = [json.loads(s) for s in subprocess.check_output(
            shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(ROOT / 'build/test_journal')))
            + ['--emit-requests'], text=True).splitlines()]
        auditor = audit.Auditor()
        auditor.consume(rows[0], 'position')
        auditor.consume(dict(rows[1], reason=3), 'second-location')
        self.assertIn('extra_location', {issue['code'] for issue in auditor.issues})
        self.assertEqual(auditor.report()['status'], 'inconclusive')

    @staticmethod
    def wire_record():
        return dict(issue=dict(known=True, observed_ns=101, serial=23, conflict=False, endpoint_matched=False),
                    reply=dict(known=True, observed_ns=102, serial=41, reply_serial=23, type=3,
                               sender=dict(value=':1.42', complete=True),
                               error=dict(value='org.freedesktop.DBus.Error.ServiceUnknown', complete=True)))

    @staticmethod
    def unknown_wire():
        return dict(issue=dict(known=False, observed_ns=None, serial=None, conflict=False, endpoint_matched=False),
                    reply=dict(known=False, observed_ns=None, serial=None, reply_serial=None, type=None,
                               sender=dict(value=None, complete=False), error=dict(value=None, complete=False)))

    def test_wire_formatter_and_legacy_compatibility(self):
        command = shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(ROOT / 'build/test_journal')))
        rows = [json.loads(s) for s in subprocess.check_output(command + ['--emit-requests'], text=True).splitlines()]
        self.assertEqual(len(rows), 6)
        p, send, failed, escaped, longest, local = rows
        self.assertEqual(p['request'].get('wire'), self.wire_record())
        self.assertEqual(send['request']['wire'], p['request']['wire'])
        self.assertEqual(failed['request']['wire'], self.unknown_wire())
        unknown_endpoint = dict(server_guid=dict(value=None, complete=False), unique_name=dict(value=None, complete=False))
        self.assertEqual(p['request']['endpoint'], unknown_endpoint)
        self.assertEqual(failed['request']['endpoint'], unknown_endpoint)
        self.assertEqual(longest['request']['endpoint'], dict(server_guid=dict(value='\x01'*64, complete=False),
                                                               unique_name=dict(value='\x01'*64, complete=False)))
        self.assertEqual(escaped['request']['wire']['reply']['sender']['value'], 'quote"\\\n\x01\xff')
        self.assertEqual(longest['request']['wire']['issue']['serial'], 2**32-1)
        self.assertEqual(longest['request']['wire']['reply']['reply_serial'], 2**32-1)
        self.assertEqual(longest['request']['wire']['reply']['sender'], dict(value='\x01'*64, complete=False))
        self.assertEqual(longest['request']['wire']['reply']['error'], dict(value='\x01'*64, complete=False))
        expected_local = self.wire_record()
        expected_local['issue']['observed_ns'] = None
        expected_local['reply'].update(observed_ns=None, serial=0,
                                       sender=dict(value=None, complete=False),
                                       error=dict(value='org.freedesktop.DBus.Error.NoReply', complete=True))
        self.assertEqual(local['request']['wire'], expected_local)
        self.assertIsNone(local['request']['reply_type'])
        self.assertIsNone(local['request']['wire_serial'])
        self.assertIsNone(local['request']['sender']['value'])
        for trace in (p['request'], {k: v for k, v in p['request'].items() if k != 'wire'}):
            a = audit.Auditor()
            a.consume(dict(p, request=trace), 'old-or-new-wire')
            self.assertNotIn('request_record_malformed', [i['code'] for i in a.issues])

    def test_wire_optional_schema_contract(self):
        p, _ = self.bus_rows()
        wire = self.wire_record()
        valid = [wire, self.unknown_wire(),
                 dict(wire, issue=dict(wire['issue'], observed_ns=None),
                      reply=dict(wire['reply'], observed_ns=None, serial=0, reply_serial=0,
                                 sender=dict(value=None, complete=False)))]
        for value in valid:
            a = audit.Auditor()
            a.consume(dict(p, request=dict(p['request'], wire=value)), 'valid-wire')
            self.assertNotIn('request_record_malformed', [i['code'] for i in a.issues])
        invalid = [None, {}, dict(issue=wire['issue']),
                   dict(wire, issue=dict(wire['issue'], known=1)),
                   dict(wire, issue=dict(wire['issue'], serial=0)),
                   dict(wire, issue=dict(wire['issue'], conflict=1)),
                   dict(wire, issue=dict(wire['issue'], known=False)),
                   dict(wire, reply=dict(wire['reply'], known=False)),
                   dict(wire, reply=dict(wire['reply'], serial=True)),
                   dict(wire, reply=dict(wire['reply'], reply_serial=2**32)),
                   dict(wire, reply=dict(wire['reply'], type=0)),
                   dict(wire, reply=dict(wire['reply'], observed_ns=True)),
                   dict(wire, reply=dict(wire['reply'], sender=dict(value=None, complete=True)))]
        for value in invalid:
            with self.subTest(wire=value):
                a = audit.Auditor()
                a.consume(dict(p, request=dict(p['request'], wire=value)), 'invalid-wire')
                self.assertIn('request_record_malformed', [i['code'] for i in a.issues])

    def test_wire_diagnostics_do_not_qualify_provenance(self):
        p, _ = self.bus_rows()
        wire = self.wire_record()
        for value, code in ((dict(wire, issue=dict(wire['issue'], conflict=True)), 'request_wire_issue_conflict'),
                            (dict(wire, reply=dict(wire['reply'], reply_serial=24)), 'request_wire_reply_mismatch')):
            a = audit.Auditor()
            a.consume(dict(p, request=dict(p['request'], wire=value)), 'wire-contradiction')
            self.assertIn(code, [i['code'] for i in a.issues])
        local = dict(wire, reply=dict(wire['reply'], serial=0, reply_serial=0,
                                     sender=dict(value=None, complete=False),
                                     error=dict(value='org.freedesktop.DBus.Error.NoReply', complete=True)))
        a = audit.Auditor()
        a.consume(dict(p, request=dict(p['request'], wire=local)), 'local-error')
        self.assertNotIn('request_wire_reply_mismatch', [i['code'] for i in a.issues])
        summary = a.report()['request_observation']
        self.assertEqual(summary['qualification'], 'not_established')
        self.assertEqual(summary['wire_headers']['reply_known'], 1)
        self.assertEqual(summary['wire_headers']['reply_without_remote_serial'], 1)
        self.assertEqual(summary['wire_complete_error_names'], {'org.freedesktop.DBus.Error.NoReply': 1})

    @staticmethod
    def bus_rows():
        command = shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(ROOT / 'build/test_journal')))
        p = json.loads(subprocess.check_output(command + ['--emit-requests'], text=True).splitlines()[0])
        connected = dict(result='connected', object=1, lifetime=7)
        p = dict(p, mono_ns=20, request=dict(p['request'], bus_lifetime=7,
                 issue_observed_ns=15, reply_observed_ns=19,
                 issue_connection=connected, reply_connection=connected))
        h = dict(kind='health', mono_ns=40, dropped=0, hook_installed=True,
                 assist_ready=False, bus_observer=dict(prepared=True, contexts=1, capacity=64, faults=0))
        return p, h

    def test_bus_history_contradictions(self):
        p, h = self.bus_rows()
        def pair(obj, lifetime):
            snapshot = dict(result='connected', object=obj, lifetime=lifetime)
            return dict(p, call=obj, request=dict(p['request'], bus_lifetime=lifetime,
                        issue_connection=snapshot, reply_connection=snapshot))
        cases = [
            ([p, pair(2, 7)], 'bus_lifetime_owner_changed'),
            ([p, dict(h, bus_observer=dict(h['bus_observer'], contexts=0))], 'bus_object_outside_contexts'),
            ([pair(65, 8), dict(h, bus_observer=dict(h['bus_observer'], contexts=64))], 'bus_object_outside_capacity'),
            ([h, pair(65, 8)], 'bus_object_outside_capacity'),
            ([dict(h, mono_ns=25, bus_observer=dict(h['bus_observer'], contexts=2)), h], 'bus_context_count_regressed'),
            ([h, dict(h, mono_ns=70, bus_observer=dict(h['bus_observer'], contexts=2)), pair(2, 8)],
             'bus_object_outside_contexts'),
            ([dict(h, bus_observer=dict(h['bus_observer'], contexts=0)),
              dict(p, request=dict(p['request'], issue_observed_ns=None, reply_observed_ns=None))],
             'bus_object_outside_contexts'),
            ([h, dict(h, mono_ns=50, bus_observer=dict(h['bus_observer'], capacity=65))], 'bus_capacity_changed'),
            ([dict(p, request=dict(p['request'], reply_connection=dict(result='connected', object=1, lifetime=6)))],
             'bus_lifetime_regressed'),
        ]
        for rows, code in cases:
            with self.subTest(code=code):
                a = audit.Auditor()
                for row in rows:
                    a.consume(row, code)
                self.assertTrue(any(i['code'] == code and i['severity'] == 'violation' for i in a.issues))

    def test_bus_delayed_snapshots_and_health_clocks(self):
        p, h = self.bus_rows()
        for observed, health_time, contradiction in ((15, 40, True), (None, 40, False),
                                                     (45, 40, False), (40, 40, False)):
            with self.subTest(observed=observed, health_time=health_time):
                old = dict(p, mono_ns=50, request=dict(p['request'],
                           issue_observed_ns=observed, reply_observed_ns=observed))
                a = audit.Auditor()
                a.consume(dict(h, mono_ns=health_time, bus_observer=dict(h['bus_observer'], contexts=0)), 'earlier-health')
                a.consume(old, 'late-row')
                found = any(i['code'] == 'bus_object_outside_contexts' for i in a.issues)
                self.assertEqual(found, contradiction)
        # Both snapshot receipt clocks may run after a newer connection was
        # observed. Only the issue -> reply order of ONE request is causal.
        snapshot = dict(result='connected', object=1, lifetime=8)
        newer = dict(p, call=2, mono_ns=420, request=dict(p['request'], bus_lifetime=8,
                     issue_observed_ns=300, reply_observed_ns=400,
                     issue_connection=snapshot, reply_connection=snapshot))
        old = dict(p, mono_ns=600, request=dict(p['request'], issue_observed_ns=450, reply_observed_ns=460))
        a = audit.Auditor()
        for row in (newer, old, dict(h, mono_ns=700)):
            a.consume(row, 'delayed-snapshot')
        self.assertFalse(any(i['code'].startswith('bus_') for i in a.issues))

    def test_bus_history_resets_at_boot_and_bounds_health_storage(self):
        p, h = self.bus_rows()
        a = audit.Auditor()
        a.consume(p, 'first-boot')
        a.consume(h, 'first-boot')
        # The same object/lifetime numbers are independent in the next boot.
        a.new_session()
        snapshot = dict(result='connected', object=2, lifetime=7)
        a.consume(dict(p, request=dict(p['request'], issue_connection=snapshot, reply_connection=snapshot)), 'next-boot')
        for now in range(40, 1040):
            a.consume(dict(h, mono_ns=now, bus_observer=dict(h['bus_observer'], contexts=2)), 'next-boot')
        self.assertFalse(any(i['code'].startswith('bus_') for i in a.issues))
        self.assertEqual(len(a.bus_health_counts), 1)

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
        for field in ('destination', 'path', 'interface', 'member', 'sender', 'error', 'wire_sender', 'wire_error'):
            for value, complete, valid in (
                    ('\0', True, False), ('a\0b', False, False),
                    ('\u0100', True, False), ('\ud800', False, False),
                    ('\U0001f680', True, False),
                    ('\x01\xff', True, True), ('\xff' * 63, True, True),
                    ('\xff' * 64, False, True), ('\xff' * 64, True, False)):
                with self.subTest(field=field, value=repr(value), complete=complete):
                    trace = dict(p['request'], route=dict(p['request']['route']))
                    if field.startswith('wire_'):
                        trace['wire'] = dict(trace['wire'], reply=dict(trace['wire']['reply']))
                        trace['wire']['reply'][field[5:]] = dict(value=value, complete=complete)
                    else:
                        target = trace['route'] if field in trace['route'] else trace
                        target[field] = dict(value=value, complete=complete)
                    a = audit.Auditor()
                    a.consume(dict(p, request=trace), 'byte-contract')
                    malformed = 'request_record_malformed' in [i['code'] for i in a.issues]
                    self.assertEqual(malformed, not valid)

    def test_production_records_and_bounds(self):
        command = shlex.split(os.environ.get('MX5DR_JOURNAL_FIXTURE', str(ROOT / 'build/test_journal')))
        rows = [json.loads(s) for s in subprocess.check_output(command + ['--emit-requests'], text=True).splitlines()]
        self.assertEqual(len(rows), 6)
        p, s, failed, escaped, longest, local = rows
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
        self.assertEqual(longest['request']['sender']['value'], '\x01'*64)
        self.assertTrue(all(v == dict(value='\x01'*64, complete=False) for v in longest['request']['route'].values()))
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
