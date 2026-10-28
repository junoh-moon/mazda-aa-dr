#!/usr/bin/env python3
"""Offline, exploratory tunnel-entry alignment; never changes product rules.

Positions are receipt-time evidence, GPS is a reference rather than truth.
Only supported entries would use the map in this research candidate; both
contradicted and insufficient retain ordinary DR. See the dated protocol.
"""
import math

SOURCE_ORDER = ('fix1', 'position', 'collector')
HISTORY_S = 15.0
MAX_GAP_S = 2.5
MIN_BASELINE_M = 30.0
CORRIDOR_M = 15.0
HORIZON_M = 150.0


def classify(fixes, entry_s, map_s, map_length, point_at, losses=(), resets=()):
    """Causal trinary geometry from (receipt_s, east_m, north_m, source).

    point_at(s) supplies the real product map shape in the entry coordinate
    frame. No observed trajectory is rotated or freely translated to fit it.
    Cross discrepancies use an exploratory horizon, not GPS accuracy bounds.
    """
    if (not all(math.isfinite(v) for v in (entry_s, map_s, map_length))
            or map_length <= 0 or map_s < -60 or map_s > map_length):
        return dict(state='insufficient', reason='invalid_map_parameters')
    past = sorted(f for f in fixes if entry_s - HISTORY_S <= f[0] <= entry_s)
    source = next((s for s in SOURCE_ORDER if any(f[3] == s for f in past)), None)
    rows = []
    for f in past:
        if f[3] != source:
            continue
        if rows and f[0] - rows[-1][0] < .3:
            continue
        rows.append(f)
    result = dict(state='insufficient', reason='few_positions', source=source, n=len(rows))
    if len(rows) < 3:
        return result
    if not all(math.isfinite(v) for f in rows for v in f[:3]):
        result['reason'] = 'nonfinite'
        return result
    if entry_s - rows[-1][0] > 2 or rows[-1][0] - rows[0][0] < 2:
        result['reason'] = 'history_age_or_span'
        return result
    if any(b[0] - a[0] > MAX_GAP_S for a, b in zip(rows, rows[1:])):
        result['reason'] = 'history_gap'
        return result
    if any(rows[0][0] < t <= entry_s for t in losses):
        result['reason'] = 'history_mode_loss'
        return result
    if any(rows[0][0] < t <= entry_s for t in resets):
        result['reason'] = 'history_reset'
        return result
    dx, dy = rows[-1][1] - rows[0][1], rows[-1][2] - rows[0][2]
    baseline = math.hypot(dx, dy)
    result['baseline_m'] = round(baseline, 3)
    if baseline < MIN_BASELINE_M:
        result['reason'] = 'short_baseline'
        return result
    cumulative = [0.0]
    for a, b in zip(rows, rows[1:]):
        cumulative.append(cumulative[-1] + math.hypot(b[1] - a[1], b[2] - a[2]))
    arc = [map_s - (cumulative[-1] - d) for d in cumulative]
    bend = max(abs((f[1] - rows[0][1]) * dy - (f[2] - rows[0][2]) * dx) / baseline
               for f in rows)
    result['unmapped_approach'] = min(arc) < 0
    result['line_residual_m'] = round(bend, 3)
    if min(arc) < 0 and bend > CORRIDOR_M:
        result['reason'] = 'curved_unmapped_approach'
        return result
    horizon = min(HORIZON_M, max(MIN_BASELINE_M, map_length - map_s))
    cross = []
    for i, a in enumerate(rows):
        for j in range(i + 1, len(rows)):
            b = rows[j]
            ox, oy = b[1] - a[1], b[2] - a[2]
            if b[0] - a[0] < 1 or math.hypot(ox, oy) < MIN_BASELINE_M:
                continue
            p, q = point_at(arc[i]), point_at(arc[j])
            mx, my = q[0] - p[0], q[1] - p[1]
            norm = math.hypot(ox, oy) * math.hypot(mx, my)
            if not math.isfinite(norm) or norm <= 0:
                result['reason'] = 'invalid_map_chord'
                return result
            # Positive means observed motion is right of the map chord.
            if ox * mx + oy * my <= 0:
                result['reason'] = 'reversed_or_ambiguous_chord'
                return result
            cross.append(horizon * (ox * my - oy * mx) / norm)
    result['chords'] = len(cross)
    if len(cross) < 2:
        result['reason'] = 'few_chords'
        return result
    result['cross_range_m'] = [round(min(cross), 3), round(max(cross), 3)]
    if all(abs(x) <= CORRIDOR_M for x in cross):
        result.update(state='supported', reason='direction_supported')
    elif all(x > CORRIDOR_M for x in cross) or all(x < -CORRIDOR_M for x in cross):
        result.update(state='contradicted', reason='constant_direction_mismatch')
    else:
        result['reason'] = 'direction_disagreement'
    return result


def heading_seed(anchors, fix_s, decision_s=None):
    """Same fix's accepted heading, available by the separate decision time.

    drive_record_log.h stores fix1 time as p.t/MS (integer floor), while the
    anchor preserves o.mono_ns. Match that exact millisecond bucket; an anchor
    later than the decision is still unavailable, even in the same bucket.
    The bucket association is diagnostic evidence, not a full pipeline replay.
    """
    decision_s = fix_s if decision_s is None else decision_s
    bucket = int(round(fix_s * 1000))
    rows = [r for r in anchors if r.get('gate') == 'ACCEPTED' and r.get('h')
            and r['mono_ns'] // 1000000 == bucket and r['mono_ns'] / 1e9 <= decision_s]
    return ((rows[-1]['h'][1] / 10, 'logged_core_heading') if rows
            else (None, 'raw_fix_course'))


class NativeMap:
    """Temporary host DSO compiled from the actual product map header."""
    def __init__(self, path, expected_md5='56271d9c2d466d239ea81b840148fe6d'):
        import ctypes as C
        import hashlib
        import subprocess
        import tempfile
        from pathlib import Path
        path = Path(path)
        digest = hashlib.md5(path.read_bytes()).hexdigest()
        if expected_md5 is not None and digest != expected_md5:
            raise ValueError('map md5 does not match the frozen database')
        self.md5 = digest
        self.tmp = tempfile.TemporaryDirectory(prefix='mx5-entry-')
        root = Path(__file__).resolve().parents[1]
        library = Path(self.tmp.name) / 'entry.so'
        subprocess.run(['c++', '-std=c++11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-shared', '-fPIC', '-I' + str(root / 'src'),
                        str(root / 'tools/tunnel_entry_probe.cpp'), '-o', str(library)], check=True)
        self.lib = C.CDLL(str(library))
        self.lib.entry_load.argtypes = [C.c_char_p]
        self.lib.entry_load.restype = C.c_void_p
        self.lib.entry_free.argtypes = [C.c_void_p]
        self.lib.entry_match.argtypes = [C.c_void_p] + [C.c_double] * 5 + [C.POINTER(C.c_double)]
        self.lib.entry_match.restype = C.c_int
        self.lib.entry_point.argtypes = [C.c_void_p, C.c_uint] + [C.c_double] * 3 + [C.POINTER(C.c_double)]
        self.lib.entry_point.restype = C.c_int
        self.lib.entry_tangent.argtypes = [C.c_void_p, C.c_uint] + [C.c_double] * 3
        self.lib.entry_tangent.restype = C.c_double
        self.ptr = self.lib.entry_load(str(path).encode())
        if not self.ptr:
            self.tmp.cleanup()
            raise ValueError('product map loader rejected database')

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.lib.entry_free(self.ptr)
        self.tmp.cleanup()

    def match(self, lat, lon, heading, speed, age):
        import ctypes as C
        out = (C.c_double * 10)()
        if not self.lib.entry_match(self.ptr, lat, lon, heading, speed, age, out):
            return None
        return list(out)

    def point(self, match, s):
        import ctypes as C
        out = (C.c_double * 2)()
        if not self.lib.entry_point(self.ptr, int(match[0]), s, match[6], match[7], out):
            raise ValueError('product map point unavailable')
        return ((out[1] - match[9]) * match[7], (out[0] - match[8]) * match[6])

    def tangent(self, match, s):
        return self.lib.entry_tangent(self.ptr, int(match[0]), s, match[6], match[7])


def load_logs(paths):
    """Read explicit archives/files only; never recurse through a directory."""
    import json
    from pathlib import Path
    import map_aid_eval as M

    class Logs(M.LogSet):
        def __init__(self):
            super().__init__()
            self.anchors, self.fix_rows, self.resets = {}, {}, {}
            self.epochs = {}

        def _parse(self, stream):
            # Preserve lifecycle/fault observations omitted by the older map
            # evaluator. Unknown epochs remain unknown rather than continuous.
            rows = []
            for line in stream:
                try:
                    r = json.loads(line)
                except (ValueError, UnicodeDecodeError):
                    continue
                if isinstance(r, dict):
                    r.pop('request', None)
                    r.pop('lds_association', None)
                    rows.append(r)
            self.files += 1
            return rows

        def _row(self, boot, kind, row):
            super()._row(boot, kind, row)
            bid = boot.boot_id
            if kind == 'beta_anchor':
                self.anchors.setdefault(bid, {})[row['mono_ns']] = row
            elif kind == 'fix1' and len(row.get('d', [])) >= 12:
                self.fix_rows.setdefault(bid, {})[row['d'][0]] = row['d']
            elif kind == 'motion_batch':
                times = [e[2] / 1e9 for e in row.get('events', []) if len(e) > 2]
                if times:
                    self.epochs.setdefault(bid, {})[min(times)] = row.get('epoch')
            if kind in ('shadow_pipeline_reset', 'shadow_input_reset', 'shadow_session', 'shadow_bus',
                        'beta_cadence_fence', 'beta_journal_lag', 'rec_drop', 'yaw_reinit'):
                t = row.get('mono_ns')
                if isinstance(t, (int, float)):
                    self.resets.setdefault(bid, set()).add(t / 1e9)

        def finish(self):
            super().finish()
            for bid, epochs in self.epochs.items():
                prev = None
                for t, epoch in sorted(epochs.items()):
                    if prev is not None and prev != epoch:
                        self.resets.setdefault(bid, set()).add(t)
                    prev = epoch

    logs = Logs()
    for name in paths:
        if Path(name).is_dir():
            raise ValueError('supply explicit archive or JSONL paths; directory recursion is disabled')
        logs.read_path(str(name))
    logs.finish()
    return logs


def simulate(native, match, rows, start, heading, resets=()):
    """Approximate immediate hypothetical loss, 1 s motion sums, not Pipeline.

    Future GPS is used solely to score positions. Future yaw/wheel sums are
    the sensors a hypothetical loss would have observed, smeared at 50 ms.
    Any missing motion interval ends as inconclusive, never zero rotation.
    """
    out = dict(complete=False, reason='no_future_motion', elapsed_s=0.0,
               max_map_gps_m=0.0, max_dr_gps_m=0.0, map_gt30_dr_le15_s=0.0)
    if start + 1 >= len(rows):
        return out
    if any(rows[start][0] / 1000 < t <= rows[start + 1][0] / 1000 for t in resets):
        out['reason'] = 'recorded_boundary'
        return out
    first = rows[start]
    origin = first[2] / 1e7, first[3] / 1e7
    core = [0., 0.]
    tan = [0., 0.]
    core_h, s = heading, match[1]
    ref_h = native.tangent(match, s)
    for a, b in zip(rows[start:], rows[start + 1:]):
        dt = (b[0] - a[0]) / 1000.
        if any(a[0] / 1000 < t <= b[0] / 1000 for t in resets):
            out['reason'] = 'recorded_boundary'
            return out
        if out['elapsed_s'] + dt > 40:
            out.update(complete=True, reason='horizon_40s')
            return out
        if (dt <= 0 or dt > 2.5 or b[10] <= 0 or b[11] < 0
                or abs(b[8] / 1000. - dt) > .015):
            out['reason'] = 'missing_motion_interval'
            return out
        speed = b[11] / 36.
        omega = b[9] / b[10] * (-0.000658615 * 180 / math.pi)
        steps = max(1, int(math.ceil(dt / .05)))
        for _ in range(steps):
            step, ds = dt / steps, speed * dt / steps
            h = math.radians(core_h + .5 * omega * step)
            core[0] += ds * math.sin(h)
            core[1] += ds * math.cos(h)
            core_h += omega * step
            h = math.radians(native.tangent(match, s + .5 * ds))
            tan[0] += ds * math.sin(h)
            tan[1] += ds * math.cos(h)
            s += ds
            out['elapsed_s'] += step
            d = (core_h - heading) - (native.tangent(match, s) - ref_h)
            d = (d + 180) % 360 - 180
            chain_end = (int(match[5]) & 2 and s >= match[4]) or s > match[4] + 150
            if chain_end or abs(d) > 15:
                out.update(complete=True, reason='chain_end' if chain_end else 'yaw_gate',
                           release_jump_m=round(math.hypot(tan[0] - core[0], tan[1] - core[1]), 3))
                return out
        if b[1] in (1, 2):
            gx = (b[3] / 1e7 - origin[1]) * match[7]
            gy = (b[2] / 1e7 - origin[0]) * match[6]
            em, ec = math.hypot(tan[0] - gx, tan[1] - gy), math.hypot(core[0] - gx, core[1] - gy)
            out['max_map_gps_m'] = max(out['max_map_gps_m'], em)
            out['max_dr_gps_m'] = max(out['max_dr_gps_m'], ec)
            if em > 30 and ec <= 15:
                out['map_gt30_dr_le15_s'] += dt
    return out


def evaluate(paths, binary, scan_paths=(), expected_md5='56271d9c2d466d239ea81b840148fe6d'):
    """Anonymized aggregate/episode output; caller may save it privately.

    Entry geometry is the real product helper, but mode/core/session gates
    outside that helper are not replayed: this is explicitly not deployment
    evidence. Older archives without accepted heading metadata remain labeled.
    """
    import collections
    import map_aid_eval as M
    logs = load_logs(paths)
    scan = load_logs(scan_paths) if scan_paths else None
    result = dict(scope='exploratory_offline_geometry_and_approximate_motion_not_product_replay',
                  episodes=[], virtual=[], summary={})
    with NativeMap(binary, expected_md5) as native:
        result['map_md5'] = native.md5

        def classify_entry(dataset, bid, fix, decision, age):
            boot = dataset.boots[bid]
            anchors = sorted(dataset.anchors.get(bid, {}).values(), key=lambda r: r['mono_ns'])
            h, provenance = heading_seed(anchors, fix[0], decision)
            if h is None:
                h = fix[3]
            match = native.match(fix[1], fix[2], h, fix[4], age)
            base = dict(heading_source=provenance, entry_kmh=round(fix[4], 3),
                        entry_age_s=round(age, 3), matched=match is not None)
            if match is None:
                base.update(state='not_matched', reason='device_map_gate')
                return base, None
            fixes = [(f[0], (f[2] - fix[2]) * match[7], (f[1] - fix[1]) * match[6], f[6])
                     for f in boot.fix_list if fix[0] - HISTORY_S <= f[0] <= fix[0]]
            losses = [t for t, mode in boot.mode_list if mode == 0]
            resets = sorted(dataset.resets.get(bid, set()))
            base.update(classify(fixes, fix[0], match[1], match[4],
                                 lambda s: native.point(match, s), losses, resets))
            base.update(chain_length_m=round(match[4], 3), map_heading_difference_deg=round(match[3], 3))
            return base, match

        for boot_num, (bid, boot) in enumerate(sorted(logs.boots.items())):
            for ep in M.episodes(boot):
                pre = ep.get('pre')
                if pre is None:
                    continue
                row, _ = classify_entry(logs, bid, pre, ep['t0'], ep['t0'] - pre[0])
                row.update(boot=boot_num, loss_s=round(ep['t0'], 3), duration_s=round(ep['dur'], 3),
                           returned=ep['ret'] is not None)
                result['episodes'].append(row)
        if scan:
            for boot_num, (bid, boot) in enumerate(sorted(scan.boots.items())):
                motion = sorted(scan.fix_rows.get(bid, {}).values(), key=lambda d: d[0])
                by_ms = {d[0]: i for i, d in enumerate(motion)}
                anchors = scan.anchors.get(bid, {})
                available = {r['mono_ns'] // 1000000: r['mono_ns'] / 1e9
                             for r in anchors.values() if r.get('gate') == 'ACCEPTED' and r.get('h')}
                for fix in boot.fix_list:
                    if fix[6] != 'fix1' or fix[4] < 10:
                        continue
                    ms = int(round(fix[0] * 1000))
                    # Hypothetical loss immediately after this observed fix:
                    # same-fix metadata is available by its exact receipt time.
                    decision = max(fix[0], available.get(ms, fix[0]))
                    row, match = classify_entry(scan, bid, fix, decision, 0.)
                    if match is None:
                        continue
                    row.update(boot=boot_num, start_s=round(fix[0], 3),
                               loss_model='immediate_after_observed_fix')
                    if ms in by_ms:
                        h, _ = heading_seed(sorted(anchors.values(), key=lambda r: r['mono_ns']), fix[0], decision)
                        row['dynamic'] = simulate(native, match, motion, by_ms[ms], fix[3] if h is None else h,
                                                  sorted(scan.resets.get(bid, set())))
                    result['virtual'].append(row)
        result['summary']['episodes'] = dict(collections.Counter(r['state'] for r in result['episodes']))
        result['summary']['virtual'] = dict(collections.Counter(r['state'] for r in result['virtual']))
        result['summary']['virtual_complete'] = sum(bool(r.get('dynamic', {}).get('complete'))
                                                    for r in result['virtual'])
        result['summary']['virtual_supported_danger'] = sum(
            r['state'] == 'supported' and r.get('dynamic', {}).get('map_gt30_dr_le15_s', 0) >= 5
            for r in result['virtual'])
        result['summary']['reasons'] = dict(collections.Counter(r['reason'] for r in result['virtual']))
    return result


def main(argv=None):
    import argparse
    import json
    from pathlib import Path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--logs', nargs='+', required=True, help='explicit log archive/file paths; no directories')
    parser.add_argument('--bin', required=True, help='pinned tunnels-v1.bin')
    parser.add_argument('--scan-logs', nargs='*', default=[], help='explicit archives for virtual immediate losses')
    parser.add_argument('--out', required=True, help='private anonymized result JSON')
    args = parser.parse_args(argv)
    result = evaluate(args.logs, args.bin, args.scan_logs)
    Path(args.out).write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    print(json.dumps(result['summary'], indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
