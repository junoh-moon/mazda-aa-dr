"""Bus boundaries must constrain MODEL diagnostics without granting qualification."""
import unittest
from test_model_session_log import audit, marker as session_marker
from test_motion_logs import shadow
from test_calibration_logs import holdout
import test_request_log as request_fixtures


def marker(**changes):
    row = dict(kind='shadow_bus', domain='model', assist_ready=False, mono_ns=100,
               reset=False, input_available=True, model_bus_epoch=1, bus_revision=3,
               raw_since_ns=100, connection=dict(result='connected', object=1, lifetime=1))
    row.update(changes)
    return row


class ModelBusLog(unittest.TestCase):
    def auditor(self):
        a = audit.Auditor()
        a.new_session(dict(kind='boot'))
        return a

    def codes(self, a):
        return {i['code'] for i in a.issues}

    def test_boundary_reset_is_inconclusive(self):
        a = self.auditor()
        a.consume(marker(), 'initial')
        self.assertFalse(a.issues)
        a.consume(marker(mono_ns=200, raw_since_ns=200, reset=True, model_bus_epoch=2,
                         bus_revision=4, input_available=False,
                         connection=dict(result='no_live_connection', object=None, lifetime=None)), 'lost')
        self.assertEqual(self.codes(a), {'shadow_bus_reset'})
        self.assertEqual(a.issues[0]['severity'], 'inconclusive')

    def test_invalid_boundaries(self):
        for changes in ({'reset': True}, {'raw_since_ns': 50}, {'bus_revision': True},
                        {'bus_revision': 0}, {'bus_revision': 2**64}, {'model_bus_epoch': 0},
                        {'input_available': False}, {'connection': dict(result='disconnected', object=1, lifetime=None)}):
            with self.subTest(changes=changes):
                a = self.auditor()
                a.consume(marker(**changes), 'bad')
                self.assertIn('model_bus_malformed', self.codes(a))
        a = self.auditor()
        a.consume(marker(), 'initial')
        a.consume(marker(reset=True), 'reused-epoch')
        self.assertIn('model_bus_malformed', self.codes(a))

    def test_unavailable_and_ambiguous_cannot_admit_prediction(self):
        for result in ('unobserved', 'no_live_connection', 'ambiguous', 'transition', 'observation_fault'):
            a = self.auditor()
            revision = 4 if result == 'no_live_connection' else 6 if result == 'ambiguous' else 0
            a.consume(marker(bus_revision=revision, input_available=False,
                             connection=dict(result=result, object=None, lifetime=None)), 'unavailable')
            row = shadow()
            row.update(mono_ns=200, frontier_ns=150, model_valid=True,
                       model_bus_epoch=1, bus_revision=revision)
            a.consume(row, 'invalid-positive')
            self.assertIn('shadow_bus_mismatch', self.codes(a))

    def test_snapshot_identity_time_and_legacy(self):
        row = shadow()
        row.update(mono_ns=200, frontier_ns=150, model_valid=True, model_bus_epoch=1, bus_revision=3)
        a = self.auditor()
        a.consume(marker(), 'initial')
        a.consume(row, 'valid')
        self.assertFalse(a.issues)
        for changes, code in (({'model_bus_epoch': 2}, 'shadow_bus_mismatch'),
                              ({'bus_revision': 4}, 'shadow_bus_mismatch'),
                              ({'frontier_ns': 90}, 'shadow_bus_time_inconsistent'),
                              ({'frontier_ns': 201}, 'shadow_bus_time_inconsistent'),
                              ({'mono_ns': 50}, 'shadow_bus_time_inconsistent')):
            a = self.auditor()
            a.consume(marker(), 'initial')
            a.consume(dict(row, **changes), 'bad')
            self.assertIn(code, self.codes(a))
        a = self.auditor()
        a.consume(row, 'missing-boundary')
        self.assertIn('shadow_bus_mismatch', self.codes(a))
        del row['model_bus_epoch'], row['bus_revision']
        a = self.auditor()
        a.consume(row, 'legacy')
        self.assertFalse(a.issues)

    def test_rejections_require_matching_bus_marker(self):
        for reason in ('bus_unavailable', 'request_bus_unobserved', 'bus_changed_since_issue',
                       'request_before_bus_boundary'):
            boundary = marker()
            if reason == 'bus_unavailable':
                boundary.update(bus_revision=0, input_available=False,
                                connection=dict(result='transition', object=None, lifetime=None))
            row = dict(kind='shadow_position_rejected', domain='model', assist_ready=False,
                       mono_ns=200, call=1, generation=2, session_revision=1,
                       model_bus_epoch=1, bus_revision=boundary['bus_revision'], reason=reason)
            a = self.auditor()
            a.consume(boundary, 'initial')
            a.consume(row, 'rejected')
            self.assertIn('shadow_position_rejected', self.codes(a))
            self.assertNotIn('model_bus_malformed', self.codes(a))
            b = self.auditor()
            b.consume(boundary, 'initial')
            b.consume(dict(row, bus_revision=boundary['bus_revision']+1), 'wrong-boundary')
            self.assertIn('model_bus_malformed', self.codes(b))

    def test_completed_mutation_revision_lower_bounds(self):
        for connection, minimum in ((dict(result='connected', object=1, lifetime=1), 3),
                                    (dict(result='connected', object=2, lifetime=1), 4),
                                    (dict(result='connected', object=1, lifetime=2), 4),
                                    (dict(result='no_live_connection', object=None, lifetime=None), 4),
                                    (dict(result='ambiguous', object=None, lifetime=None), 6)):
            for revision in (minimum-1, minimum, minimum+10):
                with self.subTest(connection=connection, revision=revision):
                    a = self.auditor()
                    a.consume(marker(connection=connection, bus_revision=revision,
                                     input_available=connection['result']=='connected'), 'boundary')
                    self.assertEqual('model_bus_malformed' in self.codes(a), revision < minimum)

    def test_worker_ordered_bus_history(self):
        first = marker(bus_revision=10)
        changed = marker(mono_ns=200, raw_since_ns=200, reset=True, model_bus_epoch=2, bus_revision=11)
        transition = dict(changed, bus_revision=0, input_available=False,
                          connection=dict(result='transition', object=None, lifetime=None))
        fault = dict(transition, connection=dict(result='observation_fault', object=None, lifetime=None))
        cases = [
            [first, dict(changed, bus_revision=9)],
            [first, dict(changed, bus_revision=10, connection=dict(result='connected', object=1, lifetime=2))],
            [first, dict(changed, bus_revision=10)],
            [first, dict(changed, input_available=False, connection=dict(result='unobserved', object=None, lifetime=None))],
            [first, transition, dict(changed, mono_ns=300, raw_since_ns=300, model_bus_epoch=3, bus_revision=9)],
            [first, transition, dict(changed, mono_ns=300, raw_since_ns=300, model_bus_epoch=3, bus_revision=10)],
            [first, fault, dict(changed, mono_ns=300, raw_since_ns=300, model_bus_epoch=3)],
            [dict(first, connection=dict(result='connected', object=1, lifetime=2)), changed],
        ]
        for rows in cases:
            with self.subTest(rows=rows):
                a = self.auditor()
                for row in rows:
                    a.consume(row, 'history')
                self.assertTrue(any(i['code']=='model_bus_history_inconsistent' and i['severity']=='violation'
                                    for i in a.issues))
        # A surviving old connection can remain after another object is freed.
        ambiguous = dict(changed, bus_revision=13, input_available=False,
                         connection=dict(result='ambiguous', object=None, lifetime=None))
        for rows in ([first, changed], [first, ambiguous,
                     dict(changed, mono_ns=300, raw_since_ns=300, model_bus_epoch=3, bus_revision=14)]):
            a = self.auditor()
            for row in rows:
                a.consume(row, 'possible')
            self.assertFalse(any(i['severity']=='violation' for i in a.issues))
        a.new_session(dict(kind='boot'))
        a.consume(marker(), 'new-boot')
        self.assertNotIn('model_bus_history_inconsistent', self.codes(a))

    def test_boundary_shares_request_lifetime_and_health_history(self):
        p, h = request_fixtures.RequestJournal.bus_rows()
        boundary = marker(bus_revision=10, connection=dict(result='connected', object=2, lifetime=7))
        for rows, code in (([p, boundary], 'bus_lifetime_owner_changed'),
                           ([boundary, dict(h, mono_ns=200)], 'bus_object_outside_contexts'),
                           ([h, marker(bus_revision=73, connection=dict(result='connected', object=65, lifetime=7))],
                            'bus_object_outside_capacity')):
            a = self.auditor()
            for row in rows:
                a.consume(row, 'contradiction')
            self.assertTrue(any(i['code']==code and i['severity']=='violation' for i in a.issues))
        # An older request may drain after a newer worker boundary: no lifetime
        # monotonic rule may be inferred across those two snapshot sites.
        a = self.auditor()
        a.consume(marker(bus_revision=12, connection=dict(result='connected', object=1, lifetime=8)), 'newer')
        a.consume(dict(p, mono_ns=300, request=dict(p['request'],
                  issue_observed_ns=250, reply_observed_ns=260)), 'old-snapshot-late-clock')
        self.assertFalse(any(i['severity']=='violation' for i in a.issues))

    def test_motion_exclusion_uses_later_available_boundary(self):
        for session_ns, bus_ns, session_live, bus_live, chosen in (
                (100, 200, True, True, 'bus'), (200, 100, True, True, 'session'),
                (100, 100, True, True, 'session'), (100, 200, False, True, None),
                (200, 100, True, False, None)):
            for reason in ('bus', 'session'):
                with self.subTest(session_ns=session_ns, bus_ns=bus_ns, reason=reason,
                                  session_live=session_live, bus_live=bus_live):
                    a = self.auditor()
                    s = session_marker(mono_ns=session_ns, raw_since_ns=session_ns)
                    b = marker(mono_ns=bus_ns, raw_since_ns=bus_ns)
                    if not session_live:
                        s.update(input_available=False, session=dict(result='unobserved', basis='unique_live_context',
                                 lifetime=None, event=None, state=None, revision=None))
                    if not bus_live:
                        b.update(input_available=False, bus_revision=0,
                                 connection=dict(result='transition', object=None, lifetime=None))
                    a.consume(s, 'session'); a.consume(b, 'bus')
                    a.consume(dict(kind='shadow_motion_excluded', mono_ns=300, domain='model', assist_ready=False,
                                   reason='receipt_before_'+reason, raw_since_ns=bus_ns if reason=='bus' else session_ns,
                                   sensor=1, epoch=1, receive_seq=1, received_ns=99, source_mono_ms=0), 'raw')
                    self.assertEqual('model_session_malformed' in self.codes(a), chosen != reason)

    def test_rejection_reason_and_session_match(self):
        row = dict(kind='shadow_position_rejected', domain='model', assist_ready=False,
                   mono_ns=200, call=1, generation=2, session_revision=3,
                   model_bus_epoch=1, bus_revision=3, reason='bus_unavailable')
        a = self.auditor()
        a.consume(marker(), 'connected')
        a.consume(row, 'impossible-unavailable')
        self.assertIn('model_bus_malformed', self.codes(a))
        a = self.auditor()
        a.consume(session_marker(), 'session'); a.consume(marker(), 'bus')
        a.consume(dict(row, session_revision=2, reason='bus_changed_since_issue'), 'wrong-session')
        self.assertIn('model_session_malformed', self.codes(a))
        old = dict(row, reason='session_changed_since_issue')
        del old['model_bus_epoch'], old['bus_revision']
        a = self.auditor()
        a.consume(old, 'legacy-without-bus-schema')
        self.assertEqual(self.codes(a), {'shadow_position_rejected'})
        a = self.auditor()
        a.consume(marker(), 'known-bus-schema')
        a.consume(old, 'missing-new-fields')
        self.assertIn('model_bus_malformed', self.codes(a))

    def test_holdout_cannot_cross_or_precede_bus_boundary(self):
        for rows, code in (([marker(mono_ns=150, raw_since_ns=150), holdout(mono_ns=160)], 'invalid_holdout_boundary'),
                           ([marker(input_available=False, bus_revision=0,
                                    connection=dict(result='transition', object=None, lifetime=None)), holdout()],
                            'invalid_holdout_boundary'),
                           ([marker(mono_ns=80, raw_since_ns=80), holdout(),
                             marker(mono_ns=150, raw_since_ns=150, reset=True, model_bus_epoch=2, bus_revision=4),
                             holdout('COMPARED')], 'holdout_crosses_bus_boundary'),
                           ([holdout('COMPARED', reason='bus_reset')], 'invalid_holdout_boundary')):
            a = self.auditor()
            for row in rows:
                a.consume(row, 'bad-holdout')
            self.assertIn(code, self.codes(a))
            self.assertEqual(a.holdout_position['count'], 0)
        a = self.auditor()
        for row in (marker(mono_ns=80, raw_since_ns=80), holdout(),
                    holdout('ABORT', mono_ns=150, reason='bus_reset'),
                    marker(mono_ns=150, raw_since_ns=150, reset=True, model_bus_epoch=2, bus_revision=4)):
            a.consume(row, 'abort-before-boundary')
        self.assertEqual(self.codes(a), {'holdout_aborted', 'shadow_bus_reset'})

    def test_old_receipt_and_transport_are_preserved_exclusions(self):
        for reason, received, transport in (('receipt_before_bus', 1990000000, 0),
                                            ('transport_before_bus', 2100000000, 1999)):
            row = dict(kind='shadow_motion_excluded', domain='model', assist_ready=False,
                       mono_ns=3000000000, raw_since_ns=2000000000, sensor=1, epoch=1,
                       receive_seq=1, received_ns=received, source_mono_ms=transport, reason=reason)
            a = self.auditor()
            a.consume(marker(mono_ns=2000000000, raw_since_ns=2000000000), 'initial')
            a.consume(row, 'excluded')
            self.assertEqual(self.codes(a), {'shadow_motion_excluded'})
            b = self.auditor()
            b.consume(marker(mono_ns=2000000000, raw_since_ns=2000000000), 'initial')
            b.consume(dict(row, received_ns=2200000000, source_mono_ms=2100), 'not-old')
            self.assertIn('model_session_malformed', self.codes(b))


if __name__ == '__main__':
    unittest.main()
