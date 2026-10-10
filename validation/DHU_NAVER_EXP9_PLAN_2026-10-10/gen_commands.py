#!/usr/bin/env python3
"""Generate DHU `location` command files for experiment 9 (stopped-state accuracy threshold, drifted stop, crawl, stop-and-go).

Derived from validation/DHU_NAVER_EXP8_PLAN_2026-10-10/gen_commands.py and, through it, the exp7 generator (Route, fmt,
offset_position, dr_error_step, decompose and hav are imported; the route geometry and the setup template are byte copies).
Base profile = exp8 / exp7 H8: 20 approach fixes at 14 m/s ending 20 m inside the entrance (accuracy 5 m), tunnel at 14 m/s with
accuracy 40 m and the correct bearing, ONE 30 s window at t=107..136 in the middle of the tunnel, 30 exit fixes (accuracy 5 m).
Only the window (and, for the DRIFT-STOP runs, the tunnel positions/bearings of the exp7 R1 drift) differs between conditions:
  AC<X>          speed 0, the identical true position, accuracy X while stopped, 40 elsewhere (product-like switching 40 -> X -> 40)
  CR3-<X>        crawl 3 km/h (0.833 m/s) along the route for 30 s (truth moves 25 m), accuracy X; then 14 m/s again
  DRIFT-STOP-<X> exp7 R1 (+0.15 deg/s yaw-zero error) DR positions/bearings, a 30 s stop at the DRIFTED position with the held drifted
                 bearing and accuracy X, then the drift continues (the heading error does not grow while stopped), exit = true GPS
  STOPGO3-20     three 15 s stops (true position, accuracy 20) separated by 20 s of movement at 14 m/s
  ST8-nostop     moving positive control (= exp8 ST8 = exp7 H0)
Syntax: location <lat> <lon> <accuracy_m|NAN> <altitude|NAN> <speed_mps> <bearing_deg|NAN>
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
    spec = importlib.util.spec_from_file_location("dhu_exp7_for_exp9", os.path.join(EXP7_DIR, "gen_commands.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


E7 = _load_exp7()
Route, fmt, offset_position, hav = E7.Route, E7.fmt, E7.offset_position, E7.hav

SHOT = "../../../../experiment9/"
APPROACH_FIXES = E7.APPROACH_FIXES  # 20
APPROACH_END_INSIDE_M = E7.APPROACH_END_INSIDE_M  # 20 m
EXIT_FIXES = E7.EXIT_FIXES  # 30
SPEED = E7.SPEED  # 14 m/s
ACC_TUNNEL = E7.ACC_TUNNEL  # 40 m
ACC_GPS = E7.ACC_GPS  # 5 m
ENTRANCE_DENSE = E7.ENTRANCE_DENSE  # 22, 24, 26, 28
STOP_S = 30
CRAWL_MPS = 3 / 3.6  # 3 km/h
DRIFT_RATE = 0.15  # deg/s = exp7 R1 (about 4 counts, the per-drive 1-sigma yaw-zero error)
STOPGO = dict(stops=3, stop_s=15, gap_s=20)
WINDOW_PRE, WINDOW_POST, WINDOW_STEP = 7, 12, 4  # photos every 4 s from s0-7 to s1+12 (exp8: 100..148 for 107..136)
FREEZE_PRE, FREEZE_POST = 36, 4  # photos every 4 s from exit-36 to exit-4 (exp8: 186..218 for exit 222)
# Accuracy rule (validation/ACCURACY_RULE_2026-10-05.md): ceil(20 + 0.3 t + 0.03 D + 0.002 * int v tau dtau), t and D since the anchor.
RULE = dict(e0=20.0, sv=0.3, h0=0.03, k=0.002)

# Conditions. window: list of dicts (gap = None -> starts when the truth passes the tunnel middle (exp8 trigger); otherwise after
# `gap` moving tunnel seconds since the previous window), length s, truth speed v (m/s), accuracy acc; dr = drift rate (deg/s).
RUNS = {
    "ST8-nostop": dict(windows=[]),
    "AC20": dict(windows=[dict(gap=None, length=STOP_S, v=0.0, acc=20.0)]),
    "AC25": dict(windows=[dict(gap=None, length=STOP_S, v=0.0, acc=25.0)]),
    "AC30": dict(windows=[dict(gap=None, length=STOP_S, v=0.0, acc=30.0)]),
    "AC35": dict(windows=[dict(gap=None, length=STOP_S, v=0.0, acc=35.0)]),
    "CR3-40": dict(windows=[dict(gap=None, length=STOP_S, v=CRAWL_MPS, acc=40.0)]),
    "CR3-20": dict(windows=[dict(gap=None, length=STOP_S, v=CRAWL_MPS, acc=20.0)]),
    "DRIFT-STOP-15": dict(windows=[dict(gap=None, length=STOP_S, v=0.0, acc=15.0)], dr=DRIFT_RATE),
    "DRIFT-STOP-20": dict(windows=[dict(gap=None, length=STOP_S, v=0.0, acc=20.0)], dr=DRIFT_RATE),
    "STOPGO3-20": dict(windows=[dict(gap=None if i == 0 else STOPGO["gap_s"], length=STOPGO["stop_s"], v=0.0, acc=20.0)
                                for i in range(STOPGO["stops"])]),
}
REPS = {"ST8-nostop": 3}  # control: first (tier 1), last of tier 2 and last of tier 3; every other condition r1 + r2
# Priority tiers (fixed before the data). Tier 3 is the CANDIDATE list in its fixed relative order; the README rule always keeps
# ST8-nostop-r3 (last) and selects the other r2s from the r1 classifications.
TIERS = [
    ["ST8-nostop-r1", "AC25-r1", "AC20-r1", "DRIFT-STOP-20-r1", "CR3-40-r1"],
    ["AC30-r1", "DRIFT-STOP-15-r1", "CR3-20-r1", "STOPGO3-20-r1", "AC35-r1", "ST8-nostop-r2"],
    ["DRIFT-STOP-20-r2", "AC20-r2", "AC25-r2", "CR3-40-r2", "AC30-r2", "AC35-r2", "STOPGO3-20-r2", "CR3-20-r2",
     "DRIFT-STOP-15-r2", "ST8-nostop-r3"],
]
ORDER = [tag for tier in TIERS for tag in tier]
TIER3_ALWAYS = ("ST8-nostop-r3",)
# Measured in exp8 (run start to run start about 6.4-7.9 min for a 252 s run incl. DHU/guidance restart, setup, saving and reading):
# 2.0-3.0 min per run outside the input itself, plus the initial environment in tier 1.
OVERHEAD_MIN = (2.0, 3.0)
ENV_MIN = 10.0


def honest_budget(t_s, dist_m, int_v_tau):
    """Reported accuracy of the accuracy rule (metres, ceil) after t_s seconds and dist_m metres since the anchor."""
    return math.ceil(RULE["e0"] + RULE["sv"] * t_s + RULE["h0"] * dist_m + RULE["k"] * int_v_tau)


def timeline_for(route, windows):
    """Truth timeline [(t, phase, along-route metres, truth speed, (window, index) or None)], window spans, exit start.

    With one 30 s stop this is exactly the exp8 / exp7 H8 timeline; without windows it is exp7 H0."""
    d_start_tun = route.entrance + APPROACH_END_INSIDE_M
    d_mid = d_start_tun + (route.exit - d_start_tun) / 2
    tl = [(k, "approach", d_start_tun - SPEED * (APPROACH_FIXES - 1 - k), SPEED, None) for k in range(APPROACH_FIXES)]
    d, t, wi, moved, spans = d_start_tun, APPROACH_FIXES - 1, 0, 0, []
    while True:
        t += 1
        d += SPEED
        if d >= route.exit:
            break
        tl.append((t, "tunnel", d, SPEED, None))
        moved += 1
        if wi < len(windows):
            w = windows[wi]
            if (d >= d_mid) if w["gap"] is None else (moved >= w["gap"]):
                spans.append([t + 1, t + w["length"]])
                for k in range(w["length"]):
                    t += 1
                    d += w["v"]
                    if d >= route.exit:
                        raise ValueError("a window reaches the tunnel exit")
                    tl.append((t, "tunnel", d, w["v"], (wi, k)))
                wi += 1
                moved = 0
    if wi != len(windows):
        raise ValueError("not every window fits into the tunnel")
    exit_start = t
    for k in range(EXIT_FIXES):
        if k:
            d += SPEED
        tl.append((exit_start + k, "exit", d, SPEED, None))
    return tl, spans, exit_start


def photo_times(tl, spans, exit_start):
    """exp7 schedule (10 s grid, dense entrance, 2 s after the exit, last second) plus, for every window [s0, s1], s0, s0+2, s1,
    s1+3 and every 4 s from s0-7 to s1+12, and, for runs with a window, every 4 s from exit-36 to exit-4 (post-stop freeze)."""
    shots = {x[0] for x in tl if x[0] % 10 == 0}
    shots |= set(ENTRANCE_DENSE)
    shots |= set(range(exit_start, exit_start + 11, 2))
    shots.add(tl[-1][0])
    for s0, s1 in spans:
        shots |= {s0, s0 + 2, s1, s1 + 3}
        shots |= set(range(s0 - WINDOW_PRE, s1 + WINDOW_POST + 1, WINDOW_STEP))
    if spans:
        shots |= set(range(exit_start - FREEZE_PRE, exit_start - FREEZE_POST + 1, WINDOW_STEP))
    return shots


def build_run(route, tag, params):
    """Return (lines without setup, plan dict). Times t are seconds since the first approach fix."""
    windows = params["windows"]
    rate = params.get("dr")
    tl, spans, exit_start = timeline_for(route, windows)
    shots = photo_times(tl, spans, exit_start)
    t_anchor = APPROACH_FIXES - 1  # last GPS fix before the tunnel (accuracy rule anchor)
    d_anchor = tl[t_anchor][2]
    err = (0.0, 0.0)  # DR position error (east, north metres), exact at the first tunnel fix
    tau = -1  # DR moving seconds since the first tunnel fix
    prev = None  # (truth d, tau) of the previous moving tunnel fix, for the next DR step
    held = None  # (lat, lon, bearing, along, cross, error, heading error) of the last moving tunnel fix
    int_v_tau = 0.0  # int v * tau dtau since the anchor (piecewise-constant speed per second)
    lines, frames, dr_stops, dr_end, honest = [], [], [], None, {}
    for i, (t, phase, d, v, win) in enumerate(tl):
        lat, lon, road = route.at(d)
        rec = dict(t=t, phase=phase, truth_from_entrance_m=round(d - route.entrance, 1),
                   truth_to_exit_m=round(route.exit - d, 1), truth_progress_m=round(d - tl[0][2], 1),
                   speed_mps=round(v, 3), stopped=win is not None and v == 0.0, road_bearing_deg=round(road, 1))
        if t > t_anchor:
            s = t - t_anchor  # seconds since the anchor at the end of this second
            int_v_tau += v * (s - 0.5)
        if phase == "tunnel":
            honest[t] = rec["accuracy_honest_m"] = honest_budget(t - t_anchor, d - d_anchor, int_v_tau)
        if win is not None:
            w = windows[win[0]]
            rec.update(window=win[0], window_index=win[1], window_kind="stop" if w["v"] == 0.0 else "crawl")
            if rate is not None:  # drifted stop: hold the last DR fix (position and bearing)
                slat, slon, sent, along, cross, error, off = held
                rec.update(dr_seconds=tau, dr_along_m=along, dr_cross_m=cross, dr_error_m=error, bearing_offset_deg=off)
            else:  # true position (stop: identical to the last moving fix; crawl: forward along the route)
                slat, slon, sent = lat, lon, road
            line = fmt(slat, slon, w["acc"], w["v"], sent)
            rec.update(accuracy_m=w["acc"], speed_sent_mps=round(w["v"], 3), bearing_sent_deg=round(sent % 360.0, 1))
        elif phase == "tunnel":
            if rate is not None:
                tau += 1
                if prev is not None:
                    de, dn = E7.dr_error_step(route, prev[0], SPEED, prev[1], rate)
                    err = (err[0] + de, err[1] + dn)
                prev = (d, tau)
                off = rate * tau
                slat, slon = offset_position(lat, lon, err)
                sent = (road + off) % 360.0
                along, cross = E7.decompose(err, road)
                held = (slat, slon, sent, round(along, 1), round(cross, 1), round(math.hypot(*err), 1), round(off, 1))
                rec.update(dr_seconds=tau, dr_along_m=held[3], dr_cross_m=held[4], dr_error_m=held[5],
                           bearing_offset_deg=held[6], sent_progress_m=round(d - tl[0][2] + along, 1))
                if tl[i + 1][4] is not None:
                    dr_stops.append(dict(t_last_moving=t, stop_start=t + 1, dr_seconds=tau, heading_error_deg=held[6],
                                         cross_track_m=held[4], along_track_m=held[3], error_m=held[5],
                                         accuracy_honest_m=rec["accuracy_honest_m"]))
                if tl[i + 1][1] == "exit":
                    nlat, nlon, _ = route.at(tl[i + 1][2])
                    dr_end = dict(t=t, dr_seconds=tau, heading_error_deg=held[6], cross_track_m=held[4],
                                  along_track_m=held[3], error_m=held[5],
                                  exit_jump_m=round(hav((slat, slon), (nlat, nlon)), 1),
                                  exit_jump_without_progress_m=round(hav((slat, slon), (lat, lon)), 1))
            else:
                slat, slon, sent = lat, lon, road
            line = fmt(slat, slon, ACC_TUNNEL, v, sent)
            rec.update(accuracy_m=ACC_TUNNEL, speed_sent_mps=round(v, 3), bearing_sent_deg=round(sent, 1))
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
    t_end = tl[-1][0]
    honest_at_window = [[honest[s0 - 1], honest[s1]] for s0, s1 in spans]  # last moving fix before, last window second
    plan = dict(tag=tag, windows=[dict(w) for w in windows], drift_rate_deg_s=rate, duration_s=t_end + 1,
                approach=[0, APPROACH_FIXES - 1], tunnel=[tunnel_ts[0], tunnel_ts[-1]], exit=[exit_start, t_end],
                window_spans=spans, accuracy_honest_at_windows_m=honest_at_window,
                start=[round(start[0], 7), round(start[1], 7), round(start[2], 1)], frames=frames)
    if rate is not None:
        plan["dr_stop_start"] = dr_stops
        plan["dr_end"] = dr_end
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
        for rep in range(1, REPS.get(base, 2) + 1):
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
            per_run = estimate(plans, ["AC20-r2"], 0.0)[1]
            tier.update(always=list(TIER3_ALWAYS), minimum_input_s=min_s, minimum_estimate_min=min_est,
                        per_added_run_min=per_run, note="candidate list; the README rule selects the runs")
        tiers.append(tier)
    drift = {base: dict(rate_deg_s=p["dr"], stop_start=plans[base + "-r1"]["dr_stop_start"][0],
                        end_of_tunnel=plans[base + "-r1"]["dr_end"]) for base, p in RUNS.items() if "dr" in p}
    doc = dict(geometry=dict(entrance_m=round(route.entrance, 1), exit_m=round(route.exit, 1),
                             tunnel_length_m=round(route.exit - route.entrance, 1), route_length_m=round(route.length, 1)),
               accuracy_rule=RULE, order=ORDER, tiers=tiers, drift_summary=drift, runs=plans)
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
        spans = " ".join(f"{a}..{b}" for a, b in p["window_spans"])
        print(f"{tag:20s} {p['duration_s']:4d} s  {len(p['frames']) + 1:3d} photos  tunnel t={p['tunnel'][0]}..{p['tunnel'][1]}"
              f"  exit t={p['exit'][0]}" + (f"  windows {spans}  honest {p['accuracy_honest_at_windows_m']}" if spans else ""))
    for tier in doc["tiers"]:
        extra = ""
        if tier["tier"] == 3:
            extra = (f" (candidates; minimum {tier['always']}: about {tier['minimum_estimate_min'][0]}-"
                     f"{tier['minimum_estimate_min'][1]} min; each added r2 about {tier['per_added_run_min'][0]}-"
                     f"{tier['per_added_run_min'][1]} min)")
        print(f"tier {tier['tier']}: {len(tier['runs'])} runs, {tier['input_s']} s input, {tier['photos']} photos, "
              f"about {tier['estimate_min'][0]}-{tier['estimate_min'][1]} min{extra}")
    for base, x in doc["drift_summary"].items():
        s, e = x["stop_start"], x["end_of_tunnel"]
        print(f"{base}: stop start t={s['stop_start']} heading {s['heading_error_deg']:+.1f} deg, cross {s['cross_track_m']:+.1f} m, "
              f"along {s['along_track_m']:+.1f} m, error {s['error_m']:.1f} m, honest {s['accuracy_honest_m']} m | end of tunnel "
              f"heading {e['heading_error_deg']:+.1f} deg, cross {e['cross_track_m']:+.1f} m, exit jump {e['exit_jump_m']:.1f} m")


if __name__ == "__main__":
    main(sys.argv)
