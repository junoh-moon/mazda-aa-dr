import filecmp
import importlib.util
import json
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PLAN = ROOT / "validation" / "DHU_NAVER_EXP13_PLAN_2026-10-12"
PLAN12 = ROOT / "validation" / "DHU_NAVER_EXP12_PLAN_2026-10-12"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load("dhu_exp13", PLAN / "gen_commands.py")


def lines(path):
    return path.read_text().split("\n")[:-1]


def dircmp_clean(a, b):
    c = filecmp.dircmp(a, b)
    return c.left_only + c.right_only + c.diff_files


class Exp13Commands(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.plan = json.loads((PLAN / "run-plan.json").read_text())
        cls.doc, cls.r, cls.m = gen.load_routes()

    def test_regeneration_matches_committed_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "commands"
            plan_path = pathlib.Path(tmp) / "run-plan.json"
            gen.generate(str(out), str(plan_path))
            for sub in ("", "prep", "prep-busan"):
                self.assertEqual(dircmp_clean(out / sub, PLAN / "commands" / sub), [], sub)
                for p in (out / sub).iterdir():
                    if p.is_file():
                        self.assertEqual(p.read_bytes(), (PLAN / "commands" / sub / p.name).read_bytes(), p.name)
            self.assertEqual(plan_path.read_bytes(), (PLAN / "run-plan.json").read_bytes())

    def test_busan_files_are_byte_copies_or_retags_of_exp12(self):
        for p in (PLAN / "commands" / "prep-busan").iterdir():
            self.assertEqual(p.read_bytes(), (PLAN12 / "commands" / "prep" / p.name).read_bytes(), p.name)
        for new, old in (("T0-E13B", "T0-E12"), ("V4REF-E13", "V4REF-r1")):
            a, b = lines(PLAN12 / "commands" / f"{old}.txt"), lines(PLAN / "commands" / f"{new}.txt")
            self.assertEqual([x.replace(f"experiment11/{new}-", f"experiment11/{old}-") for x in b], a)

    def test_seoul_prep_differs_from_b2_only_in_the_origin_line(self):
        origin = gen.origin_line(self.r)
        for new, old in gen.SEOUL_PREP.items():
            a = lines(PLAN12 / "commands" / "prep" / f"{old}.txt")
            b = lines(PLAN / "commands" / "prep" / f"{new}.txt")
            self.assertEqual(len(a), len(b), new)
            n = 0
            for x, y in zip(a, b):
                if x != y:
                    self.assertEqual((x, y), (gen.BUSAN_ORIGIN_LINE, origin))
                    n += 1
            self.assertGreaterEqual(n, 1, new)
        start = lines(PLAN / "commands" / "prep" / "S-start.txt")
        self.assertEqual(start, lines(PLAN / "commands" / "prep" / "S-route-y130.txt")[2:])
        for name in ("search-again", "end-guidance"):
            self.assertEqual((PLAN / "commands" / "prep" / f"{name}.txt").read_bytes(),
                             (PLAN12 / "commands" / "prep" / f"{name}.txt").read_bytes())

    def test_seoul_t0_differs_from_exp12_t0_only_in_origin_and_tag(self):
        a = lines(PLAN12 / "commands" / "T0-E12.txt")
        lat, lon, road = self.r.at(self.r.fork - gen.ORIGIN_BEFORE_FORK_M)
        for tag in ("T0-S13", "T0-S13S2"):
            b = lines(PLAN / "commands" / f"{tag}.txt")
            self.assertEqual(len(a), len(b))
            for x, y in zip(a, b):
                if x.startswith("location "):
                    self.assertEqual(y, f"location {lat:.7f} {lon:.7f} 1.000 NAN 0.000 {road:.1f}")
                elif x.startswith("screenshot "):
                    self.assertEqual(x.replace("T0-E12-", f"{tag}-"), y)
                else:
                    self.assertEqual(x, y)

    def test_geometry_is_an_in_tunnel_fork_with_growing_separation(self):
        ways = {w["way"]: w for k in ("R", "M") for w in self.doc["routes"][k]["ways"]}
        for wid in (862460944, 1450944243, 862460946, 931083420):  # approach and both first branch ways are tunnel ways
            self.assertEqual(ways[wid]["tunnel"], "yes", wid)
        self.assertEqual(self.r.fork, self.m.fork)
        sep = self.plan["geometry"]["branch_separation_m_by_metres_after_fork"]
        vals = [sep[k] for k in ("25", "50", "80", "120", "150", "200")]
        self.assertEqual(vals, sorted(vals))
        self.assertLess(sep["50"], 15.0)
        self.assertGreater(sep["150"], 2 * 13.0)  # X = 15 is past half the separation only for the first ~140 m

    def test_runs_differ_only_in_accuracy_and_offset(self):
        base = None
        for tag, (acc, x, _, _) in gen.RUNS.items():
            body = [y for y in lines(PLAN / "commands" / f"{tag}.txt") if not y.startswith("screenshot ")]
            locs = [y.split(" ") for y in body if y.startswith("location ")]
            self.assertEqual(len(locs), gen.APPROACH_N + gen.EPISODE_S + gen.REC_S)
            for t, w in enumerate(locs):
                self.assertEqual(w[4:], ["NAN", "20.000", w[6]])
                ep = gen.APPROACH_N <= t < gen.APPROACH_N + gen.EPISODE_S
                self.assertEqual(w[3], f"{acc:.3f}" if ep else "1.000", (tag, t))
            sleeps = [y for y in body if not y.startswith("location ")]
            if base is None:
                base = (sleeps, [w[6] for w in locs])
            self.assertEqual((sleeps, [w[6] for w in locs]), base, tag)  # same timing and bearings in every run

    def test_offsets_and_side(self):
        p = self.plan["runs"]
        for tag, (acc, x, _, _) in gen.RUNS.items():
            ep = [f for f in p[tag]["frames"] if f["phase"] == "episode"]
            self.assertEqual(max(f["offset_left_m"] for f in ep), x)
            after = [f for f in ep if f["from_fork_m"] > 0]
            for f in after:
                self.assertAlmostEqual(f["dist_to_R_m"], x, delta=1.0)
            if x == 0:
                self.assertEqual(p[tag]["closer_to_M_photos"], [])
            else:
                self.assertTrue(p[tag]["closer_to_M_photos"])  # the offset is toward the left branch M
        self.assertLess(15.0, self.plan["geometry"]["valid_route_distance_m"]["2"])
        self.assertGreater(28.0, self.plan["geometry"]["off_route_m"]["2"])

    def test_episode_and_recovery_stay_on_mapped_route_and_fork_inside(self):
        p = self.plan["runs"]["R25X0-r1"]
        fr = {f["t"]: f for f in p["frames"]}
        self.assertEqual(fr[p["fork_t"]]["from_fork_m"], 0.0)
        self.assertLess(fr[gen.APPROACH_N]["from_fork_m"], -1000.0)
        self.assertIn(p["fork_t"], fr)

    def test_plan_frames_match_command_files(self):
        for tag, r in self.plan["runs"].items():
            body = lines(PLAN / "commands" / f"{tag}.txt")
            locs = [x.split(" ") for x in body if x.startswith("location ")]
            shots = [x.split("/")[-1] for x in body if x.startswith("screenshot ")]
            self.assertEqual([f["screenshot"] for f in r["frames"]], shots)
            for f in r["frames"]:
                self.assertEqual(float(locs[f["t"]][3]), f["accuracy_m"], (tag, f["t"]))

    def test_time_budget(self):
        est, budget = self.plan["cumulative_estimate_min"], self.plan["budget_min"]
        for s, req in self.plan["required"].items():
            for tag in req:
                self.assertLessEqual(est[tag], budget - 5, tag)
            self.assertEqual(req, self.plan["sessions"][s][: len(req)])


if __name__ == "__main__":
    unittest.main()
