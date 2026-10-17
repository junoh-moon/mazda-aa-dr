#!/usr/bin/env python3
"""Offline A/B comparison of the phone gyro and the OBD 0x079 yaw from the phone app's export.

validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md, section 16 (record only). The CMU
receives only the source SourceSelector chose, so the two sources are never side
by side in the CMU journal. While an OBD adapter is configured, the phone app
(AbCompare) sums the windows of BOTH producers per interval of >= 10 s of
monotonic time and writes one row per interval to its separate A/B ring, which
'진단 기록 내보내기' appends to the exported text:

  <ISO time> t=<elapsedRealtime ms>ms ab iv_ms=I sel=PHONE|OBD|MIX|NONE
      g_n=N g_dur_us=D g_dth_urad=T g_grav=0|1 g_st=K
      o_n=N o_dur_us=D o_dth_urad=T o_gap=0|1          (one line)

A side without windows in the interval is left out and miss=obd / miss=gyro is
appended. t is the interval end (the row is written in the same tick).

Sign (traced from the code, not assumed here): both dtheta sums are the wire
values, CLOCKWISE POSITIVE seen from above (heading increasing, like a GPS
course):
  * gyro (Integrator): sum over samples of (omega . down) * dt, omega from
    TYPE_GYROSCOPE (calibrated), down = -gravity/|gravity| from TYPE_GRAVITY
    (Android's gravity points up). With right-handed phone axes a clockwise
    turn has omega pointing down, so the projection is positive. The phone's
    mounting does not enter; the projection is only as good as the fused
    gravity estimate (it may tilt under sustained lateral acceleration).
  * OBD (ObdIntegrator): -(raw12 - 2047) * 658.615 urad/s * 0.020 s per CAN
    frame; the count rises in left turns (public ND log, the 0x078 contract,
    design note section 4), relative to the NOMINAL zero 2047 (not corrected).
So the expected relation is OBD = +1 x gyro (+ a small zero offset). Neither
sign has been checked on the owner's car with both sources at once: the tool
ESTIMATES the sign by correlation and reports it next to the expected +1.

Per row: rate = sum dtheta / sum window (rad/s, shown in deg/s) for each side,
so lost frames shorten the covered time but do not bias the rate. Both sides
are binned by the same 10 s of phone RECEIPT time, so the per-row comparison is
NOT lag-free: an OBD receipt lag L (Bluetooth, ELM327 buffering; 0.3-1 s is
plausible for a clone in monitor mode) moves L / iv of each row's OBD rotation
into the next row. On a plain regression OBD on gyro this attenuates the slope
by about L / iv x (1 - row autocorrelation of the rate), i.e. the size of the
3 % agree tolerance (independent review 2026-10-17, finding 5B). The decision
reports a TWO-TAP regression on rows whose predecessor is the directly
preceding interval:
  OBD_i = a + c0 x gyro_i + c1 x gyro_(i-1)
(scale c0 + c1, lag c1 / (c0 + c1) x iv; exact when the rate is constant
within each row) and DECIDES on a THREE-TAP regression that adds gyro_(i+1):
when the rate changes smoothly across a row edge, the rotation a lag moves is
L x the edge rate (about the mean of the two rows), which the two-tap model
cannot represent; leaving out gyro_(i+1) biased its sum HIGH by +2.6 % at
500 ms and +5 % at 1 s in a continuous-time simulation (lag_fit() docstring).
The three-tap scale is the sum of the three coefficients, the lag
(c(-1) - c(+1)) / scale x iv; both regimes give the same answer. The one-tap
regression is still reported (biased low by the lag fraction for rates that
change every row, nearly unbiased for smooth rates).
Usable: gyro rows need g_grav=1, OBD rows o_gap=0, both need iv_ms <= 15000 and
covered time >= 50 % of iv_ms.

Report (text, or --json):
  * coverage of each side (rows present, covered time / interval time), rows
    excluded by reason;
  * parked intervals: runs of >= 3 consecutive usable pair rows whose GYRO rate
    is within 0.3 deg/s and >= 90 % of whose gyro windows carry the phone's
    stationary hint. The OBD rate is NOT a gate: it is relative to the nominal
    2047, so its parked value is the unknown zero offset being measured and is
    reported whatever it is (an offset above 0.30 deg/s = 8 counts used to hide
    every parked interval and the zero-range check; review finding 5A); per interval the
    zero of each source in deg/s (sum dtheta / sum window), the per-row sd and
    the drift (mean of the last third - mean of the first third, and the least
    squares slope per minute); across intervals the range and sd of each zero;
  * duplicate rows: exact duplicates (same wall-clock stamp, t and row text)
    are dropped in load() and counted; the app's A/B ring is never cleared on
    export, so successive exports overlap and the same row must count once
    (review finding 5C);
  * moving rows (mean |rate| of the two >= 2 deg/s): Pearson correlation
    (without outlier rows, |Huber residual| > max(5 robust sd, 0.5 deg/s,
    10 % of |gyro rate|)),
    Huber regression OBD = a + b * gyro and gyro = a' + b' * OBD, the scale
    (geometric mean of b and 1/b', OBD per gyro), the sign estimate, the scale
    in a low (2..8 deg/s) and a high (>= 8 deg/s) rate bin (median of
    (OBD - a) / gyro over the bin's rows) - all one-tap, for comparison;
  * two_tap and three_tap (three_tap is what the decision uses): moving pair
    rows whose neighbours are consecutive intervals with a usable gyro side;
    Huber fit of the model above, lag-free scale, lag in ms, outlier rows by
    the same limit on the fit's residual, the correlation of OBD with the
    lag-aligned gyro (the fitted neighbour combination, weights summing to 1,
    each neighbour weight clamped to [-0.5, 0.5]) without outlier rows, and the
    low/high bin scales of (OBD - a) / aligned gyro;
  * per-row differences (OBD - gyro) in deg/s and in degrees per row;
  * ab_decision: the pre-registered rule of the design note, section 16.2.
Nothing here is ground truth: the two sensors are compared with each other.
"""
import argparse
import json
import math
from pathlib import Path
import re
import statistics
import sys

URAD_S_PER_COUNT = 658.615       # ObdIntegrator / Can079 (the CMU's 0x078 contract)
NOMINAL_ZERO = 2047
EXPECTED_SIGN = +1               # both clockwise positive by the code (docstring)

# Pre-registered before any A/B row existed (2026-10-17); the design note, section 16.2, holds the same numbers.
RULE = dict(
    iv_max_ms=15000,             # a longer interval means the main-thread tick stalled; not compared
    min_coverage=0.50,           # covered time / interval per side for a usable row
    park_dps=0.30,               # parked: |rate| of both sides at most this
    park_stationary_share=0.90,  # and at least this share of gyro windows with the stationary hint
    park_min_rows=3,             # for at least this many consecutive rows (30 s)
    move_dps=2.0,                # moving rows: mean |rate| of the two at least this
    bin_split_dps=8.0,           # low bin [move_dps, 8), high bin >= 8 deg/s
    bin_min_rows=10,
    min_moving_rows=30,          # fewer: not_decidable
    min_side_rows=30,            # rows of a side needed before calling it unusable
    agree_corr=0.98,
    agree_scale_tol=0.03,
    sign_corr=0.90,              # r <= -0.90: sign_mismatch
    unusable_flag_share=0.50,    # o_gap=1 (OBD) or g_grav=0 (gyro) in more than half of that side's rows
    unusable_coverage=0.60,      # covered time / interval time of a side below this
    unusable_zero_range_dps=0.10,  # parked zero range across intervals above this (6 deg/min)
    projection_grav_share=0.10,  # g_grav=0 in more than 10 % of gyro rows
    projection_rate_dependence=0.05,  # |scale_high / scale_low - 1| above this
    projection_min_corr=0.95,    # the rate dependence is judged only when the trimmed corr is at least this
    outlier_k=5.0,               # outlier row: |residual of the Huber fit| > max(5 robust sd, 0.5 deg/s, 10 % of |gyro|)
    outlier_floor_dps=0.5,
    outlier_rel=0.10,
    max_outlier_share=0.10,      # more outlier rows than this: poor_correlation
)

LINE = re.compile(r"^(\S+) t=(-?\d+)ms ab (.*)$")
INT_KEYS = ("iv_ms", "g_n", "g_dur_us", "g_dth_urad", "g_grav", "g_st", "o_n", "o_dur_us", "o_dth_urad", "o_gap")


def parse_lines(lines):
    """A/B rows of an exported app log, in file order; other lines are ignored. Returns (rows, malformed)."""
    rows, malformed = [], 0
    for line in lines:
        m = LINE.match(line.rstrip("\n"))
        if not m:
            continue
        row = dict(time=m[1], t_ms=int(m[2]))
        ok = True
        for tok in m[3].split():
            k, eq, v = tok.partition("=")
            if not eq:
                ok = False
                break
            if k in INT_KEYS:
                try:
                    row[k] = int(v)
                except ValueError:
                    ok = False
                    break
            else:
                row[k] = v
        if not ok or "iv_ms" not in row or row["iv_ms"] <= 0:
            malformed += 1
            continue
        rows.append(row)
    return rows, malformed


def load(paths):
    """A/B rows of every file in argument order without exact duplicates. Returns (rows, malformed, duplicates).

    The app's A/B ring is cumulative (never cleared on export), so two exports overlap by up to the ring's ~8 h and
    the same file may be passed twice. A row is a duplicate when its whole line (wall-clock stamp, t=...ms and the
    row text) was already seen; such rows are dropped and counted. Distinct rows cannot collide: t is monotonic within
    a phone boot and the wall-clock stamp differs across boots."""
    rows, malformed, duplicates, seen = [], 0, 0, set()
    for p in paths:
        lines = []
        for line in Path(p).read_text(errors="replace").splitlines():
            key = line.rstrip("\n").rstrip()
            if LINE.match(key):
                if key in seen:
                    duplicates += 1
                    continue
                seen.add(key)
            lines.append(line)
        r, m = parse_lines(lines)
        rows += r
        malformed += m
    return rows, malformed, duplicates


def deg(rad):
    return rad * 180.0 / math.pi


def side(row, p):
    """(present, usable, rate deg/s or None, reason) of one side ('g' or 'o') of a row."""
    n, dur, dth = row.get(p + "_n", 0), row.get(p + "_dur_us", 0), row.get(p + "_dth_urad", 0)
    if n <= 0 or dur <= 0:
        return False, False, None, "missing"
    rate = deg(dth / dur)
    if p == "g" and row.get("g_grav", 0) != 1:
        return True, False, rate, "g_grav0"
    if p == "o" and row.get("o_gap", 0) != 0:
        return True, False, rate, "o_gap"
    if row["iv_ms"] > RULE["iv_max_ms"]:
        return True, False, rate, "long_interval"
    if dur / (row["iv_ms"] * 1000.0) < RULE["min_coverage"]:
        return True, False, rate, "low_coverage"
    return True, True, rate, None


def r4(x):
    return None if x is None else round(x, 4)


def pearson(x, y):
    if len(x) < 3:
        return None
    mx, my = statistics.fmean(x), statistics.fmean(y)
    sxx = sum((a - mx) ** 2 for a in x)
    syy = sum((b - my) ** 2 for b in y)
    if sxx <= 0 or syy <= 0:
        return None
    return sum((a - mx) * (b - my) for a, b in zip(x, y)) / math.sqrt(sxx * syy)


def wls(x, y, w):
    sw = sum(w)
    mx = sum(wi * a for wi, a in zip(w, x)) / sw
    my = sum(wi * b for wi, b in zip(w, y)) / sw
    sxx = sum(wi * (a - mx) ** 2 for wi, a in zip(w, x))
    if sxx <= 0:
        return None
    b = sum(wi * (a - mx) * (c - my) for wi, a, c in zip(w, x, y)) / sxx
    return my - b * mx, b


def solve(a, b):
    """Solves the small linear system a x = b (Gaussian elimination, partial pivoting); None when singular."""
    n = len(b)
    m = [list(a[i]) + [b[i]] for i in range(n)]
    scale = max(abs(m[i][i]) for i in range(n)) or 1.0
    for c in range(n):
        piv = max(range(c, n), key=lambda r: abs(m[r][c]))
        if abs(m[piv][c]) <= 1e-12 * scale:
            return None
        m[c], m[piv] = m[piv], m[c]
        for r in range(n):
            if r != c:
                f = m[r][c] / m[c][c]
                for k in range(c, n + 1):
                    m[r][k] -= f * m[c][k]
    return [m[i][n] / m[i][i] for i in range(n)]


def wls_multi(xs, y, w):
    """Weighted least squares of y = c[0] + c[1] x1 + c[2] x2 + ... (xs: rows of predictors); None if degenerate."""
    rows = [[1.0] + list(r) for r in xs]
    p = len(rows[0])
    a = [[sum(wi * r[i] * r[j] for wi, r in zip(w, rows)) for j in range(p)] for i in range(p)]
    b = [sum(wi * r[i] * c for wi, r, c in zip(w, rows, y)) for i in range(p)]
    return solve(a, b)


def huber_multi(xs, y, k=1.345, iters=50):
    """Huber M-estimate of y = c0 + c1 x1 + c2 x2 by IRLS (MAD scale), like huber(). None if degenerate."""
    if not xs or len(y) < len(xs[0]) + 2:
        return None
    fit = wls_multi(xs, y, [1.0] * len(y))
    if fit is None:
        return None
    for _ in range(iters):
        res = [c - fit[0] - sum(fi * v for fi, v in zip(fit[1:], r)) for r, c in zip(xs, y)]
        s = 1.4826 * statistics.median(abs(e) for e in res)
        if s <= 1e-12:
            break
        w = [1.0 if abs(e) <= k * s else k * s / abs(e) for e in res]
        new = wls_multi(xs, y, w)
        if new is None:
            break
        done = all(abs(n - o) < 1e-9 for n, o in zip(new, fit))
        fit = new
        if done:
            break
    return fit


def huber(x, y, k=1.345, iters=50):
    """Huber M-estimate of y = a + b x by iteratively reweighted least squares (MAD scale). None if degenerate."""
    if len(x) < 3:
        return None
    fit = wls(x, y, [1.0] * len(x))
    if fit is None:
        return None
    for _ in range(iters):
        a, b = fit
        res = [c - a - b * v for v, c in zip(x, y)]
        s = 1.4826 * statistics.median(abs(e) for e in res)
        if s <= 1e-12:
            break
        w = [1.0 if abs(e) <= k * s else k * s / abs(e) for e in res]
        new = wls(x, y, w)
        if new is None:
            break
        done = abs(new[1] - b) < 1e-9 and abs(new[0] - a) < 1e-9
        fit = new
        if done:
            break
    return fit


def drift(ts, vs):
    """(last third mean - first third mean, least squares slope per minute) of values over times (s)."""
    n = len(vs)
    k = max(1, n // 3)
    d = statistics.fmean(vs[-k:]) - statistics.fmean(vs[:k])
    mt = statistics.fmean(ts)
    stt = sum((t - mt) ** 2 for t in ts)
    slope = None if stt <= 0 else sum((t - mt) * (v - statistics.fmean(vs)) for t, v in zip(ts, vs)) / stt * 60.0
    return d, slope


def spread(vals):
    if not vals:
        return dict(n=0)
    return dict(n=len(vals), min=r4(min(vals)), max=r4(max(vals)), range=r4(max(vals) - min(vals)),
                sd=r4(statistics.pstdev(vals)) if len(vals) > 1 else 0.0, mean=r4(statistics.fmean(vals)))


def pct(vals, q):
    if not vals:
        return None
    s = sorted(vals)
    return s[min(len(s) - 1, int(math.ceil(q * len(s))) - 1)]


def consecutive(prev, row):
    """The row directly follows prev (same phone boot, next interval)."""
    dt = row["t_ms"] - prev["t_ms"]
    return 0 < dt <= row["iv_ms"] + 2000


def analyse(rows, malformed=0, duplicates=0):
    rule = RULE
    per = []
    excluded = dict(g_grav0=0, o_gap=0, long_interval=0, low_coverage_g=0, low_coverage_o=0)
    cov = dict(g=[0, 0], o=[0, 0])       # covered us, interval us over rows where the side is present
    present = dict(g=0, o=0, both=0, only_g=0, only_o=0)
    flagged = dict(g=0, o=0)
    for row in rows:
        gp, gu, gr, greason = side(row, "g")
        op, ou, orate, oreason = side(row, "o")
        for p, pres in (("g", gp), ("o", op)):
            if pres:
                present[p] += 1
                cov[p][0] += row[p + "_dur_us"]
                cov[p][1] += row["iv_ms"] * 1000
        if gp and op:
            present["both"] += 1
        elif gp:
            present["only_g"] += 1
        elif op:
            present["only_o"] += 1
        if gp and row.get("g_grav", 0) != 1:
            flagged["g"] += 1
        if op and row.get("o_gap", 0) != 0:
            flagged["o"] += 1
        if greason == "g_grav0":
            excluded["g_grav0"] += 1
        if oreason == "o_gap":
            excluded["o_gap"] += 1
        if "long_interval" in (greason, oreason):
            excluded["long_interval"] += 1
        if greason == "low_coverage":
            excluded["low_coverage_g"] += 1
        if oreason == "low_coverage":
            excluded["low_coverage_o"] += 1
        st = row.get("g_st")
        per.append(dict(row=row, pair=gu and ou, g=gr if gu else None, o=orate if ou else None,
                        stationary_share=(st / row["g_n"]) if (gp and st is not None) else None))
    coverage = {p: (r4(cov[p][0] / cov[p][1]) if cov[p][1] else None) for p in ("g", "o")}

    # parked intervals: runs of consecutive pair rows that are quiet and stationary by the PHONE (gyro rate and the
    # stationary hint). The OBD rate is not a gate: relative to the nominal 2047 its parked value is the zero offset
    # this section measures, so it is reported whatever it is (review finding 5A).
    def parked(e):
        if not e["pair"] or abs(e["g"]) > rule["park_dps"]:
            return False
        s = e["stationary_share"]
        return s is None or s >= rule["park_stationary_share"]

    runs, cur = [], []
    for e in per:
        if parked(e) and (not cur or consecutive(cur[-1]["row"], e["row"])):
            cur.append(e)
            continue
        if len(cur) >= rule["park_min_rows"]:
            runs.append(cur)
        cur = [e] if parked(e) else []
    if len(cur) >= rule["park_min_rows"]:
        runs.append(cur)
    intervals = []
    for run in runs:
        ts = [(e["row"]["t_ms"] - run[0]["row"]["t_ms"]) / 1000.0 for e in run]
        item = dict(start_t_ms=run[0]["row"]["t_ms"] - run[0]["row"]["iv_ms"], start_time=run[0]["row"]["time"], rows=len(run),
                    seconds=round(sum(e["row"]["iv_ms"] for e in run) / 1000.0, 1))
        for p, name in (("g", "gyro"), ("o", "obd")):
            zero = deg(sum(e["row"][p + "_dth_urad"] for e in run) / sum(e["row"][p + "_dur_us"] for e in run))
            vals = [e[p] for e in run]
            d, slope = drift(ts, vals)
            item[name] = dict(zero_dps=r4(zero), sd_dps=r4(statistics.pstdev(vals)), drift_dps=r4(d),
                              slope_dps_per_min=r4(slope))
        item["obd"]["zero_count"] = r4(NOMINAL_ZERO - math.radians(item["obd"]["zero_dps"]) * 1e6 / URAD_S_PER_COUNT)
        intervals.append(item)
    across = dict(intervals=len(intervals), gyro=spread([i["gyro"]["zero_dps"] for i in intervals]),
                  obd=spread([i["obd"]["zero_dps"] for i in intervals]))

    pairs = [e for e in per if e["pair"]]
    moving = [e for e in pairs if (abs(e["g"]) + abs(e["o"])) / 2.0 >= rule["move_dps"]]
    gx, oy = [e["g"] for e in moving], [e["o"] for e in moving]
    reg = dict(rows=len(moving), corr=None, corr_all=r4(pearson(gx, oy)), outliers=0, outlier_share=None,
               sign_estimate="undetermined", expected_sign=EXPECTED_SIGN)
    fo, fg = huber(gx, oy), huber(oy, gx)
    keep = moving
    if fo:
        res = [e["o"] - fo[0] - fo[1] * e["g"] for e in moving]
        lim = max(rule["outlier_k"] * 1.4826 * statistics.median(abs(x) for x in res), rule["outlier_floor_dps"])
        keep = [e for e, x in zip(moving, res) if abs(x) <= max(lim, rule["outlier_rel"] * abs(e["g"]))]
        reg["outliers"] = len(moving) - len(keep)
        reg["outlier_share"] = r4(reg["outliers"] / len(moving))
        reg["outlier_limit_dps"] = r4(lim)
    # corr: Pearson over the moving rows without the outlier rows (corr_all keeps them)
    r = pearson([e["g"] for e in keep], [e["o"] for e in keep])
    if r is not None:
        reg["corr"] = r4(r)
        if r >= 0.5:
            reg["sign_estimate"] = +1
        elif r <= -0.5:
            reg["sign_estimate"] = -1
    if fo:
        reg["obd_on_gyro"] = dict(intercept_dps=r4(fo[0]), slope=r4(fo[1]))
    if fg:
        reg["gyro_on_obd"] = dict(intercept_dps=r4(fg[0]), slope=r4(fg[1]))
    if fo and fg and fo[1] * fg[1] > 0:
        reg["scale_obd_per_gyro"] = r4(math.copysign(math.sqrt(fo[1] / fg[1]), fo[1]))
    if fo:
        a = fo[0]
        bins = {}
        for name, lo, hi in (("low", rule["move_dps"], rule["bin_split_dps"]), ("high", rule["bin_split_dps"], float("inf"))):
            # robust per-bin scale: the median of (OBD - intercept) / gyro over all moving rows of the bin
            sel = [e for e in moving if lo <= (abs(e["g"]) + abs(e["o"])) / 2.0 < hi and e["g"] != 0]
            bins[name] = dict(rows=len(sel), slope=r4(statistics.median((e["o"] - a) / e["g"] for e in sel)) if sel else None)
        reg["bins"] = bins
        lo, hi = bins["low"], bins["high"]
        if (lo["rows"] >= rule["bin_min_rows"] and hi["rows"] >= rule["bin_min_rows"] and lo["slope"] and hi["slope"]):
            reg["rate_dependence"] = r4(hi["slope"] / lo["slope"] - 1.0)

    reg["two_tap"] = lag_fit(per, rule, (0, -1))
    reg["three_tap"] = lag_fit(per, rule, (-1, 0, 1))

    def diffs(es):
        d = [e["o"] - e["g"] for e in es]
        rot = [(e["o"] - e["g"]) * e["row"]["iv_ms"] / 1000.0 for e in es]
        return dict(rows=len(es), median_dps=r4(statistics.median(d)) if d else None,
                    abs_p50_dps=r4(pct([abs(x) for x in d], 0.5)), abs_p90_dps=r4(pct([abs(x) for x in d], 0.9)),
                    abs_p90_deg_per_row=r4(pct([abs(x) for x in rot], 0.9)), sum_deg=r4(sum(rot)))

    report = dict(
        rows=len(rows), malformed=malformed, duplicates=duplicates, present=present, coverage=coverage, excluded=excluded,
        flag_share=dict(g_grav0=r4(flagged["g"] / present["g"]) if present["g"] else None,
                        o_gap=r4(flagged["o"] / present["o"]) if present["o"] else None),
        pair_rows=len(pairs), parked=intervals, parked_across=across, moving=reg,
        diff_all=diffs(pairs), diff_moving=diffs(moving),
        per_row=[dict(t_ms=e["row"]["t_ms"], iv_ms=e["row"]["iv_ms"], sel=e["row"].get("sel"),
                      gyro_dps=r4(e["g"]), obd_dps=r4(e["o"]),
                      diff_dps=r4(e["o"] - e["g"]) if e["pair"] else None) for e in per],
        sign_convention="both clockwise positive (heading increasing); expected OBD = +1 x gyro; sign estimated by correlation",
        rule=RULE)
    report["ab_decision"] = decide(report)
    return report


def lag_fit(per, rule, taps):
    """A lag-aware comparison over moving pair rows (review finding 5B; design note 16.6).

    taps: row offsets of the gyro predictors, 0 = the same row: (0, -1) is the two-tap model
    OBD_i = a + c0 gyro_i + c1 gyro_(i-1); (-1, 0, 1) adds the next row. Rows: moving pair rows whose neighbours at
    every offset are directly consecutive intervals (consecutive()) with a usable gyro side. The scale s is the sum of
    the coefficients (a receipt lag only moves rotation between neighbouring rows, so the sum is lag-free as long as
    the moved rotation is a combination of the modelled neighbours), and the lag is
    (c(-1) - c(+1)) / s x the median interval:
      * rates constant within a row (the review's reproduction): OBD_i = s((1 - f) g_i + f g_(i-1)), f = L / iv, so
        c(-1) = s f, c(+1) = 0;
      * rates that change smoothly across a row edge: the rotation moved at an edge is L x the EDGE rate, about the
        mean of the two rows, so OBD_i = s(g_i + f/2 (g_(i-1) - g_(i+1))): c(-1) = s f/2, c(+1) = -s f/2. The two-tap
        model cannot represent this; leaving out g_(i+1), which is correlated with g_i and g_(i-1), biases its sum
        HIGH (continuous-time simulation: +2.6 % at 500 ms, +5 % at 1 s).
    Both cases give c(-1) - c(+1) = s f. The decision uses the three-tap fit; the two-tap fit is reported."""
    out = dict(taps=list(taps), rows=0, corr=None, outliers=0, outlier_share=None, scale=None, lag_ms=None,
               sign_estimate="undetermined", no_variance=[])
    sel = []
    for i, e in enumerate(per):
        if not e["pair"] or (abs(e["g"]) + abs(e["o"])) / 2.0 < rule["move_dps"]:
            continue
        xs = []
        for k in taps:
            j = i + k
            if j < 0 or j >= len(per) or per[j]["g"] is None:
                break
            if k != 0 and not all(consecutive(per[m - 1]["row"], per[m]["row"])
                                  for m in range(min(i, j) + 1, max(i, j) + 1)):
                break
            xs.append(per[j]["g"])
        else:
            sel.append((e, xs))
    out["rows"] = len(sel)
    if len(sel) < 3:
        return out
    g = [e["g"] for e, _ in sel]
    o = [e["o"] for e, _ in sel]
    for name, vals in (("gyro", g), ("obd", o)):
        if max(vals) - min(vals) <= 0:
            out["no_variance"].append(name)
    if out["no_variance"]:
        return out
    xs = [x for _, x in sel]
    fit = huber_multi(xs, o)
    if fit is None:
        out["degenerate"] = True
        return out
    a, c = fit[0], fit[1:]
    s = sum(c)
    iv = statistics.median(e["row"]["iv_ms"] for e, _ in sel)
    coef = {k: ci for k, ci in zip(taps, c)}
    out.update(intercept_dps=r4(a), coef={str(k): r4(v) for k, v in coef.items()}, scale=r4(s), iv_ms_median=iv)
    w = {k: (1.0 if k == 0 else 0.0) for k in taps}
    if abs(s) > 1e-9:
        out["lag_ms"] = round((coef.get(-1, 0.0) - coef.get(1, 0.0)) / s * iv, 1)
        # the lag-aligned gyro: the fitted combination of the neighbours with weights summing to 1, each
        # neighbour's weight clamped to [-0.5, 0.5]
        for k in taps:
            if k != 0:
                w[k] = min(0.5, max(-0.5, coef[k] / s))
        w[0] = 1.0 - sum(v for k, v in w.items() if k != 0)
    out["aligned_weights"] = {str(k): r4(v) for k, v in w.items()}
    ga = [sum(w[k] * x for k, x in zip(taps, row)) for row in xs]
    res = [oi - a - sum(ci * x for ci, x in zip(c, row)) for oi, row in zip(o, xs)]
    lim = max(rule["outlier_k"] * 1.4826 * statistics.median(abs(x) for x in res), rule["outlier_floor_dps"])
    keep = [k for k, (x, gi) in enumerate(zip(res, g)) if abs(x) <= max(lim, rule["outlier_rel"] * abs(gi))]
    out["outliers"] = len(sel) - len(keep)
    out["outlier_share"] = r4(out["outliers"] / len(sel))
    out["outlier_limit_dps"] = r4(lim)
    r = pearson([ga[k] for k in keep], [o[k] for k in keep])
    if r is not None:
        out["corr"] = r4(r)
        out["sign_estimate"] = +1 if r >= 0.5 else (-1 if r <= -0.5 else "undetermined")
    bins = {}
    for name, lo, hi in (("low", rule["move_dps"], rule["bin_split_dps"]), ("high", rule["bin_split_dps"], float("inf"))):
        vals = [(o[k] - a) / ga[k] for k in range(len(sel))
                if lo <= (abs(g[k]) + abs(o[k])) / 2.0 < hi and ga[k] != 0]
        bins[name] = dict(rows=len(vals), slope=r4(statistics.median(vals)) if vals else None)
    out["bins"] = bins
    lo, hi = bins["low"], bins["high"]
    if lo["rows"] >= rule["bin_min_rows"] and hi["rows"] >= rule["bin_min_rows"] and lo["slope"] and hi["slope"]:
        out["rate_dependence"] = r4(hi["slope"] / lo["slope"] - 1.0)
    return out


def decide(rep):
    """The pre-registered rule (design note 16.2). One outcome; every check is reported."""
    rule = rep["rule"]
    checks, reasons = {}, []
    unusable = {}
    for p, name, flag in (("g", "gyro", "g_grav0"), ("o", "obd", "o_gap")):
        why = []
        if rep["present"][p] >= rule["min_side_rows"]:
            if rep["flag_share"][flag] > rule["unusable_flag_share"]:
                why.append("%s share %s > %.2f" % (flag, rep["flag_share"][flag], rule["unusable_flag_share"]))
            if rep["coverage"][p] < rule["unusable_coverage"]:
                why.append("coverage %s < %.2f" % (rep["coverage"][p], rule["unusable_coverage"]))
        z = rep["parked_across"][name]
        if z.get("n", 0) >= 2 and z["range"] > rule["unusable_zero_range_dps"]:
            why.append("parked zero range %s deg/s > %.2f" % (z["range"], rule["unusable_zero_range_dps"]))
        unusable[name] = why
    checks["unusable"] = unusable
    m = rep["moving"]
    t = m["three_tap"]
    gshare = rep["flag_share"]["g_grav0"]
    proj = []
    if gshare is not None and rep["present"]["g"] >= rule["min_side_rows"] and gshare > rule["projection_grav_share"]:
        proj.append("g_grav0 share %s > %.2f" % (gshare, rule["projection_grav_share"]))
    if (t.get("rate_dependence") is not None and t["corr"] is not None and t["corr"] >= rule["projection_min_corr"]
            and abs(t["rate_dependence"]) > rule["projection_rate_dependence"]):
        proj.append("scale high/low bin - 1 = %s, |.| > %.2f" % (t["rate_dependence"], rule["projection_rate_dependence"]))
    checks["projection"] = proj
    # 2026-10-17 (review 5B): the decision uses the three-tap rows, their correlation with the lag-aligned gyro and the
    # lag-free scale; the thresholds are unchanged. The two-tap and one-tap scales are shown next to it.
    scale = t.get("scale")
    checks["agreement"] = dict(corr=t["corr"], scale=scale, lag_ms=t.get("lag_ms"), sign_estimate=t["sign_estimate"],
                               moving_rows=m["rows"], lag_rows=t["rows"], two_tap_scale=m["two_tap"]["scale"],
                               two_tap_lag_ms=m["two_tap"]["lag_ms"], one_tap_scale=m.get("scale_obd_per_gyro"),
                               one_tap_corr=m["corr"])
    if unusable["gyro"] and unusable["obd"]:
        outcome, reasons = "both_unusable", unusable["gyro"] + unusable["obd"]
    elif unusable["gyro"]:
        outcome, reasons = "gyro_unusable", unusable["gyro"]
    elif unusable["obd"]:
        outcome, reasons = "obd_unusable", unusable["obd"]
    elif t["rows"] < rule["min_moving_rows"]:
        outcome, reasons = "not_decidable", ["moving pair rows with consecutive neighbours %d < %d (moving pair rows %d)"
                                             % (t["rows"], rule["min_moving_rows"], m["rows"])]
    elif t["no_variance"]:
        outcome, reasons = "not_decidable", ["no variance on the %s side over %d moving rows (constant rate; a stuck or dead "
                                             "source)" % (" and ".join(t["no_variance"]), t["rows"])]
    elif t["corr"] is None or scale is None:
        outcome, reasons = "not_decidable", ["three-tap fit degenerate over %d moving rows" % t["rows"]]
    elif t["corr"] <= -rule["sign_corr"]:
        outcome, reasons = "sign_mismatch", ["corr %s <= -%.2f (expected sign %+d)" % (t["corr"], rule["sign_corr"], EXPECTED_SIGN)]
    elif proj:
        outcome, reasons = "phone_projection_unreliable", proj
    elif t["outlier_share"] is not None and t["outlier_share"] > rule["max_outlier_share"]:
        outcome, reasons = "poor_correlation", ["outlier rows %s > %.2f" % (t["outlier_share"], rule["max_outlier_share"])]
    elif t["corr"] > rule["agree_corr"] and abs(scale - 1.0) <= rule["agree_scale_tol"]:
        outcome = "agree"
    elif t["corr"] > rule["agree_corr"]:
        outcome, reasons = "scale_disagree", ["lag-free scale %s outside 1 +- %.2f (receipt lag %s ms)" %
                                              (scale, rule["agree_scale_tol"], t.get("lag_ms"))]
    else:
        outcome, reasons = "poor_correlation", ["corr %s <= %.2f" % (t["corr"], rule["agree_corr"])]
    return dict(outcome=outcome, reasons=reasons, checks=checks,
                note="sensor-versus-sensor only; agree does not make either source a product input")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("inputs", nargs="+", help="exported phone app logs (mx5imu-log-*.txt)")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    rows, malformed, duplicates = load(args.inputs)
    rep = analyse(rows, malformed, duplicates)
    if args.json:
        print(json.dumps(rep, indent=1, sort_keys=True))
        return 0 if rows else 2
    print("A/B rows: %d (malformed %d, exact duplicates dropped %d); both sides %d, gyro only %d, OBD only %d" %
          (rep["rows"], malformed, duplicates, rep["present"]["both"], rep["present"]["only_g"], rep["present"]["only_o"]))
    if not rows:
        print("no ab rows: the app writes them only while an OBD adapter is chosen and the CMU is connected")
        return 2
    print("coverage (covered time / interval): gyro %s, OBD %s; flagged rows: g_grav=0 %s, o_gap=1 %s" %
          (rep["coverage"]["g"], rep["coverage"]["o"], rep["flag_share"]["g_grav0"], rep["flag_share"]["o_gap"]))
    print("excluded: %s; usable pair rows %d" % (rep["excluded"], rep["pair_rows"]))
    for i in rep["parked"]:
        print("  parked at %s (%d rows, %.0f s): gyro zero %+.4f deg/s sd %.4f drift %+.4f; OBD zero %+.4f deg/s "
              "(%.3f counts) sd %.4f drift %+.4f" %
              (i["start_time"], i["rows"], i["seconds"], i["gyro"]["zero_dps"], i["gyro"]["sd_dps"], i["gyro"]["drift_dps"],
               i["obd"]["zero_dps"], i["obd"]["zero_count"], i["obd"]["sd_dps"], i["obd"]["drift_dps"]))
    a = rep["parked_across"]
    print("parked zeros across %d intervals: gyro %s; OBD %s" % (a["intervals"], a["gyro"], a["obd"]))
    m = rep["moving"]
    print("moving rows %d (one-tap, biased low by the receipt lag): corr %s (all rows %s, outliers %s), sign estimate %s (expected %+d), "
          "OBD = %s + %s x gyro, scale OBD/gyro %s, bins %s, rate dependence %s" %
          (m["rows"], m["corr"], m["corr_all"], m["outliers"], m["sign_estimate"], EXPECTED_SIGN, m.get("obd_on_gyro", {}).get("intercept_dps"),
           m.get("obd_on_gyro", {}).get("slope"), m.get("scale_obd_per_gyro"), m.get("bins"), m.get("rate_dependence")))
    t2 = m["two_tap"]
    print("two-tap rows %d: OBD = %s + %s x (gyro this row, previous row); scale %s, lag %s ms" %
          (t2["rows"], t2.get("intercept_dps"), t2.get("coef"), t2.get("scale"), t2.get("lag_ms")))
    t = m["three_tap"]
    print("three-tap (decision) rows %d: OBD = %s + %s x (gyro previous, this, next row); lag-free scale %s, OBD receipt "
          "lag %s ms, corr with lag-aligned gyro %s (outliers %s), bins %s, rate dependence %s" %
          (t["rows"], t.get("intercept_dps"), t.get("coef"), t.get("scale"), t.get("lag_ms"), t["corr"],
           t["outliers"], t.get("bins"), t.get("rate_dependence")))
    print("per-row OBD - gyro: all %s; moving %s" % (rep["diff_all"], rep["diff_moving"]))
    d = rep["ab_decision"]
    print("A/B decision (pre-registered): %s %s" % (d["outcome"], d["reasons"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
