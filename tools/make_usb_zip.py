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

REPO = Path(__file__).resolve().parents[1]
ARTIFACTS = ('libmx5dr.so', 'libmx5dr-vimtap.so', 'mx5dr-collector', 'mx5dr-guard', 'mx5dr-sha256')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


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
    # This identifies uncommitted local candidates without inventing a tag or
    # pretending they are an unchanged build of the base commit.
    source = {}
    for folder in ('src', 'packaging'):
        for path in sorted((REPO / folder).rglob('*')):
            if path.is_file() and '__pycache__' not in path.parts:
                source[path.relative_to(REPO).as_posix()] = digest(path)
    extra_sources = ('Makefile', 'tools/make_usb_zip.py', 'tools/analyze_logs.py')
    for name in extra_sources:
        source[name] = digest(REPO / name)
    source_digest = hashlib.sha256(json.dumps(source, sort_keys=True).encode()).hexdigest()
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain', '--',
                                         'src', 'packaging', *extra_sources], cwd=REPO))
    with tempfile.TemporaryDirectory(prefix='mx5dr-usb-build-') as tmp:
        bundle = Path(tmp) / 'bundle'
        subprocess.run(['sh', str(REPO / 'packaging/make_bundle.sh'),
                        '--default-mode=' + args.default_mode,
                        str(args.build_dir.resolve() / 'libmx5dr.so'), str(bundle)], check=True)
        shutil.copy2(REPO / 'tools/analyze_logs.py', bundle / 'analyze_logs.py')
        info = dict(source_commit=commit, source_modified=dirty, source_files=source,
                    source_tree_sha256=source_digest,
                    toolchain_commit='61ec0343de84f6fc7c46840056df1d600d44be8a',
                    target='NA 74.00.324A', default_mode=args.default_mode,
                    artifacts={name: digest(bundle / name) for name in ARTIFACTS})
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
