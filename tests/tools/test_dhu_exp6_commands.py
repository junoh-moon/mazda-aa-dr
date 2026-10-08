import importlib.util
import math
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PLAN = ROOT / "validation" / "DHU_NAVER_EXP6_PLAN_2026-10-08"
spec = importlib.util.spec_from_file_location("dhu_exp6", PLAN / "gen_commands.py")
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)

ENTRANCE = (35.1714322, 129.0250025)  # OSM node 731823096
EXIT = (35.1853302, 129.0043248)  # OSM node 731823121


def enu(p, origin=ENTRANCE):
    """Local east/north metres (equirectangular), independent of the generator's own helpers."""
    k = math.pi / 180 * 6371000.0
    return ((p[1] - origin[1]) * k * math.cos(math.radians(origin[0])), (p[0] - origin[0]) * k)


def tunnel_frame(p):
    """(along metres from the entrance, signed cross-track metres, right of travel positive)."""
    ex, ey = enu(EXIT)
    n = math.hypot(ex, ey)
    ux, uy = ex / n, ey / n
    x, y = enu(p)
    return x * ux + y * uy, x * uy - y * ux


class Exp6Commands(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.route, cls.doc = gen.generate(cls.tmp.name)
        cls.files = {p.stem: p.read_text().splitlines() for p in pathlib.Path(cls.tmp.name).glob("*.txt")}

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def phase_fixes(self, tag, phase):
        """Location fields of one phase; the first 6 location lines belong to the setup template."""
        plan = self.doc["runs"][tag]
        fixes = [line.split() for line in self.files[tag] if line.startswith("location ")][6:]
        n_tunnel = self.tunnel_len(plan) if plan["params"].get("inject", True) else 0
        a = gen.APPROACH_FIXES
        return {"approach": fixes[:a], "tunnel": fixes[a:a + n_tunnel], "exit": fixes[a + n_tunnel:]}[phase]

    def test_every_run_and_repetition_is_written_with_a_consistent_one_hertz_timeline(self):
        self.assertEqual(len(self.files), 2 * len(gen.RUNS))
        self.assertEqual(sorted(self.doc["order"]), sorted(self.files))
        for tag, lines in self.files.items():
            plan = self.doc["runs"][tag]
            seconds = sum(1 for line in lines if line in ("sleep 1", "sleep 0.8")) - 5  # setup holds 5 x sleep 1
            self.assertEqual(seconds, plan["duration_s"], tag)
            shots = [line for line in lines if line.startswith("screenshot ")]
            self.assertTrue(all(s.startswith("screenshot ../../../../experiment6/" + tag) for s in shots))
            self.assertEqual(len(shots), len(plan["frames"]) + 1)  # + setup guidance
            fixes = sum(1 for line in lines if line.startswith("location "))
            setup = 6
            expected = plan["duration_s"] - (0 if plan["params"].get("inject", True) else self.tunnel_len(plan))
            self.assertEqual(fixes - setup, expected, tag)

    @staticmethod
    def tunnel_len(plan):
        return plan["tunnel"][1] - plan["tunnel"][0] + 1

    def test_tunnel_length_and_exp3_compatible_start(self):
        self.assertAlmostEqual(self.route.exit - self.route.entrance, 2433.1, delta=0.5)
        start = self.doc["runs"]["T1-ideal-a40-r1"]["start"]
        self.assertEqual(start, [35.1697679, 129.0267566, 328.9])  # experiment 3 approach_start

    def test_ideal_positions_lie_on_the_tunnel_line_with_road_bearing(self):
        fixes = self.phase_fixes("T1-ideal-a40-r1", "tunnel")
        prev = -1e9
        for f in fixes:
            along, cross = tunnel_frame((float(f[1]), float(f[2])))
            self.assertLess(abs(cross), 1.0)
            self.assertGreater(along, prev)
            prev = along
            self.assertEqual(f[3], "40.000")
            self.assertAlmostEqual(float(f[6]), 309.4, delta=0.2)
        self.assertGreater(prev, 2400.0)

    def test_cross_track_offsets_are_perpendicular_metres(self):
        for tag, value in (("T2-cross50-a40-r1", 50.0), ("T3-cross150-a40-r1", 150.0)):
            fixes = self.phase_fixes(tag, "tunnel")
            ideal = self.phase_fixes("T1-ideal-a40-r1", "tunnel")
            for k, (f, g) in enumerate(zip(fixes, ideal)):
                a, c = tunnel_frame((float(f[1]), float(f[2])))
                a0, _ = tunnel_frame((float(g[1]), float(g[2])))
                self.assertAlmostEqual(c, value * min(1.0, (k + 1) / gen.RAMP_S), delta=1.0)
                self.assertAlmostEqual(a, a0, delta=1.0)
        grow = [tunnel_frame((float(f[1]), float(f[2])))[1] for f in self.phase_fixes("T4-crossgrow300-a40-r1", "tunnel")]
        self.assertTrue(all(b >= a for a, b in zip(grow, grow[1:])))
        self.assertAlmostEqual(grow[-1], 300.0, delta=5.0)

    def test_along_track_offsets(self):
        ideal = [tunnel_frame((float(f[1]), float(f[2])))[0] for f in self.phase_fixes("T1-ideal-a40-r1", "tunnel")]
        for tag, value in (("T5-along-plus100-a40-r1", 100.0), ("T6-along-minus100-a40-r1", -100.0)):
            fr = [tunnel_frame((float(f[1]), float(f[2]))) for f in self.phase_fixes(tag, "tunnel")]
            for k in range(len(fr)):
                if fr[k][0] > 2433.0:  # plus-100 fixes near the end are already past the exit, on 모라로
                    continue
                self.assertAlmostEqual(fr[k][0] - ideal[k], value * min(1.0, (k + 1) / gen.RAMP_S), delta=1.0)
                self.assertLess(abs(fr[k][1]), 1.0)
            self.assertTrue(all(b[0] > a[0] for a, b in zip(fr, fr[1:])), "never moves backwards")

    def test_gate_control_nobearing_and_controls(self):
        self.assertTrue(all(f[3] == "100.000" for f in self.phase_fixes("T7-ideal-a100-r1", "tunnel")))
        self.assertTrue(all(f[6] == "NAN" for f in self.phase_fixes("T9-ideal-nobearing-a40-r1", "tunnel")))
        for tag in ("T0-control-noinput-r1", "T8C-control-noinput-slow-r1"):
            exit_fix = self.phase_fixes(tag, "exit")[0]
            self.assertEqual(exit_fix[3], "5.000")
            along, cross = tunnel_frame((float(exit_fix[1]), float(exit_fix[2])))
            self.assertAlmostEqual(along, 2433.1, delta=20.0)

    def test_real_speed_profile(self):
        speeds = [float(f[5]) for f in self.phase_fixes("T8-realspeed-a40-r1", "tunnel")]
        self.assertTrue(all(abs(s - 17 / 3.6) < 0.01 for s in speeds[:20]))
        self.assertTrue(all(abs(s - 30 / 3.6) < 0.01 for s in speeds[20:]))
        approach = self.phase_fixes("T8-realspeed-a40-r1", "approach")
        self.assertEqual(len(approach), 20)
        self.assertTrue(all(abs(float(f[5]) - 17 / 3.6) < 0.01 for f in approach))
        along, _ = tunnel_frame((float(approach[-1][1]), float(approach[-1][2])))
        self.assertAlmostEqual(along, 20.0, delta=0.5)


if __name__ == "__main__":
    unittest.main()
