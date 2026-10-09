#!/usr/bin/env python3
"""Generate DHU `location` command files for experiment 8 (which STOPPED form does Naver accept inside the Baekyang tunnel?).

Derived from validation/DHU_NAVER_EXP7_PLAN_2026-10-09/gen_commands.py (its Route/fmt/geometry helpers are imported, the route geometry
and the setup template are byte copies). Every ST run uses the exp7 H8 profile: fast profile (20 approach fixes at 14 m/s ending 20 m
inside the entrance, tunnel at 14 m/s, accuracy 40 m, correct bearing), a 30 s standstill at t=107..136 in the middle of the tunnel, moving
again at 14 m/s, 30 exit fixes (252 s). ONLY the 30 stopped fixes differ between ST0..ST7:
  location <lat> <lon> <accuracy_m|NAN> <altitude|NAN> <speed_mps> <bearing_deg|NAN>
ST0 is the product's current stopped form (= exp7 H8: speed 0, the identical position every second, accuracy 40, bearing held).
ST8 is the moving positive control without a stop (= exp7 H0, 222 s).
Usage: gen_commands.py [out_dir [run_plan_json]]
"""
import importlib.util
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXP7_DIR = os.path.join(HERE, "..", "DHU_NAVER_EXP7_PLAN_2026-10-09")


def _load_exp7():
    spec = importlib.util.spec_from_file_location("dhu_exp7_for_exp8", os.path.join(EXP7_DIR, "gen_commands.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


E7 = _load_exp7()
Route, fmt, offset_position = E7.Route, E7.fmt, E7.offset_position

SHOT = "../../../../experiment8/"
APPROACH_FIXES = E7.APPROACH_FIXES  # 20
APPROACH_END_INSIDE_M = E7.APPROACH_END_INSIDE_M  # 20 m
EXIT_FIXES = E7.EXIT_FIXES  # 30
SPEED = E7.SPEED  # 14 m/s
ACC_TUNNEL = E7.ACC_TUNNEL  # 40 m
ACC_GPS = E7.ACC_GPS  # 5 m
STOP_S = E7.STOP_S  # 30 s
ENTRANCE_DENSE = E7.ENTRANCE_DENSE  # 22, 24, 26, 28

JITTER_SEED = 20261010
JITTER_RADIUS_M = 1.5  # bound of the pseudo-random walk around the stop position (radial, metres)
JITTER_STEP_M = (0.3, 0.9)  # each second moves by 0.3-0.9 m, so no two consecutive fixes are identical
STOP_GRID = (100, 150, 4)  # photos every 4 s around the stop (rejection onset)
FREEZE_GRID = (186, 218, 4)  # photos every 4 s where exp7 saw the post-stop freeze (first frozen photo t=200, resumed t=224)

# Stopped-segment forms. acc None = NAN (stock mode 0). pos: hold (identical), creep (forward along the route at `creep` m/s),
# jitter (deterministic bounded walk). speed "last" = the last moving speed (14 m/s), as the stock mode-0 form repeats it.
RUNS = {
    "ST0-current": dict(stop=dict(acc=40.0, speed=0.0, pos="hold")),
    "ST1-acc5": dict(stop=dict(acc=5.0, speed=0.0, pos="hold")),
    "ST2-acc15": dict(stop=dict(acc=15.0, speed=0.0, pos="hold")),
    "ST3-jitter-acc40": dict(stop=dict(acc=40.0, speed=0.0, pos="jitter")),
    "ST4-jitter-acc5": dict(stop=dict(acc=5.0, speed=0.0, pos="jitter")),
    "ST5-creep030-acc40": dict(stop=dict(acc=40.0, speed=0.3, pos="creep", creep=0.3)),
    "ST6-slow005-jitter-acc40": dict(stop=dict(acc=40.0, speed=0.05, pos="jitter")),  # = ST3 positions, speed 0.05
    "ST7-stockmode0": dict(stop=dict(acc=None, speed="last", pos="hold")),  # exp6 T0M form at the stop position
    "ST8-nostop": dict(),  # positive control = exp7 H0
}
REPS = 2
# Priority tiers (fixed before the data). Tier 1 + Tier 2 = one repeat (r1) of every form; Tier 1 alone fits the 35-45 min target.
# Tier 3 is the CANDIDATE list in its fixed relative order; the README rule keeps ST0-r2 (first) and ST8-r2 (last) always and only
# those form r2s whose r1 was STOP-ACCEPTED or ambiguous (ST7: no POST-STOP-FREEZE and no harm). Order of the forms = preference order.
TIERS = [
    ["ST8-nostop-r1", "ST0-current-r1", "ST1-acc5-r1", "ST3-jitter-acc40-r1", "ST5-creep030-acc40-r1"],
    ["ST4-jitter-acc5-r1", "ST6-slow005-jitter-acc40-r1", "ST2-acc15-r1", "ST7-stockmode0-r1"],
    ["ST0-current-r2", "ST3-jitter-acc40-r2", "ST6-slow005-jitter-acc40-r2", "ST5-creep030-acc40-r2", "ST2-acc15-r2",
     "ST4-jitter-acc5-r2", "ST1-acc5-r2", "ST7-stockmode0-r2", "ST8-nostop-r2"],
]
ORDER = [tag for tier in TIERS for tag in tier]
TIER3_ALWAYS = ("ST0-current-r2", "ST8-nostop-r2")
# Time per run outside the input itself: measured in exp7 (median gap between runs about 2.7 min incl. DHU/guidance restart, setup and
# saving), so 2.0-3.0 min; plus the initial environment in tier 1.
OVERHEAD_MIN = (2.0, 3.0)
ENV_MIN = 10.0


class Lcg:
    """Tiny deterministic generator (glibc-style LCG constants), independent of the Python version's `random` module."""

    def __init__(self, seed):
        self.x = seed & 0x7FFFFFFF

    def uniform(self):
        self.x = (1103515245 * self.x + 12345) & 0x7FFFFFFF
        return self.x / 2147483648.0


def jitter_walk(n, seed=JITTER_SEED, radius=JITTER_RADIUS_M, step=JITTER_STEP_M):
    """n (east, north) offsets in metres: a bounded random walk starting one step away from (0, 0).

    Every step has length in `step` and a random direction; a step that would leave the disk of `radius` is redrawn (rejection
    sampling, still deterministic). Hence |offset| <= radius and consecutive offsets differ by >= step[0]."""
    rng = Lcg(seed)
    pe = pn = 0.0
    out = []
    for _ in range(n):
        while True:
            ang = 2.0 * math.pi * rng.uniform()
            length = step[0] + (step[1] - step[0]) * rng.uniform()
            ce, cn = pe + length * math.sin(ang), pn + length * math.cos(ang)
            if math.hypot(ce, cn) <= radius:
                break
        pe, pn = ce, cn
        out.append((pe, pn))
    return out


def timeline_for(route, stop):
    """Truth timeline [(t, phase, along-route metres, speed, stopped index or None)], identical to exp7 H0 / H8."""
    d_start_tun = route.entrance + APPROACH_END_INSIDE_M
    d_mid = d_start_tun + (route.exit - d_start_tun) / 2
    tl = [(k, "approach", d_start_tun - SPEED * (APPROACH_FIXES - 1 - k), SPEED, None) for k in range(APPROACH_FIXES)]
    d, t, done, window = d_start_tun, APPROACH_FIXES - 1, False, None
    while True:
        t += 1
        d += SPEED
        if d >= route.exit:
            break
        tl.append((t, "tunnel", d, SPEED, None))
        if stop and not done and d >= d_mid:
            window = [t + 1, t + STOP_S]
            for k in range(STOP_S):
                t += 1
                tl.append((t, "tunnel", d, 0.0, k))
            done = True
    exit_start = t
    for k in range(EXIT_FIXES):
        if k:
            d += SPEED
        tl.append((exit_start + k, "exit", d, SPEED, None))
    return tl, window, exit_start


def photo_times(tl, window, exit_start):
    """exp7 schedule (10 s grid, dense entrance, 2 s after the exit, last second; the stop extras) plus, for stopping runs,
    every 4 s in STOP_GRID and FREEZE_GRID."""
    t_end = tl[-1][0]
    shots = {x[0] for x in tl if x[0] % 10 == 0}
    shots |= set(ENTRANCE_DENSE)
    shots |= set(range(exit_start, exit_start + 11, 2))
    shots.add(t_end)
    if window:
        s0, s1 = window
        shots |= {s0, s0 + 2, s1, s1 + 3}
        shots |= set(range(STOP_GRID[0], STOP_GRID[1] + 1, STOP_GRID[2]))
        shots |= set(range(FREEZE_GRID[0], FREEZE_GRID[1] + 1, FREEZE_GRID[2]))
    return shots


def build_run(route, tag, params):
    """Return (lines without setup, plan dict). Times t are seconds since the first approach fix."""
    form = params.get("stop")
    tl, window, exit_start = timeline_for(route, form is not None)
    shots = photo_times(tl, window, exit_start)
    jit = jitter_walk(STOP_S)
    t_end = tl[-1][0]
    lines, frames = [], []
    for t, phase, d, v, k in tl:
        lat, lon, road = route.at(d)
        rec = dict(t=t, phase=phase, truth_from_entrance_m=round(d - route.entrance, 1),
                   truth_to_exit_m=round(route.exit - d, 1), truth_progress_m=round(d - tl[0][2], 1),
                   speed_mps=round(v, 3), stopped=k is not None, road_bearing_deg=round(road, 1))
        if k is not None:
            # The stopped segment: anchored at the last moving fix (truth d does not change while stopped).
            slat, slon, sbrg = lat, lon, road
            offset = 0.0
            if form["pos"] == "creep":
                offset = form["creep"] * (k + 1)
                slat, slon, _ = route.at(d + offset)
            elif form["pos"] == "jitter":
                slat, slon = offset_position(lat, lon, jit[k])
                offset = math.hypot(*jit[k])
            spd = SPEED if form["speed"] == "last" else form["speed"]
            line = fmt(slat, slon, form["acc"], spd, sbrg)
            rec.update(accuracy_m=form["acc"], speed_sent_mps=round(spd, 3), bearing_sent_deg=round(sbrg, 1),
                       stop_index=k, stop_offset_m=round(offset, 2), stop_form=form["pos"])
        elif phase == "tunnel":
            line = fmt(lat, lon, ACC_TUNNEL, v, road)
            rec.update(accuracy_m=ACC_TUNNEL, speed_sent_mps=round(v, 3), bearing_sent_deg=round(road, 1))
        else:
            line = fmt(lat, lon, ACC_GPS, v, road)
            rec.update(accuracy_m=ACC_GPS, speed_sent_mps=round(v, 3), bearing_sent_deg=round(road, 1))
        lines.append(line)
        if t in shots:
            name = f"{tag}-{t:03d}.png"
            lines += ["sleep 0.2", f"screenshot {SHOT}{name}", "sleep 0.8"]
            rec["screenshot"] = name
            frames.append(rec)
        else:
            lines.append("sleep 1")
    tunnel_ts = [x[0] for x in tl if x[1] == "tunnel"]
    start = route.at(tl[0][2])
    plan = dict(tag=tag, stop_form=form, duration_s=t_end + 1, approach=[0, APPROACH_FIXES - 1],
                tunnel=[tunnel_ts[0], tunnel_ts[-1]], exit=[exit_start, t_end], stop=window,
                start=[round(start[0], 7), round(start[1], 7), round(start[2], 1)], frames=frames)
    return lines, plan


def setup_lines(template, tag, start):
    text = template.replace("@START@", f"{start[0]:.7f} {start[1]:.7f}").replace("@BRG@", f"{start[2]:.1f}")
    return text.replace("@SHOT@", SHOT).replace("@TAG@", tag).rstrip("\n").splitlines()


def estimate(plans, runs, env):
    input_s = sum(plans[t]["duration_s"] for t in runs)
    return input_s, [round(input_s / 60 + len(runs) * o + env) for o in OVERHEAD_MIN]


def generate(out_dir, plan_path=None, route_path=None):
    route = Route(route_path or os.path.join(HERE, "geometry", "route.json"))
    with open(os.path.join(HERE, "setup-template.txt")) as f:
        template = f.read()
    os.makedirs(out_dir, exist_ok=True)
    plans = {}
    for base, params in RUNS.items():
        for rep in range(1, REPS + 1):
            tag = f"{base}-r{rep}"
            body, plan = build_run(route, tag, params)
            lines = setup_lines(template, tag, plan["start"]) + body
            with open(os.path.join(out_dir, tag + ".txt"), "w") as f:
                f.write("\n".join(lines) + "\n")
            plans[tag] = plan
    assert sorted(ORDER) == sorted(plans), "ORDER must list every run exactly once"
    tiers = []
    for n, runs in enumerate(TIERS, 1):
        input_s, est = estimate(plans, runs, ENV_MIN if n == 1 else 0.0)
        photos = sum(len(plans[t]["frames"]) + 1 for t in runs)
        tier = dict(tier=n, runs=runs, input_s=input_s, photos=photos, estimate_min=est)
        if n == 3:
            min_s, min_est = estimate(plans, list(TIER3_ALWAYS), 0.0)
            per_form = estimate(plans, ["ST3-jitter-acc40-r2"], 0.0)[1]
            tier.update(always=list(TIER3_ALWAYS), minimum_input_s=min_s, minimum_estimate_min=min_est,
                        per_added_form_min=per_form, note="candidate list; the README rule selects the runs")
        tiers.append(tier)
    jit = jitter_walk(STOP_S)
    doc = dict(geometry=dict(entrance_m=round(route.entrance, 1), exit_m=round(route.exit, 1),
                             tunnel_length_m=round(route.exit - route.entrance, 1), route_length_m=round(route.length, 1)),
               jitter=dict(seed=JITTER_SEED, radius_m=JITTER_RADIUS_M, step_m=list(JITTER_STEP_M),
                           offsets_en_m=[[round(e, 3), round(n, 3)] for e, n in jit]),
               order=ORDER, tiers=tiers, runs=plans)
    if plan_path:
        with open(plan_path, "w") as f:
            json.dump(doc, f, ensure_ascii=False, indent=1)
            f.write("\n")
    return route, doc


def main(argv):
    out = argv[1] if len(argv) > 1 else os.path.join(HERE, "commands")
    plan = argv[2] if len(argv) > 2 else os.path.join(HERE, "run-plan.json")
    _, doc = generate(out, plan)
    for tag in doc["order"]:
        p = doc["runs"][tag]
        print(f"{tag:30s} {p['duration_s']:4d} s  {len(p['frames']) + 1:3d} photos  tunnel t={p['tunnel'][0]}..{p['tunnel'][1]}"
              f"  exit t={p['exit'][0]}" + (f"  stop t={p['stop'][0]}..{p['stop'][1]}" if p["stop"] else ""))
    for tier in doc["tiers"]:
        extra = ""
        if tier["tier"] == 3:
            extra = (f" (candidates; minimum {tier['always']}: {tier['minimum_input_s']} s, about "
                     f"{tier['minimum_estimate_min'][0]}-{tier['minimum_estimate_min'][1]} min; each added form about "
                     f"{tier['per_added_form_min'][0]}-{tier['per_added_form_min'][1]} min)")
        print(f"tier {tier['tier']}: {len(tier['runs'])} runs, {tier['input_s']} s input, {tier['photos']} photos, "
              f"about {tier['estimate_min'][0]}-{tier['estimate_min'][1]} min{extra}")
    radii = [math.hypot(*o) for o in doc["jitter"]["offsets_en_m"]]
    print(f"jitter seed {JITTER_SEED}: max radius {max(radii):.2f} m")


if __name__ == "__main__":
    main(sys.argv)
