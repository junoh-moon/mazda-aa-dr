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
              "H7-burst30", "H8-stop-correct", "R0-dr-plus004", "R1-dr-plus015", "R2-dr-minus015", "R3-dr-plus030"]
DR_RATES = {"R0-dr-plus004": 0.04, "R1-dr-plus015": 0.15, "R2-dr-minus015": -0.15, "R3-dr-plus030": 0.30}
# Golden end-of-tunnel numbers (last tunnel fix, 171 s of DR at 14 m/s): heading, cross (right +), along, exit jump.
DR_GOLDEN = {"R0-dr-plus004": (6.8, 142.7, -5.7, 145.7), "R1-dr-plus015": (25.6, 527.0, -79.2, 536.8),
             "R2-dr-minus015": (-25.6, -527.0, -79.2, 533.5), "R3-dr-plus030": (51.3, 1002.0, -307.3, 1053.8)}
STOPPING = ("H5", "H8")


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
        self.assertEqual(len(self.files), 27)  # 13 conditions x 2, plus the closing control H0-r3
        self.assertEqual(sorted(self.doc["order"]), sorted(self.files))
        for tag, lines in self.files.items():
            plan = self.doc["runs"][tag]
            seconds = sum(1 for line in lines if line in ("sleep 1", "sleep 0.8")) - 5  # setup holds 5 x sleep 1
            self.assertEqual(seconds, plan["duration_s"], tag)
            self.assertEqual(len(self.fixes(tag)), plan["duration_s"], tag)  # a fix every second, never a gap
            shots = [line for line in lines if line.startswith("screenshot ")]
            self.assertTrue(all(s.startswith("screenshot ../../../../experiment7/" + tag) for s in shots))
            self.assertEqual(len(shots), len(plan["frames"]) + 1)
            expected = 222 + (gen.STOP_S if tag.startswith(STOPPING) else 0)
            self.assertEqual(plan["duration_s"], expected, tag)

    def test_photo_schedule_is_the_exp6_schedule_plus_the_dense_entrance(self):
        exp6 = {f["t"] for f in self.doc6["runs"]["T1-ideal-a40-r1"]["frames"]}
        for tag, plan in self.doc["runs"].items():
            ts = {f["t"] for f in plan["frames"]}
            if tag.startswith(STOPPING):
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
            if tag.startswith(STOPPING + ("R",)):
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
                if tag.startswith("R"):
                    self.assertEqual(f[3], "40.000", tag)
                    continue
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

    # --- H8: the standstill of H5 with the correct bearing --------------------------------------------------------
    def test_h8_is_the_h5_standstill_with_the_correct_bearing(self):
        h5, h8 = self.fixes("H5-drift-stop-r1"), self.fixes("H8-stop-correct-r1")
        self.assertEqual([f[:6] for f in h5], [f[:6] for f in h8])  # same positions, speeds (incl. the 30 s of 0) and accuracy
        self.assertEqual(self.doc["runs"]["H8-stop-correct-r1"]["stop"], self.doc["runs"]["H5-drift-stop-r1"]["stop"])
        plan = self.doc["runs"]["H8-stop-correct-r1"]
        s0, s1 = plan["stop"]
        lo = plan["tunnel"][0]
        tunnel = self.phase("H8-stop-correct-r1", "tunnel")
        stop = tunnel[s0 - lo:s1 - lo + 1]
        self.assertEqual(len(stop), gen.STOP_S)
        for f in stop:
            self.assertEqual((f[1], f[2]), (tunnel[s0 - lo - 1][1], tunnel[s0 - lo - 1][2]))
            self.assertEqual(float(f[5]), 0.0)
            self.assertEqual(f[3], "40.000")
        for off in self.tunnel_offsets("H8-stop-correct-r1"):
            self.assertAlmostEqual(off, 0.0, delta=0.25)  # correct bearing while moving AND while stopped
        self.assertEqual(self.phase("H8-stop-correct-r1", "exit"), self.phase("H5-drift-stop-r1", "exit"))
        # the moving fixes are exactly the control's
        moving = tunnel[:s0 - lo] + tunnel[s1 - lo + 1:]
        self.assertEqual(moving, self.phase("H0-correct-r1", "tunnel"))
        # H5 keeps its designed semantics: held offset +25.8 deg at the stop, +51.3 deg at the last tunnel fix
        h5plan = self.doc["runs"]["H5-drift-stop-r1"]
        held = {f["bearing_offset_deg"] for f in h5plan["frames"] if f["stopped"]}
        self.assertEqual(held, {25.8})
        self.assertAlmostEqual(self.tunnel_offsets("H5-drift-stop-r1")[-1], 51.3, delta=0.1)

    # --- R: realistic dead reckoning (heading error and position drift coupled) ------------------------------------
    def test_dr_first_tunnel_fix_is_exact_and_approach_exit_are_the_control(self):
        ref = self.fixes("H0-correct-r1")
        for base in DR_RATES:
            for rep in (1, 2):
                tag = f"{base}-r{rep}"
                self.assertEqual(self.phase(tag, "approach"), self.phase("H0-correct-r1", "approach"), tag)
                self.assertEqual(self.phase(tag, "exit"), self.phase("H0-correct-r1", "exit"), tag)
                tun, ref_tun = self.phase(tag, "tunnel"), self.phase("H0-correct-r1", "tunnel")
                self.assertEqual(tun[0], ref_tun[0], tag)  # DR time 0 = the exact first tunnel fix (position and bearing)
                self.assertEqual(len(self.fixes(tag)), len(ref))
                self.assertTrue(all(f[3] == "40.000" and f[5] == "14.000" for f in tun), tag)
            self.assertEqual(self.fixes(base + "-r1"), self.fixes(base + "-r2"))

    def test_zero_error_integrates_to_the_true_route(self):
        ref, _ = gen.build_run(self.route, "Z", dict(bearing=None))
        lines, plan = gen.build_run(self.route, "Z", dict(dr=0.0))
        self.assertEqual(lines, ref)
        self.assertEqual(plan["dr_end"]["error_m"], 0.0)
        de, dn = gen.dr_error_step(self.route, self.route.entrance + 100.0, 14.0, 50, 0.0)
        self.assertEqual((de, dn), (0.0, 0.0))

    def test_bearing_is_true_plus_eps(self):
        for base, rate in DR_RATES.items():
            tun = self.phase(base + "-r1", "tunnel")
            for tau, f in enumerate(tun):
                self.assertAlmostEqual(angle_diff(float(f[6]), ROAD_TUNNEL), rate * tau, delta=0.11, msg=(base, tau))
            frames = [x for x in self.doc["runs"][base + "-r1"]["frames"] if x["phase"] == "tunnel"]
            for x in frames:
                self.assertAlmostEqual(x["bearing_offset_deg"], round(rate * x["dr_seconds"], 1), delta=0.051)

    def test_dr_path_matches_the_closed_form_and_the_golden_numbers(self):
        v = 14.0
        for base, rate in DR_RATES.items():
            tag = base + "-r1"
            tun, ref_tun = self.phase(tag, "tunnel"), self.phase("H0-correct-r1", "tunnel")
            w = math.radians(rate)
            for tau in (0, 30, 85, len(tun) - 1):
                along, cross = tunnel_frame((float(tun[tau][1]), float(tun[tau][2])))
                t_along, t_cross = tunnel_frame((float(ref_tun[tau][1]), float(ref_tun[tau][2])))
                # straight tunnel: cross = v (1 - cos(w T)) / w (right of travel for w > 0), along = v sin(w T) / w - v T
                self.assertAlmostEqual(cross - t_cross, v * (1 - math.cos(w * tau)) / w, delta=1.0, msg=(tag, tau))
                self.assertAlmostEqual(along - t_along, v * math.sin(w * tau) / w - v * tau, delta=1.0, msg=(tag, tau))
            heading, cross, along, jump = DR_GOLDEN[base]
            end = self.doc["runs"][tag]["dr_end"]
            self.assertEqual(end["dr_seconds"], len(tun) - 1)
            self.assertEqual(end["t"], self.doc["runs"][tag]["tunnel"][1])
            self.assertAlmostEqual(end["heading_error_deg"], heading, delta=0.05)
            self.assertAlmostEqual(end["cross_track_m"], cross, delta=0.15)
            self.assertAlmostEqual(end["along_track_m"], along, delta=0.15)
            self.assertAlmostEqual(end["exit_jump_m"], jump, delta=0.15)
            self.assertEqual(self.doc["dr_summary"][base]["end_of_tunnel"], end)
        # sign conventions: +e drifts right, -e drifts left by the same amount, along shortfall is symmetric
        r1, r2 = self.doc["runs"]["R1-dr-plus015-r1"]["dr_end"], self.doc["runs"]["R2-dr-minus015-r1"]["dr_end"]
        self.assertGreater(r1["cross_track_m"], 0)
        self.assertLess(r2["cross_track_m"], 0)
        self.assertAlmostEqual(r1["cross_track_m"], -r2["cross_track_m"], delta=0.15)
        self.assertEqual(r1["along_track_m"], r2["along_track_m"])
        self.assertEqual(r1["heading_error_deg"], -r2["heading_error_deg"])
        # R3 ends with the same heading error as H5 (bearing only) but with the coupled position drift
        self.assertEqual(self.doc["runs"]["R3-dr-plus030-r1"]["dr_end"]["heading_error_deg"], 51.3)
        # the 30 km/h reference is longer and worse, and never sent
        ref = self.doc["dr_summary"]["R1-dr-plus015"]["slow_profile_reference"]
        self.assertGreater(ref["dr_seconds"], 171)
        self.assertGreater(ref["cross_track_m"], r1["cross_track_m"])

    def test_dr_frames_report_the_error_of_the_sent_position(self):
        for base in DR_RATES:
            tag = base + "-r1"
            fixes = self.fixes(tag)
            for x in self.doc["runs"][tag]["frames"]:
                if x["phase"] != "tunnel":
                    self.assertNotIn("dr_error_m", x)
                    continue
                f, g = fixes[x["t"]], self.fixes("H0-correct-r1")[x["t"]]
                along, cross = tunnel_frame((float(f[1]), float(f[2])))
                t_along, t_cross = tunnel_frame((float(g[1]), float(g[2])))
                self.assertAlmostEqual(cross - t_cross, x["dr_cross_m"], delta=1.0, msg=(tag, x["t"]))
                self.assertAlmostEqual(along - t_along, x["dr_along_m"], delta=1.0, msg=(tag, x["t"]))
                self.assertAlmostEqual(x["sent_progress_m"], x["truth_progress_m"] + x["dr_along_m"], delta=0.11)

    # --- tiers and order ----------------------------------------------------------------------------------------
    def test_tiers_order_and_counts(self):
        tiers = [t["runs"] for t in self.doc["tiers"]]
        self.assertEqual(tiers[0], ["H0-correct-r1", "R1-dr-plus015-r1", "R3-dr-plus030-r1", "H3-const90-r1",
                                    "R2-dr-minus015-r1", "H4-const180-r1", "H0-correct-r2"])
        self.assertEqual(tiers[1], ["H1-const20-r1", "H2-const45-r1", "H7-burst30-r1", "H5-drift-stop-r1",
                                    "H6-const90-cold-r1", "H8-stop-correct-r1", "R0-dr-plus004-r1"])
        self.assertEqual(self.doc["order"], tiers[0] + tiers[1] + tiers[2])
        self.assertEqual(sorted(self.doc["order"]), sorted(self.files))
        self.assertEqual(len(set(self.doc["order"])), 27)
        # every condition's r1 is in tier 1 or 2; tier 3 holds every r2 except the tier-1 closing control, and ends with H0-r3
        first_two = tiers[0] + tiers[1]
        self.assertEqual(sorted(c for c in CONDITIONS), sorted({t.rsplit("-r", 1)[0] for t in first_two}))
        self.assertTrue(all(t.endswith("-r1") for t in first_two if t != "H0-correct-r2"))
        self.assertTrue(all(t.endswith("-r2") for t in tiers[2][:-1]))
        self.assertEqual(tiers[2][-1], "H0-correct-r3")
        self.assertEqual(len(tiers[2]), 13)
        cond = [t.rsplit("-r", 1)[0] for t in self.doc["order"]]
        self.assertTrue(all(a != b for a, b in zip(cond, cond[1:])), "no condition twice in a row")
        self.assertNotIn(self.doc["order"].index("H6-const90-cold-r1"), (0, 1), "H6 is not confounded with the session start")
        # tier sizes, input time, photos and the estimate (1-1.5 min per run, 2 min per H6 reset, 10 min environment in tier 1)
        self.assertEqual([(t["input_s"], t["photos"]) for t in self.doc["tiers"]], [(1554, 238), (1614, 252), (2946, 456)])
        self.assertEqual([t["estimate_min"] for t in self.doc["tiers"]], [[43, 46], [36, 39], [64, 71]])
        for t in self.doc["tiers"]:
            self.assertEqual(t["input_s"], sum(self.doc["runs"][r]["duration_s"] for r in t["runs"]))
            self.assertEqual(t["photos"], sum(len(self.doc["runs"][r]["frames"]) + 1 for r in t["runs"]))


if __name__ == "__main__":
    unittest.main()
