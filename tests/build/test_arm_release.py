"""Release input checks; synthetic ELF headers here are never executed."""
import json
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
import build_arm  # noqa: E402
import make_usb_zip  # noqa: E402


def elf_header(name, machine=40, flags=0x05000000):
    kind = 3 if name.endswith('.so') else 2
    ident = b'\x7fELF\x01\x01\x01' + bytes(9)
    header = struct.pack('<HHIIIIIHHHHHH', kind, machine, 1, 0, 52, 0, flags,
                         52, 32, 1, 0, 0, 0)
    segment = struct.pack('<IIIIIIII', 1, 0, 0, 0, 84, 84, 5, 4)
    return ident + header + segment


class ArmReleaseTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='mx5dr-release-check-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.repo = self.root / 'repo'
        for name in ('src/example.h', 'Makefile', 'tools/build_arm.py',
                     'tools/fetch_m3_toolchain.py'):
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('input: ' + name + '\n')
        self.build = self.root / 'arm'
        self.build.mkdir()
        for name in build_arm.ARTIFACTS:
            (self.build / name).write_bytes(elf_header(name))
        # This record is a local integrity fixture, not a compiler attestation.
        self.record = dict(schema=1, source_files=build_arm.source_inputs(self.repo),
                           toolchain=dict(commit=build_arm.COMMIT),
                           artifacts={name: build_arm.digest(self.build / name)
                                      for name in build_arm.ARTIFACTS})
        self.write_record()

    def write_record(self):
        (self.build / build_arm.RECORD).write_text(json.dumps(self.record))

    def prepare_packaging(self):
        shutil.copytree(REPO / 'packaging', self.repo / 'packaging')
        for name in ('make_usb_zip.py', 'analyze_logs.py'):
            shutil.copyfile(REPO / 'tools' / name, self.repo / 'tools' / name)

    def package(self, output):
        def metadata(args, **kwargs):
            if args[:2] == ['git', 'rev-parse']:
                return 'synthetic-source-fixture\n'
            self.assertEqual(args[:2], ['git', 'status'])
            return ' M src/example.h\n'
        with mock.patch.object(make_usb_zip, 'REPO', self.repo), \
                mock.patch.object(make_usb_zip.subprocess, 'check_output', metadata), \
                mock.patch.object(sys, 'argv', ['make_usb_zip.py', '--build-dir', str(self.build),
                                               '--output', str(output)]):
            make_usb_zip.main()

    def test_matching_record_and_bytes(self):
        self.assertEqual(build_arm.verify_build(self.repo, self.build), self.record)

    def test_missing_build_record_rejected(self):
        (self.build / build_arm.RECORD).unlink()
        with self.assertRaisesRegex(ValueError, 'first run tools/build_arm.py'):
            build_arm.verify_build(self.repo, self.build)

    def test_header_changed_after_compilation_rejected(self):
        (self.repo / 'src/example.h').write_text('modified header\n')
        with self.assertRaisesRegex(ValueError, 'Build inputs differ'):
            build_arm.verify_build(self.repo, self.build)

    def test_added_compilation_input_rejected(self):
        (self.repo / 'src/new.cpp').write_text('new input\n')
        with self.assertRaisesRegex(ValueError, 'Build inputs differ'):
            build_arm.verify_build(self.repo, self.build)

    def test_mixed_artifact_rejected_even_with_valid_elf_header(self):
        path = self.build / 'libmx5dr.so'
        path.write_bytes(path.read_bytes() + b'other build')
        with self.assertRaisesRegex(ValueError, 'Artifact differs'):
            build_arm.verify_build(self.repo, self.build)

    def test_text_or_wrong_architecture_or_truncated_elf_rejected(self):
        path = self.build / 'libmx5dr.so'
        for data in (b'not ELF', elf_header(path.name, machine=62),
                     elf_header(path.name, flags=0x05000400),
                     elf_header(path.name)[:-1]):
            with self.subTest(data=data[:20]):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    build_arm.check_elf(path)

    def test_different_toolchain_record_rejected(self):
        self.record['toolchain']['commit'] = '0' * 40
        self.write_record()
        with self.assertRaisesRegex(ValueError, 'Build inputs differ'):
            build_arm.verify_build(self.repo, self.build)

    def test_fresh_build_never_reuses_an_existing_directory(self):
        result = subprocess.run([sys.executable, str(REPO / 'tools/build_arm.py'),
                                 '--build-dir', str(self.build)], text=True,
                                capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Build directory already exists', result.stderr)
        self.assertEqual(build_arm.verify_build(self.repo, self.build), self.record)

    def test_zip_builder_rejects_unrecorded_text_artifacts_without_output(self):
        (self.build / build_arm.RECORD).unlink()
        for name in build_arm.ARTIFACTS:
            (self.build / name).write_text('fake artifact\n')
        output = self.root / 'usb.zip'
        result = subprocess.run([sys.executable, str(REPO / 'tools/make_usb_zip.py'),
                                 '--build-dir', str(self.build), '--output', str(output)],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Missing arm-build.json', result.stderr)
        self.assertFalse(output.exists())
        self.assertFalse(output.with_suffix('.zip.sha256').exists())

    def test_fresh_build_uses_recorded_makefile_even_with_gnumakefile(self):
        # Exercise the actual make file-selection behavior. These recipes copy
        # validation-only ELF fixtures; no ARM compiler execution is claimed.
        copies = ''.join('\tcp ' + shlex.quote(str(self.build / name)) +
                         ' "$(BUILD)/' + name + '"\n' for name in build_arm.ARTIFACTS)
        selected = self.repo / 'recorded-makefile-selected'
        unexpected = self.repo / 'unrecorded-makefile-selected'
        (self.repo / 'Makefile').write_text('arm:\n' + copies +
            '\ttouch ' + shlex.quote(str(selected)) + '\n')
        (self.repo / 'GNUmakefile').write_text('arm:\n' + copies +
            '\ttouch ' + shlex.quote(str(unexpected)) + '\n')
        output = self.root / 'fresh-arm'
        with mock.patch.object(build_arm, 'REPO', self.repo), \
                mock.patch.object(build_arm, 'verify_toolchain', return_value=self.record['toolchain']), \
                mock.patch.object(build_arm, 'check_attributes', return_value={'validation_fixture': True}), \
                mock.patch.object(sys, 'argv', ['build_arm.py', '--build-dir', str(output),
                                               '--toolchain', str(self.root / 'unused-toolchain')]):
            build_arm.main()
        self.assertTrue(selected.exists(), 'The recorded Makefile was not executed')
        self.assertFalse(unexpected.exists(), 'An unrecorded GNUmakefile was executed')
        self.assertEqual(build_arm.verify_build(self.repo, output)['source_files'],
                         build_arm.source_inputs(self.repo))

    def test_zip_matching_validation_fixture_succeeds(self):
        self.prepare_packaging()
        output = self.root / 'matched-fixture.zip'
        self.package(output)
        self.assertTrue(output.is_file())
        self.assertTrue(output.with_suffix('.zip.sha256').is_file())

    def test_zip_header_edit_after_initial_verification_is_rejected(self):
        self.prepare_packaging()
        output = self.root / 'changed-header.zip'
        def verify_then_edit(repo, build):
            record = build_arm.verify_build(repo, build)
            (self.repo / 'src/example.h').write_text('edited after build verification\n')
            return record
        with mock.patch.object(make_usb_zip, 'verify_build', verify_then_edit):
            with self.assertRaisesRegex(RuntimeError, 'Compiled sources changed before packaging'):
                self.package(output)
        self.assertFalse(output.exists())
        self.assertFalse(output.with_suffix('.zip.sha256').exists())

    def test_zip_new_input_added_during_packaging_is_rejected(self):
        self.prepare_packaging()
        output = self.root / 'added-input.zip'
        run = subprocess.run
        def package_then_add(*args, **kwargs):
            result = run(*args, **kwargs)
            (self.repo / 'packaging/new-input.txt').write_text('added during packaging\n')
            return result
        with mock.patch.object(make_usb_zip.subprocess, 'run', package_then_add):
            with self.assertRaisesRegex(RuntimeError, 'Sources changed while packaging'):
                self.package(output)
        self.assertFalse(output.exists())
        self.assertFalse(output.with_suffix('.zip.sha256').exists())


if __name__ == '__main__':
    unittest.main()
