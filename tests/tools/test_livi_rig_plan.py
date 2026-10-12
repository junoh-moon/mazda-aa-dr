"""LIVI rig plan (validation/LIVI_RIG_PLAN_2026-10-12): generator determinism, byte reuse of the experiment 11 inputs,
one variable per run, the R4 shape against the published numbers, and the driver's scenario output (when the
git-ignored driver tools/livi-patch/driver is present and node is installed; otherwise that test is skipped)."""
import filecmp
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PLAN = ROOT / "validation" / "LIVI_RIG_PLAN_2026-10-12"
EXP11 = ROOT / "validation" / "DHU_NAVER_EXP11_PLAN_2026-10-11"
DRIVER = pathlib.Path(os.environ.get("LIVI_RIG_DRIVER", ROOT / "tools" / "livi-patch" / "driver" / "livi-driver.mjs"))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load("livi_rig_plan", PLAN / "gen_rig.py")


def dhu_seconds(path):
    """[(t, satellites or None, location words)] of a DHU command file, t = sum of the sleeps before the fix."""
    out, t, sat = [], 0.0, None
    for line in pathlib.Path(path).read_text().splitlines():
        w = line.split()
        if not w:
            continue
        if w[0] == "gps_satellite":
            sat = int(w[1])
        elif w[0] == "location":
            out.append((round(t, 6), sat, w))
            sat = None
        elif w[0] == "sleep":
            t += float(w[1])
    return out


def num(tok):
    return None if tok.upper() == "NAN" else float(tok)


def expected_pushes(sc, mazda):
    """What driver/scenario.mjs must send for a scenario: [(t, gps dict, speedKph)] (independent re-implementation)."""
    base = (PLAN / sc["base"]).resolve()
    secs = dhu_seconds(base)
    a, b = sc["loss_t"]
    head = float("inf") if sc.get("stock_whole") else sc.get("stock_head_s", 0)
    cached = [g for g in secs if g[0] < a][-1]
    out, k = [], 0
    for t, sat, w in secs:
        lat, lng, acc, alt, spd, brg = (num(x) for x in w[1:7])
        gps = dict(lat=lat, lng=lng, accuracyM=acc, alt=alt, speedMs=spd, heading=brg)
        if sat is not None:
            gps["satellitesInUse"] = sat
        if a <= t <= b:
            if k < head:
                c = [num(x) for x in cached[2][1:7]]
                gps = dict(lat=c[0], lng=c[1], accuracyM=None, alt=c[3], speedMs=c[4], heading=c[5], satellitesInUse=0)
            else:
                cap = sc.get("accuracy_cap_m")
                if cap is not None and gps["accuracyM"] is not None and gps["accuracyM"] > cap:
                    gps["accuracyM"] = cap
                if sc.get("satellites_in_loss") is not None:
                    gps["satellitesInUse"] = sc["satellites_in_loss"]
            k += 1
        if "satellitesInUse" not in gps:
            gps["satellitesInUse"] = mazda["sat_default"]
        out.append((t, gps, round(spd * 3.6 * 1000) / 1000 if spd is not None else None))
    return out


class RigPlan(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.out = pathlib.Path(cls.tmp.name)
        cls.doc, cls.plan = gen.generate(str(cls.out))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_generator_is_deterministic_and_matches_the_committed_files(self):
        for name in ("scenarios.json", "rig-run-plan.json", "commands/R4-r1.txt", "commands/T0-MOVE.txt"):
            self.assertTrue(filecmp.cmp(self.out / name, PLAN / name, shallow=False), name)
        self.assertEqual(sorted(p.name for p in (PLAN / "commands").iterdir()), ["R4-r1.txt", "T0-MOVE.txt"])

    def test_the_experiment_11_inputs_are_used_byte_for_byte(self):
        for name, sc in self.doc["scenarios"].items():
            base = (PLAN / sc["base"]).resolve()
            if name.startswith("R4"):
                self.assertEqual(base, (PLAN / "commands" / "R4-r1.txt").resolve())
                continue
            self.assertEqual(base.parent, (EXP11 / "commands").resolve(), name)
            self.assertEqual(sc["loss_t"], json.loads((EXP11 / "run-plan.json").read_text())["runs"][base.stem]["tunnel_t"])
        # T0-MOVE = the first 20 seconds of V4-r1 with the screenshot tag renamed.
        v4 = (EXP11 / "commands" / "V4-r1.txt").read_text().splitlines()
        t0 = (PLAN / "commands" / "T0-MOVE.txt").read_text().splitlines()
        self.assertEqual(t0, [x.replace("/V4-r1-", "/T0-MOVE-") for x in v4[: len(t0)]])
        self.assertEqual(sum(x.startswith("location ") for x in t0), 20)
        self.assertTrue(v4[len(t0)].startswith("gps_satellite 0"))

    def test_one_variable_per_run(self):
        sc = self.doc["scenarios"]
        keys = ("satellites_in_loss", "accuracy_cap_m", "stock_head_s", "stock_whole", "base", "loss_t")
        diff = lambda a, b: sorted(k for k in keys if sc[a].get(k) != sc[b].get(k))  # noqa: E731
        self.assertEqual(diff("R0-r1", "R1-r1"), ["satellites_in_loss"])
        self.assertEqual(diff("R0-r1", "R2-r1"), ["accuracy_cap_m"])
        self.assertEqual(diff("R0-r1", "R0-r2"), [])
        self.assertEqual(sc["R0-r1"]["satellites_in_loss"], 0)
        self.assertEqual(sc["R1-r1"]["satellites_in_loss"], 5)
        self.assertEqual(sc["R2-r1"]["accuracy_cap_m"], 20.0)
        self.assertEqual(sc["R3-r1"]["satellites_in_loss"], 0)
        self.assertTrue(sc["R5-r1"]["stock_whole"])
        self.assertEqual(sc["R4-r1"]["stock_head_s"], 1)
        # The sent values per photo differ from R0 only in the variable.
        runs = self.plan["runs"]
        for other, field in (("R1-r1", "satellites_sent"), ("R2-r1", "accuracy_m")):
            for f0, f1 in zip(runs["R0-r1"]["frames"], runs[other]["frames"]):
                changed = {k for k in f0 if k != "screenshot" and f0[k] != f1[k]}
                self.assertTrue(changed <= {field}, (other, f0["t"], changed))

    def test_r4_matches_the_published_tunnel(self):
        r4 = self.plan["r4"]
        sp = r4["speed_profile"]
        self.assertEqual((sp["min_kmh"], sp["max_kmh"]), (13, 61))
        self.assertLessEqual(abs(sp["median_kmh"] - 41), 1)
        self.assertLessEqual(abs(len(sp["below20"]) - 18), 1)
        self.assertTrue(68 <= sp["below20"][0] and sp["below20"][-1] <= 101)
        self.assertLessEqual(abs(sp["wheel_m"] - 1143) / 1143, 0.06)
        self.assertAlmostEqual(r4["episode_end"]["cross_track_m"], -118.5, delta=0.5)
        self.assertEqual([gen.r4_acc(t) for t in range(6)], [30.0, 30.0, 34.0, 37.0, 40.0, 40.0])
        frames = {f["t"]: f for f in self.plan["runs"]["R4-r1"]["frames"]}
        self.assertEqual(frames[20]["form"], "stock_mode0")
        self.assertIsNone(frames[20]["accuracy_m"])
        self.assertEqual(frames[22]["form"], "beta")
        self.assertTrue(all(f["satellites_sent"] == 0 for f in frames.values() if 20 <= f["t"] <= 130))

    def test_r5_is_the_stock_resend(self):
        frames = [f for f in self.plan["runs"]["R5-r1"]["frames"] if 20 <= f["t"] <= 79]
        self.assertTrue(frames)
        for f in frames:
            self.assertEqual((f["form"], f["accuracy_m"], f["satellites_sent"], f["location_frozen_at_t"]),
                             ("stock_mode0", None, 0, 19))
            self.assertEqual(f["speed_sent_kmh"], 50.4)

    def test_sessions_fit_the_budget(self):
        for s in self.plan["sessions"]:
            core = [r for r in s["runs"] if r not in s["last_only_if_time"]]
            tier0 = gen.TIER0_MIN[1] if s["session"] == 1 else gen.TIER0_SHORT_MIN[1]
            self.assertLessEqual(tier0 + len(core) * gen.RUN_MIN, gen.BUDGET_MIN, s)

    def test_the_driver_sends_what_the_plan_says(self):
        node = shutil.which("node")
        if not node or not DRIVER.exists():
            self.skipTest("node or the git-ignored driver tools/livi-patch/driver is missing")
        mazda = self.doc["mazda"]
        for name, sc in self.doc["scenarios"].items():
            res = subprocess.run([node, str(DRIVER), "--dry-run", "--scenario", str(PLAN / "scenarios.json"), name],
                                 capture_output=True, text=True, check=True)
            pushes = [json.loads(x) for x in res.stdout.splitlines()]
            pushes = [p for p in pushes if p["kind"] == "push"]
            want = expected_pushes(sc, mazda)
            self.assertEqual(len(pushes), len(want), name)
            for p, (t, gps, kph) in zip(pushes, want):
                self.assertEqual(p["t"], t, name)
                self.assertEqual(p["payload"]["gps"], gps, (name, t))
                self.assertEqual(p["payload"].get("speedKph"), kph, (name, t))


if __name__ == "__main__":
    unittest.main()
