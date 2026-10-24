#!/usr/bin/env python3
"""Generate DHU command files for experiment 14 (screen effect of the two unpublished changes).

Two questions, one package, two sessions. Both are screen observations of the PHONE APP only: the DHU feeds a
scripted car-sensor stream and Naver draws it. Nothing here runs product code, and no result is vehicle evidence.

Q-A (G3 screen loss). The garage carry (validation/G3_WINDOW_MARGIN_2026-10-23.md, default OFF in v1.0.0, menu 12
on) may publish a heading that is restored CARRIED with reason chain_depart. The owner is asked to accept an
unmeasured screen loss: carried headings wrong by 36.6 deg (E-Mart: carried 6.7 vs stock 43.3), 84.2 deg (home
spiral: carried -34.3 vs stock 49.9) and up to 171 deg (the largest divergence in the 2026-10-10/11 export).
Question: does Naver's arrow/route display follow the bearing field at all, and if it does, how wrong does the
screen look while the carried value is published, and how does it recover when the true bearing returns? The
stream is identical across runs except the bearing error, so any difference on screen is the delta.

Q-B (TAN release screen effect). Inside the BETA tunnel replacement the map-constrained mode ends with a hard
transition: the 2026-10-23 diagnostic measured a 15.0 m portal jump in the 53 s case (TAN 29.5 m error vs ordinary
DR 24.2 m), ~36.8 m at a fork mid-release, and 113.3 m in the 111 s case when a blanket chain-end release is
applied. Question: how does a single-fix position jump of 15 / 37 / 113 m along the driven line appear on the
map and in guidance (marker jump, route-deviance display, reroute, banner)? The stream is identical across runs
except the size of the jump applied to one fix at the transition.

Streams. Both use the Seoul route of experiment 13 (validation/DHU_NAVER_EXP13_PLAN_2026-10-12/geometry/
route-seoul.json, ODbL) and its proven origin 1500 m before the in-tunnel fork, so the preparation files and the
Tier 0 stream are byte copies of experiment 13 with only the screenshot tag renamed. The driven stretch stays
before the fork (A ends at fork-1020 m, B at fork-300 m), so no junction is crossed.

A (60 s, V = 12 m/s): 8 s stationary with no valid fix (accuracy NAN, the garage), then 40 s of valid 1 m fixes
whose bearing is true - DELTA, then 12 s of valid fixes with the true bearing (the end condition: GPS-true
bearings resume). DELTA = 0 (control), -36.6, -84.2, +171.0 deg.

B (60 s, V = 20 m/s): 10 s of ordinary GPS fixes, 35 s of tunnel replacement fixes (accuracy 25 m, the published
ceiling), then one transition fix carrying a position jump of JUMP metres along the driven line with 1 m accuracy,
then 15 s of ordinary fixes. JUMP = 0 (control), 15.0, 36.8, 113.3 m.

Not tested here (and not claimed): whether the product really sends these streams on the car, the product's
window/end conditions, tunnel engagement, chain matching, accuracy or position truth. Coordinate-free: the
geometry file is experiment 13's (already committed, ODbL); this package copies no coordinates of its own.

Usage: gen_commands.py [out_dir [run_plan_json]]
"""
import hashlib
import importlib.util
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VAL = os.path.normpath(os.path.join(HERE, ".."))
EXP13_DIR = os.path.join(VAL, "DHU_NAVER_EXP13_PLAN_2026-10-12")


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


E13 = _load("dhu_exp13_for_exp14", os.path.join(EXP13_DIR, "gen_commands.py"))
fmt = E13.fmt

SHOT = "../../../../experiment14/"
T0_SRC, T0_TAG = "T0-S13", "T0-E14"  # exp13 Tier 0 stream, retagged for fresh photos
PREP_SRC = ("S-search", "S-route-y130", "S-route-y225", "S-route-y318", "S-route-retry", "S-gate", "S-gate-nopopup",
            "S-start", "search-again", "end-guidance")

ORIGIN_BEFORE_FORK_M = 1500.0  # the experiment 13 origin; both streams stay before the fork
A_S = 60
A_STILL_S = 8        # stationary, no valid fix (the garage)
A_WRONG_S = 40       # valid fixes carrying the wrong bearing
A_TRUE_S = 12        # valid fixes with the true bearing
A_V = 12.0           # m/s (43 km/h, urban exit speed)
A_ACC = 1.0
B_S = 60
B_PLAIN_S = 10       # ordinary GPS
B_TUNNEL_S = 35      # tunnel replacement, 25 m ceiling
B_AFTER_S = 15       # ordinary GPS after the transition
B_V = 20.0           # m/s (72 km/h, the experiment 13 tunnel speed)
B_ACC_TUNNEL = 25.0
B_ACC_PLAIN = 1.0
B_TRANSITION_T = B_PLAIN_S + B_TUNNEL_S  # the jump lands on this fix (45)

A_PHOTOS = {0, 3, 7, 8, 9, 15, 30, 47, 48, 50, 55, 59}
B_PHOTOS = {0, 5, 9, 10, 20, 30, 44, 45, 46, 48, 52, 59}

# tag -> (delta deg, session, question)  (A family: the published bearing is true + delta)
A_RUNS = {
    "G0-r1": (0.0, 1, "control: the bearing field equals the true road bearing"),
    "G37-r1": (-36.6, 1, "E-Mart 2026-10-17: carried 6.7 deg where stock had 43.3 deg"),
    "G84-r1": (-84.2, 1, "home spiral 2026-10-10/11: carried -34.3 deg where stock had 49.9 deg"),
    "G171-r1": (171.0, 1, "the largest carried-vs-stock divergence in the 2026-10-10/11 export"),
}
# tag -> (jump m, session, question)  (B family: the transition fix is shifted JUMP metres along the line)
B_RUNS = {
    "J0-r1": (0.0, 2, "control: no jump, the fix stays on the true position"),
    "J15-r1": (15.0, 2, "53 s tunnel: the measured 15.0 m portal jump (TAN 29.5 m vs ordinary 24.2 m)"),
    "J37-r1": (36.8, 2, "fork mid-release: the ~36.8 m jump measured on a synthetic chain"),
    "J113-r1": (113.3, 2, "111 s case with a blanket chain-end release: the 113.3 m jump"),
}
SESSIONS = {1: ["G0-r1", "G37-r1", "G84-r1", "G171-r1"], 2: ["J0-r1", "J15-r1", "J37-r1", "J113-r1"]}
REQUIRED = {1: ["G0-r1", "G37-r1"], 2: ["J0-r1", "J15-r1"]}
BUDGET_MIN = 25
ENV_MIN = 2.0
TIER0_MIN = {1: 7.0, 2: 8.0}  # session 1 keyboard search (the experiment 13 path); session 2 recents
RUN_MIN = 2.5  # a 60 s body with 12 photos plus start-to-start overhead
DESTINATIONS = list(E13.DESTINATIONS["R"])  # whichever the Tier 0 gate confirms routes through the tunnel


def read_lines(path):
    with open(path) as f:
        return f.read().split("\n")[:-1]


def emit(lines, frames, tag, t, line, rec, photo):
    lines.append(line)
    rec["t"] = t
    if photo:
        name = f"{tag}-{t:03d}.png"
        lines.extend(["sleep 0.2", f"screenshot {SHOT}{name}", "sleep 0.8"])
        rec["screenshot"] = name
        frames.append(rec)
    else:
        lines.append("sleep 1")


def build_a(r, tag, delta):
    """Q-A: stationary without a fix, then valid fixes carrying true + delta, then the true bearing."""
    lines, frames = [], []
    d0 = r.fork - ORIGIN_BEFORE_FORK_M
    for t in range(A_S):
        if t < A_STILL_S:
            lat, lon, road = r.at(d0)
            phase, acc, spd, d = "still", None, 0.0, d0
        else:
            d = d0 + A_V * (t - A_STILL_S)
            lat, lon, road = r.at(d)
            phase = "carry" if t < A_STILL_S + A_WRONG_S else "true"
            acc, spd = A_ACC, A_V
        sent = road if phase == "true" else (road + delta) % 360.0
        rec = dict(phase=phase, accuracy_m=None if acc is None else round(acc, 3), speed_sent_mps=round(spd, 3),
                   bearing_sent_deg=round(sent, 1), road_bearing_deg=round(road, 1), delta_deg=None if phase == "still" else delta,
                   from_fork_m=round(d - r.fork, 1))
        emit(lines, frames, tag, t, fmt(lat, lon, acc, spd, sent), rec, t in A_PHOTOS)
    return lines, frames


def build_b(r, tag, jump):
    """Q-B: ordinary fixes, 25 m tunnel fixes, one shifted transition fix, ordinary fixes again."""
    lines, frames = [], []
    d0 = r.fork - ORIGIN_BEFORE_FORK_M
    d = d0
    for t in range(B_S):
        if t < B_PLAIN_S:
            phase, acc = "plain", B_ACC_PLAIN
        elif t < B_TRANSITION_T:
            phase, acc = "tunnel", B_ACC_TUNNEL
        else:
            phase, acc = "after", B_ACC_PLAIN
        d_true = d0 + B_V * t
        d_sent = d_true + (jump if t >= B_TRANSITION_T else 0.0)
        lat, lon, road = r.at(d_sent)
        rec = dict(phase=phase, accuracy_m=acc, speed_sent_mps=B_V, bearing_sent_deg=round(road, 1),
                   from_fork_m=round(d_sent - r.fork, 1), from_fork_true_m=round(d_true - r.fork, 1),
                   jump_m=jump if t >= B_TRANSITION_T else 0.0)
        emit(lines, frames, tag, t, fmt(lat, lon, acc, B_V, road), rec, t in B_PHOTOS)
    return lines, frames


def t0(r, tag):
    """Experiment 13 Tier 0 (20 s stationary stream at the same origin) with the tag renamed."""
    out = []
    for x in read_lines(os.path.join(EXP13_DIR, "commands", T0_SRC + ".txt")):
        if x.startswith("screenshot "):
            x = x.replace(f"{E13.SHOT}{T0_SRC}-", f"{SHOT}{tag}-")
        out.append(x)
    return out


def generate(out_dir, plan_path=None):
    doc, r, m = E13.load_routes()
    os.makedirs(os.path.join(out_dir, "prep"), exist_ok=True)

    def write(name, lines):
        with open(os.path.join(out_dir, name + ".txt"), "w") as f:
            f.write("\n".join(lines) + "\n")

    for name in PREP_SRC:  # byte copies: the experiment 13 preparation at the same origin
        shutil.copyfile(os.path.join(EXP13_DIR, "commands", "prep", name + ".txt"),
                        os.path.join(out_dir, "prep", name + ".txt"))
    write(T0_TAG, t0(r, T0_TAG))

    runs = {}
    for family, table, builder in (("A", A_RUNS, build_a), ("B", B_RUNS, build_b)):
        for tag, (value, session, q) in table.items():
            lines, frames = builder(r, tag, value)
            write(tag, lines)
            entry = dict(tag=tag, session=session, question=q, duration_s=A_S if family == "A" else B_S, frames=frames)
            if family == "A":
                entry.update(family="A (bearing)", delta_deg=value,
                             still_t=[0, A_STILL_S - 1], carry_t=[A_STILL_S, A_STILL_S + A_WRONG_S - 1],
                             true_t=[A_STILL_S + A_WRONG_S, A_S - 1])
            else:
                entry.update(family="B (jump)", jump_m=value, plain_t=[0, B_PLAIN_S - 1],
                             tunnel_t=[B_PLAIN_S, B_TRANSITION_T - 1], transition_t=B_TRANSITION_T,
                             after_t=[B_TRANSITION_T, B_S - 1])
            runs[tag] = entry
    est = {}
    for s, order in SESSIONS.items():
        for i, tag in enumerate(order):
            est[tag] = ENV_MIN + TIER0_MIN[s] + RUN_MIN * (i + 1)
    lat0, lon0, road0 = r.at(r.fork - ORIGIN_BEFORE_FORK_M)
    plan = dict(
        question="Screen effect of (A) a carried heading published on the first valid fixes and (B) a single-fix "
                 "position jump at the end of tunnel replacement: what does Naver draw, and for how long?",
        geometry=dict(source="exp13 geometry/route-seoul.json (ODbL), not copied",
                      fork=doc["fork"], origin_before_fork_m=ORIGIN_BEFORE_FORK_M,
                      origin=[round(lat0, 7), round(lon0, 7)], origin_bearing_deg=round(road0, 1),
                      a_end_from_fork_m=round(A_V * (A_S - A_STILL_S) - ORIGIN_BEFORE_FORK_M, 1),
                      b_end_from_fork_m=round(B_V * (B_S - 1) - ORIGIN_BEFORE_FORK_M, 1),
                      destinations=DESTINATIONS),
        timeline=dict(
            A=dict(duration_s=A_S, still_s=A_STILL_S, carry_s=A_WRONG_S, true_s=A_TRUE_S, speed_mps=A_V,
                   accuracy_valid_m=A_ACC, bearing="true + DELTA while carrying, true afterwards"),
            B=dict(duration_s=B_S, plain_s=B_PLAIN_S, tunnel_s=B_TUNNEL_S, after_s=B_AFTER_S, speed_mps=B_V,
                   accuracy_tunnel_m=B_ACC_TUNNEL, accuracy_plain_m=B_ACC_PLAIN,
                   transition_t=B_TRANSITION_T, jump="the transition fix and every later fix are shifted JUMP m along the line")),
        prep=dict(files="byte copies of exp13 commands/prep (same origin)", list=list(PREP_SRC),
                  tier0=f"{T0_TAG}.txt (exp13 {T0_SRC} retagged)", placeholder="@TAG@ in screenshot lines only"),
        tier0_min=TIER0_MIN, sessions=SESSIONS, required=REQUIRED, budget_min=BUDGET_MIN, env_min=ENV_MIN,
        run_min=RUN_MIN, cumulative_estimate_min=est, runs=runs)
    if plan_path:
        with open(plan_path, "w") as f:
            json.dump(plan, f, ensure_ascii=False, indent=1)
            f.write("\n")
    return plan


def main(argv):
    out = argv[1] if len(argv) > 1 else os.path.join(HERE, "commands")
    plan_path = argv[2] if len(argv) > 2 else os.path.join(HERE, "run-plan.json")
    p = generate(out, plan_path)
    g = p["geometry"]
    print(f"origin {g['origin']} bearing {g['origin_bearing_deg']}; A ends {g['a_end_from_fork_m']} m, B {g['b_end_from_fork_m']} m from the fork")
    for tag, r in p["runs"].items():
        v = f"{r['delta_deg']:+.1f} deg" if r["family"].startswith("A") else f"{r['jump_m']:.1f} m"
        print(f"{tag}: session {r['session']}, {v}, photos {len(r['frames'])}, estimate {p['cumulative_estimate_min'][tag]} min")


if __name__ == "__main__":
    main(sys.argv)
