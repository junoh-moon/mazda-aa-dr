#!/usr/bin/env python3
"""Generate DHU `location` command files for the BETA NO_FIX speed-overlay experiment.

Same DHU command syntax as validation/DHU_NAVER_EXP4_2026-10-03/commands:
  location <lat> <lon> <accuracy_m> <altitude|NAN> <speed_mps> <bearing_deg|NAN>
Usage: gen_commands.py [out_dir]   (reads ../DHU_NAVER_EXP4_2026-10-03/geometry/non-tunnel-path.json)
Screenshot paths are relative like experiment 4; adapt the prefix to the working directory.
"""
import json, math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "commands")
PATH = os.path.join(HERE, "..", "DHU_NAVER_EXP4_2026-10-03", "geometry", "non-tunnel-path.json")
SHOT = "../../../../experiment5/"
os.makedirs(OUT, exist_ok=True)
pts = json.load(open(PATH))["points"]


def hav(a, b):
    r = 6371000.0
    p1, p2 = math.radians(a[0]), math.radians(b[0])
    dl = math.radians(b[1] - a[1])
    h = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(h))


def bearing(a, b):
    p1, p2 = math.radians(a[0]), math.radians(b[0])
    dl = math.radians(b[1] - a[1])
    y = math.sin(dl) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    return (math.degrees(math.atan2(y, x)) + 360) % 360


def along(dist):
    """(lat, lon, bearing) at `dist` metres along the polyline."""
    acc = 0.0
    for a, b in zip(pts, pts[1:]):
        d = hav(a, b)
        if acc + d >= dist:
            f = (dist - acc) / d if d else 0
            return a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, bearing(a, b)
        acc += d
    return pts[-1][0], pts[-1][1], bearing(pts[-2], pts[-1])


def speed_profile(t):
    """Wheel-speed-like profile (m/s), 60 s period: ramp up, cruise, ramp down, stop."""
    u = t % 60
    if u < 10: return 14.0 * u / 10
    if u < 30: return 14.0
    if u < 40: return 14.0 * (40 - u) / 10
    return 0.0


def fmt(lat, lon, acc, spd, brg):
    b = "NAN" if brg is None else f"{brg:.1f}"
    return f"location {lat:.7f} {lon:.7f} {acc:.3f} NAN {spd:.3f} {b}"


def header():
    return open(os.path.join(HERE, "setup-template.txt")).read().rstrip("\n").splitlines()


def write(name, lines):
    open(os.path.join(OUT, name + ".txt"), "w").write("\n".join(lines) + "\n")


start = pts[0]

# E1 / E2: stale stored fix (fixed coordinates, accuracy 8.8 m) while the speed follows a wheel-like profile; 600 s.
for tag, brg in (("E1-stale-speed-b335", 335.0), ("E2-stale-speed-nobearing", None)):
    lines = header()
    for t in range(600):
        lines.append(fmt(start[0], start[1], 8.8, speed_profile(t), brg))
        if t % 10 == 0:
            lines += ["sleep 0.2", f"screenshot {SHOT}{tag}-{t:03d}.png", "sleep 0.8"]
        else:
            lines.append("sleep 1")
    write(tag, lines)

# E3: stale phase (60 s, speed profile, bearing 335) then the first valid fix 44 m ahead along the route
# (accuracy 3 m, speed 0 like the vehicle), 20 s standing, then normal moving fixes (accuracy 5 m, 14 m/s) for 60 s.
lines = header()
for t in range(60):
    lines.append(fmt(start[0], start[1], 8.8, speed_profile(t), 335.0))
    lines += ["sleep 0.2", f"screenshot {SHOT}E3-stale-{t:03d}.png", "sleep 0.8"] if t % 5 == 0 else ["sleep 1"]
la, lo, br = along(44.0)
for t in range(20):
    lines.append(fmt(la, lo, 3.0, 0.0, None))
    lines += ["sleep 0.2", f"screenshot {SHOT}E3-first-fix-{t:03d}.png", "sleep 0.8"] if t % 2 == 0 else ["sleep 1"]
d = 44.0
for t in range(60):
    d += 14.0
    la, lo, br = along(d)
    lines.append(fmt(la, lo, 5.0, 14.0, br))
    lines += ["sleep 0.2", f"screenshot {SHOT}E3-moving-{t:03d}.png", "sleep 0.8"] if t % 5 == 0 else ["sleep 1"]
write("E3-stale-to-first-fix", lines)

# E5: control = stock behaviour (stale fix, accuracy 8.8 m, constant 1.111 m/s = 4 km/h, bearing 335), 120 s.
lines = header()
for t in range(120):
    lines.append(fmt(start[0], start[1], 8.8, 1.111, 335.0))
    lines += ["sleep 0.2", f"screenshot {SHOT}E5-stock-{t:03d}.png", "sleep 0.8"] if t % 10 == 0 else ["sleep 1"]
write("E5-control-stock", lines)

# E6: GPS-lost style (LOST) for comparison: 20 s moving fixes with accuracy growing 10->38 m, then the Mazda mode-0 form
# (last coordinates, accuracy NAN, speed 2.2 m/s, bearing kept) for 60 s, then valid moving fixes again.
lines = header()
d = 0.0
for t in range(20):
    d += 14.0
    la, lo, br = along(d)
    lines.append(fmt(la, lo, 10.0 + 28.0 * t / 19, 14.0, br))
    lines += ["sleep 0.2", f"screenshot {SHOT}E6-bridge-{t:03d}.png", "sleep 0.8"] if t % 5 == 0 else ["sleep 1"]
for t in range(60):
    lines.append(fmt(la, lo, float("nan"), 2.2, br).replace("nan", "NAN"))
    lines += ["sleep 0.2", f"screenshot {SHOT}E6-lost-{t:03d}.png", "sleep 0.8"] if t % 5 == 0 else ["sleep 1"]
for t in range(30):
    d += 14.0
    la, lo, br = along(d)
    lines.append(fmt(la, lo, 5.0, 14.0, br))
    lines += ["sleep 0.2", f"screenshot {SHOT}E6-return-{t:03d}.png", "sleep 0.8"] if t % 5 == 0 else ["sleep 1"]
write("E6-lost-form", lines)
print("wrote", sorted(os.listdir(OUT)))
