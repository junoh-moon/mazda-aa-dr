"""Authorized-shell packaging uses authored binaries and no entry payload."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('shell_zip', ROOT / 'tools/make_usb_zip.py')
zipper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(zipper)
PRODUCTS = ('libmx5dr.so', 'libmx5dr-vimtap.so', 'libmx5dr-ldstap.so',
            'mx5dr-collector', 'mx5dr-guard', 'mx5dr-sha256')
HELPERS = ('trial install.sh uninstall.sh purge.sh export_logs.sh common.sh edit_service.awk '
           'edit_autostart.awk arm.sh start_collector.sh stop_collector.sh '
           'finish_capture.sh trial_status.sh trial_status.awk startup_diagnostics.sh '
           'reboot_cmu.sh firmware.sha256 mx5dr.conf').split()


class ShellBundle(unittest.TestCase):
    def test_cli_installer_files_modes_and_no_entry(self):
        for mode in ('OBSERVE', 'SHADOW', 'BETA'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                base = Path(tmp)
                for name in PRODUCTS:
                    (base / name).write_bytes(('authored ' + name).encode())
                output = base / 'bundle'
                result = subprocess.run(['sh', str(ROOT / 'packaging/make_bundle.sh'),
                                         '--shell-only', '--default-mode=' + mode,
                                         str(base / PRODUCTS[0]), str(output)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                expected = set(HELPERS) | set(PRODUCTS) | {n + '.sha256' for n in PRODUCTS}
                expected |= {'bundle-default-mode', 'INSTALL_KO.md'}
                self.assertEqual({p.name for p in output.iterdir()}, expected)
                self.assertEqual((output / 'bundle-default-mode').read_text(), mode + '\n')
                self.assertEqual((output / 'INSTALL_KO.md').read_bytes(),
                                 (ROOT / 'packaging/SHELL_START_KO.md').read_bytes())
                for name in PRODUCTS:
                    data = (base / name).read_bytes()
                    self.assertEqual((output / name).read_bytes(), data)
                    self.assertEqual((output / (name + '.sha256')).read_text(),
                                     hashlib.sha256(data).hexdigest() + '  ' + name + '\n')

    def archive(self, base, extra=None):
        bundle = base / 'bundle'
        bundle.mkdir()
        for name in ('trial', 'install.sh', 'INSTALL_KO.md', *PRODUCTS):
            (bundle / name).write_text('authored fixture\n')
        for name in PRODUCTS:
            (bundle / (name + '.sha256')).write_text(
                hashlib.sha256((bundle / name).read_bytes()).hexdigest() + '  ' + name + '\n')
        (bundle / 'build-info.json').write_text(json.dumps({
            'bundle_type': 'shell-only', 'entry_payloads_included': False}))
        if extra:
            path = bundle / extra
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('authored forbidden-name control; not an entry payload\n')
        paths = sorted(p for p in bundle.glob('*') if p.is_file())
        if extra and '/' in extra:
            paths.append(bundle / extra)
        (bundle / 'SHA256SUMS').write_text(''.join(
            hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + str(p.relative_to(bundle)) + '\n'
            for p in paths))
        return bundle

    def test_shell_zip_contains_verified_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            bundle = self.archive(base)
            output, checksum = base / 'shell.zip', base / 'shell.zip.sha256'
            zipper.write_verified_zip(bundle, output, checksum, shell_only=True)
            with zipfile.ZipFile(output) as archive:
                self.assertIsNone(archive.testzip())
                self.assertIn('trial', archive.namelist())
            self.assertEqual(checksum.read_text(), hashlib.sha256(output.read_bytes()).hexdigest()
                             + '  shell.zip\n')

    def test_shell_zip_rejects_entry_names_before_publication(self):
        for name in ('mp3/control.txt', 'js/control.txt', 'USB_ENTRY_NOTICE.md', 'USB_START_KO.md'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                base = Path(tmp)
                bundle = self.archive(base, name)
                output, checksum = base / 'shell.zip', base / 'shell.zip.sha256'
                with self.assertRaisesRegex(RuntimeError, 'entry'):
                    zipper.write_verified_zip(bundle, output, checksum, shell_only=True)
                self.assertFalse(output.exists())
                self.assertFalse(checksum.exists())

    def test_shell_zip_requires_every_product_and_checksum(self):
        for name in (*PRODUCTS, *(n + '.sha256' for n in PRODUCTS)):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                base = Path(tmp)
                bundle = self.archive(base)
                (bundle / name).unlink()
                lines = (bundle / 'SHA256SUMS').read_text().splitlines(keepends=True)
                (bundle / 'SHA256SUMS').write_text(''.join(
                    line for line in lines if line.split('  ', 1)[1].strip() != name))
                with self.assertRaises(RuntimeError):
                    zipper.write_verified_zip(bundle, base / 'out.zip', base / 'out.zip.sha256',
                                              shell_only=True)
                self.assertFalse((base / 'out.zip').exists())

    def test_shell_zip_requires_shell_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            bundle = self.archive(base)
            (bundle / 'build-info.json').write_text('{}')
            with self.assertRaisesRegex(RuntimeError, 'metadata'):
                zipper.write_verified_zip(bundle, base / 'out.zip', base / 'out.zip.sha256',
                                          shell_only=True)

    def test_shell_source_identity_ignores_only_unused_entry_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            paths = ('src/authored.c', 'packaging/SHELL_START_KO.md',
                     'packaging/USB_START_KO.md', 'packaging/usb-entry/control.txt',
                     *zipper.EXTRA_SOURCES)
            for name in paths:
                path = repo / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('authored source-identity fixture\n')
            def git(*args):
                return subprocess.check_output(['git', *args], cwd=repo, stderr=subprocess.PIPE,
                                               text=True).strip()
            git('init', '-q')
            git('add', '.')
            git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                'commit', '-qm', 'Authored fixture')
            commit = git('rev-parse', 'HEAD')
            with mock.patch.object(zipper, 'REPO', repo):
                def changed():
                    return zipper.source_modified(zipper.source_files(True), commit, True)
                self.assertFalse(changed())
                self.assertNotIn('packaging/usb-entry/control.txt', zipper.source_files(True))
                (repo / 'packaging/USB_START_KO.md').write_text('changed unused fixture\n')
                self.assertFalse(changed())
                git('update-index', '--assume-unchanged', 'packaging/SHELL_START_KO.md')
                (repo / 'packaging/SHELL_START_KO.md').write_text('changed included guide\n')
                self.assertTrue(changed())


if __name__ == '__main__':
    unittest.main()
