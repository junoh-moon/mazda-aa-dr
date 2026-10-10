import filecmp
import importlib.util
import json
import math
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PLAN = ROOT / "validation" / "DHU_NAVER_EXP9_PLAN_2026-10-10"
PLAN8 = ROOT / "validation" / "DHU_NAVER_EXP8_PLAN_2026-10-10"
PLAN7 = ROOT / "validation" / "DHU_NAVER_EXP7_PLAN_2026-10-09"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load("dhu_exp9", PLAN / "gen_commands.py")
gen8 = load("dhu_exp8_for_exp9_test", PLAN8 / "gen_commands.py")
gen7 = load("dhu_exp7_for_exp9_test", PLAN7 / "gen_commands.py")

ENTRANCE = (35.1714322, 129.0250025)  # OSM node 731823096
EXIT = (35.1853302, 129.0043248)  # OSM node 731823121
ROAD_TUNNEL = "309.4"
SETUP_FIXES = 6
CONDITIONS = ["ST8-nostop", "AC20", "AC25", "AC30", "AC35", "CR3-40", "CR3-20", "DRIFT-STOP-15", "DRIFT-STOP-20", "STOPGO3-20"]
AC = {"AC20": "20.000", "AC25": "25.000", "AC30": "30.000", "AC35": "35.000"}
CRAWL = {"CR3-40": "40.000", "CR3-20": "20.000"}
DRIFT = {"DRIFT-STOP-15": "15.000", "DRIFT-STOP-20": "20.000"}
# Golden drift numbers at the stop start (last moving fix t=106, 86 DR seconds) and at the last tunnel fix (= exp7 R1).
DRIFT_STOP_GOLDEN = dict(heading_error_deg=12.9, cross_track_m=135.0, along_track_m=-10.1, error_m=135.3, accuracy_honest_m=189)


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


def pos(f):
    return float(f[1]), float(f[2])


class Exp9Commands(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(cls.tmp.name)
        cls.out = base / "a"
        cls.plan_path = base / "a.json"
        cls.route, cls.doc = gen.generate(str(cls.out), str(cls.plan_path))
        cls.files = {p.stem: p.read_text().splitlines() for p in cls.out.glob("*.txt")}
        _, cls.doc8 = gen8.generate(str(base / "exp8"))
        cls.files8 = {p.stem: p.read_text().splitlines() for p in (base / "exp8").glob("*.txt")}
        _, cls.doc7 = gen7.generate(str(base / "exp7"))
        cls.files7 = {p.stem: p.read_text().splitlines() for p in (base / "exp7").glob("*.txt")}

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    @staticmethod
    def locs(lines):
        return [line.split() for line in lines if line.startswith("location ")][SETUP_FIXES:]

    def fixes(self, tag):
        return self.locs(self.files[tag])

    def split(self, tag):
        """(before the first window, the window fixes, after the window) of a single-window run."""
        (s0, s1), = self.doc["runs"][tag]["window_spans"]
        f = self.fixes(tag)
        return f[:s0], f[s0:s1 + 1], f[s1 + 1:]

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

    def test_same_geometry_and_setup_as_experiments_7_and_8(self):
        for other in (PLAN8, PLAN7):
            self.assertEqual((PLAN / "geometry" / "route.json").read_bytes(), (other / "geometry" / "route.json").read_bytes())
            self.assertEqual((PLAN / "setup-template.txt").read_bytes(), (other / "setup-template.txt").read_bytes())
        self.assertAlmostEqual(self.route.exit - self.route.entrance, 2433.1, delta=0.5)
        self.assertEqual(self.doc["runs"]["AC20-r1"]["start"], [35.1697679, 129.0267566, 328.9])

    # --- controls and the exp8 base -----------------------------------------------------------------------------
    def test_st8_is_exactly_the_exp8_st8_command_file(self):
        for rep in (1, 2, 3):
            ours = "\n".join(self.files[f"ST8-nostop-r{rep}"]).replace(f"experiment9/ST8-nostop-r{rep}", "experiment8/ST8-nostop-r1")
            self.assertEqual(ours, "\n".join(self.files8["ST8-nostop-r1"]))
            self.assertEqual(self.doc["runs"][f"ST8-nostop-r{rep}"]["window_spans"], [])

    def test_ac_runs_differ_from_exp8_st2_only_in_the_stopped_accuracy(self):
        st2 = self.locs(self.files8["ST2-acc15-r1"])
        for base, acc in AC.items():
            ours = self.fixes(base + "-r1")
            self.assertEqual(len(ours), len(st2))
            changed = [i for i, (f, g) in enumerate(zip(ours, st2)) if f != g]
            self.assertEqual(changed, list(range(107, 137)), base)
            for i in changed:
                self.assertEqual(ours[i][:3] + ours[i][4:], st2[i][:3] + st2[i][4:], base)  # only field 3 (accuracy)
                self.assertEqual(ours[i][3], acc, base)
            self.assertEqual(self.doc["runs"][base + "-r1"]["window_spans"], [[107, 136]])
            # the photo schedule is the exp8 stopping schedule
            self.assertEqual({f["t"] for f in self.doc["runs"][base + "-r1"]["frames"]},
                             {f["t"] for f in self.doc8["runs"]["ST2-acc15-r1"]["frames"]})

    def test_only_the_window_differs_from_the_control(self):
        control = self.fixes("ST8-nostop-r1")
        for base in list(AC) + list(CRAWL):
            before, window, after = self.split(base + "-r1")
            self.assertEqual(before, control[:107], base)
            self.assertEqual(len(window), gen.STOP_S, base)
            if base in AC:  # a stop: the moving fixes are the control's fixes with the stop inserted
                self.assertEqual(after, control[107:], base)
        for base in CONDITIONS:
            for rep in range(2, gen.REPS.get(base, 2) + 1):
                self.assertEqual(self.fixes(f"{base}-r1"), self.fixes(f"{base}-r{rep}"), base)
        for tag, plan in self.doc["runs"].items():
            self.assertEqual(len(self.fixes(tag)), plan["duration_s"], tag)

    # --- accuracy per phase ---------------------------------------------------------------------------------------
    def test_accuracy_per_phase(self):
        for tag, plan in self.doc["runs"].items():
            base = tag.rsplit("-r", 1)[0]
            f = self.fixes(tag)
            spans = plan["window_spans"]
            inside = {t for s0, s1 in spans for t in range(s0, s1 + 1)}
            lo, hi = plan["tunnel"]
            ex0, ex1 = plan["exit"]
            for t, fix in enumerate(f):
                if t < 20 or ex0 <= t <= ex1:
                    self.assertEqual(fix[3], "5.000", (tag, t))
                elif t in inside:
                    want = {**AC, **CRAWL, **DRIFT, "STOPGO3-20": "20.000"}[base]
                    self.assertEqual(fix[3], want, (tag, t))
                else:
                    self.assertTrue(lo <= t <= hi)
                    self.assertEqual(fix[3], "40.000", (tag, t))  # product-like switching 40 -> X -> 40
                self.assertEqual(fix[4], "NAN")
            for fr in plan["frames"]:
                self.assertEqual(f"{fr['accuracy_m']:.3f}", f[fr["t"]][3], (tag, fr["t"]))

    # --- stops: identical true position, held correct bearing ----------------------------------------------------
    def test_true_position_stops_hold_the_last_fix(self):
        for tag in [b + "-r1" for b in AC] + ["STOPGO3-20-r1"]:
            f = self.fixes(tag)
            for s0, s1 in self.doc["runs"][tag]["window_spans"]:
                anchor = f[s0 - 1]
                self.assertEqual((anchor[3], anchor[5]), ("40.000", "14.000"), tag)
                for fix in f[s0:s1 + 1]:
                    self.assertEqual((fix[1], fix[2], fix[5], fix[6]), (anchor[1], anchor[2], "0.000", ROAD_TUNNEL), tag)
                self.assertEqual((f[s1 + 1][3], f[s1 + 1][5]), ("40.000", "14.000"), tag)

    def test_stop_and_go_has_three_15s_stops_with_20s_of_movement(self):
        plan = self.doc["runs"]["STOPGO3-20-r1"]
        self.assertEqual(plan["window_spans"], [[107, 121], [142, 156], [177, 191]])
        spans = plan["window_spans"]
        for (a0, a1), (b0, b1) in zip(spans, spans[1:]):
            self.assertEqual(b0 - a1 - 1, 20)
        self.assertEqual(plan["duration_s"], 222 + 45)
        f = self.fixes("STOPGO3-20-r1")
        control = self.fixes("ST8-nostop-r1")
        moving = [fix for t, fix in enumerate(f) if not any(s0 <= t <= s1 for s0, s1 in spans)]
        self.assertEqual(moving, control)  # the moving fixes are exactly the control's

    # --- crawl -----------------------------------------------------------------------------------------------------
    def test_crawl_moves_at_3_kmh_along_the_route(self):
        for base, acc in CRAWL.items():
            before, window, after = self.split(base + "-r1")
            a0, _ = tunnel_frame(pos(before[-1]))
            along = [tunnel_frame(pos(f))[0] - a0 for f in window]
            for k, x in enumerate(along):
                self.assertAlmostEqual(x, gen.CRAWL_MPS * (k + 1), delta=0.06, msg=base)
                self.assertLess(abs(tunnel_frame(pos(window[k]))[1]), 2.0)
            self.assertAlmostEqual(along[-1], 25.0, delta=0.1)
            self.assertTrue(all(f[5] == "0.833" and f[6] == ROAD_TUNNEL and f[3] == acc for f in window), base)
            # moving again at 14 m/s from the crawled truth position, so the exit comes 2 s earlier than in a stop run
            a1, _ = tunnel_frame(pos(after[0]))
            self.assertAlmostEqual(a1 - a0, 25.0 + 14.0, delta=0.1)
            self.assertEqual((after[0][3], after[0][5]), ("40.000", "14.000"))
            plan = self.doc["runs"][base + "-r1"]
            self.assertEqual((plan["exit"][0], plan["duration_s"]), (220, 250))
        self.assertEqual(self.split("CR3-40-r1")[0], self.split("CR3-20-r1")[0])
        diff = {i for f, g in zip(self.fixes("CR3-40-r1"), self.fixes("CR3-20-r1")) for i in range(7) if f[i] != g[i]}
        self.assertEqual(diff, {3})  # the two crawls differ in the accuracy only

    # --- drifted stop -----------------------------------------------------------------------------------------------
    def test_drift_stop_is_exp7_r1_with_a_stop_inserted(self):
        r1 = self.locs(self.files7["R1-dr-plus015-r1"])
        for base in DRIFT:
            f = self.fixes(base + "-r1")
            self.assertEqual(f[:107], r1[:107], base)
            self.assertEqual(f[137:], r1[107:], base)  # the drift continues as if the stop had not happened
        stop15 = self.split("DRIFT-STOP-15-r1")[1]
        stop20 = self.split("DRIFT-STOP-20-r1")[1]
        self.assertEqual({i for f, g in zip(stop15, stop20) for i in range(7) if f[i] != g[i]}, {3})

    def test_drift_stop_holds_the_drifted_position_and_bearing(self):
        for base, acc in DRIFT.items():
            before, window, after = self.split(base + "-r1")
            last = before[-1]
            for fix in window:
                self.assertEqual((fix[1], fix[2], fix[6]), (last[1], last[2], last[6]), base)
                self.assertEqual((fix[3], fix[5]), (acc, "0.000"), base)
            # 86 DR seconds at +0.15 deg/s: heading +12.9 deg, about 135 m to the right of the tunnel line
            self.assertAlmostEqual((float(last[6]) - float(ROAD_TUNNEL)), 12.9, delta=0.11)
            _, cross = tunnel_frame(pos(last))
            self.assertAlmostEqual(cross, 135.0, delta=3.0)
            self.assertGreater(cross, 120.0)

    def test_drift_numbers_in_the_plan(self):
        for base in DRIFT:
            s = self.doc["drift_summary"][base]
            self.assertEqual(s["rate_deg_s"], 0.15)
            stop = s["stop_start"]
            self.assertEqual((stop["t_last_moving"], stop["stop_start"], stop["dr_seconds"]), (106, 107, 86))
            for k, v in DRIFT_STOP_GOLDEN.items():
                self.assertEqual(stop[k], v, (base, k))
            end, r1_end = s["end_of_tunnel"], self.doc7["runs"]["R1-dr-plus015-r1"]["dr_end"]
            for k in ("dr_seconds", "heading_error_deg", "cross_track_m", "along_track_m", "error_m", "exit_jump_m"):
                self.assertEqual(end[k], r1_end[k], (base, k))
            self.assertEqual(end["t"], r1_end["t"] + 30)
            self.assertEqual((end["cross_track_m"], end["exit_jump_m"]), (527.0, 536.8))

    def test_honest_budget_follows_the_accuracy_rule(self):
        self.assertEqual(gen.honest_budget(20, 50 / 3.6 * 20, 50 / 3.6 * 20 ** 2 / 2), 40)  # rule doc: 50 km/h -> 40 m at 20 s
        for tag in ("AC20-r1", "DRIFT-STOP-20-r1"):
            self.assertEqual(self.doc["runs"][tag]["accuracy_honest_at_windows_m"], [[189, 198]])
        frames = {f["t"]: f for f in self.doc["runs"]["AC20-r1"]["frames"]}
        self.assertTrue(all(frames[t]["accuracy_honest_m"] >= 20 for t in frames if frames[t]["phase"] == "tunnel"))

    # --- photos, counts, tiers ----------------------------------------------------------------------------------
    def test_photo_schedule_around_windows_and_exits(self):
        h0 = {f["t"] for f in self.doc8["runs"]["ST8-nostop-r1"]["frames"]}
        for tag, plan in self.doc["runs"].items():
            ts = {f["t"] for f in plan["frames"]}
            ex = plan["exit"][0]
            self.assertTrue(set(range(ex, ex + 11, 2)) | set(gen.ENTRANCE_DENSE) <= ts, tag)
            self.assertIn(plan["duration_s"] - 1, ts)
            if not plan["window_spans"]:
                self.assertEqual(ts, h0, tag)
                continue
            for s0, s1 in plan["window_spans"]:
                self.assertTrue({s0, s0 + 2, s1, s1 + 3} <= ts, tag)
                grid = sorted(t for t in ts if s0 - 7 <= t <= s1 + 12)
                self.assertLessEqual(max(b - a for a, b in zip(grid, grid[1:])), 4, tag)
            grid = sorted(t for t in ts if ex - 36 <= t <= ex)
            self.assertLessEqual(max(b - a for a, b in zip(grid, grid[1:])), 4, tag)  # post-stop freeze window
            inside = {t for s0, s1 in plan["window_spans"] for t in range(s0, s1 + 1)}
            self.assertEqual({f["t"] for f in plan["frames"] if "window" in f}, ts & inside, tag)
        drift = {f["t"]: f for f in self.doc["runs"]["DRIFT-STOP-20-r1"]["frames"]}
        self.assertEqual(drift[107]["dr_cross_m"], 135.0)
        self.assertEqual(drift[136]["dr_cross_m"], 135.0)

    def test_run_counts_and_one_hertz_timeline(self):
        self.assertEqual(sorted(gen.RUNS), sorted(CONDITIONS))
        self.assertEqual(len(self.files), 9 * 2 + 3)
        self.assertEqual(sorted(self.doc["order"]), sorted(self.files))
        for tag, lines in self.files.items():
            plan = self.doc["runs"][tag]
            seconds = sum(1 for line in lines if line in ("sleep 1", "sleep 0.8")) - 5  # setup holds 5 x sleep 1
            self.assertEqual(seconds, plan["duration_s"], tag)
            shots = [line for line in lines if line.startswith("screenshot ")]
            self.assertTrue(all(s.startswith("screenshot ../../../../experiment9/" + tag) for s in shots))
            self.assertEqual(len(shots), len(plan["frames"]) + 1)

    def test_tiers_order_and_estimates(self):
        tiers = [t["runs"] for t in self.doc["tiers"]]
        self.assertEqual(tiers[0], ["ST8-nostop-r1", "AC25-r1", "AC20-r1", "DRIFT-STOP-20-r1", "CR3-40-r1"])
        self.assertEqual(tiers[1], ["AC30-r1", "DRIFT-STOP-15-r1", "CR3-20-r1", "STOPGO3-20-r1", "AC35-r1", "ST8-nostop-r2"])
        self.assertEqual(sorted({t.rsplit("-r", 1)[0] for t in tiers[0] + tiers[1]}), sorted(CONDITIONS))
        self.assertTrue(all(t.endswith("-r1") for t in tiers[0] + tiers[1][:-1]))
        self.assertEqual((tiers[0][0], tiers[1][-1], tiers[2][-1]), ("ST8-nostop-r1", "ST8-nostop-r2", "ST8-nostop-r3"))
        self.assertTrue(all(t.endswith("-r2") for t in tiers[2][:-1]))
        self.assertEqual(self.doc["tiers"][2]["always"], ["ST8-nostop-r3"])
        self.assertEqual(self.doc["order"], tiers[0] + tiers[1] + tiers[2])
        cond = [t.rsplit("-r", 1)[0] for t in self.doc["order"]]
        self.assertTrue(all(a != b for a, b in zip(cond, cond[1:])))
        self.assertEqual([(t["input_s"], t["photos"]) for t in self.doc["tiers"]], [(1228, 261), (1495, 341), (2501, 568)])
        self.assertEqual(self.doc["tiers"][0]["estimate_min"], [40, 45])
        self.assertEqual(self.doc["tiers"][1]["estimate_min"], [37, 43])
        for t in self.doc["tiers"]:
            self.assertEqual(t["input_s"], sum(self.doc["runs"][r]["duration_s"] for r in t["runs"]))
            self.assertEqual(t["photos"], sum(len(self.doc["runs"][r]["frames"]) + 1 for r in t["runs"]))


if __name__ == "__main__":
    unittest.main()
