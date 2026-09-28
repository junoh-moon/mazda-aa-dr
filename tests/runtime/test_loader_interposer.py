#!/usr/bin/env python3
"""Execute the production loader interposer against synthetic ELF DSOs only."""
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import tempfile
import unittest
PROJECT = Path(__file__).resolve().parents[2]

class LoaderInterposerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="mx5-loader-")
        cls.build = Path(cls.temp.name)
        cls.target = cls.build / "target.so"
        cls.config = cls.build / "config"
        cls.disable = cls.build / "disabled"
        arch = shlex.split(os.environ.get("LOADER_TEST_ARCH_FLAGS", ""))
        flags = arch + ["-std=c++11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-I" + str(PROJECT / "src"),
                 '-DMX5_LOADER_TARGET="' + str(cls.target) + '"']
        cxx = os.environ.get("CXX", "c++")
        cc = os.environ.get("CC", "cc")
        fixture = PROJECT / "tests/runtime/loader"
        def build(command):
            subprocess.run(command, check=True, cwd=PROJECT)
        build([cxx] + flags + ["-fPIC", "-shared", "src/runtime/loader.cpp",
              "src/runtime/config.cpp", str(fixture / "support.cpp"), "-ldl",
              "-Wl,-z,defs,-z,now", "-o", str(cls.build / "loader.so")])
        build([cxx] + flags + ["-fPIC", "-shared", str(fixture / "spy.cpp"),
              "-ldl", "-Wl,-z,defs,-z,now", "-o", str(cls.build / "spy.so")])
        build([cxx] + flags + [str(fixture / "driver.cpp"), "-ldl", "-o",
              str(cls.build / "driver")])
        for name in ("good", "lazy", "bad", "reentrant"):
            build([cc] + arch + ["-Wall", "-Wextra", "-Werror", "-fPIC", "-shared", flags[-1],
                   str(fixture / (name + ".c")), "-ldl", "-o", str(cls.build / (name + ".so"))])

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_real_interposer(self):
        cases = ("off", "off_global", "invalid", "disabled", "missing_config",
                 "good", "lazy", "bad", "caller_now", "existing", "noload",
                 "invalid_flags", "reentrant", "concurrent", "cross_constructor",
                 "prepare_race", "patch_race", "prepare_noload", "patch_noload",
                 "marker_error", "marker_symlink", "lazy_log_closed", "bad_log_closed", "early_noload")
        for case in cases:
            with self.subTest(case=case):
                fixture = case if case in ("lazy", "bad", "reentrant") else "good"
                if case in ("existing", "caller_now", "off", "off_global", "invalid", "disabled", "missing_config", "marker_error", "marker_symlink", "lazy_log_closed", "bad_log_closed", "early_noload"):
                    fixture = "lazy"
                if case == "early_noload": fixture = "good"
                if case.endswith("log_closed"): fixture = case.split("_")[0]
                shutil.copyfile(self.build / (fixture + ".so"), self.target)
                self.config.write_text("mode=" + ("OFF" if case in ("off", "off_global") else "OBSERVE") + "\n")
                if case == "invalid": self.config.write_text("mode=ASSIST\n")
                if case == "missing_config": self.config.unlink()
                if self.disable.exists() or self.disable.is_symlink(): self.disable.unlink()
                if case == "disabled": self.disable.write_text("disabled\n")
                if case == "marker_symlink": self.disable.symlink_to(self.build / "missing-marker-target")
                env = os.environ.copy()
                env.pop("LD_BIND_NOW", None)
                env.update(LD_PRELOAD=str(self.build / "loader.so") + ":" + str(self.build / "spy.so"),
                           TEST_CONFIG=str(self.config), TEST_DISABLE=str(self.disable),
                           TEST_CROSS=str(self.build / "reentrant.so"))
                if case == "marker_error": env["TEST_DISABLE"] = str(self.config / "not-a-directory")
                if case.startswith("prepare_") or case == "early_noload": env["TEST_PREPARE_RACE"] = "1"
                else: env.pop("TEST_PREPARE_RACE", None)
                if case == "early_noload": env["TEST_EARLY_NOLOAD"] = "1"
                else: env.pop("TEST_EARLY_NOLOAD", None)
                if case.startswith("patch_"): env["TEST_PATCH_RACE"] = "1"
                else: env.pop("TEST_PATCH_RACE", None)
                if case in ("concurrent", "cross_constructor"): env["TEST_SLOW_POLICY"] = "1"
                else: env.pop("TEST_SLOW_POLICY", None)
                command = [str(self.build / "driver"), case]
                if os.environ.get("QEMU_SYSROOT"):
                    preload = env.pop("LD_PRELOAD")
                    command = [os.environ.get("QEMU_ARM", "qemu-arm"), "-L",
                               os.environ["QEMU_SYSROOT"], "-E", "LD_PRELOAD=" + preload] + command
                result = subprocess.run(command, env=env,
                                        check=True, timeout=15, stderr=subprocess.PIPE, text=True)
                if case in ("lazy", "bad"):
                    self.assertEqual(result.stderr.count("mx5dr loader: eager_failed"), 1)
                    self.assertIn("fallback=" + ("success" if case == "lazy" else "failed"), result.stderr)
                    self.assertIn("mx5_fixture_missing", result.stderr)
                else:
                    self.assertEqual(result.stderr, "")

if __name__ == "__main__": unittest.main()
