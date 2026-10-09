import filecmp
import importlib.util
import json
import math
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PLAN = ROOT / "validation" / "DHU_NAVER_EXP7_PLAN_2026-10-09"
PLAN6 = ROOT / "validation" / "DHU_NAVER_EXP6_PLAN_2026-10-08"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load("dhu_exp7", PLAN / "gen_commands.py")
gen6 = load("dhu_exp6_for_exp7", PLAN6 / "gen_commands.py")

ENTRANCE = (35.1714322, 129.0250025)  # OSM node 731823096
EXIT = (35.1853302, 129.0043248)  # OSM node 731823121
ROAD_TUNNEL = 309.4  # bearing of the straight tunnel line (same value as the exp6 test)
SETUP_FIXES = 6
CONDITIONS = ["H0-correct", "H1-const20", "H2-const45", "H3-const90", "H4-const180", "H5-drift-stop", "H6-const90-cold",
              "H7-burst30"]


def enu(p, origin=ENTRANCE):
    k = math.pi / 180 * 6371000.0
    return ((p[1] - origin[1]) * k * math.cos(math.radians(origin[0])), (p[0] - origin[0]) * k)


def tunnel_frame(p):
    """(along metres from the entrance, signed cross-track metres, right of travel positive)."""
    ex, ey = enu(EXIT)
    n = math.hypot(ex, ey)
    ux, uy = ex / n, ey / n
    x, y = enu(p)
    return x * ux + y * uy, x * uy - y * ux


def angle_diff(a, b):
    return (a - b + 180.0) % 360.0 - 180.0


class Exp7Commands(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(cls.tmp.name)
        cls.out = base / "a"
        cls.plan_path = base / "a.json"
        cls.route, cls.doc = gen.generate(str(cls.out), str(cls.plan_path))
        cls.files = {p.stem: p.read_text().splitlines() for p in cls.out.glob("*.txt")}
        cls.out6 = base / "exp6"
        _, cls.doc6 = gen6.generate(str(cls.out6))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def fixes(self, tag):
        return [line.split() for line in self.files[tag] if line.startswith("location ")][SETUP_FIXES:]

    def phase(self, tag, name):
        plan = self.doc["runs"][tag]
        lo, hi = plan[name]
        return self.fixes(tag)[lo:hi + 1]

    def tunnel_offsets(self, tag):
        return [angle_diff(float(f[6]), ROAD_TUNNEL) for f in self.phase(tag, "tunnel")]

    # --- determinism and the committed files -------------------------------------------------------------------
    def test_generator_is_deterministic_and_matches_the_committed_files(self):
        with tempfile.TemporaryDirectory() as other:
            gen.generate(other + "/c", other + "/p.json")
            cmp = filecmp.dircmp(str(self.out), other + "/c")
            self.assertEqual((cmp.left_only, cmp.right_only, cmp.diff_files), ([], [], []))
            self.assertEqual(self.plan_path.read_bytes(), pathlib.Path(other + "/p.json").read_bytes())
        committed = PLAN / "commands"
        self.assertEqual(sorted(p.name for p in committed.glob("*.txt")), sorted(p.name for p in self.out.glob("*.txt")))
        for p in self.out.glob("*.txt"):
            self.assertEqual(p.read_bytes(), (committed / p.name).read_bytes(), p.name)
        self.assertEqual(json.loads((PLAN / "run-plan.json").read_text()), json.loads(self.plan_path.read_text()))

    def test_same_geometry_and_setup_as_experiment6(self):
        self.assertEqual((PLAN / "geometry" / "route.json").read_bytes(), (PLAN6 / "geometry" / "route.json").read_bytes())
        self.assertEqual((PLAN / "setup-template.txt").read_bytes(), (PLAN6 / "setup-template.txt").read_bytes())
        self.assertAlmostEqual(self.route.exit - self.route.entrance, 2433.1, delta=0.5)
        self.assertEqual(self.doc["runs"]["H0-correct-r1"]["start"], [35.1697679, 129.0267566, 328.9])

    # --- run counts, timeline and photos ------------------------------------------------------------------------
    def test_run_counts_and_one_hertz_timeline(self):
        self.assertEqual(sorted(gen.RUNS), sorted(CONDITIONS))
        self.assertEqual(len(self.files), 16)
        self.assertEqual(sorted(self.doc["order"]), sorted(self.files))
        for tag, lines in self.files.items():
            plan = self.doc["runs"][tag]
            seconds = sum(1 for line in lines if line in ("sleep 1", "sleep 0.8")) - 5  # setup holds 5 x sleep 1
            self.assertEqual(seconds, plan["duration_s"], tag)
            self.assertEqual(len(self.fixes(tag)), plan["duration_s"], tag)  # a fix every second, never a gap
            shots = [line for line in lines if line.startswith("screenshot ")]
            self.assertTrue(all(s.startswith("screenshot ../../../../experiment7/" + tag) for s in shots))
            self.assertEqual(len(shots), len(plan["frames"]) + 1)
            expected = 222 + (gen.STOP_S if tag.startswith("H5") else 0)
            self.assertEqual(plan["duration_s"], expected, tag)

    def test_photo_schedule_is_the_exp6_schedule_plus_the_dense_entrance(self):
        exp6 = {f["t"] for f in self.doc6["runs"]["T1-ideal-a40-r1"]["frames"]}
        for tag, plan in self.doc["runs"].items():
            ts = {f["t"] for f in plan["frames"]}
            if tag.startswith("H5"):
                s0, s1 = plan["stop"]
                self.assertTrue({s0, s0 + 2, s1, s1 + 3} <= ts)
                continue
            self.assertEqual(ts, exp6 | set(gen.ENTRANCE_DENSE), tag)

    # --- nothing but the bearing differs ------------------------------------------------------------------------
    def test_control_tunnel_fixes_equal_exp6_t1(self):
        ours = self.fixes("H0-correct-r1")
        theirs = [line.split() for line in (self.out6 / "T1-ideal-a40-r1.txt").read_text().splitlines()
                  if line.startswith("location ")][SETUP_FIXES:]
        self.assertEqual(ours, theirs)

    def test_only_the_bearing_differs_from_the_control(self):
        ref = self.fixes("H0-correct-r1")
        for tag in self.files:
            if tag.startswith("H5"):
                continue
            got = self.fixes(tag)
            self.assertEqual(len(got), len(ref), tag)
            for f, g in zip(got, ref):
                self.assertEqual(f[:6], g[:6], tag)  # position, accuracy, altitude, speed identical
            self.assertEqual(self.phase(tag, "approach"), self.phase("H0-correct-r1", "approach"), tag)
            self.assertEqual(self.phase(tag, "exit"), self.phase("H0-correct-r1", "exit"), tag)

    def test_positions_are_on_route_with_accuracy_40(self):
        for tag in self.files:
            for f in self.phase(tag, "tunnel"):
                _, cross = tunnel_frame((float(f[1]), float(f[2])))
                self.assertLess(abs(cross), 1.0, tag)
                self.assertEqual(f[3], "40.000", tag)
                self.assertNotEqual(f[6], "NAN", tag)
            for f in self.phase(tag, "approach") + self.phase(tag, "exit"):
                self.assertEqual(f[3], "5.000", tag)

    # --- bearing profiles ---------------------------------------------------------------------------------------
    def test_constant_offsets(self):
        for tag, value in (("H0-correct-r1", 0.0), ("H1-const20-r1", 20.0), ("H2-const45-r1", 45.0),
                           ("H3-const90-r1", 90.0), ("H6-const90-cold-r1", 90.0)):
            for off in self.tunnel_offsets(tag):
                self.assertAlmostEqual(off, value, delta=0.25, msg=tag)
        for off in self.tunnel_offsets("H4-const180-r1"):
            self.assertAlmostEqual(abs(off), 180.0, delta=0.25)
        h6 = self.fixes("H6-const90-cold-r1")
        self.assertEqual(h6, self.fixes("H3-const90-r1"))  # H6 differs from H3 only by the reset procedure

    def test_burst_is_four_seconds_of_plus_30_then_correct(self):
        offs = self.tunnel_offsets("H7-burst30-r1")
        for off in offs[:gen.BURST_S]:
            self.assertAlmostEqual(off, 30.0, delta=0.25)
        for off in offs[gen.BURST_S:]:
            self.assertAlmostEqual(off, 0.0, delta=0.25)

    def test_drift_and_standstill_form(self):
        tag = "H5-drift-stop-r1"
        plan = self.doc["runs"][tag]
        tunnel = self.phase(tag, "tunnel")
        lo = plan["tunnel"][0]
        s0, s1 = plan["stop"]
        self.assertEqual(s1 - s0 + 1, gen.STOP_S)
        before, stop, after = tunnel[:s0 - lo], tunnel[s0 - lo:s1 - lo + 1], tunnel[s1 - lo + 1:]
        # stopped near the middle of the tunnel
        mid_along, _ = tunnel_frame((float(stop[0][1]), float(stop[0][2])))
        self.assertAlmostEqual(mid_along, 2433.1 / 2, delta=30.0)
        last = before[-1]
        for f in stop:
            self.assertEqual((f[1], f[2]), (last[1], last[2]))  # same position
            self.assertEqual(float(f[5]), 0.0)  # speed 0
            self.assertEqual(f[6], last[6])  # the held bearing
            self.assertEqual(f[3], "40.000")
        moving = before + after
        offs = [angle_diff(float(f[6]), ROAD_TUNNEL) for f in moving]
        self.assertAlmostEqual(offs[0], 0.0, delta=0.1)
        steps = [b - a for a, b in zip(offs, offs[1:])]
        self.assertTrue(all(abs(s - gen.DRIFT_RATE) < 0.11 for s in steps), "linear drift per moving second, held when stopped")
        self.assertTrue(0.15 <= gen.DRIFT_RATE <= 0.3)
        self.assertAlmostEqual(offs[-1], gen.DRIFT_RATE * (len(moving) - 1), delta=0.2)
        self.assertGreater(offs[-1], 45.0)
        # without the stop the moving fixes are exactly the control's positions and speeds
        ref = self.phase("H0-correct-r1", "tunnel")
        self.assertEqual([f[:6] for f in moving], [f[:6] for f in ref])
        self.assertEqual(self.phase(tag, "exit"), self.phase("H0-correct-r1", "exit"))

    # --- order --------------------------------------------------------------------------------------------------
    def test_order_interleaves_and_brackets_with_the_control(self):
        order = self.doc["order"]
        cond = [t.rsplit("-r", 1)[0] for t in order]
        rep = [t.rsplit("-r", 1)[1] for t in order]
        self.assertEqual(order[0], "H0-correct-r1")
        self.assertEqual(order[-1], "H0-correct-r2")
        self.assertTrue(all(a != b for a, b in zip(cond, cond[1:])), "no condition twice in a row")
        self.assertEqual(rep, ["1"] * 8 + ["2"] * 8)  # every r1 before any r2
        self.assertNotEqual(cond[1:7], cond[9:15])
        self.assertNotEqual(cond[:8][1:-1], list(reversed(cond[8:]))[1:-1])
        self.assertNotIn(order.index("H6-const90-cold-r1"), (0, 1), "H6 is not confounded with the session start")


if __name__ == "__main__":
    unittest.main()
