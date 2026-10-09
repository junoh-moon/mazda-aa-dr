import filecmp
import importlib.util
import json
import math
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PLAN = ROOT / "validation" / "DHU_NAVER_EXP8_PLAN_2026-10-10"
PLAN7 = ROOT / "validation" / "DHU_NAVER_EXP7_PLAN_2026-10-09"
PLAN6 = ROOT / "validation" / "DHU_NAVER_EXP6_PLAN_2026-10-08"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load("dhu_exp8", PLAN / "gen_commands.py")
gen7 = load("dhu_exp7_for_exp8_test", PLAN7 / "gen_commands.py")

ENTRANCE = (35.1714322, 129.0250025)  # OSM node 731823096
EXIT = (35.1853302, 129.0043248)  # OSM node 731823121
ROAD_TUNNEL = "309.4"
SETUP_FIXES = 6
FORMS = ["ST0-current", "ST1-acc5", "ST2-acc15", "ST3-jitter-acc40", "ST4-jitter-acc5", "ST5-creep030-acc40",
         "ST6-slow005-jitter-acc40", "ST7-stockmode0"]
CONDITIONS = FORMS + ["ST8-nostop"]
# Expected stopped form: (accuracy field, speed field, position kind)
EXPECTED = {
    "ST0-current": ("40.000", "0.000", "hold"),
    "ST1-acc5": ("5.000", "0.000", "hold"),
    "ST2-acc15": ("15.000", "0.000", "hold"),
    "ST3-jitter-acc40": ("40.000", "0.000", "jitter"),
    "ST4-jitter-acc5": ("5.000", "0.000", "jitter"),
    "ST5-creep030-acc40": ("40.000", "0.300", "creep"),
    "ST6-slow005-jitter-acc40": ("40.000", "0.050", "jitter"),
    "ST7-stockmode0": ("NAN", "14.000", "hold"),
}


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


def dist(a, b):
    ax, ay = enu((float(a[1]), float(a[2])))
    bx, by = enu((float(b[1]), float(b[2])))
    return math.hypot(ax - bx, ay - by)


class Exp8Commands(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(cls.tmp.name)
        cls.out = base / "a"
        cls.plan_path = base / "a.json"
        cls.route, cls.doc = gen.generate(str(cls.out), str(cls.plan_path))
        cls.files = {p.stem: p.read_text().splitlines() for p in cls.out.glob("*.txt")}
        cls.out7 = base / "exp7"
        _, cls.doc7 = gen7.generate(str(cls.out7))
        cls.files7 = {p.stem: p.read_text().splitlines() for p in cls.out7.glob("*.txt")}

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    @staticmethod
    def locs(lines):
        return [line.split() for line in lines if line.startswith("location ")][SETUP_FIXES:]

    def fixes(self, tag):
        return self.locs(self.files[tag])

    def split(self, tag):
        """(before the stop, the stopped fixes, after the stop) of a stopping run."""
        s0, s1 = self.doc["runs"][tag]["stop"]
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

    def test_same_geometry_and_setup_as_experiments_6_and_7(self):
        for other in (PLAN7, PLAN6):
            self.assertEqual((PLAN / "geometry" / "route.json").read_bytes(), (other / "geometry" / "route.json").read_bytes())
            self.assertEqual((PLAN / "setup-template.txt").read_bytes(), (other / "setup-template.txt").read_bytes())
        self.assertAlmostEqual(self.route.exit - self.route.entrance, 2433.1, delta=0.5)
        self.assertEqual(self.doc["runs"]["ST0-current-r1"]["start"], [35.1697679, 129.0267566, 328.9])

    # --- the controls reproduce experiment 7 --------------------------------------------------------------------
    def test_st0_is_exactly_the_exp7_h8_input(self):
        self.assertEqual(self.fixes("ST0-current-r1"), self.locs(self.files7["H8-stop-correct-r1"]))
        self.assertEqual(self.doc["runs"]["ST0-current-r1"]["stop"], self.doc7["runs"]["H8-stop-correct-r1"]["stop"])
        self.assertEqual(self.doc["runs"]["ST0-current-r1"]["stop"], [107, 136])

    def test_st8_is_exactly_the_exp7_h0_command_file(self):
        ours = "\n".join(self.files["ST8-nostop-r1"]).replace("experiment8/ST8-nostop-r1", "experiment7/H0-correct-r1")
        self.assertEqual(ours, "\n".join(self.files7["H0-correct-r1"]))
        self.assertIsNone(self.doc["runs"]["ST8-nostop-r1"]["stop"])

    # --- only the stopped segment differs -----------------------------------------------------------------------
    def test_only_the_stopped_segment_differs_from_st0(self):
        ref_before, ref_stop, ref_after = self.split("ST0-current-r1")
        for base in FORMS:
            for rep in (1, 2):
                tag = f"{base}-r{rep}"
                before, stop, after = self.split(tag)
                self.assertEqual(before, ref_before, tag)
                self.assertEqual(after, ref_after, tag)
                self.assertEqual(len(stop), gen.STOP_S, tag)
                self.assertEqual(len(self.fixes(tag)), self.doc["runs"][tag]["duration_s"], tag)
            self.assertEqual(self.fixes(base + "-r1"), self.fixes(base + "-r2"), base)
        # the moving fixes of every stopping run are the positive control's fixes
        ref_before, _, ref_after = self.split("ST0-current-r1")
        self.assertEqual(ref_before + ref_after, self.fixes("ST8-nostop-r1"))
        self.assertEqual(self.fixes("ST8-nostop-r1"), self.fixes("ST8-nostop-r2"))

    def test_stopped_form_fields_per_condition(self):
        for base, (acc, spd, kind) in EXPECTED.items():
            before, stop, after = self.split(base + "-r1")
            anchor = before[-1]
            for f in stop:
                self.assertEqual(f[3], acc, base)
                self.assertEqual(f[4], "NAN", base)  # altitude
                self.assertEqual(f[5], spd, base)
                self.assertEqual(f[6], ROAD_TUNNEL, base)  # the bearing is held (correct) in every form
                _, cross = tunnel_frame((float(f[1]), float(f[2])))
                self.assertLess(abs(cross), 2.0, base)
            if kind == "hold":
                self.assertTrue(all((f[1], f[2]) == (anchor[1], anchor[2]) for f in stop), base)
            else:
                self.assertTrue(all((f[1], f[2]) != (anchor[1], anchor[2]) for f in stop), base)
            # the moving fixes around the stop are the tunnel form (accuracy 40, 14 m/s)
            self.assertEqual((anchor[3], anchor[5]), ("40.000", "14.000"))
            self.assertEqual((after[0][3], after[0][5]), ("40.000", "14.000"))

    def test_st7_reuses_the_exp6_stock_mode0_form(self):
        t0m = [line.split() for line in (PLAN6 / "commands" / "T0M-stockmode0-fast-r1.txt").read_text().splitlines()
               if line.startswith("location ")][SETUP_FIXES:]
        frozen = [f for f in t0m if f[3] == "NAN"]
        self.assertTrue(frozen)
        before, stop, _ = self.split("ST7-stockmode0-r1")
        for f, g in zip(stop, frozen):
            # same field shape: frozen position, accuracy NAN, altitude NAN, the last speed and the held bearing
            self.assertEqual([f[0], f[3], f[4], f[5]], [g[0], g[3], g[4], g[5]])
            self.assertEqual((f[1], f[2], f[6]), (before[-1][1], before[-1][2], before[-1][6]))

    # --- jitter and creep ---------------------------------------------------------------------------------------
    def test_jitter_is_bounded_deterministic_and_moves_every_second(self):
        walk = gen.jitter_walk(gen.STOP_S)
        self.assertEqual(walk, gen.jitter_walk(gen.STOP_S))
        self.assertNotEqual(walk, gen.jitter_walk(gen.STOP_S, seed=gen.JITTER_SEED + 1))
        self.assertEqual(gen.JITTER_RADIUS_M, 1.5)
        for e, n in walk:
            self.assertLessEqual(math.hypot(e, n), gen.JITTER_RADIUS_M + 1e-9)
        steps = [math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip([(0.0, 0.0)] + walk, walk)]
        self.assertTrue(all(gen.JITTER_STEP_M[0] - 1e-9 <= s <= gen.JITTER_STEP_M[1] + 1e-9 for s in steps))
        self.assertEqual(len({(round(e, 3), round(n, 3)) for e, n in walk}), gen.STOP_S)
        self.assertEqual([[round(e, 3), round(n, 3)] for e, n in walk], self.doc["jitter"]["offsets_en_m"])
        for base in ("ST3-jitter-acc40", "ST4-jitter-acc5", "ST6-slow005-jitter-acc40"):
            before, stop, _ = self.split(base + "-r1")
            anchor = before[-1]
            for a, b in zip(stop, stop[1:]):
                self.assertNotEqual((a[1], a[2]), (b[1], b[2]), base)
                self.assertGreater(dist(a, b), 0.2, base)  # 7-decimal coordinates keep every step visible
            for f in stop:
                self.assertLessEqual(dist(f, anchor), gen.JITTER_RADIUS_M + 0.05, base)
        # ST3, ST4 and ST6 share the jitter positions: ST3 vs ST4 = accuracy only, ST3 vs ST6 = speed only
        pos = lambda base: [(f[1], f[2]) for f in self.split(base + "-r1")[1]]
        self.assertEqual(pos("ST3-jitter-acc40"), pos("ST4-jitter-acc5"))
        self.assertEqual(pos("ST3-jitter-acc40"), pos("ST6-slow005-jitter-acc40"))

    def test_creep_moves_forward_at_the_reported_speed(self):
        before, stop, after = self.split("ST5-creep030-acc40-r1")
        a0, _ = tunnel_frame((float(before[-1][1]), float(before[-1][2])))
        along = [tunnel_frame((float(f[1]), float(f[2])))[0] - a0 for f in stop]
        for k, x in enumerate(along):
            self.assertAlmostEqual(x, 0.3 * (k + 1), delta=0.06)
        self.assertAlmostEqual(along[-1], 9.0, delta=0.1)
        a_next, _ = tunnel_frame((float(after[0][1]), float(after[0][2])))
        self.assertAlmostEqual(a_next - a0, 14.0, delta=0.1)  # moving again from the unchanged truth timeline

    def test_pairs_isolate_one_factor(self):
        stop = lambda base: self.split(base + "-r1")[1]
        diff = lambda a, b: {i for f, g in zip(stop(a), stop(b)) for i in range(7) if f[i] != g[i]}
        self.assertEqual(diff("ST0-current", "ST1-acc5"), {3})  # accuracy only
        self.assertEqual(diff("ST0-current", "ST2-acc15"), {3})
        self.assertEqual(diff("ST0-current", "ST3-jitter-acc40"), {1, 2})  # position only
        self.assertEqual(diff("ST1-acc5", "ST4-jitter-acc5"), {1, 2})
        self.assertEqual(diff("ST3-jitter-acc40", "ST4-jitter-acc5"), {3})
        self.assertEqual(diff("ST3-jitter-acc40", "ST6-slow005-jitter-acc40"), {5})  # speed only
        self.assertEqual(diff("ST0-current", "ST7-stockmode0"), {3, 5})

    # --- photos, counts, tiers ----------------------------------------------------------------------------------
    def test_photo_schedule(self):
        h0 = {f["t"] for f in self.doc7["runs"]["H0-correct-r1"]["frames"]}
        h8 = {f["t"] for f in self.doc7["runs"]["H8-stop-correct-r1"]["frames"]}
        dense = set(range(100, 151, 4)) | set(range(186, 219, 4))
        for tag, plan in self.doc["runs"].items():
            ts = {f["t"] for f in plan["frames"]}
            if tag.startswith("ST8"):
                self.assertEqual(ts, h0, tag)
                continue
            self.assertEqual(ts, h8 | dense, tag)
            self.assertTrue({107, 109, 136, 139} <= ts)
            self.assertTrue(set(range(222, 233, 2)) | set(gen.ENTRANCE_DENSE) <= ts)  # 2 s at exit and entrance
            gaps = sorted(t for t in ts if 100 <= t <= 150)
            self.assertLessEqual(max(b - a for a, b in zip(gaps, gaps[1:])), 4)
            gaps = sorted(t for t in ts if 186 <= t <= 222)
            self.assertLessEqual(max(b - a for a, b in zip(gaps, gaps[1:])), 4)
            stopped = [f for f in plan["frames"] if f["stopped"]]
            self.assertEqual({f["t"] for f in stopped}, {t for t in ts if 107 <= t <= 136})

    def test_run_counts_and_one_hertz_timeline(self):
        self.assertEqual(sorted(gen.RUNS), sorted(CONDITIONS))
        self.assertEqual(len(self.files), 18)
        self.assertEqual(sorted(self.doc["order"]), sorted(self.files))
        for tag, lines in self.files.items():
            plan = self.doc["runs"][tag]
            seconds = sum(1 for line in lines if line in ("sleep 1", "sleep 0.8")) - 5  # setup holds 5 x sleep 1
            self.assertEqual(seconds, plan["duration_s"], tag)
            self.assertEqual(plan["duration_s"], 222 if tag.startswith("ST8") else 252, tag)
            shots = [line for line in lines if line.startswith("screenshot ")]
            self.assertTrue(all(s.startswith("screenshot ../../../../experiment8/" + tag) for s in shots))
            self.assertEqual(len(shots), len(plan["frames"]) + 1)

    def test_tiers_order_and_estimates(self):
        tiers = [t["runs"] for t in self.doc["tiers"]]
        self.assertEqual(tiers[0], ["ST8-nostop-r1", "ST0-current-r1", "ST1-acc5-r1", "ST3-jitter-acc40-r1",
                                    "ST5-creep030-acc40-r1"])
        self.assertEqual(tiers[1], ["ST4-jitter-acc5-r1", "ST6-slow005-jitter-acc40-r1", "ST2-acc15-r1", "ST7-stockmode0-r1"])
        self.assertEqual(sorted(t.rsplit("-r", 1)[0] for t in tiers[0] + tiers[1]), sorted(CONDITIONS))
        self.assertTrue(all(t.endswith("-r1") for t in tiers[0] + tiers[1]))
        self.assertTrue(all(t.endswith("-r2") for t in tiers[2]))
        self.assertEqual((tiers[2][0], tiers[2][-1]), ("ST0-current-r2", "ST8-nostop-r2"))
        self.assertEqual(self.doc["tiers"][2]["always"], ["ST0-current-r2", "ST8-nostop-r2"])
        # the candidate r2s follow the preference order: honest-accuracy forms first, accuracy-lowered next, stock last
        self.assertEqual([t.rsplit("-r", 1)[0] for t in tiers[2][1:-1]],
                         ["ST3-jitter-acc40", "ST6-slow005-jitter-acc40", "ST5-creep030-acc40", "ST2-acc15",
                          "ST4-jitter-acc5", "ST1-acc5", "ST7-stockmode0"])
        self.assertEqual(self.doc["order"], tiers[0] + tiers[1] + tiers[2])
        cond = [t.rsplit("-r", 1)[0] for t in self.doc["order"]]
        self.assertTrue(all(a != b for a, b in zip(cond, cond[1:])))
        self.assertEqual([(t["input_s"], t["photos"]) for t in self.doc["tiers"]], [(1230, 262), (1008, 228), (2238, 490)])
        self.assertEqual(self.doc["tiers"][0]["estimate_min"], [40, 46])  # tier 1 alone fits the 35-45 min target
        self.assertEqual(self.doc["tiers"][1]["estimate_min"], [25, 29])
        self.assertEqual(self.doc["tiers"][2]["minimum_estimate_min"], [12, 14])
        for t in self.doc["tiers"]:
            self.assertEqual(t["input_s"], sum(self.doc["runs"][r]["duration_s"] for r in t["runs"]))
            self.assertEqual(t["photos"], sum(len(self.doc["runs"][r]["frames"]) + 1 for r in t["runs"]))


if __name__ == "__main__":
    unittest.main()
