"""Phone IMU Bluetooth service: installer, editors, menu 11, removal and the
automatic fallback with the real guard (validation/PHONE_IMU_CHANNEL_2026-10-14.md).

Authored fixtures (SYNTHETIC, NOT OEM) like test_synthetic_install: a
stock-shaped BdsConfiguration.xml written here and synthetic bt.sha256 pins.
The editors also run under the host BusyBox ash/awk; the stock BusyBox awk and
the original bds are exercised in test_bds_parse_qemu.py.
"""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

import test_synthetic_install
import test_trial_menu

REPO = Path(__file__).resolve().parents[2]
EDIT = REPO / 'packaging/edit_bds.awk'
XML_PATH = '/data_persist/mx5-aa-dr/bt/BdsConfiguration.xml'
ARG = ' -configFilePath ' + XML_PATH
UUID = 'FAE5777963AA4424BF7166CE5951B0BE'
SERIAL = ('       <serialPort id="%d" name="%s" noOfInstances="1" critical="false" enabled="true" '
          'uuidServer="%s" uuidClient="%s" writeDelay="3"/>\n')
XML = ('<bdsConfig logLevel="info">\n'
       '   <didInfo enableDid="true" productId="0x5701" version="0x0001" />\n'
       '    <!-- authored fixture, not the OEM file -->\n'
       '   <serviceConfiguration>\n'
       '       <callClient id="1" name="Handsfree" noOfInstances="1" critical="true" enabled="true" securityLevel="LEVEL2"/>\n'
       '       <audioSink id="2" name="Audio Sink" noOfInstances="1" critical="true" enabled="true" securityLevel="LEVEL1"/>\n'
       + ''.join(SERIAL % (i, 'Serial %d' % i, '%032X' % (0x1000 + i), '%032X' % (0x1000 + i)) for i in (5, 8, 9, 10, 11, 12, 14)) +
       '  </serviceConfiguration>\n'
       '   <hfpConfiguration hfpSupportedFeatures="0x0000006F"/>\n'
       '</bdsConfig>\n')
BDS_LINE = ('        <service type="process" name="bds" path="/jci/bds/bds" autorun="yes" reset_board="yes" '
            'retry_count="30" args="%s" ready_flag="/tmp/bds_ready_flag">\n'
            '            <dependency type="service" value="stage_3"/>\n        </service>\n')
BOOT = '12345678-1234-1234-1234-123456789abc\n'
SHELLS = [['awk']] + ([['busybox', 'awk']] if shutil.which('busybox') else [])


def edit(args, text, awk=('awk',)):
    with tempfile.NamedTemporaryFile('w', suffix='.xml', delete=False) as f:
        f.write(text)
    try:
        return subprocess.run(list(awk) + args + ['-f', str(EDIT), f.name], capture_output=True, text=True)
    finally:
        os.unlink(f.name)


class Editors(unittest.TestCase):
    def test_xml_adds_exactly_one_line(self):
        for awk in SHELLS:
            with self.subTest(awk=awk):
                r = edit(['-v', 'what=xml'], XML, awk)
                self.assertEqual(r.returncode, 0, r.stderr)
                added = SERIAL % (15, 'MX5DR IMU', UUID, UUID)
                self.assertEqual(r.stdout, XML.replace('  </serviceConfiguration>', added + '  </serviceConfiguration>'))

    def test_xml_refuses_unknown_shapes(self):
        cases = {
            'crlf': XML.replace('\n', '\r\n'),
            'id15': XML.replace('id="14"', 'id="15"'),
            'ours': XML.replace('%032X' % 0x100e, UUID.lower()),
            'name': XML.replace('Serial 5', 'MX5DR IMU'),
            'duplicate': XML.replace('id="12"', 'id="11"'),
            'comment': XML.replace('  </serviceConfiguration>', '<!-- x -->\n  </serviceConfiguration>'),
            'unknown': XML.replace('  </serviceConfiguration>', '       <bogus id="30" x="1"/>\n  </serviceConfiguration>'),
            'multiline': XML.replace('<audioSink id="2"', '<audioSink\n id="2"'),
            'no_block': XML.replace('<serviceConfiguration>', '<serviceConfig>'),
            'two_blocks': XML + '<serviceConfiguration>\n</serviceConfiguration>\n',
            'full': XML.replace('  </serviceConfiguration>', ''.join(
                SERIAL % (40 + i, 'L%d' % i, '%032X' % i, '%032X' % i) for i in range(13)) + '  </serviceConfiguration>'),
            'no_uuid': XML.replace(' uuidClient="%032X"' % 0x1005, ''),
            'empty': '',
        }
        for awk in SHELLS:
            for name, text in cases.items():
                with self.subTest(awk=awk, case=name):
                    r = edit(['-v', 'what=xml'], text, awk)
                    self.assertNotEqual(r.returncode, 0, r.stdout)
                    self.assertIn('edit_bds:', r.stderr)
        # 19 serialPort entries is the last accepted shape (bds keeps 20).
        r = edit(['-v', 'what=xml'], XML.replace('  </serviceConfiguration>', ''.join(
            SERIAL % (40 + i, 'L%d' % i, '%032X' % i, '%032X' % i) for i in range(12)) + '  </serviceConfiguration>'))
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_args_add_remove_and_refusals(self):
        conf = '<sm_config><services>\n' + BDS_LINE % '-interface dbus' + '</services></sm_config>\n'
        wcp = conf.replace('-interface dbus', '-interface dbus -wcp')
        args = ['-v', 'what=args', '-v', 'xml=' + XML_PATH]
        for awk in SHELLS:
            for base in (conf, wcp):
                with self.subTest(awk=awk, wcp=base is wcp):
                    added = edit(args + ['-v', 'action=add'], base, awk)
                    self.assertEqual(added.returncode, 0, added.stderr)
                    self.assertEqual(added.stdout.count(ARG + '"'), 1)
                    self.assertEqual(added.stdout.replace(ARG, ''), base)
                    again = edit(args + ['-v', 'action=add'], added.stdout, awk)
                    self.assertNotEqual(again.returncode, 0)
                    removed = edit(args + ['-v', 'action=remove'], added.stdout, awk)
                    self.assertEqual((removed.returncode, removed.stdout), (0, base))
                    same = edit(args + ['-v', 'action=remove'], base, awk)
                    self.assertEqual((same.returncode, same.stdout), (0, base))
            bad = {
                'two': conf.replace('</services>', BDS_LINE % '-interface dbus' + '</services>'),
                'none': '<sm_config></sm_config>\n',
                'other_args': conf.replace('-interface dbus', '-interface dbus -radioOnly'),
                'path': conf.replace('/jci/bds/bds', '/tmp/bds'),
                'type': conf.replace('type="process"', 'type="jci_service"'),
                'comment': conf.replace('<service type', '<!-- <service type'),
                'split': conf.replace(' args=', '\n args='),
            }
            for name, text in bad.items():
                with self.subTest(awk=awk, case=name):
                    self.assertNotEqual(edit(args + ['-v', 'action=add'], text, awk).returncode, 0)
            for xml in ('relative/path', '/with space', '/x"y'):
                self.assertNotEqual(edit(['-v', 'what=args', '-v', 'action=add', '-v', 'xml=' + xml], conf, awk).returncode, 0)


class Installer(unittest.TestCase):
    """Installer staging with an authored stock-shaped Bluetooth tree."""

    def setUp(self):
        self.fx = test_synthetic_install.SyntheticPackagingTests()
        self.addCleanup(self.fx.doCleanups)
        self.fx.setUp()
        self.root, self.bundle = self.fx.root, self.fx.bundle
        self.base = self.root / 'data_persist/mx5-aa-dr'
        for name, args in (('sm.conf', '-interface dbus'), ('sm_WCP.conf', '-interface dbus -wcp')):
            path = self.root / 'jci/sm' / name
            path.write_text(path.read_text().replace('</services>', BDS_LINE % args + '</services>'))
        self.fx.original = self.fx.sm.read_bytes()
        self.fx.original_wcp = self.fx.wcp.read_bytes()
        (self.root / 'jci/bds').mkdir(parents=True)
        (self.root / 'jci/bds/BdsConfiguration.xml').write_text(XML)
        (self.root / 'jci/bds/bds').write_bytes(b'SYNTHETIC, NOT OEM: bds')
        self.pin()

    def pin(self):
        lines = ''.join('%s  %s\n' % (hashlib.sha256((self.root / n).read_bytes()).hexdigest(), n)
                        for n in ('jci/bds/BdsConfiguration.xml', 'jci/bds/bds'))
        (self.bundle / 'bt.sha256').write_text(lines)

    def install(self, mode='BETA', ok=True):
        return self.fx.run_script('install.sh', '--mode=' + mode, ok=ok)

    def templates(self):
        return [(self.base / 'guard' / n).read_text() for n in ('normal.trial', 'wcp.trial')]

    def test_beta_install_generates_config_and_template_argument(self):
        r = self.install()
        self.assertIn('Phone Bluetooth channel prepared, OFF (experimental; menu 11 turns it on).', r.stdout)
        # v1.0 default: the copy and the template argument are staged, but
        # the channel is OFF (bt-off) until menu 11 turns it on.
        self.assertEqual((self.base / 'bt-off').read_bytes(), b'off\n')
        xml = (self.base / 'bt/BdsConfiguration.xml').read_bytes()
        expect = edit(['-v', 'what=xml'], XML).stdout.encode()
        self.assertEqual(xml, expect)
        self.assertEqual((self.base / 'bt/BdsConfiguration.xml.sha256').read_text(), hashlib.sha256(xml).hexdigest() + '\n')
        normal, wcp = self.templates()
        self.assertIn('args="-interface dbus' + ARG + '"', normal)
        self.assertIn('args="-interface dbus -wcp' + ARG + '"', wcp)
        for t in (normal, wcp):
            self.assertEqual(t.count('-configFilePath'), 1)
        # Stock files untouched: SM configs, the Bluetooth XML, the bds binary.
        self.assertEqual(self.fx.sm.read_bytes(), self.fx.original)
        self.assertEqual(self.fx.wcp.read_bytes(), self.fx.original_wcp)
        self.assertEqual((self.root / 'jci/bds/BdsConfiguration.xml').read_text(), XML)
        self.assertEqual(oct((self.base / 'bt').stat().st_mode & 0o777), '0o755')
        self.assertFalse(list(self.base.glob('bt/*.new.*')))
        for tool in ('edit_bds.awk', 'bt.sha256'):
            self.assertTrue((self.base / 'tools' / tool).is_file())
        # Idempotent: a second menu 1 gives the same bytes.
        first = (xml, self.templates())
        self.install()
        self.assertEqual(((self.base / 'bt/BdsConfiguration.xml').read_bytes(), self.templates()), first)

    def test_shadow_yes_observe_no_and_switching_back_removes_config(self):
        self.install('SHADOW')
        self.assertTrue((self.base / 'bt/BdsConfiguration.xml').exists())
        r = self.install('OBSERVE')
        self.assertNotIn('Phone Bluetooth channel prepared', r.stdout)
        self.assertFalse((self.base / 'bt').exists())
        self.assertNotIn('-configFilePath', ''.join(self.templates()))

    def test_pin_mismatch_skips_the_service_only(self):
        (self.root / 'jci/bds/bds').write_bytes(b'SYNTHETIC, NOT OEM: other bds')
        r = self.install()
        self.assertIn('Phone IMU Bluetooth service not prepared', r.stdout)
        self.assertFalse((self.base / 'bt').exists())
        self.assertNotIn('-configFilePath', ''.join(self.templates()))
        self.assertTrue((self.base / 'guard/normal.trial').exists())

    def test_unknown_stock_shape_refuses_the_install_before_autostart(self):
        (self.root / 'jci/bds/BdsConfiguration.xml').write_text(XML.replace('id="14"', 'id="15"'))
        self.pin()
        r = self.install(ok=False)
        self.assertIn('Unsupported stock Bluetooth configuration', r.stderr)
        self.assertEqual(self.fx.autostart.read_bytes(), self.fx.original_autostart)
        self.assertFalse(list(self.base.glob('bt/*')))

    def test_menu1_resets_the_channel_to_off_and_clears_fail_records(self):
        # v1.0: menu 1 returns the experimental channel to OFF (bt-off) even
        # when menu 11 had turned it on, and clears both fallback records.
        self.install()
        (self.base / 'bt-off').unlink()                       # menu 11: ON
        (self.base / 'logs/bt-fail').write_text('mx5dr-bt-fail-v1\nboot_id=x\nreason=restart\n')
        (self.base / 'guard/bt-fail').write_text('mx5dr-bt-fail-v1\nboot_id=x\nreason=reset_after_bt\n')
        r = self.install()
        self.assertIn('Cleared logs/bt-fail', r.stdout)
        self.assertIn('Cleared guard/bt-fail', r.stdout)
        self.assertNotIn('Cleared bt-off', r.stdout)
        self.assertEqual((self.base / 'bt-off').read_bytes(), b'off\n')
        for p in ('logs/bt-fail', 'guard/bt-fail'):
            self.assertFalse((self.base / p).exists(), p)
        self.assertTrue(list((self.base / 'backups').glob('bt-fail')))

    def test_uninstall_removes_config_copy_and_keeps_stock(self):
        self.install()
        self.fx.run_script('uninstall.sh')
        self.assertFalse((self.base / 'bt').exists())
        self.assertFalse((self.base / 'bt-off').exists())
        self.assertEqual((self.root / 'jci/bds/BdsConfiguration.xml').read_text(), XML)
        self.assertEqual(self.fx.sm.read_bytes(), self.fx.original)

    def test_uninstall_strips_a_bds_argument_left_in_a_stock_file(self):
        # Never written by the installer; removal still recognises it.
        text = self.fx.sm.read_text().replace('args="-interface dbus"', 'args="-interface dbus' + ARG + '"')
        self.install()
        self.fx.sm.write_text(text)
        self.fx.run_script('uninstall.sh')
        self.assertEqual(self.fx.sm.read_bytes(), self.fx.original)

    def test_space_budget_counts_the_phone_imu_journal(self):
        r = self.install()
        self.assertIn('remaining_log_kib=', r.stdout)
        common = (self.bundle / 'common.sh').read_text()
        self.assertIn('pimu.0.jsonl:1048576 pimu.1.jsonl:1048576', common)


@unittest.skipUnless(shutil.which('g++'), 'host g++ missing')
class GuardFallback(unittest.TestCase):
    """The installer's real templates with the host guard build: bt-off and the
    collector's logs/bt-fail switch the staged bds args back to the stock ones;
    menu 1 restores them."""

    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.guard = Path(cls.build.name) / 'guard'
        subprocess.run(shlex.split(os.environ.get('GUARD_CXX', 'g++')) +
                       ['-std=c++11', '-Wall', '-Wextra', '-Werror', '-DMX5DR_GUARD_TESTING',
                        str(REPO / 'src/guard/guard.cpp'), str(REPO / 'src/runtime/sha256.cpp'), '-o', str(cls.guard)],
                       check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.i = Installer()
        self.addCleanup(self.i.doCleanups)
        self.i.setUp()
        self.root, self.base = self.i.root, self.i.base

    def guard_call(self, *args):
        env = dict(os.environ, MX5DR_GUARD_ROOT=str(self.root))
        return subprocess.run([str(self.guard)] + list(args), env=env, capture_output=True, text=True)

    def select(self, n):
        (self.root / 'proc/sys/kernel/random/boot_id').write_text('%08x-1234-4234-8234-0123456789ab\n' % (0x20000000 + n))
        r = self.guard_call('select', '/jci/sm/sm.conf')
        self.assertEqual(r.returncode, 0, r.stderr)
        trial = r.stdout.strip()
        # The SM runs with the trial (authored /proc entry), then `confirm`.
        proc = self.root / 'proc/266'
        proc.mkdir(parents=True, exist_ok=True)
        (proc / 'cmdline').write_bytes(b'/jci/sm/sm\0-f\0' + trial[len(str(self.root)):].encode() + b'\0')
        self.assertEqual(self.guard_call('confirm').returncode, 0)
        return Path(trial).read_text()

    def test_default_install_passes_no_bds_configuration(self):
        # v1.0 default (bt-off written by menu 1): the configuration the
        # guard generates for SM has no -configFilePath and is byte for byte
        # the trial configuration an installation without the phone channel
        # derives from the same stock files (stock bds arguments).
        self.i.install()
        for d in ('tmp', 'data'):
            (self.root / d).mkdir(exist_ok=True)
        self.assertEqual(self.guard_call('enable').returncode, 0)
        self.assertTrue((self.base / 'bt-off').exists())
        selected = self.select(1)
        self.assertNotIn('-configFilePath', selected)
        self.assertIn('args="-interface dbus"', selected)
        plain = Installer()
        self.addCleanup(plain.doCleanups)
        plain.setUp()
        (plain.root / 'jci/bds/bds').write_bytes(b'SYNTHETIC, NOT OEM: other bds')   # no channel
        self.assertIn('not prepared', plain.install().stdout)
        self.assertEqual(selected, plain.templates()[0])
        self.assertEqual(self.i.fx.sm.read_bytes(), self.i.fx.original)
        self.assertEqual((self.root / 'jci/bds/BdsConfiguration.xml').read_text(), XML)
        last = (self.base / 'guard/bt-last').read_text()
        self.assertIn('use=no\nreason=off\n', last)

    def test_fallback_chain(self):
        self.i.install()
        for d in ('tmp', 'data'):
            (self.root / d).mkdir(exist_ok=True)
        self.assertEqual(self.guard_call('enable').returncode, 0)
        template = self.i.templates()[0]
        self.assertIn(ARG, template)
        self.assertEqual(self.select(1), template.replace(ARG, ''))           # default OFF
        (self.base / 'bt-off').unlink()                                       # menu 11: ON
        self.assertEqual(self.select(2), template)
        (self.base / 'bt-off').write_text('off\n')                            # menu 11: OFF
        self.assertEqual(self.select(3), template.replace(ARG, ''))
        (self.base / 'bt-off').unlink()
        (self.base / 'logs/bt-fail').write_text('mx5dr-bt-fail-v1\nboot_id=x\nreason=not_ready\n')
        self.assertEqual(self.select(4), template.replace(ARG, ''))           # collector fallback
        (self.root / 'proc/uptime').write_text('100.00 1.00\n')
        env = dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root), MX5DR_FIXTURE_GUARD=str(self.guard))
        r = subprocess.run(['sh', str(self.i.bundle / 'trial_status.sh')], env=env, capture_output=True, text=True)
        out = r.stdout + r.stderr
        self.assertIn('bt_fail=runtime:not_ready bt_config=installed bds_args=not_running', out)
        # Menu 2 reads the kernel command line of the running bds (authored /proc entries).
        for pid, argv, want in ((301, [b'/jci/bds/bds', b'-interface', b'dbus', b'-configFilePath', ARG.split()[1].encode()], 'with_config'),
                                (302, [b'bds', b'-interface', b'dbus', b'-wcp', b'-configFilePath', ARG.split()[1].encode()], 'with_config'),
                                (303, [b'/jci/bds/bds', b'-interface', b'dbus'], 'stock'),
                                (304, [b'/usr/bin/bdsx', b'-configFilePath', ARG.split()[1].encode()], 'not_running')):
            proc = self.root / 'proc' / str(pid)
            proc.mkdir(parents=True)
            (proc / 'cmdline').write_bytes(b'\0'.join(argv) + b'\0')
            r = subprocess.run(['sh', str(self.i.bundle / 'trial_status.sh')], env=env, capture_output=True, text=True)
            self.assertIn('bds_args=' + want, r.stdout + r.stderr, argv)
            (proc / 'cmdline').unlink()
            proc.rmdir()
        self.i.install()                                                      # menu 1 resets to OFF
        self.assertFalse((self.base / 'logs/bt-fail').exists())
        self.assertEqual(self.guard_call('enable').returncode, 0)
        self.assertEqual(self.select(5), self.i.templates()[0].replace(ARG, ''))
        (self.base / 'bt-off').unlink()
        self.assertEqual(self.select(6), self.i.templates()[0])


class MenuEleven(unittest.TestCase):
    """Menu 11 turns the experimental phone channel on/off (v1.0 default OFF)."""

    def setUp(self):
        self.t = test_trial_menu.TrialMenuTests()
        self.addCleanup(self.t.doCleanups)
        self.t.setUp()

    def bluetooth_tree(self):
        t = self.t
        for name, args in (('sm.conf', '-interface dbus'), ('sm_WCP.conf', '-interface dbus -wcp')):
            path = t.root / 'jci/sm' / name
            path.write_text(path.read_text().replace('</services>', BDS_LINE % args + '</services>'))
        (t.root / 'jci/bds').mkdir(parents=True)
        (t.root / 'jci/bds/BdsConfiguration.xml').write_text(XML)
        (t.root / 'jci/bds/bds').write_bytes(b'SYNTHETIC, NOT OEM: bds')
        (t.usb / 'bt.sha256').write_text(''.join(
            '%s  %s\n' % (hashlib.sha256((t.root / n).read_bytes()).hexdigest(), n)
            for n in ('jci/bds/BdsConfiguration.xml', 'jci/bds/bds')))
        (t.usb / 'bundle-default-mode').write_text('BETA\n')   # the channel is SHADOW/BETA only

    def test_menu_eleven_turns_the_phone_channel_on_and_off(self):
        t = self.t
        marker = t.base / 'bt-off'
        result = t.menu('11\n0\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('11 Phone Bluetooth: turn on/off (experimental, default OFF)', result.stdout)
        self.assertIn('Phone Bluetooth: package not installed;', result.stdout)
        self.bluetooth_tree()
        result = t.menu('1\n0\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(marker.read_bytes(), b'off\n')
        config = (t.base / 'mx5dr.conf').read_bytes()
        status = t.menu('2\n0\n')
        self.assertIn('bt_marker=present', status.stdout)
        self.assertIn('EXP none (11, 12 off)', status.stdout)
        self.assertNotIn('BT phone IMU OFF', status.stdout)
        for keys in ('11\n0\n', '11\n\n0\n', '11\n1\n0\n', '11\n110\n0\n', '11\n12\n0\n'):
            result = t.menu(keys)
            self.assertIn('Phone Bluetooth now:\n  OFF 꺼짐', result.stdout)
            self.assertIn('Enter 11 again to turn it ON,', result.stdout)
            self.assertIn('Cancelled; nothing changed.', result.stdout)
            self.assertTrue(marker.exists())
        result = t.menu('11\n11\n0\n')
        self.assertIn('Phone Bluetooth turned ON 켜짐(실험).', result.stdout)
        self.assertFalse(marker.exists())
        self.assertEqual((t.base / 'mx5dr.conf').read_bytes(), config)
        out = result.stdout
        start = out.index('Phone Bluetooth now:')
        for line in out[start:out.index('(parked reboot) for it to take effect.') + 40].splitlines():
            line = line.replace('Number, then Enter: ', '')
            self.assertLessEqual(sum(2 if ord(c) > 0x1100 else 1 for c in line), 40, line)
        status = t.menu('2\n0\n')
        self.assertIn('bt_marker=absent', status.stdout)
        self.assertIn('experimental_on=phone_bt,garage', t.menu('12\n12\n2\n0\n').stdout)
        self.assertIn('EXP ON: 11 phone bt, 12 garage', t.menu('2\n0\n').stdout)
        result = t.menu('11\n11\n0\n')
        self.assertIn('Phone Bluetooth now:\n  ON (experimental) 켜짐(실험)', result.stdout)
        self.assertIn('Phone Bluetooth turned OFF 꺼짐.', result.stdout)
        self.assertEqual(marker.read_bytes(), b'off\n')
        self.assertIn('EXP ON: 12 garage', t.menu('2\n0\n').stdout)
        marker.unlink()
        marker.mkdir()
        self.assertIn('Phone Bluetooth: OFF (marker is a dir).', t.menu('11\n11\n0\n').stdout)
        marker.rmdir()
        outside = t.base.parent / 'bt-outside'
        outside.mkdir()
        marker.symlink_to(outside, target_is_directory=True)
        self.assertIn('Phone Bluetooth turned ON 켜짐(실험).', t.menu('11\n11\n0\n').stdout)
        self.assertFalse(marker.is_symlink())
        self.assertTrue(outside.is_dir())
        result = t.menu('1\n0\n')                       # reinstall: OFF again
        self.assertEqual(marker.read_bytes(), b'off\n')
        result = t.menu('4\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(marker.exists())

    def test_menu_eleven_cannot_turn_on_a_channel_that_was_not_prepared(self):
        # Stock Bluetooth files differ from the pins (here: none at all):
        # menu 1 prepared no configuration copy, so ON would do nothing.
        t = self.t
        t.fixture.run_script('install.sh')
        self.assertTrue((t.base / 'bt-off').exists())
        result = t.menu('11\n11\n0\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Not prepared on this CMU (stock', result.stdout)
        self.assertNotIn('turned ON', result.stdout)
        self.assertTrue((t.base / 'bt-off').exists())
        self.assertIn('experimental_on=none', t.menu('2\n0\n').stdout)


if __name__ == '__main__':
    unittest.main()
