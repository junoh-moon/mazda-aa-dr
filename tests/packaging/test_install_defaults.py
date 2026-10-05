"""Host-only CLI policy and real cleanup tests with mocked mount commands."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

PACK = Path(__file__).resolve().parents[2] / 'packaging'


class InstallDefaultsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def parse_options(self, *options, fixture=False, marker=None):
        # Execute the real install entrypoint and stop at its first firmware
        # check. This tests CLI defaults without touching a host target path.
        bundle = self.root / 'bundle'
        bundle.mkdir(exist_ok=True)
        shutil.copyfile(PACK / 'install.sh', bundle / 'install.sh')
        marker_path = bundle / 'bundle-default-mode'
        if marker_path.exists() or marker_path.is_symlink():
            marker_path.unlink()
        if marker is not None:
            if isinstance(marker, Path):
                marker_path.symlink_to(marker)
            else:
                marker_path.write_text(marker)
        (bundle / 'common.sh').write_text('''set -eu
ROOT=${MX5DR_FIXTURE_ROOT:-}
fail() { echo "$*" >&2; exit 1; }
regular() { [ -f "$1" ] && [ ! -L "$1" ] || fail "nonregular"; }
hash() { printf 'test-hash'; }
prepare_arm_boot() { :; }
verify_firmware() { echo "mode=$MODE remount=$ALLOW_REMOUNT"; exit 0; }
''')
        (bundle / 'mx5dr-sha256').write_text('never executed')
        (bundle / 'mx5dr-sha256.sha256').write_text('test-hash  mx5dr-sha256\n')
        env = dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root) if fixture else '')
        return subprocess.run(['sh', str(bundle / 'install.sh'), *options],
                              env=env, capture_output=True, text=True)

    def test_plain_install_defaults_to_observe_and_temporary_remount(self):
        result = self.parse_options()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), 'mode=OBSERVE remount=1')

    def test_no_remount_and_legacy_explicit_option(self):
        for args, expected in ((('--no-remount',), 0), (('--remount',), 1)):
            result = self.parse_options(*args)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('remount=' + str(expected), result.stdout)

    def test_fixture_still_never_remounts(self):
        result = self.parse_options(fixture=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('remount=0', result.stdout)
        result = self.parse_options('--remount', fixture=True)
        self.assertNotEqual(result.returncode, 0)

    def test_unknown_and_assist_options_rejected(self):
        for arg in ('--mode=ASSIST', '--typo'):
            self.assertNotEqual(self.parse_options(arg).returncode, 0)

    def test_bundle_default_and_explicit_mode_override(self):
        for args, expected in (((), 'SHADOW'), (('--mode=OBSERVE',), 'OBSERVE'),
                               (('--mode=OFF',), 'OFF'), (('--mode=SCRUB',), 'SCRUB')):
            result = self.parse_options(*args, marker='SHADOW\n')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('mode=' + expected, result.stdout)

    def test_beta_only_by_explicit_option_or_bundle_marker(self):
        # BETA is never the unmarked default; ASSIST stays undeployable.
        self.assertIn('mode=OBSERVE', self.parse_options().stdout)
        for args, marker in ((('--mode=BETA',), None), ((), 'BETA\n'), (('--mode=BETA',), 'SHADOW\n')):
            result = self.parse_options(*args, marker=marker)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), 'mode=BETA remount=1')
        result = self.parse_options('--mode=SHADOW', marker='BETA\n')
        self.assertIn('mode=SHADOW', result.stdout)
        for args, marker in ((('--mode=ASSIST',), 'BETA\n'), ((), 'ASSIST\n'), (('--mode=beta',), None),
                             ((), 'beta\n'), ((), 'BETA\nBETA\n')):
            result = self.parse_options(*args, marker=marker)
            self.assertNotEqual(result.returncode, 0, (args, marker))
            self.assertNotIn('remount=', result.stdout)

    def test_invalid_default_rejected_even_with_override(self):
        for marker in ('ASSIST\n', '', 'SHADOW\nOFF', 'SHADOW\n\n', 'SHADOW\n\n\n', '$(touch bad)'):
            result = self.parse_options('--mode=OBSERVE', marker=marker)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn('remount=', result.stdout)

    def test_symlink_default_rejected(self):
        source = self.root / 'mode'
        source.write_text('SHADOW\n')
        result = self.parse_options(marker=source)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('remount=', result.stdout)

    def mounts(self, initial='ro', fail_stage='', policy=1, body='mount_rw "$TEST_DIR"', stacked=False):
        fixture = self.root / 'fixture'
        fixture.mkdir()
        (fixture / 'tmp').mkdir()
        (fixture / '.mx5dr-fixture').touch()
        bin_dir = self.root / 'bin'
        bin_dir.mkdir()
        table = self.root / 'mounts'
        table.write_text(('rootfs / rootfs rw 0 0\n' if stacked else '') +
                         '/dev/root / ext3 ' + initial + ' 0 0\n')
        log = self.root / 'mount.log'
        # Use the system awk with the synthetic mount table; selection logic in
        # common.sh is unchanged. Only the /proc/mounts input is redirected.
        awk = shutil.which('awk')
        (bin_dir / 'awk').write_text('''#!/bin/sh
if [ "$#" = 4 ] && [ "$4" = /proc/mounts ]; then
  exec "$REAL_AWK" "$1" "$2" "$3" "$TEST_MOUNTS"
fi
exec "$REAL_AWK" "$@"
''')
        (bin_dir / 'mount').write_text('''#!/bin/sh
echo "$*" >> "$TEST_MOUNT_LOG"
case "$2" in
  remount,rw) [ "$TEST_MOUNT_FAIL" != rw ] || exit 1
              echo 'rootfs / rootfs rw 0 0' > "$TEST_MOUNTS";;
  remount,ro) [ "$TEST_MOUNT_FAIL" != ro ] || exit 1
              echo 'rootfs / rootfs ro 0 0' > "$TEST_MOUNTS";;
  *) exit 90;;
esac
''')
        # macOS lacks the flock CLI. Use the same kernel advisory lock on the
        # inherited descriptor; the production stock BusyBox applet is also
        # exercised by cmu_emulation.py.
        (bin_dir / 'flock').write_text('''#!/usr/bin/env python3
import fcntl, sys
try:
    fcntl.flock(int(sys.argv[-1]), fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    sys.exit(1)
''')
        for path in bin_dir.iterdir():
            path.chmod(0o755)
        env = dict(os.environ, MX5DR_FIXTURE_ROOT=str(fixture),
                   PATH=str(bin_dir) + os.pathsep + os.environ['PATH'],
                   REAL_AWK=awk, TEST_MOUNTS=str(table), TEST_MOUNT_LOG=str(log),
                   TEST_DIR=str(fixture),
                   TEST_MOUNT_FAIL=fail_stage, COMMON=str(PACK / 'common.sh'))
        # ROOT is emptied only inside this test harness after sourcing common;
        # there are no install/storage calls. All mount commands are mocked.
        script = '. "$COMMON"\nROOT=""\nALLOW_REMOUNT=' + str(policy) + '\n' + body + '\n'
        result = subprocess.run(['sh', '-c', script], env=env, capture_output=True, text=True)
        return result, log.read_text().splitlines() if log.exists() else [], table.read_text()

    def test_read_only_mount_restored_on_success_and_no_duplicate_remount(self):
        result, log, table = self.mounts(body='mount_rw "$TEST_DIR"\nmount_rw "$TEST_DIR"')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(log, ['-o remount,rw /', '-o remount,ro /'])
        self.assertIn(' ro ', table)

    def test_read_only_mount_restored_after_install_error(self):
        result, log, table = self.mounts(body='mount_rw "$TEST_DIR"\nfail "synthetic install failure"')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(log, ['-o remount,rw /', '-o remount,ro /'])
        self.assertIn(' ro ', table)

    def test_original_rw_mount_stays_rw(self):
        result, log, table = self.mounts(initial='rw')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(log, [])
        self.assertIn(' rw ', table)

    def test_rootfs_rw_does_not_hide_real_readonly_root(self):
        result, log, table = self.mounts(stacked=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(log, ['-o remount,rw /', '-o remount,ro /'])
        self.assertIn(' ro ', table)

    def test_failed_rw_remount_does_not_claim_install_success(self):
        result, log, _ = self.mounts(fail_stage='rw')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(log, ['-o remount,rw /'])

    def test_failed_ro_restore_propagates_failure(self):
        result, log, _ = self.mounts(fail_stage='ro')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('RESTORE READ-ONLY FAILED', result.stderr)
        self.assertEqual(log, ['-o remount,rw /', '-o remount,ro /'])

    def test_no_remount_refuses_read_only_mount_without_calling_mount(self):
        result, log, _ = self.mounts(policy=0)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(log, [])

    def test_mount_mutex_covers_persistent_unlock_and_restore(self):
        body = '''BASE=$TEST_DIR/base
LOCK=$TEST_DIR/install-lock
mkdir "$BASE"
lock
mount_rw "$TEST_DIR"
sync() {
    sh -c '. "$COMMON"; ROOT=""; ALLOW_REMOUNT=1; mount_rw "$TEST_DIR"' > "$TEST_DIR/second.out" 2>&1 && return 99
    grep -q 'Another installer or USB export is active' "$TEST_DIR/second.out"
}
'''
        result, log, table = self.mounts(body=body)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(log, ['-o remount,rw /', '-o remount,ro /'])
        self.assertIn(' ro ', table)

    def test_cleanup_sync_failure_still_restores_mount(self):
        result, log, table = self.mounts(body='mount_rw "$TEST_DIR"\nsync() { return 1; }')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(log, ['-o remount,rw /', '-o remount,ro /'])
        self.assertIn(' ro ', table)


if __name__ == '__main__':
    unittest.main()
