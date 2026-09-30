"""Cross-language journal contract: production C++ encoder -> Python auditor."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('analyze_logs', ROOT / 'tools/analyze_logs.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
FIXTURE = shlex.split(os.environ.get('MX5DR_MOTION_FIXTURE', str(ROOT / 'build/test_motion_batch')))


def event(i):
    return dict(kind='motion', sensor=1 + i % 3, epoch=42, receive_seq=i + 1,
                received_ns=1000000000000 + i * 6666667,
                source_mono_ms=-1 if i % 7 == 0 else 0,
                producer_time_status='unknown', raw=[(i * 13 + j) % 65536 for j in range(4)],
                count=i % 256, reverse=i % 5)


def packed(events):
    return dict(kind='motion_batch', schema=1, epoch=events[0]['epoch'],
                producer_time_status='unknown', events=[[
                    e['sensor'], e['receive_seq'], e['received_ns'], e['source_mono_ms'],
                    *e['raw'], e['count'], e['reverse']] for e in events])


def boot():
    return dict(kind='boot', schema=1, pid=123, mono_ns=1, mode=4,
                install='ok', assist_ready=False, assist_block='unverified',
                wire_timestamp_modified=False)


def shadow():
    return dict(kind='shadow', mono_ns=1000000000000, domain='model', model_valid=False,
                assist_ready=False, state=0, result='E_NO_SEED', pipeline='WAITING',
                uncertainties=127, events=0, intervals=0, resets=0, rejected=0,
                frontier_ns=0, lat=None, lon=None, heading_rad=None, speed_mps=None,
                error_model_m=None, stopped=False, preview_encoded=False, location_preview_hex='')


def valid_shadow(**changes):
    row = shadow()
    row.update(mono_ns=200, frontier_ns=150, state=2, result='OK', pipeline='OK',
               model_valid=True, events=6, intervals=1, lat=35, lon=129,
               heading_rad=0, speed_mps=10, error_model_m=5)
    row.update(changes)
    return row


def consume(rows):
    a = audit.Auditor()
    a.consume(boot(), 'boot')
    for i, row in enumerate(rows):
        a.consume(row, str(i))
    return a


def codes(a):
    return [issue['code'] for issue in a.issues]


class MotionLogs(unittest.TestCase):
    def test_cpp_roundtrip_all_fields_and_batch_sizes(self):
        for size in (0, 1, 31, 32, 33, 256, 1000):
            with self.subTest(size=size):
                text = subprocess.check_output(FIXTURE + ['--emit', str(size)], text=True)
                rows = [json.loads(line) for line in text.splitlines()]
                decoded = [e for row in rows for e in audit.decode_motion_records(row)]
                self.assertEqual(decoded, [event(i) for i in range(size)])
                self.assertTrue(all(len(line) < 6144 for line in text.splitlines()))
                self.assertEqual(len(rows), (size + 31) // 32)

    def test_cpp_maximum_width_integers_remain_exact(self):
        text = subprocess.check_output(FIXTURE + ['--max'], text=True)
        self.assertLess(len(text), 6144)
        rows = audit.decode_motion_records(json.loads(text))
        self.assertEqual(len(rows), 32)
        expected = event(0)
        expected.update(epoch=2**64-1, receive_seq=2**64-1, received_ns=2**64-1,
                        source_mono_ms=-2**63, raw=[65535]*4, count=65535, reverse=65535)
        self.assertTrue(all(row == expected for row in rows))

    def test_synthetic_volume_without_sampling(self):
        size = 9000
        text = subprocess.check_output(FIXTURE + ['--emit', str(size), '8'], text=True)
        decoded = [e for line in text.splitlines() for e in audit.decode_motion_records(json.loads(line))]
        expected = [event(i) for i in range(size)]
        self.assertEqual(decoded, expected)
        legacy = ''.join(json.dumps(e, separators=(',', ':')) + '\n' for e in expected)
        self.assertLess(len(text), len(legacy) * 0.5)
        print('Synthetic motion journal: %d samples, %d legacy bytes -> %d batch bytes (%.1f%% reduction)' %
              (size, len(legacy), len(text), 100 * (1 - len(text)/len(legacy))))

    def test_legacy_compact_and_mixed_statistics_match(self):
        rows = [event(i) for i in range(100)]
        compact = [packed(rows[i:i+32]) for i in range(0, len(rows), 32)]
        mixed = rows[:20] + [packed(rows[20:52])] + rows[52:]
        reports = [consume(v) for v in (rows, compact, mixed)]
        for a in reports:
            self.assertEqual(a.motion_samples, 100)
            self.assertEqual(a.motion_sensors, reports[0].motion_sensors)
            self.assertEqual(codes(a), [])
        self.assertEqual(reports[1].motion_batches, 4)

    def test_malformed_batches_reject_every_row(self):
        good = packed([event(1), event(2)])
        cases = []
        for key, value in [('schema', True), ('schema', 2), ('epoch', 0),
                           ('epoch', 2**64), ('producer_time_status', 'verified'),
                           ('events', []), ('events', good['events'] * 17), ('events', 'bad')]:
            bad = copy.deepcopy(good); bad[key] = value; cases.append(bad)
        for col, value in [(0, True), (0, 4), (1, 0), (1, 2**64), (2, -1),
                           (3, 2**63), (3, -2**63-1), (4, 65536), (5, -1),
                           (6, 1.0), (8, False), (9, -1), (9, 65536)]:
            bad = copy.deepcopy(good); bad['events'][1][col] = value; cases.append(bad)
        for values in ([1], list(range(11)), None):
            bad = copy.deepcopy(good); bad['events'][1] = values; cases.append(bad)
        for bad in cases:
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError): audit.decode_motion_records(bad)
                a = consume([event(0), bad, event(3)])
                self.assertEqual(a.motion_samples, 2)  # No valid prefix leaked.
                self.assertEqual(a.motion_batches, 0)
                self.assertIn('malformed_motion', codes(a))
                self.assertIn('motion_sequence_gap', codes(a))

    def test_legacy_uses_same_range_and_type_validation(self):
        for key, value in [('raw', [0, 0, 0]), ('count', True),
                           ('source_mono_ms', 0.5), ('producer_time_status', 'validated')]:
            bad = event(0); bad[key] = value
            self.assertIn('malformed_motion', codes(consume([bad])))

    def test_sequence_high_water_does_not_rewind(self):
        a = consume([event(99), event(49), event(50), event(100), event(100)])
        self.assertEqual(codes(a).count('motion_sequence_replayed'), 3)
        self.assertNotIn('motion_sequence_gap', codes(a))
        self.assertIn('motion_clock_regressed', codes(a))
        self.assertEqual(a.session['motion_seq'], 101)

    def test_reset_marker_does_not_hide_gap(self):
        a = consume([packed([event(0)]), dict(kind='shadow_input_reset',
                    reason='channel_discontinuity', assist_ready=False), packed([event(2)])])
        self.assertIn('shadow_input_reset', codes(a))
        self.assertIn('motion_sequence_gap', codes(a))

    def test_source_restart_and_runtime_boot_are_different(self):
        new = event(0); new['epoch'] = 43
        a = consume([event(500), new])
        self.assertEqual(codes(a), ['motion_source_restart'])
        a = consume([event(500), boot(), event(0)])
        self.assertEqual(codes(a), [])

    def test_rotation_keeps_continuity_and_batch_needs_no_dictionary(self):
        with tempfile.TemporaryDirectory() as root:
            older = Path(root) / 'trace.1.jsonl'; newer = Path(root) / 'trace.0.jsonl'
            older.write_text(json.dumps(boot()) + '\n' + json.dumps(packed([event(0)])) + '\n')
            newer.write_text(json.dumps(packed([event(2)])) + '\n')
            a = audit.Auditor(); a.read_path(root)
            self.assertIn('motion_sequence_gap', codes(a))
            self.assertEqual(a.motion_samples, 2)
            self.assertEqual(audit.decode_motion_records(json.loads(newer.read_text())), [event(2)])

    def test_shadow_records_do_not_promote_model_or_preview(self):
        start = dict(kind='shadow_boot', active=True, domain='model',
                     source='existing_vbs_vim_callback', assist_ready=False,
                     motion_log_format='motion_batch_v1', motion_sampling=False)
        a = consume([start, shadow()])
        self.assertEqual(codes(a), [])
        for key, value, code in [('domain', 'qualified', 'unexpected_shadow_domain'),
                                 ('assist_ready', True, 'impossible_live_capability'),
                                 ('location_preview_hex', 'a'*96, 'invalid_shadow_preview')]:
            bad = shadow(); bad[key] = value
            self.assertIn(code, codes(consume([bad])))
        bad = shadow(); bad['preview_encoded'] = True
        self.assertIn('invalid_shadow_preview', codes(consume([bad])))
        start['active'] = False
        self.assertIn('shadow_inactive', codes(consume([start])))

    def test_health_must_cover_motion_and_shadow_tail(self):
        for diagnostic in (event(0), packed([event(0)]), shadow()):
            health = dict(kind='health', mono_ns=2, dropped=0, hook_installed=True, assist_ready=False)
            a = consume([diagnostic, health])
            self.assertIn('uncovered_trace_tail', {i['code'] for i in a.report()['issues']})
            health['mono_ns'] = 2000000000000
            a = consume([diagnostic, health])
            self.assertNotIn('uncovered_trace_tail', {i['code'] for i in a.report()['issues']})

    def test_shadow_faults_remain_visible_after_pipeline_recovers(self):
        fault = shadow(); fault.update(pipeline='OVERFLOW', resets=1, rejected=1)
        recovered = shadow(); recovered.update(pipeline='OK', resets=1, rejected=1)
        a = consume([fault, recovered])
        self.assertIn('shadow_pipeline_fault', codes(a))
        self.assertEqual(codes(a).count('shadow_resets'), 1)
        self.assertEqual(codes(a).count('shadow_rejected'), 1)
        report = a.report()
        self.assertEqual(report['status'], 'inconclusive')
        self.assertEqual(report['shadow']['pipelines'], {'OVERFLOW': 1, 'OK': 1})
        self.assertEqual(report['shadow']['rejected_max'], 1)
        for state in ('WAITING', 'NO_ANCHOR', 'MISSING_SENSOR'):
            row = shadow(); row['pipeline'] = state
            self.assertNotIn('shadow_pipeline_fault', codes(consume([row])))

    def test_shadow_impossible_numbers_and_preview_are_rejected(self):
        for key, value in [('state', -1), ('state', 7), ('mono_ns', 0), ('mono_ns', 2**64),
                           ('resets', -1), ('rejected', -1), ('events', 2**64),
                           ('frontier_ns', -1), ('uncertainties', -1)]:
            row = shadow(); row[key] = value
            self.assertIn('partial_record', codes(consume([row])))
        row = shadow(); row.update(preview_encoded=True, location_preview_hex='a'*96)
        self.assertIn('invalid_shadow_preview', codes(consume([row])))


if __name__ == '__main__':
    unittest.main()
