#!/usr/bin/env python3
"""Generate DHU `location` command files for experiment 6 (map matching inside the mapped Baekyang tunnel).

Same DHU command syntax as validation/DHU_NAVER_EXP3_2026-10-03/commands (tunnel runs C1-C5):
  location <lat> <lon> <accuracy_m> <altitude|NAN> <speed_mps> <bearing_deg|NAN>
Usage: gen_commands.py [out_dir [run_plan_json]]
Reads geometry/route.json (OSM ways 59038272 -> 59038277 (tunnel, two nodes, interpolated straight) -> 59038275,
164684929, 1313803513). Writes one command file per run and repetition plus run-plan.json (truth per screenshot).
Screenshot paths are relative like experiments 3-5; adapt the prefix to the working directory.
"""
import json
import math
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SHOT = "../../../../experiment6/"
EARTH_R = 6371000.0
KMH = 1 / 3.6

# Approach: 20 fixes (t = 0..19) at accuracy 5 m whose last fix lies 20 m inside the OSM tunnel entrance, exactly like
# experiment 3 C1-C5 (their approach_start is 246 m before the entrance), so T0 is directly comparable with C2.
APPROACH_FIXES = 20
APPROACH_END_INSIDE_M = 20.0
EXIT_FIXES = 30
RAMP_S = 10  # constant offsets ramp in linearly over the first 10 s of the tunnel phase (no backwards jump)
REPS = 2
ORDER_SEED = 20261008

PROFILES = {
    # (approach speed, tunnel speed as a function of seconds since the tunnel phase started, exit speed)
    "fast": (14.0, lambda u: 14.0, 14.0),
    # Owner's drive: entered at 17 km/h and averaged ~30 km/h inside. 17 km/h for the first 20 s, then 30 km/h.
    "slow": (17 * KMH, lambda u: 17 * KMH if u <= 20 else 30 * KMH, 30 * KMH),
}

# tag -> parameters. cross: perpendicular metres to the RIGHT of travel; along: metres ahead (+) / behind (-).
RUNS = {
    "T0-control-noinput": dict(profile="fast", inject=False),
    "T1-ideal-a40": dict(profile="fast", acc=40.0),
    "T2-cross50-a40": dict(profile="fast", acc=40.0, cross=("ramp", 50.0)),
    "T3-cross150-a40": dict(profile="fast", acc=40.0, cross=("ramp", 150.0)),
    "T4-crossgrow300-a40": dict(profile="fast", acc=40.0, cross=("grow", 300.0)),
    "T5-along-plus100-a40": dict(profile="fast", acc=40.0, along=("ramp", 100.0)),
    "T6-along-minus100-a40": dict(profile="fast", acc=40.0, along=("ramp", -100.0)),
    "T7-ideal-a100": dict(profile="fast", acc=100.0),
    "T8-realspeed-a40": dict(profile="slow", acc=40.0),
    "T8C-control-noinput-slow": dict(profile="slow", inject=False),
    "T9-ideal-nobearing-a40": dict(profile="fast", acc=40.0, nobearing=True),
}
# Round 1: controls first, then the short (fast-profile) runs, the long slow-profile pair last.
ROUND1 = ["T0-control-noinput", "T1-ideal-a40", "T7-ideal-a100", "T2-cross50-a40", "T3-cross150-a40",
          "T4-crossgrow300-a40", "T5-along-plus100-a40", "T6-along-minus100-a40", "T9-ideal-nobearing-a40",
          "T8C-control-noinput-slow", "T8-realspeed-a40"]


def hav(a, b):
    p1, p2 = math.radians(a[0]), math.radians(b[0])
    dl = math.radians(b[1] - a[1])
    h = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * EARTH_R * math.asin(math.sqrt(h))


def bearing(a, b):
    p1, p2 = math.radians(a[0]), math.radians(b[0])
    dl = math.radians(b[1] - a[1])
    y = math.sin(dl) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    return (math.degrees(math.atan2(y, x)) + 360) % 360


class Route:
    def __init__(self, path):
        with open(path) as f:
            doc = json.load(f)
        pts, entrance, exit_ = [], None, None
        for way in doc["ways"]:
            p = [tuple(x) for x in way["points"]]
            if pts and pts[-1] == p[0]:
                p = p[1:]
            if way["way"] == doc["tunnel_way"]:
                entrance, exit_ = len(pts) - 1, len(pts) + len(p) - 1
            pts.extend(p)
        self.pts = pts
        self.cum = [0.0]
        for a, b in zip(pts, pts[1:]):
            self.cum.append(self.cum[-1] + hav(a, b))
        self.entrance = self.cum[entrance]
        self.exit = self.cum[exit_]
        self.length = self.cum[-1]

    def at(self, d):
        """(lat, lon, segment bearing) at `d` metres along the route; linear in lat/lon within a segment."""
        if not 0.0 <= d <= self.length:
            raise ValueError(f"{d:.1f} m is outside the mapped route (0..{self.length:.1f} m)")
        for i in range(len(self.pts) - 1):
            if self.cum[i + 1] >= d:
                a, b = self.pts[i], self.pts[i + 1]
                f = (d - self.cum[i]) / (self.cum[i + 1] - self.cum[i])
                return a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, bearing(a, b)
        raise AssertionError("unreachable")


def shift(lat, lon, brg, cross_m):
    """Move `cross_m` metres perpendicular to the RIGHT of bearing `brg`, using the local metric scale."""
    th = math.radians(brg + 90.0)
    dn, de = cross_m * math.cos(th), cross_m * math.sin(th)
    return (lat + math.degrees(dn / EARTH_R),
            lon + math.degrees(de / (EARTH_R * math.cos(math.radians(lat)))))


def fmt(lat, lon, acc, spd, brg):
    b = "NAN" if brg is None else f"{brg:.1f}"
    return f"location {lat:.7f} {lon:.7f} {acc:.3f} NAN {spd:.3f} {b}"


def offset_value(spec, u, frac):
    if spec is None:
        return 0.0
    kind, value = spec
    if kind == "ramp":
        return value * min(1.0, u / RAMP_S)
    if kind == "grow":
        return value * frac
    raise ValueError(kind)


def build_run(route, tag, params):
    """Return (lines without setup, plan dict). Times t are seconds since the first approach fix."""
    v_app, v_tun, v_exit = PROFILES[params["profile"]]
    d_start_tun = route.entrance + APPROACH_END_INSIDE_M
    lines, frames = [], []
    shots = set()

    # Truth timeline: (t, phase, truth along-route metres, speed)
    timeline = []
    for k in range(APPROACH_FIXES):
        timeline.append((k, "approach", d_start_tun - v_app * (APPROACH_FIXES - 1 - k), v_app))
    d, t = d_start_tun, APPROACH_FIXES - 1
    while True:
        t += 1
        v = v_tun(t - (APPROACH_FIXES - 1))
        d += v
        if d >= route.exit:
            break
        timeline.append((t, "tunnel", d, v))
    exit_start = t
    for k in range(EXIT_FIXES):
        if k:
            d += v_exit
        timeline.append((exit_start + k, "exit", d, v_exit))
    t_end = timeline[-1][0]
    shots = {t for t, *_ in timeline if t % 10 == 0}
    shots |= set(range(exit_start, exit_start + 11, 2))
    shots.add(t_end)

    tunnel_len = route.exit - d_start_tun
    for t, phase, d, v in timeline:
        rec = dict(t=t, phase=phase, truth_from_entrance_m=round(d - route.entrance, 1),
                   truth_to_exit_m=round(route.exit - d, 1),
                   truth_progress_m=round(d - timeline[0][2], 1))
        if phase == "tunnel":
            if not params.get("inject", True):
                line = None
                rec.update(injected=False)
            else:
                u = t - (APPROACH_FIXES - 1)
                frac = (d - d_start_tun) / tunnel_len
                along = offset_value(params.get("along"), u, frac)
                cross = offset_value(params.get("cross"), u, frac)
                lat, lon, brg = route.at(d + along)
                lat, lon = shift(lat, lon, brg, cross)
                line = fmt(lat, lon, params["acc"], v, None if params.get("nobearing") else brg)
                rec.update(injected=True, accuracy_m=params["acc"], along_error_m=round(along, 1),
                           cross_track_m=round(cross, 1))
        else:
            lat, lon, brg = route.at(d)
            line = fmt(lat, lon, 5.0, v, brg)
            rec.update(injected=True, accuracy_m=5.0, along_error_m=0.0, cross_track_m=0.0)
        if line:
            lines.append(line)
        if t in shots:
            name = f"{tag}-{t:03d}.png"
            lines += ["sleep 0.2", f"screenshot {SHOT}{name}", "sleep 0.8"]
            rec["screenshot"] = name
            frames.append(rec)
        else:
            lines.append("sleep 1")
    tunnel_ts = [t for t, ph, *_ in timeline if ph == "tunnel"]
    start = route.at(timeline[0][2])
    plan = dict(tag=tag, params={k: v for k, v in params.items()}, duration_s=t_end + 1,
                approach=[0, APPROACH_FIXES - 1], tunnel=[tunnel_ts[0], tunnel_ts[-1]], exit=[exit_start, t_end],
                start=[round(start[0], 7), round(start[1], 7), round(start[2], 1)], frames=frames)
    return lines, plan


def setup_lines(template, tag, start):
    text = template.replace("@START@", f"{start[0]:.7f} {start[1]:.7f}").replace("@BRG@", f"{start[2]:.1f}")
    return text.replace("@SHOT@", SHOT).replace("@TAG@", tag).rstrip("\n").splitlines()


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
    round2 = list(RUNS)
    random.Random(ORDER_SEED).shuffle(round2)
    order = [f"{b}-r1" for b in ROUND1] + [f"{b}-r2" for b in round2]
    doc = dict(geometry=dict(entrance_m=round(route.entrance, 1), exit_m=round(route.exit, 1),
                             tunnel_length_m=round(route.exit - route.entrance, 1), route_length_m=round(route.length, 1)),
               order=order, round2_seed=ORDER_SEED, runs=plans)
    if plan_path:
        with open(plan_path, "w") as f:
            json.dump(doc, f, ensure_ascii=False, indent=1)
            f.write("\n")
    return route, doc


def main(argv):
    out = argv[1] if len(argv) > 1 else os.path.join(HERE, "commands")
    plan = argv[2] if len(argv) > 2 else os.path.join(HERE, "run-plan.json")
    route, doc = generate(out, plan)
    total = 0
    for tag in doc["order"]:
        p = doc["runs"][tag]
        total += p["duration_s"]
        print(f"{tag:32s} {p['duration_s']:4d} s  tunnel t={p['tunnel'][0]}..{p['tunnel'][1]}  exit t={p['exit'][0]}")
    print(f"tunnel {doc['geometry']['tunnel_length_m']} m; {len(doc['order'])} runs, {total} s of input "
          f"({total / 60:.1f} min) without setup/restart")


if __name__ == "__main__":
    main(sys.argv)
