#!/usr/bin/env python3
"""Generate DHU `location` command files for experiment 10 (G3: bearing shown on a FRESH Android Auto session at standstill).

Every run is one fresh DHU session with a freshly started Naver process (exp7 H6 reset, no `pm clear`). From the very first
`location` line the head unit sends stationary fixes at P0 (route.json, 2,950 m along the route = 175 m after the Baekyang tunnel
exit on 모라로, a road segment Naver followed toward 모라역 in every exp7 CLEAN-EXIT run). Guidance to 모라역 is started by the
exp6-9 taps while the condition fixes keep streaming, the car stays still for 60 s with guidance, then moves off along the route with
the true course (ramp to 20 km/h in 10 s, 40 s of motion, about 197 m, all before route metre 3,195 which exp7 covered), then a
neutral tail (stationary, bearing absent) so that every run leaves the same last-known location for the next one.
Conditions differ ONLY in the standstill fixes (t = 0 .. MOVE_T - 1):
  B0 bearing absent (NAN)          B1 bearing 0 (stock placeholder)     B2 true road bearing T
  B3 T + 180                       B4 T - 90 (T + 90 is within 10 deg of B1 at this site)
  B5 trip6a/6b GPS-noise copy: random bearing 67-241 deg, speed 0.28-0.56 m/s, position wander within a 13 m span
  B7 held heading: B5 speed and position wander (same seed) with the constant true bearing T (stock LDS CarPlay-path hold below
     7 km/h, which the AA callback does not apply)
  B6 stale LDS record form: accuracy 8.8 m, constant 1.111 m/s, bearing 335 (= T + 55 here), true position
  *-A5: the same with accuracy 5 m instead of 15 m (conditional tier 3)
  RS-*: "already navigating": part a = B2 standstill + guidance start in one session, DHU restart without stopping Naver, part b =
        the condition run without the guidance taps
Syntax: location <lat> <lon> <accuracy_m|NAN> <altitude|NAN> <speed_mps> <bearing_deg|NAN>
Usage: gen_commands.py [out_dir [run_plan_json]]
"""
import importlib.util
import json
import math
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXP7_DIR = os.path.join(HERE, "..", "DHU_NAVER_EXP7_PLAN_2026-10-09")
EXP9_DIR = os.path.join(HERE, "..", "DHU_NAVER_EXP9_PLAN_2026-10-10")


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


E7 = _load("dhu_exp7_for_exp10", os.path.join(EXP7_DIR, "gen_commands.py"))
Route, fmt, offset_position = E7.Route, E7.fmt, E7.offset_position

SHOT = "../../../../experiment10/"
P0_M = 2950.0  # route metre of the standstill position
ACC_STAND = 15.0  # CONFIRMED accepted at standstill in exp8 (ST2, 2 runs); 25 m only SINGLE, 20 m once lost the gain (exp9)
ACC_A5 = 5.0
ACC_MOVE = 15.0  # unchanged at move-off so that the accuracy does not switch together with the motion
STALE = dict(acc=8.8, speed=1.111, bearing=335.0)  # LDS stored record as forwarded by the stock (trip2/trip3)
NOISE = dict(seed=10, brg=(67.0, 241.0), spd=(0.28, 0.56), radius=6.5, step=2.0)
SEARCH_T, RECENT_T = 8, 18  # tap times (s); the 10 s spacing and the coordinates come from the exp6-9 setup template
GUIDE_SHOT_T = 21
MOVE_T = 80  # first moving second (60 s of standstill after the guidance tap)
RAMP_S, V_MAX, MOVE_S = 10, 20 / 3.6, 40
TAIL_S = 10
RS_A_S = 35  # part a length (B2 standstill, guidance taps at the same times)


def _taps():
    with open(os.path.join(EXP9_DIR, "setup-template.txt")) as f:
        taps = [tuple(int(v) for v in line.split()[1:]) for line in f if line.startswith("tap ")]
    assert len(taps) == 2, taps
    return taps


TAP_SEARCH, TAP_RECENT = _taps()


def stand_photos():
    return {0, 6, GUIDE_SHOT_T} | set(range(23, 32, 2)) | set(range(35, MOVE_T, 4))


def move_photos():
    return set(range(MOVE_T, MOVE_T + 21, 2)) | set(range(MOVE_T + 24, MOVE_T + MOVE_S, 4)) | {MOVE_T + MOVE_S - 1}


T_END = MOVE_T + MOVE_S + TAIL_S  # duration in seconds
PHOTOS = stand_photos() | move_photos() | {T_END - 1}

BASES = ["B0", "B1", "B2", "B3", "B4", "B5", "B6", "B7"]
RUNS = {f"{b}-r{r}": dict(base=b, acc=None, rs=False) for b in BASES for r in (1, 2)}
RUNS["B2-r3"] = dict(base="B2", acc=None, rs=False)
RUNS.update({"B1-A5-r1": dict(base="B1", acc=ACC_A5, rs=False), "B2-A5-r1": dict(base="B2", acc=ACC_A5, rs=False)})
RUNS.update({"RS-B1-r1": dict(base="B1", acc=None, rs=True), "RS-B3-r1": dict(base="B3", acc=None, rs=True)})
RUNS.update({"C1-B0-r1": dict(base="B0", acc=None, rs=False, manual=True),
             "C1-B1-r1": dict(base="B1", acc=None, rs=False, manual=True)})
TIERS = [
    ["B2-r1", "B3-r1", "B1-r1", "B5-r1", "B7-r1"],
    ["B0-r1", "B4-r1", "B6-r1", "B2-r2"],
    ["B3-r2", "B1-r2", "B5-r2", "B7-r2", "B0-r2", "B4-r2", "B6-r2", "B1-A5-r1", "B2-A5-r1", "RS-B3-r1", "RS-B1-r1", "B2-r3"],
    ["C1-B0-r1", "C1-B1-r1"],
]
TIER_NAMES = ["1", "2", "3", "M"]
ORDER = [t for tier in TIERS[:3] for t in tier]
TIER3_ALWAYS = ("B2-r3",)
OVERHEAD_MIN = (3.0, 4.0)  # exp9 measured 2-3 min per run incl. DHU restart; + force-stop, pidof check and 30 s wait
RS_EXTRA_MIN = 1.5  # second DHU restart
ENV_MIN = 10.0


def condition_fix(route, base, t, acc, rng_state):
    """(line, record) of standstill second t."""
    lat, lon, road = route.at(P0_M)
    acc = ACC_STAND if acc is None else acc
    if base == "B6":
        acc = STALE["acc"] if acc == ACC_STAND else acc
        return fmt(lat, lon, acc, STALE["speed"], STALE["bearing"]), dict(accuracy_m=acc, speed_sent_mps=STALE["speed"],
                                                                           bearing_sent_deg=STALE["bearing"])
    if base in ("B5", "B7"):
        rng, pos = rng_state
        b = rng.uniform(*NOISE["brg"])
        if base == "B7":
            b = road
        v = rng.uniform(*NOISE["spd"])
        ang = rng.uniform(0, 2 * math.pi)
        e, n = pos[0] + NOISE["step"] * math.sin(ang), pos[1] + NOISE["step"] * math.cos(ang)
        r = math.hypot(e, n)
        if r > NOISE["radius"]:
            e, n = e * NOISE["radius"] / r, n * NOISE["radius"] / r
        rng_state[1] = (e, n)
        slat, slon = offset_position(lat, lon, (e, n))
        return fmt(slat, slon, acc, v, b), dict(accuracy_m=acc, speed_sent_mps=round(v, 3), bearing_sent_deg=round(b, 1),
                                                 wander_e_m=round(e, 1), wander_n_m=round(n, 1))
    brg = {"B0": None, "B1": 0.0, "B2": road, "B3": (road + 180) % 360, "B4": (road - 90) % 360}[base]
    return fmt(lat, lon, acc, 0.0, brg), dict(accuracy_m=acc, speed_sent_mps=0.0,
                                              bearing_sent_deg=None if brg is None else round(brg, 1))


def build_run(route, tag, p, taps=True):
    """Command lines (without part a) and the plan dict. t = seconds since the first location line of the session."""
    lines, frames = [], []
    rng_state = [random.Random(NOISE["seed"]), (0.0, 0.0)]
    d = P0_M
    lat0, lon0, road0 = route.at(P0_M)
    for t in range(T_END):
        if t < MOVE_T:
            line, rec = condition_fix(route, p["base"], t, p["acc"], rng_state)
            rec.update(phase="stand", truth_m=P0_M, speed_mps=0.0, road_bearing_deg=round(road0, 1))
        elif t < MOVE_T + MOVE_S:
            k = t - MOVE_T
            v = min(V_MAX, V_MAX * (k + 1) / RAMP_S)
            d += v
            lat, lon, road = route.at(d)
            line = fmt(lat, lon, ACC_MOVE, v, road)
            rec = dict(phase="move", truth_m=round(d, 1), speed_mps=round(v, 3), road_bearing_deg=round(road, 1),
                       accuracy_m=ACC_MOVE, speed_sent_mps=round(v, 3), bearing_sent_deg=round(road, 1),
                       moved_m=round(d - P0_M, 1))
        else:
            lat, lon, road = route.at(d)
            line = fmt(lat, lon, ACC_MOVE, 0.0, None)
            rec = dict(phase="tail", truth_m=round(d, 1), speed_mps=0.0, road_bearing_deg=round(road, 1),
                       accuracy_m=ACC_MOVE, speed_sent_mps=0.0, bearing_sent_deg=None)
        rec["t"] = t
        lines.append(line)
        extra = []
        if taps and t == SEARCH_T:
            extra.append("tap %d %d" % TAP_SEARCH)
        if taps and t == RECENT_T:
            extra.append("tap %d %d" % TAP_RECENT)
        if t in PHOTOS:
            name = f"{tag}-{t:03d}.png"
            lines += extra + ["sleep 0.2", f"screenshot {SHOT}{name}", "sleep 0.8"]
            rec["screenshot"] = name
            frames.append(rec)
        else:
            lines += extra + ["sleep 1"]
    plan = dict(tag=tag, base=p["base"], accuracy_stand_m=(STALE["acc"] if p["base"] == "B6" and p["acc"] is None
                                                         else (p["acc"] or ACC_STAND)),
                reconnect=p["rs"], manual=p.get("manual", False), duration_s=T_END, stand=[0, MOVE_T - 1],
                guidance_taps=[SEARCH_T, RECENT_T] if taps else [], move=[MOVE_T, MOVE_T + MOVE_S - 1],
                tail=[MOVE_T + MOVE_S, T_END - 1], p0=[round(lat0, 7), round(lon0, 7)], true_bearing_deg=round(road0, 1),
                end_m=round(d, 1), frames=frames)
    return lines, plan


def build_part_a(route, tag):
    lines = []
    for t in range(RS_A_S):
        line, _ = condition_fix(route, "B2", t, None, None)
        lines.append(line)
        if t == SEARCH_T:
            lines.append("tap %d %d" % TAP_SEARCH)
        if t == RECENT_T:
            lines.append("tap %d %d" % TAP_RECENT)
        if t in (GUIDE_SHOT_T, RS_A_S - 1):
            lines += ["sleep 0.2", f"screenshot {SHOT}{tag}-a{t:03d}.png", "sleep 0.8"]
        else:
            lines.append("sleep 1")
    return lines


def prime_lines(route):
    lat, lon, _ = route.at(P0_M + 197.0)
    out = []
    for _ in range(TAIL_S):
        out += [fmt(lat, lon, ACC_MOVE, 0.0, None), "sleep 1"]
    return out


def estimate(plans, runs, env):
    input_s = sum(plans[t]["duration_s"] + (RS_A_S if plans[t]["reconnect"] else 0) for t in runs)
    extra = sum(RS_EXTRA_MIN for t in runs if plans[t]["reconnect"])
    return input_s, [round(input_s / 60 + len(runs) * o + extra + env) for o in OVERHEAD_MIN]


def generate(out_dir, plan_path=None):
    route = Route(os.path.join(HERE, "geometry", "route.json"))
    os.makedirs(out_dir, exist_ok=True)
    plans = {}
    for tag, p in RUNS.items():
        lines, plan = build_run(route, tag, p, taps=not p["rs"])
        with open(os.path.join(out_dir, (tag + ".b" if p["rs"] else tag) + ".txt"), "w") as f:
            f.write("\n".join(lines) + "\n")
        if p["rs"]:
            with open(os.path.join(out_dir, tag + ".a.txt"), "w") as f:
                f.write("\n".join(build_part_a(route, tag)) + "\n")
        plans[tag] = plan
    with open(os.path.join(out_dir, "PRIME.txt"), "w") as f:
        f.write("\n".join(prime_lines(route)) + "\n")
    assert sorted(t for tier in TIERS for t in tier) == sorted(plans), "TIERS must list every run exactly once"
    tiers = []
    for name, runs in zip(TIER_NAMES, TIERS):
        input_s, est = estimate(plans, runs, ENV_MIN if name == "1" else 0.0)
        tier = dict(tier=name, runs=runs, input_s=input_s, photos=sum(len(plans[t]["frames"]) for t in runs),
                    estimate_min=est, unattended=name != "M")
        if name == "3":
            tier.update(always=list(TIER3_ALWAYS), per_added_run_min=estimate(plans, ["B1-r2"], 0.0)[1],
                        note="candidate list; the README rule selects the runs")
        tiers.append(tier)
    lat0, lon0, road0 = route.at(P0_M)
    doc = dict(site=dict(p0_route_m=P0_M, p0=[round(lat0, 7), round(lon0, 7)], true_bearing_deg=round(road0, 1),
                         after_tunnel_exit_m=round(P0_M - route.exit, 1)),
               timeline=dict(search_tap_t=SEARCH_T, recent_tap_t=RECENT_T, guidance_photo_t=GUIDE_SHOT_T, move_t=MOVE_T,
                             ramp_s=RAMP_S, v_max_mps=round(V_MAX, 3), move_s=MOVE_S, tail_s=TAIL_S, duration_s=T_END,
                             rs_part_a_s=RS_A_S, photo_t=sorted(PHOTOS)),
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
    s = doc["site"]
    print(f"P0 {s['p0']} route m {s['p0_route_m']} ({s['after_tunnel_exit_m']} m after the exit), true bearing {s['true_bearing_deg']}")
    for tier in doc["tiers"]:
        print(f"tier {tier['tier']}: {len(tier['runs'])} runs, {tier['input_s']} s input, {tier['photos']} photos, "
              f"about {tier['estimate_min'][0]}-{tier['estimate_min'][1]} min" + ("" if tier["unattended"] else " (manual)"))


if __name__ == "__main__":
    main(sys.argv)
