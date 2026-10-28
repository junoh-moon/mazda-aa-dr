"""Synthetic tests for the offline entry-history research tool."""
import importlib.util
import math
import tempfile
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
SPEC = importlib.util.find_spec('tunnel_entry_eval')
E = None
if SPEC:
    E = importlib.util.module_from_spec(SPEC)
    SPEC.loader.exec_module(E)


class EntryAlignmentTest(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(E, 'tunnel_entry_eval research classifier is not implemented')

    @staticmethod
    def fixes(angle=0, source='fix1', start=0, n=8):
        h = math.radians(angle)
        return [(start + k, k * 15 * math.sin(h), k * 15 * math.cos(h), source)
                for k in range(n)]

    def classify(self, fixes, entry=None, point=None, s=120):
        entry = fixes[-1][0] if entry is None else entry
        return E.classify(fixes, entry, s, 1000, point or (lambda q: (0, q)))

    def test_straight_aligned_history_is_supported(self):
        self.assertEqual(self.classify(self.fixes())['state'], 'supported')

    def test_constant_direction_error_is_contradicted(self):
        self.assertEqual(self.classify(self.fixes(14))['state'], 'contradicted')

    def test_future_positions_do_not_affect_classification(self):
        past = self.fixes()
        future = past + [(8, 900, 900, 'fix1'), (9, -900, -900, 'fix1')]
        self.assertEqual(self.classify(past), self.classify(future, entry=7))

    def test_collector_duplicates_are_not_extra_evidence(self):
        fixes = self.fixes(n=2)
        dup = fixes + [(t + .01, x, y, 'collector') for t, x, y, _ in fixes]
        self.assertEqual(self.classify(dup, entry=1)['state'], 'insufficient')

    def test_large_receipt_gap_is_insufficient(self):
        fixes = self.fixes()
        del fixes[2:6]
        self.assertEqual(self.classify(fixes)['reason'], 'history_gap')

    def test_short_baseline_is_insufficient(self):
        fixes = [(k, 0, k * 2, 'fix1') for k in range(8)]
        self.assertEqual(self.classify(fixes)['reason'], 'short_baseline')

    def test_curved_unknown_preportal_geometry_is_insufficient(self):
        fixes = [(k, 30 * math.sin(k * math.pi / 6), k * 15, 'fix1') for k in range(8)]
        self.assertEqual(self.classify(fixes, s=0)['reason'], 'curved_unmapped_approach')

    def test_known_curved_map_can_support_matching_history(self):
        point = lambda s: (200 * (1 - math.cos(s / 200)), 200 * math.sin(s / 200))
        fixes = [(k, *point(k * 15), 'fix1') for k in range(8)]
        self.assertEqual(self.classify(fixes, point=point, s=105)['state'], 'supported')

    def test_direction_disagreement_is_insufficient(self):
        fixes = [(0, -25, 0, 'fix1'), (1, -15, 20, 'fix1'), (2, 0, 40, 'fix1'),
                 (3, 0, 60, 'fix1'), (4, -15, 80, 'fix1'), (5, -25, 100, 'fix1')]
        self.assertEqual(self.classify(fixes)['state'], 'insufficient')

    def test_mode_loss_in_history_is_insufficient(self):
        result = E.classify(self.fixes(), 7, 120, 1000, lambda s: (0, s), losses=[3])
        self.assertEqual(result['reason'], 'history_mode_loss')

    def test_nonfinite_input_is_insufficient(self):
        fixes = self.fixes()
        fixes[3] = (3, float('nan'), 45, 'fix1')
        self.assertEqual(self.classify(fixes)['state'], 'insufficient')

    def test_nonfinite_map_parameters_are_insufficient(self):
        for start, length in [(float('nan'), 1000), (120, float('inf')), (120, -1)]:
            result = E.classify(self.fixes(), 7, start, length, lambda q: (0, q))
            self.assertEqual(result['reason'], 'invalid_map_parameters')

    def test_nonfinite_map_points_are_insufficient(self):
        result = self.classify(self.fixes(), point=lambda q: (float('nan'), q))
        self.assertEqual(result['state'], 'insufficient')

    def test_reset_in_history_is_insufficient(self):
        import inspect
        self.assertIn('resets', inspect.signature(E.classify).parameters)
        result = E.classify(self.fixes(), 7, 120, 1000, lambda q: (0, q), resets=[3])
        self.assertEqual(result['reason'], 'history_reset')


class NativeAndProvenanceTest(unittest.TestCase):
    def test_actual_header_speed_gate_and_map_point(self):
        self.assertTrue(hasattr(E, 'NativeMap'), 'native product bridge is not implemented')
        import osm_tunnels as T
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'synthetic.bin'
            coords = [[370000000, 1270000000], [370050000, 1270000000]]
            rec = dict(id=1, pts=coords, len_m=555, oneway=True, end='portal')
            T.write_bin(dict(chains=[rec]), str(path))
            with E.NativeMap(path, expected_md5=None) as native:
                self.assertIsNone(native.match(37, 127, 0, 7, 0))
                match = native.match(37, 127, 0, 20, 0)
                self.assertIsNotNone(match)
                x, y = native.point(match, 100)
                self.assertAlmostEqual(x, 0, delta=.1)
                self.assertAlmostEqual(y, 100, delta=.1)

    def test_future_accepted_anchor_is_never_heading_seed(self):
        self.assertTrue(hasattr(E, 'heading_seed'), 'causal heading selection is not implemented')
        anchors = [dict(mono_ns=101000000000, gate='ACCEPTED', h=[1, 1400, 30])]
        self.assertEqual(E.heading_seed(anchors, 100)[1], 'raw_fix_course')
        self.assertEqual(E.heading_seed(anchors, 101), (140.0, 'logged_core_heading'))

    def test_old_anchor_is_not_borrowed_by_a_different_fix(self):
        self.assertTrue(hasattr(E, 'heading_seed'), 'causal heading selection is not implemented')
        anchors = [dict(mono_ns=100000000000, gate='ACCEPTED', h=[1, 1400, 30])]
        self.assertEqual(E.heading_seed(anchors, 101)[1], 'raw_fix_course')

    def test_same_fix_floor_bucket_uses_decision_availability(self):
        import inspect
        self.assertIn('decision_s', inspect.signature(E.heading_seed).parameters)
        anchors = [dict(mono_ns=101000999999, gate='ACCEPTED', h=[1, 1400, 30])]
        self.assertEqual(E.heading_seed(anchors, 101, decision_s=101.5),
                         (140.0, 'logged_core_heading'))
        self.assertEqual(E.heading_seed(anchors, 101, decision_s=101)[1], 'raw_fix_course')


class OfflineEvaluationTest(unittest.TestCase):
    def test_input_reset_is_preserved_by_archive_reader(self):
        import json
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / 'trace.0.jsonl'
            source.write_text(json.dumps(dict(kind='boot', boot_id='synthetic')) + '\n' +
                              json.dumps(dict(kind='shadow_input_reset', mono_ns=1500000000)) + '\n')
            self.assertEqual(E.load_logs([str(source)]).resets.get('syntheti'), {1.5})

    def test_directory_input_is_rejected_before_recursive_reader(self):
        self.assertTrue(hasattr(E, 'load_logs'), 'bounded offline log reader is not implemented')
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaises(ValueError):
                E.load_logs([td])

    def test_motion_gap_is_explicitly_inconclusive(self):
        self.assertTrue(hasattr(E, 'simulate'), 'approximate dynamics is not implemented')
        rows = [[1000, 1, 370000000, 1270000000, 10000, 0, 10000, 0, 0, 0, 0, 360]]
        result = E.simulate(None, None, rows, 0, 0)
        self.assertFalse(result['complete'])
        self.assertEqual(result['reason'], 'no_future_motion')

    def test_evaluate_empty_archive_contains_no_private_path(self):
        self.assertTrue(hasattr(E, 'evaluate'), 'offline evaluator is not implemented')
        import osm_tunnels as T
        import tarfile
        with tempfile.TemporaryDirectory() as td:
            binary, archive = Path(td) / 'synthetic.bin', Path(td) / 'private-owner.tar'
            T.write_bin(dict(chains=[dict(id=1, pts=[[370000000, 1270000000],
                       [370050000, 1270000000]], len_m=555, oneway=True, end='portal')]), str(binary))
            with tarfile.open(archive, 'w'):
                pass
            result = E.evaluate([str(archive)], binary, expected_md5=None)
            import json
            self.assertNotIn(td, json.dumps(result))
            self.assertEqual(result['episodes'], [])

    def test_yaw_release_past_portal_is_not_chain_end(self):
        import osm_tunnels as T
        with tempfile.TemporaryDirectory() as td:
            binary = Path(td) / 'synthetic.bin'
            T.write_bin(dict(chains=[dict(id=1, pts=[[370000000, 1270000000],
                       [370010000, 1270000000]], len_m=111, oneway=True, end='portal')]), str(binary))
            with E.NativeMap(binary, expected_md5=None) as native:
                match = native.match(37, 127, 0, 36, 0)
                match[1] = match[4] - 2  # already advanced just inside this portal
                rows = [[1000, 1, 370000000, 1270000000, 10000, 0, 10000, 0, 1000, 0, 50, 360],
                        [2000, 1, 370000900, 1270000000, 10000, 0, 10000, 0, 1000, 40000, 50, 360]]
                result = E.simulate(native, match, rows, 0, 0)
                self.assertTrue(result['complete'])
                self.assertEqual(result['reason'], 'yaw_gate')

    def test_recorded_boundary_stops_approximate_motion(self):
        import inspect
        self.assertIn('resets', inspect.signature(E.simulate).parameters)
        rows = [[1000, 1, 370000000, 1270000000, 10000, 0, 10000, 0, 1000, 0, 50, 360],
                [2000, 1, 370000900, 1270000000, 10000, 0, 10000, 0, 1000, 0, 50, 360]]
        result = E.simulate(None, None, rows, 0, 0, resets=[1.5])
        self.assertFalse(result['complete'])
        self.assertEqual(result['reason'], 'recorded_boundary')


if __name__ == '__main__':
    unittest.main()
