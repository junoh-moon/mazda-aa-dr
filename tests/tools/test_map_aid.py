"""Map-aided tunnel counterfactual evaluator and OSM tunnel database builder
(tools/map_aid_eval.py, tools/osm_tunnels.py;
validation/MAP_AID_EVALUATOR_2026-10-14.md).

Synthetic fixtures only (coordinates are made up around 37.0 N 127.0 E, not
a real road). The private regression on the owner's recordings runs only
when MX5DR_MAP_AID_LOGS (a directory or archive of exported logs) and
MX5DR_MAP_AID_DB (a database built by tools/osm_tunnels.py from the
2026-10-13 Geofabrik extract) are set; it reads the archives in memory and
unpacks nothing to disk.
"""
import io
import json
import math
import os
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import map_aid_eval as M  # noqa: E402
import osm_tunnels as T  # noqa: E402

ORIGIN = (37.0, 127.0)
M_LAT = T.EARTH_M_PER_DEG_LAT - 559.822 * math.cos(2 * math.radians(ORIGIN[0]))
M_LON = 111412.84 * math.cos(math.radians(ORIGIN[0]))


def ll(x, y):
    """Synthetic metres east/north of ORIGIN -> (lat, lon)."""
    return (ORIGIN[0] + y / M_LAT, ORIGIN[1] + x / M_LON)


class Net:
    """Tiny synthetic OSM network: ways from metre polylines."""

    def __init__(self):
        self.ways, self.coords, self.nid, self.shared = {}, {}, 1, {}

    def node(self, x, y):
        key = (round(x, 3), round(y, 3))
        if key in self.shared:
            return self.shared[key]
        n = self.nid
        self.nid += 1
        self.coords[n] = ll(x, y)
        self.shared[key] = n
        return n

    def way(self, wid, pts, **tags):
        tags.setdefault("highway", "primary")
        tags.setdefault("tunnel", "yes")
        self.ways[wid] = dict(tags=tags, nodes=[self.node(x, y) for x, y in pts])

    def db(self, path):
        chains = T.build({k: v for k, v in self.ways.items() if T.is_tunnel_way(v["tags"])}, self.coords)
        T.write_db(T.make_db(chains, dict(file="synthetic", osm_timestamp="synthetic"), {}), path)
        return chains


def line(x0, y0, x1, y1, step=10.0):
    n = max(1, int(math.hypot(x1 - x0, y1 - y0) / step))
    return [(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n) for i in range(n + 1)]


def arc(cx, cy, r, a0, a1, step=10.0):
    """Points on a circle (angles in degrees from north, clockwise)."""
    n = max(2, int(abs(math.radians(a1 - a0)) * r / step))
    return [(cx + r * math.sin(math.radians(a0 + (a1 - a0) * i / n)),
             cy + r * math.cos(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]


def location_hex(lat, lon, spd, brg, acc=10.0):
    b = bytearray(48)
    struct.pack_into("<i", b, 8, int(round(lat * 1e7)))
    struct.pack_into("<i", b, 12, int(round(lon * 1e7)))
    b[16] = 1
    struct.pack_into("<i", b, 20, int(acc * 1000))
    b[32] = 1
    struct.pack_into("<i", b, 36, int(spd * 1000))
    b[40] = 1
    struct.pack_into("<i", b, 44, int(brg * 1e6))
    return b.hex()


class Drive:
    """Synthetic trace: path(s) -> (x, y, heading); constant speed v (m/s).
    Positions at 1 Hz, wheels at 10 Hz, GPS lost in [lost_s, ret_s) of path
    length; replacement sends during the loss dead-reckon straight from the
    loss point with heading drifting by drift_deg_s (the product's error)."""

    def __init__(self, boot="b00tcafe", t0=100.0):
        self.boot, self.t0, self.rows, self.t, self.seq = boot, t0, [], t0, 0
        self.rows.append(dict(kind="boot", schema=1, mono_ns=int(t0 * 1e9) - 1, boot_id=boot + "-0000"))

    def drive(self, path, length, v, lost=None, drift_deg_s=0.0, open_end=False):
        t_end = self.t + length / v
        dr = None
        k = 0
        while self.t <= t_end + 1e-9:
            s = (self.t - (t_end - length / v)) * v
            x, y, h = path(min(s, length))
            ns = int(self.t * 1e9)
            if k % 10 == 0:
                in_loss = lost is not None and lost[0] <= s < lost[1]
                if lost is not None and open_end and s >= lost[0]:
                    in_loss = True
                lat, lon = ll(x, y)
                pos = dict(kind="position", call=1, generation=1, mono_ns=ns, mode=0 if in_loss else 1,
                           utc_s=0 if in_loss else 1791400000 + k, lat=None if in_loss else lat,
                           lon=None if in_loss else lon, heading=0 if in_loss else int(round(h)) % 360,
                           kmh=0 if in_loss else int(round(v * 3.6)), horizontal=0.9, **{"class": 3 if in_loss else 2})
                self.rows.append(pos)
                if in_loss:
                    if dr is None:
                        dr = [x, y, h, self.t]
                    dt = self.t - dr[3]
                    hd = dr[2] + drift_deg_s * dt
                    dr[0] += v * 1.0 * math.sin(math.radians(hd)) if dt > 0 else 0.0
                    dr[1] += v * 1.0 * math.cos(math.radians(hd)) if dt > 0 else 0.0
                    dlat, dlon = ll(dr[0], dr[1])
                    self.rows.append(dict(kind="send", call=1, generation=1, mono_ns=ns + 1000, type=1, choice=3,
                                          outgoing_hex=location_hex(dlat, dlon, v, hd % 360)))
            counts = int(round((v * 3.6 + 100.0) / 0.01))
            self.rows.append(dict(kind="motion_batch", schema=1, epoch=7,
                                  events=[[1, self.seq, ns, 0, counts, counts, counts, counts, 0, 0]]))
            self.seq += 1
            self.t += 0.1
            k += 1

    def write(self, directory):
        logs = Path(directory) / "tmp/mnt/data_persist/mx5-aa-dr/logs"
        logs.mkdir(parents=True, exist_ok=True)
        with open(logs / "trace.0.jsonl", "w") as f:
            for r in self.rows:
                f.write(json.dumps(r) + "\n")
        return str(directory)


def straight_path(x0, y0, h):
    return lambda s: (x0 + s * math.sin(math.radians(h)), y0 + s * math.cos(math.radians(h)), h)


def poly_path(pts):
    """Path along a metre polyline: returns (path(s), length)."""
    cum = [0.0]
    for a, b in zip(pts, pts[1:]):
        cum.append(cum[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))

    def path(s):
        i = 0
        while i < len(pts) - 2 and cum[i + 1] < s:
            i += 1
        a, b = pts[i], pts[i + 1]
        seg = cum[i + 1] - cum[i]
        w = (s - cum[i]) / seg
        return (a[0] + (b[0] - a[0]) * w, a[1] + (b[1] - a[1]) * w,
                math.degrees(math.atan2(b[0] - a[0], b[1] - a[1])) % 360)
    return path, cum[-1]


def evaluate(drive, net, tmp, **kw):
    db = os.path.join(tmp, "db.json.gz")
    net.db(db)
    report, private, sites = M.evaluate([drive.write(os.path.join(tmp, "logs"))], db, **kw)
    return report


class BuilderTest(unittest.TestCase):
    def test_directions(self):
        self.assertEqual(T.travel_directions({"oneway": "yes"}), (True, False))
        self.assertEqual(T.travel_directions({"oneway": "-1"}), (False, True))
        self.assertEqual(T.travel_directions({"highway": "motorway"}), (True, False))
        self.assertEqual(T.travel_directions({"highway": "motorway", "oneway": "no"}), (True, True))
        self.assertEqual(T.travel_directions({"highway": "primary"}), (True, True))
        self.assertFalse(T.is_tunnel_way({"highway": "service", "tunnel": "yes"}))
        self.assertFalse(T.is_tunnel_way({"highway": "primary", "tunnel": "building_passage"}))
        self.assertTrue(T.is_tunnel_way({"highway": "primary", "tunnel": "yes"}))

    def test_chain_merges_split_ways_and_simplifies(self):
        net = Net()
        net.way(1, line(0, 0, 0, 300), oneway="yes")
        net.way(2, line(0, 300, 0, 800), oneway="yes")
        chains = T.build(net.ways, net.coords)
        self.assertEqual(len(chains), 1)
        c = chains[0]
        self.assertEqual((c["start"], c["end"], c["ways"], c["oneway"]), ("portal", "portal", [1, 2], True))
        self.assertEqual(len(c["pts"]), 2)          # 81 collinear nodes -> 2 points at 2 m
        self.assertAlmostEqual(c["len_m"], 800.0, delta=1.0)

    def test_two_way_gives_both_directions(self):
        net = Net()
        net.way(1, line(0, 0, 0, 500))
        chains = T.build(net.ways, net.coords)
        self.assertEqual(len(chains), 2)
        self.assertTrue(all(not c["oneway"] for c in chains))

    def test_fork(self):
        net = Net()
        net.way(1, line(0, 0, 0, 400), oneway="yes")
        net.way(2, line(0, 400, -100, 700), oneway="yes")
        net.way(3, line(0, 400, 100, 700), oneway="yes")
        chains = {c["ways"][0]: c for c in T.build(net.ways, net.coords)}
        self.assertEqual(chains[1]["end"], "fork")
        self.assertEqual(sorted(chains[1]["next"]), sorted([chains[2]["id"], chains[3]["id"]]))
        self.assertEqual((chains[2]["start"], chains[3]["start"]), ("branch", "branch"))

    def test_merge_does_not_end_chain(self):
        net = Net()
        net.way(1, line(0, 0, 0, 400), oneway="yes")
        net.way(2, line(100, 100, 0, 400), oneway="yes")   # ramp joining inside
        net.way(3, line(0, 400, 0, 900), oneway="yes")
        chains = {tuple(c["ways"]): c for c in T.build(net.ways, net.coords)}
        self.assertIn((1, 3), chains)
        self.assertIn((2, 3), chains)
        self.assertEqual(chains[(1, 3)]["end"], "portal")

    def test_write_load_manifest(self):
        net = Net()
        net.way(1, line(0, 0, 0, 500), oneway="yes", name="synthetic")
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "x", "tunnels-v1.json.gz")
            net.db(path)
            db = T.load_db(path)
            self.assertEqual((db["schema"], db["version"]), (T.SCHEMA, T.VERSION))
            man = json.load(open(T.manifest_path(path)))
            self.assertIn("ODbL", man["attribution"])
            self.assertEqual(man["database"]["md5"], T.md5_file(path))
            self.assertEqual(man["counts"]["chains"], 1)
            self.assertNotIn("chains", man)

    def test_osm_xml_fixture(self):
        try:
            import osmium  # noqa: F401
        except ImportError:
            self.skipTest("pyosmium not installed (optional; tools/README_MAP_AID.md)")
        nodes = "".join('<node id="%d" version="1" lat="%.7f" lon="%.7f"/>' % (i + 1, *ll(0, 100 * i))
                        for i in range(6))
        xml = ('<?xml version="1.0" encoding="UTF-8"?><osm version="0.6" generator="test">%s'
               '<way id="10" version="1"><nd ref="1"/><nd ref="2"/><nd ref="3"/>'
               '<tag k="highway" v="trunk"/><tag k="tunnel" v="yes"/><tag k="oneway" v="yes"/></way>'
               '<way id="11" version="1"><nd ref="3"/><nd ref="4"/><tag k="highway" v="trunk"/></way>'
               '<way id="12" version="1"><nd ref="4"/><nd ref="5"/><nd ref="6"/>'
               '<tag k="highway" v="service"/><tag k="tunnel" v="yes"/></way></osm>') % nodes
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "tiny.osm")
            with open(src, "w") as f:
                f.write(xml)
            out = os.path.join(tmp, "db", "t.json.gz")
            self.assertEqual(T.main([src, "--out", out]), 0)
            db = T.load_db(out)
            self.assertEqual([c["ways"] for c in db["chains"]], [[10]])
            self.assertEqual(db["source"]["tunnel_ways"], 1)


class EvaluatorTest(unittest.TestCase):
    def test_straight_tunnel_offset_kept(self):
        net = Net()
        net.way(1, line(0, 0, 0, 800), oneway="yes")
        d = Drive()
        # car 3 m right (east) of the centreline, 20 m/s north; GPS lost from 30 m before the portal to 850 m
        d.drive(straight_path(3.0, -300.0, 0.0), 1250.0, 20.0, lost=(270.0, 1150.0), drift_deg_s=0.2)
        with tempfile.TemporaryDirectory() as tmp:
            rep = evaluate(d, net, tmp)
        [ep] = [r for r in rep["episodes"] if r["match"] == "unique"]
        self.assertAlmostEqual(ep["tunnel"]["lateral_m"], 3.0, delta=0.2)
        self.assertLess(ep["TAN"]["jump"], 1.0)
        self.assertLess(ep["REL"]["jump"], 1.0)
        self.assertAlmostEqual(ep["SNAP"]["jump"], 3.0, delta=0.5)
        self.assertGreater(ep["SNAP"]["cross"], 0)          # the fix lies right of the snapped estimate
        self.assertGreater(ep["current"]["jump"], 20.0)      # drifting DR
        self.assertLess(abs(ep["TAN"]["heading_err"]), 0.5)
        text = json.dumps(rep)
        self.assertNotIn('"lat"', text)                      # no coordinates in the report
        self.assertNotIn("%.3f" % ORIGIN[1], text)

    def test_curved_tunnel(self):
        net = Net()
        net.way(1, arc(400.0, 0.0, 400.0, -90.0, 0.0), oneway="yes")   # quarter circle: west -> north end, heading N -> E
        d = Drive()
        path, length = poly_path(line(0, -200, 0, 0) + arc(400.0, 0.0, 400.0, -90.0, 0.0, step=2.0)[1:] +
                                 line(400, 400, 500, 400)[1:])
        d.drive(path, length, 15.0, lost=(220.0, length - 90.0))
        with tempfile.TemporaryDirectory() as tmp:
            rep = evaluate(d, net, tmp)
        [ep] = [r for r in rep["episodes"] if r["match"] == "unique"]
        self.assertGreater(ep["current"]["jump"], 150.0)     # straight DR through a 90 deg bend
        self.assertLess(ep["TAN"]["jump"], 6.0)
        self.assertLess(ep["SNAP"]["jump"], 6.0)
        # 2 m simplification turns the arc into chords of up to ~80 m: the
        # last chord's tangent is up to ~6 deg off the true exit heading
        self.assertLess(abs(ep["TAN"]["heading_err"]), 7.0)

    def test_twin_tubes_and_same_direction_ambiguity(self):
        net = Net()
        net.way(1, line(0, 0, 0, 600), oneway="yes")
        net.way(2, line(-30, 600, -30, 0), oneway="yes")        # opposite tube 30 m west
        d = Drive()
        d.drive(straight_path(0.0, -200.0, 0.0), 960.0, 20.0, lost=(170.0, 900.0))
        with tempfile.TemporaryDirectory() as tmp:
            rep = evaluate(d, net, tmp)
            self.assertEqual([r["match"] for r in rep["episodes"]], ["unique"])
            net.way(3, line(8, 0, 8, 600), oneway="yes")         # same-direction tube 8 m east
            rep = evaluate(d, net, tmp)
        self.assertEqual([r["match"] for r in rep["episodes"]], ["ambiguous"])

    def test_fork_flagged(self):
        net = Net()
        net.way(1, line(0, 0, 0, 400), oneway="yes")
        net.way(2, line(0, 400, -100, 700), oneway="yes")
        net.way(3, line(0, 400, 100, 700), oneway="yes")
        d = Drive()
        path, length = poly_path([(0, -200), (0, 400), (100, 700), (150, 850)])
        d.drive(path, length, 20.0, lost=(170.0, length - 100.0))
        with tempfile.TemporaryDirectory() as tmp:
            rep = evaluate(d, net, tmp)
        [ep] = [r for r in rep["episodes"] if r["match"] == "unique"]
        self.assertEqual(ep["tunnel"]["end"], "fork")
        self.assertTrue(ep["TAN"]["fork_passed"])

    def test_parallel_surface_road_and_scan(self):
        net = Net()
        net.way(1, line(0, 0, 0, 132), oneway="yes")             # 132 m underpass
        d = Drive()
        # surface road 18 m east, GPS valid, turns right 20 m after the underpass start
        path, length = poly_path([(18.0, -300.0), (18.0, 20.0), (418.0, 20.0)])
        d.drive(path, length, 15.0)
        with tempfile.TemporaryDirectory() as tmp:
            rep = evaluate(d, net, tmp)
            self.assertEqual(rep["false_match_scan"]["off_sites"], 0)
            self.assertEqual(rep["false_match_scan"]["unique"], 0)
            db = os.path.join(tmp, "db.json.gz")
            logs = M.LogSet()
            logs.read_path(os.path.join(tmp, "logs"))
            logs.finish()
            index = M.TunnelIndex(T.load_db(db))
            stats, sites = M.false_match_scan(index, logs.boots["b00tcafe"], lateral=20.0)
        self.assertGreater(stats["off"], 0)                      # the Opus 20 m gate matches it
        self.assertEqual(len(sites), 1)

    def test_garage_loss_not_matched(self):
        net = Net()
        net.way(1, line(0, 0, 0, 600), oneway="yes")
        d = Drive()
        d.drive(straight_path(500.0, -100.0, 0.0), 120.0, 2.3, lost=(100.0, 1e9), open_end=True)
        with tempfile.TemporaryDirectory() as tmp:
            rep = evaluate(d, net, tmp)
        [ep] = rep["episodes"]
        self.assertTrue(ep["open_end"])
        self.assertNotIn(ep["match"], ("unique", "unique_no_exit_fix"))

    def test_tar_zip_and_dedup(self):
        net = Net()
        net.way(1, line(0, 0, 0, 800), oneway="yes")
        d = Drive()
        d.drive(straight_path(0.0, -300.0, 0.0), 1210.0, 20.0, lost=(270.0, 1150.0))
        with tempfile.TemporaryDirectory() as tmp:
            db = os.path.join(tmp, "db.json.gz")
            net.db(db)
            src = d.write(os.path.join(tmp, "logs"))
            tar = os.path.join(tmp, "mx5dr-logs-1.tar")
            with tarfile.open(tar, "w") as t:
                t.add(os.path.join(src, "tmp"), arcname="tmp")
            with open(tar, "rb") as f:
                buf = io.BytesIO(f.read())
            z = os.path.join(tmp, "upload.zip")
            with zipfile.ZipFile(z, "w") as zf:
                zf.writestr("mx5dr-logs-1.tar", buf.getvalue())
                zf.writestr("__MACOSX/._mx5dr-logs-1.tar", b"junk")
            one, _, _ = M.evaluate([tar], db)
            both, _, _ = M.evaluate([tar, z, src], db)          # overlapping exports deduplicate
        self.assertEqual(json.dumps(one["episodes"]), json.dumps(both["episodes"]))
        self.assertEqual(len(one["episodes"]), 1)

    def test_decision_rule(self):
        def row(j):
            return dict(long=True, decision_eligible=True, match="unique", TAN=dict(jump=j), REL=dict(jump=99.0), SNAP=dict(jump=99.0))
        self.assertTrue(M.decide([row(10.0)] * 2, 0)["verdict"].startswith("not enough data"))
        self.assertTrue(M.decide([row(10.0)] * 4, 0)["verdict"].startswith("met"))
        self.assertTrue(M.decide([row(10.0)] * 4, 1)["verdict"].startswith("not met"))
        d = M.decide([row(10.0)] * 3 + [row(40.0)] * 2, 0)
        self.assertTrue(d["verdict"].startswith("not met with current data"))
        self.assertTrue(d["loose_rule_met"])
        coarse = dict(row(10.0), decision_eligible=False)
        self.assertTrue(M.decide([row(10.0)] * 3 + [coarse], 0)["verdict"].startswith("not enough data"))

    def test_cli_and_analyzer_hook(self):
        net = Net()
        net.way(1, line(0, 0, 0, 800), oneway="yes", name="secret tunnel name")
        d = Drive()
        d.drive(straight_path(0.0, -300.0, 0.0), 1210.0, 20.0, lost=(270.0, 1150.0))
        with tempfile.TemporaryDirectory() as tmp:
            db = os.path.join(tmp, "db.json.gz")
            net.db(db)
            src = d.write(os.path.join(tmp, "logs"))
            out = subprocess.run([sys.executable, str(ROOT / "tools/map_aid_eval.py"), "--db", db, src],
                                 capture_output=True, text=True, check=True).stdout
            self.assertIn("verdict: not enough data", out)
            self.assertNotIn("secret", out)
            self.assertNotIn("%.4f" % ORIGIN[0], out)
            priv = subprocess.run([sys.executable, str(ROOT / "tools/map_aid_eval.py"), "--db", db, "--private",
                                   src], capture_output=True, text=True, check=True).stdout
            self.assertIn("secret", priv)
            res = subprocess.run([sys.executable, str(ROOT / "tools/analyze_logs.py"), "--map-db", db, src],
                                 capture_output=True, text=True)
            self.assertIn("Map-aided tunnel counterfactual", res.stdout)
            plain = subprocess.run([sys.executable, str(ROOT / "tools/analyze_logs.py"), src],
                                   capture_output=True, text=True)
            self.assertNotIn("Map-aided", plain.stdout)
            self.assertEqual(res.returncode, plain.returncode)
            js = subprocess.run([sys.executable, str(ROOT / "tools/analyze_logs.py"), "--json", "--map-db", db, src],
                                capture_output=True, text=True)
            self.assertEqual(json.loads(js.stdout)["map_aid"]["status"], "evaluated")
            bad = subprocess.run([sys.executable, str(ROOT / "tools/analyze_logs.py"), "--map-db",
                                  os.path.join(tmp, "missing.json.gz"), src], capture_output=True, text=True)
            self.assertIn("Map-aided tunnel counterfactual: unavailable", bad.stdout)
            self.assertEqual(bad.returncode, plain.returncode)


@unittest.skipUnless(os.environ.get("MX5DR_MAP_AID_LOGS") and os.environ.get("MX5DR_MAP_AID_DB"),
                     "private regression: set MX5DR_MAP_AID_LOGS and MX5DR_MAP_AID_DB")
class PrivateRegressionTest(unittest.TestCase):
    """Key numbers of the two 2026-10-14 studies on the owner's recordings
    (Geofabrik extract 2026-10-13). Tolerances cover the studies' method
    differences (hole fill, seed), not tuning."""

    def test_studies_reproduced(self):
        report, _, _ = M.evaluate([os.environ["MX5DR_MAP_AID_LOGS"]], os.environ["MX5DR_MAP_AID_DB"])
        eps = {r["id"]: r for r in report["episodes"]}
        long111, long60 = eps["5a9f9b2b@758.2"], eps["5a9f9b2b@908.2"]
        for ep in (long111, long60):
            self.assertEqual(ep["match"], "unique")
        self.assertTrue(115.0 <= long111["current"]["jump"] <= 125.0, long111["current"])   # 121.0 / 117.7
        self.assertTrue(15.0 <= long111["TAN"]["jump"] <= 24.0, long111["TAN"])             # 17.8 (Fable TAN)
        self.assertTrue(15.0 <= long111["SNAP"]["jump"] <= 25.0, long111["SNAP"])           # 23.0 (Opus P) / 18.3
        self.assertTrue(49.0 <= long60["current"]["jump"] <= 56.0, long60["current"])       # 53.1 / 53.6
        self.assertTrue(45.0 <= long60["SNAP"]["jump"] <= 54.0, long60["SNAP"])             # 49.7 (Opus P)
        self.assertTrue(36.0 <= long60["TAN"]["jump"] <= 48.0, long60["TAN"])               # 41.6 (Fable TAN)
        self.assertTrue(9.0 <= long60["REL"]["jump"] <= 20.0, long60["REL"])                # 13.9 (Fable REL)
        unique = [r for r in report["episodes"] if r["match"] == "unique"]
        self.assertGreaterEqual(len(unique), 8)                                            # 9 tunnel losses in the studies
        self.assertFalse([r["id"] for r in report["episodes"] if r.get("open_end") and r["match"].startswith("unique")])
        self.assertEqual(report["false_match_scan"]["off_sites"], 0)
        self.assertFalse(report["decision"]["verdict"].startswith("met"))


if __name__ == "__main__":
    unittest.main()
