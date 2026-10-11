#!/usr/bin/env python3
"""Generate DHU `location` command files for experiment 10 v5 (v4 + one accuracy constant)

v5 (2026-10-11, after the v4 reconnect failures, validation/DHU_NAVER_EXP10_V4_RESULT_2026-10-11.md): every `location` line whose v4
accuracy field is 5 m (written `5` in the template-derived lines, `5.000` in the generated lines) is written with ACC = 1 m instead.
Nothing else changes: coordinates, times, speeds, bearings, taps, screenshots and every other accuracy value (B6 8.8 m, A15 15 m) stay
byte-identical to v4. T0-LPROBE is exempt (it keeps its 15 m / 5 m halves: the optional 5 m diagnostic). Reason: after a phone
reconnect the injected 5 m fixes no longer moved Naver off the phone's own fix (gate L failed for B3, B1 and the B2-LCHECK control).

Experiment 10 v4 (G3: bearing shown on a fresh Android Auto session at standstill).

v4 (2026-10-11, after the v3 Tier 0 ABORT): the run files are unchanged from v3, but their first 18 lines (the setup-template.txt
preamble) are NOT sent any more. Primary files show that experiments 6-9 never sent them: exp6 prepare.py and the exp7/8/9 prep_stage.py
started guidance with search (600,44) -> observed 모라역 row (230, y) -> route start (345,145) -> popup dismissal (170,177) and then sent
only the lines after the setup-guidance screenshot (exp9 begin.py: body == source[k+1:]). commands/prep/ holds those stages with the
condition's own first fix (the v3 independent variable @BRG@); for B2 they are byte-identical to the exp7/8/9 prep_stage.py stages.

v3 (2026-10-11, rework after the failed v1 night and the rejected v2, validation/DHU_NAVER_EXP10_FAILURE_2026-10-11.md).
Principle: change only the independent variable. Every run file starts with the proven guidance-start preamble of experiments 6-9
(`setup-template.txt`, byte copy of the exp6/7/8/9 file: a 5 m fix, `tap 200 50`, `tap 345 145`, five more 5 m fixes, the guidance
screenshot) at the exp9 start point (route metre 95.6, true road bearing 328.9 deg, D0 4.3 km in every exp9 run), run in the same
session state as experiments 6-9 (DHU restarted for the run, Naver warm, previous guidance ended). The ONLY substitution that differs
between conditions is @BRG@, the bearing field of the first stationary fixes of the session. After the preamble the body follows:
  t = 0 .. 79    standstill at the start point with the condition's fixes (accuracy 5 m as the preamble; *-A15 runs: 15 m, the exp8 ST2
                 standstill value, after the 5 m preamble)
  t = 80 .. 119  move-off along the route with the true course, ramp to 20 km/h in 10 s, 40 s (about 197 m, before the tunnel entrance),
                 accuracy 5 m (the exp9 approach accuracy)
  t = 120 .. 129 neutral tail (stationary, bearing absent) so that every run leaves the same last-known location
Conditions (standstill t = 0 .. 79; the preamble carries the condition's first bearing, speed 0, the start point):
  B0 bearing absent (NAN)          B1 bearing 0 (stock placeholder; T + 31 deg at this site)   B2 true road bearing T
  B3 T + 180                       B4 T - 90
  B5 trip6a/6b GPS-noise copy: random bearing 67-241 deg, speed 0.28-0.56 m/s, position wander within a 13 m span
  B7 held heading: B5 speed and wander (same seed) with the constant bearing T
  B6 stale LDS record form: accuracy 8.8 m, constant 1.111 m/s, bearing 335 (T + 6 deg here), true position
  *-A15: accuracy 15 m in the standstill (conditional tier 3)    *-cold: same file, Naver force-stopped before the DHU start (only if
  Tier 0 proves a cold start works)
  RS-*: "already navigating": part a = preamble with B2 + 35 s B2 standstill, DHU-only restart, part b = the condition body, no preamble
Tier 0 files: T0-PRE (the exp9 ST8 preamble with tag T0-PRE + 20 s of 5 m true-bearing fixes with 2 screenshots), T0-COLD (the same
with tag T0-COLD, sent after a Naver force-stop), T0-LPROBE (15 m then
5 m fixes at the start point, diagnostic only when the preamble fails), FB-<base> (the preamble's fix line for 120 s: the stream during
the screen-resolved fallback preparation).
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


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


E7 = _load("dhu_exp7_for_exp10", os.path.join(EXP7_DIR, "gen_commands.py"))
Route, fmt, offset_position = E7.Route, E7.fmt, E7.offset_position

SHOT = "../../../../experiment10/"
ACC = 1.0  # v5: replaces every v4 accuracy of exactly 5 m (see apply_acc); ACC = 5.0 reproduces the v4 command files byte for byte
ACC_EXEMPT = ("T0-LPROBE",)  # the 15 m / 5 m diagnostic keeps its v4 values
ACC_STAND = 5.0  # the preamble accuracy; exp8 ST1 (5 m standstill) CONFIRMED accepted
ACC_A15 = 15.0  # exp8 ST2 standstill value (CONFIRMED accepted after an established synthetic position)
ACC_MOVE = 5.0  # exp9 approach accuracy
STALE = dict(acc=8.8, speed=1.111, bearing=335.0)  # LDS stored record as forwarded by the stock (trip2/trip3)
NOISE = dict(seed=10, brg=(67.0, 241.0), spd=(0.28, 0.56), radius=6.5, step=2.0)
FB_S = 120  # fallback stream length (s)
LPROBE_S = 15
T0_EXTRA_S = 20
# Observed DHU coordinates (800x480, exp7 reset log / preparation deviations, v1 notes). FALLBACK ONLY, after the preamble's gate failed;
# the agent resolves taps from the screen first and a gate checks the result.
UI_FALLBACK = dict(search=(600, 44), recent_row_x=230, recent_rows_y=(130, 225, 318), route_start=(345, 145),
                   recalc_popup_dismiss=(170, 177), naver_icon=(298, 440))
EXPECTED_D0_KM = (3.9, 4.7)  # exp9 D0 4.3 km at this start
MOVE_T = 80
RAMP_S, V_MAX, MOVE_S = 10, 20 / 3.6, 40
TAIL_S = 10
RS_A_S = 35
N_TEMPLATE = 18  # lines of setup-template.txt at the top of every non-RS run file and RS part a; v4 never sends them
PREP_ROWS = (130, 225, 318)  # observed 모라역 rows of the recent list (exp7/exp9 preparation-deviations)


def start_m(route):
    """exp9 start: exp7 approach start = entrance + 20 m - 19 s * 14 m/s (route metre 95.6)."""
    return route.entrance + E7.APPROACH_END_INSIDE_M - E7.SPEED * (E7.APPROACH_FIXES - 1)


def stand_photos():
    return {0, 6, 21} | set(range(23, 32, 2)) | set(range(35, MOVE_T, 4))


def move_photos():
    return set(range(MOVE_T, MOVE_T + 21, 2)) | set(range(MOVE_T + 24, MOVE_T + MOVE_S, 4)) | {MOVE_T + MOVE_S - 1}


T_END = MOVE_T + MOVE_S + TAIL_S
PHOTOS = stand_photos() | move_photos() | {T_END - 1}

BASES = ["B0", "B1", "B2", "B3", "B4", "B5", "B6", "B7"]
RUNS = {f"{b}-r{r}": dict(base=b, acc=None, rs=False) for b in BASES for r in (1, 2)}
RUNS["B2-r3"] = dict(base="B2", acc=None, rs=False)
RUNS.update({"B1-A15-r1": dict(base="B1", acc=ACC_A15, rs=False), "B2-A15-r1": dict(base="B2", acc=ACC_A15, rs=False)})
RUNS.update({"RS-B1-r1": dict(base="B1", acc=None, rs=True), "RS-B3-r1": dict(base="B3", acc=None, rs=True)})
RUNS.update({f"{b}-cold-r1": dict(base=b, acc=None, rs=False, cold=True) for b in ("B3", "B1", "B2")})
RUNS.update({"C1-B0-r1": dict(base="B0", acc=None, rs=False, manual=True),
             "C1-B1-r1": dict(base="B1", acc=None, rs=False, manual=True)})
TIERS = [
    ["B2-r1", "B3-r1", "B1-r1", "B5-r1", "B7-r1"],
    ["B0-r1", "B4-r1", "B6-r1", "B2-r2"],
    ["B3-r2", "B1-r2", "B5-r2", "B7-r2", "B0-r2", "B4-r2", "B6-r2", "B1-A15-r1", "B2-A15-r1", "RS-B3-r1", "RS-B1-r1",
     "B3-cold-r1", "B1-cold-r1", "B2-cold-r1", "B2-r3"],
    ["C1-B0-r1", "C1-B1-r1"],
]
TIER_NAMES = ["1", "2", "3", "M"]
ORDER = [t for tier in TIERS[:3] for t in tier]
TIER3_ALWAYS = ("B2-r3",)
OVERHEAD_MIN = (3.0, 4.0)  # exp9: run start to run start 6.4-7.9 min for 252 s incl. DHU/guidance restart, preamble, saving
RS_EXTRA_MIN = 1.5
COLD_EXTRA_MIN = 1.0  # force-stop, pidof, 30 s wait
ENV_MIN = 10.0
TIER0_MIN = (6.0, 9.0)
BUDGET_MIN = 180


def condition_fix(route, base, t, acc, rng_state):
    """(line, record) of standstill second t."""
    lat, lon, road = route.at(start_m(route))
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


def preamble_bearing(route, base):
    """@BRG@: the condition's first standstill bearing (None = NAN)."""
    _, rec = condition_fix(route, base, 0, None, [random.Random(NOISE["seed"]), (0.0, 0.0)])
    return rec["bearing_sent_deg"]


def preamble(template, route, tag, brg):
    lat, lon, _ = route.at(start_m(route))
    text = template.replace("@START@", f"{lat:.7f} {lon:.7f}").replace("@BRG@", "NAN" if brg is None else f"{brg:.1f}")
    return text.replace("@SHOT@", SHOT).replace("@TAG@", tag).rstrip("\n").splitlines()


def build_body(route, tag, p):
    """Body lines (after the preamble) and the plan dict. t = seconds since the first body line."""
    lines, frames = [], []
    rng_state = [random.Random(NOISE["seed"]), (0.0, 0.0)]
    s0 = start_m(route)
    d = s0
    lat0, lon0, road0 = route.at(s0)
    for t in range(T_END):
        if t < MOVE_T:
            line, rec = condition_fix(route, p["base"], t, p["acc"], rng_state)
            rec.update(phase="stand", truth_m=round(s0, 1), speed_mps=0.0, road_bearing_deg=round(road0, 1))
        elif t < MOVE_T + MOVE_S:
            k = t - MOVE_T
            v = min(V_MAX, V_MAX * (k + 1) / RAMP_S)
            d += v
            lat, lon, road = route.at(d)
            line = fmt(lat, lon, ACC_MOVE, v, road)
            rec = dict(phase="move", truth_m=round(d, 1), speed_mps=round(v, 3), road_bearing_deg=round(road, 1),
                       accuracy_m=ACC_MOVE, speed_sent_mps=round(v, 3), bearing_sent_deg=round(road, 1), moved_m=round(d - s0, 1))
        else:
            lat, lon, road = route.at(d)
            line = fmt(lat, lon, ACC_MOVE, 0.0, None)
            rec = dict(phase="tail", truth_m=round(d, 1), speed_mps=0.0, road_bearing_deg=round(road, 1),
                       accuracy_m=ACC_MOVE, speed_sent_mps=0.0, bearing_sent_deg=None)
        rec["t"] = t
        lines.append(line)
        if t in PHOTOS:
            name = f"{tag}-{t:03d}.png"
            lines += ["sleep 0.2", f"screenshot {SHOT}{name}", "sleep 0.8"]
            rec["screenshot"] = name
            frames.append(rec)
        else:
            lines += ["sleep 1"]
    acc_stand = STALE["acc"] if p["base"] == "B6" and p["acc"] is None else (p["acc"] or ACC_STAND)
    plan = dict(tag=tag, base=p["base"], accuracy_stand_m=acc_stand, reconnect=p["rs"], cold=p.get("cold", False),
                manual=p.get("manual", False), preamble=not p["rs"], preamble_bearing_deg=preamble_bearing(route, p["base"]),
                setup_screenshot=None if p["rs"] else f"{tag}-setup-guidance.png", duration_s=T_END, stand=[0, MOVE_T - 1],
                move=[MOVE_T, MOVE_T + MOVE_S - 1], tail=[MOVE_T + MOVE_S, T_END - 1], start=[round(lat0, 7), round(lon0, 7)],
                start_route_m=round(s0, 1), true_bearing_deg=round(road0, 1), end_m=round(d, 1), fallback_stream=f"FB-{p['base']}.txt",
                frames=frames)
    return lines, plan


def build_part_a(template, route, tag):
    lines = preamble(template, route, tag + "-a", preamble_bearing(route, "B2"))
    for t in range(RS_A_S):
        line, _ = condition_fix(route, "B2", t, None, None)
        lines.append(line)
        if t in (21, RS_A_S - 1):
            lines += ["sleep 0.2", f"screenshot {SHOT}{tag}-a{t:03d}.png", "sleep 0.8"]
        else:
            lines.append("sleep 1")
    return lines


def fallback_stream(template, route, base):
    """The preamble's own fix line (5 m, condition bearing, speed 0, start point) at 1 Hz, no taps, no screenshots."""
    fix = [x for x in preamble(template, route, "FB", preamble_bearing(route, base)) if x.startswith("location ")][0]
    return [s for _ in range(FB_S) for s in (fix, "sleep 1")]


def prep_files(template, route, base):
    """v4 preparation stages for one condition (exp6 prepare.py / exp7-9 prep_stage.py form, the condition's first fix).

    @TAG@ in the screenshot lines is the only placeholder; the agent substitutes the run tag (e.g. B2-r1, T0-PRE, RS-B1-r1-a)."""
    fix = fallback_stream(template, route, base)[0]
    shot = lambda name: f"screenshot {SHOT}@TAG@-{name}.png"
    five = [s for _ in range(5) for s in (fix, "sleep 1")]
    out = {f"{base}-search": [fix, "sleep 1", "tap 600 44", "sleep 2", shot("prep-search")]}
    for y in PREP_ROWS:  # exp7/8/9 prep_stage.py stage 'route'
        out[f"{base}-route-y{y}"] = [f"tap 230 {y}", "sleep 5", "tap 345 145", "sleep 3"] + five + ["sleep 2", shot("pre-gate")]
    out[f"{base}-route-retry"] = ["tap 345 145", "sleep 3"] + five + ["sleep 2", shot("pre-gate-2")]  # exp8 ST0 deviation
    out[f"{base}-gate"] = ["tap 170 177", "sleep 1", fix, "sleep 1", shot("setup-guidance")]  # exp7/8/9 stage 'gate'
    out[f"{base}-gate-nopopup"] = [fix, "sleep 1", shot("setup-guidance")]
    return out


PREP_SEARCH_AGAIN = ["tap 600 44", "sleep 2", f"screenshot {SHOT}@TAG@-prep-search-2.png"]
PREP_END_GUIDANCE = ["tap 55 347", "sleep 2", f"screenshot {SHOT}@TAG@-end.png"]  # exp6 status.json: ETA-card X


def tier0_files(template, route):
    lat, lon, road = route.at(start_m(route))
    pre = preamble(template, route, "T0-PRE", round(road, 1))
    for t in range(T0_EXTRA_S):
        pre.append(fmt(lat, lon, ACC_STAND, 0.0, road))
        pre += ["sleep 0.2", f"screenshot {SHOT}T0-PRE-{t + 1:03d}.png", "sleep 0.8"] if t in (9, T0_EXTRA_S - 1) else ["sleep 1"]
    probe = []
    for acc in (15.0, ACC_STAND):
        for t in range(LPROBE_S):
            probe.append(fmt(lat, lon, acc, 0.0, None))
            if t == LPROBE_S - 1:
                probe += ["sleep 0.2", f"screenshot {SHOT}T0-LPROBE-{int(acc):02d}m.png", "sleep 0.8"]
            else:
                probe.append("sleep 1")
    cold = [x.replace(f"{SHOT}T0-PRE", f"{SHOT}T0-COLD") for x in pre]
    return {"T0-PRE": pre, "T0-COLD": cold, "T0-LPROBE": probe}


def prime_lines(route):
    lat, lon, _ = route.at(start_m(route) + 197.0)
    out = []
    for _ in range(TAIL_S):
        out += [fmt(lat, lon, ACC_MOVE, 0.0, None), "sleep 1"]
    return out


def estimate(plans, runs, env):
    input_s = sum(plans[t]["duration_s"] + (21 if plans[t]["preamble"] else 0) + (RS_A_S + 21 if plans[t]["reconnect"] else 0)
                  for t in runs)
    extra = sum(RS_EXTRA_MIN * plans[t]["reconnect"] + COLD_EXTRA_MIN * plans[t]["cold"] for t in runs)
    return input_s, [round(input_s / 60 + len(runs) * o + extra + env) for o in OVERHEAD_MIN]


def apply_acc(line):
    """v5: a `location` line with accuracy exactly 5 m gets ACC; every other line and accuracy value is returned unchanged."""
    w = line.split(" ")
    if w[0] != "location" or w[3] not in ("5", "5.000"):
        return line
    w[3] = f"{ACC:g}" if w[3] == "5" else f"{ACC:.3f}"
    return " ".join(w)


def apply_acc_plan(plans):
    for plan in plans.values():
        if plan["accuracy_stand_m"] == 5.0:
            plan["accuracy_stand_m"] = ACC
        for fr in plan["frames"]:
            if fr.get("accuracy_m") == 5.0:
                fr["accuracy_m"] = ACC


def generate(out_dir, plan_path=None):
    route = Route(os.path.join(HERE, "geometry", "route.json"))
    with open(os.path.join(HERE, "setup-template.txt")) as f:
        template = f.read()
    os.makedirs(out_dir, exist_ok=True)

    def write(name, lines):
        if name not in ACC_EXEMPT:
            lines = [apply_acc(x) for x in lines]
        with open(os.path.join(out_dir, name + ".txt"), "w") as f:
            f.write("\n".join(lines) + "\n")

    plans = {}
    for tag, p in RUNS.items():
        body, plan = build_body(route, tag, p)
        if p["rs"]:
            write(tag + ".b", body)
            write(tag + ".a", build_part_a(template, route, tag))
        else:
            write(tag, preamble(template, route, tag, plan["preamble_bearing_deg"]) + body)
        plans[tag] = plan
    write("PRIME", prime_lines(route))
    for base in BASES:
        write(f"FB-{base}", fallback_stream(template, route, base))
    for name, lines in tier0_files(template, route).items():
        write(name, lines)
    prep_dir = os.path.join(out_dir, "prep")
    os.makedirs(prep_dir, exist_ok=True)
    prep_names = []
    stages = {"search-again": PREP_SEARCH_AGAIN, "end-guidance": PREP_END_GUIDANCE}
    for base in BASES:
        stages.update(prep_files(template, route, base))
    for name, lines in stages.items():
        write(os.path.join("prep", name), lines)
        prep_names.append(f"prep/{name}.txt")
    apply_acc_plan(plans)
    assert sorted(t for tier in TIERS for t in tier) == sorted(plans), "TIERS must list every run exactly once"
    tiers = []
    for name, runs in zip(TIER_NAMES, TIERS):
        input_s, est = estimate(plans, runs, ENV_MIN if name == "1" else 0.0)
        tier = dict(tier=name, runs=runs, input_s=input_s, photos=sum(len(plans[t]["frames"]) + 1 for t in runs),
                    estimate_min=est, unattended=name != "M")
        if name == "3":
            tier.update(always=list(TIER3_ALWAYS), per_added_run_min=estimate(plans, ["B1-r2"], 0.0)[1],
                        note="candidate list; the README rule selects the runs")
        tiers.append(tier)
    s0 = start_m(route)
    lat0, lon0, road0 = route.at(s0)
    doc = dict(site=dict(start_route_m=round(s0, 1), start=[round(lat0, 7), round(lon0, 7)], true_bearing_deg=round(road0, 1),
                         before_tunnel_entrance_m=round(route.entrance - s0, 1), expected_d0_km=list(EXPECTED_D0_KM)),
               timeline=dict(preamble="setup-template.txt with @START@, @BRG@ (condition), @SHOT@, @TAG@ substituted",
                             t0="first body line after the preamble's guidance screenshot", move_t=MOVE_T, ramp_s=RAMP_S,
                             v_max_mps=round(V_MAX, 3), move_s=MOVE_S, tail_s=TAIL_S, duration_s=T_END, rs_part_a_s=RS_A_S,
                             photo_t=sorted(PHOTOS)),
               tier0=dict(files=["T0-PRE.txt", "T0-COLD.txt", "T0-LPROBE.txt"], extra_s=T0_EXTRA_S, lprobe_s=LPROBE_S,
                          estimate_min=list(TIER0_MIN)),
               fallback=dict(files={b: f"FB-{b}.txt" for b in BASES}, stream_s=FB_S, ui_fallback=UI_FALLBACK,
                             rule="v3 only; not used in v4 (the screen-checked preparation is the primary path)"),
               prep=dict(version="v4", files=sorted(prep_names), template_lines_not_sent=N_TEMPLATE,
                         body_rule="send the run file from line 19 (after the setup-guidance screenshot line); exp9 begin.py",
                         stages=["<base>-search", "search-again (if the search screen is not open)", "<base>-route-y<row>",
                                 "<base>-route-retry (if no route)", "<base>-gate (popup) | <base>-gate-nopopup", "gate G"],
                         rows_y=list(PREP_ROWS), placeholder="@TAG@ in screenshot lines only",
                         source="exp6 prepare.py; exp7/8/9 prep_stage.py, begin.py, reset-log.txt, preparation-deviations.json"),
               accuracy=dict(version="v5", acc_m=ACC, rule="every v4 location accuracy of exactly 5 m -> acc_m; others unchanged",
                             exempt=list(ACC_EXEMPT)),
               budget_min=BUDGET_MIN, order=ORDER, tiers=tiers, runs=plans)
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
    print(f"start {s['start']} route m {s['start_route_m']} ({s['before_tunnel_entrance_m']} m before the entrance), "
          f"true bearing {s['true_bearing_deg']}")
    for tier in doc["tiers"]:
        print(f"tier {tier['tier']}: {len(tier['runs'])} runs, {tier['input_s']} s input, {tier['photos']} photos, "
              f"about {tier['estimate_min'][0]}-{tier['estimate_min'][1]} min" + ("" if tier["unattended"] else " (manual)"))


if __name__ == "__main__":
    main(sys.argv)
