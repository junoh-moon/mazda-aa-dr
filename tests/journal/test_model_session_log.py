"""Session fences are MODEL diagnostics; resets/rejections remain inconclusive."""
import importlib.util
from pathlib import Path
import unittest
from test_motion_logs import shadow

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

    def test_malformed_or_reused_model_session_epoch(self):
        for bad in ({'reset': True}, {'input_available': False}, {'raw_since_ns': 101},
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


if __name__ == '__main__':
    unittest.main()
