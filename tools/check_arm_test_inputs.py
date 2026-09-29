#!/usr/bin/env python3
"""Identify ARM test inputs; verify pinned build/loader inputs for release runs."""
import argparse
import json
from pathlib import Path
import shutil

from build_arm import REPO, TARGET, digest, verify_build, verify_toolchain


def identify(repo, library, cross, sysroot, release_build=None):
    library = library.resolve(strict=True)
    report = dict(release_verified=False, library=str(library),
                  library_sha256=digest(library), cross_compile=cross,
                  sysroot=str(sysroot.resolve(strict=True)))
    if release_build is None:
        return report
    build = release_build.resolve(strict=True)
    if library != (build / 'libmx5dr.so').resolve(strict=True):
        raise ValueError('Test preload is not the selected release artifact')
    record = verify_build(repo, build)
    executable = shutil.which(cross + 'gcc')
    if not executable:
        raise ValueError('ARM test compiler is missing')
    compiler = Path(executable).resolve(strict=True)
    toolchain = compiler.parent.parent
    if compiler != toolchain / 'bin' / (TARGET + '-gcc'):
        raise ValueError('Release tests must use the pinned compiler layout')
    cxx = shutil.which(cross + 'g++')
    if not cxx or Path(cxx).resolve(strict=True) != toolchain / 'bin' / (TARGET + '-g++'):
        raise ValueError('Release C and C++ test compilers come from different toolchains')
    if sysroot.resolve(strict=True) != (toolchain / TARGET / 'sysroot').resolve(strict=True):
        raise ValueError('Release test sysroot differs from the compiler sysroot')
    identity = verify_toolchain(toolchain)
    if identity != record['toolchain']:
        raise ValueError('Release test compiler differs from the artifact build')
    report.update(release_verified=True, toolchain=identity,
                  library_sha256=record['artifacts']['libmx5dr.so'],
                  artifacts=record['artifacts'])
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--cross-prefix', required=True)
    parser.add_argument('--sysroot', type=Path, required=True)
    parser.add_argument('--release-build', type=Path)
    args = parser.parse_args()
    print('ARM_TEST_INPUTS=' + json.dumps(identify(REPO, args.library, args.cross_prefix,
                                                args.sysroot, args.release_build),
                                        sort_keys=True), flush=True)


if __name__ == '__main__':
    main()
