"""log_profile=persistent against the full profile on the same SYNTHETIC drive.

build/log_rate (tests/runtime/log_rate.cpp) runs the product journal path in
simulated time: real adapter POSITION/SEND logic, row formatters, Pipeline,
BetaController and the PersistentLog filter, with a synthetic drive (boot-time
NO_FIX, GPS outages every 10 minutes) and measured AA send rates. The quiet
profile must keep every BETA evidence row byte for byte, stay within its rate
budget and be accepted by the analyzer without new findings. Synthetic data:
not vehicle, phone or DHU evidence.
"""
import collections
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOL = Path(os.environ.get('MX5DR_LOG_RATE', ROOT / 'build/log_rate'))
spec = importlib.util.spec_from_file_location('profile_audit', ROOT / 'tools/analyze_logs.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)

EVIDENCE_KINDS = ('beta_state', 'beta_anchor', 'beta_hold', 'beta_session_storage', 'beta_reverse_latch')


def run(profile, directory, seconds=3600, every=600, extra=()):
    result = subprocess.run([str(TOOL), '--profile', profile, '--seconds', str(seconds),
                             '--event-every', str(every), '--out', directory] + list(extra),
                            capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    summary = json.loads(result.stdout)
    rows = []
    for index in (2, 1, 0):
        path = Path(directory) / 'logs' / ('trace.%d.jsonl' % index)
        if path.exists():
            rows += [json.loads(line) for line in path.read_text().splitlines()]
    return summary, rows


def analyze(directory):
    codes = collections.Counter()
    original = audit.Auditor.issue

    def issue(self, code, source, detail, violation=False):
        codes[(violation, code)] += 1
        return original(self, code, source, detail, violation)
    audit.Auditor.issue = issue
    try:
        report = audit.analyze([str(Path(directory) / 'logs')])
    finally:
        audit.Auditor.issue = original
    return report, codes


def evidence(rows):
    return [json.dumps(r, sort_keys=True) for r in rows
            if r['kind'] in EVIDENCE_KINDS or (r['kind'] == 'send' and r['choice'] != 0)]


@unittest.skipUnless(TOOL.exists(), 'build/log_rate not built')
class PersistentProfile(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='mx5dr-log-profile-')
        cls.full_dir = os.path.join(cls.tmp.name, 'full')
        cls.quiet_dir = os.path.join(cls.tmp.name, 'persistent')
        cls.full, cls.full_rows = run('full', cls.full_dir)
        cls.quiet, cls.quiet_rows = run('persistent', cls.quiet_dir)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_rate_budget(self):
        # The full profile reproduces the measured vehicle rate (27.7-35.8 KB/s);
        # the quiet one stays under about 2 KB/s with one GPS outage per 10 min.
        self.assertGreater(self.full['bytes_per_s'], 20000)
        self.assertLess(self.quiet['bytes_per_s'], 2048)
        # At least 3 % margin below the budget with the yaw data rows
        # (validation/YAW_DATA_COLLECTION_2026-10-09.md; 1942.0 B/s before).
        self.assertLess(self.quiet['bytes_per_s'], 2048 * 0.97)
        self.assertEqual(self.full['outages'], self.quiet['outages'])
        self.assertGreaterEqual(self.quiet['outages'], 1)

    def test_every_beta_evidence_row_is_kept_unchanged(self):
        full, quiet = evidence(self.full_rows), evidence(self.quiet_rows)
        self.assertTrue(any('"choice": 3' in r for r in full), 'the drive must engage BETA')
        self.assertTrue(any('"choice": 4' in r for r in full), 'the drive must overlay NO_FIX')
        self.assertEqual(full, quiet)

    def test_raw_rows_are_replaced_by_digests_and_windows(self):
        kinds = collections.Counter(r['kind'] for r in self.quiet_rows)
        self.assertGreater(kinds['log_digest'], 0)
        self.assertGreater(kinds['raw_window'], 0)
        self.assertLess(kinds['motion_batch'], collections.Counter(r['kind'] for r in self.full_rows)['motion_batch'])
        # Periodic rows on their cadence; the digest every 10 s.
        for kind, limit in (('log_digest', 10.05e9), ('health', 10.05e9)):
            times = [r['mono_ns'] for r in self.quiet_rows if r['kind'] == kind]
            self.assertTrue(times)
            self.assertLessEqual(max(b - a for a, b in zip(times, times[1:])), limit, kind)
        # At the vehicle cadence (10 Hz wheels and yaw) the RAW window really
        # holds the 60 s before an event (marker span_ms, F3-D).
        # Only windows that could fill: >= 60 s since the previous raw period.
        marks = [r for r in self.quiet_rows if r['kind'] == 'raw_window']
        spans = [b['span_ms'] for a, b in zip(marks, marks[1:])
                 if b['mono_ns'] - (a['mono_ns'] + a['post_ms'] * 1000000) >= 60e9]
        self.assertGreaterEqual(len(spans), 3)
        self.assertGreaterEqual(min(spans), 59000)
        # Every motion event appears in the digests exactly once.
        digested = sum(r['motion_events'] for r in self.quiet_rows if r['kind'] == 'log_digest')
        accepted = sum(len(r['events']) for r in self.full_rows if r['kind'] == 'motion_batch')
        self.assertEqual(digested, accepted)

    def test_analyzer_accepts_the_quiet_profile(self):
        full_report, full_codes = analyze(self.full_dir)
        quiet_report, quiet_codes = analyze(self.quiet_dir)
        self.assertEqual(quiet_report['issue_counts'].get('violation', 0), 0)
        self.assertFalse([c for (v, c) in quiet_codes if v])
        # No finding that the full profile of the same drive does not have.
        self.assertLessEqual({c for (_, c) in quiet_codes}, {c for (_, c) in full_codes})
        for key in ('replaced_sends', 'speed_overlay_sends', 'engaged_periods', 'withdraw_reasons'):
            self.assertEqual(quiet_report['beta'][key], full_report['beta'][key], key)
        self.assertEqual(quiet_report['log_profiles'], {'persistent': 1})
        persistent = quiet_report['persistent_profile']
        self.assertGreater(persistent['digests'], 0)
        self.assertGreater(persistent['raw_windows'], 0)

    def test_analyzer_tolerates_unknown_digest_kinds_and_isolated_replacements(self):
        # A replaced send whose neighbours are digests (no raw rows around it)
        # and digest kinds/fields this analyzer does not know yet.
        rows = [r for r in self.quiet_rows if r['kind'] not in ('motion_batch', 'raw_window')]
        rows.insert(3, dict(kind='log_digest', schema=1, digest='future_kind', mono_ns=1, novel={'x': 1}))
        rows.insert(4, dict(kind='sensor_digest', schema=1, mono_ns=1))
        a = audit.Auditor()
        for index, row in enumerate(rows):
            a.consume(row, 'synthetic:%d' % index)
        report = a.report()
        self.assertEqual(report['issue_counts'].get('violation', 0), 0)
        self.assertNotIn('unknown_record_kind', [i['code'] for i in report['issues']])
        self.assertEqual(report['persistent_profile']['digest_kinds'].get('log_digest:future_kind'), 1)


# Yaw-zero data rows (2026-10-09, src/runtime/yaw_study_log.h,
# validation/YAW_DATA_COLLECTION_2026-10-09.md): logging only. With the rows
# off the profile is the previous one; with them on, every other row is byte
# for byte the same and log_digest only gains fields.
YAW_DIGEST_KEYS = ('y1', 'y1n', 'yst', 'ystn', 'gc', 'gq', 'gv', 'gc_more', 'dw01', 'dw23')
YAW_KINDS = ('yaw_stop', 'yaw_edge', 'yaw_reinit')
VEHICLE_YAW = ('--yaw-count', '5', '--yaw-noise', '1')


def without_yaw(rows):
    out = []
    for r in rows:
        if r['kind'] in YAW_KINDS:
            continue
        if r['kind'] == 'boot':
            r = {k: v for k, v in r.items() if k != 'pid'}   # the harness's process id
        if r['kind'] == 'log_digest':
            r = {k: v for k, v in r.items() if k not in YAW_DIGEST_KEYS}
            if isinstance(r.get('suppressed'), dict):
                r['suppressed'] = {k: v for k, v in r['suppressed'].items() if k not in YAW_KINDS}
        out.append(json.dumps(r, sort_keys=True))
    return out


@unittest.skipUnless(TOOL.exists(), 'build/log_rate not built')
class YawDataRows(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='mx5dr-yaw-rows-')
        cls.runs = {}
        scenarios = dict(drive=(3600, VEHICLE_YAW), stopgo=(3600, VEHICLE_YAW + ('--drive', 'stopgo')),
                         long=(14400, VEHICLE_YAW), stopgo_long=(14400, VEHICLE_YAW + ('--drive', 'stopgo')))
        for name, (seconds, extra) in scenarios.items():
            for state in ('on', 'off'):
                directory = os.path.join(cls.tmp.name, '%s-%s' % (name, state))
                cls.runs[(name, state)] = run('persistent', directory, seconds,
                                              extra=extra + ('--yaw-rows', state)) + (directory,)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_logic_and_other_rows_unchanged(self):
        for name in ('drive', 'stopgo', 'long', 'stopgo_long'):
            (on, on_rows, _), (off, off_rows, _) = self.runs[(name, 'on')], self.runs[(name, 'off')]
            self.assertEqual(evidence(on_rows), evidence(off_rows), name)
            self.assertEqual(without_yaw(on_rows), without_yaw(off_rows), name)
            self.assertFalse([r for r in off_rows if r['kind'] in YAW_KINDS or 'y1' in r])
            self.assertEqual(on['beta_state'], off['beta_state'])

    def test_rate_increase_within_budget(self):
        rates = {}
        for name in ('drive', 'stopgo', 'long'):
            on, off = self.runs[(name, 'on')][0], self.runs[(name, 'off')][0]
            added = on['bytes_per_s'] - off['bytes_per_s']
            rates[name] = (off['bytes_per_s'], on['bytes_per_s'], added)
            self.assertGreater(added, 10, name)       # the rows are really written
            self.assertLess(added, 60, name)          # hard cap of the design
        print('yaw rows log rate (before, after, added B/s): %s' % rates)
        self.assertLess(rates['drive'][1], 2048 * 0.97)
        self.assertLess(rates['long'][1], 2048 * 0.97)
        self.assertLess(rates['stopgo'][1], 2048)
        self.assertLess(rates['drive'][2], 35)

    def test_steady_state_and_fixed_burst(self):
        # A run's average is a steady rate plus a fixed burst (boot raw
        # period and the capture-stop RAW-window flush, about 830 KB), so
        # short runs read high (30 min: about 2190 B/s with or without the
        # yaw rows). Judge the 3 % margin on the steady state (from the 1 h
        # and 4 h runs) and require the burst unchanged by the yaw rows.
        found = {}
        for short, long_ in (('drive', 'long'), ('stopgo', 'stopgo_long')):
            for state in ('on', 'off'):
                a, b = self.runs[(short, state)][0], self.runs[(long_, state)][0]
                steady = (b['total_bytes'] - a['total_bytes']) / float(b['seconds'] - a['seconds'])
                burst = a['total_bytes'] - steady * a['seconds']
                found[(short, state)] = (round(steady, 1), round(burst))
            self.assertLess(found[(short, 'on')][0], 2048 * 0.97, short)
            self.assertLess(found[(short, 'on')][0] - found[(short, 'off')][0], 60, short)
            self.assertLess(abs(found[(short, 'on')][1] - found[(short, 'off')][1]), 5000, short)
        print('yaw rows steady B/s and fixed burst B: %s' % found)

    def test_rows_and_rate_limit(self):
        on_rows = self.runs[('stopgo', 'on')][1]
        stops = [r for r in on_rows if r['kind'] == 'yaw_stop']
        suppressed = sum(r.get('suppressed', {}).get('yaw_stop', 0) for r in on_rows if r['kind'] == 'log_digest')
        # One 2 s standstill every 8 s for an hour (450), rate limited to
        # a burst of 3 and one per 10 s; the rest counted in the digests.
        self.assertLessEqual(len(stops), 3 + 3600 // 10 + 1)
        self.assertGreater(suppressed, 50)
        self.assertGreaterEqual(len(stops) + suppressed, 440)
        drive = self.runs[('drive', 'on')][1]
        kinds = collections.Counter(r['kind'] for r in drive)
        self.assertEqual(kinds['yaw_stop'], 18)     # 3 standstills per 10 min cycle
        self.assertEqual(kinds['yaw_edge'], 13)     # NO_FIX return, 6 tunnel losses and returns
        self.assertEqual(kinds['yaw_reinit'], 0)
        digests = [r for r in drive if r['kind'] == 'log_digest']
        self.assertTrue(all(len(r['y1']) == 10 and len(r['y1n']) == 10 for r in digests))

    def test_analyzer_reads_the_rows(self):
        report, codes = analyze(os.path.join(self.runs[('drive', 'on')][2]))
        self.assertFalse([c for (_, c) in codes if c in ('malformed_yaw_row', 'malformed_digest_yaw')])
        yaw = report['yaw_data']
        self.assertEqual((yaw['summary']['stops'], yaw['summary']['edges']), (18, 13))
        # The synthetic drive integrates exactly zero 2048 with no GPS delay:
        # the digest-based fit finds it on the fitted windows at lag 0.
        fits = [f['lag_0_0']['zero'] for f in yaw['driving_zero_fits'] if f['lag_0_0']]
        self.assertGreater(len(fits), 20)
        self.assertLess(max(abs(z - 2048) for z in fits), 0.5)


if __name__ == '__main__':
    unittest.main()
