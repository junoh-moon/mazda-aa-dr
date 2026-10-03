#!/usr/bin/env python3
"""Run replay variants and summarise GyroBias / DR budget / zero sensitivity / proxy headings.
Usage: python3 analyze.py <dir containing replay and windows.csv>
All anchors are SYNTHETIC; inter-sensor headings are disagreements, not accuracy."""
import subprocess, sys, io, os
import numpy as np
W = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
SC = 0.000658615  # rad/s per count (|research profile|)
STATE = {0:'UNSEEDED',1:'READY',2:'ACTIVE',3:'REACQ',4:'NATIVE',5:'INVALID',6:'LIMIT'}

def run(*args):
    out = subprocess.run([os.path.join(W, 'replay'), os.path.join(W, 'windows.csv')] + [str(a) for a in args],
                         check=True, capture_output=True, text=True).stdout
    return np.genfromtxt(io.StringIO(out), delimiter=',', names=True)

w = np.genfromtxt(os.path.join(W, 'windows.csv'), delimiter=',', names=True)
N = len(w)
fm = np.floor(w['yaw_sum'] / w['yaw_count'])          # what the Pipeline integrates
wk = np.stack([w['w%d' % i] for i in (1, 2, 3, 4)], 1) * 0.01 - 100
mps = wk / 3.6
stopped = (mps <= 0.05).all(1)
spd = mps.mean(1)

def runs(mask):
    ch = np.flatnonzero(np.diff(mask.astype(int))) + 1
    b = np.r_[0, ch]; e = np.r_[ch, len(mask)]
    return [(s, t) for s, t in zip(b, e) if mask[s]]
stops = runs(stopped)

auto = run('--auto-bias', 1)
nom = run('--auto-bias', 0, '--zero', 2047)
anchors = [int(k) for k in np.flatnonzero(auto['anchor'] == 2)]  # seed window (2nd fix)

print('== 0. input sanity ==')
print('windows %d (%.1f s synthetic); yaw count per window always %s; max yaw_sum %d (u16 wrap needs >65535)'
      % (N, N / 10, np.unique(w['yaw_count']), w['yaw_sum'].max()))
print('floor(sum/count) - float mean: mean %.3f counts (min %.2f max %.2f)'
      % ((fm - w['yaw12_mean']).mean(), (fm - w['yaw12_mean']).min(), (fm - w['yaw12_mean']).max()))
for c in ['r_wheel', 'r_rev', 'r_yaw', 'r_pos', 'r_drain']:
    u, n = np.unique(auto[c], return_counts=True)
    print('  auto %-8s results %s' % (c, dict(zip(u.astype(int).tolist(), n.tolist()))))
print('  auto resets %d rejected %d events %d intervals %d' % (auto['resets'][-1], auto['rejected'][-1], auto['events'][-1], auto['intervals'][-1]))
u, n = np.unique(auto['d_result'], return_counts=True); print('  diagnostic results', dict(zip(u.astype(int).tolist(), n.tolist())))

print('\n== 1. stationary periods (all 4 wheels <= 0.05 m/s; floored window means) ==')
print('stop  k0    k1    dur_s  mean_fm   std_fm  mean_float std_float  min max  rcm_mean_dps  long_G  lat_G (diag only)')
truth = []
for i, (s, t) in enumerate(stops):
    x = fm[s:t]; y = w['yaw12_mean'][s:t]
    truth.append(x.mean())
    print('S%d  %5d %5d %6.1f  %8.3f %7.3f  %9.3f %7.3f   %d %d  %+.4f      %+.3f  %+.3f'
          % (i + 1, s, t, (t - s) / 10, x.mean(), x.std(), y.mean(), y.std(), x.min(), x.max(), w['rcm_dps_left'][s:t].mean(),
             (w['long13'][s:t].mean() - 3991) / 961, (w['lat13'][s:t].mean() - 3988) / 782))
# robust: excluding windows deviating > 4 counts from median
print('  (robust mean excluding |fm-median|>4: %s)' % ', '.join('%.3f' % fm[s:t][np.abs(fm[s:t] - np.median(fm[s:t])) <= 4].mean() for s, t in stops))

print('\n== 2. GyroBias on real data (auto_bias=true run) ==')
print('stop  READY_at_s  invalidations(k:reason)  final_cand  n   var    ev_end_s  next_stop_mean  err_cnt  err_dps')
def reason(seq):
    # replay GyroBias accumulation to label the invalidation reason
    n = 0; lo = hi = mean = m2 = 0
    for v in seq:
        if n == 0: lo = hi = mean = v; m2 = 0; n = 1; continue
        l, h = min(lo, v), max(hi, v); d = v - mean; nm = mean + d / (n + 1); nm2 = m2 + d * (v - nm)
        if h - l > 4: return 'spread %d..%d' % (l, h)
        if nm2 / (n + 1) > 1.0: return 'var %.2f' % (nm2 / (n + 1))
        lo, hi, mean, m2, n = l, h, nm, nm2, n + 1
    return '?'
for i, (s, t) in enumerate(stops):
    st = auto['gb_state'][s:t]; rd = auto['gb_ready'][s:t]
    ready = np.flatnonzero(rd == 1)
    inv = []
    for j in range(s + 1, t):
        if auto['gb_ready'][j - 1] == 1 and auto['gb_ready'][j] == 0 and auto['stopped'][j] == 1:
            inv.append(j)
        elif auto['gb_state'][j - 1] == 2 and auto['gb_state'][j] in (1, 4) and auto['gb_ready'][j] == 0:
            inv.append(j)
    labels = []
    for j in inv:
        # start of the collection that was invalidated: previous transition into COLLECTING
        k0 = j - 1
        while k0 > s and auto['gb_state'][k0 - 1] in (2, 3): k0 -= 1
        labels.append('%d:%s' % (j, reason(fm[k0:j + 1])))
    e = t - 1
    cand, n, var, ev = auto['gb_cand'][e], auto['gb_n'][e], auto['gb_var'][e], auto['gb_ev_end'][e]
    nxt = truth[i + 1] if i + 1 < len(stops) else float('nan')
    err = cand - nxt
    print('S%d   %s   %-40s %9.3f %4d %6.3f %8.2f  %9.3f  %+7.3f %+8.4f'
          % (i + 1, '%5.1f' % ((ready[0]) / 10) if len(ready) else ' none', ','.join(labels) or '-',
             cand, n, var, ev, nxt, err, np.degrees(err * SC)))
print('applied at anchors (window: version, active_zero, evidence age s):')
for a in anchors:
    g = a + 1
    print('  anchor seed k=%d (t=%.1f s): version %d active_zero %.3f (candidate evidence end %.2f s, age %.2f s)'
          % (a, a / 10, auto['gb_ver'][g], auto['gb_active'][g], auto['gb_ev_end'][a], (a + 1.13) / 10 - auto['gb_ev_end'][a]))

def seg_rows(d, a):
    end = next((k for k in range(a + 1, N) if d['anchor'][k] == 1), N)
    return np.arange(a, end)

print('\n== 3. production budget (auto run, default config + research profile) ==')
print('anchor  t(s)  10s:budget/hdg/dist  20s  30s  45s  60s   end_state  end_elapsed end_dist end_budget  limit')
for a in anchors:
    rows = seg_rows(auto, a)
    live = rows[(auto['state'][rows] == 2)]
    out = []
    for T in (10, 20, 30, 45, 60):
        m = live[np.abs(auto['elapsed'][live] - T) < 0.051]
        out.append('%5.1f/%.3f/%4.0f' % (auto['budget'][m[0]], auto['hbudget'][m[0]], auto['dist'][m[0]]) if len(m) else '   --   ')
    last = live[-1]
    lim_rows = rows[auto['state'][rows] == 6]
    lim = 'budget>100m' if len(lim_rows) and auto['budget'][last] > 95 else ('60s' if auto['elapsed'][last] >= 59.8 else
          ('next anchor/stop' if not len(lim_rows) else 'other'))
    print('k=%-5d %5.1f %s  %s %6.1f %7.1f %7.2f  %s' % (a, a / 10, '  '.join(out),
          STATE[int(auto['state'][rows[-1]])], auto['elapsed'][last], auto['dist'][last], auto['budget'][last], lim))
print('(analytic: budget(t)=10+0.3t+int v*2sin((0.15+0.002t)/2)dt; at constant v the 100 m budget is hit at:)')
for v in (5, 8, 10, 15, 20):
    t = 0; b = 10; dt = 0.05
    while b < 100 and t < 60: b += 0.3 * dt + v * 2 * np.sin((0.15 + 0.002 * t) / 2) * dt; t += dt
    print('   v=%2d m/s: %s (distance %.0f m; 1500 m limit at %.0f s)' % (v, ('%.1f s' % t) if b >= 100 else '>60 s', v * t, 1500 / v))

print('\n== 3b. zero sensitivity (relaxed budget: only 60 s/1500 m limits; positions identical to production while alive) ==')
rel_nom = run('--auto-bias', 0, '--zero', 2047, '--relaxed', 1)
rel_est = run('--auto-bias', 1, '--relaxed', 1)
# check positions are identical to production where both alive
both = (auto['state'] == 2) & (rel_est['state'] == 2)
print('max |production - relaxed| east/north while both ACTIVE: %.2e m' %
      max(np.abs(auto['east'][both] - rel_est['east'][both]).max(), np.abs(auto['north'][both] - rel_est['north'][both]).max()))
def stop_before(a):
    return max(i for i, (s, t) in enumerate(stops) if t <= a)
def at(d, a, T):
    rows = seg_rows(d, a); live = rows[(d['state'][rows] == 2)]
    m = live[np.abs(d['elapsed'][live] - T) < 0.051]
    return (d['east'][m[0]], d['north'][m[0]], d['heading'][m[0]]) if len(m) else None
print('anchor zero_nom zero_est zero_oracle(prev) zero_next |  T   |est-oracle| |nom-oracle| |next-oracle| m   hdg(est-orc,nom-orc) deg')
for a in anchors:
    i = stop_before(a)
    zo = truth[i]; zn = truth[i + 1] if i + 1 < len(stops) else float('nan')
    orc = run('--auto-bias', 0, '--zero', '%.6f' % zo, '--relaxed', 1)
    nxt = run('--auto-bias', 0, '--zero', '%.6f' % zn, '--relaxed', 1) if zn == zn else None
    ze = rel_est['gb_active'][a + 1]
    for T in (15, 30, 60):
        po, pe, pn = at(orc, a, T), at(rel_est, a, T), at(rel_nom, a, T)
        px = at(nxt, a, T) if nxt is not None else None
        if po is None: continue
        f = lambda p: '%7.2f' % np.hypot(p[0] - po[0], p[1] - po[1]) if p else '   --  '
        dh = lambda p: '%+6.2f' % np.degrees((p[2] - po[2] + np.pi) % (2 * np.pi) - np.pi) if p else '  -- '
        print('k=%-5d %7.1f %8.3f %8.3f %9.3f | %3d | %s %s %s     %s %s' % (a, 2047, ze, zo, zn, T, f(pe), f(pn), f(px), dh(pe), dh(pn)))

print('\n== 4. inter-sensor heading disagreement over MOVING windows from each synthetic anchor (NOT truth) ==')
rcm_cw = -np.radians(w['rcm_dps_left'])
wd_cw = (((mps[:, 0] + mps[:, 2]) / 2) - ((mps[:, 1] + mps[:, 3]) / 2)) / 1.5   # (vL - vR)/track, cw+
print('anchor  T  | hdg a-b  a-c  b-c (deg) | pos |a-b| |a-c| |b-c| (m) | dist m')
for a in anchors:
    i = stop_before(a); s, t = stops[i]
    za = auto['gb_active'][a + 1]
    zr = rcm_cw[s:t].mean()
    ya = (za - fm) * SC        # yaw_rad_per_count is negative: (fm-zero)*(-SC)
    yb = rcm_cw - zr
    yc = wd_cw
    h = np.zeros(3); p = np.zeros((3, 2)); mov = 0.0; dist = 0
    marks = {10: None, 30: None, 60: None}
    hp = 0.0  # all-window integration (mirrors core, which skips heading only when 'stopped')
    for k in range(a + 1, N):
        if auto['anchor'][k] == 1: break
        if spd[k] > 0.2:
            for j, y in enumerate((ya[k], yb[k], yc[k])):
                hm = h[j] + y * 0.05
                p[j] += spd[k] * 0.1 * np.array([np.sin(hm), np.cos(hm)]); h[j] += y * 0.1
            mov += 0.1; dist += spd[k] * 0.1
            for T in marks:
                if marks[T] is None and mov >= T - 1e-9:
                    marks[T] = (h.copy(), p.copy(), dist, k)
    for T in (10, 30, 60):
        if marks[T] is None:
            print('k=%-5d %2d | (segment has only %.1f s moving before next anchor)' % (a, T, mov)); continue
        hh, pp, dd, k = marks[T]
        dg = lambda x, y: np.degrees(hh[x] - hh[y])
        dp = lambda x, y: np.hypot(*(pp[x] - pp[y]))
        print('k=%-5d %2d | %+6.2f %+6.2f %+6.2f | %6.1f %6.1f %6.1f | %6.0f |' % (a, T, dg(0, 1), dg(0, 2), dg(1, 2), dp(0, 1), dp(0, 2), dp(1, 2), dd))
    # yaw scale cross-check during this segment's moving windows
print('\nscale check (moving, |rate|>0.05 rad/s): slope of a vs b, a vs c over all segments:')
mv = (spd > 3) & (np.abs(rcm_cw) > 0.05)
yaz = (2045.3 - fm) * SC
print('  a/b %.3f  a/c %.3f' % (np.polyfit(rcm_cw[mv], yaz[mv], 1)[0], np.polyfit(wd_cw[mv], yaz[mv], 1)[0]))

print('\n== 4b. in-motion zero of yaw12 implied by RCM 0x075 (regression fm = z + g*rcm_cw, moving v>3 m/s) ==')
print('segment(after stop)  windows  z_counts  gain(counts per rad/s)  prev_stop_mean  next_stop_mean')
rcm_c = rcm_cw - np.mean([rcm_cw[s:t].mean() for s, t in stops])
for i in range(len(stops) - 1):
    s0 = stops[i][1]; s1 = stops[i + 1][0]
    m = np.arange(s0, s1)[spd[s0:s1] > 3]
    g, z = np.polyfit(rcm_c[m], fm[m], 1)
    print('M%d (k %d-%d)        %5d   %8.3f   %8.1f            %8.3f        %8.3f' % (i + 1, s0, s1, len(m), z, g, truth[i], truth[i + 1]))

print('\n== 5a. cross-check: Python re-integration of pipeline heading until the core first enters STOPPED ==')
for a in anchors:
    za = rel_est['gb_active'][a + 1]; h = 0.0; worst = 0.0; n = 0
    for k in range(a + 2, N):
        if rel_est['state'][k] != 2 or rel_est['snap_stopped'][k]: break
        # row k frontier = receipt of window k-1; interval [rx_{k-2},rx_{k-1}] uses yaw window k-1
        h += (fm[k - 1] - za) * (-SC) * 0.1; n += 1
        worst = max(worst, abs((rel_est['heading'][k] - h + np.pi) % (2 * np.pi) - np.pi))
    print('  anchor k=%d: %d rows, max |pipeline - python| heading = %.6f deg' % (a, n, np.degrees(worst)))

print('\n== 5b. wheel-vs-yaw window alignment in the pipeline (speed held from window j-1 while yaw window j integrates) ==')
for a in anchors:
    za = auto['gb_active'][a + 1]; y = (fm - za) * (-SC)
    end = min(next((k for k in range(a + 1, N) if auto['anchor'][k] == 1), N), a + 300)
    pa = np.zeros(2); pl = np.zeros(2); h = 0.0
    for k in range(a + 2, end):
        hm = h + y[k] * 0.05
        u = np.array([np.sin(hm), np.cos(hm)])
        pa += spd[k] * 0.1 * u; pl += spd[k - 1] * 0.1 * u; h += y[k] * 0.1
    print('  anchor k=%d, %.0f s: aligned-vs-lagged speed final position difference %.2f m (path %.0f m)'
          % (a, (end - a - 2) / 10, np.hypot(*(pa - pl)), spd[a + 2:end].sum() * 0.1))
