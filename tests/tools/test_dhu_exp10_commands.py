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
T = 328.9
TEMPLATE = (PLAN9 / "setup-template.txt").read_text().rstrip("\n").splitlines()
N_PRE = len(TEMPLATE)


def locs(lines):
    return [line.split() for line in lines if line.startswith("location ")]


def expected_preamble(tag, brg):
    start = "35.1697679 129.0267566"  # exp9 start fix
    out = []
    for x in TEMPLATE:
        out.append(x.replace("@START@", start).replace("@BRG@", brg).replace("@SHOT@", "../../../../experiment10/")
                   .replace("@TAG@", tag))
    return out


class Exp10Commands(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(cls.tmp.name)
        cls.out, cls.plan_path = base / "a", base / "a.json"
        cls.route, cls.doc = gen.generate(str(cls.out), str(cls.plan_path))
        cls.files = {p.name[:-4]: p.read_text().splitlines() for p in cls.out.glob("*.txt")}
        cls.json = json.loads(cls.plan_path.read_text())

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def body(self, tag):
        plan = self.doc["runs"][tag]
        return self.files[tag + ".b"] if plan["reconnect"] else self.files[tag][N_PRE:]

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
        self.assertEqual(json.loads((PLAN / "run-plan.json").read_text()), self.json)

    def test_proven_preamble_geometry_and_start_of_experiment_9(self):
        for name in ("setup-template.txt", "geometry/route.json"):
            self.assertEqual((PLAN / name).read_bytes(), (PLAN9 / name).read_bytes(), name)
        e9 = (PLAN9 / "commands" / "ST8-nostop-r1.txt").read_text().splitlines()
        self.assertEqual(self.files["T0-PRE"][:N_PRE], [x.replace("experiment9/ST8-nostop-r1", "experiment10/T0-PRE")
                                                         for x in e9[:N_PRE]])
        site = self.doc["site"]
        self.assertEqual((site["start"], site["true_bearing_deg"]), ([35.1697679, 129.0267566], T))
        for tag, plan in self.doc["runs"].items():
            self.assertLess(plan["end_m"], self.route.entrance, tag)  # the whole run stays before the tunnel

    def test_every_run_starts_with_the_template_only_the_bearing_differs(self):
        want = {"B0": "NAN", "B1": "0.0", "B2": f"{T}", "B3": "148.9", "B4": "238.9", "B6": "335.0", "B7": f"{T}"}
        for tag, plan in self.doc["runs"].items():
            if plan["reconnect"]:
                self.assertEqual(self.files[tag + ".a"][:N_PRE], expected_preamble(tag + "-a", f"{T}"))
                self.assertFalse([x for x in self.files[tag + ".b"] if x.startswith("tap ")])
                continue
            base = plan["base"]
            brg = want[base] if base in want else f"{plan['preamble_bearing_deg']:.1f}"
            self.assertEqual(self.files[tag][:N_PRE], expected_preamble(tag, brg), tag)
            self.assertEqual([x for x in self.files[tag] if x.startswith("tap ")], ["tap 200 50", "tap 345 145"])
            if base == "B5":
                self.assertEqual(float(brg), float(locs(self.body(tag))[0][6]))

    def test_file_count_and_syntax(self):
        # 26 runs (RS as .b) + 2 RS part a + PRIME + 8 fallback streams + T0-PRE + T0-COLD + T0-LPROBE
        self.assertEqual(len(self.files), 26 + 2 + 1 + 8 + 3)
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

    def test_one_hertz_body_and_photos(self):
        for tag, plan in self.doc["runs"].items():
            lines = self.body(tag)
            self.assertEqual(len(locs(lines)), plan["duration_s"])
            self.assertEqual(sum(1 for x in lines if x in ("sleep 1", "sleep 0.8")), plan["duration_s"])
            shots = [x for x in lines if x.startswith("screenshot ")]
            self.assertEqual(len(shots), len(plan["frames"]))
            self.assertEqual(len(shots), 37)
            self.assertFalse([x for x in lines if x.startswith("tap ")])

    def test_conditions_differ_only_in_the_standstill(self):
        ref = locs(self.body("B2-r1"))
        want = {"B0": "NAN", "B1": "0.0", "B2": f"{T}", "B3": "148.9", "B4": "238.9", "B6": "335.0"}
        for tag, plan in self.doc["runs"].items():
            f = locs(self.body(tag))
            self.assertEqual(f[gen.MOVE_T:], ref[gen.MOVE_T:], tag)
            base = plan["base"]
            stand = f[:gen.MOVE_T]
            if base in ("B5", "B7"):
                b = [float(x[6]) for x in stand]
                if base == "B7":
                    self.assertEqual(set(b), {T})
                    b5 = locs(self.body("B5-r1"))[:gen.MOVE_T]
                    self.assertEqual([x[:6] for x in stand], [x[:6] for x in b5])
                    b = [100.0]
                v = [float(x[5]) for x in stand]
                self.assertTrue(all(67.0 <= x <= 241.0 for x in b))
                self.assertTrue(all(0.28 <= x <= 0.56 for x in v))
                lat = [float(x[1]) for x in stand]
                lon = [float(x[2]) for x in stand]
                span = math.hypot((max(lat) - min(lat)) * 111195, (max(lon) - min(lon)) * 111195 * math.cos(math.radians(35.17)))
                self.assertLess(span, 19.0)
                self.assertGreater(span, 5.0)
                continue
            self.assertEqual({x[6] for x in stand}, {want[base]}, tag)
            self.assertEqual({(x[1], x[2]) for x in stand}, {(ref[0][1], ref[0][2])}, tag)
            if base == "B6":
                self.assertEqual({(x[3], x[5]) for x in stand}, {("8.800", "1.111")})
            else:
                acc = "15.000" if "-A15-" in tag else "5.000"
                self.assertEqual({(x[3], x[5]) for x in stand}, {(acc, "0.000")}, tag)

    def test_motion_ramp_true_course_and_neutral_tail(self):
        f = locs(self.body("B2-r1"))
        move = f[gen.MOVE_T:gen.MOVE_T + gen.MOVE_S]
        self.assertAlmostEqual(float(move[0][5]), 20 / 3.6 / 10, places=3)
        self.assertEqual(move[9][5], "5.556")
        self.assertTrue(all(x[3] == "5.000" for x in move))
        for fr in self.doc["runs"]["B2-r1"]["frames"]:
            if fr["phase"] == "move":
                self.assertEqual(fr["bearing_sent_deg"], fr["road_bearing_deg"])
        tail = f[gen.MOVE_T + gen.MOVE_S:]
        self.assertEqual(len(tail), gen.TAIL_S)
        self.assertTrue(all(x[5] == "0.000" and x[6] == "NAN" for x in tail))
        prime = locs(self.files["PRIME"])
        self.assertEqual(len(prime), gen.TAIL_S)
        self.assertLess(abs(float(prime[0][1]) - float(tail[0][1])) * 111195, 2.0)

    def test_reconnect_part_a_is_b2_with_the_preamble(self):
        for tag in ("RS-B1-r1", "RS-B3-r1"):
            a = self.files[tag + ".a"]
            self.assertEqual(len(locs(a[N_PRE:])), gen.RS_A_S)
            self.assertEqual(locs(a[N_PRE:]), locs(self.body("B2-r1"))[:gen.RS_A_S])

    def test_fallback_streams_and_tier0(self):
        for base in gen.BASES:
            fb = locs(self.files["FB-" + base])
            self.assertEqual(len(fb), gen.FB_S)
            self.assertEqual({tuple(x) for x in fb}, {tuple(locs(self.files[base + "-r1"])[0])}, base)  # the preamble fix
            self.assertFalse([x for x in self.files["FB-" + base] if x.startswith(("tap ", "screenshot "))])
        extra = locs(self.files["T0-PRE"][N_PRE:])
        self.assertEqual(len(extra), gen.T0_EXTRA_S)
        self.assertEqual(extra, locs(self.body("B2-r1"))[:gen.T0_EXTRA_S])
        self.assertEqual(locs(self.files["T0-COLD"]), locs(self.files["T0-PRE"]))
        self.assertEqual(self.files["T0-COLD"][:N_PRE], expected_preamble("T0-COLD", f"{T}"))
        probe = locs(self.files["T0-LPROBE"])
        self.assertEqual([x[3] for x in probe], ["15.000"] * gen.LPROBE_S + ["5.000"] * gen.LPROBE_S)
        self.assertTrue(all(x[6] == "NAN" for x in probe))

    def test_tiers(self):
        tiers = {t["tier"]: t for t in self.doc["tiers"]}
        self.assertEqual(tiers["1"]["runs"], ["B2-r1", "B3-r1", "B1-r1", "B5-r1", "B7-r1"])
        self.assertEqual(tiers["2"]["runs"], ["B0-r1", "B4-r1", "B6-r1", "B2-r2"])
        self.assertEqual(tiers["3"]["runs"][-1], "B2-r3")
        self.assertFalse(tiers["M"]["unattended"])
        self.assertTrue(all(self.doc["runs"][r]["manual"] for r in tiers["M"]["runs"]))
        self.assertTrue(all(not self.doc["runs"][r]["manual"] for r in self.doc["order"]))
        self.assertEqual(self.doc["order"], tiers["1"]["runs"] + tiers["2"]["runs"] + tiers["3"]["runs"])
        self.assertEqual(tiers["1"]["estimate_min"], [38, 43])
        total = sum(tiers[k]["estimate_min"][0] for k in ("1", "2", "3")) + self.doc["tier0"]["estimate_min"][0]
        self.assertLessEqual(total, self.doc["budget_min"])
        self.assertTrue(all(self.doc["runs"][r]["cold"] == ("-cold-" in r) for r in self.doc["runs"]))
        cond = [r.rsplit("-r", 1)[0] for r in self.doc["order"]]
        self.assertTrue(all(a != b for a, b in zip(cond, cond[1:])))


if __name__ == "__main__":
    unittest.main()
