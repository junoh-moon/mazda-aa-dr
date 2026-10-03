#!/usr/bin/env python3
"""How long does the DR error budget stay below an accuracy limit?

Reproduces the worst-case budget of src/core/dr_core.c for straight constant-speed travel:
    heading(t)  = h0 + k*t                         (k = yaw error rate, rad/s)
    position(t) = e0 + sv*t + integral(v*2*sin(h/2)) dt   (sv = speed error, m/s)
with the core's 50 ms steps. The terms are summed linearly (bounds, not statistics).

  dr_budget_window.py                 table of seconds until the budget exceeds a limit
  dr_budget_window.py --verify        cross-check the model against the real core (needs build/replay)

The parameter sets are explicit assumptions or observations named in
validation/DR_BUDGET_WINDOW_2026-10-03.md; none is a calibrated Mazda value.
"""
import argparse
import math
import os
import subprocess
import sys
import tempfile

DT = 0.05  # core integration step


def budget_at(v, t, e0, h0, k, sv):
    """Budget after t seconds (same stepping as mx5_dr_step)."""
    e, h, elapsed = e0, h0, 0.0
    while elapsed < t - 1e-12:
        dt = min(DT, t - elapsed)
        h += k * dt
        e += sv * dt
        e += v * 2.0 * math.sin(min(h, math.pi) * 0.5) * dt
        elapsed += dt
    return e


def seconds_until(v, limit, e0, h0, k, sv, horizon=600.0):
    """First time (s) the budget exceeds `limit`, or None within the horizon."""
    e, h, t = e0, h0, 0.0
    if e > limit:
        return 0.0
    while t < horizon:
        h += k * DT
        e += sv * DT
        e += v * 2.0 * math.sin(min(h, math.pi) * 0.5) * DT
        t += DT
        if e > limit:
            return t
    return None


def contributions(v, t, e0, h0, k, sv):
    """Linear decomposition at time t (small-angle form)."""
    return {"anchor position": e0, "speed error": sv * t,
            "anchor heading": v * h0 * t, "yaw rate error": v * k * t * t / 2.0}


def fmt(x):
    return ">600" if x is None else "%.0f" % x


SCENARIOS = [
    # name, e0 m, h0 rad, k rad/s, sv m/s
    ("current defaults (placeholders)", 10.0, 0.15, 0.002, 0.30),
    ("anchor heading 0.05 rad", 10.0, 0.05, 0.002, 0.30),
    ("+ anchor position 5 m", 5.0, 0.05, 0.002, 0.30),
    ("+ speed error 1% (0.14 m/s)", 5.0, 0.05, 0.002, 0.14),
    ("anchor heading 0.03 rad", 5.0, 0.03, 0.002, 0.14),
    ("yaw bias 0.0022 (observed stop-to-stop swing)", 5.0, 0.03, 0.0022, 0.14),
    ("yaw bias 0.0010 (in-motion zero scatter)", 5.0, 0.03, 0.0010, 0.14),
    ("yaw bias 0.0005", 5.0, 0.03, 0.0005, 0.14),
    ("yaw bias 0.0003, heading 0.02", 5.0, 0.02, 0.0003, 0.14),
]


def table(limits, speeds):
    for limit in limits:
        print("\nSeconds until the budget exceeds %.0f m (straight, constant speed):" % limit)
        print("%-47s " % "scenario" + " ".join("%6s" % ("%.0f m/s" % s) for s in speeds))
        print("%-47s " % "" + " ".join("%6s" % ("%.0f km/h" % (s * 3.6)) for s in speeds))
        for name, e0, h0, k, sv in SCENARIOS:
            print("%-47s " % name + " ".join("%6s" % fmt(seconds_until(s, limit, e0, h0, k, sv))
                                            for s in speeds))
    print("\nWhat dominates at 14 m/s (m, linear terms) for the first and last scenario:")
    for idx in (0, 7):
        name, e0, h0, k, sv = SCENARIOS[idx]
        print("  %s" % name)
        for t in (10, 30, 60):
            c = contributions(14.0, t, e0, h0, k, sv)
            print("    t=%2ds: %s   total %.1f m" % (
                t, ", ".join("%s %.1f" % kv for kv in c.items()),
                budget_at(14.0, t, e0, h0, k, sv)))


def verify(replay):
    """Run the real core on constant-speed straight motion and compare budgets."""
    worst = 0.0
    for v, e0, h0 in ((8.0, 1.0, 0.01), (14.0, 10.0, 0.15), (20.0, 5.0, 0.05)):
        lines = ["RESET,1,1,1",
                 "SEED,1,1,1,1,1,1000000000,1700000000000000000,0,0,0,%g,%g,1,1,1,1,REPLAY" % (e0, h0),
                 "CONTROL,1,1,2,2,0"]
        n = 200  # 20 s of 100 ms intervals
        for i in range(1, n + 1):
            s, e = 1000000000 + (i - 1) * 100000000, 1000000000 + i * 100000000
            ev = lambda sid: "%d,1,%d,%d,%d,%d,0,1,1" % (sid, i, s, s, s + 250000000)
            lines.append("MOTION,1,1,2,%d,%d,%d,%d,%g,0,0,2047,1,0,0,0,%s,%s,%s" % (
                i, s, e, e, v, ev(1), ev(2), ev(3)))
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as f:
            f.write("\n".join(lines) + "\n")
            path = f.name
        try:
            out = subprocess.run([replay, path], capture_output=True, text=True, check=True).stdout
        finally:
            os.unlink(path)
        rows = [r.split(",") for r in out.splitlines()[1:] if r.split(",")[1] == "MOTION"]
        for i in (10, 50, 100, 200):
            core = float(rows[i - 1][-1])
            model = budget_at(v, i * 0.1, e0, h0, 0.002, 0.3)
            worst = max(worst, abs(core - model))
            print("v=%4.1f m/s e0=%g h0=%g t=%5.1f s: core %.6f  model %.6f" % (v, e0, h0, i * 0.1, core, model))
    print("max |core - model| = %.2e m" % worst)
    return worst < 1e-3


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verify", metavar="REPLAY", nargs="?", const="build/replay",
                    help="cross-check the model against the real core binary (default build/replay)")
    ap.add_argument("--limits", type=float, nargs="*", default=[30.0, 50.0, 100.0])
    ap.add_argument("--speeds", type=float, nargs="*", default=[5.0, 8.0, 14.0, 20.0, 28.0])
    a = ap.parse_args()
    if a.verify:
        sys.exit(0 if verify(a.verify) else 1)
    table(a.limits, a.speeds)


if __name__ == "__main__":
    main()
