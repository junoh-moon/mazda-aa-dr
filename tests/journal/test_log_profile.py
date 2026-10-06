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


def run(profile, directory, seconds=3600, every=600):
    result = subprocess.run([str(TOOL), '--profile', profile, '--seconds', str(seconds),
                             '--event-every', str(every), '--out', directory],
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


if __name__ == '__main__':
    unittest.main()
