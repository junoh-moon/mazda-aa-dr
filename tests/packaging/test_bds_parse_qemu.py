"""Mandatory offline parse check of the generated Bluetooth configuration.

Runs the ORIGINAL stock bds (and the stock BusyBox awk) from MX5DR_STOCK_ROOT
under qemu-arm user mode, as validation/PHONE_IMU_FEASIBILITY_FABLE_2026-10-13.md
did: `bds -interface dbus -configFilePath <generated XML>`, -strace, and a
recording `cp` stand-in for the `cp -vf <xml> /tmp/BdsConfiguration.xml` that
bds runs through system(). An ARM LD_PRELOAD probe (bds_parse_probe.c, our
code, it never calls bds code) reads BDS_serialPortCount and the ids in
BDS_SerialPortList at the radio UART open. Required release step (docs/RELEASING_KO.md).

Asserts: no crash before the radio open, serial count 8 (7 stock + ours), our
id 15 appended to the stock ids 5,8,9,10,11,12,14, bds then ends on the normal "no radio"
path (exit 1). Controls: the stock XML (count 7, no MX5DR), a malformed XML
(the crash is detected) and 21 entries (count capped at 20).

Not covered (needs the parked vehicle): SDP registration, RFCOMM, pairing,
the PTY and every radio path; qemu ends at the missing /dev/ttymxc3.
The stock tree is only read: tmp, data and config are private directories of
the temporary root and every write-mode open is checked against the stock tree.
Skips (exit is a SKIP, never a pass) when the stock root, qemu-arm or the
pinned ARM compiler is missing.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
STOCK = Path(os.environ.get('MX5DR_STOCK_ROOT', str(REPO.parent / 'design_inputs/evidence/stock_reference')))
QEMU = os.environ.get('QEMU_ARM') or ('/tmp/qbin/qemu-arm' if Path('/tmp/qbin/qemu-arm').exists() else shutil.which('qemu-arm'))
CROSS = os.environ.get('CROSS_COMPILE') or str(REPO / 'tools/m3-toolchain/bin/arm-cortexa9_neon-linux-gnueabi-')
XML_PATH = '/data_persist/mx5-aa-dr/bt/BdsConfiguration.xml'
NEEDED = ['jci/bds/bds', 'jci/bds/BdsConfiguration.xml', 'jci/lib/libjcibds.so', 'bin/busybox', 'jci/sm/sm.conf', 'jci/sm/sm_WCP.conf']
CP = '''#!/bin/sh
echo "cp-argv: $*" >> "$MX5_CP_LOG"
[ "$#" = 3 ] && [ "$1" = -vf ] && [ "$3" = /tmp/BdsConfiguration.xml ] || { echo cp-refused >> "$MX5_CP_LOG"; exit 1; }
exec /bin/cp -f "$2" "$MX5_FAKE_ROOT/tmp/BdsConfiguration.xml"
'''
MKDIR = '''#!/bin/sh
[ "$#" = 2 ] && [ "$1" = -p ] || exit 1
case "$2" in /tmp/*) exec /bin/mkdir -p "$MX5_FAKE_ROOT$2";; *) exit 1;; esac
'''


def missing():
    if not QEMU or not Path(QEMU).exists():
        return 'qemu-arm not found (QEMU_ARM)'
    if not Path(CROSS + 'gcc').exists():
        return 'pinned ARM compiler not found (CROSS_COMPILE or tools/m3-toolchain)'
    for name in NEEDED:
        if not (STOCK / name).exists():
            return f'stock root lacks {name} (MX5DR_STOCK_ROOT)'
    return None


SKIP = missing()


@unittest.skipIf(SKIP, f'SKIP offline bds parse check: {SKIP}')
class BdsParseCheck(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.work = Path(cls.tmp.name)
        cls.probe = cls.work / 'probe.so'
        subprocess.run([CROSS + 'gcc', '-std=gnu99', '-O1', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        '-mcpu=cortex-a9', '-mfloat-abi=softfp', '-marm', '-o', str(cls.probe),
                        str(REPO / 'tests/packaging/bds_parse_probe.c'), '-ldl'], check=True)
        # Private root: every stock top-level entry is a read-only symlink
        # except tmp, data and config (real directories of this test).
        cls.root = cls.work / 'root'
        cls.root.mkdir()
        for entry in sorted(STOCK.iterdir()):
            if entry.name not in ('tmp', 'data', 'config'):
                (cls.root / entry.name).symlink_to(entry)
        for d in ('tmp', 'data', 'config/common/log'):
            (cls.root / d).mkdir(parents=True)
        if (STOCK / 'config/com').exists():
            (cls.root / 'config/com').symlink_to(STOCK / 'config/com')
        (cls.root / 'config/common/log/settings.txt').write_text(
            'COMMON_LOG_FORMAT=SYSLOGNGEXT\nCOMMON_LOG_STRIP=True\nCOMMON_LOG_LEVEL=Info\n')
        cls.bin = cls.work / 'bin'
        cls.bin.mkdir()
        for name, text in (('cp', CP), ('mkdir', MKDIR)):
            (cls.bin / name).write_text(text)
            (cls.bin / name).chmod(0o755)
        cls.stock_xml = (STOCK / 'jci/bds/BdsConfiguration.xml').read_text()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def awk(self, busybox, *args, src):
        if busybox:  # the stock BusyBox 1.19.2 awk, under qemu-arm
            cmd = [QEMU, '-L', str(STOCK), str(STOCK / 'bin/busybox'), 'awk']
        else:
            cmd = ['awk']
        result = subprocess.run(cmd + list(args) + ['-f', str(REPO / 'packaging/edit_bds.awk'), str(src)],
                                capture_output=True, text=True, timeout=60)
        return result

    def generated(self):
        result = self.awk(False, '-v', 'what=xml', src=STOCK / 'jci/bds/BdsConfiguration.xml')
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def run_bds(self, tag, xml_text=None, wcp=False):
        """Original bds under qemu-arm; returns (rc, cp log, strace, probe lines)."""
        log, probe = self.work / f'cp-{tag}.log', self.work / f'probe-{tag}.txt'
        for p in (log, probe, self.root / 'tmp/BdsConfiguration.xml'):
            if p.exists():
                p.unlink()
        args = ['-interface', 'dbus'] + (['-wcp'] if wcp else [])
        if xml_text is not None:
            xml = self.work / f'{tag}.xml'
            xml.write_text(xml_text)
            args += ['-configFilePath', str(xml)]
        env = dict(os.environ, PATH=str(self.bin) + ':' + os.environ['PATH'], MX5_CP_LOG=str(log),
                   MX5_FAKE_ROOT=str(self.root))
        result = subprocess.run(['nice', '-n', '19', QEMU, '-strace', '-L', str(self.root),
                                 '-E', 'LD_LIBRARY_PATH=/jci/lib:/usr/lib:/lib',
                                 '-E', 'LD_PRELOAD=' + str(self.probe), '-E', 'MX5DR_BDS_PROBE=' + str(probe),
                                 str(STOCK / 'jci/bds/bds')] + args,
                                env=env, capture_output=True, text=True, errors='replace', timeout=120)
        cp = log.read_text() if log.exists() else ''
        lines = probe.read_text().splitlines() if probe.exists() else []
        # The stock tree is never opened for writing.
        for m in re.finditer(r'open\("([^"]+)",([A-Z_|]+)', result.stderr):
            path, flags = m.group(1), m.group(2)
            if ('O_WRONLY' in flags or 'O_RDWR' in flags or 'O_CREAT' in flags) and path != '/dev/ttymxc3':
                self.assertFalse((STOCK / path.lstrip('/')).exists(), f'write-mode open of stock {path}')
        return result.returncode, cp, result.stderr, lines

    def test_generated_xml_is_parsed_without_crash(self):
        xml = self.generated()
        self.assertEqual(xml.count('name="MX5DR IMU"'), 1)
        rc, cp, strace, probe = self.run_bds('ours', xml)
        self.assertIn('probe_loaded', probe)
        self.assertRegex(cp, r'^cp-argv: -vf \S+/ours\.xml /tmp/BdsConfiguration\.xml\n$')
        self.assertRegex(strace, r'open\("/tmp/BdsConfiguration\.xml",O_RDONLY\) = [0-9]+')
        self.assertNotIn('--- SIG', strace)
        radio = [l for l in probe if l.startswith('radio_open')]
        self.assertEqual(len(radio), 1, probe)
        self.assertEqual(radio, ['radio_open count=8 ids=5,8,9,10,11,12,14,15'])
        self.assertEqual(rc, 1, strace[-2000:])  # normal "Failed to start BDS" without a radio
        print(f'\nbds parse check: {radio[0]}; exit {rc}')

    def test_stock_inputs_are_the_pinned_ones(self):
        # The installer prepares the service only for these exact files.
        import hashlib
        for line in (REPO / 'packaging/bt.sha256').read_text().splitlines():
            want, name = line.split()
            self.assertEqual(hashlib.sha256((STOCK / name).read_bytes()).hexdigest(), want, name)

    def test_generated_xml_with_the_wcp_arguments(self):
        # wcp.trial starts bds with "-interface dbus -wcp -configFilePath <xml>".
        rc, cp, strace, probe = self.run_bds('ours_wcp', self.generated(), wcp=True)
        self.assertRegex(cp, r'^cp-argv: -vf \S+/ours_wcp\.xml /tmp/BdsConfiguration\.xml\n$')
        self.assertNotIn('--- SIG', strace)
        radio = [l for l in probe if l.startswith('radio_open')]
        self.assertEqual(radio, ['radio_open count=8 ids=5,8,9,10,11,12,14,15'])
        self.assertEqual(rc, 1)
        print(f'\nbds parse check (wcp): {radio[0]}; exit {rc}')

    def test_installer_args_edit_on_stock_service_manager_files(self):
        for name, want in (('sm.conf', '-interface dbus'), ('sm_WCP.conf', '-interface dbus -wcp')):
            host = self.awk(False, '-v', 'what=args', '-v', 'action=add', '-v', 'xml=' + XML_PATH, src=STOCK / 'jci/sm' / name)
            self.assertEqual(host.returncode, 0, host.stderr)
            self.assertIn(f'args="{want} -configFilePath {XML_PATH}"', host.stdout)
            stock = (STOCK / 'jci/sm' / name).read_text()
            changed = [(a, b) for a, b in zip(stock.splitlines(), host.stdout.splitlines()) if a != b]
            self.assertEqual(len(changed), 1)
            self.assertEqual(len(stock.splitlines()), len(host.stdout.splitlines()))

    def test_stock_busybox_awk_gives_identical_output(self):
        ours = self.generated()
        bb = self.awk(True, '-v', 'what=xml', src=STOCK / 'jci/bds/BdsConfiguration.xml')
        self.assertEqual(bb.returncode, 0, bb.stderr)
        self.assertEqual(bb.stdout, ours)
        for name in ('sm.conf', 'sm_WCP.conf'):
            args = ['-v', 'what=args', '-v', 'action=add', '-v', 'xml=' + XML_PATH]
            host = self.awk(False, *args, src=STOCK / 'jci/sm' / name)
            bb = self.awk(True, *args, src=STOCK / 'jci/sm' / name)
            self.assertEqual((bb.returncode, bb.stdout), (0, host.stdout))
        # The stock awk refuses a second addition like the host awk.
        twice = self.work / 'twice.xml'
        twice.write_text(ours)
        bb = self.awk(True, '-v', 'what=xml', src=twice)
        self.assertNotEqual(bb.returncode, 0)

    def test_controls_stock_malformed_and_overflow(self):
        rc, cp, strace, probe = self.run_bds('stock', self.stock_xml)
        radio = [l for l in probe if l.startswith('radio_open')]
        self.assertEqual(radio, ['radio_open count=7 ids=5,8,9,10,11,12,14'], probe)
        self.assertEqual(rc, 1)
        broken = self.generated().replace('</serviceConfiguration>', '</serviceConfigurat')
        rc, cp, strace, probe = self.run_bds('malformed', broken)
        self.assertFalse([l for l in probe if l.startswith('radio_open')], probe)
        self.assertTrue('SIGSEGV' in strace or rc in (-11, 139), (rc, strace[-1500:]))
        extra = ''.join(
            '       <serialPort id="%d" name="Load %d" noOfInstances="1" critical="false" enabled="true" '
            'uuidServer="%032X" uuidClient="%032X" writeDelay="3"/>\n' % (20 + i, i, 0xABC00 + i, 0xABC00 + i)
            for i in range(14))
        over = self.stock_xml.replace('  </serviceConfiguration>', extra + '  </serviceConfiguration>')
        self.assertEqual(over.count('<serialPort '), 21)
        rc, cp, strace, probe = self.run_bds('overflow', over)
        radio = [l for l in probe if l.startswith('radio_open')]
        self.assertTrue(radio and radio[0].startswith('radio_open count=20 ids=5,8,9,10,11,12,14,20,'), probe)


if __name__ == '__main__':
    unittest.main()
