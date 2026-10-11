import filecmp
import importlib.util
import json
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PLAN = ROOT / "validation" / "DHU_NAVER_EXP11_PLAN_2026-10-11"
PLAN10 = ROOT / "validation" / "DHU_NAVER_EXP10_PLAN_2026-10-11"
PLAN9 = ROOT / "validation" / "DHU_NAVER_EXP9_PLAN_2026-10-10"
EXP4_INI = ROOT / "validation" / "DHU_NAVER_EXP4_2026-10-03" / "dhu.ini"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load("dhu_exp11", PLAN / "gen_commands.py")


def locs(lines):
    return [line.split() for line in lines if line.startswith("location ")]


def seconds(lines):
    """[(satellites or None, location words)] per second of a body file."""
    out, sat = [], None
    for line in lines:
        if line.startswith("gps_satellite "):
            sat = int(line.split()[1])
        elif line.startswith("location "):
            out.append((sat, line.split()))
            sat = None
    return out


class Exp11Commands(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(cls.tmp.name)
        cls.out, cls.plan_path = base / "c", base / "p.json"
        cls.route, cls.doc = gen.generate(str(cls.out), str(cls.plan_path))
        cls.files = {p.name[:-4]: p.read_text().splitlines() for p in cls.out.glob("*.txt")}

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def tunnel_runs(self):
        return [t for t, p in self.doc["runs"].items() if p.get("kind") != "garage"]

    def test_generator_is_deterministic_and_matches_the_committed_files(self):
        with tempfile.TemporaryDirectory() as other:
            gen.generate(other + "/c", other + "/p.json")
            for sub in ("", "prep"):
                cmp = filecmp.dircmp(str(self.out / sub), other + "/c/" + sub)
                self.assertEqual((cmp.left_only, cmp.right_only, cmp.diff_files), ([], [], []), sub)
            self.assertEqual(self.plan_path.read_bytes(), pathlib.Path(other + "/p.json").read_bytes())
        committed = PLAN / "commands"
        for sub in ("", "prep"):
            self.assertEqual(sorted(p.name for p in (committed / sub).glob("*.txt")),
                             sorted(p.name for p in (self.out / sub).glob("*.txt")), sub)
            for p in (self.out / sub).glob("*.txt"):
                self.assertEqual(p.read_bytes(), (committed / sub / p.name).read_bytes(), p.name)
        self.assertEqual(json.loads((PLAN / "run-plan.json").read_text()), json.loads(self.plan_path.read_text()))

    def test_preparation_is_the_exp10_v5_b2_preparation_byte_for_byte_at_1_m(self):
        for name in gen.PREP_FILES:
            mine = (self.out / "prep" / (name + ".txt")).read_bytes()
            self.assertEqual(mine, (PLAN10 / "commands" / "prep" / (name + ".txt")).read_bytes(), name)
            for w in locs(mine.decode().splitlines()):
                self.assertEqual((w[1], w[2], w[3], w[5], w[6]), ("35.1697679", "129.0267566", "1", "0", "328.9"), name)
        self.assertEqual((PLAN / "geometry" / "route.json").read_bytes(), (PLAN10 / "geometry" / "route.json").read_bytes())

    def test_tier0_is_the_exp10_v5_stability_stream(self):
        src = (PLAN10 / "commands" / "T0-PRE.txt").read_text().splitlines()
        self.assertIn("setup-guidance.png", src[gen.N_T0_SKIP - 1])
        body = src[gen.N_T0_SKIP:]
        nosat = self.files["T0-NOSAT"]
        self.assertEqual(len(nosat), len(body))
        for a, b in zip(nosat, body):
            if b.startswith("screenshot "):
                self.assertEqual(a, b.replace("../../../../experiment10/T0-PRE", "../../../../experiment11/T0-NOSAT"))
            else:
                self.assertEqual(a, b)
        sat = self.files["T0-SAT"]
        self.assertEqual([x for x in sat if not x.startswith("gps_satellite ")],
                         [x.replace("T0-NOSAT", "T0-SAT") for x in nosat])
        self.assertEqual([s for s, _ in seconds(sat)], [5] * 20)
        self.assertEqual(self.files["SAT5"], ["gps_satellite 5"])

    def test_sat_ini_is_the_exp4_ini_plus_one_line(self):
        self.assertEqual((PLAN / "dhu-sat.ini").read_text(), EXP4_INI.read_text() + "gps_satellite = true\n")

    def test_approach_is_the_exp9_approach_at_1_m(self):
        src = (PLAN9 / "commands" / "ST8-nostop-r1.txt").read_text().splitlines()
        k = next(i for i, x in enumerate(src) if "setup-guidance.png" in x)
        exp9 = locs(src[k + 1:])  # the body exp9 begin.py sent (after the preparation screenshot)
        exp9_approach = [w for w in exp9 if w[3] == "5.000"][:gen.APPROACH_N]
        self.assertEqual(len(exp9_approach), 20)
        self.assertEqual(exp9[gen.APPROACH_N][3], "40.000")  # exp9 t=20: first tunnel fix
        for tag in self.tunnel_runs():
            sec = seconds(self.files[tag])
            for (s, w), w9 in zip(sec[:gen.APPROACH_N], exp9_approach):
                self.assertEqual(s, 5)
                self.assertEqual(w, w9[:3] + ["1.000"] + w9[4:], tag)
            # exp9 t=20 position is the first tunnel fix (drift is exactly 0 there)
            self.assertEqual(sec[gen.APPROACH_N][1][1:3], exp9[gen.APPROACH_N][1:3], tag)

    def test_one_hertz_and_photos(self):
        for tag, p in self.doc["runs"].items():
            lines = self.files[tag]
            sec = seconds(lines)
            self.assertEqual(len(sec), p["duration_s"], tag)
            self.assertTrue(all(s is not None for s, _ in sec), tag)
            per = sum(float(x.split()[1]) for x in lines if x.startswith("sleep "))
            self.assertAlmostEqual(per, p["duration_s"], places=6)
            shots = [x for x in lines if x.startswith("screenshot ")]
            self.assertEqual(len(shots), len(p["frames"]))
            self.assertTrue(all(x.startswith("screenshot ../../../../experiment11/" + tag + "-") for x in shots))
            for fr in p["frames"]:
                if fr["phase"] == "tunnel" and fr["tau"] in gen.CHECK_TAU:
                    self.assertTrue(fr["speed_check"])
            self.assertEqual(sorted(fr["tau"] for fr in p["frames"] if fr.get("speed_check")), p["speed_checks"])

    def test_speed_profile_is_readable_and_inside_the_real_range(self):
        for a, b, v in gen.PLATEAUS:
            self.assertTrue(3.9 <= v <= 17.0)
        kmh = [v * 3.6 for _, _, v in gen.PLATEAUS]
        for x, y in zip(kmh, kmh[1:]):
            self.assertGreaterEqual(abs(x - y), 10.0)
        for x in kmh[1:]:
            self.assertGreaterEqual(abs(x - gen.V_APPROACH * 3.6), 10.0)
        for tau in gen.CHECK_TAU:
            v = gen.plateau_of(tau)
            self.assertIsNotNone(v)
            self.assertTrue(all(gen.plateau_of(tau - k) == v for k in range(5)), tau)  # >= 5 s into the plateau
            self.assertIn(tau, gen.EPISODE_PHOTOS)
        for tau in range(120):
            self.assertTrue(3.9 <= gen.speed(tau) <= 17.0)

    def test_conditions_differ_only_in_their_variables(self):
        sec = {t: seconds(self.files[t]) for t in self.doc["runs"]}
        A, T20 = gen.APPROACH_N, 20

        def tunnel(tag):
            p = self.doc["runs"][tag]
            return sec[tag][A:A + p["episode_s"]]

        # same truth along the route for every run of the same length: speeds identical
        for t in self.tunnel_runs():
            self.assertEqual([w[5] for _, w in tunnel(t)], [f"{gen.speed(k):.3f}" for k in range(self.doc["runs"][t]["episode_s"])])
        # V1S0 / V1S5 / V2: same positions and bearings; differ only in satellites (V1) or accuracy (V2)
        for t in ("V1S0-r1", "V2-r1"):
            for (s1, w1), (s2, w2) in zip(tunnel("V1S5-r1"), tunnel(t)):
                self.assertEqual(w1[1:3] + w1[4:], w2[1:3] + w2[4:])
        self.assertEqual({(s, w[3]) for s, w in tunnel("V1S5-r1")}, {(5, "20.000")})
        self.assertEqual({(s, w[3]) for s, w in tunnel("V1S0-r1")}, {(0, "20.000")})
        self.assertEqual({(s, w[3]) for s, w in tunnel("V2-r1")}, {(5, "40.000")})
        # V3 vs V1S5: only position and bearing (the drift)
        for (s1, w1), (s2, w2) in zip(tunnel("V1S5-r1"), tunnel("V3-r1")):
            self.assertEqual((s1, w1[3], w1[4], w1[5]), (s2, w2[3], w2[4], w2[5]))
        # V4 vs V5: only position and bearing
        for (s1, w1), (s2, w2) in zip(tunnel("V5-r1"), tunnel("V4-r1")):
            self.assertEqual((s1, w1[3], w1[4], w1[5]), (s2, w2[3], w2[4], w2[5]))
        # V1SN is the V1S5 file (the DHU ignores gps_satellite with the exp4 ini)
        self.assertEqual(self.files["V1SN-r1"], [x.replace("V1S5-r1", "V1SN-r1") for x in self.files["V1S5-r1"]])
        self.assertEqual(self.doc["runs"]["V1SN-r1"]["ini"], "exp4")
        self.assertEqual({p["ini"] for t, p in self.doc["runs"].items() if not t.startswith("V1SN")}, {"sat"})
        # r2 files equal r1 modulo the tag
        for t in self.doc["runs"]:
            if t.endswith("-r2"):
                r1 = t[:-1] + "1"
                self.assertEqual(self.files[t], [x.replace(r1, t) for x in self.files[r1]])
        # recovery identical for runs of the same length, 1 m, satellites 5
        for t in self.tunnel_runs():
            p = self.doc["runs"][t]
            rec = sec[t][A + p["episode_s"]:]
            self.assertEqual(len(rec), gen.REC_S)
            self.assertEqual({(s, w[3], w[5]) for s, w in rec}, {(5, "1.000", "14.000")})
            if p["episode_s"] == 120:
                self.assertEqual(rec, sec["V1S5-r1"][A + 120:])
        self.assertEqual(T20, A)

    def test_accuracy_ramp_cap_and_drift_targets(self):
        v4 = self.doc["runs"]["V4-r1"]
        acc = [fr["accuracy_m"] for fr in v4["frames"] if fr["phase"] == "tunnel"]
        self.assertEqual(acc[0], 30.0)
        self.assertEqual(max(acc), 40.0)
        tun = [w for s, w in seconds(self.files["V4-r1"])[gen.APPROACH_N:gen.APPROACH_N + 111]]
        vals = [float(w[3]) for w in tun]
        self.assertEqual(vals[0], 30.0)
        self.assertTrue(all(b >= a for a, b in zip(vals, vals[1:])))
        self.assertTrue(all(v == 40.0 for v in vals[14:]))
        for tag, target in (("V4-r1", 119.0), ("V3-r1", 120.0), ("V4T60-r1", 52.0), ("V4T14-r1", 24.0)):
            end = self.doc["runs"][tag]["episode_end"]
            self.assertAlmostEqual(end["cross_track_m"], -target, delta=0.5, msg=tag)  # left of travel as in the vehicle
        for tag in ("V1S0-r1", "V1S5-r1", "V2-r1", "V5-r1"):
            self.assertEqual(self.doc["runs"][tag]["episode_end"]["cross_track_m"], 0.0)
        self.assertEqual(self.doc["runs"]["V4-r1"]["episode_s"], 111)
        self.assertLess(self.doc["runs"]["V3-r1"]["episode_end"]["truth_from_entrance_m"] + gen.REC_S * gen.V_REC,
                        self.doc["site"]["tunnel_length_m"])

    def test_v2c30_is_v2_for_30_s_then_v1s5(self):
        sec = {t: seconds(self.files[t]) for t in ("V2-r1", "V2C30-r1", "V1S5-r1")}
        A = gen.APPROACH_N
        for tau in range(120):
            s2, w2 = sec["V2-r1"][A + tau]
            sc, wc = sec["V2C30-r1"][A + tau]
            ref = w2 if tau < gen.CAP30_S else sec["V1S5-r1"][A + tau][1]
            self.assertEqual((sc, wc), (5, ref), tau)
        self.assertEqual(sec["V2C30-r1"][:A], sec["V2-r1"][:A])
        self.assertEqual(sec["V2C30-r1"][A + 120:], sec["V2-r1"][A + 120:])
        self.assertIn(gen.CAP30_S, gen.EPISODE_PHOTOS)

    def test_t_stop_resolution(self):
        tl = self.doc["timeline"]
        t0 = tl["freeze_detectable_from_tau"]
        self.assertGreaterEqual(abs(gen.speed(t0) - gen.V_APPROACH) * 3.6, 10.0)
        self.assertLess(abs(gen.speed(t0 - 1) - gen.V_APPROACH) * 3.6, 10.0)
        photos = tl["t_stop_photo_tau"]
        self.assertTrue(all(b - a == 5 for a, b in zip(photos, photos[1:])))  # every 5 s through the 120 s at 40 m
        v2 = self.doc["runs"]["V2-r1"]
        shot = {fr["tau"] for fr in v2["frames"] if fr["phase"] == "tunnel"}
        self.assertTrue(set(photos) <= shot)
        self.assertTrue({0, 2, 4, 6, 8, 10} <= shot)  # the grey indicator is readable from the first tunnel second

    def test_garage_runs(self):
        runs = self.doc["runs"]
        sec = {t: seconds(self.files[t]) for t in ("V6S-r1", "V6G0-r1", "V6G5-r1")}
        self.assertEqual({runs[t]["duration_s"] for t in sec}, {gen.G_T + gen.G_REC_S})
        g0, g5 = sec["V6G0-r1"], sec["V6G5-r1"]
        # V6G0 vs V6G5: same path, speed and bearing; differ only in accuracy and satellites
        for tau in range(gen.G_T):
            (s0, w0), (s5, w5) = g0[tau], g5[tau]
            self.assertEqual(w0[1:3] + w0[4:], w5[1:3] + w5[4:])
            self.assertEqual((s0, s5, w5[3]), (0, 5, "20.000"))
            self.assertTrue(5.0 <= float(w0[5]) * 3.6 <= 8.0 + 1e-6)
            self.assertEqual(float(w0[3]), min(40.0, float(gen.math.ceil(30 + 0.36 * tau))))
        self.assertTrue(all(float(w[3]) == 40.0 for _, w in g0[28:gen.G_T]))  # pinned at the cap
        # position follows the motion: step length = speed, step direction = mean of the bearings (straight legs exact)
        for tau in range(1, gen.G_T):
            a, b = g0[tau - 1][1], g0[tau][1]
            d = gen.hav((float(a[1]), float(a[2])), (float(b[1]), float(b[2])))
            self.assertAlmostEqual(d, float(b[5]), delta=0.02, msg=tau)
            if a[6] == b[6] and d > 0.5:
                self.assertLess(abs((gen.E7.bearing((float(a[1]), float(a[2])), (float(b[1]), float(b[2]))) - float(b[6]) + 180) % 360
                                    - 180), 1.0, tau)
        # two 90 deg turns in 3 s each
        brg = [float(w[6]) for _, w in g0[:gen.G_T]]
        h0 = runs["V6G0-r1"]["start_heading_deg"]
        self.assertEqual(brg[59], h0)
        self.assertAlmostEqual(brg[62], (h0 + 90) % 360, places=1)
        self.assertEqual(len({round(x, 1) for x in brg[59:63]}), 4)
        self.assertAlmostEqual(brg[102], h0, places=1)
        # V6S: one 1 m fix, then the exp6 T0M / exp8 ST7 stock mode-0 form (NAN accuracy) frozen, speed 0, cached bearing, satellites 0
        stock = sec["V6S-r1"]
        self.assertEqual(stock[0], (5, "location 35.1697679 129.0267566 1.000 NAN 0.000 328.9".split()))
        exp8 = [x for x in (ROOT / "validation" / "DHU_NAVER_EXP8_PLAN_2026-10-10" / "commands" / "ST7-stockmode0-r1.txt")
                .read_text().splitlines() if " NAN NAN " in x]
        self.assertTrue(exp8)
        form = lambda w: (w[0], w[3], w[4])
        for s, w in stock[1:gen.G_T]:
            self.assertEqual((s, w), (0, "location 35.1697679 129.0267566 NAN NAN 0.000 328.9".split()))
            self.assertEqual(form(w), form(exp8[0].split()))
        # all three share the recovery (the true path position, 1 m, satellites 5)
        self.assertEqual(stock[gen.G_T:], g0[gen.G_T:])
        self.assertEqual(g5[gen.G_T:], g0[gen.G_T:])
        self.assertEqual({(s, w[3]) for s, w in g0[gen.G_T:]}, {(5, "1.000")})
        # photos every 5 s plus the turns
        for t in sec:
            taus = {fr["tau"] for fr in runs[t]["frames"] if fr["phase"] == "garage"}
            self.assertTrue(set(range(0, gen.G_T, 5)) <= taus)
        self.assertEqual(self.doc["tiers"][1]["runs"][0], "V6S-r1")
        self.assertNotIn("V6S-r1", self.doc["tiers"][0]["runs"])

    def test_tiers_and_budget(self):
        tiers = self.doc["tiers"]
        # v2: V2 before V3 so the 25 min rule truncates V3 (weakened by the 53 s drive), not V2
        self.assertEqual(tiers[0]["runs"], ["V4-r1", "V1S0-r1", "V2-r1", "V3-r1"])
        self.assertEqual(gen.NOSAT_TIER1, ["V4-r1", "V2-r1", "V3-r1"])
        self.assertEqual(tiers[1]["runs"][:3], ["V6S-r1", "V1S5-r1", "V2C30-r1"])
        self.assertEqual(sorted(t for x in tiers for t in x["runs"]), sorted(self.doc["runs"]))
        self.assertLessEqual(gen.TIER0_MIN[1] + tiers[0]["estimate_min"], gen.BUDGET_MIN)
        self.assertTrue(set(gen.NOSAT_TIER1) <= set(tiers[0]["runs"]))

    def test_readme_prompt_and_rows(self):
        text = (PLAN / "README.md").read_text()
        self.assertIn("COMMIT_TO_PIN", text.split("##")[1] if "##" in text else text)
        for tag in self.doc["runs"]:
            self.assertIn(tag, text)
        for row in ("P-0", "P-1", "P-2", "A-1", "B-1", "B-2", "B-3", "C-1", "D-1", "IND-GREY", "SPEED-FROZEN", "NAV-STOPS",
                    "t_stop", "53", "G-1", "G-2", "G-3", "S-1", "S-2", "TURN-FOLLOWED", "menu 3"):
            self.assertIn(row, text)
        self.assertTrue((PLAN / "OWNER_SETUP_KO.md").exists())


if __name__ == "__main__":
    unittest.main()
