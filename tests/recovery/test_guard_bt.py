#!/usr/bin/env python3
"""Guard decision for the phone IMU Bluetooth service (bds -configFilePath).

validation/PHONE_IMU_CHANNEL_2026-10-14.md. Host fixtures only; the same
harness runs under ARM/QEMU through GUARD_CXX/GUARD_RUNNER. The staged trial
keeps the bds argument only when bt-off, both bt-fail records and an invalid
XML digest are all absent; otherwise it is the template minus that argument
(the stock bds command line). A reset after a boot that used it writes
guard/bt-fail. The product selection itself never depends on it.
"""
import hashlib, os, pathlib, sys, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import test_guard  # noqa: E402
import test_guard_persistent  # noqa: E402
BASE = test_guard.BASE
BOOTS = test_guard_persistent.BOOTS

ARG = ' -configFilePath /data_persist/mx5-aa-dr/bt/BdsConfiguration.xml'
BDS = ('<service type="process" name="bds" path="/jci/bds/bds" autorun="yes" reset_board="yes" '
       'retry_count="30" args="-interface dbus%s" ready_flag="/tmp/bds_ready_flag"></service>')
XML = b'<bdsConfig>generated</bdsConfig>\n'


def with_bds(text, arg):
    return text.replace('</services>', BDS % arg + '</services>')


class PersistentBluetooth(test_guard_persistent.Fixture):
    def setUp(self):
        super().setUp()
        trial = (self.root / BASE / 'guard/normal.trial').read_text()
        self.template = with_bds(trial, ARG)
        for p in ['normal.trial', 'wcp.trial']:
            self.put(BASE + '/guard/' + p, self.template.encode())
        (self.root / BASE / 'bt').mkdir()
        (self.root / BASE / 'bt').chmod(0o755)
        self.put(BASE + '/bt/BdsConfiguration.xml', XML, 0o644)
        self.put(BASE + '/bt/BdsConfiguration.xml.sha256', (hashlib.sha256(XML).hexdigest() + '\n').encode(), 0o644)
        self.enable()

    def staged(self, n, expect_bt, reason):
        self.boot(n)
        r = self.select()
        self.assertEqual(r.returncode, 0, r.stderr)
        text = pathlib.Path(r.stdout.strip()).read_text()
        want = self.template if expect_bt else self.template.replace(ARG, '')
        self.assertEqual(text, want)
        self.assertEqual(text.count('-configFilePath'), 1 if expect_bt else 0)
        last = (self.root / BASE / 'guard/bt-last').read_text()
        self.assertEqual(last, 'mx5dr-bt-v1\nboot_id=%s\nuse=%s\nreason=%s\n' % (BOOTS[n], 'yes' if expect_bt else 'no', reason))
        status = self.call('status').stdout
        self.assertIn('bt_this_boot=%s:%s\n' % ('yes' if expect_bt else 'no', reason), status)
        self.assertTrue(status.rstrip('\n').splitlines()[-1].startswith('verify_message='))
        self.sm_process(r.stdout.strip())
        self.assertEqual(self.call('confirm').returncode, 0)
        return r

    def test_valid_configuration_keeps_the_argument(self):
        self.staged(1, True, 'ok')
        self.staged(2, True, 'ok')
        self.assertFalse((self.root / BASE / 'guard/bt-fail').exists())

    def test_bt_off_marker_of_any_type_removes_it(self):
        marker = self.root / BASE / 'bt-off'
        for n, make in enumerate((lambda: marker.write_text('off\n'), lambda: marker.mkdir(),
                                  lambda: marker.symlink_to('/nonexistent')), start=1):
            make()
            self.staged(n, False, 'off')
            if marker.is_dir() and not marker.is_symlink():
                marker.rmdir()
            else:
                marker.unlink()
        self.staged(4, True, 'ok')

    def test_runtime_bt_fail_and_unreadable_logs_remove_it(self):
        fail = self.root / BASE / 'logs/bt-fail'
        fail.write_text('mx5dr-bt-fail-v1\nboot_id=x\nreason=not_ready\n')
        self.staged(1, False, 'fail_runtime')
        fail.unlink()
        logs = self.root / BASE / 'logs'
        logs.rename(self.root / BASE / 'logs.away')
        self.staged(2, False, 'fail_runtime')  # cannot prove absence: fail closed
        (self.root / BASE / 'logs.away').rename(logs)
        self.staged(3, True, 'ok')

    def test_xml_digest_mismatch_or_bad_record_removes_it(self):
        xml = self.root / BASE / 'bt/BdsConfiguration.xml'
        sha = self.root / BASE / 'bt/BdsConfiguration.xml.sha256'
        good = sha.read_bytes()
        cases = [(xml, XML + b' '), (sha, good[:-1]), (sha, good.upper()), (sha, good + b'\n')]
        for n, (path, data) in enumerate(cases, start=1):
            with self.subTest(n=n):
                saved = path.read_bytes()
                path.write_bytes(data)
                self.staged(n, False, 'xml_invalid')
                path.write_bytes(saved)
        sha.unlink()
        self.staged(len(cases) + 1, False, 'xml_invalid')
        sha.write_bytes(good)
        sha.chmod(0o666)  # group/world writable: not trusted
        self.staged(len(cases) + 2, False, 'xml_invalid')

    def test_reset_after_a_bt_boot_writes_guard_bt_fail(self):
        self.staged(1, True, 'ok')
        self.reset_during('bt crash')       # SM reset report after the boot that used it
        self.staged(2, False, 'fail_guard')
        self.assertEqual((self.root / BASE / 'guard/bt-fail').read_text(),
                         'mx5dr-bt-fail-v1\nboot_id=%s\nreason=reset_after_bt\n' % BOOTS[1])
        self.assertEqual(self.state()['fail_count'], '1')  # the product itself continues
        self.staged(3, False, 'fail_guard')
        self.assertIn('bt_fail_guard=present\n', self.call('status').stdout)

    def test_reset_after_a_boot_without_bt_writes_nothing(self):
        (self.root / BASE / 'bt-off').write_text('off\n')
        self.staged(1, False, 'off')
        self.reset_during('other')
        (self.root / BASE / 'bt-off').unlink()
        self.staged(2, True, 'ok')
        self.assertFalse((self.root / BASE / 'guard/bt-fail').exists())

    def test_template_without_argument_is_unchanged(self):
        plain = self.template.replace(ARG, '')
        for p in ['normal.trial', 'wcp.trial']:
            self.put(BASE + '/guard/' + p, plain.encode())
        self.enable()
        self.template = plain
        self.boot(1)
        r = self.select()
        self.assertEqual(pathlib.Path(r.stdout.strip()).read_text(), plain)
        self.assertIn('use=no\nreason=not_installed\n', (self.root / BASE / 'guard/bt-last').read_text())

    def test_repeated_argument_is_removed_everywhere(self):
        double = with_bds(self.template, ARG)
        for p in ['normal.trial', 'wcp.trial']:
            self.put(BASE + '/guard/' + p, double.encode())
        self.enable()
        self.boot(1)
        r = self.select()
        self.assertEqual(pathlib.Path(r.stdout.strip()).read_text(), double.replace(ARG, ''))
        self.assertIn('reason=template_invalid\n', (self.root / BASE / 'guard/bt-last').read_text())

    def test_failed_bt_record_falls_back_to_stock_arguments(self):
        env = dict(self.env, MX5DR_GUARD_FAIL_FSYNC='bt-last')
        self.boot(1)
        r = self.select(env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn('-configFilePath', pathlib.Path(r.stdout.strip()).read_text())


class OneBootBluetooth(unittest.TestCase):
    """Borrows the one-boot fixture of test_guard.Gate without its tests."""
    setUpClass = classmethod(lambda cls: test_guard.Gate.setUpClass.__func__(cls))
    tearDownClass = classmethod(lambda cls: cls.build.cleanup())
    put, call, arm, tearDown = test_guard.Gate.put, test_guard.Gate.call, test_guard.Gate.arm, test_guard.Gate.tearDown

    def setUp(self):
        test_guard.Gate.setUp(self)
        trial = (self.root / BASE / 'guard/normal.trial').read_text()
        self.template = with_bds(trial, ARG)
        for p in ['normal.trial', 'wcp.trial']:
            self.put(BASE + '/guard/' + p, self.template.encode())
        (self.root / BASE / 'logs').mkdir()
        (self.root / BASE / 'bt').mkdir()
        (self.root / BASE / 'bt').chmod(0o755)
        self.put(BASE + '/bt/BdsConfiguration.xml', XML)
        self.put(BASE + '/bt/BdsConfiguration.xml.sha256', (hashlib.sha256(XML).hexdigest() + '\n').encode())

    def test_one_boot_selection_applies_the_same_decision(self):
        self.arm()
        r = self.call('select', '/jci/sm/sm.conf')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(pathlib.Path(r.stdout.strip()).read_text(), self.template)
        self.put(BASE + '/bt-off', b'off\n')
        (self.root / BASE / 'guard/last-boot').unlink()
        self.arm()
        r = self.call('select', '/jci/sm/sm.conf')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(pathlib.Path(r.stdout.strip()).read_text(), self.template.replace(ARG, ''))


if __name__ == '__main__':
    unittest.main()
