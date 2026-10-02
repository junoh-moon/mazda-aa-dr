"""Release input checks; synthetic ELF headers here are never executed."""
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

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
                mock.patch.object(make_usb_zip, 'source_modified', return_value=True, create=True), \
                mock.patch.object(sys, 'argv', ['make_usb_zip.py', '--build-dir', str(self.build),
                                               '--output', str(output)]):
            make_usb_zip.main()

    def test_zip_carries_usb_startup_diagnostic_helper(self):
        self.prepare_packaging()
        # This fixture tests shipping the helper, not its diagnostic behavior.
        helper = self.repo / 'packaging/startup_diagnostics.sh'
        helper.write_text('#!/bin/sh\necho diagnostic_schema=1\n')
        reboot = self.repo / 'packaging/reboot_cmu.sh'
        reboot.write_text('#!/bin/sh\necho reboot_fixture_only\n')
        output = self.root / 'diagnostic.zip'
        self.package(output)
        with zipfile.ZipFile(output) as archive:
            self.assertIn('startup_diagnostics.sh', archive.namelist())
            self.assertEqual(archive.read('startup_diagnostics.sh'), helper.read_bytes())
            self.assertIn('  startup_diagnostics.sh\n', archive.read('SHA256SUMS').decode())
            self.assertEqual(archive.read('reboot_cmu.sh'), reboot.read_bytes())
            self.assertIn('  reboot_cmu.sh\n', archive.read('SHA256SUMS').decode())

    def test_matching_record_and_bytes(self):
        self.assertEqual(build_arm.verify_build(self.repo, self.build), self.record)

    def test_lds_product_is_recorded_and_shipped_with_its_checksum(self):
        name = 'libmx5dr-ldstap.so'
        self.assertIn(name, build_arm.ARTIFACTS)
        self.prepare_packaging()
        output = self.root / 'lds.zip'
        self.package(output)
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(archive.read(name), (self.build / name).read_bytes())
            self.assertIn(name + '.sha256', archive.namelist())
            self.assertIn('  ' + name + '\n', archive.read('SHA256SUMS').decode())

    def test_build_environment_removes_ld_run_path(self):
        with mock.patch.dict(os.environ, {'LD_RUN_PATH': '/unexpected/library/path'}, clear=True):
            self.assertFalse('LD_RUN_PATH' in build_arm.build_environment())

    def test_attributes_reject_rpath_and_runpath(self):
        report = ('  Tag_CPU_arch: v7\n'
                  ' 0x00000001 (NEEDED) Shared library: [libc.so.6]\n')
        path = self.build / 'libmx5dr.so'
        with mock.patch.object(build_arm.subprocess, 'check_output', return_value=report):
            self.assertEqual(build_arm.check_attributes(path, 'readelf')['needed'], ['libc.so.6'])
        for tag in ('RPATH', 'RUNPATH'):
            with self.subTest(tag=tag), \
                    mock.patch.object(build_arm.subprocess, 'check_output',
                                      return_value=report + ' 0x0000000f (' + tag + ') [/unexpected]\n'):
                with self.assertRaisesRegex(ValueError, 'RPATH|RUNPATH'):
                    build_arm.check_attributes(path, 'readelf')

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

    def test_small_thread_rejects_large_preload_tls(self):
        for name, limit in (('libmx5dr-ldstap.so', build_arm.LDS_TLS_REGRESSION_LIMIT),
                            ('libmx5dr.so', build_arm.AA_TLS_REGRESSION_LIMIT)):
            with self.subTest(name=name):
                path = self.build / name
                data = bytearray(elf_header(path.name))
                # Add an ELF32 PT_TLS header without changing the synthetic PT_LOAD.
                struct.pack_into('<H', data, 44, 2)
                tls_offset = len(data)
                data.extend(struct.pack('<IIIIIIII', 7, tls_offset, 0, 0, 0,
                                        limit + 1, 4, 8))
                path.write_bytes(data)
                with self.assertRaisesRegex(ValueError, 'thread-local storage exceeds regression limit'):
                    build_arm.check_elf(path)
                struct.pack_into('<I', data, tls_offset + 20, limit - 7)
                path.write_bytes(data)
                build_arm.check_elf(path)
                struct.pack_into('<I', data, tls_offset + 20, limit - 6)
                path.write_bytes(data)
                with self.assertRaisesRegex(ValueError, 'thread-local storage exceeds regression limit'):
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
        output = self.root / 'published' / 'matched-fixture.zip'
        self.package(output)
        self.assertTrue(output.is_file())
        sidecar = output.with_suffix('.zip.sha256')
        self.assertEqual(sidecar.read_text(), build_arm.digest(output) + '  ' + output.name + '\n')
        with zipfile.ZipFile(output) as archive:
            self.assertIsNone(archive.testzip())
            self.assertIn('build-info.json', archive.namelist())
        self.assertEqual(set(output.parent.iterdir()), {output, sidecar})

    def test_zip_late_integrity_failure_leaves_no_outputs_or_temporary_files(self):
        self.prepare_packaging()
        output = self.root / 'published' / 'failed.zip'
        with mock.patch.object(zipfile.ZipFile, 'testzip', return_value='install.sh'):
            with self.assertRaisesRegex(RuntimeError, 'ZIP integrity failure'):
                self.package(output)
        self.assertEqual(list(output.parent.iterdir()), [])

    def test_zip_interruption_leaves_no_outputs_or_temporary_files(self):
        self.prepare_packaging()
        output = self.root / 'published' / 'interrupted.zip'
        with mock.patch.object(zipfile.ZipFile, 'testzip', side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.package(output)
        self.assertEqual(list(output.parent.iterdir()), [])

    def test_zip_concurrent_output_is_never_overwritten(self):
        self.prepare_packaging()
        output = self.root / 'published' / 'competing.zip'
        run = subprocess.run
        def create_competing_output(*args, **kwargs):
            result = run(*args, **kwargs)
            output.parent.mkdir()
            output.write_bytes(b'other publisher output')
            return result
        with mock.patch.object(make_usb_zip.subprocess, 'run', create_competing_output):
            with self.assertRaises(FileExistsError):
                self.package(output)
        self.assertEqual(output.read_bytes(), b'other publisher output')
        self.assertEqual(set(output.parent.iterdir()), {output})

    def test_zip_concurrent_sidecar_is_never_overwritten(self):
        self.prepare_packaging()
        output = self.root / 'published' / 'competing.zip'
        sidecar = output.with_suffix('.zip.sha256')
        run = subprocess.run
        def create_competing_sidecar(*args, **kwargs):
            result = run(*args, **kwargs)
            output.parent.mkdir()
            sidecar.write_bytes(b'other publisher checksum')
            return result
        with mock.patch.object(make_usb_zip.subprocess, 'run', create_competing_sidecar):
            with self.assertRaises(FileExistsError):
                self.package(output)
        self.assertEqual(sidecar.read_bytes(), b'other publisher checksum')
        self.assertEqual(set(output.parent.iterdir()), {sidecar})

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


class SourceModifiedTests(unittest.TestCase):
    """Use a real isolated Git index, including flags that hide changes."""
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='mx5dr-source-check-')
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name) / 'repo'
        subprocess.run(['git', 'clone', '--quiet', '--shared', '--no-checkout',
                        str(REPO), str(self.repo)], check=True)
        self.paths = ['src', 'packaging', *make_usb_zip.EXTRA_SOURCES]
        self.git('checkout', 'HEAD', '--', *self.paths)
        self.commit = self.git('rev-parse', 'HEAD').strip()

    def git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.repo, text=True)

    def modified(self):
        with mock.patch.object(make_usb_zip, 'REPO', self.repo):
            return make_usb_zip.source_modified(make_usb_zip.source_files(), self.commit)

    def assert_status_clean(self):
        self.assertEqual(self.git('status', '--porcelain', '--', *self.paths), '')

    def test_unchanged_sources_are_not_modified(self):
        self.assert_status_clean()
        self.assertFalse(self.modified())

    def hidden_tracked_change(self, flag):
        name = 'src/core/dr_core.c'
        self.git('update-index', flag, name)
        with (self.repo / name).open('a') as stream:
            stream.write('\n/* changed but hidden from git status */\n')
        self.assert_status_clean()
        self.assertTrue(self.modified())

    def test_assume_unchanged_does_not_hide_source_modification(self):
        self.hidden_tracked_change('--assume-unchanged')

    def test_skip_worktree_does_not_hide_source_modification(self):
        self.hidden_tracked_change('--skip-worktree')

    def test_untracked_configuration_does_not_hide_new_input(self):
        self.git('config', 'status.showUntrackedFiles', 'no')
        (self.repo / 'src/new-input.c').write_text('/* new compilation input */\n')
        self.assert_status_clean()
        self.assertTrue(self.modified())

    def test_ignored_source_is_still_modified(self):
        (self.repo / '.git/info/exclude').write_text('src/ignored-input.c\n')
        (self.repo / 'src/ignored-input.c').write_text('/* ignored compilation input */\n')
        self.assert_status_clean()
        self.assertTrue(self.modified())

    def test_missing_tracked_source_is_modified(self):
        (self.repo / 'src/core/dr_core.c').unlink()
        self.assertTrue(self.modified())

    def test_git_replace_cannot_hide_changed_commit_input(self):
        name = 'src/core/dr_core.c'
        original = self.git('rev-parse', self.commit + ':' + name).strip()
        with (self.repo / name).open('a') as stream:
            stream.write('\n/* different from the original commit */\n')
        replacement = self.git('hash-object', '-w', name).strip()
        self.assertTrue(self.modified())
        self.git('replace', original, replacement)
        self.assertEqual(self.git('rev-parse', 'HEAD').strip(), self.commit)
        self.assertTrue(self.modified())

    def test_zip_metadata_uses_blob_comparison_for_hidden_modification(self):
        self.hidden_tracked_change('--assume-unchanged')
        build = self.repo.parent / 'arm'
        build.mkdir()
        for name in build_arm.ARTIFACTS:
            (build / name).write_bytes(elf_header(name))
        record = dict(schema=1, source_files=build_arm.source_inputs(self.repo),
                      toolchain=dict(commit=build_arm.COMMIT),
                      artifacts={name: build_arm.digest(build / name)
                                 for name in build_arm.ARTIFACTS})
        (build / build_arm.RECORD).write_text(json.dumps(record))
        output = self.repo.parent / 'hidden-change.zip'
        # Only the ELF bytes are fixtures: Git, the bundle script, metadata
        # selection and final ZIP verification execute their production paths.
        with mock.patch.object(make_usb_zip, 'REPO', self.repo), \
                mock.patch.object(sys, 'argv', ['make_usb_zip.py', '--build-dir', str(build),
                                               '--output', str(output)]):
            make_usb_zip.main()
        with zipfile.ZipFile(output) as archive:
            info = json.loads(archive.read('build-info.json'))
        self.assertEqual(info['source_commit'], self.commit)
        self.assertIs(info['source_modified'], True)


if __name__ == '__main__':
    unittest.main()
