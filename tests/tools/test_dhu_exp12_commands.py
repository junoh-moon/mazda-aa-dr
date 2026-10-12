import filecmp
import hashlib
import importlib.util
import json
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PLAN = ROOT / "validation" / "DHU_NAVER_EXP12_PLAN_2026-10-12"
PLAN11 = ROOT / "validation" / "DHU_NAVER_EXP11_PLAN_2026-10-11"
EXP4_INI = ROOT / "validation" / "DHU_NAVER_EXP4_2026-10-03" / "dhu.ini"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load("dhu_exp12", PLAN / "gen_commands.py")


def lines(path):
    return path.read_text().split("\n")[:-1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Exp12Commands(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.plan = json.loads((PLAN / "run-plan.json").read_text())
        cls.v4 = lines(PLAN11 / "commands" / "V4-r1.txt")

    def test_regeneration_matches_committed_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "commands"
            plan_path = pathlib.Path(tmp) / "run-plan.json"
            gen.generate(str(out), str(plan_path))
            cmp = filecmp.dircmp(out, PLAN / "commands")
            self.assertEqual(cmp.left_only + cmp.right_only + cmp.diff_files, [])
            sub = filecmp.dircmp(out / "prep", PLAN / "commands" / "prep")
            self.assertEqual(sub.left_only + sub.right_only + sub.diff_files, [])
            for name in cmp.common_files:
                self.assertEqual((out / name).read_bytes(), (PLAN / "commands" / name).read_bytes(), name)
            self.assertEqual(plan_path.read_bytes(), (PLAN / "run-plan.json").read_bytes())

    def test_sources_are_the_runs_that_were_executed(self):
        self.assertEqual(sha(PLAN11 / "commands" / "V4-r1.txt"), gen.SOURCE_SHA256)
        self.assertEqual(sha(EXP4_INI), gen.EXP4_INI_SHA256)
        self.assertEqual(self.plan["ini"]["sha256"], gen.EXP4_INI_SHA256)

    def test_prep_is_byte_copy_of_exp11(self):
        names = sorted(p.name for p in (PLAN / "commands" / "prep").iterdir())
        self.assertEqual(names, sorted(n + ".txt" for n in gen.PREP_FILES))
        for n in names:
            self.assertEqual((PLAN / "commands" / "prep" / n).read_bytes(), (PLAN11 / "commands" / "prep" / n).read_bytes(), n)

    def test_tier0_differs_from_exp11_only_by_tag(self):
        new = lines(PLAN / "commands" / "T0-E12.txt")
        old = lines(PLAN11 / "commands" / "T0-NOSAT.txt")
        self.assertEqual(len(new), len(old))
        for a, b in zip(old, new):
            if a != b:
                self.assertTrue(a.startswith("screenshot "))
                self.assertEqual(a.replace("experiment11/T0-NOSAT-", "experiment11/T0-E12-"), b)

    def test_runs_change_only_tag_and_tunnel_accuracy(self):
        expected = {"V4A29-r1": 29.0, "V4A31-r1": 31.0, "V4REF-r1": None, "V4A25-r1": 25.0}
        self.assertEqual(self.plan["order"], list(expected))
        for tag, acc in expected.items():
            new = lines(PLAN / "commands" / f"{tag}.txt")
            self.assertEqual(len(new), len(self.v4), tag)
            t, changed = -1, 0
            for a, b in zip(self.v4, new):
                if a.startswith("screenshot "):
                    self.assertEqual(a.replace("experiment11/V4-r1-", f"experiment11/{tag}-"), b)
                    self.assertNotEqual(a, b)
                    continue
                if not a.startswith("location "):
                    self.assertEqual(a, b)  # sleeps and gps_satellite lines are byte-identical
                    continue
                t += 1
                wa, wb = a.split(" "), b.split(" ")
                self.assertEqual(len(wa), 7)
                self.assertEqual(wa[:3] + wa[4:], wb[:3] + wb[4:], (tag, t))  # position, altitude, speed, bearing untouched
                if 20 <= t <= 130:
                    self.assertGreaterEqual(float(wa[3]), 30.0)
                    if acc is None:
                        self.assertEqual(wa[3], wb[3])
                    else:
                        self.assertEqual(wb[3], f"{acc:.3f}", (tag, t))
                        changed += wa[3] != wb[3]
                else:
                    self.assertEqual(wa[3], "1.000")
                    self.assertEqual(wb[3], "1.000")
            self.assertEqual(t, 150)
            # V4 already had 31 m at tau 1 and never 25 or 29 m, so A31 changes 110 lines and A25/A29 all 111
            self.assertEqual(changed, {None: 0, 31.0: 110}.get(acc, 111), tag)

    def test_v4ref_equals_v4_except_tag(self):
        new = lines(PLAN / "commands" / "V4REF-r1.txt")
        self.assertEqual([x.replace("experiment11/V4REF-r1-", "experiment11/V4-r1-") for x in new], self.v4)

    def test_bracket_straddles_30_and_reference_is_invalid_by_rule(self):
        acc = {t: r["accuracy_tunnel"] for t, r in self.plan["runs"].items()}
        self.assertLess(acc["V4A29-r1"], 30.0)
        self.assertGreaterEqual(acc["V4A31-r1"], 30.0)
        self.assertLess(acc["V4A31-r1"], 33.3)  # below the VectorAngle 33.3 m band, so A31 vs V4REF separates 30 from 33.3/40
        tun = [float(x.split(" ")[3]) for x in self.v4 if x.startswith("location ")][20:131]
        self.assertEqual(min(tun), 30.0)
        self.assertEqual(max(tun), 40.0)

    def test_plan_frames_match_command_files(self):
        for tag, r in self.plan["runs"].items():
            body = lines(PLAN / "commands" / f"{tag}.txt")
            locs = [x.split(" ") for x in body if x.startswith("location ")]
            shots = [x.split("/")[-1] for x in body if x.startswith("screenshot ")]
            self.assertEqual([f["screenshot"] for f in r["frames"]], shots)
            for f in r["frames"]:
                self.assertEqual(float(locs[f["t"]][3]), f["accuracy_m"], (tag, f["t"]))

    def test_time_budget(self):
        est = self.plan["cumulative_estimate_min"]
        budget = self.plan["budget_min"]
        for tag in self.plan["tier1_required"]:
            self.assertLessEqual(est[tag], budget - 5, tag)  # both bracket runs start with >= 5 min left
        self.assertEqual(self.plan["tier1_required"], self.plan["order"][:2])


if __name__ == "__main__":
    unittest.main()
