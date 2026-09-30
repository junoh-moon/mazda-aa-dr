"""Session fences are MODEL diagnostics; resets/rejections remain inconclusive."""
import importlib.util
from pathlib import Path
import unittest
from test_motion_logs import shadow, valid_shadow

spec = importlib.util.spec_from_file_location('model_session_audit',
        Path(__file__).resolve().parents[2] / 'tools/analyze_logs.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def session(**changes):
    row = dict(result='observed', basis='unique_live_context', lifetime=1,
               revision=3, event=2, state=0)
    row.update(changes)
    return row


def marker(**changes):
    row = dict(kind='shadow_session', domain='model', assist_ready=False, mono_ns=100,
               reset=False, input_available=True, model_session_epoch=1,
               raw_since_ns=100, session=session())
    row.update(changes)
    return row


class ModelSessionLog(unittest.TestCase):
    def auditor(self):
        a = audit.Auditor()
        a.new_session(dict(kind='boot'))
        return a

    def codes(self, a):
        return {i['code'] for i in a.issues}

    def test_marker_and_reset_are_understood_without_qualification(self):
        a = self.auditor()
        a.consume(marker(), 'initial')
        self.assertFalse(a.issues)
        a.consume(marker(mono_ns=200, raw_since_ns=200, reset=True,
                         model_session_epoch=2, session=session(revision=4)), 'boundary')
        self.assertEqual(self.codes(a), {'shadow_session_reset'})
        self.assertEqual(a.issues[0]['severity'], 'inconclusive')

    def test_rejection_is_not_an_unknown_record_or_success(self):
        a = self.auditor()
        a.consume(marker(), 'initial')
        a.consume(dict(kind='shadow_position_rejected', domain='model', assist_ready=False,
                       mono_ns=200, call=1, generation=2, session_revision=3,
                       reason='session_changed_since_issue'), 'delayed')
        self.assertEqual(self.codes(a), {'shadow_position_rejected'})
        self.assertEqual(a.issues[0]['severity'], 'inconclusive')

    def test_excluded_motion_keeps_its_actual_time_and_incomplete_scope(self):
        boundary = marker(mono_ns=2000000000, raw_since_ns=2000000000)
        row = dict(kind='shadow_motion_excluded', domain='model', assist_ready=False,
                   mono_ns=3000000000, raw_since_ns=2000000000, sensor=1,
                   epoch=1, receive_seq=5, received_ns=2100000000, source_mono_ms=1999,
                   reason='transport_before_session')
        for changes in ({}, {'reason': 'receipt_before_session', 'received_ns': 1990000000}):
            a = self.auditor()
            a.consume(boundary, 'initial')
            a.consume(dict(row, **changes), 'excluded')
            self.assertEqual(self.codes(a), {'shadow_motion_excluded'})
            self.assertEqual(a.issues[0]['severity'], 'inconclusive')
        for changes in ({'raw_since_ns': 1990000000}, {'epoch': True}, {'receive_seq': 0},
                        {'sensor': 4}, {'received_ns': 4000000000}, {'source_mono_ms': -1},
                        {'source_mono_ms': 2**63}, {'source_mono_ms': 2000},
                        {'reason': 'receipt_before_session'}, {'reason': 'unknown'}):
            with self.subTest(changes=changes):
                a = self.auditor()
                a.consume(boundary, 'initial')
                a.consume(dict(row, **changes), 'invalid-exclusion')
                self.assertIn('model_session_malformed', self.codes(a))
        a = self.auditor()
        a.consume(row, 'missing-boundary')
        self.assertIn('model_session_malformed', self.codes(a))

    def test_revision_types_and_backward_compatibility(self):
        old = session()
        del old['revision']
        self.assertTrue(audit.session_snapshot(old, 'unique_live_context'))
        for revision in (None, True, -1, 0, 2**64, '3'):
            with self.subTest(revision=revision):
                self.assertFalse(audit.session_snapshot(session(revision=revision), 'unique_live_context'))
        self.assertTrue(audit.session_snapshot(session(), 'unique_live_context'))
        for result in ('unobserved', 'transition', 'observation_fault'):
            self.assertTrue(audit.session_snapshot(session(result=result, lifetime=None,
                event=None, state=None, revision=None), 'unique_live_context'))
            self.assertFalse(audit.session_snapshot(session(result=result, lifetime=None,
                event=None, state=None, revision=1), 'unique_live_context'))

    def test_revision_counts_completed_creates_and_callbacks(self):
        for value in (session(revision=2), session(lifetime=8, event=2, revision=9),
                      session(lifetime=8, event=None, state=None, revision=7)):
            with self.subTest(value=value):
                self.assertFalse(audit.session_snapshot(value, 'unique_live_context'))
        for value in (session(), session(lifetime=8, event=2, revision=10),
                      session(lifetime=8, event=None, state=None, revision=8)):
            with self.subTest(valid=value):
                self.assertTrue(audit.session_snapshot(value, 'unique_live_context'))
        old = session(lifetime=8, event=2)
        del old['revision']
        self.assertTrue(audit.session_snapshot(old, 'unique_live_context'))

    def test_malformed_or_reused_model_session_epoch(self):
        for bad in ({'reset': True}, {'input_available': False}, {'raw_since_ns': 101},
                    {'raw_since_ns': 50},
                    {'model_session_epoch': True}, {'session': session(revision=None)}):
            with self.subTest(bad=bad):
                a = self.auditor()
                a.consume(marker(**bad), 'bad')
                self.assertIn('model_session_malformed', self.codes(a))
        a = self.auditor()
        a.consume(marker(), 'initial')
        a.consume(marker(mono_ns=200, raw_since_ns=200, reset=True), 'reused')
        self.assertIn('model_session_malformed', self.codes(a))

    def test_shadow_snapshot_must_match_recorded_boundary(self):
        row = shadow()
        row.update(mono_ns=150, model_session_epoch=1, session_revision=3)
        a = self.auditor()
        a.consume(marker(), 'initial')
        a.consume(row, 'same')
        self.assertFalse(a.issues)
        for changes in ({'model_session_epoch': 2}, {'session_revision': 4},
                        {'session_revision': True}):
            with self.subTest(changes=changes):
                b = self.auditor()
                b.consume(marker(), 'initial')
                b.consume(dict(row, **changes), 'stale')
                self.assertIn('shadow_session_mismatch', self.codes(b))
        b = self.auditor()
        b.consume(row, 'missing-marker')
        self.assertIn('shadow_session_mismatch', self.codes(b))

    def test_model_frontier_cannot_precede_its_session_or_follow_capture(self):
        # All rows carry matching epoch/revision. Identity alone cannot make
        # an old estimate or a future integration frontier valid evidence.
        base = valid_shadow()
        base.update(model_valid=True, mono_ns=200, frontier_ns=150,
                    model_session_epoch=1, session_revision=3)
        for changes in ({'frontier_ns': 90}, {'frontier_ns': 0},
                        {'mono_ns': 50, 'frontier_ns': 40}, {'frontier_ns': 300},
                        {'model_valid': False, 'result': 'E_NO_SEED', 'mono_ns': 50, 'frontier_ns': 0},
                        {'model_valid': False, 'result': 'E_TIME', 'frontier_ns': 300}):
            with self.subTest(changes=changes):
                a = self.auditor()
                a.consume(marker(), 'initial')
                a.consume(dict(base, **changes), 'impossible-time')
                found = [i for i in a.issues if i['code'] == 'shadow_session_time_inconsistent']
                self.assertEqual(len(found), 1)
                self.assertEqual(found[0]['severity'], 'violation')
        for changes in ({}, {'mono_ns': 100, 'frontier_ns': 100},
                        {'frontier_ns': 200}, {'model_valid': False, 'result': 'E_NO_SEED', 'frontier_ns': 0}):
            with self.subTest(valid=changes):
                a = self.auditor()
                a.consume(marker(), 'initial')
                a.consume(dict(base, **changes), 'valid-time')
                self.assertFalse(a.issues)
        legacy = dict(base, mono_ns=50, frontier_ns=40)
        del legacy['model_session_epoch'], legacy['session_revision']
        a = self.auditor()
        a.consume(legacy, 'legacy-without-observed-boundary')
        self.assertFalse(a.issues)


if __name__ == '__main__':
    unittest.main()
