#!/usr/bin/env python3
"""Generate DHU command files for experiment 13 (tunnel fork guidance test, H-FORK).

Question (owner). In a long tunnel with a fork, while the GPS is lost and BETA sends dead-reckoned fixes whose lateral error pushes the
position toward the WRONG branch, does Naver still announce and follow the intended branch, and does the reported accuracy (product
40 m cap vs the planned 25 m ceiling) change whether Naver map-matches the car onto the wrong branch?
Static reading (validation/NAVER_MAP_APP_STATIC_RE_2026-10-12.md, not executed): a car fix is valid only with accuracy < 30 m and a
route distance < 1.5 x road width; a valid fix is map-matched; an invalid one leaves Naver on its route-following virtual drive.
Prediction: 40 m -> route-driven, the branch is kept whatever the offset; 25 m -> our coordinate is used, so an offset beyond half the
branch separation may match the wrong branch (bearings of the two branches differ by only ~10 deg, so the 75 deg bearing gate does not help).

Why a new route. The proven Busan route (experiments 9-12, 백양터널) has NO fork inside the tunnel: its tunnel way has no branch and the
next diverge (a right-hand secondary_link) is 463 m after the exit, 6-12 m from the main line (OSM, computed in the plan). The owner's
candidate 서부간선지하도로 has no branch inside its 9.0 km tunnel either in OSM (its split is 217 m after the tagged north portal, the two
lines 7-10 m apart for 190 m). Width rule: lanes x 3.3/2 + 8 m, valid < 1.5 x (17.0 / 21.9 m for 2 / 4 lanes), off-route > 2.3 x (26.0 / 33.6 m).
The OSM-confirmed in-tunnel fork used here: 신월여의지하도로 eastbound, 6.6 km after the west portal; branch R
(trunk_link, tunnel, right) -> 올림픽대로 east, branch M (main, tunnel, straight) -> 여의도. geometry/route-seoul.json (ODbL).

Single variable per comparison. Every run uses the same Seoul body: origin ORIGIN_BEFORE_FORK_M before the fork on the tunnel line,
constant 20 m/s, true road bearing on every fix, 20 approach fixes at 1 m, a GPS-loss episode of EPISODE_S seconds (fork at tau FORK_TAU),
20 recovery fixes at 1 m on the intended branch R. Between runs only (a) the episode accuracy (25 or 40 m) and (b) the lateral offset
target X (0, 15, 28 m, to the LEFT = toward branch M; linear growth 0 -> X until the fork, then held) change; the tag changes the photo names.
The preparation for Seoul is the proven experiment 10 v5 B2 step files with only the location line replaced by the Seoul origin; the
destination search itself is typed on the DHU on-screen keyboard (experiments 6/7 precedent, recorded with photos). The Busan files
(prep byte copies, T0-E13B = experiment 12 T0-E12 retagged, V4REF-E13 = experiment 12 V4REF-r1 retagged) are the environment fallback
and the optional same-session Busan control.
Usage: gen_commands.py [out_dir [run_plan_json]]
"""
import hashlib
import importlib.util
import json
import math
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VAL = os.path.normpath(os.path.join(HERE, ".."))
EXP7_DIR = os.path.join(VAL, "DHU_NAVER_EXP7_PLAN_2026-10-09")
EXP12_DIR = os.path.join(VAL, "DHU_NAVER_EXP12_PLAN_2026-10-12")
GEOMETRY = os.path.join(HERE, "geometry", "route-seoul.json")


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


E7 = _load("dhu_exp7_for_exp13", os.path.join(EXP7_DIR, "gen_commands.py"))
hav, bearing, fmt, offset_position, unit = E7.hav, E7.bearing, E7.fmt, E7.offset_position, E7.unit

SHOT = "../../../../experiment11/"
PREP_SHOT = "../../../../experiment10/"
BUSAN_ORIGIN_LINE = "location 35.1697679 129.0267566 1 NAN 0 328.9"  # the B2 preparation fix (experiment 10 v5)
PREP_FILES = ("B2-search", "B2-route-y130", "B2-route-y225", "B2-route-y318", "B2-route-retry", "B2-gate", "B2-gate-nopopup",
              "search-again", "end-guidance")
SEOUL_PREP = {  # Seoul step file -> B2 source (only the B2 location line is replaced)
    "S-search": "B2-search", "S-route-y130": "B2-route-y130", "S-route-y225": "B2-route-y225", "S-route-y318": "B2-route-y318",
    "S-route-retry": "B2-route-retry", "S-gate": "B2-gate", "S-gate-nopopup": "B2-gate-nopopup",
}
EXP12_T0 = "T0-E12"
BUSAN_T0_TAG = "T0-E13B"
BUSAN_REF_SRC, BUSAN_REF_TAG = "V4REF-r1", "V4REF-E13"
SEOUL_T0_TAGS = ("T0-S13", "T0-S13S2")  # session 1, session 2 (no photo overwrite)

V = 20.0  # m/s (72 km/h, under the 80 km/h limit of the tunnel way)
ORIGIN_BEFORE_FORK_M = 1500.0
APPROACH_N = 20
EPISODE_S = 70
FORK_TAU = 55  # (ORIGIN_BEFORE_FORK_M - V * APPROACH_N) / V
REC_S = 20
ACC_GPS = 1.0
EPISODE_PHOTOS = set(range(0, EPISODE_S, 5)) | set(range(48, 63)) | {EPISODE_S - 1}
APPROACH_PHOTOS = {0, 5, 10, 15, 19}
REC_PHOTOS = {0, 1, 2, 3, 5, 10, REC_S - 1}
T0_STREAM_S = 20
T0_PHOTOS = {10, 20}

# tag -> (episode accuracy m, lateral offset target X m toward branch M (left), session, question)
RUNS = {
    "R25X15-r1": (25.0, 15.0, 1, "planned 25 m ceiling, offset past half the branch separation: wrong-branch match?"),
    "R25X0-r1": (25.0, 0.0, 1, "same-session control: 25 m, no offset (does the DHU show the fork guidance and keep branch R?)"),
    "R40X15-r1": (40.0, 15.0, 1, "current 40 m cap, same offset: route-driven virtual drive keeps branch R?"),
    "R25X28-r1": (25.0, 28.0, 2, "25 m, offset beyond 1.5 x width (17.0 m 2-lane, 21.9 m 4-lane) and 2.3 x width of a 2-lane link (26.0 m): invalid by route distance?"),
    "R40X28-r1": (40.0, 28.0, 2, "40 m, offset 28 m"),
    "R25X15-r2": (25.0, 15.0, 2, "repeat of the key run in a second session"),
    "R40X0-r1": (40.0, 0.0, 2, "40 m control (only if time allows)"),
}
SESSIONS = {1: ["R25X15-r1", "R25X0-r1", "R40X15-r1"], 2: ["R25X28-r1", "R40X28-r1", "R25X15-r2", "R40X0-r1"]}
REQUIRED = {1: ["R25X15-r1", "R25X0-r1"], 2: ["R25X28-r1", "R40X28-r1"]}
BUDGET_MIN = 25
ENV_MIN = 2.0
TIER0_MIN = {1: 7.0, 2: 8.0}  # session 1: keyboard search + gate + 20 s stream; session 2: recents + M-route discovery (3 min)
RUN_MIN = 4.5  # experiment 11/12: 4.6-5.3 min start-to-start for 151 s bodies; these bodies are 110 s
ROAD = dict(lane_m=3.3, base_m=8.0, valid_factor=1.5, off_route_factor=2.3)
DESTINATIONS = dict(
    R=["노량진수산시장", "노량진역", "반포한강공원"],  # expected to take branch R (올림픽대로 east); Tier 0 confirms
    M=["여의도공원", "국회의사당", "여의도한강공원"],  # expected to take branch M (여의도); session 2 discovery only
)


class Route:
    """Polyline of one route in route-seoul.json (same interpolation as experiment 7 Route.at)."""

    def __init__(self, doc, key):
        pts = []
        for way in doc["routes"][key]["ways"]:
            p = [tuple(x) for x in way["points"]]
            if pts and pts[-1] == p[0]:
                p = p[1:]
            pts.extend(p)
        self.pts = pts
        self.cum = [0.0]
        for a, b in zip(pts, pts[1:]):
            self.cum.append(self.cum[-1] + hav(a, b))
        self.fork = self.cum[pts.index(tuple(doc["fork"]))]
        self.length = self.cum[-1]
        self.ways = [w["way"] for w in doc["routes"][key]["ways"]]

    def at(self, d):
        if not 0.0 <= d <= self.length:
            raise ValueError(f"{d:.1f} m is outside the mapped route (0..{self.length:.1f} m)")
        for i in range(len(self.pts) - 1):
            if self.cum[i + 1] >= d:
                a, b = self.pts[i], self.pts[i + 1]
                f = (d - self.cum[i]) / (self.cum[i + 1] - self.cum[i])
                return a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, bearing(a, b)
        raise AssertionError("unreachable")

    def distance_to(self, p, d_from):
        """Shortest horizontal distance (m) from p to the polyline after route distance d_from (local plane)."""
        best = float("inf")
        lat0 = math.radians(p[0])
        kx, ky = 111320.0 * math.cos(lat0), 110574.0
        px, py = 0.0, 0.0
        for i in range(len(self.pts) - 1):
            if self.cum[i + 1] < d_from:
                continue
            a, b = self.pts[i], self.pts[i + 1]
            ax, ay = (a[1] - p[1]) * kx, (a[0] - p[0]) * ky
            bx, by = (b[1] - p[1]) * kx, (b[0] - p[0]) * ky
            dx, dy = bx - ax, by - ay
            t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / ((dx * dx + dy * dy) or 1.0)))
            best = min(best, math.hypot(ax + t * dx, ay + t * dy))
        return best


def load_routes():
    with open(GEOMETRY) as f:
        doc = json.load(f)
    return doc, Route(doc, "R"), Route(doc, "M")


def offset_m(x, tau):
    return x * min(1.0, tau / FORK_TAU)


def left_offset(road, x):
    """(east, north) of x metres to the LEFT of a bearing."""
    ue, un = unit(road)
    return -un * x, ue * x


def origin_line(r):
    lat, lon, road = r.at(r.fork - ORIGIN_BEFORE_FORK_M)
    return f"location {lat:.7f} {lon:.7f} 1 NAN 0 {road:.1f}"


def build_run(r, m, tag, acc, x):
    lines, frames = [], []
    d0 = r.fork - ORIGIN_BEFORE_FORK_M

    def emit(t, line, rec, photo):
        lines.append(line)
        rec["t"] = t
        if photo:
            name = f"{tag}-{t:03d}.png"
            lines.extend(["sleep 0.2", f"screenshot {SHOT}{name}", "sleep 0.8"])
            rec["screenshot"] = name
            frames.append(rec)
        else:
            lines.append("sleep 1")

    for t in range(APPROACH_N + EPISODE_S + REC_S):
        d = d0 + V * t
        lat, lon, road = r.at(d)
        to_fork = round(d - r.fork, 1)
        if APPROACH_N <= t < APPROACH_N + EPISODE_S:
            tau = t - APPROACH_N
            off = offset_m(x, tau)
            slat, slon = offset_position(lat, lon, left_offset(road, off))
            rec = dict(phase="episode", tau=tau, accuracy_m=acc, offset_left_m=round(off, 1), from_fork_m=to_fork,
                       speed_sent_kmh=round(V * 3.6, 1), bearing_sent_deg=round(road, 1))
            if d > r.fork:
                rec.update(dist_to_R_m=round(r.distance_to((slat, slon), r.fork), 1),
                           dist_to_M_m=round(m.distance_to((slat, slon), m.fork), 1),
                           branch_separation_m=round(m.distance_to((lat, lon), m.fork), 1))
                rec["closer_to"] = "M" if rec["dist_to_M_m"] < rec["dist_to_R_m"] else "R"
            emit(t, fmt(slat, slon, acc, V, road), rec, tau in EPISODE_PHOTOS)
        else:
            phase, k = ("approach", t) if t < APPROACH_N else ("recovery", t - APPROACH_N - EPISODE_S)
            rec = dict(phase=phase, accuracy_m=ACC_GPS, offset_left_m=0.0, from_fork_m=to_fork, speed_sent_kmh=round(V * 3.6, 1),
                       bearing_sent_deg=round(road, 1))
            if phase == "recovery":
                rec["rho"] = k
            emit(t, fmt(lat, lon, ACC_GPS, V, road), rec, k in (APPROACH_PHOTOS if phase == "approach" else REC_PHOTOS))
    return lines, frames


def read_lines(path):
    with open(path) as f:
        return f.read().split("\n")[:-1]


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def seoul_t0(r, tag):
    """Experiment 12 T0-E12 (20 s stationary stream) with the Busan fix replaced by the Seoul origin and the tag renamed."""
    lat, lon, road = r.at(r.fork - ORIGIN_BEFORE_FORK_M)
    out = []
    for x in read_lines(os.path.join(EXP12_DIR, "commands", EXP12_T0 + ".txt")):
        if x.startswith("location "):
            x = f"location {lat:.7f} {lon:.7f} 1.000 NAN 0.000 {road:.1f}"
        elif x.startswith("screenshot "):
            x = x.replace(f"{SHOT}{EXP12_T0}-", f"{SHOT}{tag}-")
        out.append(x)
    return out


def generate(out_dir, plan_path=None):
    doc, r, m = load_routes()
    os.makedirs(os.path.join(out_dir, "prep"), exist_ok=True)
    os.makedirs(os.path.join(out_dir, "prep-busan"), exist_ok=True)

    def write(name, lines):
        with open(os.path.join(out_dir, name + ".txt"), "w") as f:
            f.write("\n".join(lines) + "\n")

    src_prep = os.path.join(EXP12_DIR, "commands", "prep")
    for name in PREP_FILES:  # Busan fallback: byte copies (experiment 12 = experiment 11 = experiment 10 v5)
        shutil.copyfile(os.path.join(src_prep, name + ".txt"), os.path.join(out_dir, "prep-busan", name + ".txt"))
    o_line = origin_line(r)
    for new, old in SEOUL_PREP.items():
        src = read_lines(os.path.join(src_prep, old + ".txt"))
        assert BUSAN_ORIGIN_LINE in src, old
        write(os.path.join("prep", new), [o_line if x == BUSAN_ORIGIN_LINE else x for x in src])
    for name in ("search-again", "end-guidance"):  # no location line: byte copies
        shutil.copyfile(os.path.join(src_prep, name + ".txt"), os.path.join(out_dir, "prep", name + ".txt"))
    # keyboard path: after the destination row of the typed search is tapped, the route-start tap and the origin fixes
    start = read_lines(os.path.join(src_prep, "B2-route-y130.txt"))[2:]
    write(os.path.join("prep", "S-start"), [o_line if x == BUSAN_ORIGIN_LINE else x for x in start])

    for tag in SEOUL_T0_TAGS:
        write(tag, seoul_t0(r, tag))
    t0b = read_lines(os.path.join(EXP12_DIR, "commands", EXP12_T0 + ".txt"))
    write(BUSAN_T0_TAG, [x.replace(f"{SHOT}{EXP12_T0}-", f"{SHOT}{BUSAN_T0_TAG}-") for x in t0b])
    ref = read_lines(os.path.join(EXP12_DIR, "commands", BUSAN_REF_SRC + ".txt"))
    write(BUSAN_REF_TAG, [x.replace(f"{SHOT}{BUSAN_REF_SRC}-", f"{SHOT}{BUSAN_REF_TAG}-") for x in ref])

    runs = {}
    for tag, (acc, x, session, q) in RUNS.items():
        lines, frames = build_run(r, m, tag, acc, x)
        write(tag, lines)
        after = [f for f in frames if f["phase"] == "episode" and f["from_fork_m"] > 0]
        runs[tag] = dict(tag=tag, condition=tag.rsplit("-r", 1)[0], session=session, question=q, accuracy_episode_m=acc,
                         offset_target_left_m=x, intended_branch="R", wrong_branch="M (left)",
                         duration_s=APPROACH_N + EPISODE_S + REC_S, episode_t=[APPROACH_N, APPROACH_N + EPISODE_S - 1],
                         fork_t=APPROACH_N + FORK_TAU, recovery_t=[APPROACH_N + EPISODE_S, APPROACH_N + EPISODE_S + REC_S - 1],
                         closer_to_M_photos=[f["t"] for f in after if f.get("closer_to") == "M"], frames=frames)
    sep = {}
    for k in (25, 50, 80, 120, 150, 200, 280):
        lat, lon, _ = r.at(r.fork + k)
        sep[str(k)] = round(m.distance_to((lat, lon), m.fork), 1)
    width = {n: n * ROAD["lane_m"] / 2 + ROAD["base_m"] for n in (2, 4)}
    est = {}
    for s, order in SESSIONS.items():
        for i, tag in enumerate(order):
            est[tag] = ENV_MIN + TIER0_MIN[s] + RUN_MIN * (i + 1)
    lat0, lon0, road0 = r.at(r.fork - ORIGIN_BEFORE_FORK_M)
    plan = dict(
        question="H-FORK: at an in-tunnel fork, does a lateral DR error toward the wrong branch make Naver match the wrong branch, "
                 "and does the reported accuracy (25 m vs 40 m) change it",
        geometry=dict(file="geometry/route-seoul.json", fork=doc["fork"], route_R_ways=r.ways, route_M_ways=m.ways,
                      fork_from_west_portal_m=round(r.fork - r.cum[r.pts.index(tuple(doc["routes"]["R"]["ways"][2]["points"][0]))], 1),
                      branch_separation_m_by_metres_after_fork=sep, side_of_wrong_branch="left (M is the straight continuation)",
                      road_width_m=width, valid_route_distance_m={n: round(w * ROAD["valid_factor"], 1) for n, w in width.items()},
                      off_route_m={n: round(w * ROAD["off_route_factor"], 1) for n, w in width.items()}),
        site=dict(origin=[round(lat0, 7), round(lon0, 7)], origin_bearing_deg=round(road0, 1), origin_before_fork_m=ORIGIN_BEFORE_FORK_M,
                  speed_mps=V, destinations=DESTINATIONS),
        timeline=dict(approach_t=[0, APPROACH_N - 1], episode_s=EPISODE_S, fork_tau=FORK_TAU, recovery_s=REC_S,
                      offset="linear 0 -> X over tau 0..fork_tau, then X; perpendicular to the intended road, to the left",
                      bearing="true road bearing of the intended route at the truth point", accuracy_gps_m=ACC_GPS),
        prep=dict(seoul={k: f"exp12 commands/prep/{v}.txt with the B2 location line replaced by the Seoul origin" for k, v in SEOUL_PREP.items()},
                  seoul_start="exp12 B2-route-y130.txt without its first two lines (row tap, sleep), origin replaced",
                  busan="prep-busan/: byte copies of exp12 commands/prep",
                  placeholder="@TAG@ in screenshot lines only"),
        tier0=dict(seoul=[f"{t}.txt (exp12 {EXP12_T0} with the Seoul origin)" for t in SEOUL_T0_TAGS],
                   busan_fallback=f"{BUSAN_T0_TAG}.txt (exp12 {EXP12_T0} retagged)", busan_control=f"{BUSAN_REF_TAG}.txt (exp12 {BUSAN_REF_SRC} retagged)",
                   minutes=TIER0_MIN),
        sessions=SESSIONS, required=REQUIRED, budget_min=BUDGET_MIN, env_min=ENV_MIN, run_min=RUN_MIN, cumulative_estimate_min=est,
        runs=runs)
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
    print(f"fork {g['fork_from_west_portal_m']} m after the west portal; separation {g['branch_separation_m_by_metres_after_fork']}")
    for tag, r in p["runs"].items():
        print(f"{tag}: session {r['session']}, acc {r['accuracy_episode_m']}, X {r['offset_target_left_m']}, photos {len(r['frames'])}, "
              f"closer to M at t {r['closer_to_M_photos']}, estimate {p['cumulative_estimate_min'][tag]} min")


if __name__ == "__main__":
    main(sys.argv)
