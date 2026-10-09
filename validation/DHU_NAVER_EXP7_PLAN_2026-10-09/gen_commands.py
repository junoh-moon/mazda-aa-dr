#!/usr/bin/env python3
"""Generate DHU `location` command files for experiment 7 (wrong reported bearing inside the mapped Baekyang tunnel).

Derived from validation/DHU_NAVER_EXP6_PLAN_2026-10-08/gen_commands.py: same route geometry (copied geometry/route.json),
same setup template, same fast profile (20 approach fixes at 14 m/s ending 20 m inside the entrance, tunnel at 14 m/s,
30 exit fixes; 222 s), same accuracy 40 m inside the tunnel and the same DHU command syntax:
  location <lat> <lon> <accuracy_m> <altitude|NAN> <speed_mps> <bearing_deg|NAN>
The ONLY thing that differs between the conditions is the reported bearing inside the tunnel (and, for H5, a 30 s
standstill in the middle of the tunnel: speed 0, same position, the held bearing). Positions are the true on-route positions.
Usage: gen_commands.py [out_dir [run_plan_json]]
"""
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SHOT = "../../../../experiment7/"
EARTH_R = 6371000.0

APPROACH_FIXES = 20
APPROACH_END_INSIDE_M = 20.0
EXIT_FIXES = 30
SPEED = 14.0  # exp6 "fast" profile: approach, tunnel and exit at 14 m/s
ACC_TUNNEL = 40.0
ACC_GPS = 5.0
REPS = 2
STOP_S = 30  # H5 standstill length
DRIFT_RATE = 0.3  # deg/s for H5 (upper end of 0.15-0.3 deg/s; reaches ~+52 deg at the last tunnel fix, see README)
BURST_S = 4
ENTRANCE_DENSE = (22, 24, 26, 28)  # extra photos in the first 10 s of the tunnel for EVERY run (H7 burst window)

# tag -> parameters. bearing: (kind, value[, extra]) added to the true road bearing inside the tunnel.
RUNS = {
    "H0-correct": dict(bearing=None),
    "H1-const20": dict(bearing=("const", 20.0)),
    "H2-const45": dict(bearing=("const", 45.0)),
    "H3-const90": dict(bearing=("const", 90.0)),
    "H4-const180": dict(bearing=("const", 180.0)),
    "H5-drift-stop": dict(bearing=("drift", DRIFT_RATE), stop=STOP_S),
    "H6-const90-cold": dict(bearing=("const", 90.0), cold=True),
    "H7-burst30": dict(bearing=("burst", 30.0, BURST_S)),
}
# Fixed interleaved order (not blocked): H0 first and last, every condition's r1 before any r2, no condition twice in a row,
# round 2 in a different order than round 1. H6 runs are preceded by the reset procedure of the README.
ORDER = ["H0-correct-r1", "H1-const20-r1", "H4-const180-r1", "H7-burst30-r1", "H3-const90-r1", "H6-const90-cold-r1",
         "H2-const45-r1", "H5-drift-stop-r1",
         "H1-const20-r2", "H6-const90-cold-r2", "H4-const180-r2", "H5-drift-stop-r2", "H2-const45-r2", "H7-burst30-r2",
         "H3-const90-r2", "H0-correct-r2"]


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


def fmt(lat, lon, acc, spd, brg):
    b = "NAN" if brg is None else f"{brg:.1f}"
    a = "NAN" if acc is None else f"{acc:.3f}"
    return f"location {lat:.7f} {lon:.7f} {a} NAN {spd:.3f} {b}"


def bearing_offset(spec, moving_s):
    """Bearing error (deg) after `moving_s` seconds of tunnel MOVEMENT (1 = first tunnel fix; a standstill does not count)."""
    if spec is None:
        return 0.0
    kind = spec[0]
    if kind == "const":
        return spec[1]
    if kind == "drift":
        return spec[1] * (moving_s - 1)  # 0 at the first tunnel fix, then +rate per moving second
    if kind == "burst":
        return spec[1] if moving_s <= spec[2] else 0.0
    raise ValueError(kind)


def build_run(route, tag, params):
    """Return (lines without setup, plan dict). Times t are seconds since the first approach fix."""
    d_start_tun = route.entrance + APPROACH_END_INSIDE_M
    stop_len = params.get("stop", 0)
    d_mid = d_start_tun + (route.exit - d_start_tun) / 2

    # Truth timeline: (t, phase, truth along-route metres, speed, tunnel moving seconds, stopped)
    timeline = []
    for k in range(APPROACH_FIXES):
        timeline.append((k, "approach", d_start_tun - SPEED * (APPROACH_FIXES - 1 - k), SPEED, 0, False))
    d, t, moving, stopped_done = d_start_tun, APPROACH_FIXES - 1, 0, False
    stop_window = None
    while True:
        t += 1
        d += SPEED
        if d >= route.exit:
            break
        moving += 1
        timeline.append((t, "tunnel", d, SPEED, moving, False))
        if stop_len and not stopped_done and d >= d_mid:
            stop_window = [t + 1, t + stop_len]
            for _ in range(stop_len):
                t += 1
                timeline.append((t, "tunnel", d, 0.0, moving, True))
            stopped_done = True
    exit_start = t
    for k in range(EXIT_FIXES):
        if k:
            d += SPEED
        timeline.append((exit_start + k, "exit", d, SPEED, 0, False))
    t_end = timeline[-1][0]
    shots = {x[0] for x in timeline if x[0] % 10 == 0}
    shots |= set(ENTRANCE_DENSE)
    shots |= set(range(exit_start, exit_start + 11, 2))
    shots.add(t_end)
    if stop_window:
        s0, s1 = stop_window
        shots |= {s0, s0 + 2, s1, s1 + 3}  # stop start, settled stop, last stopped second, 2 s after moving again

    lines, frames = [], []
    for t, phase, d, v, mv, stopped in timeline:
        lat, lon, road = route.at(d)
        rec = dict(t=t, phase=phase, truth_from_entrance_m=round(d - route.entrance, 1),
                   truth_to_exit_m=round(route.exit - d, 1), truth_progress_m=round(d - timeline[0][2], 1),
                   speed_mps=round(v, 3), stopped=stopped, road_bearing_deg=round(road, 1))
        if phase == "tunnel":
            off = bearing_offset(params["bearing"], mv)
            sent = (road + off) % 360.0
            line = fmt(lat, lon, ACC_TUNNEL, v, sent)
            rec.update(accuracy_m=ACC_TUNNEL, bearing_offset_deg=round(off, 1), bearing_sent_deg=round(sent, 1))
        else:
            line = fmt(lat, lon, ACC_GPS, v, road)
            rec.update(accuracy_m=ACC_GPS, bearing_offset_deg=0.0, bearing_sent_deg=round(road, 1))
        lines.append(line)
        if t in shots:
            name = f"{tag}-{t:03d}.png"
            lines += ["sleep 0.2", f"screenshot {SHOT}{name}", "sleep 0.8"]
            rec["screenshot"] = name
            frames.append(rec)
        else:
            lines.append("sleep 1")
    tunnel_ts = [x[0] for x in timeline if x[1] == "tunnel"]
    start = route.at(timeline[0][2])
    plan = dict(tag=tag, params={k: (list(v) if isinstance(v, tuple) else v) for k, v in params.items()},
                duration_s=t_end + 1, approach=[0, APPROACH_FIXES - 1], tunnel=[tunnel_ts[0], tunnel_ts[-1]],
                exit=[exit_start, t_end], stop=stop_window,
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
    assert sorted(ORDER) == sorted(plans), "ORDER must list every run exactly once"
    doc = dict(geometry=dict(entrance_m=round(route.entrance, 1), exit_m=round(route.exit, 1),
                             tunnel_length_m=round(route.exit - route.entrance, 1), route_length_m=round(route.length, 1)),
               order=ORDER, runs=plans)
    if plan_path:
        with open(plan_path, "w") as f:
            json.dump(doc, f, ensure_ascii=False, indent=1)
            f.write("\n")
    return route, doc


def main(argv):
    out = argv[1] if len(argv) > 1 else os.path.join(HERE, "commands")
    plan = argv[2] if len(argv) > 2 else os.path.join(HERE, "run-plan.json")
    route, doc = generate(out, plan)
    total = shots = 0
    for tag in doc["order"]:
        p = doc["runs"][tag]
        total += p["duration_s"]
        shots += len(p["frames"]) + 1
        print(f"{tag:22s} {p['duration_s']:4d} s  {len(p['frames']) + 1:3d} photos  tunnel t={p['tunnel'][0]}..{p['tunnel'][1]}"
              f"  exit t={p['exit'][0]}" + (f"  stop t={p['stop'][0]}..{p['stop'][1]}" if p["stop"] else ""))
    print(f"tunnel {doc['geometry']['tunnel_length_m']} m; {len(doc['order'])} runs, {total} s of input "
          f"({total / 60:.1f} min) without setup/restart, {shots} photos incl. setup")


if __name__ == "__main__":
    main(sys.argv)
