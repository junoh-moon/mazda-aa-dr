#!/usr/bin/env python3
"""Create a flat, complete USB ZIP from a single ARM build; no publication."""
import argparse
import hashlib
import json
import os
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


def entry_source(name):
    return name.startswith('packaging/usb-entry/') or name == 'packaging/USB_START_KO.md'


def source_files(shell_only=False):
    source = {}
    for folder in ('src', 'packaging'):
        for path in sorted((REPO / folder).rglob('*')):
            name = path.relative_to(REPO).as_posix()
            if shell_only and entry_source(name):
                continue
            if path.is_file() and '__pycache__' not in path.parts:
                source[name] = digest(path)
    for name in EXTRA_SOURCES:
        source[name] = digest(REPO / name)
    return source


def source_modified(source, commit, shell_only=False):
    # Compare captured input bytes with the pinned commit, independently of
    # index flags, ignore rules and status.showUntrackedFiles configuration.
    tree = subprocess.check_output(['git', '--no-replace-objects', 'ls-tree', '-r', '-z', '--full-tree',
                                    commit, '--', 'src', 'packaging', *EXTRA_SOURCES], cwd=REPO)
    tracked = {}
    for entry in tree.split(b'\0'):
        if entry:
            metadata, raw_name = entry.split(b'\t', 1)
            _, kind, object_id = metadata.split()
            name = os.fsdecode(raw_name)
            if shell_only and entry_source(name):
                continue
            if kind == b'blob' and '__pycache__' not in Path(name).parts:
                tracked[name] = object_id.decode('ascii')
    if set(tracked) != set(source):
        return True
    for name, object_id in tracked.items():
        blob = subprocess.check_output(['git', '--no-replace-objects', 'cat-file', 'blob', object_id], cwd=REPO)
        if hashlib.sha256(blob).hexdigest() != source[name]:
            return True
    return False


def write_verified_zip(bundle, output, sidecar, shell_only=False):
    if shell_only:
        names = [p.relative_to(bundle).as_posix() for p in bundle.rglob('*')]
        if any(n.split('/')[0] in ('mp3', 'js', 'USB_ENTRY_NOTICE.md', 'USB_START_KO.md') for n in names):
            raise RuntimeError('Shell-only bundle contains an entry asset')
        info = json.loads((bundle / 'build-info.json').read_text())
        if info.get('bundle_type') != 'shell-only' or info.get('entry_payloads_included') is not False:
            raise RuntimeError('Shell-only bundle metadata missing or inconsistent')
    output.parent.mkdir(parents=True, exist_ok=True)
    # The temporary files share the destination filesystem. Hard links publish
    # complete files without replacing any concurrently created destination.
    with tempfile.NamedTemporaryFile(dir=output.parent, prefix='.' + output.name + '.',
                                     suffix='.partial') as partial, \
            tempfile.NamedTemporaryFile(dir=output.parent, prefix='.' + sidecar.name + '.',
                                         suffix='.partial') as checksum:
        with zipfile.ZipFile(partial.name, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(p for p in bundle.rglob('*') if p.is_file()):
                archive.write(path, path.relative_to(bundle).as_posix())
        # Verify the bytes actually written, including trigger subdirectories.
        with zipfile.ZipFile(partial.name) as archive:
            if archive.testzip() is not None:
                raise RuntimeError('ZIP integrity failure')
            for line in archive.read('SHA256SUMS').decode().splitlines():
                expected, name = line.split('  ', 1)
                if hashlib.sha256(archive.read(name)).hexdigest() != expected:
                    raise RuntimeError('ZIP checksum mismatch: ' + name)
            required = {'install.sh', 'mx5dr-sha256'}
            required |= {'trial', 'INSTALL_KO.md', 'build-info.json'} if shell_only else {'js/run.js', 'mp3/a.mp3'}
            if shell_only:
                required |= set(ARTIFACTS) | {name + '.sha256' for name in ARTIFACTS}
            if not required <= set(archive.namelist()):
                raise RuntimeError('Missing USB root entry')
        checksum_line = digest(Path(partial.name)) + '  ' + output.name + '\n'
        checksum.write(checksum_line.encode())
        checksum.flush()
        try:
            # Publish the ZIP last so a failed sidecar cannot leave a final ZIP.
            os.link(checksum.name, sidecar)
            os.link(partial.name, output)
        except BaseException:
            for path, temporary in ((output, partial), (sidecar, checksum)):
                try:
                    if os.path.samestat(path.lstat(), os.fstat(temporary.fileno())):
                        path.unlink()
                except FileNotFoundError:
                    pass
            raise
    return checksum_line


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=REPO / 'build')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--default-mode', choices=('OBSERVE', 'SHADOW', 'BETA'), default='OBSERVE',
                        help='Bundle install mode; BETA must be requested explicitly (never the default) and installs '
                             'the persistent product (every boot); OBSERVE/SHADOW stay one-boot trials')
    parser.add_argument('--shell-only', action='store_true',
                        help='Package for an already authorized shell, without entry assets')
    args = parser.parse_args()
    output = args.output.absolute()
    sidecar = output.with_suffix(output.suffix + '.sha256')
    if any(path.exists() or path.is_symlink() for path in (output, sidecar)):
        parser.error('Output already exists; choose a new name')
    build_record = verify_build(REPO, args.build_dir.resolve())
    # This identifies uncommitted local candidates without inventing a tag or
    # pretending they are an unchanged build of the base commit.
    source = source_files(args.shell_only)
    if any(source.get(name) != expected
           for name, expected in build_record['source_files'].items()):
        raise RuntimeError('Compiled sources changed before packaging')
    source_digest = hashlib.sha256(json.dumps(source, sort_keys=True).encode()).hexdigest()
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True).strip()
    dirty = source_modified(source, commit, args.shell_only)
    with tempfile.TemporaryDirectory(prefix='mx5dr-usb-build-') as tmp:
        bundle = Path(tmp) / 'bundle'
        bundle_args = ['--shell-only'] if args.shell_only else []
        subprocess.run(['sh', str(REPO / 'packaging/make_bundle.sh'), *bundle_args,
                        '--default-mode=' + args.default_mode,
                        str(args.build_dir.resolve() / 'libmx5dr.so'), str(bundle)], check=True)
        shutil.copy2(REPO / 'tools/analyze_logs.py', bundle / 'analyze_logs.py')
        info = dict(source_commit=commit, source_modified=dirty, source_files=source,
                    source_tree_sha256=source_digest,
                    toolchain_commit=build_record['toolchain']['commit'],
                    arm_build=build_record,
                    target='NA 74.00.324A', default_mode=args.default_mode,
                    install_policy='persistent' if args.default_mode == 'BETA' else 'one-boot',
                    artifacts={name: digest(bundle / name) for name in ARTIFACTS})
        if args.shell_only:
            info.update(bundle_type='shell-only', entry_payloads_included=False)
        if info['artifacts'] != build_record['artifacts']:
            raise RuntimeError('Artifacts changed while packaging')
        if source_files(args.shell_only) != source:
            raise RuntimeError('Sources changed while packaging')
        verify_build(REPO, args.build_dir.resolve())
        (bundle / 'build-info.json').write_text(json.dumps(info, indent=2) + '\n')
        files = sorted(p for p in bundle.rglob('*') if p.is_file())
        manifest = ''.join(digest(p) + '  ' + p.relative_to(bundle).as_posix() + '\n' for p in files)
        (bundle / 'SHA256SUMS').write_text(manifest)
        checksum_line = write_verified_zip(bundle, output, sidecar, args.shell_only)
        print(output)
        print(checksum_line, end='')


if __name__ == '__main__':
    main()
