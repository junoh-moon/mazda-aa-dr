"""Host-only integration fixtures; synthetic ARM ELF header is NEVER runnable.

Uses supplied stock firmware as read-only identity fixtures; never executes it.
Run: python3 -m unittest discover -s tests/packaging -v
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[2]
PACK = REPO / 'packaging'
STOCK = Path(os.environ.get('MX5DR_STOCK_ROOT', str(REPO.parent / 'design_inputs/evidence/stock_reference')))
TOKEN = '/data_persist/mx5-aa-dr/libmx5dr.so'
TOUCH = '/data_persist/oem-aa-mod/libpatch-blmjciaapa.so'


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'root'
        self.root.mkdir()
        (self.root / '.mx5dr-fixture').touch()
        (self.root / 'data_persist').mkdir()
        self.bundle = Path(self.tmp.name) / 'bundle'
        shutil.copytree(PACK, self.bundle)
        # Deliberately fake test-only ELF prefix. No executable code.
        fake = bytearray(52)
        fake[:7] = b'\x7fELF\x01\x01\x01'
        fake[16:20] = b'\x03\x00\x28\x00'
        (self.bundle / 'libmx5dr.so').write_bytes(fake)
        self.set_payload_hash()
        fake[16] = 2  # Separate test-only ARM ET_EXEC header; never run.
        (self.bundle / 'mx5dr-collector').write_bytes(fake)
        digest = hashlib.sha256(fake).hexdigest()
        (self.bundle / 'mx5dr-collector.sha256').write_text(digest + '  mx5dr-collector\n')
        (self.bundle / 'mx5dr-guard').write_bytes(fake)
        (self.bundle / 'mx5dr-guard.sha256').write_text(hashlib.sha256(fake).hexdigest() + '  mx5dr-guard\n')
        files = [line.split()[1] for line in (PACK / 'firmware.sha256').read_text().splitlines()]
        files += ['jci/version.ini', 'jci/sm/sm.conf', 'jci/sm/sm_WCP.conf', 'usr/bin/autostart']
        for file in files:
            source = STOCK / file
            if not source.exists():
                self.skipTest(f'Stock identity fixture unavailable: {source}')
            target = self.root / file
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        self.sm = self.root / 'jci/sm/sm.conf'
        self.wcp = self.root / 'jci/sm/sm_WCP.conf'
        self.original = self.sm.read_bytes()
        self.original_wcp = self.wcp.read_bytes()
        self.autostart = self.root / 'usr/bin/autostart'
        self.original_autostart = self.autostart.read_bytes()
        self.trial = self.root / 'data_persist/mx5-aa-dr/guard/normal.trial'

    def set_payload_hash(self):
        digest = hashlib.sha256((self.bundle / 'libmx5dr.so').read_bytes()).hexdigest()
        (self.bundle / 'libmx5dr.so.sha256').write_text(digest + '  libmx5dr.so\n')

    def run_script(self, name, *args, ok=True):
        env = dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root))
        result = subprocess.run(['sh', str(self.bundle / name), *args], env=env,
                                capture_output=True, text=True)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def preload(self, path=None):
        tree = ET.parse(path or self.sm)
        service = tree.find(".//service[@name='jciAAPA']")
        return [node.attrib['env_value'] for node in service.findall('environ_var')
                if node.attrib.get('env_name') == 'LD_PRELOAD']

    def add_touch(self, multiline=False):
        text = self.sm.read_text()
        start = text.index('name="jciAAPA"')
        end = text.index('</service>', start)
        env = f'<environ_var env_name="LD_PRELOAD" env_value="{TOUCH}"/>\n        '
        if multiline:
            env = env.replace(' env_value=', '\n                env_value=')
        self.sm.write_text(text[:end] + env + text[end:])

    def test_stock_and_uninstall_round_trip(self):
        self.run_script('install.sh')
        self.assertEqual(self.preload(), [])
        self.assertEqual(self.preload(self.trial), [TOKEN])
        self.assertEqual(self.autostart.read_text().count('ONE-BOOT BEGIN'), 2)
        self.assertEqual(self.wcp.read_bytes(), self.original_wcp)
        base = self.root / 'data_persist/mx5-aa-dr'
        self.assertTrue((base / 'tools/uninstall.sh').exists())
        self.assertIn('mode=OBSERVE', (base / 'mx5dr.conf').read_text())
        self.run_script('uninstall.sh')
        self.assertEqual(self.sm.read_bytes(), self.original)
        self.assertEqual(self.autostart.read_bytes(), self.original_autostart)
        self.assertTrue((base / 'libmx5dr.so').exists())  # retain mapped inode/file
        self.assertIn('mode=OFF', (base / 'mx5dr.conf').read_text())

    def test_collector_payload_checksum_is_guarded_before_launcher_write(self):
        (self.bundle / 'mx5dr-collector').write_bytes(b'wrong collector')
        self.run_script('install.sh', ok=False)
        self.assertEqual(self.sm.read_bytes(), self.original)

    def test_release_payload_with_existing_touch(self):
        release = REPO / 'bundle/libmx5dr.so'
        if not release.exists():
            self.skipTest('Release bundle has not been built')
        for name in ('libmx5dr.so', 'mx5dr-collector', 'mx5dr-guard'):
            artifact = release.parent / name
            self.assertTrue(artifact.is_file(), str(artifact))
            shutil.copyfile(artifact, self.bundle / name)
            digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
            (self.bundle / (name + '.sha256')).write_text(digest + '  ' + name + '\n')
        self.add_touch(multiline=True)
        self.run_script('install.sh')
        installed = self.root / 'data_persist/mx5-aa-dr/libmx5dr.so'
        self.assertEqual(installed.read_bytes(), release.read_bytes())
        self.assertEqual(self.preload(), [TOUCH])
        self.assertEqual(self.preload(self.trial), [TOKEN + ':' + TOUCH])
        self.run_script('uninstall.sh')
        self.assertEqual(self.preload(), [TOUCH])

    def test_existing_touch_multiline_and_repeat(self):
        self.add_touch(multiline=True)
        self.run_script('install.sh', '--mode=SCRUB')
        once = self.sm.read_bytes()
        self.assertEqual(self.preload(), [TOUCH])
        self.assertEqual(self.preload(self.trial), [TOKEN + ':' + TOUCH])
        self.run_script('install.sh', '--mode=SCRUB')
        self.assertEqual(self.sm.read_bytes(), once)
        self.run_script('uninstall.sh')
        self.assertEqual(self.preload(), [TOUCH])
        self.run_script('uninstall.sh')
        self.assertEqual(self.preload(), [TOUCH])

    def test_uninstall_preserves_unrelated_drift(self):
        self.add_touch()
        self.run_script('install.sh')
        text = self.sm.read_text().replace('</sm_config>', '<!-- later edit -->\n</sm_config>')
        text = text.replace(TOUCH, TOUCH + ':/data_persist/later.so')
        self.sm.write_text(text)
        self.run_script('uninstall.sh')
        self.assertEqual(self.preload(), [TOUCH + ':/data_persist/later.so'])
        self.assertIn('later edit', self.sm.read_text())

    def test_legacy_permanent_install_rejected_before_replacement(self):
        self.add_touch()
        self.sm.write_text(self.sm.read_text().replace(TOUCH, TOKEN + ':' + TOUCH))
        before = self.sm.read_bytes()
        old_payload = self.root / 'data_persist/mx5-aa-dr/libmx5dr.so'
        old_payload.parent.mkdir()
        old_payload.write_bytes(b'legacy-payload-must-not-be-replaced')
        r = self.run_script('install.sh', ok=False)
        self.assertIn('run uninstall.sh first', r.stderr)
        self.assertEqual(old_payload.read_bytes(), b'legacy-payload-must-not-be-replaced')
        self.assertEqual(self.sm.read_bytes(), before)
        self.assertEqual(self.autostart.read_bytes(), self.original_autostart)

    def test_wrong_firmware_hash_no_launcher_writes(self):
        with (self.root / 'jci/aapa/blmjciaapa.so').open('ab') as f:
            f.write(b'wrong-hash')
        self.run_script('install.sh', ok=False)
        self.assertEqual(self.sm.read_bytes(), self.original)
        self.assertFalse((self.root / 'data_persist/mx5-aa-dr').exists())

    def test_wrong_payload_arch_and_checksum(self):
        (self.bundle / 'libmx5dr.so').write_bytes(b'not an ARM shared object')
        self.run_script('install.sh', ok=False)
        self.set_payload_hash()
        self.run_script('install.sh', ok=False)
        self.assertEqual(self.sm.read_bytes(), self.original)

    def test_explicit_wcp_and_pending_recovery(self):
        self.run_script('install.sh', '--with-wcp')
        self.assertEqual(self.preload(self.wcp), [])
        self.assertEqual(self.preload(self.trial.with_name('wcp.trial')), [TOKEN])
        pending = self.root / 'data_persist/mx5-aa-dr/pending'
        pending.write_text('sm.conf sm_WCP.conf\n')  # simulated interrupted commit
        self.run_script('install.sh', ok=False)
        self.run_script('uninstall.sh')
        self.assertFalse(pending.exists())
        self.assertEqual(self.sm.read_bytes(), self.original)
        self.assertEqual(self.wcp.read_bytes(), self.original_wcp)

    def test_invalid_second_config_stages_nothing_live(self):
        self.wcp.write_text(self.wcp.read_text().replace('name="jciAAPA"', 'name="unexpected"'))
        self.run_script('install.sh', '--with-wcp', ok=False)
        self.assertEqual(self.sm.read_bytes(), self.original)
        self.assertEqual(self.preload(), [])

    def test_duplicate_preload_rejected(self):
        self.add_touch()
        self.add_touch()
        before = self.sm.read_bytes()
        self.run_script('install.sh', ok=False)
        self.assertEqual(self.sm.read_bytes(), before)

    def test_mode_and_fixture_mount_guard(self):
        self.run_script('install.sh', '--mode=ASSIST', ok=False)
        self.run_script('install.sh', '--remount', ok=False)
        self.assertEqual(self.sm.read_bytes(), self.original)

    def test_export_is_local_and_preserves_source(self):
        self.run_script('install.sh')
        log = self.root / 'data_persist/mx5-aa-dr/logs/events.jsonl'
        log.write_text('{"test_only":true}\n')
        dest = Path(self.tmp.name) / 'usb'
        dest.mkdir()
        self.run_script('export_logs.sh', str(dest))
        self.assertEqual(len(list(dest.glob('*.tar'))), 1)
        self.assertEqual(len(list(dest.glob('*.sha256'))), 1)
        self.assertTrue(log.exists())


if __name__ == '__main__':
    unittest.main()
