import filecmp
import importlib.util
import json
import math
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PLAN = ROOT / "validation" / "DHU_NAVER_EXP10_PLAN_2026-10-11"
PLAN9 = ROOT / "validation" / "DHU_NAVER_EXP9_PLAN_2026-10-10"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load("dhu_exp10", PLAN / "gen_commands.py")
T = 278.7


def locs(lines):
    return [line.split() for line in lines if line.startswith("location ")]


class Exp10Commands(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(cls.tmp.name)
        cls.out, cls.plan_path = base / "a", base / "a.json"
        cls.route, cls.doc = gen.generate(str(cls.out), str(cls.plan_path))
        cls.files = {p.name[:-4]: p.read_text().splitlines() for p in cls.out.glob("*.txt")}

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

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

    def test_same_geometry_as_experiment_9_and_no_blind_taps(self):
        self.assertEqual((PLAN / "geometry" / "route.json").read_bytes(), (PLAN9 / "geometry" / "route.json").read_bytes())
        # v2: no command file taps; the agent resolves taps from the screen, the observed exp7 coordinates are only a fallback
        self.assertFalse(hasattr(gen, "TAP_SEARCH"))
        for name, lines in self.files.items():
            self.assertEqual([x for x in lines if x.startswith("tap ")], [], name)
        ui = json.loads(self.plan_path.read_text())["ui_fallback"]
        self.assertEqual((ui["search"], ui["route_start"], ui["recalc_popup_dismiss"]), ([600, 44], [345, 145], [170, 177]))
        site = self.doc["site"]
        self.assertEqual(site["true_bearing_deg"], T)
        self.assertGreater(site["after_tunnel_exit_m"], 100)
        for tag, plan in self.doc["runs"].items():
            self.assertLess(plan["end_m"], 3195.0, tag)  # inside the post-exit stretch exp7 verified

    def test_file_count_and_syntax(self):
        # 23 runs (RS as .b) + 2 RS part a + PRIME + 8 preparation files + 3 tier 0 files
        self.assertEqual(len(self.files), 23 + 2 + 1 + 8 + 3)
        for name, lines in self.files.items():
            for line in lines:
                w = line.split()
                self.assertIn(w[0], ("location", "sleep", "tap", "screenshot"), (name, line))
                if w[0] == "location":
                    self.assertEqual(len(w), 7)
                    float(w[1]), float(w[2]), float(w[3]), float(w[5])
                    self.assertEqual(w[4], "NAN")
                    if w[6] != "NAN":
                        self.assertTrue(0.0 <= float(w[6]) < 360.0)
                if w[0] == "screenshot":
                    self.assertTrue(w[1].startswith("../../../../experiment10/"))

    def test_one_hertz_timeline_and_photos(self):
        for tag, plan in self.doc["runs"].items():
            name = tag + ".b" if plan["reconnect"] else tag
            lines = self.files[name]
            self.assertEqual(len(locs(lines)), plan["duration_s"])
            self.assertEqual(sum(1 for x in lines if x in ("sleep 1", "sleep 0.8")), plan["duration_s"])
            shots = [x for x in lines if x.startswith("screenshot ")]
            self.assertEqual(len(shots), len(plan["frames"]))
            self.assertEqual(len(shots), 37)
            self.assertEqual(plan["prep"], None if plan["reconnect"] else "PREP-" + plan["base"])

    def test_conditions_differ_only_in_the_standstill(self):
        ref = locs(self.files["B2-r1"])
        want = {"B0": "NAN", "B1": "0.0", "B2": f"{T}", "B3": "98.7", "B4": "188.7", "B6": "335.0"}
        for tag, plan in self.doc["runs"].items():
            f = locs(self.files[tag + ".b" if plan["reconnect"] else tag])
            self.assertEqual(f[gen.MOVE_T:], ref[gen.MOVE_T:], tag)
            base = plan["base"]
            stand = f[:gen.MOVE_T]
            if base in ("B5", "B7"):
                b = [float(x[6]) for x in stand]
                if base == "B7":
                    self.assertEqual(set(b), {T})
                    b5 = locs(self.files["B5-r1"])[:gen.MOVE_T]
                    self.assertEqual([x[:6] for x in stand], [x[:6] for x in b5])  # same speed and wander as B5
                    b = [100.0]
                v = [float(x[5]) for x in stand]
                self.assertTrue(all(67.0 <= x <= 241.0 for x in b))
                self.assertTrue(all(0.28 <= x <= 0.56 for x in v))
                lat = [float(x[1]) for x in stand]
                lon = [float(x[2]) for x in stand]
                span = math.hypot((max(lat) - min(lat)) * 111195, (max(lon) - min(lon)) * 111195 * math.cos(math.radians(35.19)))
                self.assertLess(span, 19.0)
                self.assertGreater(span, 5.0)
                continue
            self.assertEqual({x[6] for x in stand}, {want[base]}, tag)
            self.assertEqual({(x[1], x[2]) for x in stand}, {(ref[0][1], ref[0][2])}, tag)
            if base == "B6":
                self.assertEqual({(x[3], x[5]) for x in stand}, {("8.800", "1.111")})
            else:
                acc = "5.000" if "-A5-" in tag else "15.000"
                self.assertEqual({(x[3], x[5]) for x in stand}, {(acc, "0.000")}, tag)

    def test_motion_ramp_true_course_and_neutral_tail(self):
        f = locs(self.files["B2-r1"])
        move = f[gen.MOVE_T:gen.MOVE_T + gen.MOVE_S]
        self.assertAlmostEqual(float(move[0][5]), 20 / 3.6 / 10, places=3)
        self.assertEqual(move[9][5], "5.556")
        self.assertTrue(all(x[3] == "15.000" for x in move))
        for fr in self.doc["runs"]["B2-r1"]["frames"]:
            if fr["phase"] == "move":
                self.assertEqual(fr["bearing_sent_deg"], fr["road_bearing_deg"])
        tail = f[gen.MOVE_T + gen.MOVE_S:]
        self.assertEqual(len(tail), gen.TAIL_S)
        self.assertTrue(all(x[5] == "0.000" and x[6] == "NAN" for x in tail))
        prime = locs(self.files["PRIME"])
        self.assertEqual(len(prime), gen.TAIL_S)
        self.assertLess(abs(float(prime[0][1]) - float(tail[0][1])) * 111195, 2.0)

    def test_reconnect_part_a_is_b2_with_guidance(self):
        for tag in ("RS-B1-r1", "RS-B3-r1"):
            a = self.files[tag + ".a"]
            self.assertEqual(len(locs(a)), gen.RS_A_S)
            self.assertEqual(locs(a), locs(self.files["B2-r1"])[:gen.RS_A_S])
            self.assertEqual([x for x in a if x.startswith("tap ")], [])

    def test_tiers(self):
        tiers = {t["tier"]: t for t in self.doc["tiers"]}
        self.assertEqual(tiers["1"]["runs"], ["B2-r1", "B3-r1", "B1-r1", "B5-r1", "B7-r1"])
        self.assertEqual(tiers["2"]["runs"], ["B0-r1", "B4-r1", "B6-r1", "B2-r2"])
        self.assertEqual(tiers["3"]["runs"][-1], "B2-r3")
        self.assertFalse(tiers["M"]["unattended"])
        self.assertTrue(all(self.doc["runs"][r]["manual"] for r in tiers["M"]["runs"]))
        self.assertTrue(all(not self.doc["runs"][r]["manual"] for r in self.doc["order"]))
        self.assertEqual(self.doc["order"], tiers["1"]["runs"] + tiers["2"]["runs"] + tiers["3"]["runs"])
        self.assertEqual(tiers["1"]["estimate_min"], [43, 48])
        total = sum(tiers[k]["estimate_min"][0] for k in ("1", "2", "3")) + self.doc["tier0"]["estimate_min"][0]
        self.assertLessEqual(total, self.doc["budget_min"])  # the low estimate of everything fits the night budget
        cond = [r.rsplit("-r", 1)[0] for r in self.doc["order"]]
        self.assertTrue(all(a != b for a, b in zip(cond, cond[1:])))


    def test_preparation_is_the_standstill_condition_at_5_m(self):
        for base in gen.BASES:
            prep = self.files["PREP-" + base]
            p = locs(prep)
            self.assertEqual(len(p), gen.PREP_S)
            self.assertEqual(sum(1 for x in prep if x == "sleep 1"), gen.PREP_S)
            self.assertFalse([x for x in prep if x.startswith(("tap ", "screenshot "))])
            self.assertEqual({x[3] for x in p}, {"5.000"}, base)
            body = locs(self.files[base + "-r1"])[:gen.MOVE_T]
            strip = lambda rows: [x[:3] + x[4:] for x in rows]
            self.assertEqual(strip(p[:gen.MOVE_T]), strip(body), base)  # same position, speed and bearing as the run

    def test_tier0_files(self):
        lat0, lon0 = self.doc["site"]["p0"]
        probe = locs(self.files["T0-LPROBE"])
        self.assertEqual([x[3] for x in probe], ["15.000"] * gen.LPROBE_S + ["5.000"] * gen.LPROBE_S)
        self.assertTrue(all(x[6] == "NAN" and x[5] == "0.000" for x in probe))
        self.assertEqual({(float(x[1]), float(x[2])) for x in probe}, {(lat0, lon0)})
        self.assertEqual(len([x for x in self.files["T0-LPROBE"] if x.startswith("screenshot ")]), 2)
        smoke = locs(self.files["T0-SMOKE-BODY"])
        self.assertEqual(smoke, locs(self.files["B2-r1"])[:gen.SMOKE_S])
        exp9 = locs(self.files["T0-EXP9-PREP"])
        e9 = (PLAN9 / "commands" / "ST8-nostop-r1.txt").read_text().split("\n")[0].split()
        self.assertEqual({(x[1], x[2], x[6]) for x in exp9}, {(e9[1], e9[2], e9[6])})
        self.assertEqual({x[3] for x in exp9}, {"5.000"})


if __name__ == "__main__":
    unittest.main()
