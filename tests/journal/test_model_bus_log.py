"""Bus boundaries must constrain MODEL diagnostics without granting qualification."""
import unittest
from test_model_session_log import audit
from test_motion_logs import shadow


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
            revision = 3 if result in ('no_live_connection', 'ambiguous') else 0
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
            row = dict(kind='shadow_position_rejected', domain='model', assist_ready=False,
                       mono_ns=200, call=1, generation=2, session_revision=1,
                       model_bus_epoch=1, bus_revision=3, reason=reason)
            a = self.auditor()
            a.consume(marker(), 'initial')
            a.consume(row, 'rejected')
            self.assertEqual(self.codes(a), {'shadow_position_rejected'})
            b = self.auditor()
            b.consume(marker(), 'initial')
            b.consume(dict(row, bus_revision=4), 'wrong-boundary')
            self.assertIn('model_bus_malformed', self.codes(b))

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
