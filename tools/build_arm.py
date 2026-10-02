#!/usr/bin/env python3
"""Build release ARM artifacts from scratch and record their actual inputs.

Run on x86-64 Linux with the pinned toolchain. This is build provenance for
accidental stale/mixed artifacts, not a signature or vehicle validation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess

from fetch_m3_toolchain import COMMIT, matches, wanted

REPO = Path(__file__).resolve().parents[1]
TARGET = 'arm-cortexa9_neon-linux-gnueabi'
ARTIFACTS = ('libmx5dr.so', 'libmx5dr-vimtap.so', 'libmx5dr-ldstap.so', 'mx5dr-collector',
             'mx5dr-guard', 'mx5dr-sha256')
RECORD = 'arm-build.json'
# Regression guard for the LDS DSO's own static TLS, including worst-case
# alignment padding. The published 7,288-byte image repeatedly crashed in a
# partial OEM SM QEMU run; this cap does not prove total process stack headroom.
LDS_TLS_REGRESSION_LIMIT = 512
# The AA preload also runs inside OEM-owned threads. Its former 7,284-byte
# static TLS image failed an authored 16 KiB-thread QEMU-user boundary probe.
# This catches that regression; it does not establish OEM stack headroom.
AA_TLS_REGRESSION_LIMIT = 512


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_inputs(repo):
    paths = [p for p in (repo / 'src').rglob('*') if p.is_file()]
    paths += [repo / name for name in ('Makefile', 'tools/build_arm.py',
                                      'tools/fetch_m3_toolchain.py')]
    return {p.relative_to(repo).as_posix(): digest(p) for p in sorted(paths)}


def verify_toolchain(root):
    if (root / 'SOURCE_COMMIT').read_text().strip() != COMMIT:
        raise ValueError('Toolchain SOURCE_COMMIT differs from the pinned revision')
    tree = json.loads((root / 'source_tree.json').read_text())
    if tree.get('source_commit') != COMMIT or tree.get('truncated'):
        raise ValueError('Toolchain source tree is incomplete or from another revision')
    selected = [entry for entry in tree['tree']
                if entry['type'] == 'blob' and wanted(entry['path'])]
    manifest = json.loads((root / 'SUBSET_MANIFEST.json').read_text())
    if not selected or manifest != selected:
        raise ValueError('Toolchain subset manifest differs from the source tree')
    for entry in selected:
        relative = Path(entry['path'])
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Invalid toolchain path')
        path = root / relative
        if entry['mode'] == '120000':
            if not path.is_symlink():
                raise ValueError('Missing toolchain symlink: ' + entry['path'])
            data = os.readlink(path).encode()
        else:
            if path.is_symlink():
                raise ValueError('Unexpected toolchain symlink: ' + entry['path'])
            data = path.read_bytes()
            if entry['mode'] == '100755' and not os.access(path, os.X_OK):
                raise ValueError('Toolchain file is not executable: ' + entry['path'])
        if not matches(data, entry):
            raise ValueError('Toolchain blob mismatch: ' + entry['path'])
    compiler = root / 'bin' / (TARGET + '-gcc')
    environment = build_environment()
    machine = subprocess.check_output([str(compiler), '-dumpmachine'],
                                      env=environment, text=True).strip()
    version = subprocess.check_output([str(compiler), '-dumpversion'],
                                      env=environment, text=True).strip()
    if machine != TARGET or version != '4.9.1':
        raise ValueError('Unexpected compiler target/version: ' + machine + ' ' + version)
    return dict(commit=COMMIT, subset_sha256=digest(root / 'SUBSET_MANIFEST.json'),
                compiler_sha256=digest(compiler), target=machine, gcc_version=version)


def check_elf(path):
    """Portable sanity check; the build also checks attributes with readelf."""
    data = path.read_bytes()
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('Not an ELF32 little-endian artifact: ' + path.name)
    header = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    kind, machine, version, _, phoff, _, flags, size, phsize, phcount, _, _, _ = header
    expected_kind = 3 if path.name.endswith('.so') else 2
    if (kind != expected_kind or machine != 40 or version != 1 or size != 52 or
            flags & 0xff000000 != 0x05000000 or flags & 0x400):
        raise ValueError('Wrong ARM EABI/type or hard-float artifact: ' + path.name)
    if not phcount or phsize != 32 or phoff < size or phoff + phsize * phcount > len(data):
        raise ValueError('Invalid ELF program headers: ' + path.name)
    load_segments = 0
    for index in range(phcount):
        segment = struct.unpack_from('<IIIIIIII', data, phoff + index * phsize)
        kind, offset, _, _, filesz, memsz, _, align = segment
        if offset + filesz > len(data) or (kind in (1, 7) and filesz > memsz):
            raise ValueError('Truncated ELF segment: ' + path.name)
        load_segments += kind == 1
        if path.name in ('libmx5dr-ldstap.so', 'libmx5dr.so') and kind == 7:
            label = 'LDS' if path.name == 'libmx5dr-ldstap.so' else 'AA'
            limit = (LDS_TLS_REGRESSION_LIMIT if label == 'LDS'
                     else AA_TLS_REGRESSION_LIMIT)
            if align > 1 and align & (align - 1):
                raise ValueError('Invalid ' + label + ' TLS alignment: ' + path.name)
            if memsz + max(align - 1, 0) > limit:
                raise ValueError(label + ' thread-local storage exceeds regression limit: ' + path.name)
        if path.name == 'mx5dr-sha256' and kind in (2, 3):
            raise ValueError('Hash helper is not statically linked')
    if not load_segments:
        raise ValueError('ELF has no loadable segment: ' + path.name)


def check_attributes(path, readelf):
    report = subprocess.check_output([str(readelf), '-A', '-d', '-V', str(path)],
                                     env=build_environment(), text=True)
    if ('Tag_CPU_arch: v7\n' not in report or
            'Tag_ABI_VFP_args: VFP registers' in report or 'TEXTREL' in report):
        raise ValueError('Unexpected ARM attributes or TEXTREL: ' + path.name)
    if re.search(r'\((?:RPATH|RUNPATH)\)', report):
        raise ValueError('Unexpected RPATH/RUNPATH: ' + path.name)
    if set(re.findall(r'\bGLIBC_[0-9.]+', report)) - {'GLIBC_2.4'}:
        raise ValueError('Unexpected GLIBC version: ' + path.name)
    needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', report)
    if any(name.startswith('libstdc++') for name in needed):
        raise ValueError('Dynamic libstdc++ dependency: ' + path.name)
    if path.name != 'mx5dr-collector' and any('dbus' in name for name in needed):
        raise ValueError('D-Bus dependency outside collector: ' + path.name)
    return dict(needed=needed, readelf_sha256=hashlib.sha256(report.encode()).hexdigest())


def build_environment():
    environment = dict(os.environ)
    # Prevent inherited make -n/-e, included makefiles or GCC search-path
    # overrides from bypassing the fresh build or selecting outside headers.
    for name in ('MAKEFLAGS', 'GNUMAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES', 'MAKEFILES',
                 'MAKELEVEL', 'GCC_EXEC_PREFIX', 'COMPILER_PATH', 'LIBRARY_PATH',
                 'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'DEPENDENCIES_OUTPUT',
                 'SUNPRO_DEPENDENCIES', 'LD_RUN_PATH'):
        environment.pop(name, None)
    environment['LC_ALL'] = 'C'
    return environment


def verify_build(repo, build):
    try:
        record = json.loads((build / RECORD).read_text())
    except FileNotFoundError as error:
        raise ValueError('Missing arm-build.json; first run tools/build_arm.py in a new build directory') from error
    if (record.get('schema') != 1 or record.get('source_files') != source_inputs(repo) or
            record.get('toolchain', {}).get('commit') != COMMIT):
        raise ValueError('Build inputs differ from the recorded ARM build; rebuild from scratch')
    for name in ARTIFACTS:
        check_elf(build / name)
        if record.get('artifacts', {}).get(name) != digest(build / name):
            raise ValueError('Artifact differs from the recorded ARM build: ' + name)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path,
                        help='new directory; existing directories are never reused')
    parser.add_argument('--toolchain', type=Path, default=REPO / 'tools/m3-toolchain')
    args = parser.parse_args()
    build = args.build_dir.absolute()
    toolchain = args.toolchain.resolve()
    if build.exists() or build.is_symlink():
        parser.error('Build directory already exists; choose a new directory')
    inputs = source_inputs(REPO)
    compiler = verify_toolchain(toolchain)
    build.mkdir(parents=True)
    subprocess.run(['make', '--no-print-directory', '-f', str(REPO / 'Makefile'),
                    'arm', 'BUILD=' + str(build),
                    'ARM_PREFIX=' + str(toolchain / 'bin' / (TARGET + '-')),
                    'ARM_SYSROOT=' + str(toolchain / TARGET / 'sysroot')],
                   cwd=REPO, env=build_environment(), check=True)
    attributes = {}
    for name in ARTIFACTS:
        check_elf(build / name)
        attributes[name] = check_attributes(build / name, toolchain / 'bin' / (TARGET + '-readelf'))
    if source_inputs(REPO) != inputs or verify_toolchain(toolchain) != compiler:
        raise ValueError('Build inputs changed during compilation; no build record written')
    record = dict(schema=1, source_files=inputs, toolchain=compiler,
                  artifacts={name: digest(build / name) for name in ARTIFACTS},
                  elf=attributes)
    (build / RECORD).write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
    print('Verified ARM build: ' + str(build), flush=True)


if __name__ == '__main__':
    main()
