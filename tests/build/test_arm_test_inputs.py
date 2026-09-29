"""Bind ARM test metadata to a verified build and matching loader inputs."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import check_arm_test_inputs as inputs  # noqa: E402


class ArmTestInputs(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='mx5dr-arm-inputs-')
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name).resolve()
        self.build = self.root / 'release'
        self.build.mkdir()
        self.library = self.build / 'libmx5dr.so'
        self.library.write_bytes(b'input identity fixture, never executed')
        self.toolchain = self.root / 'compiler'
        (self.toolchain / 'bin').mkdir(parents=True)
        self.cross = str(self.toolchain / 'bin' / (inputs.TARGET + '-'))
        for suffix in ('gcc', 'g++'):
            path = Path(self.cross + suffix)
            path.write_bytes(b'not executed')
            path.chmod(0o755)
        self.sysroot = self.toolchain / inputs.TARGET / 'sysroot'
        self.sysroot.mkdir(parents=True)
        self.identity = dict(commit='test-only-toolchain-identity')
        self.record = dict(toolchain=self.identity,
                           artifacts={'libmx5dr.so': inputs.digest(self.library)})
        self.verify = self.enterContext(mock.patch.object(inputs, 'verify_build', return_value=self.record))
        self.compiler = self.enterContext(mock.patch.object(inputs, 'verify_toolchain', return_value=self.identity))

    def identify(self, **changes):
        args = dict(repo=self.root, library=self.library, cross=self.cross,
                    sysroot=self.sysroot, release_build=self.build)
        args.update(changes)
        return inputs.identify(**args)

    def test_matching_inputs_identify_the_verified_artifact(self):
        result = self.identify()
        self.assertTrue(result['release_verified'])
        self.assertEqual(result['library_sha256'], self.record['artifacts']['libmx5dr.so'])
        self.verify.assert_called_once_with(self.root, self.build)
        self.compiler.assert_called_once_with(self.toolchain)

    def test_development_run_is_never_reported_as_release(self):
        self.assertFalse(self.identify(release_build=None)['release_verified'])
        self.verify.assert_not_called()
        self.compiler.assert_not_called()

    def test_failed_record_verification_stops_the_test(self):
        self.verify.side_effect = ValueError('stale artifact')
        with self.assertRaisesRegex(ValueError, 'stale artifact'):
            self.identify()

    def test_other_library_is_rejected(self):
        other = self.root / 'old.so'
        other.write_bytes(self.library.read_bytes())
        with self.assertRaisesRegex(ValueError, 'selected release artifact'):
            self.identify(library=other)

    def test_other_loader_sysroot_is_rejected(self):
        other = self.root / 'other-sysroot'
        other.mkdir()
        with self.assertRaisesRegex(ValueError, 'sysroot differs'):
            self.identify(sysroot=other)

    def test_different_compiler_identity_is_rejected(self):
        self.compiler.return_value = dict(commit='different')
        with self.assertRaisesRegex(ValueError, 'differs from the artifact build'):
            self.identify()

    def test_gcc_alias_cannot_select_an_unverified_cxx(self):
        aliases = self.root / 'aliases'
        aliases.mkdir()
        for suffix in ('gcc', 'g++'):
            (aliases / ('arm-' + suffix)).symlink_to(self.cross + suffix)
        cross = str(aliases / 'arm-')
        self.assertTrue(self.identify(cross=cross)['release_verified'])
        (aliases / 'arm-g++').unlink()
        (aliases / 'arm-g++').write_bytes(b'different compiler')
        (aliases / 'arm-g++').chmod(0o755)
        with self.assertRaisesRegex(ValueError, 'different toolchains'):
            self.identify(cross=cross)

    def test_runner_does_not_pass_search_overrides_to_the_compiler(self):
        # Execute the real shell entry point in development mode and stop at
        # its first compiler call. Only authored fixture values enter the child.
        rejected = ('MAKEFLAGS', 'GNUMAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES',
                    'MAKEFILES', 'MAKELEVEL', 'GCC_EXEC_PREFIX', 'COMPILER_PATH',
                    'LIBRARY_PATH', 'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH',
                    'DEPENDENCIES_OUTPUT', 'SUNPRO_DEPENDENCIES', 'LD_RUN_PATH',
                    'LD_LIBRARY_PATH', 'LD_PRELOAD', 'LD_AUDIT',
                    'QEMU_SET_ENV', 'QEMU_UNSET_ENV', 'QEMU_LD_PREFIX')
        compiler = Path(self.cross + 'g++')
        compiler.write_text('#!/bin/sh\n' + ''.join(
            'test "${' + name + '+set}" != set || exit 73\n' for name in rejected
        ) + 'exit 74\n')
        environment = dict(PATH=os.environ['PATH'],
                           CROSS_COMPILE=self.cross, QEMU_SYSROOT=str(self.sysroot),
                           MX5DR_ARM_LIBRARY=str(self.library))
        environment.update({name: 'fixture-only' for name in rejected
                            if name not in ('LD_PRELOAD', 'LD_AUDIT')})
        # Export loader variables inside the existing shell, then source the
        # entry point so the host loader never tries to open the fixture name.
        run = subprocess.run(['sh', '-c',
                              'export LD_PRELOAD=fixture-only LD_AUDIT=fixture-only; . "$0"',
                              str(inputs.REPO / 'tests/run_arm_all.sh')],
                             env=environment, capture_output=True, text=True)
        self.assertEqual(run.returncode, 74, run.stderr)
        self.assertIn('"release_verified": false', run.stdout)


if __name__ == '__main__':
    unittest.main()
