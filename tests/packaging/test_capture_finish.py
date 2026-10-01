"""Cooperative finish policy; synthetic acknowledgements never execute firmware."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import unittest

PACK = Path(__file__).resolve().parents[2] / 'packaging'
BOOT = '12345678-1234-1234-1234-123456789abc\n'


class CaptureFinishTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / '.mx5dr-fixture').touch()
        self.logs = self.root / 'data_persist/mx5-aa-dr/logs'
        self.logs.mkdir(parents=True)
        p = self.root / 'proc/sys/kernel/random/boot_id'
        p.parent.mkdir(parents=True)
        p.write_text(BOOT)
        self.env = dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root), MX5DR_FIXTURE_FINISH_WAIT='0')
        (self.logs / 'trace.0.jsonl').write_text('retained evidence\n')

    def finish(self, wait=0):
        r = subprocess.run(['sh', str(PACK / 'finish_capture.sh')], capture_output=True,
                           text=True, env=dict(self.env, MX5DR_FIXTURE_FINISH_WAIT=str(wait)))
        self.assertEqual((self.logs / 'trace.0.jsonl').read_text(), 'retained evidence\n')
        return r

    def test_acknowledged_current_boot_and_absent_collector(self):
        (self.logs / 'capture.done').write_text(BOOT)
        r = self.finish()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue((self.logs / 'capture.stop').is_dir())
        self.assertTrue((self.logs / 'collector.stop').is_dir())

    def test_wait_for_atomic_runtime_ack_and_collector_exit(self):
        pid = self.logs / 'collector.pid'
        pid.write_text('1234\n')
        def acknowledge():
            for _ in range(100):
                if (self.logs / 'collector.stop').is_dir():
                    temp = self.logs / 'capture.done.tmp'
                    temp.write_text(BOOT)
                    temp.rename(self.logs / 'capture.done')
                    pid.unlink()
                    return
                time.sleep(.01)
        thread = threading.Thread(target=acknowledge)
        thread.start()
        r = self.finish(wait=2)
        thread.join()
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_missing_wrong_stale_and_inexact_ack_fail(self):
        self.assertNotEqual(self.finish().returncode, 0)
        for value in (BOOT.replace('12345678', '87654321'), BOOT.rstrip(), BOOT + '\n', ''):
            (self.logs / 'capture.done').write_text(value)
            self.assertNotEqual(self.finish().returncode, 0)

    def test_stale_pid_cannot_be_killed_or_ignored(self):
        (self.logs / 'capture.done').write_text(BOOT)
        pid = self.logs / 'collector.pid'
        pid.write_text('999999999\n')
        self.assertNotEqual(self.finish().returncode, 0)
        self.assertEqual(pid.read_text(), '999999999\n')

    def test_symlink_markers_rejected(self):
        outside = self.root / 'outside'
        outside.write_text(BOOT)
        for name in ('capture.done', 'capture.stop', 'collector.stop', 'collector.pid'):
            p = self.logs / name
            if p.is_dir():
                p.rmdir()
            elif p.exists():
                p.unlink()
            p.symlink_to(outside)
            self.assertNotEqual(self.finish().returncode, 0, name)
            self.assertEqual(outside.read_text(), BOOT)
            p.unlink()

    def clear(self):
        return subprocess.run(['sh', '-c', '. "$COMMON"; clear_capture_markers'],
                              env=dict(self.env, COMMON=str(PACK / 'common.sh')),
                              capture_output=True, text=True)

    def test_explicit_rearm_cleanup_preserves_logs(self):
        (self.logs / 'capture.stop').mkdir()
        (self.logs / 'capture.done').write_text(BOOT)
        self.assertEqual(self.clear().returncode, 0)
        self.assertFalse((self.logs / 'capture.stop').exists())
        self.assertFalse((self.logs / 'capture.done').exists())
        self.assertTrue((self.logs / 'trace.0.jsonl').exists())

    def test_rearm_cleanup_rejects_symlink_and_nonempty_directory(self):
        outside = self.root / 'outside'
        outside.write_text(BOOT)
        (self.logs / 'capture.done').symlink_to(outside)
        self.assertNotEqual(self.clear().returncode, 0)
        self.assertEqual(outside.read_text(), BOOT)
        (self.logs / 'capture.done').unlink()
        (self.logs / 'capture.stop').mkdir()
        (self.logs / 'capture.stop/unexpected').touch()
        self.assertNotEqual(self.clear().returncode, 0)
        self.assertTrue((self.logs / 'capture.stop/unexpected').exists())


class TrialBundleTests(unittest.TestCase):
    def test_default_profiles_and_rejection_before_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = root / 'build'
            build.mkdir()
            for name in ('libmx5dr.so', 'libmx5dr-vimtap.so', 'libmx5dr-ldstap.so', 'mx5dr-guard', 'mx5dr-collector', 'mx5dr-sha256'):
                (build / name).write_bytes(b'never executed fixture')
            for args, expected in (([], 'OBSERVE'), (['--default-mode=SHADOW'], 'SHADOW')):
                dest = root / expected
                r = subprocess.run(['sh', str(PACK / 'make_bundle.sh'), *args,
                                    str(build / 'libmx5dr.so'), str(dest)], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual((dest / 'bundle-default-mode').read_text(), expected + '\n')
                for name in ('trial', 'trial_status.sh', 'trial_status.awk', 'finish_capture.sh',
                             'mx5dr-sha256', 'mx5dr-sha256.sha256', 'libmx5dr-ldstap.so',
                             'libmx5dr-ldstap.so.sha256', 'js/run.js', 'INSTALL_KO.md'):
                    self.assertTrue((dest / name).is_file())
                for letter in 'abcd':
                    mp3 = (dest / 'mp3' / (letter + '.mp3')).read_bytes()
                    self.assertEqual(mp3[:4], b'ID3\x03')
                    self.assertIn(('../../../mnt/sd' + letter + '1/js/run.js').encode(), mp3)
            dest = root / 'rejected'
            r = subprocess.run(['sh', str(PACK / 'make_bundle.sh'), '--default-mode=ASSIST',
                                str(build / 'libmx5dr.so'), str(dest)], capture_output=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertFalse(dest.exists())


if __name__ == '__main__':
    unittest.main()
