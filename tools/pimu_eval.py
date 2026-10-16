#!/usr/bin/env python3
"""Offline counterfactual: phone gyro and OBD 0x079 yaw (pimu rows) versus the car's yaw.

validation/PHONE_IMU_CHANNEL_2026-10-14.md (stage 1) and
validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md (stage 2), both record only.
Input: an exported trip (directories or tar files) that holds the trace
journal (trace.N.jsonl: boot, log_digest with the yaw digest fields
y1/y1n/yst/ystn/gc, yaw_stop, beta_state) and the phone IMU journal
(pimu.N.jsonl). Nothing is replayed into the product; this only compares
recorded rotation sources.

Sources of a 1 s pimu bin [n, sum dtheta urad, sum window ms, seq gap, flags OR]:
  * flags bit 4 (source_obd): CAN 0x079 yaw via the OBD-II adapter, one CAN
    frame per DATA frame, dtheta relative to the nominal zero 2047, window
    20 ms per frame (covered producer time); bit 5 (obd_gap): frames were
    lost in that second; bit 3 (frame_reset): monitor restart or source
    switch. A bin with bits 4 and 2 (gravity_valid, set by gyro frames) holds
    both sources and is used for neither.
  * no bit 4: the phone gyro about gravity (stage 1), counted as before.
  * OBD bins with frame_reset, and OBD bins one second before or after any
    frame_reset bin, are excluded from every integral (the analyzer's
    phone_imu.obd eval_excluded_m; a switch loses up to ~0.5 s of rotation
    or counts ~the Bluetooth delay twice, design note section 8).

Method (the digest harness of validation/YAW_DEADBAND_2026-10-13.md):
  * per CMU second (CLOCK_MONOTONIC, same boot): the car's heading change is
    -YAW_RAD_PER_COUNT * y1 / rate (rate = yaw samples per second, the
    session median; zero 2048 as the product), the phone's is the bin's sum
    of dtheta (clockwise positive, the GPS course direction), the OBD's is
    the bin's sum of dtheta minus the learned zero rate times the covered
    time (sum of window); GPS courses come from the digest's gc fix codes
    (deciseconds after since_ns * 1000 + course), delay 1.2 s.
  * OBD zero: learned at stops (yaw_stop rows: all four wheels exactly zero
    for >= 1 s, the rows the yaw tools already use; without them, runs of
    whole stationary digests). Usable OBD bins lying entirely inside the
    stop with a 1 s margin give the stop's zero = sum dtheta / sum window
    (as a count on the 2047-based scale), its per-bin sd and drift. The
    same stops' CMU 0x078 zero (yaw_stop mean) is reported next to it.
    A window uses the zero of the last stop that ended at or before the
    window start (causal, as a product would); none: the window has no
    OBD value.
  * windows: every GPS-lost episode (beta_state into GPS_LOST/ENGAGED until it
    leaves them; fixes up to 15 s outside) and synthetic windows of 30/60/120 s starting every 10 s,
    each from the last GPS course at or before the start to the first one at
    or after the end; the course at the start plus the integrated change is
    compared with the course at the end (heading error, deg), and the
    exit jump is approximated by the digest mean speed times the integral of
    the heading error against a linear course between the two fixes (m).
    The exit jump is computed only for straight windows (GPS turn < 10 deg,
    the classes of that record: straight < 10, gentle 10..45, sharp > 45;
    "turning" = gentle + sharp).
  * a window counts for the phone only if every second has a phone bin with
    at least 40 frames (80 % of 50 Hz); for the OBD only if every second has
    a usable OBD bin (not mixed, not near a frame_reset, >= 1 frame) and the
    window holds >= 90 % of 50 frames per second (Bluetooth batches move
    frames between receipt seconds without losing rotation); otherwise it is
    reported as missing. Lost OBD frames are not imputed.
  * OBD coverage: frames per second, covered time / bin time, the share of
    bins with the loss bit, frame_reset bins per hour, usable bins.
  * obd_decision: the pre-registered rule of the design note, section 15.
GPS courses are a reference, not ground truth (integer degrees).
"""
import argparse
import bisect
import json
import math
from pathlib import Path, PurePosixPath
import re
import sys
import tarfile

YAW_RAD_PER_COUNT = 0.000658615
GPS_DELAY_S = 1.2
MIN_FRAMES = 40
WINDOWS_S = (30, 60, 120)
STEP_S = 10
MIN_KMH = 15
LIVE = ("GPS_LOST", "ENGAGED")
EPISODE_SLACK_S = 15.0   # BETA enters GPS_LOST some seconds after the last fix

# Stage 2: OBD 0x079 bins (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md, sections 4 and 6).
FLAG_GRAVITY, FLAG_RESET, FLAG_OBD, FLAG_OBD_GAP = 4, 8, 16, 32
OBD_ZERO = 2047                                   # nominal zero the app subtracts
OBD_URAD_S_PER_COUNT = 658.615
OBD_PERIOD_S = 0.020
OBD_URAD_PER_FRAME_COUNT = OBD_URAD_S_PER_COUNT * OBD_PERIOD_S   # 13.1723 urad per count and frame
DPS_PER_COUNT = math.degrees(YAW_RAD_PER_COUNT)   # 0.03774 deg/s
LATTICE_STEP_COUNTS = 1.52751423                  # the 0x078/0x079 integer lattice (YAW_DECODE_2026-10-10)
CAR_ZERO = 2048                                   # the CMU digest's nominal zero (0x078 via the VIP)
STOP_EDGE_S = 1.0          # OBD bins must lie inside the stop by this margin (receipt time, BT delay)
MIN_STOP_BINS = 5          # usable OBD bins for a stop zero
# A window counts for the OBD source when every second has a usable OBD bin (not mixed, not near a
# frame_reset, at least one frame) and the window holds >= 90 % of 50 frames per second. Per-second
# counts are not required to reach 40: Bluetooth batches move frames between neighbouring receipt
# seconds without losing rotation (the public log's logger does the same), lost frames do lose it.
OBD_WINDOW_MIN_COVERAGE = 0.90

# Pre-registered decision rule (design note section 15, written before any real OBD data).
RULE = dict(
    adapter_min_coverage=0.60,        # = the app's BAD boundary (docs/OBD_YAW_CHECK_KO.md)
    adapter_min_usable_share=0.50,    # half the OBD seconds unusable: no evaluation possible
    adapter_max_resets_per_h=60.0,    # a restart/switch every minute
    wire_min_coverage=0.90,           # = the app's GOOD boundary
    wire_min_usable_share=0.90,
    wire_max_resets_per_h=6.0,
    min_stops=3,
    zero_max_range_counts=0.80,       # 0.030 deg/s: the phone-gyro "good" bound (3.6 deg in 120 s)
    zero_max_drift_counts=0.80,       # within one stop, last third minus first third
    zero_relative_factor=3.0,         # OBD range <= 0x078 range / 3 on the same stops ...
    zero_relative_min_078_counts=LATTICE_STEP_COUNTS,   # ... when 0x078 moved by >= one lattice step
    min_straight_windows=20,          # 60 s windows with an OBD value
    min_turning_windows=10,
    straight_median_factor=0.5,       # OBD median |err| <= half the car's (YAW_DEADBAND criterion (1))
    straight_max_p90_deg=5.0,
    turning_p90_slack_deg=2.0,        # turning p90 <= car p90 + 2 deg (scale borrowed from 0x078, 1-3 %)
    window_s=60,
)


def wrap(deg):
    return (deg + 540.0) % 360.0 - 180.0


def rows_of(path):
    """(name, line) of every trace.N/pimu.N JSONL member, oldest file first."""
    def key(name):
        m = re.fullmatch(r"(trace|pimu)\.(\d+)\.jsonl", PurePosixPath(name).name)
        return (m[1], -int(m[2])) if m else None
    path = Path(path)
    found = []
    if path.is_dir():
        for p in path.rglob("*.jsonl"):
            if key(p.name):
                found.append((key(p.name), str(p), p.read_bytes()))
    elif path.name.endswith((".tar", ".tar.gz", ".tgz")):
        with tarfile.open(path) as archive:
            for m in archive:
                if m.isfile() and key(m.name):
                    found.append((key(m.name), m.name, archive.extractfile(m).read()))
    elif key(path.name):
        found.append((key(path.name), str(path), path.read_bytes()))
    for _, name, data in sorted(found, key=lambda x: x[0]):
        for raw in data.splitlines():
            try:
                row = json.loads(raw)
            except ValueError:
                continue
            if isinstance(row, dict) and isinstance(row.get("kind"), str):
                yield PurePosixPath(name).name.split(".")[0], row


def _int(v):
    return isinstance(v, int) and not isinstance(v, bool)


class Boot:
    def __init__(self, boot_id):
        self.boot_id = boot_id
        self.digests, self.states, self.stops = [], [], []
        self.bins = {}      # whole CMU second of the row clock -> (bin, bin start s)

    def car_rate(self):
        rates = sorted((sum(d["y1n"]) + d.get("ystn", 0)) / ((d["mono_ns"] - d["since_ns"]) / 1e9)
                       for d in self.digests if d["mono_ns"] > d["since_ns"] and sum(d["y1n"]) + d.get("ystn", 0) > 0)
        return rates[len(rates) // 2] if rates else None


def load(paths):
    boots, trace_boot, pimu_boot = {}, None, None
    for path in paths:
        for stream, row in rows_of(path):
            kind = row["kind"]
            if stream == "trace":
                if kind == "boot" and isinstance(row.get("boot_id"), str):
                    trace_boot = boots.setdefault(row["boot_id"], Boot(row["boot_id"]))
                elif trace_boot is None:
                    continue
                elif kind == "log_digest" and isinstance(row.get("y1"), list) and isinstance(row.get("since_ns"), int):
                    trace_boot.digests.append(row)
                elif kind == "beta_state" and isinstance(row.get("mono_ns"), int):
                    trace_boot.states.append((row["mono_ns"], row.get("to")))
                elif (kind == "yaw_stop" and isinstance(row.get("boot_s"), (int, float)) and _int(row.get("dur_ms"))
                      and _int(row.get("yn")) and _int(row.get("ys"))):
                    trace_boot.stops.append(dict(
                        start_s=float(row["boot_s"]), end_s=float(row["boot_s"]) + row["dur_ms"] / 1000.0,
                        mean_078=CAR_ZERO + row["ys"] / float(row["yn"]) if row["yn"] else None,
                        sd_078=row["sd"] / 100.0 if isinstance(row.get("sd"), (int, float)) else None,
                        open=bool(row.get("open", 0)), source="yaw_stop"))
            else:
                if kind == "pimu_boot" and isinstance(row.get("boot_id"), str):
                    pimu_boot = boots.setdefault(row["boot_id"], Boot(row["boot_id"]))
                elif kind == "pimu" and pimu_boot is not None:
                    for i, b in enumerate(row["d"]):
                        # whole CMU seconds of the row clock; the start keeps the row clock's fraction
                        pimu_boot.bins[row["m"] // 1000 + i] = (b, row["m"] / 1000.0 + i)
    return boots


def digest_stops(boot):
    """Fallback stops when no yaw_stop row exists: runs of whole stationary digests
    (no moving sample in any of the ten seconds, stationary samples present)."""
    stops, cur = [], None
    for d in sorted(boot.digests, key=lambda d: d["since_ns"]):
        still = sum(d["y1n"]) == 0 and d.get("ystn", 0) > 0
        a, b = d["since_ns"] / 1e9, d["mono_ns"] / 1e9
        if still and cur and abs(a - cur["end_s"]) <= 0.2:
            cur["end_s"], cur["s"], cur["n"] = b, cur["s"] + d["yst"], cur["n"] + d["ystn"]
        elif still:
            cur = dict(start_s=a, end_s=b, s=d["yst"], n=d["ystn"])
            stops.append(cur)
        else:
            cur = None
    return [dict(start_s=s["start_s"], end_s=s["end_s"], mean_078=CAR_ZERO + s["s"] / float(s["n"]), sd_078=None,
                 open=False, source="digest_stationary") for s in stops]


def _lsq_slope(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    return None if sxx <= 0 else sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx


def _sd(values):
    if len(values) < 2:
        return None
    m = sum(values) / len(values)
    return math.sqrt(sum((v - m) ** 2 for v in values) / len(values))


def _r(v, digits=4):
    return None if v is None else round(v, digits)


def _pct(values, q):
    values = sorted(values)
    return values[min(len(values) - 1, int(q * len(values)))] if values else None


def classify_bins(boot):
    """(phone series, OBD usable series, OBD coverage stats, excluded seconds)."""
    reset = {s for s, (b, _) in boot.bins.items() if b[4] & FLAG_RESET}
    phone, obd = {}, {}
    st = dict(bins=0, frames=0, window_s=0.0, gap_bins=0, mixed_bins=0, reset_bins=0, excluded_bins=0,
              usable_bins=0, seq_gaps=0, hole_bins=0, count_sum_residual_max=None)
    excluded, counts = [], []
    # Empty seconds (n = 0, flags 0) carry no source. One lying between two OBD bins (no phone bin in
    # between) is an OBD hole: it counts as an OBD bin with 0 frames, so coverage sees stalls.
    last_source, last_sec, pending = None, None, []
    for sec in sorted(boot.bins):
        b, start = boot.bins[sec]
        n, dth, wu_ms, gap, fl = b
        if n == 0 and not fl:
            pending.append(sec)
            phone[sec] = (0.0, 0)
            continue
        source = "obd" if fl & FLAG_OBD else "phone"
        if (source == "obd" and last_source == "obd" and pending and pending[0] == last_sec + 1
                and pending[-1] == sec - 1 and len(pending) == sec - last_sec - 1):
            st["bins"] += len(pending)
            st["hole_bins"] += len(pending)
            counts.extend([0] * len(pending))
        last_source, last_sec, pending = source, sec, []
        if not fl & FLAG_OBD:
            phone[sec] = (math.degrees(dth * 1e-6), n)
            continue
        st["bins"] += 1
        st["frames"] += n
        st["window_s"] += wu_ms / 1000.0
        st["seq_gaps"] += gap
        st["gap_bins"] += 1 if fl & FLAG_OBD_GAP else 0
        st["reset_bins"] += 1 if fl & FLAG_RESET else 0
        counts.append(n)
        if fl & FLAG_GRAVITY:
            st["mixed_bins"] += 1
            continue
        if sec in reset or sec - 1 in reset or sec + 1 in reset:
            st["excluded_bins"] += 1
            excluded.append(sec)
            continue
        # One frame = -(raw - 2047) * 13.1723 urad, rounding carried: the bin's count sum is an
        # integer within 1 urad / 13.17 (0.076 counts). A larger residual: not 0x079 frames of this scale.
        if n:
            s = -dth / OBD_URAD_PER_FRAME_COUNT
            res = abs(s - round(s))
            st["count_sum_residual_max"] = res if st["count_sum_residual_max"] is None else max(st["count_sum_residual_max"], res)
        if n:
            obd[sec] = (math.degrees(dth * 1e-6), n, wu_ms / 1000.0, start)
            st["usable_bins"] += 1
    if st["bins"]:
        st["frames_per_s"] = round(st["frames"] / float(st["bins"]), 2)
        st["frames_per_bin_median"] = _pct(counts, 0.5)
        st["frames_per_bin_p10"] = _pct(counts, 0.1)
        st["coverage"] = round(st["window_s"] / st["bins"], 4)
        st["gap_bin_rate"] = round(st["gap_bins"] / float(st["bins"]), 4)
        st["usable_share"] = round(st["usable_bins"] / float(st["bins"]), 4)
        st["resets_per_hour"] = round(st["reset_bins"] * 3600.0 / st["bins"], 2)
    st["window_s"] = round(st["window_s"], 3)
    st["count_sum_residual_max"] = _r(st["count_sum_residual_max"])
    return phone, obd, st, excluded


def stop_zeros(boot, obd):
    """Per stop: the OBD 0x079 zero from usable OBD bins inside it, and the CMU 0x078 zero."""
    stops = boot.stops or digest_stops(boot)
    out = []
    for st in sorted(stops, key=lambda s: s["start_s"]):
        inside = sorted((start, dth_deg, n, wu_s) for (dth_deg, n, wu_s, start) in obd.values()
                        if n > 0 and wu_s > 0 and start >= st["start_s"] + STOP_EDGE_S
                        and start + 1.0 <= st["end_s"] - STOP_EDGE_S)
        row = dict(start_s=round(st["start_s"], 1), seconds=round(st["end_s"] - st["start_s"], 1), source=st["source"],
                   open=st["open"], zero_078=_r(st["mean_078"], 3), sd_078=st["sd_078"], obd_bins=len(inside),
                   obd_frames=sum(x[2] for x in inside))
        if len(inside) >= MIN_STOP_BINS:
            # per-bin zero on the 2047-based count scale: rate_cw = dtheta / window; count = 2047 - rate/658.615
            per = [OBD_ZERO - math.radians(dth) * 1e6 / wu / OBD_URAD_S_PER_COUNT for _, dth, _, wu in inside]
            total_rate = math.radians(sum(x[1] for x in inside)) * 1e6 / sum(x[3] for x in inside)   # urad/s cw
            zero = OBD_ZERO - total_rate / OBD_URAD_S_PER_COUNT
            k = max(1, len(per) // 3)
            slope = _lsq_slope([x[0] for x in inside], per)
            row.update(zero_obd=round(zero, 3), zero_obd_dps_cw=round(math.degrees(total_rate * 1e-6), 5),
                       zero_rate_urad_s=total_rate, sd_obd=_r(_sd(per)),
                       drift_obd=round(sum(per[-k:]) / k - sum(per[:k]) / k, 3),
                       slope_obd_per_min=None if slope is None else round(slope * 60.0, 4),
                       zero_end_s=st["end_s"])
            if st["mean_078"] is not None:
                row["obd_minus_078"] = round(zero - st["mean_078"], 3)
        out.append(row)
    return out


def across(stops):
    """Zero stability across the stops that have an OBD zero, for 0x079 and 0x078 on the same stops."""
    used = [s for s in stops if s.get("zero_obd") is not None]
    out = dict(stops=len(used), counts_to_dps=round(DPS_PER_COUNT, 5), lattice_step_counts=LATTICE_STEP_COUNTS,
               lattice_step_dps=round(LATTICE_STEP_COUNTS * DPS_PER_COUNT, 5),
               one_frame_count_in_bin_mean="1/n counts (n frames per bin)")
    for key, name in (("zero_obd", "obd"), ("zero_078", "car_078")):
        vals = [s[key] for s in used if s.get(key) is not None]
        if not vals:
            out[name] = None
            continue
        steps = [abs(b - a) for a, b in zip(vals, vals[1:])]
        times = [s["start_s"] for s in used if s.get(key) is not None]
        slope = _lsq_slope(times, vals)
        rng = max(vals) - min(vals)
        out[name] = dict(n=len(vals), mean=round(sum(vals) / len(vals), 3), sd=_r(_sd(vals)), range_counts=round(rng, 3),
                         range_dps=round(rng * DPS_PER_COUNT, 5), range_lattice_steps=round(rng / LATTICE_STEP_COUNTS, 3),
                         max_step_counts=round(max(steps), 3) if steps else None,
                         slope_counts_per_h=None if slope is None else round(slope * 3600.0, 3))
    o, c = out.get("obd"), out.get("car_078")
    if o and c and len(used) >= 2:
        out["range_ratio_078_over_obd"] = round(c["range_counts"] / o["range_counts"], 2) if o["range_counts"] > 0 else None
    within = [abs(s["drift_obd"]) for s in used]
    out["max_abs_within_stop_drift_obd"] = round(max(within), 3) if within else None
    sds = [s["sd_obd"] for s in used if s.get("sd_obd") is not None]
    out["median_within_stop_sd_obd"] = _pct(sds, 0.5)
    return out


def evaluate_boot(boot):
    rate = boot.car_rate()
    phone, obd, obd_cov, excluded = classify_bins(boot)
    stops = stop_zeros(boot, obd)
    if not rate or not boot.digests:
        if not obd_cov["bins"]:
            return None
        return dict(boot_id=boot.boot_id, car_samples_per_s=None, phone_seconds=len(phone), obd=obd_cov,
                    obd_excluded_s=excluded[:200], obd_stops=stops, obd_segments=[], episodes=[],
                    synthetic={str(k): [] for k in WINDOWS_S})
    car, car_n, speed, fixes = {}, {}, [], []
    for d in boot.digests:
        since = d["since_ns"] / 1e9
        for j in range(10):
            car[int(since) + j] = car.get(int(since) + j, 0.0) - math.degrees(YAW_RAD_PER_COUNT * d["y1"][j] / rate)
            car_n[int(since) + j] = car_n.get(int(since) + j, 0) + d["y1n"][j]
        for code in d.get("gc") or []:
            if (d.get("gv") or 0) >= MIN_KMH:
                fixes.append((since + (code // 1000) / 10.0 - GPS_DELAY_S, float(code % 1000)))
        speed.append((since, d["mono_ns"] / 1e9, d.get("speed_kmh_mean")))
    fixes.sort()
    fix_t = [t for t, _ in fixes]
    zeros = sorted((s["zero_end_s"], s["zero_rate_urad_s"], s["zero_obd"]) for s in stops if s.get("zero_obd") is not None)
    zero_ends = [z[0] for z in zeros]

    def kmh(t):
        for a, b, v in speed:
            if a <= t < b and isinstance(v, (int, float)):
                return v
        return None

    def window(t0, t1, slack=2.0):
        i = bisect.bisect_right(fix_t, t0) - 1
        k = bisect.bisect_left(fix_t, t1)
        if i < 0 or k >= len(fixes) or t0 - fix_t[i] > slack or fix_t[k] - t1 > slack:
            return None
        a_t, a_c = fixes[i]
        b_t, b_c = fixes[k]
        truth = wrap(b_c - a_c)
        out = dict(start_s=round(a_t, 1), seconds=round(b_t - a_t, 1), gps_turn_deg=round(truth, 2),
                   **{"class": "straight" if abs(truth) < 10 else "gentle" if abs(truth) <= 45 else "sharp"})
        secs = range(int(math.floor(a_t)), int(math.ceil(b_t)))
        v = [kmh(s) for s in secs]
        v = [x for x in v if x is not None]
        out["kmh_mean"] = round(sum(v) / len(v), 1) if v else None
        z = bisect.bisect_right(zero_ends, a_t) - 1
        zero_dps = math.degrees(zeros[z][1] * 1e-6) if z >= 0 else None
        if z >= 0:
            out["obd_zero"] = dict(counts=zeros[z][2], age_s=round(a_t - zeros[z][0], 1))
        for name, series in (("car", car), ("phone", phone), ("obd", obd)):
            psi, err_area, ok = 0.0, 0.0, True
            if name == "obd" and zero_dps is None:
                ok = False
            if name == "obd" and ok:
                ok = (all(s in obd for s in secs) and
                      sum(obd[s][1] for s in secs) >= OBD_WINDOW_MIN_COVERAGE * 50 * len(secs))
            for idx, s in enumerate(secs):
                if not ok:
                    break
                value = series.get(s)
                if value is None or (name == "phone" and value[1] < MIN_FRAMES):
                    ok = False
                    break
                if name == "car":
                    psi += value
                elif name == "phone":
                    psi += value[0]
                else:
                    psi += value[0] - zero_dps * value[2]     # learned zero times the covered time
                err_area += math.radians(wrap(psi - truth * (idx + 1) / len(secs)))
            if not ok:
                out[name] = None
                continue
            err = wrap(psi - truth)
            # The linear course reference is only meaningful on a straight window.
            jump = round(abs(err_area) * (out["kmh_mean"] or 0) / 3.6, 1) if out["class"] == "straight" else None
            out[name] = dict(heading_error_deg=round(err, 2), exit_jump_m=jump)
        return out

    episodes, start = [], None
    for t, to in sorted(boot.states):
        if to in LIVE and start is None:
            start = t / 1e9
        elif to not in LIVE and start is not None:
            episodes.append(window(start, t / 1e9, EPISODE_SLACK_S))
            start = None
    synthetic = {}
    lo = min(fix_t) if fix_t else 0
    hi = max(fix_t) if fix_t else 0
    for length in WINDOWS_S:
        rows = []
        t = lo
        while t + length <= hi:
            w = window(t, t + length)
            if w and w["kmh_mean"] and w["kmh_mean"] >= MIN_KMH:
                rows.append(w)
            t += STEP_S
        synthetic[str(length)] = rows
    # Stop-to-stop moving segments: the heading change of each source between two stops, the OBD with
    # the zero of the stop before, the car with 2048 (the product) and with that stop's 0x078 mean.
    # A disagreement between two sensors, not an error against a reference (no GPS needed).
    segments = []
    for a, b in zip(stops, stops[1:]):
        if a.get("zero_obd") is None:
            continue
        secs = range(int(math.floor(a["start_s"] + a["seconds"])) - 1, int(math.ceil(b["start_s"])) + 1)
        zero_dps = math.degrees(a["zero_rate_urad_s"] * 1e-6)
        car_nom = sum(car.get(x, 0.0) for x in secs)
        car_learned = car_nom + sum(math.degrees(YAW_RAD_PER_COUNT * (a["zero_078"] - CAR_ZERO) * car_n.get(x, 0) / rate)
                                    for x in secs) if a["zero_078"] is not None else None
        got = [obd[x] for x in secs if x in obd]
        missing = sum(1 for x in secs if x not in obd)
        obd_deg = sum(v[0] - zero_dps * v[2] for v in got)
        segments.append(dict(start_s=round(a["start_s"] + a["seconds"], 1), seconds=len(secs),
                             car_2048_deg=round(car_nom, 2), car_stop_zero_deg=_r(car_learned, 2), obd_deg=round(obd_deg, 2),
                             obd_minus_car_stop_zero_deg=None if car_learned is None else round(obd_deg - car_learned, 2),
                             obd_minus_car_2048_deg=round(obd_deg - car_nom, 2),
                             obd_frame_coverage=round(sum(v[1] for v in got) / (50.0 * len(secs)), 4),
                             obd_complete=missing == 0, obd_missing_s=missing))
    return dict(boot_id=boot.boot_id, car_samples_per_s=round(rate, 2), phone_seconds=len(phone), obd=obd_cov,
                obd_excluded_s=excluded[:200], obd_stops=stops, obd_segments=segments,
                episodes=[e for e in episodes if e], synthetic=synthetic)


def summary(windows):
    out = {}
    for name in ("car", "phone", "obd"):
        errs = sorted(abs(w[name]["heading_error_deg"]) for w in windows if w.get(name))
        jumps = sorted(w[name]["exit_jump_m"] for w in windows if w.get(name) and w[name]["exit_jump_m"] is not None)
        out[name] = dict(n=len(errs),
                         heading_abs_median_deg=errs[len(errs) // 2] if errs else None,
                         heading_abs_p90_deg=errs[min(len(errs) - 1, int(0.9 * len(errs)))] if errs else None,
                         exit_jump_p90_m=jumps[min(len(jumps) - 1, int(0.9 * len(jumps)))] if jumps else None)
    out["phone_missing"] = sum(1 for w in windows if w.get("phone") is None)
    out["obd_missing"] = sum(1 for w in windows if w.get("obd") is None)
    # The car on exactly the windows the OBD source covers (the paired comparison of the decision rule).
    paired = sorted(abs(w["car"]["heading_error_deg"]) for w in windows if w.get("obd") and w.get("car"))
    out["car_on_obd_windows"] = dict(n=len(paired),
                                     heading_abs_median_deg=paired[len(paired) // 2] if paired else None,
                                     heading_abs_p90_deg=paired[min(len(paired) - 1, int(0.9 * len(paired)))] if paired else None)
    return out


def obd_totals(results):
    keys = ("bins", "frames", "gap_bins", "mixed_bins", "reset_bins", "excluded_bins", "usable_bins", "seq_gaps",
            "hole_bins")
    tot = {k: sum(r["obd"][k] for r in results) for k in keys}
    tot["window_s"] = round(sum(r["obd"]["window_s"] for r in results), 3)
    if tot["bins"]:
        tot["frames_per_s"] = round(tot["frames"] / float(tot["bins"]), 2)
        tot["coverage"] = round(tot["window_s"] / tot["bins"], 4)
        tot["gap_bin_rate"] = round(tot["gap_bins"] / float(tot["bins"]), 4)
        tot["usable_share"] = round(tot["usable_bins"] / float(tot["bins"]), 4)
        tot["resets_per_hour"] = round(tot["reset_bins"] * 3600.0 / tot["bins"], 2)
    res = [r["obd"]["count_sum_residual_max"] for r in results if r["obd"]["count_sum_residual_max"] is not None]
    tot["count_sum_residual_max"] = max(res) if res else None
    return tot


def obd_decision(report):
    """The pre-registered rule (design note section 15): wire_candidate, record_only,
    adapter_inadequate, no_obd_data or not_decidable, with every check and its inputs."""
    rule, tot = RULE, report["obd_totals"]
    checks, reasons = {}, []
    app = report.get("app_check")
    if not tot["bins"]:
        if app and app.get("verdict") == "BAD":
            return dict(outcome="adapter_inadequate", reasons=["app parked check BAD"], checks={})
        return dict(outcome="no_obd_data", reasons=["no OBD bins (flag bit 4) in the journal"], checks={})
    adapter_bad = (tot["coverage"] < rule["adapter_min_coverage"] or tot["usable_share"] < rule["adapter_min_usable_share"]
                   or tot["resets_per_hour"] > rule["adapter_max_resets_per_h"] or bool(app and app.get("verdict") == "BAD"))
    # C1 coverage
    c1 = (tot["coverage"] >= rule["wire_min_coverage"] and tot["usable_share"] >= rule["wire_min_usable_share"]
          and tot["resets_per_hour"] <= rule["wire_max_resets_per_h"])
    checks["C1_coverage"] = dict(result="pass" if c1 else "fail", coverage=tot["coverage"], usable_share=tot["usable_share"],
                                 resets_per_hour=tot["resets_per_hour"])
    # C2 zero stability
    z = report["obd_zero"]
    if z["stops"] < rule["min_stops"]:
        checks["C2_zero"] = dict(result="undecidable", stops=z["stops"])
    else:
        o, c = z["obd"], z["car_078"]
        ok = (o["range_counts"] <= rule["zero_max_range_counts"] and
              z["max_abs_within_stop_drift_obd"] <= rule["zero_max_drift_counts"])
        rel = None
        if c and c["range_counts"] >= rule["zero_relative_min_078_counts"]:
            rel = o["range_counts"] <= c["range_counts"] / rule["zero_relative_factor"]
            ok = ok and rel
        checks["C2_zero"] = dict(result="pass" if ok else "fail", stops=z["stops"], obd_range_counts=o["range_counts"],
                                 car_078_range_counts=c["range_counts"] if c else None,
                                 max_within_stop_drift=z["max_abs_within_stop_drift_obd"],
                                 relative_test="not applicable (0x078 moved < one lattice step)" if rel is None else rel)
    # C3 heading error, 60 s windows, paired with the car
    key = str(rule["window_s"])
    s, t = report["synthetic_straight_summary"][key], report["synthetic_turning_summary"][key]
    if s["obd"]["n"] < rule["min_straight_windows"] or t["obd"]["n"] < rule["min_turning_windows"]:
        checks["C3_heading"] = dict(result="undecidable", straight_windows=s["obd"]["n"], turning_windows=t["obd"]["n"])
    else:
        so, sc = s["obd"], s["car_on_obd_windows"]
        to, tc = t["obd"], t["car_on_obd_windows"]
        ok = (so["heading_abs_median_deg"] <= rule["straight_median_factor"] * sc["heading_abs_median_deg"] and
              so["heading_abs_p90_deg"] <= min(sc["heading_abs_p90_deg"], rule["straight_max_p90_deg"]) and
              to["heading_abs_p90_deg"] <= tc["heading_abs_p90_deg"] + rule["turning_p90_slack_deg"])
        checks["C3_heading"] = dict(result="pass" if ok else "fail", straight_obd=so, straight_car=sc, turning_obd=to,
                                    turning_car=tc)
    results = [v["result"] for v in checks.values()]
    if adapter_bad:
        # The other checks are still computed for the record; the outcome says nothing about 0x079 itself.
        outcome = "adapter_inadequate"
        reasons = [text for bad, text in (
            (tot["coverage"] < rule["adapter_min_coverage"], "coverage %s < %.2f" % (tot["coverage"], rule["adapter_min_coverage"])),
            (tot["usable_share"] < rule["adapter_min_usable_share"],
             "usable share %s < %.2f" % (tot["usable_share"], rule["adapter_min_usable_share"])),
            (tot["resets_per_hour"] > rule["adapter_max_resets_per_h"],
             "frame_reset %s/h > %.0f/h" % (tot["resets_per_hour"], rule["adapter_max_resets_per_h"])),
            (bool(app and app.get("verdict") == "BAD"), "app parked check BAD")) if bad]
    elif "fail" in results:
        outcome = "record_only"
        reasons = [k for k, v in checks.items() if v["result"] == "fail"]
    elif "undecidable" in results:
        outcome = "not_decidable"
        reasons = [k for k, v in checks.items() if v["result"] == "undecidable"]
    else:
        outcome = "wire_candidate"
    return dict(outcome=outcome, reasons=reasons, checks=checks,
                note="wire_candidate only means worth proposing as a product input; it needs the AGENTS.md "
                     "exception, an independent review and its own release work")


APP_CHECK = re.compile(r"obd_check_end verdict=(\w+) wall_coverage=([0-9.]+) restarts=(\d+)")


def app_check(path):
    """Last obd_check_end line of the phone app's diagnostic export (the parked check)."""
    found = None
    for line in Path(path).read_text(errors="replace").splitlines():
        m = APP_CHECK.search(line)
        if m:
            found = dict(verdict=m[1], wall_coverage=float(m[2]), restarts=int(m[3]))
            for key in ("fps", "loss_pct", "max_gap_ms", "frames"):
                mm = re.search(r"\b%s=([0-9.]+)" % key, line)
                if mm:
                    found[key] = float(mm[1])
    return found


def build_report(paths, app_log=None):
    boots = load(paths)
    results = [r for r in (evaluate_boot(b) for b in boots.values()) if r]
    report = dict(method="validation/YAW_DEADBAND_2026-10-13.md digest harness; GPS is a reference; OBD zero learned "
                         "at stops (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md section 15)", boots=results)
    allw = {k: [w for r in results for w in r["synthetic"][k]] for k in map(str, WINDOWS_S)}
    report["synthetic_summary"] = {k: summary(v) for k, v in allw.items()}
    report["synthetic_straight_summary"] = {k: summary([w for w in v if w["class"] == "straight"]) for k, v in allw.items()}
    report["synthetic_turning_summary"] = {k: summary([w for w in v if w["class"] != "straight"]) for k, v in allw.items()}
    report["episode_summary"] = summary([e for r in results for e in r["episodes"]])
    report["obd_totals"] = obd_totals(results)
    report["obd_zero"] = across([s for r in results for s in r["obd_stops"]])
    if app_log:
        report["app_check"] = app_check(app_log)
    report["obd_decision"] = obd_decision(report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("inputs", nargs="+", help="exported trip directories, tar files or JSONL files")
    parser.add_argument("--app-log", help="the phone app's diagnostic export (obd_check_end of the parked check)")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    report = build_report(args.inputs, args.app_log)
    results = report["boots"]
    if args.json:
        print(json.dumps(report, indent=1, sort_keys=True))
    else:
        for k, s in report["synthetic_summary"].items():
            print("windows %s s: car %s; phone %s; obd %s; phone missing %d, obd missing %d" %
                  (k, s["car"], s["phone"], s["obd"], s["phone_missing"], s["obd_missing"]))
            for cls in ("straight", "turning"):
                s = report["synthetic_%s_summary" % cls][k]
                print("  %s only: car %s; phone %s; obd %s (car on the same windows %s)" %
                      (cls, s["car"], s["phone"], s["obd"], s["car_on_obd_windows"]))
        s = report["episode_summary"]
        print("GPS-lost episodes: car %s; phone %s; obd %s; phone missing %d, obd missing %d" %
              (s["car"], s["phone"], s["obd"], s["phone_missing"], s["obd_missing"]))
        for r in results:
            for e in r["episodes"]:
                print("  boot %s episode at %.1f s (%.0f s, %s km/h): car %s, phone %s, obd %s" %
                      (r["boot_id"][:8], e["start_s"], e["seconds"], e["kmh_mean"], e["car"], e["phone"], e["obd"]))
        t = report["obd_totals"]
        if t["bins"]:
            print("OBD 0x079 bins: %d, %d frames (%s/s), coverage %s, loss-bit bins %s, usable %s, frame_reset %d (%s/h), "
                  "mixed %d, excluded %d, count-sum residual max %s" %
                  (t["bins"], t["frames"], t["frames_per_s"], t["coverage"], t["gap_bin_rate"], t["usable_share"],
                   t["reset_bins"], t["resets_per_hour"], t["mixed_bins"], t["excluded_bins"], t["count_sum_residual_max"]))
            for r in results:
                for st in r["obd_stops"]:
                    print("  stop at %.1f s (%.1f s, %s): 0x079 zero %s sd %s drift %s (%d bins), 0x078 zero %s" %
                          (st["start_s"], st["seconds"], st["source"], st.get("zero_obd"), st.get("sd_obd"),
                           st.get("drift_obd"), st["obd_bins"], st["zero_078"]))
                for g in r.get("obd_segments", []):
                    print("  segment from %.1f s (%d s): 0x079 %+.2f deg, car 2048 %+.2f, car stop zero %s, "
                          "0x079 - car %s deg (complete %s, missing %d s)" %
                          (g["start_s"], g["seconds"], g["obd_deg"], g["car_2048_deg"], g["car_stop_zero_deg"],
                           g["obd_minus_car_stop_zero_deg"], g["obd_complete"], g["obd_missing_s"]))
            z = report["obd_zero"]
            print("OBD zero across %d stops: 0x079 %s; 0x078 same stops %s; ratio %s" %
                  (z["stops"], z["obd"], z["car_078"], z.get("range_ratio_078_over_obd")))
        d = report["obd_decision"]
        print("OBD decision (pre-registered): %s %s" % (d["outcome"], d["reasons"]))
    return 0 if results else 2


if __name__ == "__main__":
    sys.exit(main())
