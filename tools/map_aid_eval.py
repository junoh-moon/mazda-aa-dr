#!/usr/bin/env python3
"""Map-aided tunnel counterfactual evaluator (host only, record only).

For every GPS-lost episode (a run of POSITION mode 0 that ends with a fix)
in exported drive logs, match the last pre-loss fix to an OSM tunnel chain
(tools/osm_tunnels.py database) and compute what a map-constrained dead
reckoning WOULD have sent at the first returned fix, next to what the product
actually sent. Nothing here changes the product; it accumulates evidence for
the decision in validation/MAP_AID_EVALUATOR_2026-10-14.md.

Method (validation/MAP_AIDED_TUNNEL_HEADING_2026-10-14.md and
validation/MAP_AIDED_TUNNEL_HEADING_FABLE_2026-10-14.md):
  match   unique directed chain: the chain extended 60 m back from its start,
          lateral distance <= 15 m, along position s in [-60, min(150, L-100)]
          m, |course - tangent| <= 20 deg, pre-loss fix >= 10 km/h and at most
          2 s before the loss; zero or several candidates: no constraint.
  advance s(t) = s0 + wheel distance since the pre-loss fix (mean of four
          wheels, gaps filled from replacement-send speed, log_digest mean
          speed or fix speed; uncovered time is reported).
  TAN     heading = chain tangent at s, integrated from the pre-loss fix
          (keeps the pre-loss lateral offset).
  REL     heading = seed + (tangent(s) - tangent(s0)), integrated; seed = the
          bearing of the first replacement LOCATION of the episode (the
          product's own heading, Fable study), else the pre-loss fix course.
  SNAP    position = chain point at s (the offset is dropped).
  Past the chain end every variant continues straight on the last tangent
  (a fork end is flagged; the branch is not chosen).
  current the product's last replacement LOCATION extrapolated to the fix time
          with its own speed and bearing (and the beta.14 exit_jump row when
          present, unextrapolated).
Jumps are |first returned fix - estimate| in metres, split along/cross the
fix course (cross > 0: the fix lies right of the estimate). The GPS fix is a
reference, not ground truth.

False-match scan (Opus method): every GPS-valid fix >= 10 km/h is treated as
a synthetic loss; a unique match is a false match when the following fixes
(40 s, up to min(L, 300) m along) leave the chain by more than 20 m without
any mode-0 sample.

Privacy: the default text and JSON outputs carry no coordinates, tunnel
names or OSM ids; episodes are named <boot prefix>@<return time s>. --private
prints those details to the terminal only.
"""
import argparse
import bisect
import io
import json
import math
import os
import re
import struct
import sys
import tarfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import osm_tunnels  # noqa: E402

# Matching gates (validation/MAP_AIDED_TUNNEL_HEADING_2026-10-14.md section 2).
GATE_LATERAL_M = 15.0
GATE_S_MIN_M = -60.0
GATE_S_MAX_M = 150.0
GATE_END_MARGIN_M = 100.0
GATE_HEADING_DEG = 20.0
# Entry speed: the Opus design proposed >= 10 km/h, but its own evaluation
# matched the 60 s tunnel entered at 7 km/h in traffic; the recorded parking
# losses (<= 8 km/h) all fail the spatial gates. The evaluator gates at 5 km/h
# (GPS course still meaningful) and reports the entry speed; the synthetic
# scan keeps the Opus 10 km/h.
GATE_MIN_KMH = 5.0
SCAN_MIN_KMH = 10.0
GATE_MAX_AGE_S = 2.0
MIN_EPISODE_S = 3.0
# Decision rule (both studies): long tunnels, jump threshold, sample size.
LONG_EPISODE_S = 50.0
JUMP_OK_M = 25.0
MIN_LONG_STRICT = 4   # Fable form (>= 4 long tunnels)
MIN_LONG_LOOSE = 3    # Opus form (>= 3 long tunnels)
# False-match scan.
SCAN_WINDOW_S = 40.0
SCAN_OFF_M = 20.0
SCAN_MAX_ALONG_M = 300.0
GRID_DEG = 0.005
VARIANTS = ("TAN", "REL", "SNAP")
WHEEL_HOLD_S = 0.5
SEND_HOLD_S = 1.5
FIX_HOLD_S = 1.5
STEP_S = 0.05
# A long episode counts for the decision only when at most this many seconds
# of its distance come from coarse speed (log_digest mean, fix speed, none);
# the 244 s beta.3 tunnel (153 s sensor hole) is excluded, as Fable did.
MAX_COARSE_SPEED_S = 10.0
LOG_NAME = re.compile(r"(?:^|/)(trace|collector)\.(\d+)\.jsonl$")


# ---------------------------------------------------------------- geometry
def ang(a):
    return (a + 180.0) % 360.0 - 180.0


def bearing_xy(dx, dy):
    return math.degrees(math.atan2(dx, dy)) % 360.0


class Chain:
    """A directed tunnel chain in a local metric frame."""

    def __init__(self, rec, origin):
        self.rec = rec
        self.xy = [osm_tunnels.local_xy(origin, (p[0] / 1e7, p[1] / 1e7)) for p in rec["pts"]]
        self.cum = [0.0]
        for a, b in zip(self.xy, self.xy[1:]):
            self.cum.append(self.cum[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
        self.length = self.cum[-1]

    def _segment(self, s):
        if s <= 0:
            i = 0
        elif s >= self.length:
            i = len(self.xy) - 2
        else:
            i = min(max(bisect.bisect_right(self.cum, s) - 1, 0), len(self.xy) - 2)
        while i > 0 and self.cum[i + 1] - self.cum[i] < 1e-6:
            i -= 1
        return i

    def tangent(self, s):
        i = self._segment(s)
        a, b = self.xy[i], self.xy[i + 1]
        return bearing_xy(b[0] - a[0], b[1] - a[1])

    def point(self, s):
        """Point at along-distance s; straight extrapolation before/after."""
        i = self._segment(s)
        a, b = self.xy[i], self.xy[i + 1]
        seg = self.cum[i + 1] - self.cum[i]
        if seg < 1e-9:
            return a
        w = (s - self.cum[i]) / seg
        return (a[0] + (b[0] - a[0]) * w, a[1] + (b[1] - a[1]) * w)

    def project(self, p, back=0.0, ahead=0.0):
        """(distance, s, cross, tangent) of the closest point, the line
        extended back/ahead by the given metres; cross > 0: p right of it."""
        best = None
        n = len(self.xy)
        for i in range(n - 1):
            a, b = self.xy[i], self.xy[i + 1]
            dx, dy = b[0] - a[0], b[1] - a[1]
            seg = math.hypot(dx, dy)
            if seg < 1e-9:
                continue
            t = ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / seg
            lo = -back if i == 0 else 0.0
            hi = seg + ahead if i == n - 2 else seg
            tc = min(max(t, lo), hi)
            q = (a[0] + dx * tc / seg, a[1] + dy * tc / seg)
            dist = math.hypot(p[0] - q[0], p[1] - q[1])
            if best is None or dist < best[0]:
                cross = ((p[0] - a[0]) * dy - (p[1] - a[1]) * dx) / seg
                best = (dist, self.cum[i] + tc, cross, bearing_xy(dx, dy))
        return best


class TunnelIndex:
    def __init__(self, db):
        self.db = db
        self.chains = db["chains"]
        self.grid = {}
        for idx, c in enumerate(self.chains):
            pts = c["pts"]
            if len(pts) < 2:
                continue
            cells = set()
            # cells around the entry region: 60 m before the start to 250 m in
            org = (pts[0][0] / 1e7, pts[0][1] / 1e7)
            ch = Chain(c, org)
            s = GATE_S_MIN_M - 20.0
            while s <= min(ch.length, GATE_S_MAX_M + 100.0):
                x, y = ch.point(s)
                lat = org[0] + y / (osm_tunnels.EARTH_M_PER_DEG_LAT)
                lon = org[1] + x / (111412.84 * math.cos(math.radians(org[0])))
                cells.add((int(math.floor(lat / GRID_DEG)), int(math.floor(lon / GRID_DEG))))
                s += 25.0
            for cell in cells:
                self.grid.setdefault(cell, []).append(idx)

    def near(self, lat, lon):
        ci, cj = int(math.floor(lat / GRID_DEG)), int(math.floor(lon / GRID_DEG))
        out = set()
        for di in (-1, 0, 1):
            for dj in (-1, 0, 1):
                out.update(self.grid.get((ci + di, cj + dj), ()))
        return sorted(out)

    def candidates(self, lat, lon, course, lateral=GATE_LATERAL_M, s_min=GATE_S_MIN_M, s_max=GATE_S_MAX_M,
                   heading=GATE_HEADING_DEG, margin=GATE_END_MARGIN_M):
        """Chains passing the entry gates for a fix at (lat, lon) heading course."""
        origin = (lat, lon)
        out = []
        for idx in self.near(lat, lon):
            ch = Chain(self.chains[idx], origin)
            if ch.length < 1.0:
                continue
            dist, s, cross, tan = ch.project((0.0, 0.0), back=-s_min)
            dh = ang(course - tan)
            ok = (dist <= lateral and s_min <= s <= min(s_max, ch.length - margin) and abs(dh) <= heading)
            if ok:
                out.append(dict(index=idx, chain=ch, lateral=dist, s=s, cross=cross, dh=dh, length=ch.length))
        return out


# ---------------------------------------------------------------- log input
def decode_location(hexstr):
    """LOCATION payload (48 bytes): lat, lon, accuracy, speed, bearing."""
    try:
        b = bytes.fromhex(hexstr)
    except (TypeError, ValueError):
        return None
    if len(b) != 48:
        return None
    g = lambda o: struct.unpack_from("<i", b, o)[0]
    return dict(lat=g(8) / 1e7, lon=g(12) / 1e7, acc=g(20) / 1e3 if b[16] else None,
                spd=g(36) / 1e3 if b[32] else None, brg=g(44) / 1e6 if b[40] else None)


class Boot:
    def __init__(self, boot_id):
        self.boot_id = boot_id
        self.fixes = {}      # key -> (t, lat, lon, course, kmh, acc_hint, source)
        self.modes = {}      # key -> (t, mode)
        self.wheels = {}     # (epoch, seq) -> (t, kmh)
        self.digests = {}    # mono -> (since, mono, mean kmh)
        self.reps = {}       # mono -> (t, lat, lon, spd m/s, brg, acc)
        self.exit_rows = {}  # mono -> row
        self.sources = set()

    def finish(self):
        self.fix_list = sorted(self.fixes.values())
        self.fix_t = [f[0] for f in self.fix_list]
        self.mode_list = sorted(self.modes.values())
        self.mode_t = [m[0] for m in self.mode_list]
        w = sorted(self.wheels.values())
        self.wheel_t = [x[0] for x in w]
        self.wheel_v = [x[1] for x in w]
        self.digest_list = sorted(self.digests.values())
        self.rep_list = sorted(self.reps.values())
        self.rep_t = [r[0] for r in self.rep_list]
        self.exit_list = sorted(self.exit_rows.values(), key=lambda r: r["fix_ms"])


class LogSet:
    """Rows needed by the evaluator, deduplicated across overlapping exports."""

    def __init__(self):
        self.boots = {}
        self.files = 0
        self.rows = 0

    def boot(self, bid):
        b = self.boots.get(bid)
        if b is None:
            b = self.boots[bid] = Boot(bid)
        return b

    # -- sources
    def read_path(self, path):
        if os.path.isdir(path):
            members = []
            for root, dirs, files in os.walk(path):
                dirs.sort()
                for name in sorted(files):
                    full = os.path.join(root, name)
                    if os.path.islink(full):
                        continue
                    if LOG_NAME.search(full.replace(os.sep, "/")):
                        members.append(full)
                    elif name.endswith((".tar", ".zip")):
                        self.read_path(full)
            self._read_group([(m, open(m, "rb")) for m in members], path)
        elif path.endswith(".zip"):
            with zipfile.ZipFile(path) as zf:
                logs = []
                for info in zf.infolist():
                    base = os.path.basename(info.filename)
                    if "__MACOSX" in info.filename or base.startswith("._"):
                        continue
                    if info.filename.endswith(".tar"):
                        with zf.open(info) as stream:
                            self._read_tar(tarfile.open(fileobj=stream, mode="r|*"), path + "!" + info.filename)
                    elif LOG_NAME.search(info.filename):
                        logs.append((info.filename, io.BytesIO(zf.read(info))))
                if logs:
                    self._read_group(logs, path)
        elif path.endswith((".tar", ".tar.gz", ".tgz")):
            with open(path, "rb") as f:
                self._read_tar(tarfile.open(fileobj=f, mode="r|*"), path)
        else:
            with open(path, "rb") as f:
                self._read_group([(path, f)], path)

    def _read_tar(self, archive, label):
        parsed = []
        for member in archive:
            if not member.isfile() or not LOG_NAME.search(member.name):
                continue
            if member.name.startswith("/") or ".." in member.name.split("/"):
                continue
            stream = archive.extractfile(member)
            parsed.append((member.name, self._parse(stream)))
        self._apply(parsed, label)

    def _read_group(self, named_streams, label):
        parsed = []
        for name, stream in named_streams:
            with stream:
                parsed.append((name, self._parse(stream)))
        self._apply(parsed, label)

    @staticmethod
    def _order(name):
        m = LOG_NAME.search(name.replace(os.sep, "/"))
        return (os.path.dirname(name), m.group(1), -int(m.group(2))) if m else (name, "", 0)

    def _parse(self, stream):
        """Keep only the row kinds the evaluator needs, as parsed dicts."""
        keep = (b'"boot"', b'"shadow_boot"', b'"collector_boot"', b'"position"', b'"position_poll"', b'"send"',
                b'"motion_batch"', b'"log_digest"', b'"beta_anchor"', b'"fix1"', b'"exit_jump"')
        rows = []
        for line in stream:
            head = line[:256]
            if not any(k in head for k in keep):
                continue
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if isinstance(r, dict):
                r.pop("request", None)
                r.pop("lds_association", None)
                rows.append(r)
        self.files += 1
        return rows

    def _apply(self, parsed, label):
        parsed.sort(key=lambda x: self._order(x[0]))
        current = {}
        for name, rows in parsed:
            family = self._order(name)[:2]
            for r in rows:
                kind = r.get("kind")
                if kind in ("boot", "shadow_boot", "collector_boot"):
                    if isinstance(r.get("boot_id"), str):
                        current[family] = r["boot_id"][:8]
                    continue
                bid = current.get(family)
                if bid is None:
                    continue
                self.rows += 1
                self._row(self.boot(bid), kind, r)
                self.boots[bid].sources.add(label)

    def _row(self, b, kind, r):
        try:
            if kind == "position":
                t = r["mono_ns"] / 1e9
                mode = r.get("mode")
                b.modes[("p", r["mono_ns"])] = (t, mode)
                if mode in (1, 2) and r.get("lat") is not None and r.get("class") != 1 and r.get("utc_s", 1):
                    b.fixes[("p", r["mono_ns"])] = (t, r["lat"], r["lon"], float(r.get("heading") or 0),
                                                   float(r.get("kmh") or 0), r.get("horizontal"), "position")
            elif kind == "position_poll":
                t = r["receipt_ns"] / 1e9
                mode = r.get("mode")
                b.modes[("c", r["receipt_ns"])] = (t, mode)
                if mode in (1, 2) and r.get("lat") is not None and r.get("utc_s", 1):
                    b.fixes[("c", r["receipt_ns"])] = (t, r["lat"], r["lon"], float(r.get("heading") or 0),
                                                      float(r.get("kmh") or 0), None, "collector")
            elif kind == "beta_anchor":
                b.modes[("a", r["mono_ns"])] = (r["mono_ns"] / 1e9, r.get("mode"))
            elif kind == "fix1":
                d = r.get("d")
                if not isinstance(d, list) or len(d) < 8:
                    return
                t = d[0] / 1000.0
                b.modes[("f", d[0])] = (t, d[1])
                if d[1] in (1, 2) and d[5] >= 0:
                    acc = d[6] / 1000.0 if d[6] >= 0 else None
                    b.fixes[("f", d[0])] = (t, d[2] / 1e7, d[3] / 1e7, d[5] / 1e6,
                                           d[4] * 3.6 / 1000.0 if d[4] >= 0 else 0.0, acc, "fix1")
            elif kind == "send":
                if r.get("type") == 1 and r.get("choice") == 3:
                    q = decode_location(r.get("outgoing_hex"))
                    if q and q["brg"] is not None and q["spd"] is not None:
                        b.reps[r["mono_ns"]] = (r["mono_ns"] / 1e9, q["lat"], q["lon"], q["spd"], q["brg"], q["acc"])
            elif kind == "motion_batch":
                epoch = r.get("epoch")
                for e in r.get("events") or ():
                    if e and e[0] == 1 and len(e) >= 8:
                        b.wheels[(epoch, e[1])] = (e[2] / 1e9, sum(x * 0.01 - 100.0 for x in e[4:8]) / 4.0)
            elif kind == "log_digest":
                if r.get("speed_kmh_mean") is not None and r.get("since_ns") is not None:
                    b.digests[r["mono_ns"]] = (r["since_ns"] / 1e9, r["mono_ns"] / 1e9, float(r["speed_kmh_mean"]))
            elif kind == "exit_jump":
                b.exit_rows[r["mono_ns"]] = r
        except (KeyError, TypeError, ValueError, IndexError):
            return

    def finish(self):
        for b in self.boots.values():
            b.finish()
        return self


# ---------------------------------------------------------------- evaluation
def speed_at(b, t):
    """(m/s or None, source) at time t."""
    i = bisect.bisect_right(b.wheel_t, t) - 1
    if i >= 0 and t - b.wheel_t[i] <= WHEEL_HOLD_S:
        return max(b.wheel_v[i], 0.0) / 3.6, "wheel"
    j = bisect.bisect_right(b.rep_t, t) - 1
    if j >= 0 and t - b.rep_t[j] <= SEND_HOLD_S:
        return b.rep_list[j][3], "send"
    for since, mono, mean in b.digest_list:
        if since <= t <= mono:
            return mean / 3.6, "digest"
    k = bisect.bisect_right(b.fix_t, t) - 1
    if k >= 0 and t - b.fix_t[k] <= FIX_HOLD_S:
        return b.fix_list[k][4] / 3.6, "fix"
    return None, "none"


def distance_steps(b, t0, t1):
    """[(ds)] per STEP_S step and the seconds per speed source."""
    steps, src = [], {}
    t = t0
    while t < t1 - 1e-9:
        dt = min(STEP_S, t1 - t)
        v, k = speed_at(b, t)
        src[k] = src.get(k, 0.0) + dt
        steps.append((v or 0.0) * dt)
        t += dt
    return steps, {k: round(v, 1) for k, v in src.items()}


def run_variant(name, ch, s0, seed_course, steps):
    """End position (x, y) and heading of one variant from the origin."""
    x = y = 0.0
    s = s0
    tan0 = ch.tangent(s0)
    h = ch.tangent(s0) if name != "REL" else seed_course
    for ds in steps:
        if name == "TAN":
            h = ch.tangent(s + ds / 2.0)
        elif name == "REL":
            h = (seed_course + ang(ch.tangent(s + ds / 2.0) - tan0)) % 360.0
        x += ds * math.sin(math.radians(h))
        y += ds * math.cos(math.radians(h))
        s += ds
    if name == "SNAP":
        x, y = ch.point(s)
        h = ch.tangent(s)
    elif name == "TAN":
        h = ch.tangent(s)
    elif name == "REL":
        h = (seed_course + ang(ch.tangent(s) - tan0)) % 360.0
    return (x, y), h, s


def jump_parts(fix_xy, est_xy, course):
    jx, jy = fix_xy[0] - est_xy[0], fix_xy[1] - est_xy[1]
    u = (math.sin(math.radians(course)), math.cos(math.radians(course)))
    return dict(jump=round(math.hypot(jx, jy), 1), along=round(jx * u[0] + jy * u[1], 1),
                cross=round(jx * u[1] - jy * u[0], 1))


def episodes(b):
    """GPS-lost runs (mode 0): list of dict. A run still open at the end of
    the boot (parking, ignition off) has ret None and no post fix; it is
    kept for the match check only."""
    out = []
    run = None

    def add(t0, ret, last):
        dur = (ret if ret is not None else last) - t0
        if dur < MIN_EPISODE_S:
            return
        i = bisect.bisect_left(b.fix_t, t0) - 1
        post = None
        if ret is not None:
            j = bisect.bisect_left(b.fix_t, ret - 0.01)
            post = b.fix_list[j] if j < len(b.fix_list) else None
        out.append(dict(t0=t0, ret=ret, dur=dur, pre=b.fix_list[i] if i >= 0 else None, post=post))

    for t, mode in b.mode_list:
        if mode == 0:
            if run is None:
                run = [t, t]
            else:
                run[1] = t
        elif mode in (1, 2) and run is not None:
            add(run[0], t, t)
            run = None
    if run is not None:
        add(run[0], None, run[1])
    return out


def episode_id(b, ep):
    if ep["ret"] is None:
        return "%s@%.1f(open)" % (b.boot_id, ep["t0"])
    return "%s@%.1f" % (b.boot_id, ep["ret"])


def evaluate_episode(index, b, ep):
    row = dict(id=episode_id(b, ep), boot=b.boot_id, lost_s=round(ep["t0"], 1),
               return_s=None if ep["ret"] is None else round(ep["ret"], 1), dur_s=round(ep["dur"], 1),
               long=ep["dur"] >= LONG_EPISODE_S, open_end=ep["ret"] is None)
    pre, post = ep["pre"], ep["post"]
    if pre is None:
        row["match"] = "no_pre_fix"
        return row, None
    age = ep["t0"] - pre[0]
    row.update(entry_kmh=round(pre[4], 1), entry_age_s=round(age, 1))
    gates = []
    if pre[4] < GATE_MIN_KMH:
        gates.append("entry_speed")
    if age > GATE_MAX_AGE_S:
        gates.append("entry_age")
    cands = index.candidates(pre[1], pre[2], pre[3])
    row["candidates"] = len(cands)
    reps = []
    if post is not None and post[0] - ep["ret"] <= 10.0:
        row.update(exit_kmh=round(post[4], 1), exit_fix_lag_s=round(post[0] - ep["ret"], 2),
                   exit_fix_source=post[6])
        origin = (pre[1], pre[2])
        fix_xy = osm_tunnels.local_xy(origin, (post[1], post[2]))
        course = post[3]
        # current product: last replacement LOCATION extrapolated to the fix time
        lo = bisect.bisect_left(b.rep_t, ep["t0"] - 1.0)
        hi = bisect.bisect_right(b.rep_t, ep["ret"] + 0.01)
        reps = b.rep_list[lo:hi]
        row["replaced_sends"] = len(reps)
        if reps:
            last = reps[-1]
            dt = post[0] - last[0]
            x, y = osm_tunnels.local_xy(origin, (last[1], last[2]))
            x += last[3] * dt * math.sin(math.radians(last[4]))
            y += last[3] * dt * math.cos(math.radians(last[4]))
            cur = jump_parts(fix_xy, (x, y), course)
            cur["heading_err"] = round(ang(last[4] - course), 1)
            row["current"] = cur
        rows = [r for r in b.exit_list if abs(r.get("fix_ms", 0) / 1000.0 - post[0]) <= 2.0]
        if rows:
            r = rows[0]
            row["exit_jump_row"] = dict(jump=r.get("jump_m"), along=r.get("along_m"), cross=r.get("cross_m"),
                                        dbrg=r.get("dbrg"))
    else:
        post = None
    if gates:
        row["match"] = "gate:" + ",".join(gates)
        return row, None
    if not cands:
        row["match"] = "none"
        return row, None
    if len(cands) > 1:
        row["match"] = "ambiguous"
        row["ambiguous_lengths_m"] = sorted(round(c["length"]) for c in cands)
        return row, None
    if post is None:
        row["match"] = "unique_no_exit_fix"
        row["tunnel"] = dict(length_m=round(cands[0]["length"]))
        return row, None
    c = cands[0]
    ch = c["chain"]
    row["match"] = "unique"
    row["tunnel"] = dict(length_m=round(ch.length), lateral_m=round(c["lateral"], 1), s_m=round(c["s"], 1),
                         heading_diff_deg=round(c["dh"], 1), end=ch.rec.get("end"), oneway=ch.rec.get("oneway"))
    steps, src = distance_steps(b, pre[0], post[0])
    dist = sum(steps)
    row["wheel_distance_m"] = round(dist)
    row["speed_sources_s"] = src
    row["uncovered_s"] = src.get("none", 0.0)
    # seconds whose speed is neither a wheel sample nor the product's own
    # (wheel-based) replacement speed: log_digest means, fix speeds, none
    row["coarse_speed_s"] = round(sum(v for k, v in src.items() if k not in ("wheel", "send")), 1)
    row["decision_eligible"] = bool(row["long"] and row["coarse_speed_s"] <= MAX_COARSE_SPEED_S)
    seed, row["rel_seed"] = (reps[0][4], "first_replacement") if reps else (pre[3], "pre_loss_fix_course")
    for name in VARIANTS:
        est, h, s_end = run_variant(name, ch, c["s"], seed, steps)
        res = jump_parts(fix_xy, est, course)
        res["heading_err"] = round(ang(h - course), 1)
        res["beyond_end_m"] = round(max(0.0, s_end - ch.length))
        res["fork_passed"] = bool(ch.rec.get("end") == "fork" and s_end > ch.length)
        row[name] = res
    private = dict(pre=(round(pre[1], 6), round(pre[2], 6)), post=(round(post[1], 6), round(post[2], 6)),
                   chain=ch.rec.get("id"), ways=ch.rec.get("ways"), name=ch.rec.get("name"))
    return row, private


def false_match_scan(index, b, lateral=GATE_LATERAL_M):
    stats = dict(fixes=0, unique=0, ambiguous=0, on=0, lost=0, off=0, none=0)
    off_sites = set()
    fixes = []
    for f in b.fix_list:
        if fixes and f[0] - fixes[-1][0] < 0.3:
            continue
        fixes.append(f)
    for i, f in enumerate(fixes):
        if f[4] < SCAN_MIN_KMH:
            continue
        stats["fixes"] += 1
        cands = index.candidates(f[1], f[2], f[3], lateral=lateral)
        if not cands:
            continue
        if len(cands) > 1:
            stats["ambiguous"] += 1
            continue
        stats["unique"] += 1
        c = cands[0]
        ch = c["chain"]
        origin = (f[1], f[2])
        limit = min(ch.length, SCAN_MAX_ALONG_M)
        tend = f[0] + SCAN_WINDOW_S
        on = off = 0
        for g in fixes[i + 1:]:
            if g[0] > tend:
                break
            dist, s, cross, tan = ch.project(osm_tunnels.local_xy(origin, (g[1], g[2])), back=400.0, ahead=400.0)
            if s > limit:
                tend = g[0]
                break
            if s < 20.0:
                continue
            if dist <= SCAN_OFF_M:
                on += 1
            else:
                off += 1
        j0 = bisect.bisect_left(b.mode_t, f[0])
        j1 = bisect.bisect_right(b.mode_t, tend)
        if any(m[1] == 0 for m in b.mode_list[j0:j1]):
            stats["lost"] += 1
        elif off:
            stats["off"] += 1
            off_sites.add(c["index"])
        elif on:
            stats["on"] += 1
        else:
            stats["none"] += 1
    return stats, off_sites


def decide(rows, false_sites, min_long=MIN_LONG_STRICT):
    long_rows = [r for r in rows if r.get("match") == "unique" and r.get("decision_eligible")]
    per = {}
    for name in VARIANTS + ("current",):
        vals = [r[name]["jump"] for r in long_rows if name in r]
        per[name] = dict(evaluable=len(vals), below=sum(1 for v in vals if v < JUMP_OK_M))
    best = max(VARIANTS, key=lambda n: (per[n]["below"], -per[n]["evaluable"]))
    n_eval = max(per[n]["evaluable"] for n in VARIANTS)
    if false_sites:
        verdict = "not met: %d false-match site(s) in the scan" % false_sites
    elif per[best]["below"] >= min_long:
        verdict = "met: %s below %.0f m in %d of %d long tunnels (>= %d needed), zero false matches" % (
            best, JUMP_OK_M, per[best]["below"], per[best]["evaluable"], min_long)
    elif n_eval < min_long:
        verdict = "not enough data: %d matched long tunnel(s) (>= %d s) evaluable, %d needed" % (
            n_eval, LONG_EPISODE_S, min_long)
    else:
        verdict = "not met with current data: best %s below %.0f m in %d of %d long tunnels (%d needed)" % (
            best, JUMP_OK_M, per[best]["below"], per[best]["evaluable"], min_long)
    return dict(rule=dict(long_episode_s=LONG_EPISODE_S, jump_ok_m=JUMP_OK_M, min_long=min_long,
                          false_matches_allowed=0),
                long_matched=len(long_rows), per_variant=per,
                loose_rule_met=bool(not false_sites and per[best]["below"] >= MIN_LONG_LOOSE),
                verdict=verdict)


def evaluate(paths, db_path, min_long=MIN_LONG_STRICT, scan=True, logs=None):
    db = osm_tunnels.load_db(db_path)
    index = TunnelIndex(db)
    if logs is None:
        logs = LogSet()
        for p in paths:
            logs.read_path(p)
        logs.finish()
    rows, private = [], {}
    scan_total = dict(fixes=0, unique=0, ambiguous=0, on=0, lost=0, off=0, none=0)
    off_sites = set()
    for bid in sorted(logs.boots):
        b = logs.boots[bid]
        for ep in episodes(b):
            row, priv = evaluate_episode(index, b, ep)
            rows.append(row)
            if priv:
                private[row["id"]] = priv
        if scan:
            stats, sites = false_match_scan(index, b)
            for k, v in stats.items():
                scan_total[k] += v
            off_sites |= sites
    rows.sort(key=lambda r: (r["boot"], r["lost_s"]))
    scan_total["off_sites"] = len(off_sites)
    decision = decide(rows, len(off_sites), min_long)
    if not scan:
        decision["verdict"] += " (false-match scan skipped)"
    db_info = dict(source=db.get("source", {}).get("file"), osm_timestamp=db.get("source", {}).get("osm_timestamp"),
                   md5=db.get("source", {}).get("md5"), chains=db.get("counts", {}).get("chains"))
    report = dict(scope="record_only_counterfactual_not_product_behaviour", database=db_info,
                  boots=len(logs.boots), episodes=rows, false_match_scan=scan_total if scan else None,
                  decision=decision, attribution=db.get("attribution"),
                  reference="first returned GPS fix (a reference, not ground truth)")
    return report, private, off_sites


def fmt(v, spec="%.1f"):
    return "-" if v is None else spec % v


def print_report(report, private=None, out=sys.stdout):
    w = lambda s="": print(s, file=out)
    w("Map-aided tunnel counterfactual (record only; OSM %s, %s chains)" %
      (report["database"]["osm_timestamp"] or report["database"]["source"], report["database"]["chains"]))
    eps = report["episodes"]
    matched = [r for r in eps if r.get("match") == "unique"]
    counts = {}
    for r in eps:
        counts[r["match"]] = counts.get(r["match"], 0) + 1
    w("GPS-lost episodes >= %.0f s: %d in %d boots; match: %s" % (
        MIN_EPISODE_S, len(eps), report["boots"], ", ".join("%s %d" % kv for kv in sorted(counts.items()))))
    w("%-20s %6s %6s %5s %6s %6s %5s | %-17s | %-17s | %-17s | %-17s" %
      ("episode", "dur_s", "L_m", "lat", "s_m", "dh", "crs_s", "current jump/al/cr", "TAN jump/al/cr",
       "REL jump/al/cr", "SNAP jump/al/cr"))
    for r in eps:
        t = r.get("tunnel") or {}

        def j(name):
            v = r.get(name)
            return "%5.1f/%5.1f/%5.1f" % (v["jump"], v["along"], v["cross"]) if v else "%-17s" % "-"
        w("%-20s %6.1f %6s %5s %6s %6s %5s | %s | %s | %s | %s%s" %
          (r["id"], r["dur_s"], fmt(t.get("length_m"), "%d"), fmt(t.get("lateral_m")), fmt(t.get("s_m"), "%.0f"),
           fmt(t.get("heading_diff_deg"), "%.0f"), fmt(r.get("coarse_speed_s")), j("current"), j("TAN"), j("REL"),
           j("SNAP"), "" if r.get("match") == "unique" else "  (%s)" % r["match"]))
    for r in matched:
        w("  %s exit heading error (deg): current %s, TAN %s, REL %s, SNAP %s; beyond end %s m%s%s" %
          (r["id"], fmt((r.get("current") or {}).get("heading_err")), r["TAN"]["heading_err"],
           r["REL"]["heading_err"], r["SNAP"]["heading_err"], r["TAN"]["beyond_end_m"],
           "; fork passed (branch not chosen)" if r["TAN"]["fork_passed"] else "",
           "; exit_jump row %s m" % r["exit_jump_row"]["jump"] if r.get("exit_jump_row") else ""))
    scan = report.get("false_match_scan")
    if scan:
        w("False-match scan (fixes >= %.0f km/h as synthetic losses): %d fixes, %d unique matches (%d entered GPS "
          "loss, %d stayed on the chain, %d left it, %d no later fix), %d ambiguous; false-match sites: %d" %
          (SCAN_MIN_KMH, scan["fixes"], scan["unique"], scan["lost"], scan["on"], scan["off"], scan["none"],
           scan["ambiguous"], scan["off_sites"]))
    d = report["decision"]
    per = d["per_variant"]
    w("Decision rule: long tunnels (>= %.0f s) with jump < %.0f m for one variant >= %d, zero false matches" %
      (d["rule"]["long_episode_s"], d["rule"]["jump_ok_m"], d["rule"]["min_long"]))
    w("  long matched and eligible (coarse speed <= %.0f s) %d; below/evaluable: %s" % (
        MAX_COARSE_SPEED_S, d["long_matched"],
        ", ".join("%s %d/%d" % (n, per[n]["below"], per[n]["evaluable"]) for n in VARIANTS + ("current",))))
    w("  verdict: %s (Opus form >= %d: %s)" % (d["verdict"], MIN_LONG_LOOSE, "met" if d["loose_rule_met"] else
                                                 "not met"))
    w("Reference = first returned GPS fix, not ground truth. Counterfactual only; the product is unchanged.")
    if private:
        w("PRIVATE details (terminal only; do not publish):")
        for k, v in sorted(private.items()):
            w("  %s %s" % (k, json.dumps(v, ensure_ascii=False)))
        if report.get("_off_sites"):
            w("  false-match chain ids: %s" % report["_off_sites"])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("inputs", nargs="+", help="exported mx5dr-logs-*.tar, upload .zip, directories or JSONL")
    parser.add_argument("--db", required=True, help="tunnel database from tools/osm_tunnels.py")
    parser.add_argument("--min-long", type=int, default=MIN_LONG_STRICT, help="long tunnels needed (default 4)")
    parser.add_argument("--no-scan", action="store_true", help="skip the false-match scan")
    parser.add_argument("--json", action="store_true", help="machine-readable report (no coordinates)")
    parser.add_argument("--private", action="store_true", help="also print coordinates, names and OSM ids")
    args = parser.parse_args(argv)
    report, private, off_sites = evaluate(args.inputs, args.db, args.min_long, scan=not args.no_scan)
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        if args.private:
            report["_off_sites"] = sorted(off_sites)
        print_report(report, private if args.private else None)
    return 0


if __name__ == "__main__":
    sys.exit(main())
