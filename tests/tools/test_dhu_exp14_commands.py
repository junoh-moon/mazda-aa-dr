import filecmp
import importlib.util
import json
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PLAN = ROOT / "validation" / "DHU_NAVER_EXP14_PLAN_2026-10-24"
PLAN13 = ROOT / "validation" / "DHU_NAVER_EXP13_PLAN_2026-10-12"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load("dhu_exp14", PLAN / "gen_commands.py")


def lines(path):
    return path.read_text().split("\n")[:-1]


def dircmp_clean(a, b):
    c = filecmp.dircmp(a, b)
    return c.left_only + c.right_only + c.diff_files


class Exp14Commands(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.plan = json.loads((PLAN / "run-plan.json").read_text())
        cls.doc, cls.r, cls.m = gen.E13.load_routes()

    def test_regeneration_matches_committed_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "commands"
            plan_path = pathlib.Path(tmp) / "run-plan.json"
            gen.generate(str(out), str(plan_path))
            for sub in ("", "prep"):
                self.assertEqual(dircmp_clean(out / sub, PLAN / "commands" / sub), [], sub)
                for p in (out / sub).iterdir():
                    if p.is_file():
                        self.assertEqual(p.read_bytes(), (PLAN / "commands" / sub / p.name).read_bytes(), p.name)
            self.assertEqual(plan_path.read_bytes(), (PLAN / "run-plan.json").read_bytes())

    def test_prep_and_tier0_are_exp13_copies_with_the_new_tag(self):
        for name in gen.PREP_SRC:
            self.assertEqual((PLAN / "commands" / "prep" / f"{name}.txt").read_bytes(),
                             (PLAN13 / "commands" / "prep" / f"{name}.txt").read_bytes(), name)
        a = lines(PLAN13 / "commands" / f"{gen.T0_SRC}.txt")
        b = lines(PLAN / "commands" / f"{gen.T0_TAG}.txt")
        self.assertEqual(len(a), len(b))
        for x, y in zip(a, b):
            if x.startswith("screenshot "):
                self.assertEqual(x.replace(f"experiment11/{gen.T0_SRC}-", f"experiment14/{gen.T0_TAG}-"), y)
            else:
                self.assertEqual(x, y)
        self.assertTrue(any(x.startswith("location ") for x in a))

    def test_a_runs_differ_only_in_the_bearing_delta(self):
        base = None
        for tag, (delta, _, _) in gen.A_RUNS.items():
            body = [y for y in lines(PLAN / "commands" / f"{tag}.txt") if not y.startswith(("screenshot ", "sleep "))]
            locs = [y.split(" ") for y in body]
            self.assertEqual(len(locs), gen.A_S, tag)
            for t, w in enumerate(locs):
                self.assertEqual(w[4], "NAN")  # altitude never sent
                if t < gen.A_STILL_S:
                    self.assertEqual((w[3], w[5]), ("NAN", "0.000"), (tag, t))
                    self.assertEqual(len(w), 7)
                else:
                    self.assertEqual(w[3], "1.000")
                    self.assertEqual(w[5], "12.000")
            if base is None:
                base = self.plan["runs"][tag]["frames"]
                continue
            fr = self.plan["runs"][tag]["frames"]
            self.assertEqual([f["t"] for f in fr], [f["t"] for f in base])
            for f, b in zip(fr, base):
                self.assertEqual(f["from_fork_m"], b["from_fork_m"])
                self.assertEqual(f["accuracy_m"], b["accuracy_m"])
                if f["phase"] == "carry":
                    self.assertAlmostEqual((f["bearing_sent_deg"] - b["bearing_sent_deg"]) % 360.0,
                                           (delta - gen.A_RUNS["G0-r1"][0]) % 360.0, delta=0.2)
        # the true-bearing tail is identical in every run (the end condition)
        tails = {}
        for tag in gen.A_RUNS:
            tails[tag] = [f["bearing_sent_deg"] for f in self.plan["runs"][tag]["frames"] if f["phase"] == "true"]
        first = tails["G0-r1"]
        for tag, vals in tails.items():
            self.assertEqual(vals, first, tag)

    def test_b_runs_differ_only_in_the_jump_after_the_transition(self):
        base = None
        for tag, (jump, _, _) in gen.B_RUNS.items():
            body = [y for y in lines(PLAN / "commands" / f"{tag}.txt") if not y.startswith(("screenshot ", "sleep "))]
            locs = [y.split(" ") for y in body]
            self.assertEqual(len(locs), gen.B_S, tag)
            for t, w in enumerate(locs):
                self.assertEqual(w[5], "20.000", (tag, t))
                self.assertEqual(w[3], "25.000" if gen.B_PLAIN_S <= t < gen.B_TRANSITION_T else "1.000", (tag, t))
            if base is None:
                base = locs
                continue
            for t, (w, b) in enumerate(zip(locs, base)):
                if t < gen.B_TRANSITION_T:
                    self.assertEqual(w[1:3], b[1:3], (tag, t))  # same positions before the transition
                else:
                    self.assertNotEqual((w[1], w[2]) if jump else None, (b[1], b[2]) if jump else None, (tag, t))
                    f = {x["t"]: x for x in self.plan["runs"][tag]["frames"]}[t] if t in {x["t"] for x in self.plan["runs"][tag]["frames"]} else None
                    if f is not None:
                        self.assertAlmostEqual(f["from_fork_m"] - f["from_fork_true_m"], jump, delta=0.2)

    def test_streams_stay_on_the_mapped_route_before_the_fork(self):
        for tag, r in self.plan["runs"].items():
            for f in r["frames"]:
                self.assertLess(f["from_fork_m"], 0.0, tag)

    def test_jump_and_delta_values_are_the_recorded_measurements(self):
        self.assertEqual({tag: v[0] for tag, v in gen.A_RUNS.items()},
                         {"G0-r1": 0.0, "G37-r1": -36.6, "G84-r1": -84.2, "G171-r1": 171.0})
        self.assertEqual({tag: v[0] for tag, v in gen.B_RUNS.items()},
                         {"J0-r1": 0.0, "J15-r1": 15.0, "J37-r1": 36.8, "J113-r1": 113.3})

    def test_time_budget(self):
        est, budget = self.plan["cumulative_estimate_min"], self.plan["budget_min"]
        for s, req in self.plan["required"].items():
            for tag in req:
                self.assertLessEqual(est[tag], budget - 5, tag)
            self.assertEqual(req, self.plan["sessions"][s][: len(req)])


if __name__ == "__main__":
    unittest.main()
