"""Exercise production Make rules with real compiler-generated dependencies.

An isolated copy of the production sources uses a native compiler and overrides
only the ARM flags. These checks establish rebuild behavior, not target ABI or
vehicle execution. The repository and existing build directories are unchanged.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import time
import unittest


REPO = Path(__file__).resolve().parents[2]


class ArmDependencyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='mx5dr-dependencies-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        makefile = Path(os.environ.get('MX5DR_MAKEFILE_UNDER_TEST', REPO / 'Makefile'))
        shutil.copyfile(makefile, self.root / 'Makefile')
        shutil.copytree(REPO / 'src', self.root / 'src')
        source_time = time.time() - 3600
        for path in (self.root / 'src').rglob('*'):
            if path.is_file():
                os.utime(path, (source_time, source_time))
        compiler = shutil.which('c++')
        self.assertIsNotNone(compiler, 'A real C++ compiler is required')
        c_compiler = shutil.which('cc')
        self.assertIsNotNone(c_compiler, 'A real C compiler is required')
        toolchain = self.root / 'toolchain'
        toolchain.mkdir()
        # Keep the compiler's real argv[0]; macOS xcrun shims reject aliases.
        for name, executable in (('native-g++', compiler), ('native-gcc', c_compiler)):
            launcher = toolchain / name
            launcher.write_text('#!/bin/sh\nexec ' + shlex.quote(executable) + ' "$@"\n')
            launcher.chmod(0o755)
        self.arguments = ['make', '--no-print-directory', 'BUILD=build',
                          'ARM_PREFIX=' + str(toolchain / 'native-'),
                          'ARM_SYSROOT=' + str(self.root), 'ARM_FLAGS=',
                          'ARM_CPPFLAGS=-Isrc', 'ARM_CXXFLAGS=-std=c++11 -O0 -Isrc']
        self.config = 'build/arm/src/runtime/config.o'
        self.sha = 'build/arm/src/runtime/sha256.o'
        self.guard = 'build/arm/src/guard/guard.o'
        self.core = 'build/arm/src/core/dr_core.o'

    def make(self, *args):
        environment = dict(os.environ)
        for name in ('MAKEFLAGS', 'MFLAGS', 'MAKELEVEL'):
            environment.pop(name, None)
        result = subprocess.run(self.arguments + list(args), cwd=self.root,
                                env=environment, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def older_object(self, name):
        # Deterministic even on filesystems/make versions with coarse mtimes:
        # changing the header will be the only newly newer prerequisite.
        past = time.time() - 10
        os.utime(self.root / name, (past, past))

    def test_header_edit_rebuilds_affected_production_object_only(self):
        self.make(self.config, self.sha)
        untouched = (self.root / self.sha).stat().st_mtime_ns
        self.older_object(self.config)
        header = self.root / 'src/runtime/config.h'
        header.write_text(header.read_text() + '\n// dependency regression\n')
        changed = self.make(self.config, self.sha)
        self.assertIn('-c src/runtime/config.cpp', changed.stdout)
        self.assertNotIn('-c src/runtime/sha256.cpp', changed.stdout)
        self.assertEqual((self.root / self.sha).stat().st_mtime_ns, untouched)
        self.assertEqual(self.make('-q', self.config, self.sha).returncode, 0)

    def test_transitive_shared_header_rebuilds_guard_and_hash_objects(self):
        shared = self.root / 'src/runtime/sha256.h'
        shared.write_text(shared.read_text() + '\n#include "nested_fixture.h"\n')
        header = self.root / 'src/runtime/nested_fixture.h'
        header.write_text('// transitive dependency fixture\n')
        self.make(self.guard, self.sha, self.config)
        unchanged = (self.root / self.config).stat().st_mtime_ns
        # Backdating the objects must not make their directly included header
        # newer too: only the nested header edit should trigger this rebuild.
        past = time.time() - 3600
        os.utime(shared, (past, past))
        for name in (self.guard, self.sha):
            self.older_object(name)
        header.write_text(header.read_text() + '\n// shared header regression\n')
        changed = self.make(self.guard, self.sha, self.config)
        self.assertIn('-c src/guard/guard.cpp', changed.stdout)
        self.assertIn('-c src/runtime/sha256.cpp', changed.stdout)
        self.assertNotIn('-c src/runtime/config.cpp', changed.stdout)
        self.assertEqual((self.root / self.config).stat().st_mtime_ns, unchanged)

    def test_c_header_edit_rebuilds_production_core_object(self):
        self.make(self.core)
        self.older_object(self.core)
        header = self.root / 'src/core/dr_core.h'
        header.write_text(header.read_text() + '\n// C header regression\n')
        changed = self.make(self.core)
        self.assertIn('-c src/core/dr_core.c', changed.stdout)
        self.assertEqual(self.make('-q', self.core).returncode, 0)

    def test_missing_dependency_file_recompiles_existing_object_once(self):
        self.make(self.config)
        dependency = (self.root / self.config).with_suffix('.d')
        self.assertTrue(dependency.is_file(), 'Compiler dependency file was not generated')
        dependency.unlink()
        changed = self.make(self.config)
        self.assertIn('-c src/runtime/config.cpp', changed.stdout)
        self.assertTrue(dependency.is_file())
        self.assertEqual(self.make('-q', self.config).returncode, 0)

    def test_removed_include_does_not_leave_unbuildable_stale_dependency(self):
        header = self.root / 'src/runtime/config.h'
        original = header.read_text()
        retired = self.root / 'src/runtime/retired_fixture.h'
        retired.write_text('// dependency removed by a subsequent source edit\n')
        header.write_text(original + '\n#include "retired_fixture.h"\n')
        self.make(self.config)
        self.older_object(self.config)
        retired.unlink()
        header.write_text(original)
        changed = self.make(self.config)
        self.assertIn('-c src/runtime/config.cpp', changed.stdout)
        self.assertNotIn('retired_fixture.h',
                         (self.root / self.config).with_suffix('.d').read_text())

    def test_clean_then_fresh_build_restores_dependency_tracking(self):
        self.make(self.config)
        self.make('clean')
        self.assertFalse((self.root / 'build').exists())
        self.make(self.config)
        self.assertTrue((self.root / self.config).with_suffix('.d').is_file())
        self.assertEqual(self.make('-q', self.config).returncode, 0)

    def test_lds_loader_has_its_own_target_and_header_dependencies(self):
        aa = 'build/arm/src/runtime/loader.o'
        lds = 'build/arm-lds/src/runtime/loader.o'
        install = 'build/arm-lds/src/adapter/lds_install.o'
        self.make(aa, lds, install)
        self.assertIn(b'/jci/aapa/blmjciaapa.so', (self.root / aa).read_bytes())
        self.assertNotIn(b'/jci/lds/svcjcilds.so', (self.root / aa).read_bytes())
        self.assertIn(b'/jci/lds/svcjcilds.so', (self.root / lds).read_bytes())
        self.assertNotIn(b'/jci/aapa/blmjciaapa.so', (self.root / lds).read_bytes())
        before = (self.root / aa).stat().st_mtime_ns
        self.older_object(install)
        header = self.root / 'src/adapter/lds_install.h'
        header.write_text(header.read_text() + '\n// LDS dependency regression\n')
        changed = self.make(aa, lds, install)
        self.assertIn('-c src/adapter/lds_install.cpp', changed.stdout)
        self.assertNotIn('-c src/runtime/loader.cpp', changed.stdout)
        self.assertEqual((self.root / aa).stat().st_mtime_ns, before)
        self.assertEqual(self.make('-q', aa, lds, install).returncode, 0)


if __name__ == '__main__':
    unittest.main()
