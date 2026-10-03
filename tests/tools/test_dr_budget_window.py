import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("dr_budget_window", ROOT / "tools" / "dr_budget_window.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


class BudgetModelTests(unittest.TestCase):
    def test_matches_values_measured_from_the_real_core(self):
        # Values printed by `dr_budget_window.py --verify` from build/replay (dr_core.c).
        self.assertAlmostEqual(tool.budget_at(14.0, 10.0, 10.0, 0.15, 0.002, 0.3), 35.382999, places=5)
        self.assertAlmostEqual(tool.budget_at(8.0, 20.0, 1.0, 0.01, 0.002, 0.3), 11.807739, places=5)
        self.assertAlmostEqual(tool.budget_at(20.0, 5.0, 5.0, 0.05, 0.002, 0.3), 12.004299, places=5)

    def test_time_to_limit_is_consistent_with_the_budget(self):
        t = tool.seconds_until(14.0, 30.0, 10.0, 0.15, 0.002, 0.3)
        self.assertLess(tool.budget_at(14.0, t - 0.1, 10.0, 0.15, 0.002, 0.3), 30.0)
        self.assertGreaterEqual(tool.budget_at(14.0, t, 10.0, 0.15, 0.002, 0.3), 30.0)

    def test_smaller_error_terms_never_shorten_the_window(self):
        base = tool.seconds_until(14.0, 30.0, 10.0, 0.15, 0.002, 0.3)
        for args in ((5.0, 0.15, 0.002, 0.3), (10.0, 0.05, 0.002, 0.3),
                     (10.0, 0.15, 0.0005, 0.3), (10.0, 0.15, 0.002, 0.14)):
            self.assertGreater(tool.seconds_until(14.0, 30.0, *args), base)

    def test_budget_that_starts_above_the_limit_has_no_window(self):
        self.assertEqual(tool.seconds_until(14.0, 30.0, 31.0, 0.0, 0.0, 0.0), 0.0)

    def test_core_cross_check_when_the_replay_binary_exists(self):
        replay = ROOT / "build" / "replay"
        if not replay.exists():
            self.skipTest("build/replay missing; run `make build/replay` to cross-check against dr_core.c")
        self.assertTrue(tool.verify(str(replay)))


if __name__ == "__main__":
    unittest.main()
