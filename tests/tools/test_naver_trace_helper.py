"""Tests for the Naver trace helper scripts (validation/NAVER_TRACE_HELPER_2026-10-13) with a stubbed adb.

The helper runs on the owner's Mac; the tests only check what it would send to adb: read-only check, one-line
trace=1 write behind a typed confirmation, scoped removal, and clear-after-zip."""
import os
import shutil
import stat
import subprocess
import tempfile
import unittest

HELPER = os.path.join(os.path.dirname(__file__), "..", "..", "validation", "NAVER_TRACE_HELPER_2026-10-13")

STUB = """#!/bin/sh
echo "$*" >> "$ADB_LOG"
case "$1" in
  devices) printf 'List of devices attached\\nSERIAL1\\tdevice\\n'; exit 0;;
  pull) mkdir -p "$3" 2>/dev/null; mkdir -p "$3/$(basename "$2")"; echo x > "$3/$(basename "$2")/f"; exit 0;;
esac
if [ "$1" = shell ]; then
  case "$2" in
    "cat > "*) cat > "$ADB_CAP"; exit 0;;
    "[ -f "*) [ -n "$ADB_EXIST" ] && exit 0 || exit 1;;
    "dumpsys "*) echo "    versionName=6.11.1.2"; exit 0;;
    "pm path "*) printf 'package:/data/app/x/base.apk\\r\\npackage:/data/app/x/split_a.apk\\r\\n'; exit 0;;
  esac
fi
exit 0
"""


class HelperTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        stub = os.path.join(self.tmp, "adb")
        with open(stub, "w") as f:
            f.write(STUB)
        os.chmod(stub, os.stat(stub).st_mode | stat.S_IXUSR)
        self.log = os.path.join(self.tmp, "calls.log")
        self.cap = os.path.join(self.tmp, "cap.txt")
        open(self.log, "w").close()

    def run_script(self, name, stdin="", args=(), exist=""):
        env = dict(os.environ, PATH=self.tmp + os.pathsep + os.environ["PATH"], ADB_LOG=self.log,
                   ADB_CAP=self.cap, ADB_EXIST=exist)
        return subprocess.run(["sh", os.path.join(HELPER, name), *args], input=stdin, text=True, cwd=self.tmp,
                              env=env, capture_output=True)

    def calls(self):
        return open(self.log).read().splitlines()

    def test_check_is_read_only(self):
        r = self.run_script("check.sh")
        self.assertEqual(r.returncode, 0, r.stderr)
        for c in self.calls():
            self.assertTrue(c.startswith(("devices", "shell dumpsys", "shell ls", "shell cat")), c)
            self.assertNotIn("rm ", c)
            self.assertNotIn(" > ", c)
        self.assertFalse([c for c in self.calls() if c.startswith(("push", "pull"))])

    def test_enable_needs_the_typed_confirmation(self):
        r = self.run_script("enable.sh", stdin="no\n")
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse([c for c in self.calls() if "navi.config" in c and ">" in c])
        self.assertFalse(os.path.exists(self.cap))

    def test_enable_writes_exactly_trace_1(self):
        r = self.run_script("enable.sh", stdin="yes\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(open(self.cap).read(), "trace=1\n")
        self.assertFalse([c for c in self.calls() if "cp " in c])  # no backup without an existing file

    def test_enable_backs_up_an_existing_config(self):
        r = self.run_script("enable.sh", stdin="yes\n", exist="1")
        self.assertEqual(r.returncode, 0, r.stderr)
        calls = self.calls()
        backup = [i for i, c in enumerate(calls) if "cp " in c and "navi.config.bak" in c]
        write = [i for i, c in enumerate(calls) if c.startswith("shell cat >")]
        self.assertTrue(backup and write and backup[0] < write[0])

    @unittest.skipUnless(shutil.which("zip"), "zip not available")
    def test_pull_apk_is_read_only(self):
        r = self.run_script("pull_apk.sh")
        self.assertEqual(r.returncode, 0, r.stderr)
        for c in self.calls():
            self.assertTrue(c.startswith(("devices", "shell dumpsys", "shell pm path", "pull")), c)

    def test_scripts_never_mention_the_dangerous_key(self):
        for name in os.listdir(HELPER):
            if name.endswith(".sh"):
                text = open(os.path.join(HELPER, name)).read()
                self.assertNotIn("ignore_input_location=", text)

    def test_disable_only_touches_navi_config(self):
        r = self.run_script("disable.sh")
        self.assertEqual(r.returncode, 0, r.stderr)
        for c in self.calls():
            if "rm " in c or "mv " in c:
                self.assertIn("navi.config", c)
                self.assertNotIn("NaverNavi", c)

    @unittest.skipUnless(shutil.which("zip"), "zip not available")
    def test_collect_clears_only_with_the_flag_and_after_the_zip(self):
        r = self.run_script("collect.sh")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse([c for c in self.calls() if "rm " in c])
        r = self.run_script("collect.sh", args=("--clear",))
        self.assertEqual(r.returncode, 0, r.stderr)
        rm = [c for c in self.calls() if "rm -rf" in c]
        self.assertEqual(len(rm), 1)
        self.assertIn("NaverNavi/trace", rm[0])
        self.assertIn("NaverNavi/nmea", rm[0])


if __name__ == "__main__":
    unittest.main()
