#!/usr/bin/env python3
"""Offline counterfactual: phone gyro (pimu rows) versus the car's yaw.

validation/PHONE_IMU_CHANNEL_2026-10-14.md, stage 1 (record only). Input: an
exported trip (directories or tar files) that holds the trace journal
(trace.N.jsonl: boot, log_digest with the yaw digest fields y1/y1n/gc,
beta_state) and the phone IMU journal (pimu.N.jsonl). Nothing is replayed
into the product; this only compares two recorded rotation sources.

Method (the digest harness of validation/YAW_DEADBAND_2026-10-13.md):
  * per CMU second (CLOCK_MONOTONIC, same boot): the car's heading change is
    -YAW_RAD_PER_COUNT * y1 / rate (rate = yaw samples per second, the
    session median), the phone's is the pimu bin's sum of dtheta (clockwise
    positive, the GPS course direction); GPS courses come from the digest's
    gc fix codes (deciseconds after since_ns * 1000 + course), delay 1.2 s.
  * windows: every GPS-lost episode (beta_state into GPS_LOST/ENGAGED until it
    leaves them; fixes up to 15 s outside) and synthetic windows of 30/60/120 s starting every 10 s,
    each from the last GPS course at or before the start to the first one at
    or after the end; the course at the start plus the integrated change is
    compared with the course at the end (heading error, deg), and the
    exit jump is approximated by the digest mean speed times the integral of
    the heading error against a linear course between the two fixes (m).
    The exit jump is computed only for straight windows (GPS turn < 10 deg,
    the classes of that record: straight < 10, gentle 10..45, sharp > 45).
  * a window counts for the phone only if every second has a pimu bin with
    at least 40 frames (80 % of 50 Hz); otherwise it is reported as missing.
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


class Boot:
    def __init__(self, boot_id):
        self.boot_id = boot_id
        self.digests, self.states, self.bins = [], [], {}

    def car_rate(self):
        rates = sorted((sum(d["y1n"]) + d.get("ystn", 0)) / ((d["mono_ns"] - d["since_ns"]) / 1e9)
                       for d in self.digests if d["mono_ns"] > d["since_ns"] and sum(d["y1n"]) > 0)
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
            else:
                if kind == "pimu_boot" and isinstance(row.get("boot_id"), str):
                    pimu_boot = boots.setdefault(row["boot_id"], Boot(row["boot_id"]))
                elif kind == "pimu" and pimu_boot is not None:
                    for i, b in enumerate(row["d"]):
                        pimu_boot.bins[row["m"] // 1000 + i] = b  # whole CMU seconds of the row clock
    return boots


def evaluate_boot(boot):
    rate = boot.car_rate()
    if not rate or not boot.digests:
        return None
    car, phone, speed, fixes = {}, {}, [], []
    for d in boot.digests:
        since = d["since_ns"] / 1e9
        for j in range(10):
            car[int(since) + j] = car.get(int(since) + j, 0.0) - math.degrees(YAW_RAD_PER_COUNT * d["y1"][j] / rate)
        for code in d.get("gc") or []:
            if (d.get("gv") or 0) >= MIN_KMH:
                fixes.append((since + (code // 1000) / 10.0 - GPS_DELAY_S, float(code % 1000)))
        speed.append((since, d["mono_ns"] / 1e9, d.get("speed_kmh_mean")))
    for sec, b in boot.bins.items():
        n, dth = b[0], b[1]
        phone[sec] = (math.degrees(dth * 1e-6), n)
    fixes.sort()
    fix_t = [t for t, _ in fixes]

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
        for name, series, need in (("car", car, None), ("phone", phone, MIN_FRAMES)):
            psi, err_area, ok = 0.0, 0.0, True
            for idx, s in enumerate(secs):
                value = series.get(s)
                if value is None or (need and value[1] < need):
                    ok = False
                    break
                psi += value[0] if need else value
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
    return dict(boot_id=boot.boot_id, car_samples_per_s=round(rate, 2), phone_seconds=len(phone),
                episodes=[e for e in episodes if e], synthetic=synthetic)


def summary(windows):
    out = {}
    for name in ("car", "phone"):
        errs = sorted(abs(w[name]["heading_error_deg"]) for w in windows if w.get(name))
        jumps = sorted(w[name]["exit_jump_m"] for w in windows if w.get(name) and w[name]["exit_jump_m"] is not None)
        out[name] = dict(n=len(errs),
                         heading_abs_median_deg=errs[len(errs) // 2] if errs else None,
                         heading_abs_p90_deg=errs[min(len(errs) - 1, int(0.9 * len(errs)))] if errs else None,
                         exit_jump_p90_m=jumps[min(len(jumps) - 1, int(0.9 * len(jumps)))] if jumps else None)
    out["phone_missing"] = sum(1 for w in windows if w.get("phone") is None)
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("inputs", nargs="+", help="exported trip directories, tar files or JSONL files")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    boots = load(args.inputs)
    results = [r for r in (evaluate_boot(b) for b in boots.values()) if r]
    report = dict(method="validation/YAW_DEADBAND_2026-10-13.md digest harness; GPS is a reference", boots=results)
    allw = {k: [w for r in results for w in r["synthetic"][k]] for k in map(str, WINDOWS_S)}
    report["synthetic_summary"] = {k: summary(v) for k, v in allw.items()}
    report["synthetic_straight_summary"] = {k: summary([w for w in v if w["class"] == "straight"]) for k, v in allw.items()}
    report["episode_summary"] = summary([e for r in results for e in r["episodes"]])
    if args.json:
        print(json.dumps(report, indent=1, sort_keys=True))
    else:
        for k, s in report["synthetic_summary"].items():
            print("windows %s s: car %s; phone %s; phone missing %d" % (k, s["car"], s["phone"], s["phone_missing"]))
            s = report["synthetic_straight_summary"][k]
            print("  straight only: car %s; phone %s" % (s["car"], s["phone"]))
        s = report["episode_summary"]
        print("GPS-lost episodes: car %s; phone %s; phone missing %d" % (s["car"], s["phone"], s["phone_missing"]))
        for r in results:
            for e in r["episodes"]:
                print("  boot %s episode at %.1f s (%.0f s, %s km/h): car %s, phone %s" %
                      (r["boot_id"][:8], e["start_s"], e["seconds"], e["kmh_mean"], e["car"], e["phone"]))
    return 0 if results else 2


if __name__ == "__main__":
    sys.exit(main())
