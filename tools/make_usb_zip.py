#!/usr/bin/env python3
"""Create a flat, complete USB ZIP from a single ARM build; no publication."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

from build_arm import ARTIFACTS, verify_build

REPO = Path(__file__).resolve().parents[1]
EXTRA_SOURCES = ('Makefile', 'tools/make_usb_zip.py', 'tools/analyze_logs.py',
                 'tools/build_arm.py', 'tools/fetch_m3_toolchain.py')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_files():
    source = {}
    for folder in ('src', 'packaging'):
        for path in sorted((REPO / folder).rglob('*')):
            if path.is_file() and '__pycache__' not in path.parts:
                source[path.relative_to(REPO).as_posix()] = digest(path)
    for name in EXTRA_SOURCES:
        source[name] = digest(REPO / name)
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=REPO / 'build')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--default-mode', choices=('OBSERVE', 'SHADOW'), default='OBSERVE')
    args = parser.parse_args()
    output = args.output.resolve()
    sidecar = output.with_suffix(output.suffix + '.sha256')
    if output.exists() or sidecar.exists():
        parser.error('Output already exists; choose a new name')
    build_record = verify_build(REPO, args.build_dir.resolve())
    # This identifies uncommitted local candidates without inventing a tag or
    # pretending they are an unchanged build of the base commit.
    source = source_files()
    if any(source.get(name) != expected
           for name, expected in build_record['source_files'].items()):
        raise RuntimeError('Compiled sources changed before packaging')
    source_digest = hashlib.sha256(json.dumps(source, sort_keys=True).encode()).hexdigest()
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain', '--',
                                         'src', 'packaging', *EXTRA_SOURCES], cwd=REPO))
    with tempfile.TemporaryDirectory(prefix='mx5dr-usb-build-') as tmp:
        bundle = Path(tmp) / 'bundle'
        subprocess.run(['sh', str(REPO / 'packaging/make_bundle.sh'),
                        '--default-mode=' + args.default_mode,
                        str(args.build_dir.resolve() / 'libmx5dr.so'), str(bundle)], check=True)
        shutil.copy2(REPO / 'tools/analyze_logs.py', bundle / 'analyze_logs.py')
        info = dict(source_commit=commit, source_modified=dirty, source_files=source,
                    source_tree_sha256=source_digest,
                    toolchain_commit=build_record['toolchain']['commit'],
                    arm_build=build_record,
                    target='NA 74.00.324A', default_mode=args.default_mode,
                    artifacts={name: digest(bundle / name) for name in ARTIFACTS})
        if info['artifacts'] != build_record['artifacts']:
            raise RuntimeError('Artifacts changed while packaging')
        if source_files() != source:
            raise RuntimeError('Sources changed while packaging')
        verify_build(REPO, args.build_dir.resolve())
        (bundle / 'build-info.json').write_text(json.dumps(info, indent=2) + '\n')
        files = sorted(p for p in bundle.rglob('*') if p.is_file())
        manifest = ''.join(digest(p) + '  ' + p.relative_to(bundle).as_posix() + '\n' for p in files)
        (bundle / 'SHA256SUMS').write_text(manifest)
        output.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(p for p in bundle.rglob('*') if p.is_file()):
                archive.write(path, path.relative_to(bundle).as_posix())
        # Verify the bytes actually written, including trigger subdirectories.
        with zipfile.ZipFile(output) as archive:
            if archive.testzip() is not None:
                raise RuntimeError('ZIP integrity failure')
            for line in archive.read('SHA256SUMS').decode().splitlines():
                expected, name = line.split('  ', 1)
                if hashlib.sha256(archive.read(name)).hexdigest() != expected:
                    raise RuntimeError('ZIP checksum mismatch: ' + name)
            if not {'install.sh', 'js/run.js', 'mp3/a.mp3', 'mx5dr-sha256'} <= set(archive.namelist()):
                raise RuntimeError('Missing USB root entry')
        sidecar.write_text(digest(output) + '  ' + output.name + '\n')
        print(output)
        print(sidecar.read_text(), end='')


if __name__ == '__main__':
    main()
