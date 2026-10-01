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
TAP_TOKEN = '/data_persist/mx5-aa-dr/libmx5dr-vimtap.so'
LDS_TOKEN = '/data_persist/mx5-aa-dr/libmx5dr-ldstap.so'
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
        (self.bundle / 'libmx5dr-vimtap.so').write_bytes(fake)
        (self.bundle / 'libmx5dr-vimtap.so.sha256').write_text(
            hashlib.sha256(fake).hexdigest() + '  libmx5dr-vimtap.so\n')
        (self.bundle / 'libmx5dr-ldstap.so').write_bytes(fake)
        (self.bundle / 'libmx5dr-ldstap.so.sha256').write_text(
            hashlib.sha256(fake).hexdigest() + '  libmx5dr-ldstap.so\n')
        fake[16] = 2  # Separate test-only ARM ET_EXEC header; never run.
        (self.bundle / 'mx5dr-collector').write_bytes(fake)
        digest = hashlib.sha256(fake).hexdigest()
        (self.bundle / 'mx5dr-collector.sha256').write_text(digest + '  mx5dr-collector\n')
        (self.bundle / 'mx5dr-guard').write_bytes(fake)
        (self.bundle / 'mx5dr-guard.sha256').write_text(hashlib.sha256(fake).hexdigest() + '  mx5dr-guard\n')
        (self.bundle / 'mx5dr-sha256').write_bytes(fake)  # never run; host hash tool used
        (self.bundle / 'mx5dr-sha256.sha256').write_text(hashlib.sha256(fake).hexdigest() + '  mx5dr-sha256\n')
        files = [line.split()[1] for line in (PACK / 'firmware.sha256').read_text().splitlines()]
        files += ['jci/version.ini', 'jci/sm/sm.conf', 'jci/sm/sm_WCP.conf', 'usr/bin/autostart']
        for file in files:
            source = getattr(self, 'stock_root', STOCK) / file
            if not source.exists():
                self.skipTest(f'Stock identity fixture unavailable: {source}')
            target = self.root / file
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        if hasattr(self, 'synthetic_manifest'):
            (self.bundle / 'firmware.sha256').write_text(self.synthetic_manifest)
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

    def preload(self, path=None, service_name='jciAAPA'):
        tree = ET.parse(path or self.sm)
        service = tree.find(".//service[@name='" + service_name + "']")
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
        self.assertEqual(self.preload(self.trial, 'jciVBS'), [])
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
        release = Path(os.environ.get('MX5DR_RELEASE_BUNDLE', str(REPO / 'bundle'))) / 'libmx5dr.so'
        if not release.exists() or not (release.parent / 'libmx5dr-ldstap.so').exists():
            self.skipTest('Current six-artifact release bundle has not been built')
        artifacts = ('libmx5dr.so', 'libmx5dr-vimtap.so', 'libmx5dr-ldstap.so', 'mx5dr-collector',
                     'mx5dr-guard', 'mx5dr-sha256')
        for name in artifacts:
            artifact = release.parent / name
            self.assertTrue(artifact.is_file(), str(artifact))
            shutil.copyfile(artifact, self.bundle / name)
            digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
            (self.bundle / (name + '.sha256')).write_text(digest + '  ' + name + '\n')
        self.add_touch(multiline=True)
        self.run_script('install.sh')
        installed = self.root / 'data_persist/mx5-aa-dr'
        for name in artifacts:
            relative = {'mx5dr-guard': 'guard/mx5dr-guard',
                        'mx5dr-sha256': 'tools/mx5dr-sha256'}.get(name, name)
            self.assertEqual((installed / relative).read_bytes(),
                             (release.parent / name).read_bytes(), name)
        self.assertEqual(self.preload(), [TOUCH])
        self.assertEqual(self.preload(self.trial), [TOKEN + ':' + TOUCH])
        self.assertEqual(self.preload(self.trial, 'jciVBS'), [])
        self.run_script('uninstall.sh')
        self.assertEqual(self.preload(), [TOUCH])

    def test_bundle_manifest_is_checked_without_separate_user_command(self):
        manifest = self.bundle / 'SHA256SUMS'
        content = self.bundle / 'start_collector.sh'
        manifest.write_text(hashlib.sha256(content.read_bytes()).hexdigest() + '  start_collector.sh\n')
        self.run_script('install.sh')
        self.run_script('uninstall.sh')
        content.write_text(content.read_text() + '\n# synthetic damage\n')
        result = self.run_script('install.sh', ok=False)
        self.assertIn('Bundle checksum mismatch', result.stderr)
        self.assertEqual(self.sm.read_bytes(), self.original)
        self.assertEqual(self.autostart.read_bytes(), self.original_autostart)

    def test_symlink_manifest_rejected_before_installation(self):
        source = self.root / 'checksums'
        source.write_text('')
        (self.bundle / 'SHA256SUMS').symlink_to(source)
        self.run_script('install.sh', ok=False)
        self.assertEqual(self.autostart.read_bytes(), self.original_autostart)

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

    def test_shadow_trial_wraps_both_services_and_rearm_removes_tap(self):
        self.add_touch(multiline=True)
        other = '/data_persist/vbs-a.so:/data_persist/vbs-b.so'
        for path in (self.sm, self.wcp):
            text = path.read_text()
            start = text.index('name="jciVBS"')
            end = text.index('</service>', start)
            path.write_text(text[:end] + '<environ_var env_name="LD_PRELOAD"\n'
                            ' env_value="' + other + '"/>\n' + text[end:])
        baseline = [path.read_bytes() for path in (self.sm, self.wcp)]
        self.run_script('install.sh', '--mode=SHADOW')
        for path, before in zip((self.sm, self.wcp), baseline):
            self.assertEqual(path.read_bytes(), before)
        for path in (self.trial, self.trial.with_name('wcp.trial')):
            self.assertEqual(self.preload(path, 'jciVBS'), [TAP_TOKEN + ':' + other])
            self.assertNotIn(TAP_TOKEN, self.preload(path)[0])
        self.assertEqual(self.preload(self.trial), [TOKEN + ':' + TOUCH])
        for mode in ('OBSERVE', 'SCRUB'):
            self.run_script('arm.sh', '--mode=' + mode)
            for path in (self.trial, self.trial.with_name('wcp.trial')):
                self.assertEqual(self.preload(path, 'jciVBS'), [other])
        self.run_script('arm.sh', '--mode=SHADOW')
        self.assertEqual(self.preload(self.trial, 'jciVBS'), [TAP_TOKEN + ':' + other])
        self.run_script('uninstall.sh')
        for path, before in zip((self.sm, self.wcp), baseline):
            self.assertEqual(path.read_bytes(), before)

    def test_shadow_bad_vbs_identity_does_not_publish_startup_or_payload(self):
        self.wcp.write_text(self.wcp.read_text().replace(
            'path="/jci/vbs/svcjcivbs.so"', 'path="/jci/vbs/different.so"'))
        before = self.wcp.read_bytes()
        self.run_script('install.sh', '--mode=SHADOW', ok=False)
        self.assertEqual(self.sm.read_bytes(), self.original)
        self.assertEqual(self.wcp.read_bytes(), before)
        self.assertEqual(self.autostart.read_bytes(), self.original_autostart)
        self.assertFalse((self.root / 'data_persist/mx5-aa-dr/libmx5dr-vimtap.so').exists())

    def test_tap_checksum_and_arch_checked_before_installation(self):
        tap = self.bundle / 'libmx5dr-vimtap.so'
        original = tap.read_bytes()
        tap.write_bytes(original + b'corrupt')
        self.run_script('install.sh', ok=False)
        fake = bytearray(original)
        fake[18] = 3  # x86 instead of ARM, with matching checksum.
        tap.write_bytes(fake)
        tap.with_name(tap.name + '.sha256').write_text(hashlib.sha256(fake).hexdigest() + '\n')
        self.run_script('install.sh', ok=False)
        self.assertEqual(self.autostart.read_bytes(), self.original_autostart)
        self.assertFalse((self.root / 'data_persist/mx5-aa-dr').exists())

    def test_permanent_vbs_tap_rejected_then_removed_by_token_only(self):
        other = TAP_TOKEN + '.backup:/data_persist/vbs-later.so'
        text = self.sm.read_text()
        start = text.index('name="jciVBS"')
        end = text.index('</service>', start)
        self.sm.write_text(text[:end] + '<environ_var env_name="LD_PRELOAD" env_value="' +
                           TAP_TOKEN + ':' + other + '"/>' + text[end:])
        before = self.sm.read_bytes()
        old_tap = self.root / 'data_persist/mx5-aa-dr/libmx5dr-vimtap.so'
        old_tap.parent.mkdir()
        old_tap.write_bytes(b'mapped old tap')
        self.run_script('install.sh', '--mode=SHADOW', ok=False)
        self.assertEqual(self.sm.read_bytes(), before)
        self.assertEqual(old_tap.read_bytes(), b'mapped old tap')
        self.run_script('uninstall.sh')
        self.assertEqual(self.preload(self.sm, 'jciVBS'), [other])
        self.assertEqual(old_tap.read_bytes(), b'mapped old tap')

    def test_bundle_requires_and_hashes_tap(self):
        dest = Path(self.tmp.name) / 'new-release'
        command = ['sh', str(PACK / 'make_bundle.sh'), str(self.bundle / 'libmx5dr.so'), str(dest)]
        tap = self.bundle / 'libmx5dr-vimtap.so'
        data = tap.read_bytes()
        tap.unlink()
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(dest.exists())
        tap.write_bytes(data)
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((dest / tap.name).read_bytes(), data)
        self.assertEqual((dest / (tap.name + '.sha256')).read_text().split()[0],
                         hashlib.sha256(data).hexdigest())

    def test_lds_observation_in_each_active_mode_preserves_other_preloads(self):
        other = '/data_persist/lds-existing.so'
        for path in (self.sm, self.wcp):
            value = path.read_text()
            end = value.index('</service>', value.index('name="jciLDS"'))
            path.write_text(value[:end] + '<environ_var env_name="LD_PRELOAD" env_value="' +
                            other + '"/>' + value[end:])
        baseline = [path.read_bytes() for path in (self.sm, self.wcp)]
        for index, mode in enumerate(('OBSERVE', 'SCRUB', 'SHADOW')):
            self.run_script('install.sh' if index == 0 else 'arm.sh', '--mode=' + mode)
            for path in (self.trial, self.trial.with_name('wcp.trial')):
                self.assertEqual(self.preload(path, 'jciLDS'), [LDS_TOKEN + ':' + other])
                self.assertEqual(self.preload(path), [TOKEN])
                self.assertEqual(self.preload(path, 'jciVBS'), [TAP_TOKEN] if mode == 'SHADOW' else [])
        installed = self.root / LDS_TOKEN.lstrip('/')
        self.assertEqual(installed.read_bytes(), (self.bundle / installed.name).read_bytes())
        self.run_script('uninstall.sh')
        self.assertEqual([p.read_bytes() for p in (self.sm, self.wcp)], baseline)
        self.assertTrue(installed.exists())

    def test_lds_off_template_has_no_observation_preload(self):
        self.run_script('install.sh', '--mode=OFF')
        for path in (self.trial, self.trial.with_name('wcp.trial')):
            self.assertEqual(self.preload(path, 'jciLDS'), [])

    def test_lds_wrong_service_identity_rejected_before_startup(self):
        self.wcp.write_text(self.wcp.read_text().replace(
            'path="/jci/lds/svcjcilds.so"', 'path="/jci/lds/different.so"'))
        self.run_script('install.sh', '--mode=OBSERVE', ok=False)
        self.assertEqual(self.autostart.read_bytes(), self.original_autostart)
        self.assertFalse((self.root / LDS_TOKEN.lstrip('/')).exists())

    def test_lds_payload_checksum_arch_and_absence_checked_before_writes(self):
        tap = self.bundle / 'libmx5dr-ldstap.so'
        original = tap.read_bytes()
        for data in (original + b'wrong checksum', b'missing'):
            tap.write_bytes(data)
            if data == b'missing':
                tap.unlink()
            self.run_script('install.sh', ok=False)
        data = bytearray(original)
        data[18] = 3
        tap.write_bytes(data)
        tap.with_name(tap.name + '.sha256').write_text(hashlib.sha256(data).hexdigest() + '\n')
        self.run_script('install.sh', ok=False)
        self.assertEqual(self.autostart.read_bytes(), self.original_autostart)
        self.assertFalse((self.root / 'data_persist/mx5-aa-dr').exists())

    def test_lds_persistent_token_removal_preserves_unowned_similar_names(self):
        other = LDS_TOKEN + '.backup:/data_persist/lds-existing.so'
        value = self.sm.read_text()
        end = value.index('</service>', value.index('name="jciLDS"'))
        self.sm.write_text(value[:end] + '<environ_var env_name="LD_PRELOAD" env_value="' +
                           LDS_TOKEN + ':' + other + '"/>' + value[end:])
        before = self.sm.read_bytes()
        tap = self.root / LDS_TOKEN.lstrip('/')
        tap.parent.mkdir()
        tap.write_bytes(b'mapped original tap')
        self.run_script('install.sh', ok=False)
        self.assertEqual(self.sm.read_bytes(), before)
        self.run_script('uninstall.sh')
        self.assertEqual(self.preload(self.sm, 'jciLDS'), [other])
        self.assertEqual(tap.read_bytes(), b'mapped original tap')

    def test_lds_missing_installed_payload_cannot_rearm(self):
        self.run_script('install.sh')
        tap = self.root / LDS_TOKEN.lstrip('/')
        if tap.exists():
            tap.unlink()
        self.run_script('arm.sh', ok=False)

    def test_lds_bundle_requires_and_hashes_product(self):
        tap = self.bundle / 'libmx5dr-ldstap.so'
        original = tap.read_bytes()
        tap.unlink()
        dest = Path(self.tmp.name) / 'lds-bundle'
        command = ['sh', str(PACK / 'make_bundle.sh'), str(self.bundle / 'libmx5dr.so'), str(dest)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(dest.exists())
        tap.write_bytes(original)
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((dest / tap.name).read_bytes(), original)
        self.assertEqual((dest / (tap.name + '.sha256')).read_text().split()[0],
                         hashlib.sha256(original).hexdigest())

    def test_lds_factory_libdbus_link_allows_install_and_rearm(self):
        alias = self.root / 'usr/lib/libdbus-1.so.3'
        target = alias.with_name('libdbus-1.so.3.7.2')
        if hasattr(self, 'synthetic_manifest'):
            # The authored fixture has only manifest inputs. Recreate the same
            # alias shape without treating its bytes as original firmware.
            if not target.exists():
                alias.rename(target)
            if alias.exists() or alias.is_symlink():
                alias.unlink()
            alias.symlink_to(target.name)
        else:
            # setUp's copyfile follows links; restore the actual factory alias
            # and target here so it cannot hide a SONAME regular-file rejection.
            stock_alias = STOCK / 'usr/lib/libdbus-1.so.3'
            self.assertTrue(stock_alias.is_symlink())
            self.assertEqual(os.readlink(stock_alias), target.name)
            if alias.exists() or alias.is_symlink():
                alias.unlink()
            shutil.copy2(stock_alias, alias, follow_symlinks=False)
            shutil.copy2(stock_alias.with_name(target.name), target, follow_symlinks=False)
        self.assertTrue(alias.is_symlink())
        self.assertTrue(target.is_file())
        self.assertFalse(target.is_symlink())
        before = target.read_bytes()
        self.run_script('install.sh', '--mode=SHADOW')
        self.run_script('arm.sh', '--mode=SHADOW')
        for trial in (self.trial, self.trial.with_name('wcp.trial')):
            self.assertEqual(self.preload(trial, 'jciLDS'), [LDS_TOKEN])
        self.assertTrue(alias.is_symlink())
        self.assertEqual(os.readlink(alias), target.name)
        self.assertEqual(target.read_bytes(), before)

    def test_lds_original_dependency_changes_are_rejected_before_writes(self):
        paths = ('jci/lds/svcjcilds.so', 'jci/lib/libjcilds-dbus.so',
                 'jci/lib/libjcilds-driver.so', 'jci/lib/libjcidbus.so',
                 'jci/lib/libjcicommon.so', 'usr/lib/libdbus-1.so.3.7.2')
        for relative in paths:
            with self.subTest(relative=relative):
                path = self.root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                original = path.read_bytes() if path.exists() else None
                path.write_bytes(b'replaced original LDS dependency')
                self.run_script('install.sh', ok=False)
                self.assertEqual(self.autostart.read_bytes(), self.original_autostart)
                self.assertFalse((self.root / 'data_persist/mx5-aa-dr').exists())
                if original is None:
                    path.unlink()
                else:
                    path.write_bytes(original)


if __name__ == '__main__':
    unittest.main()
