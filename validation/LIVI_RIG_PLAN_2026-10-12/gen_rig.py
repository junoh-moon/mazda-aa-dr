#!/usr/bin/env python3
"""Generate the LIVI test head unit (rig) plan files: the R4 long-tunnel input, the Tier 0 moving stream, the
driver scenarios and the per-photo sent values (rig-run-plan.json).

The rig replaces the DHU by the patched f-io/LIVI head unit (tools/livi-patch, v2) on the owner's Mac, with the
Mazda Connect sensor configuration (characterization 256, sensors 1,3,7,8,10,13,21, GPS_SATELLITE batches,
1 Hz fix resend, unoffered sensor requests rejected). The bodies of R0-R3 and R5 are the experiment 11 command
files BYTE FOR BYTE (they are read from ../DHU_NAVER_EXP11_PLAN_2026-10-11/commands/ by the driver); only the
driver scenario (scenarios.json) changes the one variable of each run during the GPS loss:
  R0  V4-r1, satellites 0 in the loss (as the file)       control: does the rig show what the DHU showed (grey at 65-85 s, speed follows)?
  R1  V4-r1, satellites 5 in the loss                      the satellite count alone (the DHU could not send it)
  R2  V4-r1, satellites 0, accuracy capped at 20 m (A20)   the reported accuracy alone
  R3  V2-r1 (40 m for 120 s, no offset), satellites 0      the 40 m hold with satellites 0 (DHU V2 was normal with no satellite sensor)
  R4  a long tunnel shaped like the real 111 s one (built here from the published G1/log audit numbers), with the
      Mazda cadence: one stock mode-0 message first, then BETA-like fixes (30 -> 40 m within 4 s), satellites 0
  R5  V4T60-r1 replaced by the stock mode-0 resend for the whole 60 s (cached fix, no accuracy, satellites 0)
The LIVI switches (LIVI_ENV) also make the rig answer an unoffered sensor request with -9 and send a sensor only after
the phone subscribed it, as the Mazda unit does. Every scenario also sends SPEED (type 3) with the base file's speed (the Mazda's wheel speed) and satellites 5 next to
every fix outside the loss (mazda.sat_default, the Mazda's mode-1 value).

R4 is a REALISM run, not a one-variable run: its speed profile is SYNTHETIC, chosen to match the published numbers of
the trip8 (b) episode (validation/G1_FIRST_TUNNELS_2026-10-11.md: 111 s, wheel speed 13/41/61 km/h min/median/max, no
stop, cross-track 118.5 m to the left at the exit, DR end bearing 16-22 deg left of the exit course;
validation/LOG_AUDIT_TUNNELS_2026-10-12.md: 40 m reached at +4.0 s, 18 sends below 20 km/h between +68 and +101 s, the
first replacement about 1 s after one stock mode-0 message). No private per-second data is copied.

Usage: gen_rig.py [out_dir]   (default: this directory; writes commands/, scenarios.json, rig-run-plan.json)
"""
import importlib.util
import json
import math
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXP11_DIR = os.path.join(HERE, "..", "DHU_NAVER_EXP11_PLAN_2026-10-11")
EXP11_REL = "../DHU_NAVER_EXP11_PLAN_2026-10-11"


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


G11 = _load("dhu_exp11_for_rig", os.path.join(EXP11_DIR, "gen_commands.py"))
E7 = G11.E7
SHOT = G11.SHOT  # ../../../../experiment11/ (the driver keeps only the file name)

MAZDA = {"sat_default": 5, "speed_sensor": True}
SCREEN = [800, 480]  # exp4 dhu.ini resolution: the preparation taps are in these pixels
LIVI_ENV = {
    "LIVI_LOC_CHARACTERIZATION": "256",
    "LIVI_SENSORS": "1,3,7,8,10,13,21",
    "LIVI_GPS_SATELLITES": "1",
    "LIVI_RESEND_FIXES": "1",
    "LIVI_REJECT_UNOFFERED_SENSORS": "1",
    "LIVI_SEND_ONLY_REQUESTED_SENSORS": "1",
    "LIVI_TEST_TAPS": "1",
    "LIVI_LOG_SENSOR_REQUESTS": "<session folder>/sensor-log.jsonl",
}
LIVI_CONFIG = {"projectionWidth": 800, "projectionHeight": 480, "projectionDpi": 160, "projectionFps": 30}

# R4: synthetic speed knots (tau s, km/h), linear in between, rounded to whole km/h per second.
R4_T = 111
R4_KNOTS = [(0, 47), (12, 55), (22, 61), (32, 58), (44, 50), (56, 41), (63, 31), (67, 22), (70, 16), (73, 13), (76, 18),
            (79, 24), (87, 26), (90, 19), (93, 14), (96, 17), (100, 19), (102, 24), (110, 38)]
R4_XT = 118.5  # cross-track to the left at the last episode fix (m)
R4_ACC = dict(start=30.0, rate=3.4, cap=40.0)  # tau >= 1: min(40, ceil(30 + 3.4 (tau - 1))): 30, 34, 37, 40 at tau 4
R4_STOCK_HEAD = 1
PUBLISHED_R4 = dict(min_kmh=13, median_kmh=41, max_kmh=61, below20_sends=18, below20_window=[68, 101], wheel_m=1143,
                    end_bearing_left_deg=[16, 22], cross_left_m=118.5, cap_at_s=4.0)

RUNS = {  # name -> scenario (driver keys) + question
    "R0-r1": dict(base="V4-r1", sat=0, cap=None, head=0, whole=False,
                  q="control: V4 under the Mazda sensor set with satellites 0 (DHU V4: grey at 65-85 s in 3/3, speed followed 12/12)"),
    "R1-r1": dict(base="V4-r1", sat=5, cap=None, head=0, whole=False, q="R0 with satellites 5 in the loss (hypothesis G2/S)"),
    "R2-r1": dict(base="V4-r1", sat=0, cap=20.0, head=0, whole=False, q="R0 with the accuracy capped at 20 m (A20, hypothesis G1/G3)"),
    "R3-r1": dict(base="V2-r1", sat=0, cap=None, head=0, whole=False,
                  q="40 m for 120 s, no offset, satellites 0 (DHU V2 without a satellite sensor was normal)"),
    "R4-r1": dict(base="R4-r1", sat=0, cap=None, head=R4_STOCK_HEAD, whole=False,
                  q="real-like 111 s tunnel with the Mazda cadence (stock first message, 30 -> 40 m in 4 s, satellites 0): frozen speed?"),
    "R5-r1": dict(base="V4T60-r1", sat=None, cap=None, head=0, whole=True,
                  q="the unmodified Mazda: stock mode-0 resend of the cached fix without accuracy, satellites 0, for 60 s"),
    "R0-r2": dict(base="V4-r1", sat=0, cap=None, head=0, whole=False, q="R0 repeated (session 2 opener: same rig as session 1?)"),
    "R1-r2": dict(base="V4-r1", sat=5, cap=None, head=0, whole=False, q="R1 repeated (session 3)"),
    "R2-r2": dict(base="V4-r1", sat=0, cap=20.0, head=0, whole=False, q="R2 repeated (session 3)"),
    "R4-r2": dict(base="R4-r1", sat=0, cap=None, head=R4_STOCK_HEAD, whole=False, q="R4 repeated (session 3)"),
}
SESSIONS = [
    dict(session=1, runs=["R0-r1", "R1-r1", "R2-r1", "R3-r1"], last_only_if_time=["R3-r1"]),
    dict(session=2, runs=["R0-r2", "R4-r1", "R5-r1", "R3-r1"], last_only_if_time=["R3-r1"],
         note="R3-r1 here only if session 1 did not reach it"),
    dict(session=3, runs=["R1-r2", "R2-r2", "R4-r2"], last_only_if_time=[], note="optional repeats for CONFIRMED"),
]
RUN_MIN = 5.0  # DHU exp 10 v4 measured 4.5 min per run (preparation + body); + about 0.5 min to restart LIVI and reconnect
TIER0_MIN = (7.0, 9.0)
TIER0_SHORT_MIN = (4.0, 5.0)
BUDGET_MIN = 25


def read_lines(path):
    with open(path) as f:
        return f.read().splitlines()


def r4_speed_kmh(tau):
    for (a, va), (b, vb) in zip(R4_KNOTS, R4_KNOTS[1:]):
        if a <= tau <= b:
            return float(round(va + (vb - va) * (tau - a) / (b - a)))
    raise ValueError(tau)


def r4_acc(tau):
    return min(R4_ACC["cap"], float(math.ceil(R4_ACC["start"] + R4_ACC["rate"] * max(0, tau - 1) - 1e-9)))


def r4_episode(route, rate):
    """[(tau, d, v_mps, err)] as exp11 episode() with the R4 speed profile (heading error rate * tau, exp7 R model)."""
    d, err, out = G11.start_tunnel(route) + G11.V_APPROACH, (0.0, 0.0), []
    for tau in range(R4_T):
        v = r4_speed_kmh(tau) / 3.6
        out.append((tau, d, v, err))
        v1 = r4_speed_kmh(min(tau + 1, R4_T - 1)) / 3.6
        de, dn = E7.dr_error_step(route, d, v1, tau, rate)
        err, d = (err[0] + de, err[1] + dn), d + v1
    return out


def r4_rate(route):
    lo, hi = -2.0, 0.0
    for _ in range(60):
        mid = (lo + hi) / 2
        _, d, _, err = r4_episode(route, mid)[-1]
        cross = E7.decompose(err, route.at(d)[2])[1]
        lo, hi = (mid, hi) if cross < -R4_XT else (lo, mid)
    return (lo + hi) / 2


def r4_checks():
    """Photo taus whose sent speed differs from the approach speed (50.4 km/h) by >= 10 km/h and is steady (range of
    tau-2..tau <= 4 km/h): there a frozen display and a following display read differently."""
    v0 = G11.V_APPROACH * 3.6
    out = []
    for tau in sorted(G11.EPISODE_PHOTOS):
        if tau < 2 or tau >= R4_T:
            continue
        w = [r4_speed_kmh(t) for t in (tau - 2, tau - 1, tau)]
        if abs(w[-1] - v0) >= 10 and max(w) - min(w) <= 4:
            out.append(tau)
    return out


def build_r4(route):
    """R4-r1 command file (DHU syntax, same approach and recovery as exp11 V4) and its plan."""
    tag, lines, frames = "R4-r1", [], []
    s_tun = G11.start_tunnel(route)
    s0 = s_tun - G11.V_APPROACH * (G11.APPROACH_N - 1)
    rate = r4_rate(route)
    ep = r4_episode(route, rate)
    checks = set(r4_checks())

    def emit(t, sat, line, rec):
        lines.extend([f"gps_satellite {sat}", line])
        rec.update(t=t, satellites_sent=sat)
        if rec.pop("photo"):
            name = f"{tag}-{t:03d}.png"
            lines.extend(["sleep 0.2", f"screenshot {SHOT}{name}", "sleep 0.8"])
            rec["screenshot"] = name
            frames.append(rec)
        else:
            lines.append("sleep 1")

    for k in range(G11.APPROACH_N):
        d = s0 + G11.V_APPROACH * k
        lat, lon, road = route.at(d)
        emit(k, G11.SAT_FIX, E7.fmt(lat, lon, G11.ACC_GPS, G11.V_APPROACH, road),
             dict(phase="approach", photo=k in G11.APPROACH_PHOTOS, progress_m=round(d - s0, 1), accuracy_m=G11.ACC_GPS,
                  speed_sent_kmh=round(G11.V_APPROACH * 3.6, 1), bearing_sent_deg=round(road, 1), cross_track_m=0.0))
    frozen_d = s_tun + G11.V_APPROACH
    end = None
    for tau, d, v, err in ep:
        lat, lon, road = route.at(d)
        along, cross = E7.decompose(err, road)
        slat, slon = E7.offset_position(lat, lon, err)
        brg = (road + rate * tau) % 360.0
        acc = r4_acc(tau)
        rec = dict(phase="tunnel", tau=tau, photo=tau in G11.EPISODE_PHOTOS, progress_m=round(d - s0, 1),
                   progress_if_frozen_m=round(frozen_d + G11.V_APPROACH * tau - s0, 1), accuracy_m=acc,
                   speed_sent_kmh=round(v * 3.6, 1), speed_check=tau in checks, bearing_sent_deg=round(brg, 1),
                   heading_error_deg=round(rate * tau, 1), cross_track_m=round(cross, 1), along_track_m=round(along, 1),
                   truth_from_entrance_m=round(d - route.entrance, 1))
        emit(G11.APPROACH_N + tau, G11.SAT_LOST, E7.fmt(slat, slon, acc, v, brg), rec)
        end = dict(tau=tau, cross_track_m=round(cross, 1), along_track_m=round(along, 1),
                   heading_error_deg=round(rate * tau, 1), truth_from_entrance_m=round(d - route.entrance, 1))
    d = ep[-1][1]
    for r in range(G11.REC_S):
        d += G11.V_REC
        lat, lon, road = route.at(d)
        if d >= route.exit:
            raise AssertionError("recovery must stay inside the mapped route")
        emit(G11.APPROACH_N + R4_T + r, G11.SAT_FIX, E7.fmt(lat, lon, G11.ACC_GPS, G11.V_REC, road),
             dict(phase="recovery", rho=r, photo=r in G11.REC_PHOTOS, progress_m=round(d - s0, 1),
                  accuracy_m=G11.ACC_GPS, speed_sent_kmh=round(G11.V_REC * 3.6, 1), bearing_sent_deg=round(road, 1),
                  cross_track_m=0.0, truth_from_entrance_m=round(d - route.entrance, 1)))
    speeds = [r4_speed_kmh(t) for t in range(R4_T)]
    plan = dict(tag=tag, episode_s=R4_T, heading_error_rate_deg_s=round(rate, 5), episode_end=end,
                duration_s=G11.APPROACH_N + R4_T + G11.REC_S, tunnel_t=[G11.APPROACH_N, G11.APPROACH_N + R4_T - 1],
                recovery_t=[G11.APPROACH_N + R4_T, G11.APPROACH_N + R4_T + G11.REC_S - 1], speed_checks=sorted(checks),
                speed_profile=dict(knots=R4_KNOTS, min_kmh=min(speeds), median_kmh=statistics.median(speeds),
                                   max_kmh=max(speeds), below20=[t for t, v in enumerate(speeds) if v < 20],
                                   wheel_m=round(sum(v / 3.6 for v in speeds), 1)),
                published=PUBLISHED_R4, frames=frames)
    return lines, plan


def transform_frames(base_frames, sc, loss_t):
    """The sent values per photo after the driver scenario (mirrors driver/scenario.mjs applyScenario)."""
    a, b = loss_t
    head = math.inf if sc["whole"] else sc["head"]
    cached = None
    out = []
    k = 0
    for f in base_frames:
        f = dict(f)
        f["speed_sensor_kmh"] = f["speed_sent_kmh"]  # SPEED = the base's (true) speed, also during a stock resend
        if a <= f["t"] <= b:
            k = f["t"] - a  # every base second has a fix, so the loss index is t - a
            if k < head:
                f.update(form="stock_mode0", accuracy_m=None, satellites_sent=0, speed_sent_kmh=cached["speed_sent_kmh"],
                         bearing_sent_deg=cached["bearing_sent_deg"], location_frozen_at_t=cached["t"])
            else:
                f["form"] = "beta"
                if sc["cap"] is not None and f["accuracy_m"] is not None and f["accuracy_m"] > sc["cap"]:
                    f["accuracy_m"] = sc["cap"]
                if sc["sat"] is not None:
                    f["satellites_sent"] = sc["sat"]
        else:
            f["form"] = "fix"
        if f["t"] < a:
            cached = f
        out.append(f)
    return out


def last_pre_loss_frame(lines, loss_a):
    """(speed km/h, bearing) of the last fix before the loss (the stock resend's cached values), from the file lines."""
    fixes = [x.split() for x in lines if x.startswith("location ")]
    w = fixes[loss_a - 1]
    return dict(t=loss_a - 1, speed_sent_kmh=round(float(w[5]) * 3.6, 1), bearing_sent_deg=float(w[6]))


def generate(out_dir=HERE):
    route = E7.Route(os.path.join(EXP11_DIR, "geometry", "route.json"))
    with open(os.path.join(EXP11_DIR, "run-plan.json")) as f:
        p11 = json.load(f)
    os.makedirs(os.path.join(out_dir, "commands"), exist_ok=True)

    def write(name, lines):
        with open(os.path.join(out_dir, "commands", name + ".txt"), "w") as f:
            f.write("\n".join(lines) + "\n")

    r4_lines, r4_plan = build_r4(route)
    write("R4-r1", r4_lines)
    v4 = read_lines(os.path.join(EXP11_DIR, "commands", "V4-r1.txt"))
    t0, seen = [], 0
    for x in v4:  # the exp11 approach (t = 0..19, 14 m/s, 1 m, satellites 5) as the Tier 0 moving stream
        if x.startswith("location "):
            seen += 1
        if seen == G11.APPROACH_N and x.startswith("gps_satellite "):
            break  # the first tunnel second starts here
        t0.append(x.replace(f"{SHOT}V4-r1-", f"{SHOT}T0-MOVE-") if x.startswith("screenshot ") else x)
    write("T0-MOVE", t0)

    scenarios, runs = {}, {}
    for name, r in RUNS.items():
        if r["base"] == "R4-r1":
            base_path, base_plan, base_lines = "commands/R4-r1.txt", r4_plan, r4_lines
        else:
            base_path = f"{EXP11_REL}/commands/{r['base']}.txt"
            base_plan = p11["runs"][r["base"]]
            base_lines = read_lines(os.path.join(EXP11_DIR, "commands", r["base"] + ".txt"))
        loss = base_plan["tunnel_t"]
        sc = {"base": base_path, "loss_t": loss}
        if r["base"] != name:
            sc["shot_tag"] = [r["base"], name]
        sc.update(satellites_in_loss=r["sat"], accuracy_cap_m=r["cap"], stock_head_s=r["head"], stock_whole=r["whole"],
                  question=r["q"])
        scenarios[name] = sc
        frames = transform_frames(base_plan["frames"], r, loss)
        cached = last_pre_loss_frame(base_lines, loss[0])
        for f in frames:  # the stock frames' cached values come from the last fix before the loss, not the last photo
            if f.get("form") == "stock_mode0":
                f.update(speed_sent_kmh=cached["speed_sent_kmh"], bearing_sent_deg=cached["bearing_sent_deg"],
                         location_frozen_at_t=cached["t"])
            if "screenshot" in f and r["base"] != name:
                f["screenshot"] = f["screenshot"].replace(r["base"] + "-", name + "-", 1)
        runs[name] = dict(tag=name, base=r["base"], base_file=base_path, question=r["q"], loss_t=loss,
                          duration_s=base_plan["duration_s"], speed_checks=base_plan.get("speed_checks", []),
                          satellites_in_loss=r["sat"], accuracy_cap_m=r["cap"], stock_head_s=r["head"],
                          stock_whole=r["whole"], frames=frames)
    sessions = []
    for s in SESSIONS:
        body = sum(runs[t]["duration_s"] for t in s["runs"])
        sessions.append(dict(s, input_s=body, photos=sum(len(runs[t]["frames"]) for t in s["runs"]),
                             estimate_min=round(len(s["runs"]) * RUN_MIN, 1)))
    doc = dict(version=1, screen=SCREEN, mazda=MAZDA, scenarios=scenarios)
    with open(os.path.join(out_dir, "scenarios.json"), "w") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
        f.write("\n")
    plan = dict(livi_env=LIVI_ENV, livi_config=LIVI_CONFIG, mazda=MAZDA, screen=SCREEN, prep_dir=f"{EXP11_REL}/commands/prep",
                tier0=dict(file="commands/T0-MOVE.txt", minutes=list(TIER0_MIN), short_minutes=list(TIER0_SHORT_MIN)),
                budget_min=BUDGET_MIN, run_min=RUN_MIN, sessions=sessions, r4=r4_plan | dict(frames=None), runs=runs)
    with open(os.path.join(out_dir, "rig-run-plan.json"), "w") as f:
        json.dump(plan, f, ensure_ascii=False, indent=1)
        f.write("\n")
    return doc, plan


def main(argv):
    out = argv[1] if len(argv) > 1 else HERE
    doc, plan = generate(out)
    r4 = plan["r4"]
    sp = r4["speed_profile"]
    print(f"R4: {r4['episode_s']} s, rate {r4['heading_error_rate_deg_s']} deg/s, end {r4['episode_end']}, speed "
          f"{sp['min_kmh']}/{sp['median_kmh']}/{sp['max_kmh']} km/h, <20 at {len(sp['below20'])} s "
          f"({sp['below20'][0]}..{sp['below20'][-1]}), wheel {sp['wheel_m']} m, checks {r4['speed_checks']}")
    for name, r in plan["runs"].items():
        print(f"{name}: base {r['base']}, loss {r['loss_t']}, sat {r['satellites_in_loss']}, cap {r['accuracy_cap_m']}, "
              f"stock head {r['stock_head_s']} whole {r['stock_whole']}, {r['duration_s']} s, photos {len(r['frames'])}")
    for s in plan["sessions"]:
        print(f"session {s['session']}: {s['runs']} {s['input_s']} s input, {s['photos']} photos, about {s['estimate_min']} min + Tier 0")


if __name__ == "__main__":
    main(sys.argv)
