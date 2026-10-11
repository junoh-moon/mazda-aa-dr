#!/usr/bin/env python3
"""Generate DHU command files for experiment 11 (G1: why Naver showed 'GPS not acquired' and a frozen speed in real tunnels).

Question. In the first real v1.0.0-beta.10 drive, while BETA replaced the stock mode-0 sends in tunnels, Naver showed a 111 s episode
grey with the displayed speed frozen (our sent speed kept changing 3.9-17 m/s), a 60 s episode grey with the speed following, and a
14 s episode normal. What we sent: 1 fix per second, accuracy ramping 30 -> 40 m (cap), wheel speed, a bearing, a position drifting
left of the truth (cross-track 119 m after 111 s, 52 m after 60 s, 24 m after 14 s), timestamp 0 (firmware fact, not a variable), and
GPS_SATELLITE (sensor type 21) number_in_use 0 in tunnels vs 5 with a fix. Experiment 11 injects a tunnel of the same shape on the
proven experiment 9/10 route and changes one candidate mechanism at a time:
  V1 satellites in the tunnel: 5 (V1S5) / 0 (V1S0) / never sent (V1SN: same file, DHU sensor disabled), accuracy 20 m, cross-track 0
  V2 accuracy 40 m for 120 s (satellites 5, cross-track 0)        V3 cross-track drift 0 -> 120 m over 120 s (satellites 5, 20 m)
  V4 the real combination (ramp 30 -> 40 m cap, satellites 0, drift to 119 m over 111 s): positive control
  V2C30 (v2, Tier 2): V2 with the 40 m held for 30 s only, then 20 m (does following come back after the accuracy drops?)
v2 (2026-10-11, a second real tunnel: 53 s, 40 m cap within 4 s for 49 s, return error only 24-35 m, owner: 'it worked a bit inside
the tunnel and then stopped'): the cross-track candidate V3 is weakened, the time at the 40 m cap (V2) and the satellites (V1) lead.
Tier 1 order V4 -> V1S0 -> V2 -> V3 (V3 last, so the time rule truncates V3, not V2). Every v1 command file is unchanged.
  V5 V4 with cross-track 0                                        V4T60 / V4T14: V4 shape for 60 s (52 m) / 14 s (24 m)
Proven procedure reused byte for byte (rule 1 of the owner's DHU rules):
  * preparation = experiment 10 v5 commands/prep/ (search -> 모라역 row (tap 230 y) -> route start -> gate), condition B2 (true bearing,
    accuracy 1 m), copied, not regenerated; the Tier 0 stability lines = experiment 10 v5 T0-PRE.txt after its line 18.
  * approach = the experiment 9 ST8-nostop approach (20 fixes at 14 m/s from the exp 9/10 start point), accuracy 5 m -> 1 m (exp 10 v5).
Independent variables only: the tunnel fixes (accuracy, cross-track drift and its bearing), the `gps_satellite <n>` lines and, for V1SN,
the DHU ini (sensor off). Every run uses the same varying tunnel speed profile so that a frozen speed is readable (experiment 7 used a
constant 14 m/s, so 'follows' and 'extrapolates' could not be told apart there).
Body timeline (t = seconds since the first body line, sent after the gate-G screenshot of the preparation):
  t = 0 .. 19           approach, 1 m, satellites 5
  t = 20 .. 20+T-1      tunnel episode (tau = t - 20; tau 0 is the exp 9 t=20 fix), the variable under test, speed profile SPEED_PROFILE
  t = 19+T .. +REC-1    recovery ('GPS back'): true position along the route at 14 m/s, 1 m, satellites 5 (still inside the mapped tunnel)
Syntax: location <lat> <lon> <accuracy_m|NAN> <altitude|NAN> <speed_mps> <bearing_deg|NAN>; gps_satellite <number_in_use>
(Android DHU documentation, 'gps_satellite number_in_use [[azimuth] [elevation] [prn] [snr] [used_in_fix]]'; only number_in_use is sent).
Usage: gen_commands.py [out_dir [run_plan_json]]
"""
import importlib.util
import json
import math
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VAL = os.path.join(HERE, "..")
EXP7_DIR = os.path.join(VAL, "DHU_NAVER_EXP7_PLAN_2026-10-09")
EXP10_DIR = os.path.join(VAL, "DHU_NAVER_EXP10_PLAN_2026-10-11")


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


E7 = _load("dhu_exp7_for_exp11", os.path.join(EXP7_DIR, "gen_commands.py"))
Route, fmt, offset_position, dr_error_step, decompose, hav = (E7.Route, E7.fmt, E7.offset_position, E7.dr_error_step,
                                                              E7.decompose, E7.hav)

SHOT = "../../../../experiment11/"
ACC_GPS = 1.0  # exp 10 v5: every preparation / pre-tunnel / recovery fix at 1 m (the phone's own fix beat 5 m after a reconnect)
ACC_BASE = 20.0  # V1/V3 tunnel accuracy (exp3 C3: 20 m moving fixes followed in the mapped tunnel)
ACC_CAP = 40.0  # product tunnel cap (TUNNEL_UNBOUNDED_2026-10-09)
CAP30_S = 30  # V2C30: seconds at 40 m before the drop to 20 m
RAMP = dict(start=30.0, rate=0.72)  # V4 accuracy ceil(30 + 0.72 tau) capped at 40 (honest-budget growth 0.3 m/s + 0.03 rad * 14 m/s)
SAT_FIX, SAT_LOST = 5, 0  # number_in_use with a fix / in a tunnel (coordinator brief on the beta.10 drive)
V_APPROACH = E7.SPEED  # 14 m/s
APPROACH_N = E7.APPROACH_FIXES  # 20 approach fixes (exp 9 ST8: t=0..19 at 5 m, the last one 20 m inside the entrance; t=20 first 40 m fix)
REC_S = 20
V_REC = E7.SPEED
# Tunnel speed profile (m/s) by tau: plateaus 14 / 4 / 17 / 9 / 6 m/s (50.4 / 14.4 / 61.2 / 32.4 / 21.6 km/h, inside the real 3.9-17 m/s
# range), 5 s linear transitions. Every plateau differs from the approach speed and from its neighbours by >= 10 km/h.
PLATEAUS = [(0, 14, 14.0), (20, 39, 4.0), (45, 64, 17.0), (70, 89, 9.0), (95, 119, 6.0)]
CHECK_TAU = (25, 30, 35, 50, 55, 60, 75, 80, 85, 100, 105, 110)  # plateau speed checks (>= 5 s after each plateau starts)
EPISODE_PHOTOS = {0, 2, 4, 6, 8, 10} | set(range(15, 120, 5))
REC_PHOTOS = {0, 1, 2, 3, 5, 7, 10, 15, REC_S - 1}
APPROACH_PHOTOS = {0, 5, 10, 15, 19}
EXPECTED_D0_KM = (3.9, 4.7)

PREP_FILES = ("B2-search", "B2-route-y130", "B2-route-y225", "B2-route-y318", "B2-route-retry", "B2-gate", "B2-gate-nopopup",
              "search-again", "end-guidance")
T0_SOURCE = "T0-PRE"  # exp 10 v5 file; its lines after N_T0_SKIP are the 20 s stability stream
N_T0_SKIP = 18

# tag base -> parameters. sat: satellites in the tunnel; acc: 'base' 20 m, 'cap' 40 m, 'ramp' 30 -> 40; xt: cross-track target at the
# episode end (m, drift to the LEFT as in the vehicle: 'GPS right of DR'); ini: 'sat' (gps_satellite enabled) or 'exp4' (disabled).
CONDS = {
    "V4": dict(T=111, sat=SAT_LOST, acc="ramp", xt=119.0, ini="sat", q="real combination (positive control)"),
    "V1S0": dict(T=120, sat=SAT_LOST, acc="base", xt=0.0, ini="sat", q="satellites 0 alone"),
    "V3": dict(T=120, sat=SAT_FIX, acc="base", xt=120.0, ini="sat", q="cross-track drift alone"),
    "V2": dict(T=120, sat=SAT_FIX, acc="cap", xt=0.0, ini="sat", q="40 m cap for 120 s alone (time to the first non-following sign)"),
    "V2C30": dict(T=120, sat=SAT_FIX, acc="cap30", xt=0.0, ini="sat", q="40 m for 30 s, then 20 m: does following come back"),
    "V1S5": dict(T=120, sat=SAT_FIX, acc="base", xt=0.0, ini="sat", q="baseline (satellites 5, 20 m, no drift)"),
    "V5": dict(T=111, sat=SAT_LOST, acc="ramp", xt=0.0, ini="sat", q="real combination without the drift"),
    "V1SN": dict(T=120, sat=SAT_FIX, acc="base", xt=0.0, ini="exp4", q="satellite sensor never sent (V1S5 file, sensor off)"),
    "V4T60": dict(T=60, sat=SAT_LOST, acc="ramp", xt=52.0, ini="sat", q="real combination, 60 s episode"),
    "V4T14": dict(T=14, sat=SAT_LOST, acc="ramp", xt=24.0, ini="sat", model="seed", q="real combination, 14 s episode (seed-limited)"),
}
TIERS = [
    ["V4-r1", "V1S0-r1", "V2-r1", "V3-r1"],
    ["V1S5-r1", "V2C30-r1", "V5-r1", "V1SN-r1", "V4-r2", "V4T60-r1", "V4T14-r1", "V1S0-r2", "V2-r2", "V3-r2", "V1S5-r2"],
]
NOSAT_TIER1 = ["V4-r1", "V2-r1", "V3-r1"]  # Tier 0 fallback: run with the exp4 ini (gps_satellite lines are ignored by the DHU)
RUN_MIN = 4.5  # exp 10 v4 measured: preparation + 130 s body about 4 min; body here up to 159 s
TIER0_MIN = (5.0, 7.0)
BUDGET_MIN = 25


def speed(tau):
    for i, (a, b, v) in enumerate(PLATEAUS):
        if a <= tau <= b:
            return v
        nxt = PLATEAUS[i + 1] if i + 1 < len(PLATEAUS) else None
        if nxt and b < tau < nxt[0]:
            return v + (nxt[2] - v) * (tau - b) / (nxt[0] - b)
    raise ValueError(tau)


def plateau_of(tau):
    for a, b, v in PLATEAUS:
        if a <= tau <= b:
            return v
    return None


def freeze_detectable_from():
    """First tau whose sent speed differs from the approach speed by >= 10 km/h (a frozen display becomes readable)."""
    return next(t for t in range(120) if abs(speed(t) - V_APPROACH) * 3.6 >= 10.0)


def tunnel_acc(kind, tau):
    if kind == "base":
        return ACC_BASE
    if kind == "cap":
        return ACC_CAP
    if kind == "cap30":
        return ACC_CAP if tau < CAP30_S else ACC_BASE
    return min(ACC_CAP, float(math.ceil(RAMP["start"] + RAMP["rate"] * tau)))


def start_tunnel(route):
    return route.entrance + E7.APPROACH_END_INSIDE_M


def seed_error_step(route, d0, v, off, substeps=E7.DR_SUBSTEPS):
    """exp7 dr_error_step with a CONSTANT heading offset `off` (deg): the seed-limited form of the real 14 s episode."""
    de = dn = 0.0
    h = 1.0 / substeps
    for k in range(substeps):
        road = route.at(min(d0 + v * (k + 0.5) * h, route.length))[2]
        te, tn = E7.unit(road)
        we, wn = E7.unit(road + off)
        de += v * h * (we - te)
        dn += v * h * (wn - tn)
    return de, dn


def heading_error(model, x, tau):
    return x * tau if model == "yaw" else x


def episode(route, T, x, model="yaw"):
    """[(tau, d, v, err)] for tau = 0..T-1. model 'yaw': heading error x * tau (deg/s, exp7 R model); 'seed': constant x (deg)."""
    d, err, out = start_tunnel(route) + V_APPROACH, (0.0, 0.0), []
    for tau in range(T):
        out.append((tau, d, speed(tau), err))
        v1 = speed(tau + 1) if tau + 1 < 120 else speed(119)
        de, dn = dr_error_step(route, d, v1, tau, x) if model == "yaw" else seed_error_step(route, d, v1, x)
        err, d = (err[0] + de, err[1] + dn), d + v1
    return out


def solve_rate(route, T, target_left_m, model="yaw"):
    """Heading error rate (deg/s) or constant offset (deg) (negative = drift to the left): cross-track at the last fix = -target."""
    if target_left_m == 0:
        return 0.0
    lo, hi = (-2.0, 0.0) if model == "yaw" else (-45.0, 0.0)
    for _ in range(60):
        mid = (lo + hi) / 2
        tau, d, _, err = episode(route, T, mid, model)[-1]
        cross = decompose(err, route.at(d)[2])[1]
        if cross < -target_left_m:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def build_run(route, tag, c, sat_lines=True):
    lines, frames = [], []
    s_tun = start_tunnel(route)
    s0 = s_tun - V_APPROACH * (APPROACH_N - 1)
    model = c.get("model", "yaw")
    rate = solve_rate(route, c["T"], c["xt"], model)
    ep = episode(route, c["T"], rate, model)

    def emit(t, sat, line, rec):
        if sat_lines:
            lines.append(f"gps_satellite {sat}")
        lines.append(line)
        rec.update(t=t, satellites_sent=sat)
        if rec.pop("photo"):
            name = f"{tag}-{t:03d}.png"
            lines.extend(["sleep 0.2", f"screenshot {SHOT}{name}", "sleep 0.8"])
            rec["screenshot"] = name
            frames.append(rec)
        else:
            lines.append("sleep 1")

    frozen_d = s_tun + V_APPROACH  # progress if Naver extrapolated the approach speed from the first tunnel second
    for k in range(APPROACH_N):
        d = s0 + V_APPROACH * k
        lat, lon, road = route.at(d)
        emit(k, SAT_FIX, fmt(lat, lon, ACC_GPS, V_APPROACH, road),
             dict(phase="approach", photo=k in APPROACH_PHOTOS, progress_m=round(d - s0, 1), accuracy_m=ACC_GPS,
                  speed_sent_kmh=round(V_APPROACH * 3.6, 1), bearing_sent_deg=round(road, 1), cross_track_m=0.0))
    end = None
    for tau, d, v, err in ep:
        lat, lon, road = route.at(d)
        along, cross = decompose(err, road)
        slat, slon = offset_position(lat, lon, err)
        acc = tunnel_acc(c["acc"], tau)
        brg = (road + heading_error(model, rate, tau)) % 360.0
        frozen = frozen_d + V_APPROACH * tau
        rec = dict(phase="tunnel", tau=tau, photo=tau in EPISODE_PHOTOS, progress_m=round(d - s0, 1),
                   progress_if_frozen_m=round(frozen - s0, 1), accuracy_m=acc, speed_sent_kmh=round(v * 3.6, 1),
                   plateau_kmh=None if plateau_of(tau) is None else round(plateau_of(tau) * 3.6, 1), speed_check=tau in CHECK_TAU,
                   bearing_sent_deg=round(brg, 1), heading_error_deg=round(heading_error(model, rate, tau), 1), cross_track_m=round(cross, 1),
                   along_track_m=round(along, 1), truth_from_entrance_m=round(d - route.entrance, 1))
        emit(APPROACH_N + tau, c["sat"], fmt(slat, slon, acc, v, brg), rec)
        end = dict(tau=tau, cross_track_m=round(cross, 1), along_track_m=round(along, 1), heading_error_deg=round(heading_error(model, rate, tau), 1),
                   truth_from_entrance_m=round(d - route.entrance, 1))
    d = ep[-1][1]
    for r in range(REC_S):
        d += V_REC
        lat, lon, road = route.at(d)
        if d >= route.exit:
            raise AssertionError("recovery must stay inside the mapped route")
        emit(APPROACH_N + c["T"] + r, SAT_FIX, fmt(lat, lon, ACC_GPS, V_REC, road),
             dict(phase="recovery", rho=r, photo=r in REC_PHOTOS, progress_m=round(d - s0, 1), accuracy_m=ACC_GPS,
                  speed_sent_kmh=round(V_REC * 3.6, 1), bearing_sent_deg=round(road, 1), cross_track_m=0.0,
                  truth_from_entrance_m=round(d - route.entrance, 1)))
    plan = dict(tag=tag, condition=tag.rsplit("-r", 1)[0], question=c["q"], ini=c["ini"], episode_s=c["T"],
                satellites_tunnel=c["sat"], accuracy_tunnel=c["acc"], cross_track_target_m=c["xt"],
                drift_model=model, heading_error_rate_deg_s=round(rate, 5) if model == "yaw" else None,
                heading_offset_deg=round(rate, 3) if model == "seed" else None, episode_end=end, duration_s=APPROACH_N + c["T"] + REC_S,
                tunnel_t=[APPROACH_N, APPROACH_N + c["T"] - 1], recovery_t=[APPROACH_N + c["T"], APPROACH_N + c["T"] + REC_S - 1],
                speed_checks=[x for x in CHECK_TAU if x < c["T"]], frames=frames)
    return lines, plan


def tier0_lines(sat):
    """exp 10 v5 T0-PRE stability stream (after line 18), screenshots renamed; with sat, `gps_satellite 5` before each fix."""
    src = open(os.path.join(EXP10_DIR, "commands", T0_SOURCE + ".txt")).read().splitlines()[N_T0_SKIP:]
    tag = "T0-SAT" if sat else "T0-NOSAT"
    out = []
    for x in src:
        if x.startswith("screenshot "):
            x = x.replace("../../../../experiment10/T0-PRE", SHOT + tag)
        if x.startswith("location ") and sat:
            out.append(f"gps_satellite {SAT_FIX}")
        out.append(x)
    return out


def generate(out_dir, plan_path=None):
    route = Route(os.path.join(HERE, "geometry", "route.json"))
    os.makedirs(os.path.join(out_dir, "prep"), exist_ok=True)

    def write(name, lines):
        with open(os.path.join(out_dir, name + ".txt"), "w") as f:
            f.write("\n".join(lines) + "\n")

    for name in PREP_FILES:  # byte copies of the experiment 10 v5 preparation
        shutil.copyfile(os.path.join(EXP10_DIR, "commands", "prep", name + ".txt"), os.path.join(out_dir, "prep", name + ".txt"))
    write("SAT5", [f"gps_satellite {SAT_FIX}"])
    write("T0-SAT", tier0_lines(True))
    write("T0-NOSAT", tier0_lines(False))
    plans = {}
    for tier in TIERS:
        for tag in tier:
            c = CONDS[tag.rsplit("-r", 1)[0]]
            lines, plan = build_run(route, tag, c)
            write(tag, lines)
            plans[tag] = plan
    s_tun = start_tunnel(route)
    s0 = s_tun - V_APPROACH * (APPROACH_N - 1)
    lat0, lon0, road0 = route.at(s0)
    tiers = []
    for i, runs in enumerate(TIERS):
        tiers.append(dict(tier=str(i + 1), runs=runs, input_s=sum(plans[t]["duration_s"] for t in runs),
                          photos=sum(len(plans[t]["frames"]) for t in runs), estimate_min=round(len(runs) * RUN_MIN, 1)))
    doc = dict(site=dict(start=[round(lat0, 7), round(lon0, 7)], start_route_m=round(s0, 1), true_bearing_deg=round(road0, 1),
                         tunnel_start_route_m=round(s_tun, 1), entrance_route_m=round(route.entrance, 1),
                         tunnel_length_m=round(route.exit - route.entrance, 1), expected_d0_km=list(EXPECTED_D0_KM)),
               timeline=dict(approach_t=[0, APPROACH_N - 1], tunnel_first_t=APPROACH_N, recovery_s=REC_S,
                             speed_profile_kmh=[[a, b, round(v * 3.6, 1)] for a, b, v in PLATEAUS], transition_s=5,
                             check_tau=list(CHECK_TAU), freeze_detectable_from_tau=freeze_detectable_from(),
                             t_stop_photo_tau=sorted(t for t in EPISODE_PHOTOS if t >= freeze_detectable_from()), accuracy_gps_m=ACC_GPS, accuracy_base_m=ACC_BASE, accuracy_cap_m=ACC_CAP,
                             ramp=RAMP, satellites=dict(fix=SAT_FIX, lost=SAT_LOST)),
               prep=dict(source="validation/DHU_NAVER_EXP10_PLAN_2026-10-11/commands/prep (v5, accuracy 1 m), byte copies",
                         files=[f"prep/{n}.txt" for n in PREP_FILES], condition="B2 (true bearing 328.9)",
                         placeholder="@TAG@ in screenshot lines only (photos land in experiment10/ with exp11 tags)"),
               tier0=dict(files=["SAT5.txt", "T0-SAT.txt", "T0-NOSAT.txt"], source=f"exp10 v5 {T0_SOURCE}.txt after line {N_T0_SKIP}",
                          estimate_min=list(TIER0_MIN)),
               ini=dict(sat="dhu-sat.ini (exp4 dhu.ini + gps_satellite = true)", exp4="validation/DHU_NAVER_EXP4_2026-10-03/dhu.ini"),
               budget_min=BUDGET_MIN, run_min=RUN_MIN, tiers=tiers, nosat_tier1=NOSAT_TIER1, runs=plans)
    if plan_path:
        with open(plan_path, "w") as f:
            json.dump(doc, f, ensure_ascii=False, indent=1)
            f.write("\n")
    return route, doc


def main(argv):
    out = argv[1] if len(argv) > 1 else os.path.join(HERE, "commands")
    plan = argv[2] if len(argv) > 2 else os.path.join(HERE, "run-plan.json")
    _, doc = generate(out, plan)
    for tag, p in doc["runs"].items():
        e = p["episode_end"]
        print(f"{tag}: {p['duration_s']} s, episode {p['episode_s']} s, sat {p['satellites_tunnel']}, acc {p['accuracy_tunnel']}, "
              f"rate {p['heading_error_rate_deg_s']} deg/s off {p['heading_offset_deg']}, end cross {e['cross_track_m']} m, heading err {e['heading_error_deg']}, "
              f"from entrance {e['truth_from_entrance_m']} m, photos {len(p['frames'])}")
    for t in doc["tiers"]:
        print(f"tier {t['tier']}: {len(t['runs'])} runs, {t['input_s']} s input, {t['photos']} photos, about {t['estimate_min']} min")


if __name__ == "__main__":
    main(sys.argv)
