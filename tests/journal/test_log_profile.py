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
import re
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
# Record-only drive data rows (validation/RELEASE_V100_BETA14_CANDIDATE_2026-10-13.md).
DRIVE_KINDS = ('fix1', 'exit_jump', 'yaw_learn', 'rec_drop')
DRIVE_BUDGET_B_S = 600000 / 3600.0
# The yaw and chan row classes below measure the profile before the drive rows.
NO_DRIVE_ROWS = ('--record-rows', 'off')


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
        # The record-only drive data rows (2026-10-13, DriveRows below) have
        # their own budget on top: at most 600 KB per hour.
        self.assertGreater(self.full['bytes_per_s'], 20000)
        drive = sum(self.quiet['kinds'].get(k, [0, 0])[1] for k in DRIVE_KINDS) / float(self.quiet['seconds'])
        self.assertGreater(drive, 0)
        self.assertLess(drive, DRIVE_BUDGET_B_S)
        self.assertLess(self.quiet['bytes_per_s'] - drive, 2048)
        # At least 3 % margin below the budget with the yaw data rows
        # (validation/YAW_DATA_COLLECTION_2026-10-09.md; 1942.0 B/s before).
        self.assertLess(self.quiet['bytes_per_s'] - drive, 2048 * 0.97)
        self.assertLess(self.quiet['bytes_per_s'], 2048 + DRIVE_BUDGET_B_S)
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
                                              extra=extra + ('--yaw-rows', state) + NO_DRIVE_ROWS) + (directory,)

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


# VIM side-channel rows (2026-10-10, src/runtime/chan_digest_log.h,
# validation/VIM_CHANNEL_CAPTURE_2026-10-10.md): logging only. With the
# synthetic side channel off (--chan off) the profile is the previous one;
# with it on, chan_digest rows are added and every other row is unchanged.
def without_chan(rows):
    out = []
    for r in rows:
        if r['kind'] == 'chan_digest':
            continue
        if r['kind'] == 'boot':
            r = {k: v for k, v in r.items() if k != 'pid'}
        out.append(json.dumps(r, sort_keys=True))
    return out


@unittest.skipUnless(TOOL.exists(), 'build/log_rate not built')
class ChanRows(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='mx5dr-chan-rows-')
        cls.runs = {}
        scenarios = dict(drive=(3600, VEHICLE_YAW), stopgo=(3600, VEHICLE_YAW + ('--drive', 'stopgo')),
                         long=(14400, VEHICLE_YAW))
        for name, (seconds, extra) in scenarios.items():
            for state in ('on', 'off'):
                directory = os.path.join(cls.tmp.name, '%s-%s' % (name, state))
                cls.runs[(name, state)] = run('persistent', directory, seconds,
                                              extra=extra + ('--chan', state) + NO_DRIVE_ROWS) + (directory,)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_logic_and_other_rows_unchanged(self):
        for name in ('drive', 'stopgo', 'long'):
            (on, on_rows, _), (off, off_rows, _) = self.runs[(name, 'on')], self.runs[(name, 'off')]
            self.assertEqual(evidence(on_rows), evidence(off_rows), name)
            self.assertEqual(without_chan(on_rows), without_chan(off_rows), name)
            self.assertFalse([r for r in off_rows if r['kind'] == 'chan_digest'])
            self.assertTrue([r for r in on_rows if r['kind'] == 'chan_digest'])
            self.assertEqual(on['beta_state'], off['beta_state'])

    def test_rate_within_budget(self):
        rates = {}
        for name in ('drive', 'stopgo', 'long'):
            on, off = self.runs[(name, 'on')][0], self.runs[(name, 'off')][0]
            rates[name] = (off['bytes_per_s'], on['bytes_per_s'], round(on['bytes_per_s'] - off['bytes_per_s'], 1))
            self.assertGreater(rates[name][2], 5, name)    # the rows are really written
            self.assertLess(rates[name][2], 15, name)      # design: ~10 B/s (20 s cadence)
        print('chan rows log rate (before, after, added B/s): %s' % rates)
        self.assertLess(rates['drive'][1], 2048 * 0.97)
        self.assertLess(rates['long'][1], 2048 * 0.97)
        self.assertLess(rates['stopgo'][1], 2048)
        found = {}
        for state in ('on', 'off'):
            a, b = self.runs[('drive', state)][0], self.runs[('long', state)][0]
            steady = (b['total_bytes'] - a['total_bytes']) / float(b['seconds'] - a['seconds'])
            found[state] = (round(steady, 1), round(a['total_bytes'] - steady * a['seconds']))
        print('chan rows steady B/s and fixed burst B: %s' % found)
        self.assertLess(found['on'][0], 2048 * 0.97)
        self.assertLess(abs(found['on'][1] - found['off'][1]), 5000)

    def test_rows(self):
        rows = [r for r in self.runs[('drive', 'on')][1] if r['kind'] == 'chan_digest']
        self.assertEqual(len(rows), 180)                  # one per 20 s
        times = [r['mono_ns'] for r in rows]
        self.assertLessEqual(max(b - a for a, b in zip(times, times[1:])), 20.06e9)
        self.assertTrue(all(audit.chan_row_valid(r) for r in rows))
        self.assertTrue(all(len(json.dumps(r, separators=(',', ':'))) < 400 for r in rows))
        self.assertTrue(all('bad' not in r and 'lost' not in r and 'wrap' not in r for r in rows))
        self.assertEqual([r.get('lost0') for r in rows[:2]], [0, None])   # baseline once
        self.assertEqual(sum(r['n'][0] for r in rows), 36000)   # every synthetic 0x116 (10 Hz)

    def test_analyzer_reads_the_rows(self):
        report, codes = analyze(self.runs[('drive', 'on')][2])
        self.assertFalse([c for (_, c) in codes if c == 'malformed_chan_row'])
        chan = report['chan_data']
        self.assertEqual(chan['summary']['rows'], 180)
        self.assertEqual(chan['summary']['reached'], {'0x116': True, '0x169': True, '0x15B': True})
        self.assertEqual(chan['summary']['qf_values'], {'0x116_a': [3], '0x116_b': [3], '0x169': [3]})
        # Authored zero 4096: the stationary means find it.
        self.assertAlmostEqual(chan['summary']['ay_stationary_mean'], 4096, delta=0.5)
        self.assertAlmostEqual(chan['summary']['ax_stationary_mean'], 4096, delta=1.0)
        self.assertEqual(len(chan['per_minute']), 61)
        # One table entry per yaw_stop row, with stationary acceleration means.
        self.assertEqual(len(chan['standstills']), 18)
        self.assertTrue(all(s['ay_stationary'] is not None for s in chan['standstills']))
        # Straight stretches: the synthetic drive integrates exactly zero 2048.
        straight = chan['straight_stretches']
        self.assertGreater(len(straight), 10)
        self.assertTrue(all(s['min_kmh'] > 30 for s in straight))
        self.assertLess(max(abs(s['yaw_excess_counts']) for s in straight if s['yaw_excess_counts'] is not None), 0.5)
        _, off_codes = analyze(self.runs[('drive', 'off')][2])
        self.assertEqual({c for (_, c) in codes}, {c for (_, c) in off_codes})

    def test_analyzer_rejects_malformed_rows(self):
        good = dict(kind='chan_digest', schema=1, mono_ns=20000000000, n=[200, 200, 3],
                    ax=[4000, 4100, 4050.5, 4001.0, 20], bp=[0, 300, 4], ay=[4090, 4100, 4095.0, None, 0],
                    q=[8, 10, 8], v=[4000, 2, 3], rpm=[2400, 2])
        self.assertTrue(audit.chan_row_valid(good))
        self.assertTrue(audit.chan_row_valid(dict(good, ax=None, bp=None, ay=None, v=None, rpm=None,
                                                  bad=[1, 0, 0, 2], lost=4, lost0=0, wrap=3, future='tolerated')))
        # Wrap hint from the flag or from min < 200 and max > 7900.
        self.assertEqual(audit.chan_wrap(dict(good, ay=[100, 8000, 4000.0, None, 0])), 2)
        self.assertEqual(audit.chan_wrap(dict(good, wrap=1)), 1)
        self.assertEqual(audit.chan_wrap(good), 0)
        # Per boot: worker listened, motion events, >= 40 s of digests, no rows.
        long_boot = {1: [0, 60 * 10**9, 500]}
        silent = audit.chan_report([], [], [], [(1, 'open')], long_boot)
        self.assertTrue(silent['summary']['tap_silent'])
        self.assertEqual(silent['summary']['tap_silent_sessions'], [1])
        self.assertFalse(audit.chan_report([], [], [], [(1, 'disabled')], long_boot)['summary']['tap_silent'])
        self.assertFalse(audit.chan_report([], [], [], [(1, 'open')], {1: [0, 30 * 10**9, 500]})['summary']['tap_silent'])
        self.assertFalse(audit.chan_report([], [], [], [(1, 'open')], {1: [0, 60 * 10**9, 0]})['summary']['tap_silent'])
        self.assertFalse(audit.chan_report([], [], [], [(1, 'open')], None)['summary']['tap_silent'])
        # Another boot with rows does not hide a silent boot.
        row = dict(good, mono_ns=20 * 10**9)
        both = audit.chan_report([(2, row)], [], [], [(1, 'open'), (2, 'open')], {1: long_boot[1], 2: [0, 60 * 10**9, 9]})
        self.assertEqual(both['summary']['tap_silent_sessions'], [1])
        for change in (dict(q=[16, 8, 8]), dict(n=[1, 2]), dict(ax=[4100, 4000, 4050.0, None, 0]),
                       dict(ax=[0, 9000, 1.0, None, 0]), dict(ay=[0, 1, 0.5, 1.0, 0]), dict(ay=[0, 1, 0.5, None, 3]),
                       dict(v=None), dict(rpm=[9000, 0]), dict(v=[1, 0, 4]), dict(bad=[0, 0, 0]), dict(lost=0),
                       dict(wrap=0), dict(wrap=4), dict(lost0=-1),
                       dict(schema=2), dict(bp=[5, 1, 0])):
            self.assertFalse(audit.chan_row_valid(dict(good, **change)), change)
        a = audit.Auditor()
        a.consume(dict(good, q=[99, 0, 0]), 'synthetic:1')
        self.assertIn('malformed_chan_row', [i['code'] for i in a.report()['issues']])



# G3 first-connection bearing send (validation/G3_SEND_2026-10-13.md).
G3_SEND_KINDS = ('g3_send', 'g3_send_conflict')
BEARING_BYTES = (40, 44, 45, 46, 47)
# Digest send counters move between "location" (RAW context) and "changed"
# (kept) when a stock FIX send is overlaid; raw_direct_rows follows.
DIGEST_SEND_KEYS = ('send_location', 'send_changed', 'raw_direct_rows')


def g3_lines(directory):
    lines = []
    for index in (2, 1, 0):
        path = Path(directory) / 'logs' / ('trace.%d.jsonl' % index)
        if path.exists():
            lines += path.read_text().splitlines()
    return lines


def fold_digest(row):
    total = 0

    def fold(d):
        nonlocal total
        for k in list(d):
            if isinstance(d[k], dict):
                fold(d[k])
            elif k in DIGEST_SEND_KEYS[:2]:
                total += d.pop(k)
            elif k == DIGEST_SEND_KEYS[2]:
                d.pop(k)
    fold(row)
    row['send_location_or_changed'] = total


def without_g3_send(lines, overlaid=None):
    """The journal with the bearing send removed: no g3_send rows; every
    overlaid SEND row turned back into the stock row it replaced (choice 0,
    NOT_UNKNOWN, outgoing = original) after checking that only hasBearing (1)
    and bearing changed; a FIX send inside the window that the overlay did not
    take (candidate not yet published) back to NOT_UNKNOWN; the digest send
    counters folded. overlaid: collects (call, generation, bearing_e6)."""
    out = []
    for line in lines:
        row = json.loads(line)
        if row['kind'] in G3_SEND_KINDS:
            continue
        if row['kind'] == 'boot':
            row.pop('pid', None)
        if row['kind'] == 'send' and row.get('type') == 1 and row.get('class') == 2:
            if row['choice'] == 6:
                original, outgoing = bytes.fromhex(row['original_hex']), bytes.fromhex(row['outgoing_hex'])
                changed = [i for i in range(48) if original[i] != outgoing[i]]
                assert all(i in BEARING_BYTES for i in changed) and outgoing[40] == 1, changed
                bearing = int.from_bytes(outgoing[44:48], 'little', signed=True)
                assert 10000 < bearing <= 359990000, bearing
                if overlaid is not None:
                    overlaid.append((row['call'], row['generation'], bearing))
                row.update(choice=0, reason=7, outgoing_hex=row['original_hex'])
            elif row['choice'] == 0 and row['reason'] != 7:
                row['reason'] = 7
        if row['kind'] == 'log_digest':
            fold_digest(row)
        out.append(json.dumps(row, sort_keys=True))
    return out


@unittest.skipUnless(TOOL.exists(), 'build/log_rate not built')
class G3SendRows(unittest.TestCase):
    """The product journal path with the G3 tracker restored CARRIED: the
    bearing send on and off (the beta.14 behaviour) on the same synthetic
    drive (parked at the start, then away; GPS outages every 10 minutes)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='mx5dr-g3-send-')
        cls.runs = {}
        for profile in ('full', 'persistent'):
            for state in ('on', 'off'):
                directory = os.path.join(cls.tmp.name, '%s-%s' % (profile, state))
                cls.runs[(profile, state)] = run(profile, directory, 1300,
                                                 extra=('--nofix', '0', '--g3', 'on', '--g3-send', state)) + (directory,)
        directory = os.path.join(cls.tmp.name, 'conflict')
        cls.runs['conflict'] = run('full', directory, 120, extra=('--nofix', '0', '--g3', 'on', '--g3-send', 'on',
                                                                  '--g3-heading', '180')) + (directory,)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_off_and_on_differ_only_in_bearing(self):
        for profile in ('full', 'persistent'):
            on, off = self.runs[(profile, 'on')], self.runs[(profile, 'off')]
            overlaid = []
            on_lines, off_lines = g3_lines(on[2]), g3_lines(off[2])
            self.assertEqual(without_g3_send(on_lines, overlaid), without_g3_send(off_lines), profile)
            # Off: no window, no row, no overlaid send (the stock bytes, byte for byte).
            self.assertFalse([l for l in off_lines if '"kind":"g3_send' in l or '"choice":6,' in l], profile)
            self.assertGreaterEqual(len(overlaid), 5, profile)
            self.assertEqual({b for _, _, b in overlaid}, {90000000}, profile)   # carried 90.0, straight
            self.assertEqual((on[0]['beta_state'], on[0]['outages']), (off[0]['beta_state'], off[0]['outages']))
            self.assertEqual(evidence([r for r in on[1] if r['kind'] != 'send']),
                             evidence([r for r in off[1] if r['kind'] != 'send']), profile)

    def test_window_ends_and_never_rearms(self):
        for key in (('full', 'on'), ('persistent', 'on'), 'conflict'):
            rows = self.runs[key][1]
            send = [r for r in rows if r['kind'] in G3_SEND_KINDS]
            starts = [r for r in send if r.get('event') == 'start']
            ends = [r for r in send if r.get('event') == 'end']
            self.assertEqual((len(starts), len(ends)), (1, 1), key)
            reason = 'conflict' if key == 'conflict' else 'trusted'
            self.assertEqual((ends[0]['reason'], ends[0]['why']), (reason, 'stock_course'), key)
            self.assertEqual(len([r for r in send if r['kind'] == 'g3_send_conflict']), int(key == 'conflict'))
            # Only the call of the fix that ended the window (decided before the
            # worker saw that fix) may follow the end row; no later call.
            end_at = rows.index(ends[0])
            last_position = max(r['call'] for r in rows[:end_at] if r['kind'] == 'position')
            overlaid = [r for r in rows if r['kind'] == 'send' and r['choice'] == 6]
            self.assertTrue(overlaid and max(r['call'] for r in overlaid) <= last_position, key)
            self.assertEqual(ends[0]['sent'] + 1, len(overlaid), key)

    def test_analyzer_accepts_the_send(self):
        for profile in ('full', 'persistent'):
            report, found = analyze(self.runs[(profile, 'on')][2])
            violations = [code for (violation, code), n in found.items() if violation]
            self.assertEqual(violations, [], profile)
            self.assertFalse([code for (_, code) in found if code.startswith('g3_bearing')], profile)
            send = report['g3']['send']
            self.assertEqual((send['windows'], send['end_reasons']), (1, {'trusted': 1}), profile)
            self.assertGreaterEqual(send['overlaid_sends'], 5, profile)


if __name__ == '__main__':
    unittest.main()


# Drive data rows (2026-10-13, src/runtime/drive_record_log.h,
# validation/RELEASE_V100_BETA14_CANDIDATE_2026-10-13.md): record only. With
# them off the profile is the previous one; with them on, every other row is
# byte for byte the same and the added rate stays below 600 KB per hour.
def without_drive(rows):
    out = []
    for r in rows:
        if r['kind'] in DRIVE_KINDS:
            continue
        if r['kind'] == 'boot':
            r = {k: v for k, v in r.items() if k != 'pid'}
        out.append(json.dumps(r, sort_keys=True))
    return out


def raw_without_drive(directory):
    lines = []
    for index in (2, 1, 0):
        path = Path(directory) / 'logs' / ('trace.%d.jsonl' % index)
        if path.exists():
            for line in path.read_bytes().splitlines():
                if any(line.startswith(b'{"kind":"%s"' % k.encode()) for k in DRIVE_KINDS):
                    continue
                if line.startswith(b'{"kind":"boot"'):
                    line = re.sub(rb'"pid":[0-9]+', b'"pid":0', line)
                lines.append(line)
    return lines


@unittest.skipUnless(TOOL.exists(), 'build/log_rate not built')
class DriveRows(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='mx5dr-drive-rows-')
        cls.runs = {}
        scenarios = dict(drive=(3600, VEHICLE_YAW), stopgo=(3600, VEHICLE_YAW + ('--drive', 'stopgo')))
        for name, (seconds, extra) in scenarios.items():
            for state in ('on', 'off'):
                directory = os.path.join(cls.tmp.name, '%s-%s' % (name, state))
                cls.runs[(name, state)] = run('persistent', directory, seconds,
                                              extra=extra + ('--record-rows', state)) + (directory,)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_logic_and_other_rows_unchanged(self):
        for name in ('drive', 'stopgo'):
            (on, on_rows, on_dir), (off, off_rows, off_dir) = self.runs[(name, 'on')], self.runs[(name, 'off')]
            self.assertEqual(evidence(on_rows), evidence(off_rows), name)
            self.assertEqual(without_drive(on_rows), without_drive(off_rows), name)
            # Byte for byte: the journal lines without the drive kinds (the
            # boot row's pid is the harness process id).
            self.assertEqual(raw_without_drive(on_dir), raw_without_drive(off_dir), name)
            self.assertFalse([r for r in off_rows if r['kind'] in DRIVE_KINDS])
            self.assertEqual(on['beta_state'], off['beta_state'])
            self.assertEqual(on['outages'], off['outages'])

    def test_rate_within_budget(self):
        rates = {}
        for name in ('drive', 'stopgo'):
            on, off = self.runs[(name, 'on')][0], self.runs[(name, 'off')][0]
            added = on['bytes_per_s'] - off['bytes_per_s']
            rates[name] = (off['bytes_per_s'], on['bytes_per_s'], round(added, 1), round(added * 3.6, 1))
            self.assertGreater(added, 20, name)                  # the rows are really written
            self.assertLess(added, DRIVE_BUDGET_B_S, name)       # 600 KB per hour
        print('drive rows log rate (before, after, added B/s, added KB/h): %s' % rates)

    def test_rows(self):
        on = self.runs[('drive', 'on')][1]
        kinds = collections.Counter(r['kind'] for r in on)
        # At most one fix1 per second, only while moving (> 5 km/h).
        self.assertGreater(kinds['fix1'], 2500)
        self.assertLessEqual(kinds['fix1'], 3600)
        fix1 = [r['d'] for r in on if r['kind'] == 'fix1']
        self.assertTrue(all(audit.drive_row_valid(r) for r in on if r['kind'] in DRIVE_KINDS))
        self.assertTrue(all(d[4] > 5 / 3.6 * 1000 or d[11] > 50 for d in fix1))
        self.assertTrue(all(len(json.dumps(dict(kind='fix1', d=d), separators=(',', ':'))) < 120 for d in fix1))
        # One exit_jump per GPS outage of the synthetic drive, each after a replacement.
        outages = self.runs[('drive', 'on')][0]['outages']
        exits = [r for r in on if r['kind'] == 'exit_jump']
        self.assertEqual(len(exits), outages)
        # The real runtime path: each synthetic outage is replaced (BETA) and ends with a fix.
        self.assertTrue(all(r['reps'] > 10 and r['sent_choice'] == 3 and 900 <= r['dt_ms'] <= 1100 and
                            r['jump_m'] < 50 for r in exits))
        # yaw_learn: one row per GPS loss with the synthetic zero (2048), the rest every 60 s.
        lost = [r for r in on if r['kind'] == 'yaw_learn' and r['ev'] == 'lost']
        self.assertEqual(len(lost), outages)
        self.assertTrue(all(r['z'] is not None and abs(r['z']) <= 20 and r['used'] == 0 for r in lost))
        self.assertEqual(kinds['rec_drop'], 0)                   # no writer backlog in this harness
        # stop-and-go at <= 12 km/h: fix1 only while moving, no learned zero (< 10 km/h most of the time).
        stopgo = collections.Counter(r['kind'] for r in self.runs[('stopgo', 'on')][1])
        self.assertLess(stopgo['fix1'], 3600)

    def test_analyzer_reads_the_rows(self):
        report, codes = analyze(os.path.join(self.runs[('drive', 'on')][2]))
        self.assertFalse([c for (_, c) in codes if c in ('malformed_drive_row', 'unknown_record_kind')])
        drive = report['drive_data']
        outages = self.runs[('drive', 'on')][0]['outages']
        self.assertEqual((drive['summary']['exits'], drive['summary']['lost_rows']), (outages, outages))
        self.assertEqual(drive['summary']['fix1_rows'],
                         collections.Counter(r['kind'] for r in self.runs[('drive', 'on')][1])['fix1'])
        self.assertTrue(all(abs(e['zero'] - 2048) < 0.2 for e in drive['learn'] if e['zero'] is not None))

class G3GarageRows(unittest.TestCase):
    """The garage carry rows (validation/G3_GARAGE_EXIT_FIX_2026-10-17.md) on
    the product journal path: with the switch on the tracker's parked records
    and departure marks are journaled (in-memory store); every non-G3 row
    stays the same and the added rate stays far below 600 KB per hour."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='mx5dr-g3-garage-')
        cls.runs = {}
        for state in ('on', 'off'):
            directory = os.path.join(cls.tmp.name, state)
            cls.runs[state] = run('persistent', directory, 3600,
                                  extra=('--nofix', '0', '--g3', 'on', '--g3-send', 'on', '--g3-garage', state)) + (directory,)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_only_g3_rows_change_and_the_rate_is_bounded(self):
        on, off = self.runs['on'], self.runs['off']
        self.assertEqual(evidence([r for r in on[1] if r['kind'] != 'send']),
                         evidence([r for r in off[1] if r['kind'] != 'send']))
        kinds = lambda rows: collections.Counter(r['kind'] for r in rows if not r['kind'].startswith('g3_'))
        self.assertEqual(kinds(on[1]), kinds(off[1]))
        self.assertEqual((on[0]['beta_state'], on[0]['outages']), (off[0]['beta_state'], off[0]['outages']))
        written = [r for r in on[1] if r['kind'] == 'g3_store' and r.get('event') == 'written']
        marks = [r for r in written if r['st'] == 'M']
        self.assertGreater(len(written), 0)
        self.assertGreater(len(marks), 0)
        self.assertFalse([r for r in written if r['st'] == 'M' and r['hv'] != 0])
        size = lambda rows: sum(len(json.dumps(r, separators=(',', ':'))) + 1 for r in rows if r['kind'].startswith('g3_'))
        added = (size(on[1]) - size(off[1])) / float(on[0]['seconds'])
        self.assertLess(added, DRIVE_BUDGET_B_S / 10.0)        # < 60 KB per hour on this drive
        print('g3 garage rows: %d writes (%d marks), added %.1f B/s (%.1f KB/h)' %
              (len(written), len(marks), added, added * 3.6))
